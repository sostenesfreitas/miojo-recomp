// (hookshot) The states of what the game's programs need loaded (vk_programs.cpp), and nothing of Vulkan: tests/test_program_life.py.
//
// The game makes a program (gives it its shaders), draws with it some time later, and frees it. What the renderer keeps for a
// program is a unit: the kept program variants of those stages and the pipelines the lists name for them. Several of the game's
// programs can have the same stages, and so share one unit.
//
//   create  ->  Created  (the game made a program of these stages; nothing loaded)
//   queue   ->  Loading  (a background job loads the unit from what the prepare kept)
//   loaded  ->  Ready    (variants and pipelines in memory; a draw finds them)
//   release ->  Released (the game freed every program of the unit and it was let go; made again as Created by the next create)
//
// A first draw or dispatch asks use(): Ready draws at once, Loading makes the job urgent and waits for it, anything else (nothing
// was prepared for these stages, or the create call was never seen) takes the first-use path there was before. Nothing here
// translates or compiles: a unit is only ever loaded from prepared data.
//
// Release (phase C) is by count, not by time: only units every program of which the game has freed, only the excess above a
// threshold, the longest freed first, and never one that was loaded and not drawn yet (the game made it for something it has not
// shown: letting it go early is a reload for nothing).
#pragma once

#include <algorithm>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

namespace program_life {

enum class State : uint8_t { Created, Loading, Ready, Released };
enum class Use : uint8_t { Ready, Wait, Miss };

struct Unit {
    State state = State::Created;
    bool prepared = false;    // (the prepare kept something for these stages: there is a job to run)
    bool drawn = false;       // (a draw or dispatch used it since it was loaded)
    uint32_t live = 0;        // (the game's programs of these stages not freed yet)
    uint64_t freed_at = 0;    // (the order of the free call that left it with none: lower is longer ago)
    uint64_t loads = 0;       // (times loaded: more than one is a reload after a release)
};

class Table {
public:
    /// The game made program serial with the stages unit names; prepared: the prepare kept something for them.
    /// True: a load is to be queued (the caller calls loading(), then loaded() when the job is done)
    bool create(uint64_t serial, uint64_t unit, bool prepared) {
        // (a serial given other shaders without a free call between: the old stages' unit loses it)
        if (const auto old = serials_.find(serial); old != serials_.end()) drop(old->second);
        serials_[serial] = unit;
        Unit& u = units_[unit];
        u.live++;
        u.prepared = u.prepared || prepared;
        if (u.state == State::Released) u.state = State::Created;
        return u.state == State::Created && u.prepared;
    }
    /// A job for the unit is queued or running
    void loading(uint64_t unit) {
        if (const auto it = units_.find(unit); it != units_.end() && it->second.state == State::Created) it->second.state = State::Loading;
    }
    /// The unit's job ended. ok: what it loaded is in memory (not ok: nothing is, and a draw takes the first-use path)
    void loaded(uint64_t unit, bool ok) {
        const auto it = units_.find(unit);
        if (it == units_.end() || it->second.state != State::Loading) return;
        Unit& u = it->second;
        u.state = ok ? State::Ready : State::Created;
        u.prepared = u.prepared && ok;   // (a job that failed is not queued again by the next create)
        u.drawn = false;
        u.loads += ok;
    }
    /// A draw or dispatch uses the unit for the first time since it was loaded
    Use use(uint64_t unit) {
        const auto it = units_.find(unit);
        if (it == units_.end()) return Use::Miss;
        Unit& u = it->second;
        if (u.state == State::Loading) return Use::Wait;
        if (u.state != State::Ready) return Use::Miss;
        u.drawn = true;
        return Use::Ready;
    }
    /// The unit of a program the game made (0: no create call was seen for that serial)
    uint64_t unit_of(uint64_t serial) const {
        const auto it = serials_.find(serial);
        return it == serials_.end() ? 0 : it->second;
    }
    /// The game freed program serial (or gave it other shaders)
    void free(uint64_t serial) {
        const auto it = serials_.find(serial);
        if (it == serials_.end()) return;
        drop(it->second);
        serials_.erase(it);
    }
    /// The units to let go now: Ready, every program of theirs freed, drawn since they were loaded; only as many as there are
    /// such units above keep, the longest freed first. The caller releases each and calls released()
    std::vector<uint64_t> release_candidates(size_t keep) const {
        std::vector<std::pair<uint64_t, uint64_t>> freed;   // (freed_at, unit)
        for (const auto& [id, u] : units_)
            if (u.state == State::Ready && !u.live && u.drawn) freed.push_back({u.freed_at, id});
        std::vector<uint64_t> out;
        if (freed.size() <= keep) return out;
        // (the oldest first: a partial sort of the excess)
        const size_t excess = freed.size() - keep;
        std::partial_sort(freed.begin(), freed.begin() + (std::ptrdiff_t)excess, freed.end());
        for (size_t i = 0; i < excess; i++) out.push_back(freed[i].second);
        return out;
    }
    /// The unit was let go: a later create loads it again, from prepared data
    void released(uint64_t unit) {
        const auto it = units_.find(unit);
        if (it == units_.end() || it->second.state != State::Ready || it->second.live) return;
        it->second.state = State::Released;
        it->second.drawn = false;
    }
    const Unit* find(uint64_t unit) const {
        const auto it = units_.find(unit);
        return it == units_.end() ? nullptr : &it->second;
    }
    size_t units() const { return units_.size(); }
    size_t programs() const { return serials_.size(); }
    size_t count(State s) const {
        size_t n = 0;
        for (const auto& kv : units_) n += kv.second.state == s;
        return n;
    }

private:
    void drop(uint64_t unit) {
        const auto it = units_.find(unit);
        if (it == units_.end() || !it->second.live) return;
        if (!--it->second.live) it->second.freed_at = ++clock_;
    }
    std::unordered_map<uint64_t, uint64_t> serials_;   // program serial -> unit
    std::unordered_map<uint64_t, Unit> units_;
    uint64_t clock_ = 0;
};

} // namespace program_life
