// The Insert menu's Bug report (vk_menu.cpp, the Gameplay tab): one press writes what an issue needs into a folder of its
// own, bug-reports/<date>_<time> in the game folder, and nothing else. report.txt holds the answers the issue form asks for
// (.github/ISSUE_TEMPLATE/1-game-problem.yml: the version, where the player is, the GPU and its driver, CPU, RAM and system,
// the options and switches changed from their defaults) under the form's own headings, so scripts/hardware_report.py reads
// them as it reads an issue; beside it this run's log and the last run's, the home folder and the user's name taken out of
// their paths, and a picture of the game's frame without the menu (vk_replay.cpp's shot_ask takes it at its next present). Nothing is
// sent anywhere: the player attaches the files to an issue. What is read and how it is written is here, apart from Vulkan
// and the menu, for tests/test_bug_report.py.
#pragma once

#include <cstdint>
#include <ctime>
#include <string>
#include <vector>

namespace mkwvk::bug_report {

/// What a report is made of: the menu's part (version, gpu, options, game_dir) and this process's (fill_here)
struct Inputs {
    std::string version;   // HOOKSHOT_VERSION, as the log's first line has it
    std::string gpu;       // the device the game runs on and its driver, as the form asks: "NVIDIA GeForce RTX 4090, NVIDIA 580.95.05"
    std::string options;   // the menu's rows that are not at their defaults, a "Name: value (default: value)" line each
    std::string system;    // CPU, RAM and system (system_text)
    std::vector<std::string> env;   // the environment, NAME=VALUE: the game's switches set, and software hooked in
    std::string game_dir;  // the game folder, its separator at the end: the logs are there, and the report's folder goes there
    std::string current_log;   // the file this run logs to (stderr), "" when that is no file (a terminal, a pipe)
    std::string home, user;    // taken out of the logs: the home folder and the user's name
    bool windows = false;      // paths as Windows writes them: either slash, and case does not matter
    std::time_t now = 0;
    int picture_wait_ms = 3000;   // how long the report waits for its picture before it goes without one
    void (*log)(const char* line) = nullptr;
    /// Asks the renderer for a PNG of its next frame without the menu at path, answered by picture_written(ticket, ...) once it
    /// is written (vk_menu.cpp: vk_replay.cpp's shot_ask). False, or none: the report goes without a picture
    bool (*ask_picture)(const char* path, uint64_t ticket) = nullptr;
};

/// The process's part of a report: system, env, current_log, home, user, windows, now
void fill_here(Inputs& in);
/// CPU (and its threads), RAM and system, as the form asks: "13th Gen Intel(R) Core(TM) i9-13900K (32 threads), 94 GB,
/// Arch Linux (Linux 7.2.15)"
std::string system_text();
/// A log's line with the home folder written ~ (%USERPROFILE% on Windows) and the user's name as a folder <user>
std::string redact(const std::string& line, const std::string& home, const std::string& user, bool windows);

/// Writes a report: its folder, relative to the game folder ("bug-reports/2026-11-10_22-41-05"), or "" when the folder could
/// not be made. Asks for the picture (ask_picture) and waits for it (picture_wait_ms)
std::string write(const Inputs& in);
/// write() on a thread of its own: false while one is still being written
bool start(Inputs in);
enum State { kIdle, kSaving, kSaved, kFailed };
State state();
/// The last report's folder relative to the game folder (kSaved), or why it was not written (kFailed)
std::string shown();

/// (the renderer: ask_picture's answer) The picture was written, or could not be. One that comes after the report went without
/// it (picture_wait_ms) is removed again, as the report says it has none
void picture_written(uint64_t ticket, bool ok);

}  // namespace mkwvk::bug_report
