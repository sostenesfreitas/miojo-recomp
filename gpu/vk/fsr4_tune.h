// AMD FSR 4's passes, tuned as the library hands them to the driver (vk_fsr4.cpp): plain functions over SPIR-V, no Vulkan
// calls, so tests/test_fsr4_tune.py builds and runs them as they are.
//
// Measured 2026-11-12 on an RX 6700 XT, Adrenalin 26.10.1, Windows 11 (FSR 4.1.2, 720p -> 1440p, all of its passes 8.97 ms),
// each change checked bit for bit against AMD's own output on that driver. None of them reorders the arithmetic, but with the
// weights constants in the code another driver (or RADV) may fold them differently: elsewhere the same maths, not
// necessarily the same bits:
//  - wave64 for the network's layers (entry points "..._passN"): they ask for 32 lanes, and their loops' scalar work is then
//    paid once per 32 pixels instead of 64: 8.97 -> 6.85 ms. The prepass and postpass stay at 32 (slower at 64).
//  - weights out of per-thread arrays: the postpass and the layers copy constant weight tables (2.8 KB; 576 and 128 words,
//    again at every iteration) into Function arrays and read them at loop-dependent places, which the driver keeps in
//    scratch memory. Read from a tail appended to the library's storage buffer instead (arrays_to_buffer): 6.85 -> 4.48 ms.
//  - the layers with weight tables that stay small unrolled whole (spirv_unroll.h, vk_fsr4.cpp): the weights become
//    constants in the code. Passes 1, 2 and 12, 0.45 -> 0.27 ms each: 4.04 ms in all.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "spirv_unroll.h"   // (its operand walker: which words of an instruction are ids)

namespace fsr4_tune {

/// The first compute entry point's name, "" when there is none
inline std::string entry_name(const uint32_t* code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return "";
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return "";
        if (op == 15 && n > 3 && code[i + 1] == 5)   // OpEntryPoint GLCompute
            return std::string((const char*)&code[i + 3], strnlen((const char*)&code[i + 3], (n - 3) * 4));
        i += n;
    }
    return "";
}

/// The module declares subgroup (GroupNonUniform*) or cooperative matrix capabilities: its lane count is part of its maths,
/// and it is left at the size it asked for
inline bool lane_dependent(const uint32_t* code, size_t words) {
    if (words < 5 || code[0] != 0x07230203u) return true;
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return true;
        if (op == 17 && n == 2) {   // OpCapability
            const uint32_t c = code[i + 1];
            if ((c >= 61 && c <= 69) || c == 4431 /* SubgroupBallotKHR */ || c == 6022 /* CooperativeMatrixKHR */) return true;
        } else if (op != 17 && op != 11 && op != 14 && op != 10) {
            break;   // (capabilities come first: past them, past the ext imports and the memory model)
        }
        i += n;
    }
    return false;
}

/// One of the network's layers ("..._pass<digits>", not "_post", the prepass or the postpass): built for 64 lanes
inline bool wants_wave64(const std::string& entry) {
    const size_t at = entry.rfind("_pass");
    if (at == std::string::npos || at + 5 >= entry.size()) return false;
    for (size_t i = at + 5; i < entry.size(); i++)
        if (entry[i] < '0' || entry[i] > '9') return false;
    return entry.compare(0, 5, "fsr4_") == 0;
}
/// The pass whose constant arrays move to the buffer's tail
inline bool is_postpass(const std::string& entry) {
    return entry.size() > 9 && entry.compare(entry.size() - 9, 9, "_postpass") == 0 && entry.compare(0, 5, "fsr4_") == 0;
}

