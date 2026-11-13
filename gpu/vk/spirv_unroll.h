// Full unrolling of counted loops in a SPIR-V module, innermost first: for AMD FSR 4's network layers (fsr4_tune.h), whose
// nested loops over a kernel's taps and channels the driver leaves rolled. Rolled, every index into the weights and the
// accumulators is a loop counter and every tap pays a counter, a compare and a branch; unrolled, the indices are constants
// and the weights the code's own. Plain functions over the words, no Vulkan; operands are walked by spirv_operands.h, so only
// ids are renamed, never a literal that happens to equal one. Code size grows with the trips: unroll_all's max_words is
// what keeps a driver's compile of the result affordable (vk_fsr4.cpp kUnrollWords).
//
// A loop is taken only in this shape (DXC's for its counted loops):
//   %H = OpLabel ; %i = OpPhi %int-type %c0 %pre %inext %cont (the header's only phi, c0 a constant) ;
//   %t = OpULessThan / OpSLessThan %bool %i %N (N a constant) ; OpLoopMerge %M %cont ... ; OpBranchConditional %t %B %M
//   ... its blocks laid out after %H and before %M ... ; %cont = OpLabel ... %inext = OpIAdd %i %step (step a constant) ...
//   OpBranch %H
// and only when: nothing outside it names a value it makes (the header excepted, by the branch into it); its body does not
// name the header, the test or the step; the merge block starts without a phi; every instruction in it is one the operand
// table describes. Anything else is left as it is.
#pragma once

#include <cstdint>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spirv_operands.h"

namespace spirv_unroll {

using Ins = std::vector<uint32_t>;   // one instruction, its first word included

/// Calls f(word&, is_result) for each id in the instruction (result type, result, operands); false when its operands are
/// not all described (the instruction is then not to be renamed)
template <class F>
inline bool for_each_id(Ins& in, F&& f) {
    const char* k = spirv_operands::of(in[0] & 0xffff);
    if (!k) return false;
    size_t w = 1;
    const size_t n = in.size();
    for (const char* p = k; *p; p++) {
        const char c = *p;
        const bool many = p[1] == '*', opt = p[1] == '?';
        if (many || opt) p++;
        do {
            if (w >= n) {
                if (many || opt) break;
                return false;   // (a required operand missing)
            }
            switch (c) {
            case 'T': f(in[w++], false); break;
            case 'R': f(in[w++], true); break;
            case 'i': f(in[w++], false); break;
            case 'l': case 'e': w++; break;
            case 's': { while (w < n) { const uint32_t x = in[w++]; if (!(x & 0xff000000u) || !(x & 0xff0000u) || !(x & 0xff00u) || !(x & 0xffu)) break; } break; }
            case 'c': w = n; break;
            case 'P': if (w + 1 >= n) return false; f(in[w++], false); f(in[w++], false); break;
            case 'Q': if (w + 1 >= n) return false; w++; f(in[w++], false); break;
            case 'q': if (w + 1 >= n) return false; f(in[w++], false); w++; break;
            case 'M': {   // MemoryAccess: Aligned a literal; MakePointerAvailable / Visible an id each
                const uint32_t m = in[w++];
                if (m & ~0x3fu) return false;
                if (m & 0x2) w++;
                if (m & 0x8) { if (w >= n) return false; f(in[w++], false); }
                if (m & 0x10) { if (w >= n) return false; f(in[w++], false); }
                break;
            }
            case 'G': {   // ImageOperands: every parameter an id, two for Grad
                const uint32_t m = in[w++];
                if (m & ~0x17fffu) return false;
                for (uint32_t b = 0; b < 17; b++) {
                    if (!(m & (1u << b)) || (b >= 10 && b <= 14)) continue;
                    for (int r = 0; r < (b == 2 ? 2 : 1); r++) { if (w >= n) return false; f(in[w++], false); }
                }
                break;
            }
            case 'L': {   // LoopControl: its parameters are literals
                const uint32_t m = in[w++];
                if (m & ~0x1ffu) return false;
                for (uint32_t b = 3; b <= 8; b++) if (m & (1u << b)) w++;
                break;
            }
            default: return false;   // ('X')
            }
        } while (many && w < n);
    }
    return w == n;
}

inline uint32_t result_of(Ins& in) {
    uint32_t r = 0;
    for_each_id(in, [&](uint32_t& id, bool res) { if (res) r = id; });
    return r;
}

struct Module {
    uint32_t header[5];
    std::vector<Ins> ins;
};

inline bool parse(const uint32_t* code, size_t words, Module& m) {
    if (words < 5 || code[0] != 0x07230203u) return false;
    for (int i = 0; i < 5; i++) m.header[i] = code[i];
    m.ins.clear();
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16;
        if (!n || i + n > words) return false;
        m.ins.emplace_back(code + i, code + i + n);
        i += n;
    }
    return true;
}
inline std::vector<uint32_t> emit(const Module& m) {
    std::vector<uint32_t> o(m.header, m.header + 5);
    for (const Ins& in : m.ins) o.insert(o.end(), in.begin(), in.end());
    return o;
}

