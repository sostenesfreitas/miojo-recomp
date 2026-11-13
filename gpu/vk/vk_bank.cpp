// Texture / sampler registrations and the driver's constant banks (from the WebGPU renderer's gpu/render/rx_nvn.cpp,
// whose comments explain the layouts: measured on TOTK by the MIT-licensed reference layer, NVN2 compute's packed c0
// found from MKW's programs).
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "vk_internal.h"
#include "vk_state.h"
#ifndef HOOKSHOT_ENV_H   // (env_on: the switches' one rule)
#include "hookshot_env.h"
#endif

namespace mkwvk {
// (hookshot) the seed's compute programs (vk_shader.cpp seed_program) have no bound program to read their layout from. Translated
// with Maxwell's table instead, one kept the right key and the wrong storage buffers, and play would have used it as it was
thread_local const uint8_t* seed_cs_control = nullptr;
namespace {

struct TexReg {
    nvn_tex t{};
    nvn_view v{};
    bool has_view = false, ok = false, released = false;
};
struct SampReg {
    nvn_samp_par p{};
    bool ok = false;
};
std::mutex reg_mu;
std::vector<TexReg> tex_regs;
std::vector<SampReg> samp_regs;
std::unordered_map<uint64_t, std::vector<int>> regs_by_serial;
std::atomic<uint64_t> reg_gen{1};   // (bumped by every texture registration: resolved handles go stale)
std::atomic<uint64_t> samp_gen{1};  // (bumped when a sampler registration's contents change: sampler_for's cache, vk_draw.cpp)
// (and a version a slot: caches of what a slot resolves to check theirs, not every registration's)
constexpr int kSlotVersions = 1 << 20;
std::unique_ptr<std::atomic<uint32_t>[]> slot_versions{new std::atomic<uint32_t>[kSlotVersions]()};
// (hookshot, RX_VK_SAMPLER_CACHE: the same for sampler ids, so what a sampler id resolves to can be cached and checked
// without taking reg_mu, and descriptor reuse can tell that a sampler was registered anew)
constexpr int kSampVersions = 1 << 12;
std::unique_ptr<std::atomic<uint32_t>[]> samp_versions{new std::atomic<uint32_t>[kSampVersions]()};

// NVN2 compute's c0: the workgroup counts at 0, then the uniform buffer table, the storage buffer table, texture
// handles and image handles, as many slots of each as the control section says (+0xb1, +0xb2, +0xb3, +0xb6)
// (hookshot) with push constants first, right after the counts: +0xa9 is their size in words (OoT's NV12 -> RGBA
// shader: 15, reading 0x10..0x4b; its upscaler's last pass: 21), the area rounded to 16 bytes; +0xba is the whole
// bank's size (0x10 + push area + the tables), which this matches for every program OoT dispatches.
struct CsLayout {
    uint32_t ubo = 0, ssbo = 0, tex = 0, img = 0, push = 0;
    uint32_t ubo_at() const { return 0x10 + push; }
    uint32_t ssbo_at() const { return ubo_at() + 16 * ubo; }
    uint32_t tex_at() const { return ssbo_at() + 16 * ssbo; }
    uint32_t img_at() const { return tex_at() + 8 * tex; }
    uint32_t end() const { return img_at() + 8 * img; }
};
CsLayout cs_layout_of(const uint8_t* c) {   // (c: the control section from +0x58)
    CsLayout l;
    l.ubo = c[0xb1 - 0x58], l.ssbo = c[0xb2 - 0x58], l.tex = c[0xb3 - 0x58], l.img = c[0xb6 - 0x58];
    l.push = ((uint32_t)c[0xa9 - 0x58] * 4 + 15) & ~15u;
    return l;
}
CsLayout cs_layout(const NvnState& s) {
    if (seed_cs_control) return cs_layout_of(seed_cs_control + 0x58);
    static std::mutex mu;
    static std::unordered_map<uint64_t, CsLayout> by_serial;
    std::lock_guard lk(mu);
    if (auto it = by_serial.find(s.prog.serial); it != by_serial.end()) return it->second;
    CsLayout l;
    for (uint32_t i = 0; i < s.prog.nstage && i < 6; i++) {
        uint8_t c[0x60];
        uint32_t magic = 0;
        uint16_t st = 0;
        if (!s.prog.sd[i].control || !host->gpu_read(s.prog.sd[i].control + 0x58, c, sizeof c)) continue;
        host->gpu_read(s.prog.sd[i].control, &magic, 4);
        host->gpu_read(s.prog.sd[i].control + 0x56, &st, 2);
        if (magic != 0x98761234 || st != 5) continue;
        l = cs_layout_of(c);
        static const bool log_cs = env_on("RX_VK_LOG_CS");   // (hookshot) the control bytes 0xb0..0xbf
        if (log_cs) {
            char hex[700];
            int k = 0;
            uint8_t wide[0xc0]{};
            host->gpu_read(s.prog.sd[i].control + 0x58, wide, sizeof wide);
            for (int j = 0; j < 0xc0; j++) k += snprintf(hex + k, sizeof hex - k, "%s%02x", j % 16 ? "" : " |", wide[j]);
            logf("vk: compute control %#llx: %s(ubo %u ssbo %u tex %u img %u; push constants %u bytes)",
                 (unsigned long long)s.prog.sd[i].control, hex, l.ubo, l.ssbo, l.tex, l.img, s.cs_push_n);
        }
        break;
    }
    if (by_serial.size() > 4096) by_serial.clear();
    by_serial[s.prog.serial] = l;
    return l;
}

} // namespace

/// (EvilOOT) where the bound compute program's storage buffer table is in c0, and how many slots it has (vk_shader.cpp gives
/// them to the translator: Eden's Maxwell layout had it at 0x310, NVN2 compute right after the uniform buffer table)
uint32_t compute_ssbo_table(const NvnState& s, uint32_t* count, uint32_t* layout_end) {
    const CsLayout l = cs_layout(s);
    if (count) *count = l.ssbo;
    if (layout_end) *layout_end = l.end();
    return l.ssbo_at();
}

/// (hookshot, #940) For the seed's compute programs (vk_shader.cpp seed_compute), whether c0+offset holds a buffer's address in play:
/// the address word of an entry of the uniform or storage buffer table its control section lays out. Nothing is bound before the
/// game, so the storage buffer pass's port (EDEN_NX2_GM_LEGACY=0), which binds an access to a pointer only when that pointer is
/// mapped (Env::IsCbufStorageBufferPointer), found none and left every access to the run-time search, under the key play then used
/// the program by. Answered from the layout, the prepared program comes out as play translates it
bool seed_cs_buffer_word(uint32_t offset) {
    if (!seed_cs_control || (offset & 15)) return false;
    const CsLayout l = cs_layout_of(seed_cs_control + 0x58);
    const auto in = [offset](uint32_t at, uint32_t n) { return offset >= at && offset < at + 16 * std::min(n, 16u); };
    return in(l.ubo_at(), l.ubo) || in(l.ssbo_at(), l.ssbo);
}

const void* descriptor_ok(const void* p, size_t n) {
    if (!p) return nullptr;
    const uintptr_t q = (uintptr_t)p, base = (uintptr_t)host->guest_base();
    if (q >= base && q + n <= base + host->guest_size()) return p;
    // (the host's own copies: registrations made before the renderer started, kept by host/rx_backend.c. A registration's
    // pointer is a host pointer, never a GPU address: asked as one (host->gpu_ptr), a heap address between 16 MB and 512 GB,
    // as Windows gives them, named guest memory, and the registration was copied from there, all zero: the game's sampler
    // 258 filtered nearest and texture slot 0x102 had no image, issue 1117's blocky halos)
    MEMORY_BASIC_INFORMATION mi{};
    if (VirtualQuery(p, &mi, sizeof mi) == sizeof mi && mi.State == MEM_COMMIT &&
        (mi.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) && !(mi.Protect & PAGE_GUARD) &&
        q + n <= (uintptr_t)mi.BaseAddress + mi.RegionSize)
        return p;
    static int logged;
    if (logged++ < 8) logf("vk: descriptor outside guest memory: refused");
    return nullptr;
}
void register_texture(int id, const nvn_tex* t, const nvn_view* v) {
    if (id < 0 || id >= (1 << 20) || !t) return;
    t = (const nvn_tex*)descriptor_ok(t, sizeof(nvn_tex));
    if (!t) return;
    v = (const nvn_view*)descriptor_ok(v, sizeof(nvn_view));   // a bad view only loses the view
    register_texture_copy(id, *t, v);
}
void register_texture_copy(int id, const nvn_tex& tex, const nvn_view* v) {
    if (id < 0 || id >= (1 << 20)) return;
    const nvn_tex* t = &tex;
    std::lock_guard lk(reg_mu);
    if ((size_t)id >= tex_regs.size()) tex_regs.resize((size_t)id + 1024);
    TexReg& r = tex_regs[id];
    {   // (slots registered again at another size: the dynamic resolution's steps, counted and logged now and then)
        static int64_t resized, all;
        all++;
        if (r.ok && (r.t.d.width != t->d.width || r.t.d.height != t->d.height)) {
            resized++;
            static int64_t logged_at = 0;
            if (resized - logged_at >= 200000) {
                logged_at = resized;
                logf("vk: texture slots registered again at another size: %lld (of %lld registrations; slot %d %ux%u -> %ux%u)",
                     (long long)resized, (long long)all, id, r.t.d.width, r.t.d.height, t->d.width, t->d.height);
            }
        }
    }
    if (r.ok) {
        auto it = regs_by_serial.find(r.t.serial);
        if (it != regs_by_serial.end()) {
            std::erase(it->second, id);
            if (it->second.empty()) regs_by_serial.erase(it);
        }
    }
    r.t = *t;
    r.has_view = v != nullptr;
    r.v = v ? *v : nvn_view{};
    r.ok = true;
    r.released = false;
    regs_by_serial[t->serial].push_back(id);
    slot_versions[id].fetch_add(1, std::memory_order_acq_rel);
    reg_gen++;
}
uint32_t texture_slot_version(int id) {
    return id >= 0 && id < kSlotVersions ? slot_versions[id].load(std::memory_order_acquire) : 0;
}
uint64_t registrations_generation() {
    return reg_gen.load(std::memory_order_acquire);
}
void register_sampler(int id, const nvn_samp_par* p) {
    if (id < 0 || id >= (1 << 12) || !p) return;
    p = (const nvn_samp_par*)descriptor_ok(p, sizeof(nvn_samp_par));
    if (!p) return;
    register_sampler_copy(id, *p);
}
void register_sampler_copy(int id, const nvn_samp_par& par) {
    if (id < 0 || id >= (1 << 12)) return;
    const nvn_samp_par* p = &par;
    std::lock_guard lk(reg_mu);
    if ((size_t)id >= samp_regs.size()) samp_regs.resize((size_t)id + 256);
    // (sampler_for's cache goes stale only when a registration's contents change, not at each rewrite of the same)
    const bool same = samp_regs[id].ok && !std::memcmp(&samp_regs[id].p, p, sizeof *p);
    samp_regs[id].p = *p;
    samp_regs[id].ok = true;
    if (!same) samp_gen.fetch_add(1, std::memory_order_acq_rel);
    samp_versions[id].fetch_add(1, std::memory_order_acq_rel);   // (hookshot: caches of what an id resolves to check this)
}
uint32_t sampler_slot_version(int id) {
    return id >= 0 && id < kSampVersions ? samp_versions[id].load(std::memory_order_acquire) : 0;
}
uint64_t sampler_registrations_generation() {
    return samp_gen.load(std::memory_order_acquire);
}
void samplers_changed() {
    samp_gen.fetch_add(1, std::memory_order_acq_rel);
}
void texture_released(uint64_t serial) {
    std::lock_guard lk(reg_mu);
    auto it = regs_by_serial.find(serial);
    if (it == regs_by_serial.end()) return;
    for (int id : it->second)
        if (id >= 0 && (size_t)id < tex_regs.size()) tex_regs[id].released = true, slot_versions[id].fetch_add(1);
    reg_gen++;
}
bool texture_of(int id, nvn_tex* t, nvn_view* v, bool* has_view) {
    std::lock_guard lk(reg_mu);
    if (id < 0 || (size_t)id >= tex_regs.size() || !tex_regs[id].ok) return false;
    const TexReg& r = tex_regs[id];
    *t = r.t, *v = r.v, *has_view = r.has_view;
    return true;
}
bool sampler_of(int id, nvn_samp_par* p) {
    std::lock_guard lk(reg_mu);
    if (id < 0 || (size_t)id >= samp_regs.size() || !samp_regs[id].ok) return false;
    *p = samp_regs[id].p;
    return true;
}

void driver_bank(const NvnState& s, int bank, int stage, uint8_t d[0xA00]) {
    std::memset(d, 0, 0xA00);
    for (uint32_t off = 0; off < 0xA00; off += 4) {
        const uint32_t w = driver_bank_word(s, bank, stage, off);
        std::memcpy(d + off, &w, 4);
    }
}

// (hookshot) What task and mesh programs read through pointers: the words of their small storage buffers that are GPU addresses
// outside every binding, gathered into up to three regions of one pool each. c0 from 0x880 (after the mesh table, which nothing else
// uses) gives them to the programs as a table of storage buffers (translate_program.cpp AddGlobalTables). Without it those reads
// gave 0: skinned and instanced models lost their bones and collapsed into spikes.
struct PointerEntry { uint64_t addr, size; };
constexpr uint32_t kPointerTable = 0x880, kPointerEntries = 3;
const PointerEntry* pointer_table(const NvnState& s, int stage) {
    thread_local PointerEntry e[kPointerEntries];
    thread_local uint64_t for_gen = ~0ull;
    thread_local const NvnState* for_state = nullptr;
    thread_local int for_stage = -1;
    if (for_gen == s.bind_gen && for_state == &s && for_stage == stage) return e;
    for_gen = s.bind_gen, for_state = &s, for_stage = stage;
    std::memset(e, 0, sizeof e);
    uint64_t found[32];
    uint32_t n = 0;
    const auto bound = [&](uint64_t v) {
        for (const nvn_p_bind_buf& b : s.ssbo[stage])
            if (b.addr && v >= b.addr && v < b.addr + b.size) return true;
        return false;
    };
    for (const nvn_p_bind_buf& b : s.ssbo[stage]) {
        if (!b.addr || !b.size || b.size > 0x100) continue;
        uint64_t w[32]{};
        const uint32_t words = (uint32_t)std::min<uint64_t>(b.size / 8, 32);
        if (!memory_read_as_submitted(b.addr, w, (uint64_t)words * 8)) continue;
        for (uint32_t i = 0; i < words && n < 32; i++) {
            const uint64_t v = w[i];
            if ((v & 3) || v < (1ull << 32) || v >= (1ull << 40) || !host->gpu_guest(v) || bound(v)) continue;
            bool have = false;
            for (uint32_t k = 0; k < n; k++) have |= found[k] == v;
            if (!have) found[n++] = v;
        }
    }
    std::sort(found, found + n);
    uint32_t regions = 0;
    if (stage == 5 || stage == 6 || stage == 7) {   // (EvilOOT) compute, task and mesh: whole pools
        // (mesh programs too: once OoT's foliage draws were given work they read their instances through the same kind of
        // pointers, and with 1 MB regions every primitive was clipped away: positions read as 0)
        // OoT's culling passes follow pointers found in their records into lists that hold more pointers, spread over one
        // ~100 MB pool: regions of 1 MB past the first pointers missed them, the reads gave 0 and every object was culled.
        // The pools that hold the pointers are bound whole (read by the shader's search; GPU-only, never copied back, as
        // vk_draw.cpp binds the table), as far as a storage buffer reaches.
        const uint64_t most = std::min<uint64_t>(dev.props.limits.maxStorageBufferRange, 0xFFFFFF00ull) & ~0xFFull;
        for (uint32_t i = 0; i < n && regions < kPointerEntries; i++) {
            const uint64_t lo = gpu_pool_start(found[i]), hi = gpu_pool_end(found[i]);
            if (!hi || hi <= lo) continue;
            bool have = false;
            for (uint32_t k = 0; k < regions; k++) have |= e[k].addr == lo;
            if (have) continue;
            e[regions++] = {lo, std::min<uint64_t>(hi - lo, most)};
        }
        return e;
    }
    for (uint32_t i = 0; i < n && regions < kPointerEntries; regions++) {
        const uint64_t first = found[i], end = gpu_pool_end(first);
        if (!end) {
            i++, regions--;
            continue;
        }
        while (i + 1 < n && found[i + 1] < end && found[i + 1] - first < (64ull << 20)) i++;
        // (past the last pointer: what is left of the pool, 1 MB at most)
        e[regions] = {first, std::min<uint64_t>(found[i] + (1ull << 20), end) - first};
        i++;
    }
    return e;
}

void driver_bank_fill(const NvnState& s, int bank, int stage, uint8_t* d, uint32_t n) {
    std::memset(d, 0, n);
    if (stage < 0 || stage >= kNvnStages) return;
    auto put = [&](uint32_t off, uint32_t v) {
        if (off + 4 <= n) std::memcpy(d + off, &v, 4);
    };
    auto entry = [&](uint32_t off, const nvn_p_bind_buf& e) {
        put(off, (uint32_t)e.addr), put(off + 4, (uint32_t)(e.addr >> 32)), put(off + 8, (uint32_t)e.size);
    };
    if (bank == 2) {   // (driver_bank_word's layout: handles' low words, 8 bytes a slot)
        for (uint32_t i = 0; i < 32 && 0x20 + 8 * i < n; i++) put(0x20 + 8 * i, (uint32_t)s.tex[stage][i]);
        for (uint32_t i = 0; i < 9 && 0x120 + 8 * i < n; i++) put(0x120 + 8 * i, (uint32_t)s.img[stage][i]);
        for (uint32_t i = 0; i < 128 && 0x168 + 8 * i < n; i++) put(0x168 + 8 * i, (uint32_t)s.tex[stage][i]);
        for (uint32_t i = 0; i < 128 && 0x568 + 8 * i < n; i++) put(0x568 + 8 * i, (uint32_t)s.samp[stage][i]);
        return;
    }
    if (bank != 0) return;
    if (stage == 5) {
        const CsLayout l = cs_layout(s);
        for (uint32_t i = 0; i < 3; i++) put(4 * i, s.grid[i]);
        for (uint32_t o = 0; o + 4 <= l.push && 0x10 + o + 4 <= n; o += 4) {   // (hookshot) the push area
            uint32_t v = 0;
            if (o + 4 <= s.cs_push_n) std::memcpy(&v, s.cs_push + o, 4);
            put(0x10 + o, v);
        }
        for (uint32_t i = 0; i < std::min(l.ubo, 16u); i++) entry(l.ubo_at() + 16 * i, s.ubo[5][i]);
        for (uint32_t i = 0; i < std::min(l.ssbo, 16u); i++) entry(l.ssbo_at() + 16 * i, s.ssbo[5][i]);
        for (uint32_t i = 0; i < l.tex && l.tex_at() + 8 * i < n; i++) put(l.tex_at() + 8 * i, (uint32_t)s.tex[5][std::min(i, 127u)]);
        for (uint32_t i = 0; i < l.img && l.img_at() + 8 * i < n; i++) put(l.img_at() + 8 * i, (uint32_t)s.img[5][std::min(i, 31u)]);
        // (EvilOOT) compute programs read through pointers kept in their small storage buffers too (OoT's culling passes:
        // their object lists, reached through the setup pass's records): the same table as task and mesh programs get, at
        // 0x880, past the layout (translate_program.cpp AddGlobalTables gives it to compute programs whose layout ends before)
        if (l.end() <= kPointerTable && n > kPointerTable) {
            const PointerEntry* e = pointer_table(s, 5);
            for (uint32_t i = 0; i < kPointerEntries; i++) {
                const uint32_t off = kPointerTable + 16 * i;
                put(off, (uint32_t)e[i].addr), put(off + 4, (uint32_t)(e[i].addr >> 32)), put(off + 8, (uint32_t)e[i].size);
            }
        }
        return;
    }
    static constexpr uint32_t kSsboTable[kNvnStages] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
    if (const uint32_t base = kSsboTable[stage])
        for (uint32_t i = 0; i < 16 && base + 16 * i < n; i++) entry(base + 16 * i, s.ssbo[stage][i]);
    if ((stage == 6 || stage == 7) && n > kPointerTable) {
        const PointerEntry* e = pointer_table(s, stage);
        for (uint32_t i = 0; i < kPointerEntries; i++) {
            const uint32_t off = kPointerTable + 16 * i;
            put(off, (uint32_t)e[i].addr), put(off + 4, (uint32_t)(e[i].addr >> 32)), put(off + 8, (uint32_t)e[i].size);
        }
    }
    if (stage == 0) put(0x610, std::bit_cast<uint32_t>(2047.9375f));
}

/// (hookshot) the bound compute program's texture and image slot counts (its control section's)
void cs_slot_counts(const NvnState& s, uint32_t* tex, uint32_t* img) {
    const CsLayout l = cs_layout(s);
    *tex = l.tex, *img = l.img;
}

uint32_t driver_bank_word(const NvnState& s, int bank, int stage, uint32_t offset) {
    if (stage < 0 || stage >= kNvnStages || offset + 4 > 0xA00 || (offset & 3)) return 0;
    auto lo = [](uint64_t v) { return (uint32_t)v; };
    auto hi = [](uint64_t v) { return (uint32_t)(v >> 32); };
    if (bank == 2) {
        if (offset & 7) return 0;
        if (offset >= 0x568 && offset < 0x568 + 8 * 128) return lo(s.samp[stage][(offset - 0x568) / 8]);
        if (offset >= 0x168 && offset < 0x168 + 8 * 128) return lo(s.tex[stage][(offset - 0x168) / 8]);
        if (offset >= 0x120 && offset < 0x120 + 8 * 9) return lo(s.img[stage][(offset - 0x120) / 8]);
        if (offset >= 0x20 && offset < 0x20 + 8 * 32) return lo(s.tex[stage][(offset - 0x20) / 8]);
        return 0;
    }
    if (bank != 0) return 0;
    if (stage == 5) {
        const CsLayout l = cs_layout(s);
        if (offset < 0x10) return offset < sizeof s.grid ? s.grid[offset / 4] : 0;
        auto entry = [&](const nvn_p_bind_buf& e, uint32_t in) -> uint32_t {
            return in == 0 ? lo(e.addr) : in == 4 ? hi(e.addr) : in == 8 ? (uint32_t)e.size : 0;
        };
        if (offset < l.ubo_at()) {   // (hookshot) the push area
            uint32_t v = 0;
            if (offset - 0x10 + 4 <= s.cs_push_n) std::memcpy(&v, s.cs_push + (offset - 0x10), 4);
            return v;
        }
        if (offset < l.ssbo_at())
            return (offset - l.ubo_at()) / 16 < 16 ? entry(s.ubo[5][(offset - l.ubo_at()) / 16], (offset - l.ubo_at()) % 16) : 0;
        if (offset < l.tex_at())
            return (offset - l.ssbo_at()) / 16 < 16 ? entry(s.ssbo[5][(offset - l.ssbo_at()) / 16], (offset - l.ssbo_at()) % 16) : 0;
        if (offset < l.img_at()) return (offset & 7) ? 0 : lo(s.tex[5][std::min<uint32_t>((offset - l.tex_at()) / 8, 127)]);
        if (offset < l.end()) return (offset & 7) ? 0 : lo(s.img[5][std::min<uint32_t>((offset - l.img_at()) / 8, 31)]);
        // (EvilOOT) compute's pointer table, as task and mesh programs' (translate_program.cpp AddGlobalTables): past the
        // layout's end only
        if (l.end() <= kPointerTable && offset >= kPointerTable && offset < kPointerTable + 16 * kPointerEntries) {
            const PointerEntry& e = pointer_table(s, 5)[(offset - kPointerTable) / 16];
            const uint32_t in = (offset - kPointerTable) % 16;
            return in == 0 ? lo(e.addr) : in == 4 ? hi(e.addr) : in == 8 ? (uint32_t)e.size : 0;
        }
        return 0;
    }
    // (the storage buffer tables: 0x110 + 0x100 a hardware stage: vertex 0x110, tessellation control 0x210 and
    // evaluation 0x310, geometry 0x410, fragment 0x510; NVN2's task and mesh stages at 0x670 and 0x770: OoT binds index 0 to
    // both and its programs read those addresses. Taken for slot 6 of tables at 0x610 and 0x710, every task program read a null
    // buffer, found nothing to draw and made no primitives: the door, the fences and all else drawn this way never showed)
    static constexpr uint32_t kSsboTable[kNvnStages] = {0x110, 0x510, 0x410, 0x210, 0x310, 0, 0x670, 0x770};
    const uint32_t base = kSsboTable[stage];
    auto table = [&](const nvn_p_bind_buf* t, uint32_t at) -> uint32_t {
        const nvn_p_bind_buf& e = t[(offset - at) / 16];
        switch ((offset - at) % 16) {
        case 0: return lo(e.addr);
        case 4: return hi(e.addr);
        case 8: return (uint32_t)e.size;
        default: return 0;
        }
    };
    if (base && offset >= base && offset < base + 16 * 16) return table(s.ssbo[stage], base);
    if ((stage == 6 || stage == 7) && offset >= kPointerTable && offset < kPointerTable + 16 * kPointerEntries) {
        const PointerEntry& e = pointer_table(s, stage)[(offset - kPointerTable) / 16];
        const uint32_t in = (offset - kPointerTable) % 16;
        return in == 0 ? lo(e.addr) : in == 4 ? hi(e.addr) : in == 8 ? (uint32_t)e.size : 0;
    }
    if (stage == 0 && offset == 0x610) return std::bit_cast<uint32_t>(2047.9375f);
    return 0;
}

} // namespace mkwvk
