/* nn::oe (applet), nn::account, nn::time, nn::pl, nn::err: what a game asks the system about itself.
   The answers describe a docked console in English (HOS_LANGUAGE / language.txt: another language) with one user,
   "Player". */
#include "hos.h"
#include "hos_paths.h"
#include "hos_wide_size.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>   /* (the exe's folder; the browser build: the Microsoft C runtime's time functions) */

static int64_t boot_ns;

/* ---- nn::oe ------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn2oe10InitializeEv) { if (!boot_ns) boot_ns = hos_now_ns(); }
HLE(_ZN2nn2oe17FinishStartupLogoEv) { hos_log("oe: startup logo finished"); }
/* GetDesiredLanguage() -> settings::LanguageCode (char[8]) by value: the language the game shows. HOS_LANGUAGE, or language.txt
   beside the exe: its first line that is not blank or a comment. The file is written on the first start, with the choices: the 16
   languages in romfs/Mals. After SkyboundTotk PR 16 by sardigital. */
static const struct { const char *code, *name; } languages[] = {
    {"en-US", "English (US)"},       {"en-GB", "English (UK)"},       {"fr", "French (France)"},     {"fr-CA", "French (Canada)"},
    {"de", "German"},                {"it", "Italian"},               {"es", "Spanish (Spain)"},     {"es-419", "Spanish (Latin America)"},
    {"pt-BR", "Portuguese (Brazil)"}, {"nl", "Dutch"},                {"pl", "Polish"},              {"ru", "Russian"},
    {"ja", "Japanese"},              {"ko", "Korean"},                {"zh-Hans", "Chinese (Simplified)"}, {"zh-Hant", "Chinese (Traditional)"},
};
static const char *desired_language(void) {
    static char lang[8];
    if (lang[0]) return lang;
    char want[32] = "";
    const char *from = "the default";
    if (getenv("HOS_LANGUAGE")) {
        snprintf(want, sizeof want, "%s", getenv("HOS_LANGUAGE"));
        from = "HOS_LANGUAGE";
    } else {
        char path[300] = "language.txt";
#if !defined(__EMSCRIPTEN__)   /* (linux too: host/posix) */
        DWORD n = hos_game_path_a(path, 260);
        while (n > 0 && path[n - 1] != 92 && path[n - 1] != 47) n--;
        snprintf(path + n, sizeof path - n, "language.txt");
#endif
        FILE *f = fopen(path, "r");
        if (f) {
            char line[80];
            int whole = 1;   /* (the first line is longer than this: its second piece, "ave, and start...", was taken for the code) */
            while (fgets(line, sizeof line, f)) {
                const int starts = whole;
                whole = strchr(line, 10) != NULL;
                if (!starts) continue;
                const char *p = line;
                if ((unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;   /* (a BOM: Notepad's UTF-8) */
                while (*p == ' ' || *p == '\t') p++;
                if (*p == '#' || *p == '\r' || *p == '\n' || !*p) continue;
                /* (only a code the game has: a stray line was taken before, and the game stayed in English, Hookshot issue 75) */
                char code[32];
                snprintf(code, sizeof code, "%s", p);
                code[strcspn(code, " \t\r\n")] = 0;
                int known = 0;
                for (size_t i = 0; i < sizeof languages / sizeof *languages; i++)
                    if (!_stricmp(code, languages[i].code)) snprintf(code, sizeof code, "%s", languages[i].code), known = 1;
                if (!known) continue;
                snprintf(want, sizeof want, "%s", code);
                from = "language.txt";
                break;
            }
            fclose(f);
        } else if ((f = fopen(path, "w")) != NULL) {
            fputs("# The game's language. Change the line below to one of the codes listed here, save, and start the game again.\n\nen-US\n\n", f);
            for (size_t i = 0; i < sizeof languages / sizeof *languages; i++) fprintf(f, "# %-8s %s\n", languages[i].code, languages[i].name);
            fclose(f);
        }
    }
    want[strcspn(want, " \t\r\n")] = 0;
    snprintf(lang, sizeof lang, "en-US");
    int found = 0;
    for (size_t i = 0; want[0] && i < sizeof languages / sizeof *languages; i++)
        if (!_stricmp(want, languages[i].code)) snprintf(lang, sizeof lang, "%s", languages[i].code), found = 1;   /* (HOS_LANGUAGE=pt-br too, as language.txt) */
    if (want[0] && !found) hos_log("oe: language \"%s\" (%s) is not one the game has: English (US)", want, from);
    else hos_log("oe: language %s (%s)", lang, from);
    return lang;
}
HLE(_ZN2nn2oe18GetDesiredLanguageEv) {
    uint64_t v = 0;
    memcpy(&v, desired_language(), strlen(desired_language()));
    RET(v);
}
/* HOS_HANDHELD=1: handheld (the 1080p screen: 1080p output and the game's handheld resolutions) instead of docked
   (MKW docked composes 3840 x 2160) */
int hos_ini_option(const char *key, int otherwise);
#define ini_option hos_ini_option
/* (hookshot) An option's text in hookshot_options.ini, the game folder's (the Insert menu's choices, gpu/vk/vk_menu.cpp), the last
   line that sets it, its line end cut: whether there was one */
static int ini_text(const char *key, char *out, size_t size) {
    char path[MAX_PATH];
    const DWORD n = hos_game_path_a(path, MAX_PATH);
    char *slash = n ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/'; the game folder's, not bin\\'s) */
    if (!slash || !size) return 0;
    snprintf(slash + 1, (size_t)(path + MAX_PATH - slash - 1), "hookshot_options.ini");
    FILE *f = fopen(path, "r");
    if (!f) return 0;
    char line[80];
    const size_t len = strlen(key);
    int found = 0;
    while (fgets(line, sizeof line, f))
        if (!strncmp(line, key, len) && line[len] == '=') {
            snprintf(out, size, "%s", line + len + 1);
            out[strcspn(out, "\r\n")] = 0;
            found = 1;
        }
    fclose(f);
    return found;
}
int hos_ini_option(const char *key, int otherwise) {   /* (hookshot: the Insert menu's choice, gpu/vk/vk_menu.cpp; nn_vi.c reads it too) */
    char text[80];
    return ini_text(key, text, sizeof text) ? atoi(text) : otherwise;
}
static int handheld(void) {
    static int v = -1;
    if (v < 0) v = getenv("HOS_HANDHELD") ? atoi(getenv("HOS_HANDHELD")) != 0 : ini_option("handheld_profile", 0) != 0;
    return v;
}
/* (hookshot) The Insert menu's Output resolution (output_res=, HOS_OUTPUT_RES over it), docked only: a size, "<width>x<height>",
   each side a multiple of 8 (rounded down to one), at least 640 x 360, at most 5120 x 2880's pixels (the largest the render
   targets' pool, hos_rt_pool_size, was played at) and between 5:4 and 48:9 in shape (host/hos_main.c's screen shapes); anything
   else is the game's own size, said in the log. The menu offers 2048x1152 and the screen's display modes (gpu/vk/display_modes.h;
   where they are not known 1920x1080, 2560x1440, 3840x2160, 5120x1440 and 5120x2880), and any other size as its own choice. The numbers it saved before are read as their sizes: 0 the game's
   2048 x 1152, 1 2560 x 1440, 2 3840 x 2160, 3 1920 x 1080, 4 5120 x 1440, 5 5120 x 2880. The game's frame is its display size
   (2560 x 1440 as shipped; nx2/oot_patches.py apply_output_size asks hos_output_size) times its graphics scale
   (GetCurrentGraphicsScale: 0 x1, 1 x0.8, 2 x0.667). Here it saw scale 1 (x0 left at the call before's answer), so 2048 x 1152,
   the game's own, which 2048x1152 asks for; each other size uses x1. */
int hos_output_parse(const char *text, uint32_t *w, uint32_t *h) {   /* (an explicit size: 1, and *w x *h; the game's own: 0) */
    static const uint32_t old[][2] = {{0, 0}, {2560, 1440}, {3840, 2160}, {1920, 1080}, {5120, 1440}, {5120, 2880}};
    unsigned a = 0, b = 0;
    *w = *h = 0;
    if (!text) return 0;
    if (sscanf(text, "%u%*[xX]%u", &a, &b) == 2) {
        a &= ~7u, b &= ~7u;
        if (!hos_output_size_valid(a, b)) {
            hos_log("oe: output_res=%s is not a size the game can use (640x360 to 5120x2880's pixels, 5:4 to 48:9): the game's own", text);
            return 0;
        }
        if (a == 2048 && b == 1152) return 0;   /* (the game's own) */
        *w = a, *h = b;
        return 1;
    }
    const int i = atoi(text);
    if (i < 1 || i >= (int)(sizeof old / sizeof *old)) {
        if (*text && strcmp(text, "0")) hos_log("oe: output_res=%s is not a size or one of the numbers saved before: the game's own", text);
        return 0;
    }
    *w = old[i][0], *h = old[i][1];
    return 1;
}
static uint32_t output_w, output_h;   /* (the explicit size; 0: the game's own) */
static int output_mode(void) {   /* (an explicit size: 1; the game's own: 0) */
    static int read;
    if (!read) {
        read = 1;
        char text[80] = "";
        const char *e = getenv("HOS_OUTPUT_RES");
        if (!handheld() && (e || ini_text("output_res", text, sizeof text))) hos_output_parse(e ? e : text, &output_w, &output_h);
        if (output_w) hos_log("oe: output resolution %ux%u", output_w, output_h);
        else hos_log("oe: output resolution 2048x1152 (the game's)");
    }
    return output_w != 0;
}
uint32_t hos_output_size(int height) {
    if (!output_mode()) return height ? 1440 : 2560;
    return height ? output_h : output_w;
}
/* (hookshot) The render targets' GPU pool (nx2/oot_patches.py), 0x13100000 bytes as shipped, for 2560 x 1440: a 3840x2160 3D at
   the 4K output ran out of it. Scaled with the output's pixels, or the widescreen picture's when it has more (host/hos_main.c:
   hos_wide_out, the output's height at the screen's shape: 7680 x 2160 for a 32:9 screen at the 4K output, twice the pixels the pool
   was scaled for, and the game crashed as Kokiri Forest loaded, a null pointer or "texture handle ids -1", issue 1031);
   Castle Town's guard-house conversation/camera test crashed with 305 MB at 2560 x 1440 and passed with 512 MB.
   Keep at least 512 MB by default; larger scaled pools and an explicit HOS_RT_POOL_MB=<MB> still win. An output with more pixels
   than 3840 x 2160 (a size of the player's own, up to 5120 x 2880's pixels: hos_output_parse) keeps its pool to 900 MB:
   5120 x 2880 scales to 1220 MB, and 1220 and 1000 MB both failed the game's own reservation at boot; at 900 MB it boots and
   plays with the 3D at the full size. */
uint64_t hos_rt_pool_size(uint64_t game) {
    const char *e = getenv("HOS_RT_POOL_MB");
    extern volatile uint64_t hos_wide_out;
    const uint64_t wide = hos_wide_out, wide_px = (wide >> 32) * (wide & 0xffffffffULL);
    uint64_t px = (uint64_t)hos_output_size(0) * hos_output_size(1);
    if (wide_px > px) px = wide_px;
    const uint64_t scaled = px > 2560 * 1440 ? (game * px / (2560 * 1440) + 0xfffff) & ~0xfffffULL : game;
    const uint64_t minimum = 512ULL << 20, most = 900ULL << 20;
    const int big = (uint64_t)hos_output_size(0) * hos_output_size(1) > 3840ULL * 2160;
    const uint64_t v = e ? strtoull(e, NULL, 0) << 20 : scaled < minimum ? minimum : big && scaled > most ? most : scaled;
    static int logged;
    if (!logged++) hos_log("applet: render target pool %llu MB (the game's %llu MB)", (unsigned long long)(v >> 20), (unsigned long long)(game >> 20));
    return v;
}
HLE(_ZN2nn2oe16GetOperationModeEv) { RET(handheld() ? 0 : 1); }     /* OperationMode_Handheld / _Console (docked) */
HLE(_ZN2nn2oe18GetPerformanceModeEv) { RET(handheld() ? 0 : 1); }   /* PerformanceMode_Normal / _Boost (docked) */
HLE(_ZN2nn2oe20GetCurrentFocusStateEv) { RET(1); }    /* FocusState_InFocus */
HLE(_ZN2nn2oe25GetProgramTotalActiveTimeEv) { RET(hos_now_ns() - boot_ns); }
HLE(_ZN2nn2oe24GetExpectedVolumeBalanceEPfS1_) {
    float one = 1.0f;
    memcpy(GP(ARG(0)), &one, 4);
    memcpy(GP(ARG(1)), &one, 4);
}
HLE(_ZN2nn2oe24SetExpectedVolumeBalanceEff) {}
HLE(_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE) {}
HLE(_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE) {}
HLE(_ZN2nn2oe27SetPerformanceConfigurationENS0_15PerformanceModeEi) {}
HLE(_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb) {}
HLE(_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb) {}
HLE(_ZN2nn2oe28SetResumeNotificationEnabledEb) {}
HLE(_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb) {}
/* No system notifications ever arrive: TryPop says none, Pop waits forever (as it would on a quiet console). */
HLE(_ZN2nn2oe25TryPopNotificationMessageEPj) { RET(0); }
HLE(_ZN2nn2oe22PopNotificationMessageEv) { for (;;) hos_sleep_ns(1000000000LL); }

/* ---- nn::account: one user ------------------------------------------------------------------------------------ */
static const uint64_t user_uid[2] = {0x0000000000000001ULL, 0x5a3c9e1f0b7d2468ULL};
HLE(_ZN2nn7account10InitializeEv) {}
/* TryOpenPreselectedUser(UserHandle*) -> bool. A handle is three words; ours carries the uid. */
HLE(_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE) {
    G64(ARG(0)) = user_uid[0];
    G64(ARG(0) + 8) = user_uid[1];
    G64(ARG(0) + 16) = 1;
    RET(1);
}
HLE(_ZN2nn7account9CloseUserERKNS0_10UserHandleE) {}
/* GetUserId(Uid*, UserHandle const&) -> Result */
HLE(_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE) {
    G64(ARG(0)) = user_uid[0];
    G64(ARG(0) + 8) = user_uid[1];
    RET(0);
}
/* GetNickname(Nickname*, Uid const&) -> Result: Nickname is char[33] */
HLE(_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE) {
    memset(GP(ARG(0)), 0, 33);
    memcpy(GP(ARG(0)), "Player", 6);
    RET(0);
}
HLE(_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE) {
    *(uint8_t *)GP(ARG(0)) = G64(ARG(1)) == user_uid[0] && G64(ARG(1) + 8) == user_uid[1];
    RET(0);
}

/* ---- nn::time: the host clock --------------------------------------------------------------------------------- */
HLE(_ZN2nn4time10InitializeEv) { RET(0); }
HLE(_ZN2nn4time8FinalizeEv) { RET(0); }
HLE(_ZN2nn4time13IsInitializedEv) { RET(1); }
/* HOS_FIXED_TIME=<POSIX seconds>: the clock starts there at boot (runs that can be compared: what the title shows
   follows the time) */
static uint64_t now_posix(void) {
    static int64_t fixed = -1;
    if (fixed < 0) fixed = getenv("HOS_FIXED_TIME") ? _atoi64(getenv("HOS_FIXED_TIME")) : 0;
    return fixed ? (uint64_t)(fixed + hos_now_ns() / 1000000000LL) : (uint64_t)time(NULL);
}
HLE(_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { G64(ARG(0)) = now_posix(); RET(0); }
HLE(_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE) { G64(ARG(0)) = now_posix(); RET(0); }
/* StandardSteadyClock::now() -> time_point (std::chrono, rep int64, period seconds): one register, x0.
   (hookshot) It was a 24-byte SteadyClockTimePoint written through x8; OoT reads x0 and leaves x8 unset (it was 0:
   the write faulted at guest 0). MKW does not import it. */
HLE(_ZN2nn4time19StandardSteadyClock3nowEv) { RET((uint64_t)(hos_now_ns() / 1000000000LL)); }

/* (hookshot) nn::socket::detail::Poll(pollfd* fds, unsigned long n, int timeout_ms): offline, no descriptor is ever
   ready. OoT's gRPC poll engine (nn.npln.GrpcDispatcher) calls it through a function pointer, past the socket
   imports the runtime stubs, and the SDK's own Poll aborted on the uninitialized socket layer. It waits out the
   timeout (an infinite one in 50 ms steps, which the poll loop takes as a spurious wake) and reports a timeout. */
static void socket_poll(Ctx *c) {
    const uint64_t fds = ARG(0), n = ARG(1);
    const int timeout = (int)ARG(2);
    for (uint64_t i = 0; fds && i < n && i < 4096; i++) *(uint16_t *)GP(fds + 8 * i + 6) = 0;   /* revents */
    if (timeout != 0) {
        const int away = hos_vcore_away();
        hos_sleep_ns((int64_t)(timeout < 0 || timeout > 50 ? 50 : timeout) * 1000000);
        if (away) hos_vcore_back();
    }
    RET(0);
}
HLE(_ZN2nn6socket6detail4PollEP6pollfdmi) { socket_poll(c); }
HLE(_ZN2nn6socket6detail4PollEPNS0_6PollFdEmi) { socket_poll(c); }

/* (hookshot) nn::account::EnsureNetworkServiceAccountIdTokenCacheAsync(AsyncContext*, UserHandle const&): offline,
   so no network service account. The generic stub's 0 said the task started, and OoT's npln worker (main
   0x8214eac0) then waited on the AsyncContext nothing had set up. This failure (account module 124, description
   280) takes the game's error branch right after the call. */
HLE(_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE) {
    RET(124 | (280 << 9));
}

/* (hookshot) nn::nsd::GetEnvironmentIdentifier(EnvironmentIdentifier* out): char[8]; retail units say "lp1".
   OoT (main 0x8243f490) only checks for a "t-" test prefix. Left to the SDK it asks the nsd service over HIPC. */
HLE(_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE) {
    if (ARG(0)) { memset(GP(ARG(0)), 0, 8); memcpy(GP(ARG(0)), "lp1", 3); }
}

static void calendar(uint64_t out, uint64_t info, int64_t posix, int local) {
    time_t t = (time_t)posix;
    struct tm tmv = {0};   /* (hookshot: a time the C library cannot convert left it as the stack had it) */
    if (local) localtime_s(&tmv, &t); else gmtime_s(&tmv, &t);
    /* CalendarTime { s16 year; s8 month, day, hour, minute, second; pad } */
    uint8_t ct[8] = {0};
    int16_t year = (int16_t)(tmv.tm_year + 1900);
    memcpy(ct, &year, 2);
    ct[2] = (uint8_t)(tmv.tm_mon + 1); ct[3] = (uint8_t)tmv.tm_mday; ct[4] = (uint8_t)tmv.tm_hour;
    ct[5] = (uint8_t)tmv.tm_min; ct[6] = (uint8_t)tmv.tm_sec;
    memcpy(GP(out), ct, 8);
    if (info) {   /* CalendarAdditionalInfo { s32 dayOfWeek, dayOfYear; char tz[8]; s32 isDst; s32 utcOffset } */
        memset(GP(info), 0, 0x18);
        G32(info) = (uint32_t)tmv.tm_wday;
        G32(info + 4) = (uint32_t)tmv.tm_yday;
        memcpy(GP(info + 8), "UTC", 3);
    }
}
/* ToCalendarTime(CalendarTime*, CalendarAdditionalInfo*, PosixTime const&) -> Result */
HLE(_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE) {
    calendar(ARG(0), ARG(1), (int64_t)G64(ARG(2)), 1);
    RET(0);
}
/* ToCalendarTimeInUtc(PosixTime const&) -> CalendarTime (8 bytes, in x0) */
HLE(_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE) {
    static _Thread_local uint64_t tmp;
    if (!tmp) tmp = hos_sys_alloc(8, 8);
    calendar(tmp, 0, (int64_t)G64(ARG(0)), 0);
    RET(G64(tmp));
}
/* ToPosixTimeFromUtc(CalendarTime const&) -> PosixTime */
HLE(_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE) {
    uint8_t ct[8];
    memcpy(ct, GP(ARG(0)), 8);
    struct tm tmv = {0};
    int16_t year;
    memcpy(&year, ct, 2);
    tmv.tm_year = year - 1900; tmv.tm_mon = ct[2] - 1; tmv.tm_mday = ct[3];
    tmv.tm_hour = ct[4]; tmv.tm_min = ct[5]; tmv.tm_sec = ct[6];
    RET((uint64_t)_mkgmtime(&tmv));
}
/* ClockSnapshot: an opaque 0xD0-byte record; ours holds the POSIX time at +0 */
HLE(_ZN2nn4time13ClockSnapshotC1Ev) { memset(GP(ARG(0)), 0, 0xD0); }
HLE(_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_) {
    memset(GP(ARG(0)), 0, 0xD0);
    G64(ARG(0)) = now_posix();
    RET(0);
}
/* CalculateSpanBetween(TimeSpan*, ClockSnapshot const& a, ClockSnapshot const& b) -> Result */
HLE(_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_) {
    G64(ARG(0)) = (uint64_t)(((int64_t)G64(ARG(2)) - (int64_t)G64(ARG(1))) * 1000000000LL);
    RET(0);
}
HLE(_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_) { RET(0); }

/* ---- nn::pl: system shared fonts. We have none; report an empty one, loaded. ------------------------------------ */
static uint64_t empty_font;
HLE(_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE) { RET(1); }   /* SharedFontLoadState_Loaded */
HLE(_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE) {
    if (!empty_font) empty_font = hos_sys_alloc(0x1000, 0x1000);
    RET(empty_font);
}
HLE(_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE) { RET(0); }

/* ---- nn::err ------------------------------------------------------------------------------------------------- */
HLE(_ZN2nn3err9ShowErrorENS_6ResultE) { hos_log("err: the game shows error %#x (module %u, description %u)", (uint32_t)ARG(0),
                                                (uint32_t)ARG(0) & 0x1FF, ((uint32_t)ARG(0) >> 9) & 0x1FFF); }

/* ---- nn::prepo: play reports. The PlayReport builder (Add*, SetEventId) is the SDK's own code; saving one would
   send it to the prepo service. Reports are dropped. ---------------------------------------------------------- */
HLE(_ZN2nn5prepo10PlayReport4SaveEv) { RET(0); }
HLE(_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE) { RET(0); }
/* (hookshot) PlayReport::Send(Uid const&), SDK 22's: kept nowhere, as Save. Left to the SDK it opened the prepo
   service through sm: and aborted on the empty reply (OoT's PlayReporter thread, ~5 minutes into a game). */
HLE(_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE) { RET(0); }
/* (hookshot) prepo::AsyncContext: the report "sent" at once. GetCompletionEvent(SystemEvent* out) gives an event
   already signaled (the SDK's own aborted on the context the empty IPC reply left), GetResult success. */
void hos_system_event_init(uint64_t key, int autoclear);   /* nn_os_sync.c */
void hos_system_event_signal(uint64_t key);
HLE(_ZN2nn5prepo12AsyncContext18GetCompletionEventEPNS_2os11SystemEventE) {
    if (ARG(1)) {
        hos_system_event_init(ARG(1), 0);
        hos_system_event_signal(ARG(1));
    }
    RET(0);
}
HLE(_ZN2nn5prepo12AsyncContext9GetResultEv) { RET(0); }
HLE(_ZN2nn5prepo12AsyncContextD1Ev) {}

/* ---- (mkw-recomp) MKW's account and oe calls TOTK never made: the one user, offline ------------------------- */
/* ListAllUsers(int* count, Uid* out, int max) -> Result */
HLE(_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi) {
    const int n = (int)ARG(2) > 0 ? 1 : 0;
    if (n && ARG(1)) {
        G64(ARG(1)) = user_uid[0];
        G64(ARG(1) + 8) = user_uid[1];
    }
    if (ARG(0)) G32(ARG(0)) = (uint32_t)n;
    RET(0);
}
/* OpenUser(UserHandle*, Uid const&) -> Result */
HLE(_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE) {
    G64(ARG(0)) = G64(ARG(1));
    G64(ARG(0) + 8) = G64(ARG(1) + 8);
    G64(ARG(0) + 16) = 0;
    RET(0);
}
/* the network service account: not available (the game stays offline) */
HLE(_ZN2nn7account32IsNetworkServiceAccountAvailableEPbRKNS0_10UserHandleE) {
    *(uint8_t *)GP(ARG(0)) = 0;
    RET(0);
}
HLE(_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE) { RET(0x7C7C); }   /* (an account error) */
HLE(_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE) {
    G64(ARG(0)) = 0;
    RET(0x7C7C);
}
/* nn::oe::GetCurrentGraphicsScale() -> float (docked/handheld render scale): 1.0 */
HLE(_ZN2nn2oe23GetCurrentGraphicsScaleEv) {
    /* (hookshot) HOS_GRAPHICS_SCALE=<f>: another scale, a test of whether the game sizes its picture by it (4K, issue 68) */
    static float scale = 0;
    static volatile LONG told;
    if (scale == 0) scale = getenv("HOS_GRAPHICS_SCALE") ? (float)atof(getenv("HOS_GRAPHICS_SCALE")) : 1.0f;
    if (scale <= 0) scale = 1.0f;
    if (InterlockedIncrement(&told) <= 3) hos_log("oe: GetCurrentGraphicsScale -> %.2f", scale);
    const float one = scale;
    uint32_t bits;
    memcpy(&bits, &one, 4);
    c->vreg[0][0] = bits, c->vreg[0][1] = 0;
    c->x[0] = output_mode() ? 0 : 1;   /* (the scale enum the game reads, w0: x1 for a chosen output, x0.8 as before) */
}
HLE(_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb) {}
HLE(_ZN2nn2oe31EnterExitRequestHandlingSectionEv) {}
HLE(_ZN2nn2oe31LeaveExitRequestHandlingSectionEv) {}
HLE(_ZN2nn2oe15EnableRecordingEv) {}
HLE(_ZN2nn2oe16DisableRecordingEv) {}
HLE(_ZN2nn2oe16EnableScreenShotEv) {}
HLE(_ZN2nn2oe17DisableScreenShotEv) {}
/* GetDisplayVersion(DisplayVersion*): a 16-byte string */
HLE(_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE) {
    memset(GP(ARG(0)), 0, 16);
    memcpy(GP(ARG(0)), "1.8.0", 5);
}
/* nn::camera::sdk::CreateCameraContext(gsdrv::UniqueNativeHandle*) -> Result: no camera (GameChat's video), so the
   game does not open the camera service through a handle it never got */
HLE(_ZN2nn6camera3sdk19CreateCameraContextEPNS_5gsdrv18UniqueNativeHandleE) {
    /* the game aborts unless this succeeds: a session handle that hos_svc.c's SendSyncRequest answers generically */
    if (ARG(0)) G32(ARG(0)) = 0xC0FE0001u;
    RET(0);
}
/* StandardNetworkSystemClock::IsAccuracySufficient(): offline, so no (the lifted SDK version aborted, with no time
   service session) */
HLE(_ZN2nn4time26StandardNetworkSystemClock20IsAccuracySufficientEv) { RET(0); }
