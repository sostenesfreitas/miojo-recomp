// (hookshot) The Insert menu's languages (vk_menu_i18n.h).
//
// menu_i18n.h holds one table a language: each English text of vk_menu.cpp and its translation. A text is looked up whole;
// a text the menu made with snprintf is matched against the table's texts that hold printf conversions ("%d of %d") and
// put together from the translated format, its %s parts looked up in turn. Place and item names are not in the tables (the
// official translated names are not checked yet): they stay English. The table's pointers live for the process; a text
// made from a format is kept in a cache that is let go when it grows past a few thousand entries or the language changes,
// at the start of a frame's first lookup, so no pointer handed out during a frame is freed in it.
#include "vk_menu_i18n.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "menu_i18n.h"
#include "vk_menu.h"
#include "vk_paths.h"

namespace mkwvk::i18n {

namespace {

/// A printf conversion at f (after its '%'): its length, and its kind ('s' text, 'n' a number, 'c' a character, '%'), or 0
size_t conversion(const char* f, char& kind) {
    size_t i = 0;
    while (f[i] && strchr("-+#0", f[i])) i++;   // (not ' ': "80% suits" is a percent sign, not a "% s")
    while (f[i] >= '0' && f[i] <= '9') i++;
    if (f[i] == '.') {
        i++;
        while (f[i] >= '0' && f[i] <= '9') i++;
    }
    while (f[i] == 'l' || f[i] == 'h' || f[i] == 'z') i++;
    switch (f[i]) {
    case 's': kind = 's'; return i + 1;
    case 'd': case 'u': case 'f': case 'x': case 'g': kind = 'n'; return i + 1;
    case 'c': kind = 'c'; return i + 1;
    case '%': kind = '%'; return i + 1;
    default: return 0;
    }
}

/// Whether text t is what format f prints; its conversions' texts into args
bool match_format(const char* f, const char* t, std::vector<std::string>& args) {
    while (*f) {
        if (*f != '%') {
            if (*f != *t) return false;
            f++, t++;
            continue;
        }
        char kind = 0;
        const size_t n = conversion(f + 1, kind);
        if (!n) {   // (a lone '%': a plain character)
            if (*t != '%') return false;
            f++, t++;
            continue;
        }
        f += 1 + n;
        if (kind == '%') {
            if (*t != '%') return false;
            t++;
            continue;
        }
        if (kind == 'c') {
            if (!*t) return false;
            args.emplace_back(t, 1);
            if (match_format(f, t + 1, args)) return true;
            args.pop_back();
            return false;
        }
        // (a text or a number: each length from the shortest, the rest of the format matched after it)
        for (size_t len = 1; t[len - 1]; len++) {
            if (kind == 'n' && !strchr("0123456789.-+x", t[len - 1])) break;
            args.emplace_back(t, len);
            if (match_format(f, t + len, args)) return true;
            args.pop_back();
        }
        return false;
    }
    return !*t;
}

std::string fill_format(const char* fmt, const std::vector<std::string>& args) {
    std::string out;
    size_t a = 0;
    for (const char* f = fmt; *f;) {
        char kind = 0;
        const size_t n = *f == '%' ? conversion(f + 1, kind) : 0;
        if (!n) {
            out += *f++;
            continue;
        }
        f += 1 + n;
        if (kind == '%') out += '%';
        else if (a < args.size()) out += kind == 's' ? tr_text(args[a++]) : args[a++];
    }
    return out;
}

struct Table {
    std::unordered_map<std::string_view, const char*> exact;   // (keys: the table's own texts, which live for the process)
    std::vector<const menu_i18n::Entry*> formats;   // (the entries with printf conversions)
};
/// A language's table, made at its first use. A lookup after that takes no lock and makes no string (call_once's check only)
const Table& table(int lang) {
    static std::unique_ptr<Table> t[menu_i18n::kLangCount];
    static std::once_flag made[menu_i18n::kLangCount];
    std::call_once(made[lang], [lang] {
        t[lang] = std::make_unique<Table>();
        const menu_i18n::Lang& l = menu_i18n::kLangs[lang];
        for (int i = 0; i < l.count; i++) {
            const menu_i18n::Entry& e = l.entries[i];
            t[lang]->exact.emplace(e.en, e.text);
            for (const char* p = strchr(e.en, '%'); p; p = strchr(p + 1, '%')) {
                char kind = 0;
                if (conversion(p + 1, kind) && kind != '%') {
                    t[lang]->formats.push_back(&e);
                    break;
                }
            }
        }
    });
    return *t[lang];
}

}  // namespace

namespace {
struct GameLang {
    const char* code;
    const char* name;
};
constexpr GameLang kGameLangs[] = {
    {"en-US", "English (US)"}, {"en-GB", "English (UK)"}, {"fr", "Français (France)"}, {"fr-CA", "Français (Canada)"},
    {"de", "Deutsch"}, {"it", "Italiano"}, {"es", "Español (España)"}, {"es-419", "Español (Latinoamérica)"},
    {"pt-BR", "Português (Brasil)"}, {"nl", "Nederlands"}, {"pl", "Polski"}, {"ru", "Русский"}, {"ja", "日本語"},
    {"ko", "한국어"}, {"zh-Hans", "简体中文"}, {"zh-Hant", "繁體中文"}};
constexpr int kGameLangCount = sizeof kGameLangs / sizeof kGameLangs[0];
bool same_code(const std::string& a, const char* b) {   // (as host/nn_applet.c: pt-br is pt-BR)
    const size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}
/// The game's own spelling of a code the game has, any case; "" for one it has not
std::string known_code(const std::string& code) {
    for (const GameLang& g : kGameLangs)
        if (same_code(code, g.code)) return g.code;
    return "";
}
std::string language_txt() {
    char exe[MAX_PATH];
    const DWORD n = game_module_path(exe, MAX_PATH);
    std::string path(exe, n);
    return path.substr(0, path.find_last_of("\\/") + 1) + "language.txt";
}
/// language.txt's language, as host/nn_applet.c reads it: its first line that is not blank or a comment and holds a code the
/// game has (a stray line is passed over); "" when none does
std::string read_language_txt() {
    std::string found;
    if (FILE* f = fopen(language_txt().c_str(), "r")) {
        char line[128];
        while (found.empty() && fgets(line, sizeof line, f)) {
            std::string l(line);
            if (l.compare(0, 3, "\xEF\xBB\xBF") == 0) l.erase(0, 3);   // (a BOM: Notepad's UTF-8)
            const size_t a = l.find_first_not_of(" \t");
            if (a == std::string::npos || l[a] == '#' || l[a] == '\r' || l[a] == '\n') continue;
            found = known_code(l.substr(a, l.find_first_of(" \t\r\n", a) - a));
        }
        fclose(f);
    }
    return found;
}
/// The language the game runs in, read once (host/nn_applet.c's rule): HOS_LANGUAGE over language.txt, any case, English (US)
/// when neither names one the game has. It changes only with a restart, so the menu's Auto stays on it
const std::string& game_language_started() {
    static const std::string lang = [] {
        const char* env = getenv("HOS_LANGUAGE");
        const std::string want = env ? known_code(env) : read_language_txt();
        return want.empty() ? std::string("en-US") : want;
    }();
    return lang;
}
/// language.txt's language as the Game language row last wrote it: the game's next start (the one it started in until then)
std::string& game_language_saved() {
    static std::string lang = [] {
        const std::string txt = read_language_txt();
        return txt.empty() ? game_language_started() : txt;
    }();
    return lang;
}
}  // namespace

const std::string& game_language() { return game_language_started(); }
int game_lang_count() { return kGameLangCount; }
const char* game_lang_code(int index) { return kGameLangs[std::clamp(index, 0, kGameLangCount - 1)].code; }
const char* game_lang_name(int index) { return kGameLangs[std::clamp(index, 0, kGameLangCount - 1)].name; }
int game_lang_index() {
    for (int i = 0; i < kGameLangCount; i++)
        if (same_code(game_language_saved(), kGameLangs[i].code)) return i;
    return 0;
}
bool set_game_language(int index) {
    const char* code = game_lang_code(index);
    std::vector<std::string> lines;
    if (FILE* f = fopen(language_txt().c_str(), "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) lines.emplace_back(line);
        fclose(f);
    }
    bool set = false;
    for (auto& l : lines) {
        const size_t a = l.find_first_not_of(" \t\r\n");
        if (set || a == std::string::npos || l[a] == '#') continue;
        l = std::string(code) + "\n", set = true;
    }
    if (!set) lines.push_back(std::string(code) + "\n");
    // (written whole or not at all: a folder that cannot be written keeps language.txt, and the row goes back to it)
    FILE* f = fopen(language_txt().c_str(), "w");
    if (!f) return false;
    bool ok = true;
    for (const auto& l : lines) ok = fputs(l.c_str(), f) >= 0 && ok;
    ok = fclose(f) == 0 && ok;
    if (ok) game_language_saved() = code;
    return ok;
}

int lang_now() {
    if (opts.menu_lang > 0) return std::clamp(opts.menu_lang - 1, 0, menu_i18n::kLangCount - 1);
    // (Auto: the language the game runs in (not the Game language row's next one), its code, else its first part (fr-CA: fr,
    // en-GB: en), else English)
    static const int started = [] {
        const std::string& code = game_language();
        for (int i = 0; i < menu_i18n::kLangCount; i++)
            if (same_code(code, menu_i18n::kLangs[i].code)) return i;
        const std::string first = code.substr(0, code.find('-'));
        for (int i = 0; i < menu_i18n::kLangCount; i++)
            if (same_code(first, menu_i18n::kLangs[i].code)) return i;
        return 0;
    }();
    return started;
}
const char* lang_code() { return menu_i18n::kLangs[lang_now()].code; }
int plural(int n) {
    const std::string code = lang_code();
    const int ten = n % 10, hundred = n % 100;
    const bool few = ten >= 2 && ten <= 4 && !(hundred >= 12 && hundred <= 14);
    if (code == "ru") return ten == 1 && hundred != 11 ? 0 : few ? 1 : 2;   // (1, 21: параметр; 2-4: параметра; 5, 11: параметров)
    if (code == "pl") return n == 1 ? 0 : few ? 1 : 2;                      // (1: ustawienie; 2-4, 22: ustawienia; 5, 12: ustawień)
    return n == 1 ? 0 : 1;
}
int lang_count() { return menu_i18n::kLangCount; }
const char* lang_name(int index) { return menu_i18n::kLangs[std::clamp(index, 0, menu_i18n::kLangCount - 1)].name; }

std::string tr_text(const std::string& s) {
    const int lang = lang_now();
    if (s.empty() || !lang) return s;
    const Table& t = table(lang);
    if (auto it = t.exact.find(s); it != t.exact.end()) return it->second;
    for (const menu_i18n::Entry* e : t.formats) {
        std::vector<std::string> args;
        if (match_format(e->en, s.c_str(), args)) return fill_format(e->text, args);
    }
    return s;
}

const char* tr(const char* s) {
    const int lang = lang_now();
    if (!s || !*s || !lang) return s;
    const Table& t = table(lang);
    if (auto it = t.exact.find(s); it != t.exact.end()) return it->second;
    if (t.formats.empty()) return s;
    // (a text made from a format: put together once, kept. Two generations: when the newer one passes 4096 texts it becomes
    // the older and the older is let go, so a pointer handed out lives through 4096 more texts at least, a frame's many times)
    static std::unordered_map<std::string, std::string> gen[2];
    static int newer = 0, made_for = -1;
    static std::mutex mu;
    std::lock_guard lk(mu);
    if (made_for != lang) gen[0].clear(), gen[1].clear(), made_for = lang;
    auto it = gen[newer].find(s);
    if (it == gen[newer].end()) {
        auto old = gen[newer ^ 1].find(s);
        std::string text = old != gen[newer ^ 1].end() ? old->second : tr_text(s);
        if (gen[newer].size() >= 4096) newer ^= 1, gen[newer].clear();
        it = gen[newer].emplace(s, std::move(text)).first;
    }
    return it->second == s ? s : it->second.c_str();
}

}  // namespace mkwvk::i18n
