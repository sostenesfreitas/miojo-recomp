// (hookshot) The programs the driver's compiler faulted on, kept out of the driver for good (vk_draw.cpp build_graphics).
//
// guarded_graphics catches an access violation inside vkCreateGraphicsPipelines so that one draw is lost, not the game. On
// AMD's Windows driver (amdvlk64.dll, driver 0x800161: issue 996) the fault leaves that program's compile unfinished inside
// the driver. Every later vkCreateGraphicsPipelines of the same program, with any state, then waits for it on a condition
// variable that is never signalled. A full dump of the freeze on closing the pause menu (RX 6800 XT, v2.0.13) showed it:
// - the presentation thread waited for program 6eaa646d07ea4e43's pipeline;
// - the worker building it and a precompile thread were both inside the driver on that same program;
// - a second precompile thread was inside the driver on 5fa56485a6b07619.
// Both programs were among the three logged "not made (-13)" (the fault) at every start, and those two precompile threads
// never finished ("6625 of 6627 done, nothing finished for 30 s"). There was no TDR: the GPU was idle and the driver was
// waiting on itself.
//
// So a program the driver faulted on is not asked of it again:
// - in this run (Quarantine::begin answers kFaulted for it);
// - in later runs on the same driver (hookshot_driver_faults.txt beside the executable, one "<vendor> <driver> <key>" line a
//   fault, read at the first build; a driver update tries them again).
// Where the driver needs it, the builds of one program also run one at a time (serial): a second thread waits in
// Quarantine::begin, outside the driver, and learns of the first one's fault there instead of being caught in the driver behind
// it. That is on AMD by default, where the hang was seen. Other drivers keep building a program's state variants in parallel, as
// before, and only the quarantine applies to them.
// RX_VK_DRIVER_FAULTS=0: off (every program asked of the driver, as before); =2: serial on every driver.
#pragma once

#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>

namespace driver_faults {

/// A line of hookshot_driver_faults.txt: "<vendor> <driver version> <program key>", in hex
inline std::string line(uint32_t vendor, uint32_t driver, uint64_t key) {
    char b[64];
    snprintf(b, sizeof b, "%04x %08x %016llx\n", vendor, driver, (unsigned long long)key);
    return b;
}

/// The keys the file holds for this vendor and driver version; the others' lines, comments and malformed lines are skipped
inline std::unordered_set<uint64_t> parse(const std::string& text, uint32_t vendor, uint32_t driver) {
    std::unordered_set<uint64_t> keys;
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        const std::string l = text.substr(at, end - at);
        at = end + 1;
        unsigned v = 0, d = 0;
        unsigned long long k = 0;
        char extra = 0;
        if (l.empty() || l[0] == '#') continue;
        if (sscanf(l.c_str(), "%x %x %llx %c", &v, &d, &k, &extra) != 3) continue;
        if (v == vendor && d == driver && k) keys.insert(k);
    }
    return keys;
}

/// The programs kept out of the driver, and the gate that lets one build of a program at a time into it
class Quarantine {
public:
    /// The keys of earlier runs (parse); added to those this run found
    void load(const std::unordered_set<uint64_t>& keys) {
        std::lock_guard lk(mu_);
        bad_.insert(keys.begin(), keys.end());
    }
    bool contains(uint64_t key) const {
        std::lock_guard lk(mu_);
        return bad_.count(key) != 0;
    }
    /// One build of a program in the driver at a time (true), or as many as ask (false: the quarantine alone). Set before the
    /// first build
    void set_serial(bool serial) {
        std::lock_guard lk(mu_);
        serial_ = serial;
    }
    bool serial() const {
        std::lock_guard lk(mu_);
        return serial_;
    }
    enum Gate { kGo, kBusy, kFaulted };
    /// Before a build of the program goes to the driver. kGo: call it, then end(). kFaulted: the program is (or has just become)
    /// one the driver faulted on: do not call it. When serial, while another thread builds the same program, waits for that
    /// build to end; with wait false (a build that must not block, such as a cold probe on the render thread) answers kBusy
    /// instead. When not serial, never waits nor answers kBusy
    Gate begin(uint64_t key, bool wait = true) {
        std::unique_lock lk(mu_);
        if (!serial_) return bad_.count(key) ? kFaulted : kGo;
        if (wait) cv_.wait(lk, [&] { return bad_.count(key) || !building_.count(key); });
        if (bad_.count(key)) return kFaulted;
        if (building_.count(key)) return kBusy;
        building_.insert(key);
        return kGo;
    }
    /// After the driver returned (begin was kGo). faulted: it faulted on the program; true when that is new
    bool end(uint64_t key, bool faulted) {
        bool fresh = false;
        {
            std::lock_guard lk(mu_);
            building_.erase(key);
            if (faulted) fresh = bad_.insert(key).second;
        }
        cv_.notify_all();
        return fresh;
    }

private:
    mutable std::mutex mu_;
    std::condition_variable cv_;
    std::unordered_set<uint64_t> bad_, building_;
    bool serial_ = true;
};

}  // namespace driver_faults