enum : uint32_t { kLabel = 248, kPhi = 245, kLoopMerge = 246, kBranch = 249, kBranchCond = 250, kULess = 176, kSLess = 177,
                  kIAdd = 128, kConstant = 43, kTypeInt = 21 };
inline bool is_terminator(uint32_t op) { return op == 249 || op == 250 || op == 251 || op == 252 || op == 253 || op == 254 || op == 255; }

/// One loop unrolled, if one can be: true when the module changed
inline bool unroll_one(Module& m, std::unordered_set<uint64_t>& refused, uint32_t max_trips) {
    auto& I = m.ins;
    std::unordered_map<uint32_t, std::pair<uint32_t, int64_t>> consts;   // id -> (type, value) of 32-bit integer constants
    std::unordered_map<uint32_t, bool> int32_signed;                     // OpTypeInt 32 -> signedness
    size_t last_const = 0;
    for (size_t k = 0; k < I.size(); k++) {
        const Ins& in = I[k];
        const uint32_t op = in[0] & 0xffff;
        if (op == kTypeInt && in.size() == 4 && in[2] == 32) int32_signed[in[1]] = in[3] != 0;
        if (op == kConstant && in.size() == 4 && int32_signed.count(in[1]))
            consts[in[2]] = {in[1], int32_signed[in[1]] ? (int64_t)(int32_t)in[3] : (int64_t)in[3]}, last_const = k;
        if (op == 54) break;   // (OpFunction: the globals end)
    }
    if (!last_const) return false;
    for (size_t li = 3; li + 1 < I.size(); li++) {
        if ((I[li][0] & 0xffff) != kLoopMerge || I[li].size() < 4) continue;
        const uint32_t M = I[li][1], cont = I[li][2];
        const uint64_t key = (uint64_t)M << 32 | cont;
        if (refused.count(key)) continue;
        const Ins &hl = I[li - 3], &pl = I[li - 2], &tl = I[li - 1], &bl = I[li + 1];
        if ((hl[0] & 0xffff) != kLabel || (pl[0] & 0xffff) != kPhi || pl.size() != 7 ||
            ((tl[0] & 0xffff) != kULess && (tl[0] & 0xffff) != kSLess) || tl.size() != 5 ||
            (bl[0] & 0xffff) != kBranchCond || bl.size() < 4) { refused.insert(key); continue; }
        const uint32_t H = hl[1], ty = pl[1], i = pl[2], test = tl[2];
        uint32_t c0, pre, inext;
        if (pl[6] == cont) c0 = pl[3], pre = pl[4], inext = pl[5];
        else if (pl[4] == cont) c0 = pl[5], pre = pl[6], inext = pl[3];
        else { refused.insert(key); continue; }
        (void)pre;
        if (tl[3] != i || bl[1] != test || bl[3] != M || !consts.count(c0) || !consts.count(tl[4])) { refused.insert(key); continue; }
        const uint32_t B = bl[2];
        size_t mi = li + 1;
        while (mi < I.size() && !((I[mi][0] & 0xffff) == kLabel && I[mi][1] == M)) mi++;
        if (mi >= I.size()) { refused.insert(key); continue; }
        bool inner = true;
        for (size_t k = li + 1; k < mi && inner; k++) inner = (I[k][0] & 0xffff) != kLoopMerge;
        if (!inner) continue;   // (it may be innermost later)
        size_t ci = li + 1;
        while (ci < mi && !((I[ci][0] & 0xffff) == kLabel && I[ci][1] == cont)) ci++;
        size_t ce = ci + 1;
        while (ce < mi && !is_terminator(I[ce][0] & 0xffff) && (I[ce][0] & 0xffff) != kLabel) ce++;
        if (ci >= mi || ce >= mi || (I[ce][0] & 0xffff) != kBranch || I[ce][1] != H) { refused.insert(key); continue; }
        size_t sl = ci + 1;
        while (sl < ce && !(I[sl].size() == 5 && (I[sl][0] & 0xffff) == kIAdd && I[sl][2] == inext)) sl++;
        if (sl >= ce || I[sl][3] != i || !consts.count(I[sl][4])) { refused.insert(key); continue; }
        const int64_t start = consts[c0].second, n = consts[tl[4]].second, step = consts[I[sl][4]].second;
        const int64_t trips = step > 0 && n > start ? (n - start + step - 1) / step : 0;
        if (trips <= 0 || trips > (int64_t)max_trips) { refused.insert(key); continue; }
        // what the loop makes; names of it from outside; instructions the table does not describe
        std::unordered_set<uint32_t> made;
        bool ok = true;
        for (size_t k = li - 3; k < mi && ok; k++) {
            uint32_t r = 0;
            ok = for_each_id(I[k], [&](uint32_t& id, bool res) { if (res) r = id; });
            if (r) made.insert(r);
        }
        for (size_t k = 0; k < I.size() && ok; k++) {
            if (k >= li - 3 && k < mi) continue;
            Ins tmp = I[k];
            bool named = false;
            if (!for_each_id(tmp, [&](uint32_t& id, bool) { named |= id != H && made.count(id); })) {
                // (an instruction the table cannot walk: only acceptable outside functions' bodies where no id of ours can be)
                const uint32_t op = tmp[0] & 0xffff;
                if (op != 15 && op != 16 && op != 5 && op != 6 && op != 71 && op != 72 && op != 7 && op != 3 && op != 8) ok = false;
            }
            if (named) ok = false;
        }
        if (!ok) { refused.insert(key); continue; }
        std::vector<size_t> body;
        for (size_t k = li + 2; k < mi; k++)
            if (k != sl && k != ce) body.push_back(k);
        for (size_t k : body) {
            Ins tmp = I[k];
            for_each_id(tmp, [&](uint32_t& id, bool) { ok &= id != H && id != test && id != inext; });
        }
        if (!ok || (mi + 1 < I.size() && (I[mi + 1][0] & 0xffff) == kPhi)) { refused.insert(key); continue; }
        // the counter's values as constants (new ones after the last constant)
        std::vector<Ins> new_consts;
        auto value_id = [&](int64_t v) {
            for (auto& [id, tv] : consts)
                if (tv.first == ty && tv.second == v) return id;
            const uint32_t id = m.header[3]++;
            new_consts.push_back({(4u << 16) | kConstant, ty, id, (uint32_t)v});
            consts[id] = {ty, v};
            return id;
        };
        std::vector<Ins> out;
        std::vector<uint32_t> entries((size_t)trips), conts((size_t)trips);
        std::vector<std::vector<Ins>> copies((size_t)trips);
        for (int64_t it = 0; it < trips; it++) {
            std::unordered_map<uint32_t, uint32_t> ren;
            for (uint32_t id : made)
                if (id != H && id != i && id != test && id != inext) ren[id] = m.header[3]++;
            ren[i] = value_id(start + it * step);
            entries[(size_t)it] = ren.count(B) ? ren[B] : B;
            conts[(size_t)it] = ren[cont];
            for (size_t k : body) {
                Ins c = I[k];
                for_each_id(c, [&](uint32_t& id, bool) { auto r = ren.find(id); if (r != ren.end()) id = r->second; });
                copies[(size_t)it].push_back(std::move(c));
            }
        }
        out.push_back(I[li - 3]);
        out.push_back({(2u << 16) | kBranch, entries[0]});
        for (int64_t it = 0; it < trips; it++) {
            const uint32_t target = it + 1 < trips ? entries[(size_t)it + 1] : M;
            bool in_cont = false;
            for (Ins& c : copies[(size_t)it]) {
                const bool label = (c[0] & 0xffff) == kLabel;
                if (label && in_cont) out.push_back({(2u << 16) | kBranch, target}), in_cont = false;
                if (label && c[1] == conts[(size_t)it]) in_cont = true;
                out.push_back(std::move(c));
            }
            if (in_cont) out.push_back({(2u << 16) | kBranch, target});
        }
        std::vector<Ins> res;
        res.reserve(I.size() + out.size() + new_consts.size());
        for (size_t k = 0; k < li - 3; k++) {
            res.push_back(std::move(I[k]));
            if (k == last_const)
                for (Ins& c : new_consts) res.push_back(std::move(c));
        }
        for (Ins& c : out) res.push_back(std::move(c));
        for (size_t k = mi; k < I.size(); k++) res.push_back(std::move(I[k]));
        I.swap(res);
        return true;
    }
    return false;
}

/// Every loop that can be, innermost first, while the module stays under max_words: false when none was. left: the loops
/// still rolled
inline bool unroll_all(const uint32_t* code, size_t words, std::vector<uint32_t>& out, size_t max_words, uint32_t max_trips,
                       int* left = nullptr) {
    Module m;
    if (!parse(code, words, m)) return false;
    std::unordered_set<uint64_t> refused;
    int count = 0;
    size_t size = words;
    while (size < max_words) {
        Module keep = m;
        if (!unroll_one(m, refused, max_trips)) break;
        size = 5;
        for (const Ins& in : m.ins) size += in.size();
        if (size > max_words) { m = std::move(keep); break; }
        count++;
    }
    if (left) {
        *left = 0;
        for (const Ins& in : m.ins) *left += (in[0] & 0xffff) == kLoopMerge;
    }
    if (!count) return false;
    out = emit(m);
    return true;
}

} // namespace spirv_unroll
