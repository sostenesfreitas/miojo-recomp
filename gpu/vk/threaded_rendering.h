// (hookshot) Threaded rendering, the Insert menu's row (vk_menu.cpp, threaded_rendering= in hookshot_options.ini): Auto, On or
// Off, as one rule every reader shares: the menu (menu_threaded_rendering resolves it once, at the start), the replay
// (vk_replay.cpp render_thread_mode), the recorder (vk_recorder.h mode_from_env), the host's present poll (HOS_OPT_RENDER_THREAD,
// host/nvn_native.c) and the checks (tests/test_threaded_rendering.py compiles this file alone).
//
//   Off    the replay on the game's submitting thread, the recorder as RX_VK_RECORDER chooses it: as before the row
//   On     the render thread (what RX_VK_ASYNC=1 gives: the replay on a thread of its own, the game a frame ahead of it) and
//          the recorder thread (what RX_VK_RECORDER=thread gives) together, whatever the CPU
//   Auto   (the default) decided from the logical processors the process may run on (its affinity mask, usable_cpus, not
//          the PC's count): kAutoCpus or more, the render thread and the recorder thread; fewer, off. And off where the mode
//          is not safe, whatever the CPU (the caller says so: the capture off, pools with no mirrors,
//          RX_VK_LOW_SPEC, a frame dump, a benchmark replay). All of that behind kAutoEnabled, below, which is on:
//          turned off, Auto is Off on every PC.
//
// The environment's switches stay the overrides they were, for testing: RX_VK_ASYNC set chooses the render thread and
// RX_VK_RECORDER set the recorder, each over the row; unset, the row decides. Read once, at the start: a change of the row
// takes effect at the next start (the replay's async_on and the recorder's mode are read once).
#pragma once

namespace mkwvk::threaded {

enum { AUTO = 0, ON = 1, OFF = 2 };   // (the row's values, in V-Sync's order: Auto, On, Off)

/// THE GATE: Auto's one switch. True: Auto follows the rule below (kAutoCpus, and off where unsafe), so a PC with kAutoCpus
/// logical processors for the game runs the render thread and the recorder thread by default. False: Auto is Off
/// everywhere and the row changes nobody's default; nothing else needs to change to turn Auto off again. Still open with
/// it on: the game's large GPU-written tables (Hyrule Field's culling tables) can be read a frame old behind the game.
constexpr bool kAutoEnabled = true;
/// Auto's threshold: the logical processors the process may run on, from which it picks the render thread and the recorder
/// thread (the game's own threads, the replay, the recorder and the parallel replay's workers beside one another)
constexpr unsigned kAutoCpus = 12;

enum class Why {
    On,            // the row says On
    Off,           // the row says Off
    AutoGate,      // Auto, kAutoEnabled off: off everywhere
    AutoUnsafe,    // Auto, and the mode is not safe on this run
    AutoFewCpus,   // Auto, fewer than kAutoCpus logical processors (or their count unknown: 0)
    AutoCpus,      // Auto, kAutoCpus or more: on
};
struct Choice {
    bool on;   // (the render thread and the recorder thread, together)
    Why why;
};

/// What Auto picks for `cpus` logical processors where the mode is safe, the gate aside
constexpr bool auto_picks(unsigned cpus) { return cpus >= kAutoCpus; }

/// The rule, from the row's value, the logical processors the process may run on and whether the mode is unsafe on this
/// run: no environment, no clock, for the checks. `auto_enabled`: the gate (the checks pass it both ways). A value that is
/// none of the row's is Auto
constexpr Choice choose(int setting, unsigned cpus, bool unsafe, bool auto_enabled = kAutoEnabled) {
    if (setting == ON) return {true, Why::On};
    if (setting == OFF) return {false, Why::Off};
    if (!auto_enabled) return {false, Why::AutoGate};
    if (unsafe) return {false, Why::AutoUnsafe};
    return auto_picks(cpus) ? Choice{true, Why::AutoCpus} : Choice{false, Why::AutoFewCpus};
}

} // namespace mkwvk::threaded
