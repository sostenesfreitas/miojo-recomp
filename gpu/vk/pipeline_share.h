/* (hookshot) Save my pipeline list (the Insert menu's Debug tab, vk_menu.cpp): the zip a player attaches to the "Submit your
   pipelines" issue (#2034), as plain functions. No Vulkan, no menu, no files: tests/test_pipeline_share.py compiles this header.
   The builders make the same zip with nx2/pipeline_share.py: the same two files, the same checks of the list (nx2/pipeline_states.py
   --check), the same template lines.

   The zip holds hookshot_pipeline_states.bin as the game wrote it and hookshot_pipeline_report.txt (the issue's template filled
   in), nothing else. Nothing is uploaded: the player attaches the zip to the issue and pastes the template. */
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace mkwvk::pipeline_share {

constexpr const char* kIssueUrl = "https://github.com/NotRexed/Hookshot/issues/2034";
constexpr const char* kListName = "hookshot_pipeline_states.bin";
constexpr const char* kReportName = "hookshot_pipeline_report.txt";
// (the issue's template, its lines in its order: nx2/pipeline_share.py FIELDS, the same)
constexpr const char* kFields[] = {"Hookshot version", "OS, GPU and driver", "Output resolution", "Upscaler + render resolution",
                                   "Frame generation / HDR", "Mods or texture packs",
                                   "Where you played (areas, dungeons, age, night/rain, ...)", "Pipelines"};
constexpr int kFieldCount = sizeof kFields / sizeof kFields[0];
constexpr int kWhere = 6;   // (left for the player to fill in)

/// The list's records as nx2/pipeline_states.py --check counts them (parse, strict): "HKSTATE2" with 552-byte records or
/// "HKSTATE1" with 528, whole records, at least one. -1 when it is not one to share, *why saying so in the player's words
inline long long check_list(const std::string& bytes, bool present, std::string* why) {
    auto no = [&](const std::string& text) { return *why = text, -1LL; };
    if (!present)
        return no(std::string("No pipeline list yet: ") + kListName + " is not in the game folder. The game writes it as you play: "
                  "play a while, then save it again.");
    uint32_t size = 0;
    if (bytes.size() >= 12) std::memcpy(&size, bytes.data() + 8, 4);
    const bool v2 = bytes.size() >= 12 && !bytes.compare(0, 8, "HKSTATE2") && size == 552;
    const bool v1 = bytes.size() >= 12 && !bytes.compare(0, 8, "HKSTATE1") && size == 528;
    if (!v1 && !v2)
        return no(std::string(kListName) + " cannot be shared: it is not a pipeline state list this build reads.");
    const size_t over = (bytes.size() - 12) % size;
    if (over)
        return no(std::string(kListName) + " cannot be shared: cut short (" + std::to_string(over) +
                  " bytes over the last whole record). Delete it, play again, then save it again.");
    const long long n = (long long)((bytes.size() - 12) / size);
    if (!n) return no(std::string(kListName) + " holds no pipelines yet. Play a while, then save it again.");
    return n;
}

/// The issue's template, filled in: "- <field>: <value>" a line (an empty value: "- <field>:", for the player)
inline std::string template_lines(const std::vector<std::string>& values) {
    std::string out;
    for (int i = 0; i < kFieldCount; i++) {
        const std::string v = i < (int)values.size() ? values[i] : "";
        out += std::string("- ") + kFields[i] + (v.empty() ? ":" : ": " + v) + "\n";
    }
    return out;
}
/// hookshot_pipeline_report.txt: a note on what to do with the zip, then the template
inline std::string report_text(const std::string& lines) {
    return std::string("Attach this zip to a comment on ") + kIssueUrl +
           " and paste the lines below into it, with where you played filled in.\nThe zip holds only " + kListName +
           " (shader hashes, formats and draw state, and the hashes of the compute programs your play ran: no game code, game data or "
           "personal info) and this text.\n\n" + lines;
}
/// How many of a checked list's records name a compute program (a version 2 record whose kind, its second last word, is 1:
/// vk_draw.cpp StateRec2::kCompute, nx2/pipeline_states.py COMPUTE). The list is zipped as it is: they go with the pipelines
inline long long compute_count(const std::string& bytes) {
    if (bytes.size() < 12 || bytes.compare(0, 8, "HKSTATE2")) return 0;
    long long n = 0;
    for (size_t at = 12; at + 552 <= bytes.size(); at += 552) {
        uint32_t kind = 0;
        std::memcpy(&kind, bytes.data() + at + 544, 4);
        n += kind == 1;
    }
    return n;
}
/// The Pipelines line: the count and the list's version, and how many of them are compute programs when it names any
inline std::string pipelines_text(long long n, bool v2, long long computes = 0) {
    return std::to_string(n) + (v2 ? " (version 2" : " (version 1") +
           (computes ? ", " + std::to_string(computes) + " of them compute programs)" : ")");
}
inline std::string mods_text(const std::vector<std::string>& names) {
    std::string out;
    for (const std::string& n : names) out += (out.empty() ? "" : ", ") + n;
    return out.empty() ? "none" : out;
}
/// The Output resolution line: the size, then in brackets the Handheld profile or the game's own size, and Ultrawide (docked only),
/// as nx2/pipeline_share.py output_text combines them
inline std::string output_text(unsigned w, unsigned h, bool handheld, bool game_own, bool ultrawide) {
    std::string extra = handheld ? "Handheld profile" : game_own ? "the game's own" : "";
    if (ultrawide && !handheld) extra += std::string(extra.empty() ? "" : ", ") + "Ultrawide on";
    return std::to_string(w) + "x" + std::to_string(h) + (extra.empty() ? "" : " (" + extra + ")");
}
/// hookshot-pipelines-<date>.zip
inline std::string zip_name(int year, int month, int day) {
    char b[48];
    snprintf(b, sizeof b, "hookshot-pipelines-%04d-%02d-%02d.zip", year, month, day);
    return b;
}

inline uint32_t crc32(const void* data, size_t n) {
    static uint32_t table[256];
    static bool made = false;
    if (!made) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        made = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ ((const uint8_t*)data)[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}
struct Entry {
    std::string name, data;
};
/// A zip of the entries, stored (no compression), each with the DOS date and time given: what any unzip and GitHub take
inline std::string zip(const std::vector<Entry>& entries, uint16_t dos_date, uint16_t dos_time) {
    std::string out, dir;
    auto u16 = [](std::string& s, uint32_t v) { s += (char)(v & 0xFF), s += (char)(v >> 8 & 0xFF); };
    auto u32 = [&](std::string& s, uint32_t v) { u16(s, v & 0xFFFF), u16(s, v >> 16); };
    for (const Entry& e : entries) {
        const uint32_t crc = crc32(e.data.data(), e.data.size()), size = (uint32_t)e.data.size(), at = (uint32_t)out.size();
        auto header = [&](std::string& s, bool central) {
            u32(s, central ? 0x02014B50u : 0x04034B50u);
            if (central) u16(s, 20);   // (made by: 2.0)
            u16(s, 20), u16(s, 0x0800), u16(s, 0);   // (needs 2.0; names in UTF-8; stored)
            u16(s, dos_time), u16(s, dos_date), u32(s, crc), u32(s, size), u32(s, size), u16(s, (uint32_t)e.name.size()), u16(s, 0);
            if (central) u16(s, 0), u16(s, 0), u16(s, 0), u32(s, 0), u32(s, at);   // (comment, disk, attributes, offset)
            s += e.name;
        };
        header(out, false);
        out += e.data;
        header(dir, true);
    }
    const uint32_t dir_at = (uint32_t)out.size();
    out += dir;
    u32(out, 0x06054B50u), u16(out, 0), u16(out, 0), u16(out, (uint32_t)entries.size()), u16(out, (uint32_t)entries.size());
    u32(out, (uint32_t)dir.size()), u32(out, dir_at), u16(out, 0);
    return out;
}
/// The zip to share: exactly the list and the report
inline std::string share_zip(const std::string& list, const std::string& lines, uint16_t dos_date, uint16_t dos_time) {
    return zip({{kListName, list}, {kReportName, report_text(lines)}}, dos_date, dos_time);
}

/// The Downloads folder named in user-dirs.dirs' text (XDG_DOWNLOAD_DIR="$HOME/Downloads"), with $HOME as home; "" when it names
/// none, or only the home folder
inline std::string xdg_download_dir(const std::string& text, const std::string& home) {
    size_t at = 0;
    while (at < text.size()) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(at, end - at);
        at = end + 1;
        const size_t k = line.find_first_not_of(" \t");
        if (k == std::string::npos || line.compare(k, 16, "XDG_DOWNLOAD_DIR")) continue;
        const size_t q0 = line.find('"'), q1 = line.rfind('"');
        if (q0 == std::string::npos || q1 <= q0) continue;
        std::string d = line.substr(q0 + 1, q1 - q0 - 1);
        if (!d.compare(0, 5, "$HOME")) d = home + d.substr(5);
        while (d.size() > 1 && d.back() == '/') d.pop_back();
        if (d.empty() || d[0] != '/' || d == home) return "";
        return d;
    }
    return "";
}

}  // namespace mkwvk::pipeline_share
