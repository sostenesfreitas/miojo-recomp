// SPDX-License-Identifier: GPL-3.0-or-later
// (hookshot) The shader module identifiers of kept programs (VK_EXT_shader_module_identifier), by program key and variant, in a
// side file beside the program cache: with one, a pipeline the driver's or the start's cache holds is made without the program's
// code being read and without a module. The file is good for one program cache (its stamp) and one driver's identifiers (the
// algorithm UUID): any other, and it starts empty. Nothing of Vulkan here: tests/test_module_ids.py runs it alone.
#pragma once
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace module_ids {

constexpr uint32_t kMost = 32;   // (VK_MAX_SHADER_MODULE_IDENTIFIER_SIZE_EXT)
struct Id {
    uint32_t n = 0;
    uint8_t bytes[kMost]{};
};

class Store {
public:
    /// The file read (missing, of another stamp or driver, or cut short at its end: what reads is kept, the rest is not)
    void open(const std::string& path, uint64_t stamp, const uint8_t uuid[16]) {
        std::lock_guard lk(mu_);
        path_ = path, stamp_ = stamp;
        std::memcpy(uuid_, uuid, 16);
        ids_.clear(), pending_.clear(), fresh_ = true;
        FILE* f = fopen(path.c_str(), "rb");
        if (!f) return;
        uint8_t head[kHead];
        if (fread(head, 1, kHead, f) == kHead && !std::memcmp(head, kMagic, 8) && !std::memcmp(head + 8, &stamp, 8) &&
            !std::memcmp(head + 16, uuid, 16)) {
            fresh_ = false;
            uint8_t e[kEntry];
            while (fread(e, 1, kEntry, f) == kEntry) {
                Entry k;
                Id id;
                std::memcpy(&k.key, e, 8), std::memcpy(&k.variant, e + 8, 8), std::memcpy(&id.n, e + 16, 4);
                if (!id.n || id.n > kMost) break;   // (not an entry: nothing after it is trusted)
                std::memcpy(id.bytes, e + 20, kMost);
                ids_[k] = id;
            }
        }
        fclose(f);
    }
    bool find(uint64_t key, uint64_t variant, Id* out) const {
        std::lock_guard lk(mu_);
        const auto it = ids_.find({key, variant});
        if (it == ids_.end()) return false;
        *out = it->second;
        return true;
    }
    bool has(uint64_t key, uint64_t variant) const {
        std::lock_guard lk(mu_);
        return ids_.count({key, variant}) != 0;
    }
    /// One learned: kept, and written by the next flush. False: known already (the same or not: the first stays), or not one
    bool add(uint64_t key, uint64_t variant, const uint8_t* bytes, uint32_t n) {
        if (!n || n > kMost) return false;
        Id id;
        id.n = n;
        std::memcpy(id.bytes, bytes, n);
        std::lock_guard lk(mu_);
        if (!ids_.emplace(Entry{key, variant}, id).second) return false;
        pending_.push_back({{key, variant}, id});
        return true;
    }
    size_t size() const {
        std::lock_guard lk(mu_);
        return ids_.size();
    }
    bool pending() const {
        std::lock_guard lk(mu_);
        return !pending_.empty();
    }
    /// What was learned since the last flush, appended (the file begun again when it was another's). False: it did not write
    bool flush() {
        std::lock_guard lk(mu_);
        if (pending_.empty() || path_.empty()) return true;
        FILE* f = fopen(path_.c_str(), fresh_ ? "wb" : "ab");
        if (!f) return false;
        bool ok = true;
        if (fresh_) {
            uint8_t head[kHead];
            std::memcpy(head, kMagic, 8), std::memcpy(head + 8, &stamp_, 8), std::memcpy(head + 16, uuid_, 16);
            ok = fwrite(head, 1, kHead, f) == kHead;
        }
        for (const auto& [k, id] : pending_) {
            uint8_t e[kEntry];
            std::memcpy(e, &k.key, 8), std::memcpy(e + 8, &k.variant, 8), std::memcpy(e + 16, &id.n, 4);
            std::memcpy(e + 20, id.bytes, kMost);
            ok = ok && fwrite(e, 1, kEntry, f) == kEntry;
        }
        ok = fclose(f) == 0 && ok;
        if (ok) pending_.clear(), fresh_ = false;
        return ok;
    }

private:
    struct Entry {
        uint64_t key = 0, variant = 0;
        bool operator==(const Entry& o) const { return key == o.key && variant == o.variant; }
    };
    struct Hash {
        size_t operator()(const Entry& e) const { return (size_t)((e.key ^ e.variant * 0x9E3779B97F4A7C15ull) * 1099511628211ull); }
    };
    static constexpr size_t kHead = 32, kEntry = 20 + kMost;
    static constexpr char kMagic[9] = "HKMODID1";
    mutable std::mutex mu_;
    std::string path_;
    uint64_t stamp_ = 0;
    uint8_t uuid_[16]{};
    bool fresh_ = true;   // (no file of this stamp and driver yet: the next flush begins one)
    std::unordered_map<Entry, Id, Hash> ids_;
    std::vector<std::pair<Entry, Id>> pending_;
};

}   // namespace module_ids