/// Function-scope uint arrays that are stored once from a constant and then only read through OpAccessChain + OpLoad become
/// reads of the module's storage buffer (a struct of one runtime uint array, as an HLSL RWByteAddressBuffer) at word
/// base_word onward: out gets the module, tail the constants in the order the reads expect. false (out untouched): nothing
/// of that shape, or the module is not as expected
inline bool arrays_to_buffer(const uint32_t* code, size_t words, uint32_t base_word, std::vector<uint32_t>& out,
                             std::vector<uint32_t>& tail) {
    if (words < 5 || code[0] != 0x07230203u) return false;
    struct Ins { size_t at; uint32_t op, n; };
    std::vector<Ins> ins;
    for (size_t i = 5; i < words;) {
        const uint32_t n = code[i] >> 16, op = code[i] & 0xffff;
        if (!n || i + n > words) return false;
        ins.push_back({i, op, n});
        i += n;
    }
    const auto w = [&](const Ins& x, uint32_t k) { return code[x.at + k]; };
    uint32_t t_uint = 0;
    std::unordered_map<uint32_t, uint32_t> uconst;                 // id -> value (32-bit uint constants)
    std::unordered_map<uint32_t, std::vector<uint32_t>> composite; // id -> constituents
    std::unordered_map<uint32_t, uint32_t> arr_elem;               // array type -> element type
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> ptr; // pointer type -> (storage class, pointee)
    std::unordered_map<uint32_t, uint32_t> struct_member0, runtime_elem;
    std::unordered_map<uint32_t, uint32_t> var_type;               // variable -> pointer type
    std::unordered_set<uint32_t> read_only;                         // structs (member 0) and variables decorated NonWritable
    for (const Ins& x : ins) {
        switch (x.op) {
        case 71: if (x.n >= 3 && w(x, 2) == 24) read_only.insert(w(x, 1)); break;                 // OpDecorate NonWritable
        case 72: if (x.n >= 4 && w(x, 2) == 0 && w(x, 3) == 24) read_only.insert(w(x, 1)); break; // OpMemberDecorate 0 NonWritable
        case 21: if (x.n == 4 && w(x, 2) == 32 && w(x, 3) == 0) t_uint = w(x, 1); break;           // OpTypeInt 32 0
        case 28: arr_elem[w(x, 1)] = w(x, 2); break;                                              // OpTypeArray
        case 29: runtime_elem[w(x, 1)] = w(x, 2); break;                                          // OpTypeRuntimeArray
        case 30: if (x.n == 3) struct_member0[w(x, 1)] = w(x, 2); break;                          // OpTypeStruct, one member
        case 32: ptr[w(x, 1)] = {w(x, 2), w(x, 3)}; break;                                        // OpTypePointer
        case 43: if (x.n == 4) uconst[w(x, 2)] = w(x, 3); break;                                  // OpConstant (32-bit)
        case 44: composite[w(x, 2)].assign(&code[x.at + 3], &code[x.at + x.n]); break;            // OpConstantComposite
        case 59: var_type[w(x, 2)] = w(x, 1); break;                                              // OpVariable
        }
    }
    if (!t_uint) return false;
    // (the storage buffer to read: the one writable StorageBuffer variable of a struct of one runtime array of uint, the
    // library's ScratchBuffer, which the tail is added to (the layers also read a read-only InitializerBuffer); the pointer type
    // to its elements; the constant 0)
    uint32_t sb = 0, p_sb_uint = 0, c0 = 0;
    for (auto& [v, pt] : var_type) {
        auto p = ptr.find(pt);
        if (p == ptr.end() || p->second.first != 12) continue;   // StorageBuffer
        if (read_only.count(v) || read_only.count(p->second.second)) continue;
        auto m = struct_member0.find(p->second.second);
        if (m == struct_member0.end()) continue;
        auto r = runtime_elem.find(m->second);
        if (r != runtime_elem.end() && r->second == t_uint) { if (sb) return false; sb = v; }   // (one only: no guessing)
    }
    for (auto& [id, p] : ptr) if (p.first == 12 && p.second == t_uint) p_sb_uint = id;
    for (auto& [id, v] : uconst) if (v == 0 && !c0) {
        // (an unsigned 0: its type is t_uint)
        for (const Ins& x : ins) if (x.op == 43 && w(x, 2) == id && w(x, 1) == t_uint) { c0 = id; break; }
    }
    if (!sb || !p_sb_uint || !c0) return false;
    // candidates: Function variables of pointer-to-array-of-uint
    std::unordered_map<uint32_t, uint32_t> cand;   // var -> its one store's constant
    std::unordered_map<uint32_t, int> stores;
    for (auto& [v, pt] : var_type) {
        auto p = ptr.find(pt);
        if (p == ptr.end() || p->second.first != 7) continue;    // Function
        auto a = arr_elem.find(p->second.second);
        if (a != arr_elem.end() && a->second == t_uint) cand[v] = 0;
    }
    // (every store of the variable the same constant: once in the postpass; again at each iteration of the loop that reads it
    // in the network's layers, which copy their 576 weights in every time round)
    for (const Ins& x : ins)
        if (x.op == 62 && cand.count(w(x, 1))) {   // OpStore var value
            const uint32_t v = w(x, 1), c = composite.count(w(x, 2)) ? w(x, 2) : 0;
            cand[v] = stores[v]++ == 0 || cand[v] == c ? c : 0;
        }
    // their element pointers, and every other use of the variables and of those pointers
    std::unordered_map<uint32_t, uint32_t> chain_var;   // access chain result -> var
    std::unordered_set<uint32_t> bad;
    for (const Ins& x : ins)
        if (x.op == 65 && cand.count(w(x, 3))) {   // OpAccessChain type result base index...
            auto p = ptr.find(w(x, 1));
            if (x.n != 5 || p == ptr.end() || p->second.first != 7 || p->second.second != t_uint) bad.insert(w(x, 3));
            else chain_var[w(x, 2)] = w(x, 3);
        }
    // (only the words that are ids: a constant's value, a binding or a name's letters equal to a variable's id once kept that
    // variable where it was. An instruction the operand table does not describe has every word looked at)
    std::vector<uint32_t> at;
    for (const Ins& x : ins) {
        spirv_unroll::Ins one(&code[x.at], &code[x.at + x.n]);
        at.clear();
        if (!spirv_unroll::for_each_id(one, [&](uint32_t& id, bool) { at.push_back((uint32_t)(&id - one.data())); })) {
            at.clear();
            for (uint32_t k = 1; k < x.n; k++) at.push_back(k);
        }
        for (uint32_t k : at) {
            const uint32_t id = w(x, k);
            const bool is_var = cand.count(id), is_chain = chain_var.count(id);
            if (!is_var && !is_chain) continue;
            if (is_var && ((x.op == 59 && k == 2) || (x.op == 62 && k == 1) || (x.op == 65 && (k == 3 || k == 2)))) continue;
            if (is_chain && ((x.op == 65 && k == 2) || (x.op == 61 && k == 3))) continue;   // its definition; OpLoad's pointer
            if (x.op == 15 && k > 2) continue;   // (an entry point interface list names only globals: not these)
            if ((x.op == 5 || x.op == 71) && k == 1) continue;   // (OpName, OpDecorate: dropped with the variable)
            bad.insert(is_var ? id : chain_var[id]);
        }
    }
    std::unordered_map<uint32_t, uint32_t> base_of_const;   // constant composite -> word in the buffer
    std::unordered_map<uint32_t, uint32_t> moved;           // var -> its constant
    std::vector<uint32_t> new_tail;
    for (auto& [v, c] : cand) {
        if (bad.count(v) || !stores[v] || !c) continue;
        const auto& parts = composite[c];
        bool all = true;
        for (uint32_t e : parts) all = all && uconst.count(e);
        if (!all) continue;
        if (!base_of_const.count(c)) {
            base_of_const[c] = base_word + (uint32_t)new_tail.size();
            for (uint32_t e : parts) new_tail.push_back(uconst[e]);
        }
        moved[v] = c;
    }
    if (moved.empty()) return false;
    // new ids: a constant for each base, an index sum for each access chain
    uint32_t bound = code[3];
    std::unordered_map<uint32_t, uint32_t> base_id;
    for (auto& [c, b] : base_of_const) base_id[c] = bound++;
    std::vector<uint32_t> o(code, code + 5);
    o.reserve(words + 64);
    for (const Ins& x : ins) {
        if (x.op == 59 && moved.count(w(x, 2))) continue;   // the variable
        if ((x.op == 5 || x.op == 71) && moved.count(w(x, 1))) continue;   // its name and decorations
        if (x.op == 62 && moved.count(w(x, 1))) continue;   // its stores (all of the same constant)
        if (x.op == 65 && chain_var.count(w(x, 2)) && moved.count(chain_var[w(x, 2)])) {
            const uint32_t c = moved[chain_var[w(x, 2)]], sum = bound++;
            o.insert(o.end(), {(5u << 16) | 128u, t_uint, sum, w(x, 4), base_id[c]});                 // OpIAdd
            o.insert(o.end(), {(6u << 16) | 65u, p_sb_uint, w(x, 2), sb, c0, sum});                    // OpAccessChain
            continue;
        }
        o.insert(o.end(), &code[x.at], &code[x.at + x.n]);
        if (x.op == 43 && w(x, 2) == c0)   // (the bases right after the constant 0: among the constants, before any use)
            for (auto& [c, id] : base_id) o.insert(o.end(), {(4u << 16) | 43u, t_uint, id, base_of_const[c]});
    }
    o[3] = bound;
    out.swap(o);
    tail.insert(tail.end(), new_tail.begin(), new_tail.end());
    return true;
}

/// Whether a storage-buffer binding (offset and range as in VkDescriptorBufferInfo, ~0 for VK_WHOLE_SIZE) of a buffer the
/// library asked `size` bytes of, made `tail` bytes larger, takes in the tail the rewritten shaders read: they read it at
/// fixed places from the buffer's start, so only a binding at offset 0 that reaches past `size + tail` does
inline bool covers_tail(uint64_t offset, uint64_t range, uint64_t size, uint64_t tail) {
    return offset == 0 && (range == ~0ull || range >= size + tail);
}
/// The range to bind in a binding's place: exactly what the library asked for, at offset 0, widened to the tail (the buffer
/// is that large); any other as it was
inline uint64_t widened_range(uint64_t offset, uint64_t range, uint64_t size, uint64_t tail) {
    return offset == 0 && range == size ? size + tail : range;
}

} // namespace fsr4_tune
