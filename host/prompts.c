/* (hookshot) The button prompts of the controller in use: the game's button-icon font (Font2/Scalable/External.*.bfotf,
   every icon in a text: dialogue, the control hints, menus) served from prompts/External.bfotf in the game folder, which
   nx2/prompt_assets.py draws with every controller face's icons at build time, with each Switch button's icon pointed at
   the face's icon of the pad input bound to it (host/nn_hid.c's controls, the menu's Layout preset and Controls page). So
   the icons say which button to press on this pad, custom controls too.

   The face: hookshot_options.ini's button_prompts (the menu's Button prompts row): 0 Auto, from the first controller's name
   (SDL; with none, and none through XInput, the keyboard's), else 1 Switch, 2 Xbox, 3 PlayStation 4, 4 PlayStation 5, 5 Steam
   Deck, 6 Keyboard. The keyboard face (nx2/prompt_keys.py) points each Switch button at the icon of the first key bound to it,
   the sticks at the keys that move (WASD) and the mouse that turns the camera, and keeps the Switch's textures and the Talk
   ring's places. The game reads the font once, at its start; a new face or new controls are written where it keeps it, and its
   texts drawn again (the live part, below).

   The font as built: an OpenType font behind an 8-byte header, XORed per little-endian word with the key the header's magic
   (0x18029a7f) gives. Its cmap has, for each face, one icon per pad input at U+F700 + 32 x HOS_PROMPT_* + HOS_PAD_* and the
   face's d-pad icons at U+F700 + 32 x HOS_PROMPT_* + 24.., and each Switch button's and d-pad icon's code point in a format 4
   segment of its own, by idDelta: pointing one at another icon is that segment's idDelta. The result goes out
   as a zstd frame of raw blocks (the file name says .zs; the game's decoder takes a frame made without its dictionary).

   The font is larger than the game's, and the game drops a file larger than its entry in the resource size table: the
   game's table is served with the font's entry raised (prompts/rsizetable.zs, nx2/prompt_assets.py), and a mod's own table with
   our entries merged into it (sizes), so a mod that grows files keeps the icons. */
#include "hos.h"
#include "hos_paths.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "../gpu/vk/vk_api.h"   /* (HOS_SW_*, HOS_PAD_*, HOS_BIND_PAD) */
#include "zstd.h"   /* (third_party/zstd: a mod's size table) */

int hos_binding(int set, int button, int slot);   /* nn_hid.c */
int hos_sdl_pads(uint64_t *buttons, int32_t stick[4]);   /* pad_sdl.c */
void hos_sdl_pad_first_name(char *out, size_t cap);
int hos_mods_file(const char *rel, char *out, size_t cap);   /* mods.c */
extern const char *hos_romfs_dir;   /* nn_fs.c: the game's RomFS folder */
int hos_xinput_pads(void);   /* nn_hid.c */
int rx_option(int id, int otherwise);   /* rx_backend.c: the Insert menu's rows */

#define FONT_PATH "font2/scalable/external.nin_ounce_nvn2.bfotf.zs"   /* (as hos_mods_romfs's keys: lower case) */
#define TEXTURES_PATH "ui/layout2archive/common.product.100.nin_ounce_nvn2.bntx.zs"
#define LAYOUTS_PATH "ui/layout2archive/common.product.100.nin_ounce_nvn2.blarc.zs"
#define SIZES_PATH "system/resource/resourcesizetable."   /* (then the version, ".rsizetable.zs") */
#define MAGIC 0x18029a7fu
#define PAD_BASE 0xF700
#define FACE_CODES 32   /* (each face's code points: its pad icons, then at STATIC_AT its d-pad icons; nx2/prompt_assets.py) */
#define STATIC_AT 24
/* the keyboard face (nx2/prompt_keys.py): each key's icon at KEY_BASE + its key code; a text's code for a Switch button on a key,
   given out as they come (key_slot); the sticks' icons in a text, the keyboard's and the game's own (stick_code's order) */
#define KEY_BASE (PAD_BASE + FACE_CODES * HOS_PROMPT_N)
#define SLOT_BASE 0xEEC0
#define SLOTS 320
#define STICK_KEYS 0xF8C0
#define STICK_GAME 0xF8D0
#define BINDS (2 * HOS_SW_N)   /* (what the prompts follow: each Switch button's pad input, then its first key: current_binds) */
static const char *const faces[HOS_PROMPT_N] = {"switch", "xbox", "ps4", "ps5", "steamdeck", "keyboard"};
/* the Switch buttons' code points in the font, in HOS_SW_* order (nx2/prompt_assets.py SWITCH) */
static const uint16_t switch_code[HOS_SW_N] = {0xE040, 0xE041, 0xE042, 0xE043, 0xE056, 0xE057, 0xE044, 0xE045,
                                               0xE046, 0xE047, 0xE04B, 0xE04C, 0xE062, 0xE060, 0xE063, 0xE061};
/* the pad input whose icon an unbound Switch button shows (nx2/prompt_assets.py DEFAULT: host/nn_hid.c's default controls) */
static const unsigned char switch_pad[HOS_SW_N] = {
    HOS_PAD_EAST, HOS_PAD_SOUTH, HOS_PAD_NORTH, HOS_PAD_WEST, HOS_PAD_LSTICK, HOS_PAD_RSTICK, HOS_PAD_LSHOULDER,
    HOS_PAD_RSHOULDER, HOS_PAD_LTRIGGER, HOS_PAD_RTRIGGER, HOS_PAD_START, HOS_PAD_BACK, HOS_PAD_LEFT, HOS_PAD_UP,
    HOS_PAD_RIGHT, HOS_PAD_DOWN};
/* the d-pad icons that follow the face, not the controls (nx2/prompt_assets.py STATIC) */
static const uint16_t static_code[3] = {0xE049, 0xE064, 0xE065};
/* the sticks' icons the keyboard face shows as keys (nx2/prompt_keys.py STICKS): L down, up, left-right, right, left, up-down,
   any way; R the same; R's four ways */
static const uint16_t stick_code[15] = {0xE050, 0xE052, 0xE054, 0xE058, 0xE05A, 0xE05C, 0xE05E, 0xE051, 0xE053, 0xE055,
                                        0xE059, 0xE05B, 0xE05D, 0xE05F, 0xE068};

static int game_dir(char *out, size_t cap) {   /* the game folder, with its separator */
    char path[MAX_PATH];
    const DWORD len = hos_game_path_a(path, MAX_PATH);
    char *slash = len ? (strrchr(path, 92) ? strrchr(path, 92) : strrchr(path, 47)) : NULL;   /* (linux: '/') */
    if (!slash) return 0;
    slash[1] = 0;
    snprintf(out, cap, "%s", path);
    return 1;
}

static int contains(const char *text, const char *word) {   /* (any case) */
    for (; *text; text++) {
        size_t i = 0;
        while (word[i] && tolower((unsigned char)text[i]) == word[i]) i++;
        if (!word[i]) return 1;
    }
    return 0;
}

/* the face of a controller by its name (SDL's), HOS_PROMPT_*: "" no controller, the Switch's (the keyboard, the game's own
   icons; or XInput without SDL) */
static int face_of_name(const char *name) {
    if (!name[0]) return HOS_PROMPT_SWITCH;
    if (contains(name, "dualsense") || contains(name, "ps5")) return HOS_PROMPT_PS5;
    if (contains(name, "dualshock") || contains(name, "ps4") || contains(name, "playstation")) return HOS_PROMPT_PS4;
    /* (a Steam Controller names its buttons as the Deck does: L1/R1, View, Menu, Steam. SDL3 calls it "Steam Controller") */
    if (contains(name, "steam deck") || contains(name, "steam controller")) return HOS_PROMPT_STEAMDECK;
    if (contains(name, "nintendo") || contains(name, "switch") || contains(name, "joy-con")) return HOS_PROMPT_SWITCH;
    return HOS_PROMPT_XBOX;   /* (Xbox pads, and the many that copy their layout) */
}

/* the first key bound to Switch button b (a Windows key code, the second slot's when the first is clear), 0 none */
static int first_key(int b) {
    int k = hos_binding(HOS_BIND_KEY, b, 0);
    if (k <= 0 || k > 0xFF) k = hos_binding(HOS_BIND_KEY, b, 1);
    return k > 0 && k <= 0xFF ? k : 0;
}

/* the controls now (BINDS: hos_binding's, as the Controls page sets them): the pad input bound to each Switch button, then its
   first key */
static void current_binds(int *bind) {
    for (int b = 0; b < HOS_SW_N; b++) bind[b] = hos_binding(HOS_BIND_PAD, b, 0), bind[HOS_SW_N + b] = first_key(b);
}

static int keyboard_built;   /* (the build made the keyboard face: prompts/keyboard/; choose_face) */

/* Auto's face: the first controller's (by SDL's name); with none, and none through XInput either, the keyboard's */
static int auto_face(const char *name) {
    return !name[0] && keyboard_built && !hos_xinput_pads() ? HOS_PROMPT_KEYBOARD : face_of_name(name);
}

/* HOS_PROMPT_* (faces[]) */
static int choose_face(const char *dir) {
    char path[MAX_PATH], line[80];
    int choice = 0;
    snprintf(path, sizeof path, "%sprompts/keyboard", dir);
    keyboard_built = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
    snprintf(path, sizeof path, "%shookshot_options.ini", dir);
    FILE *f = fopen(path, "r");
    if (f) {
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "button_prompts=", 15)) choice = atoi(line + 15);
        fclose(f);
    }
    if (choice >= 1 && choice <= HOS_PROMPT_N) return choice - 1;
    /* Auto: the first controller SDL opens (it scans every second: up to 1.5 s for one, at the game's start) */
    char name[128] = "";
    uint64_t b = 0;
    int32_t s[4] = {0};
    for (int i = 0; i < 30 && !name[0]; i++) {
        hos_sdl_pads(&b, s);
        hos_sdl_pad_first_name(name, sizeof name);
        if (!name[0]) Sleep(50);
    }
    const int face = auto_face(name);
    hos_log("prompts: auto: controller \"%s\": %s", name[0] ? name : "(none)", faces[face]);
    return face;
}

static uint32_t rd32le(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }
static void wr32le(uint8_t *p, uint32_t v) { p[0] = (uint8_t)v, p[1] = (uint8_t)(v >> 8), p[2] = (uint8_t)(v >> 16), p[3] = (uint8_t)(v >> 24); }
static unsigned rd16(const uint8_t *p) { return p[0] << 8 | p[1]; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static void wr16(uint8_t *p, unsigned v) { p[0] = (uint8_t)(v >> 8), p[1] = (uint8_t)v; }

/* the glyph of a code point in a format 4 subtable (st: its start, n its bytes), 0 none */
static unsigned cmap4_glyph(const uint8_t *st, size_t n, unsigned code) {
    const unsigned segs = rd16(st + 6) / 2;
    if (14 + 8 * (size_t)segs + 2 > n) return 0;
    const uint8_t *ends = st + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (unsigned i = 0; i < segs; i++) {
        if (rd16(ends + 2 * i) < code) continue;
        if (rd16(starts + 2 * i) > code) return 0;
        const unsigned ro = rd16(ranges + 2 * i);
        if (!ro) return (code + rd16(deltas + 2 * i)) & 0xFFFF;
        const uint8_t *g = ranges + 2 * i + ro + 2 * (code - rd16(starts + 2 * i));
        if (g + 2 > st + n) return 0;
        const unsigned gid = rd16(g);
        return gid ? (gid + rd16(deltas + 2 * i)) & 0xFFFF : 0;
    }
    return 0;
}

/* a code point's own segment (one code, by idDelta alone) pointed at glyph gid; 0 when it has none */
static int cmap4_point(uint8_t *st, unsigned segs, unsigned code, unsigned gid) {
    uint8_t *ends = st + 14, *starts = ends + 2 * segs + 2, *deltas = starts + 2 * segs, *ranges = deltas + 2 * segs;
    for (unsigned i = 0; i < segs; i++)
        if (rd16(starts + 2 * i) == code && rd16(ends + 2 * i) == code && !rd16(ranges + 2 * i)) {
            wr16(deltas + 2 * i, (gid - code) & 0xFFFF);
            return 1;
        }
    return 0;
}

/* the face's icons: each Switch button's segment pointed at the face's icon of its pad input (unbound: of its default's; the
   keyboard face: the icon of its first key, if it has one), the d-pad's at the face's, and the sticks' at the keyboard's or the
   game's own; how many buttons bound */
static int cmap4_bind(uint8_t *st, size_t n, int face) {
    const unsigned segs = rd16(st + 6) / 2;
    if (14 + 8 * (size_t)segs + 2 > n) return 0;   /* (as cmap4_glyph: the ranges' last entry too) */
    const unsigned base = PAD_BASE + FACE_CODES * (unsigned)face;
    const int keys = face == HOS_PROMPT_KEYBOARD;
    int done = 0;
    for (int b = 0; b < HOS_SW_N; b++) {
        const int k = keys ? first_key(b) : 0;
        const unsigned kid = k ? cmap4_glyph(st, n, KEY_BASE + (unsigned)k) : 0;
        if (kid && cmap4_point(st, segs, switch_code[b], kid)) {
            done++;
            continue;
        }
        const int pad = hos_binding(HOS_BIND_PAD, b, 0), bound = pad >= 0 && pad < HOS_PAD_ART_N;   /* (a paddle: the default's icon) */
        const unsigned gid = cmap4_glyph(st, n, base + (unsigned)(bound ? pad : switch_pad[b]));
        if (gid && cmap4_point(st, segs, switch_code[b], gid)) done += bound && !keys;
    }
    for (int k = 0; k < 3; k++) {
        const unsigned gid = cmap4_glyph(st, n, base + STATIC_AT + (unsigned)k);
        if (gid) cmap4_point(st, segs, static_code[k], gid);
    }
    for (int i = 0; i < 15; i++) {   /* (a font built without the keyboard face has none of these: nothing changes) */
        const unsigned gid = cmap4_glyph(st, n, (keys ? STICK_KEYS : STICK_GAME) + (unsigned)i);
        if (gid) cmap4_point(st, segs, stick_code[i], gid);
    }
    return done;
}

/* the face and the controls into the decrypted font's format 4 subtables; how many buttons */
static int bind_font(uint8_t *otf, size_t n, int face) {
    if (n < 12) return 0;
    const unsigned tables = rd16(otf + 4);
    for (unsigned t = 0; t < tables && 12 + 16 * (size_t)(t + 1) <= n; t++) {
        const uint8_t *rec = otf + 12 + 16 * t;
        if (memcmp(rec, "cmap", 4)) continue;
        const uint32_t off = rd32(rec + 8), len = rd32(rec + 12);
        if (off > n || len > n - off || len < 4) return 0;
        uint8_t *cmap = otf + off;
        const unsigned subs = rd16(cmap + 2);
        uint32_t seen[8];
        int nseen = 0, done = 0;
        for (unsigned s = 0; s < subs && 4 + 8 * (size_t)(s + 1) <= len; s++) {
            const uint32_t so = rd32(cmap + 4 + 8 * s + 4);
            if (so > len || len - so < 14 || rd16(cmap + so) != 4) continue;   /* (so + 14 wrapped past 4 GB) */
            int dup = 0;   /* (platforms 0 and 3 share one subtable) */
            for (int k = 0; k < nseen; k++) dup |= seen[k] == so;
            if (dup) continue;
            if (nseen < 8) seen[nseen++] = so;
            const unsigned sl = rd16(cmap + so + 2);
            done = cmap4_bind(cmap + so, sl <= len - so ? sl : len - so, face);
        }
        return done;
    }
    return 0;
}

/* a zstd frame's header for raw blocks of 128 KB: the 4-byte content size, and a window of 128 KB (raw blocks never look back:
   a single-segment frame's window, the whole content, is more than decoders take: zstd's own refuses 244 MB) */
static void zstd_head(uint8_t head[10], size_t n) {
    wr32le(head, 0xFD2FB528u);
    head[4] = 0x80;   /* (FCS 4 bytes, not single segment, no checksum, no dictionary) */
    head[5] = 7 << 3;   /* (window 2^(10+7)) */
    wr32le(head + 6, (uint32_t)n);
}

/* one raw block of a frame (zstd_head): k bytes (at most 128 KB), last for the frame's last */
static int zstd_block(FILE *o, const uint8_t *data, size_t k, int last) {
    const uint32_t bh = (uint32_t)(k << 3) | (last ? 1u : 0u);
    const uint8_t b3[3] = {(uint8_t)bh, (uint8_t)(bh >> 8), (uint8_t)(bh >> 16)};
    return fwrite(b3, 1, 3, o) == 3 && fwrite(data, 1, k, o) == k;
}

/* data as a zstd frame of raw blocks (zstd_head) */
static int write_zstd_raw(const char *path, const uint8_t *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    uint8_t head[10];
    zstd_head(head, n);
    int ok = fwrite(head, 1, 10, f) == 10;
    size_t at = 0;
    do {
        const size_t k = n - at < 131072 ? n - at : 131072;
        ok &= zstd_block(f, data + at, k, at + k == n);
        at += k;
    } while (at < n);
    ok &= fclose(f) == 0;
    return ok;
}

static uint8_t *read_all(const char *path, const char *magic, long *n) {   /* (the whole file, if it starts with magic: NULL any) */
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    *n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *t = *n > 8 ? (uint8_t *)malloc((size_t)*n) : NULL;
    const int got = t && fread(t, 1, (size_t)*n, f) == (size_t)*n;
    fclose(f);
    if (!got || (magic && memcmp(t, magic, 4))) {
        free(t);
        return NULL;
    }
    return t;
}

/* a zstd frame's content, when its header gives its size (at most 64 MB): the bytes (malloc, *out_n), NULL otherwise */
static uint8_t *unzstd(const uint8_t *in, size_t n, size_t *out_n) {
    const unsigned long long size = ZSTD_getFrameContentSize(in, n);
    if (size == ZSTD_CONTENTSIZE_ERROR || size == ZSTD_CONTENTSIZE_UNKNOWN || size > (64u << 20)) return NULL;
    uint8_t *out = (uint8_t *)malloc(size ? (size_t)size : 1);
    const size_t got = out ? ZSTD_decompress(out, (size_t)size, in, n) : 0;
    if (!out || ZSTD_isError(got) || got != size) {
        free(out);
        return NULL;
    }
    *out_n = (size_t)size;
    return out;
}

/* the resource size table, decompressed (nx2/prompt_assets.py size_table): "RESTBL", u32 version, u32 name field's size, u32
   hashed entries, u32 named entries; each hashed one u32 CRC-32 of a file's path and u32 size, by CRC; each named one (a path
   whose CRC-32 another path has too) the path in the name field and u32 size, by name. 1 when t (n bytes) is one */
static int restbl_head(const uint8_t *t, size_t n, uint32_t *name, uint32_t *hashed, uint32_t *named) {
    if (n < 22 || memcmp(t, "RESTBL", 6)) return 0;
    *name = rd32le(t + 10), *hashed = rd32le(t + 14), *named = rd32le(t + 18);
    return *name && *name <= 1024 && 22 + 8 * (uint64_t)*hashed + (*name + 4ull) * *named == n;
}

static int restbl_cmp(const uint8_t *x, const uint8_t *y, size_t key) {   /* (a hashed entry's CRC, a named entry's path) */
    if (key == 4) return (rd32le(x) > rd32le(y)) - (rd32le(x) < rd32le(y));
    return strncmp((const char *)x, (const char *)y, key);
}

/* the entries of a part of two tables (a: na of them, b: nb; each a key of key bytes and its u32 size) into out, in order: a's,
   with b's size where it is larger, and b's that a lacks; how many, -1 when a part is out of order. *raised: b's that went in */
static long restbl_part(const uint8_t *a, uint32_t na, const uint8_t *b, uint32_t nb, size_t key, uint8_t *out, int *raised) {
    const size_t es = key + 4;
    long n = 0;
    for (uint32_t i = 0, j = 0; i < na || j < nb; n++) {
        const uint8_t *x = i < na ? a + es * i : NULL, *y = j < nb ? b + es * j : NULL;
        if ((x && i && restbl_cmp(x - es, x, key) >= 0) || (y && j && restbl_cmp(y - es, y, key) >= 0)) return -1;
        const int c = !x ? 1 : !y ? -1 : restbl_cmp(x, y, key);
        memcpy(out + es * n, c <= 0 ? x : y, es);
        if (c > 0 || (c == 0 && rd32le(y + key) > rd32le(x + key))) {
            wr32le(out + es * n + key, rd32le(y + key));
            (*raised)++;
        }
        i += c <= 0, j += c >= 0;
    }
    return n;
}

/* a mod's size table (a, an bytes) with ours (b) merged in: each file the larger of the two entries, sorted as the game's
   (restbl_head). The bytes (malloc, *n), *raised how many of ours went in; NULL when either is no table, their name fields
   differ or one's entries are out of order */
static uint8_t *merge_restbl(const uint8_t *a, size_t an, const uint8_t *b, size_t bn, size_t *n, int *raised) {
    uint32_t name, hashed, named, bname, bhashed, bnamed;
    if (!restbl_head(a, an, &name, &hashed, &named) || !restbl_head(b, bn, &bname, &bhashed, &bnamed) || name != bname) return NULL;
    uint8_t *t = (uint8_t *)malloc(22 + 8 * ((size_t)hashed + bhashed) + (name + (size_t)4) * ((size_t)named + bnamed));
    *raised = 0;
    const long h = t ? restbl_part(a + 22, hashed, b + 22, bhashed, 4, t + 22, raised) : -1;
    const long m = h < 0 ? -1 : restbl_part(a + 22 + 8 * (size_t)hashed, named, b + 22 + 8 * (size_t)bhashed, bnamed, name,
                                            t + 22 + 8 * (size_t)h, raised);
    if (m < 0) {
        free(t);
        return NULL;
    }
    memcpy(t, a, 14);   /* (the mod's version and name field) */
    wr32le(t + 14, (uint32_t)h), wr32le(t + 18, (uint32_t)m);
    *n = 22 + 8 * (size_t)h + (name + (size_t)4) * (size_t)m;
    return t;
}

/* the size table served for a mod's own (mod: its file): the mod's with ours (prompts/rsizetable.zs) merged in, as raw zstd
   blocks in prompts/rsizetable.mod.zs (out). 1 with it; 0 when we have none (the faces' fonts fit the game's entry); -1 when
   the mod's cannot take ours: it is served as it is, and the game's own icons with it. Made once, for the first mod table asked */
static int sizes(const char *dir, const char *mod, char *out, size_t cap) {
    static volatile LONG state;   /* 0 not made, 1 being made, 2 made, 3 none of ours, 4 failed */
    static char from[MAX_PATH], path[MAX_PATH];
    if (InterlockedCompareExchange(&state, 1, 0) == 0) {
        snprintf(from, sizeof from, "%s", mod);
        snprintf(path, sizeof path, "%sprompts/rsizetable.zs", dir);
        LONG r = 3;
        if (GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES) {
            long on = 0, mn = 0;
            size_t bn = 0, an = 0, n = 0;
            int raised = 0;
            char file[MAX_PATH];   /* (mods.c's paths have '\', which fopen on Linux does not take) */
            snprintf(file, sizeof file, "%s", mod);
#ifndef _WIN32
            for (char *p = file; *p; p++) if (*p == '\\') *p = '/';
#endif
            uint8_t *ours = read_all(path, NULL, &on), *theirs = read_all(file, NULL, &mn);
            uint8_t *b = ours ? unzstd(ours, (size_t)on, &bn) : NULL, *a = theirs ? unzstd(theirs, (size_t)mn, &an) : NULL;
            uint8_t *t = a && b ? merge_restbl(a, an, b, bn, &n, &raised) : NULL;
            snprintf(path, sizeof path, "%sprompts/rsizetable.mod.zs", dir);
            r = t && write_zstd_raw(path, t, n) ? 2 : 4;
            if (r == 2)
                hos_log("prompts: %s: the button prompts' sizes merged into the mod's resource size table (%d entries raised or added)",
                        mod, raised);
            else
                hos_log("prompts: %s: %s: the mod's resource size table as it is", mod,
                        !b ? "prompts/rsizetable.zs could not be read" : !theirs ? "could not be read"
                        : !a ? "not a zstd frame that gives its size" : !t ? "not a resource size table as the game's"
                        : "prompts/rsizetable.mod.zs could not be written");
            free(t), free(a), free(b), free(theirs), free(ours);
        }
        InterlockedExchange(&state, r);
    }
    while (state == 1) Sleep(1);
    if (strcmp(from, mod)) return -1;   /* (a second table, of another name: as it is) */
    if (state == 2) snprintf(out, cap, "%s", path);
    return state == 2 ? 1 : state == 3 ? 0 : -1;
}

static volatile LONG setup_state;   /* setup's: 0 not looked, 1 looking, 2 prompts, 3 none */

/* the game folder and the face, found once; 0 when the folder has no prompts/ (built before them: the game's own icons) */
static int setup(char *dir, size_t cap, int *face) {
    static char d[MAX_PATH];
    static int f;
    if (InterlockedCompareExchange(&setup_state, 1, 0) == 0) {
        char path[MAX_PATH];
        int ok = game_dir(d, sizeof d);
        if (ok) {
            snprintf(path, sizeof path, "%sprompts", d);
            ok = GetFileAttributesA(path) != INVALID_FILE_ATTRIBUTES;
        }
        if (ok) f = choose_face(d);
        InterlockedExchange(&setup_state, ok ? 2 : 3);
    }
    while (setup_state == 1) Sleep(1);
    snprintf(dir, cap, "%s", d);
    *face = f;
    return setup_state == 2;
}

static uint64_t fnv(uint64_t h, const void *p, size_t n) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t *)p)[i]) * 0x100000001b3ull;
    return h;
}

typedef struct { uint32_t off, len, idx; const uint8_t *data; } Run;   /* (idx: its place in textures.bin) */
static int run_cmp(const void *a, const void *b) {
    const Run *x = (const Run *)a, *y = (const Run *)b;
    return x->off != y->off ? (x->off < y->off ? -1 : 1) : (x->idx > y->idx) - (x->idx < y->idx);
}
static int run_order(const void *a, const void *b) {
    const uint32_t x = (*(const Run *const *)a)->idx, y = (*(const Run *const *)b)->idx;
    return (x > y) - (x < y);
}

static void add_run(Run **runs, size_t *nruns, size_t *cap, uint32_t off, uint32_t len, const uint8_t *data) {
    if (*nruns == *cap) *runs = (Run *)realloc(*runs, (*cap *= 2) * sizeof **runs);
    (*runs)[*nruns] = (Run){off, len, (uint32_t)*nruns, data};
    (*nruns)++;
}

/* prompts/Common.bntx.zs: the game's textures (prompts/Common.bntx) with the blocks of prompts/<face>/textures.bin that this
   pad's controls choose (nx2/prompt_assets.py: "HPTX", u32 count; each: u8 Switch button (0xff: always), u8 pad input, u16,
   u32 runs; each run: u32 offset, u32 length, the bytes; a later entry over an earlier). Written again only when the face, the
   controls or the blocks changed (prompts/Common.key) */
/* out: the game's file (game) with the runs written over it (a later one over an earlier where they meet), as raw zstd blocks;
   written again only when its key (keypath: the hash h of what chose the runs) changed */
static int write_patched(const char *game, const char *out, const char *keypath, uint64_t h, Run *runs, size_t nruns) {
    qsort(runs, nruns, sizeof *runs, run_cmp);
    char keyline[32] = "", want[32];
    FILE *f;
    if ((f = fopen(keypath, "r"))) {
        if (!fgets(keyline, sizeof keyline, f)) keyline[0] = 0;
        fclose(f);
    }
    snprintf(want, sizeof want, "%016llx", (unsigned long long)h);
    int ok = !strcmp(keyline, want) && GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    if (!ok) {   /* the game's file through, the chosen runs written over it, out as raw zstd blocks */
        FILE *in = fopen(game, "rb"), *o = fopen(out, "wb");
        uint8_t *chunk = (uint8_t *)malloc(131072);
        ok = in && o && chunk;
        if (ok) {
            fseek(in, 0, SEEK_END);
            const uint64_t n = (uint64_t)ftell(in);
            fseek(in, 0, SEEK_SET);
            uint8_t head[10];
            zstd_head(head, n);
            ok = n < 0xFFFFFFFFull && fwrite(head, 1, 10, o) == 10;
            size_t first = 0;   /* (the first run that may reach this chunk) */
            const Run **here = (const Run **)malloc((nruns ? nruns : 1) * sizeof *here);
            ok = ok && here;
            for (uint64_t pos = 0; ok && pos < n;) {
                const size_t k = n - pos < 131072 ? (size_t)(n - pos) : 131072;
                ok = fread(chunk, 1, k, in) == k;
                while (first < nruns && runs[first].off + runs[first].len <= pos) first++;
                size_t nh = 0;   /* (the runs in this chunk, written in their order: a later one over an earlier) */
                for (size_t r = first; r < nruns && runs[r].off < pos + k; r++)
                    if (runs[r].off + runs[r].len > pos) here[nh++] = &runs[r];
                qsort(here, nh, sizeof *here, run_order);
                for (size_t i = 0; i < nh; i++) {
                    const Run *r = here[i];
                    const uint64_t a = r->off > pos ? r->off : pos;
                    const uint64_t z = r->off + r->len < pos + k ? r->off + r->len : pos + k;
                    memcpy(chunk + (a - pos), r->data + (a - r->off), (size_t)(z - a));
                }
                ok = ok && zstd_block(o, chunk, k, pos + k == n);
                pos += k;
            }
            free(here);
        }
        if (in) fclose(in);
        if (o) ok &= fclose(o) == 0;
        free(chunk);
        if (ok && (f = fopen(keypath, "w"))) {
            fputs(want, f);
            fclose(f);
        }
    }
    return ok;
}

/* the runs of a face's textures.bin (t, tn bytes) that the controls (bind: each Switch button's pad input) choose, in the
   file's order; *nruns of them */
static Run *texture_runs(const uint8_t *t, long tn, const int *bind, size_t *nruns) {
    const uint32_t count = rd32le(t + 4);
    size_t at = 8, cap_runs = 256;
    Run *runs = (Run *)malloc(cap_runs * sizeof *runs);
    *nruns = 0;
    for (uint32_t e = 0; e < count && at + 8 <= (size_t)tn; e++) {
        const int sw = t[at], pad = t[at + 1];
        const uint32_t nr = rd32le(t + at + 4);
        const int on = sw == 0xff || (sw < HOS_SW_N && bind[sw] == pad);
        at += 8;
        for (uint32_t r = 0; r < nr && at + 8 <= (size_t)tn; r++) {
            const uint32_t off = rd32le(t + at), len = rd32le(t + at + 4);
            if (len > (size_t)tn - at - 8) break;
            if (on) add_run(&runs, nruns, &cap_runs, off, len, t + at + 8);
            at += 8 + len;
        }
    }
    return runs;
}

/* the face whose textures a face shows (the keyboard's: the Switch's), and the controls they follow (the keyboard face: the
   Switch's own, its default controls: the game's pictures as they are); own: room for those */
static int texture_face(int face) { return face == HOS_PROMPT_KEYBOARD ? HOS_PROMPT_SWITCH : face; }
static const int *texture_bind(int face, const int *bind, int *own) {
    if (face != HOS_PROMPT_KEYBOARD) return bind;
    for (int b = 0; b < HOS_SW_N; b++) own[b] = switch_pad[b];
    return own;
}

static void live_textures_start(const char *bntx, const Run *runs, size_t nruns);
static void part_served(int p, int face, const int *bind);
enum { PART_TEXTURES, PART_LAYOUTS, PART_FONT, PARTS };

static int build_textures(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    snprintf(path, sizeof path, "%sprompts/%s/textures.bin", dir, faces[texture_face(face)]);
    long tn;
    uint8_t *t = read_all(path, "HPTX", &tn);
    if (!t) return 0;   /* (built without the textures: the game's) */
    int bind[BINDS], own[HOS_SW_N];
    current_binds(bind);
    uint64_t h = fnv(fnv(fnv(0xcbf29ce484222325ull, &face, sizeof face), bind, sizeof bind), t, (size_t)tn);
    size_t nruns;
    Run *runs = texture_runs(t, tn, texture_bind(face, bind, own), &nruns);
    snprintf(path, sizeof path, "%sprompts/Common.bntx", dir);
    live_textures_start(path, runs, nruns);   /* (before write_patched sorts them: the ranges only) */
    snprintf(out, cap, "%sprompts/Common.bntx.zs", dir);
    snprintf(path, sizeof path, "%sprompts/Common.bntx", dir);
    char keypath[MAX_PATH];
    snprintf(keypath, sizeof keypath, "%sprompts/Common.key", dir);
    const int ok = write_patched(path, out, keypath, h, runs, nruns);
    hos_log("prompts: %s textures, %zu runs for this pad's controls%s", faces[face], nruns,
            ok ? "" : ": could not be written");
    if (ok) part_served(PART_TEXTURES, face, bind);
    free(runs);
    free(t);
    return ok;
}

/* ---- (hookshot) the prompts changed while the game runs: a new face (the Button prompts row, or Auto's controller) or new
   controls (the Controls page, a layout preset) reach what the game has loaded, at once, by the rules the start follows
   (each icon the pad input bound to its button; a mod's file never touched: only the files served here are looked for).
   - The textures (Common.bntx): their data lies in a memory pool the game makes over the file (hos_prompts_pool, found by
     bytes no run writes). The renderer uploads what is written again (HOS_WRITTEN).
   - The layouts (Common.blarc): where the game keeps them, found by their first bytes near the start of an address region
     (find_layouts); written only where the game's bytes are still the file's.
   - The font (External.bfotf, every face's icons): where FreeType reads it (hos_prompts_glyph, from nx2/oot_patches.py's
     hook in its glyph render); only its cmap changes (for the texts set after), and the texts' button codes are rewritten
     to the face's own (hos_prompts_text_layout).
   A change puts the game's own bytes back where the runs written before fall, then writes the runs the new face and controls
   choose (as the start: texture_runs, layout_runs). Only within what was found: the rest waits for the next start. */
typedef struct {
    char path[MAX_PATH];        /* the game's file the runs go over (prompts/Common.bntx, prompts/Common.blarc) */
    volatile uint64_t base;     /* the guest address of the file's offset 0 once seen, 0 before */
    uint64_t lo, hi;            /* the guest range runs may be written in */
    uint32_t (*ranges)[2];      /* the runs written: (offset, length), the game's own bytes back first */
    size_t nranges;
    int guarded;                /* a run written only where the game's bytes are still the file's */
} Live;

static void keep_ranges(Live *l, const Run *runs, size_t nruns) {
    free(l->ranges);
    l->ranges = (uint32_t (*)[2])malloc((nruns ? nruns : 1) * sizeof *l->ranges);
    for (size_t r = 0; r < nruns; r++) l->ranges[r][0] = runs[r].off, l->ranges[r][1] = runs[r].len;
    l->nranges = nruns;
}

/* the game's own bytes back where l's runs were, then these runs (in their order: a later one over an earlier), each marked
   written; how many were outside l's range, or (guarded) over bytes the game changed: not written. Guarded, every run is
   looked at before any is written (a later one over an earlier finds the file's bytes), and only those written are kept */
static size_t put_runs(Live *l, const Run *runs, size_t nruns, size_t *written, size_t *put_back) {
    FILE *f = fopen(l->path, "rb");
    uint8_t *chunk = (uint8_t *)malloc(131072);
    uint8_t *ok = (uint8_t *)calloc(nruns ? nruns : 1, 1);
    size_t outside = 0;
    *written = *put_back = 0;
    const uint64_t base = l->base;   /* (once: 0 when the game let the memory go, nothing written then) */
    if (!base) nruns = 0;
    for (size_t r = 0; base && f && chunk && r < l->nranges; r++) {
        const uint64_t off = l->ranges[r][0], len = l->ranges[r][1], a = base + off;
        uint8_t *q = a >= l->lo && a + len <= l->hi ? (uint8_t *)hos_guest_range(a, len) : NULL;
        if (!q) continue;
        for (uint64_t k = 0; k < len;) {
            const size_t n = len - k < 131072 ? (size_t)(len - k) : 131072;
            if (fseek(f, (long)(off + k), SEEK_SET) || fread(chunk, 1, n, f) != n) break;
            memcpy(q + k, chunk, n);
            k += n;
        }
        HOS_WRITTEN(q, (size_t)len);
        (*put_back)++;
    }
    for (size_t r = 0; r < nruns && ok; r++) {
        const uint64_t a = base + runs[r].off, len = runs[r].len;
        const uint8_t *q = a >= l->lo && a + len <= l->hi ? (const uint8_t *)hos_guest_range(a, len) : NULL;
        ok[r] = q != NULL;
        for (uint64_t k = 0; ok[r] && l->guarded && k < len;) {
            const size_t n = len - k < 131072 ? (size_t)(len - k) : 131072;
            ok[r] = f && chunk && !fseek(f, (long)(runs[r].off + k), SEEK_SET) && fread(chunk, 1, n, f) == n &&
                    !memcmp(q + k, chunk, n);
            k += n;
        }
    }
    for (size_t r = 0; r < nruns; r++) {
        if (!ok || !ok[r]) {
            outside++;
            continue;
        }
        const uint64_t a = base + runs[r].off;
        uint8_t *q = a >= l->lo && a + runs[r].len <= l->hi ? (uint8_t *)hos_guest_range(a, runs[r].len) : NULL;
        if (!q) {
            outside++;
            continue;
        }
        memcpy(q, runs[r].data, runs[r].len);
        HOS_WRITTEN(q, runs[r].len);
        (*written)++;
    }
    if (f) fclose(f);
    free(chunk);
    keep_ranges(l, runs, nruns);
    size_t kept = 0;   /* (the ones written: the next change puts the game's bytes back there) */
    for (size_t r = 0; r < nruns; r++)
        if (ok && ok[r]) memmove(l->ranges[kept++], l->ranges[r], sizeof *l->ranges);
    l->nranges = kept;
    free(ok);
    return outside;
}

static int in_runs(uint64_t a, uint64_t n, const Run *runs, size_t nruns) {
    for (size_t r = 0; r < nruns; r++)
        if (a < (uint64_t)runs[r].off + runs[r].len && runs[r].off < a + n) return 1;
    return 0;
}

enum { SAMPLES = 3, SAMPLE_LEN = 64 };
static struct {
    Live l;                               /* over the game's Common.bntx */
    uint64_t data_off, data_len;          /* its texture data: offset (the BRTD block's + 16) and length */
    uint64_t sample[SAMPLES];             /* file offsets of bytes no run of the start's writes */
    uint8_t want[SAMPLES][SAMPLE_LEN];
    uint8_t *bins[HOS_PROMPT_N];          /* each face's textures.bin, read when first chosen (the runs point into it) */
    uint32_t (*served)[2];                /* the runs of the file as served (offset, length): a pool made again holds them */
    size_t nserved;
    long nbins[HOS_PROMPT_N];
} tex;

/* what the game is to show now (the start's, then each change's): the face and the controls; and what each part it loaded
   shows (PART_*: the start's until written over, so a part found after a change catches up) */
static int live_face_now = -1, live_bind_now[BINDS];
static int part_face[PARTS], part_bind[PARTS][BINDS];
static int served_face[PARTS], served_bind[PARTS][BINDS];   /* (each part's file as served: what the game loads again) */
static void live_start(int face, const int *bind) {   /* (the first file served: the start's) */
    if (live_face_now >= 0) return;
    memcpy(live_bind_now, bind, sizeof live_bind_now);
    live_face_now = face;
}
static void part_served(int p, int face, const int *bind) {   /* (its file written for this face and these controls) */
    served_face[p] = part_face[p] = face;
    memcpy(served_bind[p], bind, sizeof served_bind[p]);
    memcpy(part_bind[p], bind, sizeof part_bind[p]);
    live_start(face, bind);
}

/* (build_textures) the file's texture data, with bytes to know its pool by, and the ranges of the start's runs */
static void live_textures_start(const char *bntx, const Run *runs, size_t nruns) {
    FILE *f = fopen(bntx, "rb");
    uint8_t h[0x38], blk[16];
    if (!f) return;
    int ok = fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "BNTX", 4);
    uint64_t brtd = 0;
    if (ok) memcpy(&brtd, h + 0x30, 8);   /* (the NX block's pointer to the BRTD block, as the file has it: an offset) */
    ok = ok && !fseek(f, (long)brtd, SEEK_SET) && fread(blk, 1, 16, f) == 16 && !memcmp(blk, "BRTD", 4);
    if (ok) {
        uint64_t size;
        memcpy(&size, blk + 8, 8);
        tex.data_off = brtd + 16;
        tex.data_len = size > 16 ? size - 16 : 0;
        for (int s = 0; s < SAMPLES && ok; s++) {   /* (a quarter, a half, three quarters in; past any run there) */
            uint64_t at = tex.data_off + tex.data_len / 4 * (s + 1);
            while (in_runs(at, SAMPLE_LEN, runs, nruns) && at + 2 * SAMPLE_LEN < tex.data_off + tex.data_len) at += 4096;
            tex.sample[s] = at;
            ok = !fseek(f, (long)at, SEEK_SET) && fread(tex.want[s], 1, SAMPLE_LEN, f) == SAMPLE_LEN;
        }
    }
    fclose(f);
    if (!ok || !tex.data_len) {
        tex.data_len = 0;
        hos_log("prompts: %s is not a texture file known: the textures change at the next start", bntx);
        return;
    }
    snprintf(tex.l.path, sizeof tex.l.path, "%s", bntx);
    keep_ranges(&tex.l, runs, nruns);
    free(tex.served);
    tex.served = (uint32_t (*)[2])malloc((nruns ? nruns : 1) * sizeof *tex.served);
    if (tex.served) memcpy(tex.served, tex.l.ranges, nruns * sizeof *tex.served);
    tex.nserved = tex.served ? nruns : 0;
}

/* nvn_native.c: a memory pool the game made (its storage, size bytes): the one Common.bntx's texture data is in? (Its
   texture data there, or the whole file) */
static SRWLOCK tex_lock = SRWLOCK_INIT;   /* (the textures' pool: found, let go, written; the game's threads and the input's) */
void hos_prompts_pool(uint64_t mem, uint64_t size) {
    if (!tex.data_len || tex.l.base || size < SAMPLE_LEN) return;
    const uint8_t *p = (const uint8_t *)hos_guest_range(mem, 4);
    if (!p) return;
    const uint64_t base = !memcmp(p, "BNTX", 4) ? mem : mem - tex.data_off;
    for (int s = 0; s < SAMPLES; s++) {
        const uint64_t a = base + tex.sample[s];
        const uint8_t *q = a >= mem && a + SAMPLE_LEN <= mem + size ? (const uint8_t *)hos_guest_range(a, SAMPLE_LEN) : NULL;
        if (!q || memcmp(q, tex.want[s], SAMPLE_LEN)) return;
    }
    AcquireSRWLockExclusive(&tex_lock);
    const int first = !tex.l.base;
    if (first) {
        tex.l.lo = mem, tex.l.hi = mem + size;
        part_face[PART_TEXTURES] = served_face[PART_TEXTURES];   /* (the file as served: a change since is written at the next look) */
        memcpy(part_bind[PART_TEXTURES], served_bind[PART_TEXTURES], sizeof part_bind[PART_TEXTURES]);
        if (tex.served) {   /* (the served runs: the bytes put back at that look) */
            free(tex.l.ranges);
            tex.l.ranges = (uint32_t (*)[2])malloc((tex.nserved ? tex.nserved : 1) * sizeof *tex.l.ranges);
            if (tex.l.ranges) memcpy(tex.l.ranges, tex.served, tex.nserved * sizeof *tex.served);
            tex.l.nranges = tex.l.ranges ? tex.nserved : 0;
        }
        __atomic_store_n(&tex.l.base, base, __ATOMIC_RELEASE);   /* (lo and hi first: the input thread reads them once base is set) */
    }
    ReleaseSRWLockExclusive(&tex_lock);
    if (first)
        hos_log("prompts: the textures' pool at %#llx (+%#llx): a new face or new controls show at once", (unsigned long long)mem,
                (unsigned long long)size);
}

/* nvn_native.c: the game finalized a memory pool (its storage): the textures' one? Then nothing is written there any more
   (the memory may hold something else), until the game makes it again (hos_prompts_pool). A change being written waits
   (tex_lock): the game frees the memory once this returns */
void hos_prompts_pool_gone(uint64_t mem) {
    if (!__atomic_load_n(&tex.l.base, __ATOMIC_ACQUIRE) || mem != tex.l.lo) return;
    AcquireSRWLockExclusive(&tex_lock);
    const int ours = tex.l.base && mem == tex.l.lo;
    if (ours) __atomic_store_n(&tex.l.base, 0, __ATOMIC_RELEASE);
    ReleaseSRWLockExclusive(&tex_lock);
    if (ours)
        hos_log("prompts: the textures' pool at %#llx was finalized: not written until the game makes it again",
                (unsigned long long)mem);
}

/* the pool's textures as this face and these controls choose them; 0 when they could not be */
/* the face's textures.bin, read once (before tex_lock: a finalize waits for the writes alone); 0 none */
static int texture_bin(const char *dir, int face) {
    face = texture_face(face);
    if (!tex.bins[face]) {
        char path[MAX_PATH];
        snprintf(path, sizeof path, "%sprompts/%s/textures.bin", dir, faces[face]);
        tex.bins[face] = read_all(path, "HPTX", &tex.nbins[face]);
    }
    return tex.bins[face] != NULL;
}

static int live_textures(const char *dir, int face, const int *bind) {
    if (!texture_bin(dir, face)) return 0;
    size_t nruns, written, put_back;
    if (!__atomic_load_n(&tex.l.base, __ATOMIC_ACQUIRE)) return 0;   /* (finalized since: hos_prompts_pool_gone; tex_lock held) */
    int own[HOS_SW_N];
    Run *runs = texture_runs(tex.bins[texture_face(face)], tex.nbins[texture_face(face)], texture_bind(face, bind, own), &nruns);
    const size_t outside = put_runs(&tex.l, runs, nruns, &written, &put_back);
    free(runs);
    hos_log("prompts: now %s textures: %zu runs written, %zu put back%s", faces[face], written, put_back,
            outside ? " (some outside the pool: at the next start)" : "");
    return 1;
}

static Live lay;                       /* over the game's Common.blarc */
static struct {
    volatile uint64_t at;              /* where the game keeps the served font, decrypted (behind its header), 0 not seen */
    uint32_t n;                        /* the file's length */
    uint8_t head[64];                  /* its first bytes, decrypted: the tables' directory */
} font;

typedef struct {
    Run *runs;
    size_t n;
    uint64_t h;   /* (what chose them: the file's key) */
    uint8_t *ring, *hint;
    float *xs;
    int place[4], spread;
} Layout;
static int layout_runs(const char *dir, int face, const int *bind, Layout *o);
static void layout_free(Layout *o) {
    free(o->xs);
    free(o->hint);
    free(o->runs);
    free(o->ring);
}

static Layout lay_start;      /* the runs served (until find_layouts) */
static uint8_t lay_head[32];   /* the file's first bytes (no run writes them) */
static uint64_t lay_n;         /* its length, 0 not served */

/* where the game keeps the layouts: the whole Common.blarc, unpacked near the start of an address region of its own (the
   game's resources each take one: the textures' pool and the font too), its offsets made pointers where they lie. Looked
   for by the file's first bytes at the pages of each region's first 1 MB, every 2 s from when the textures' pool is known
   (the game unpacks its resources then; without the textures, at once), 30 times at most; then the runs served that are still there are the ones a change
   puts the game's bytes back over (put_runs, guarded: never over a pointer the game made) */
/* run r's bytes at q as write_patched left them: each one the run's, or a later run's (idx) over it */
static int run_as_served(const Run *runs, size_t n, size_t r, const uint8_t *q) {
    for (uint32_t k = 0; k < runs[r].len; k++) {
        if (q[k] == runs[r].data[k]) continue;
        const uint64_t at = (uint64_t)runs[r].off + k;
        int over = 0;
        for (size_t s = 0; s < n && !over; s++) over = runs[s].idx > runs[r].idx && at >= runs[s].off && at < (uint64_t)runs[s].off + runs[s].len;
        if (!over) return 0;
    }
    return 1;
}

static void find_layouts(void) {
    static int tries;
    static int64_t last;
    static uint64_t regions[4096][2];
    const int64_t now = hos_now_ns();
    if (lay.base || tries >= 30 || !lay_n || (tex.data_len && !tex.l.lo) || now - last < 2000000000) return;
    last = now, tries++;
    int n = hos_regions(regions, 4096);
    if (n > 4096) n = 4096;
    for (int i = 0; i < n && !lay.base; i++) {
        const uint64_t lo = regions[i][0], hi = regions[i][1];
        for (uint64_t a = lo; a < lo + (1u << 20) && hi - a >= lay_n && !lay.base; a += 4096) {
            const uint8_t *q = (const uint8_t *)hos_guest_range(a, sizeof lay_head);
            if (q && !memcmp(q, lay_head, sizeof lay_head) && hos_guest_range(a, lay_n)) lay.lo = a, lay.hi = a + lay_n, lay.base = a;
        }
    }
    size_t kept = 0;
    if (lay.base) {
        keep_ranges(&lay, lay_start.runs, lay_start.n);
        for (size_t r = 0; r < lay_start.n; r++) {
            const Run *run = &lay_start.runs[r];
            const uint8_t *q = (const uint8_t *)hos_guest_range(lay.base + run->off, run->len);
            if (q && run_as_served(lay_start.runs, lay_start.n, r, q)) memmove(lay.ranges[kept++], lay.ranges[r], sizeof *lay.ranges);
        }
        lay.nranges = kept;
    }
    if (!lay.base && tries < 30) return;
    if (lay.base)
        hos_log("prompts: the layouts at %#llx: the Talk ring and the ocarina hint follow at once", (unsigned long long)lay.base);
    else
        hos_log("prompts: the layouts not found among the game's %d address regions: they change at the next start", n);
    if (lay.base && kept < lay_start.n)
        hos_log("prompts: %zu of the layouts' %zu runs served are no longer as served: left as the game has them", lay_start.n - kept,
                lay_start.n);
    layout_free(&lay_start);
}

static int live_layouts(const char *dir, int face, const int *bind) {
    const uint8_t *q = (const uint8_t *)hos_guest_range(lay.base, sizeof lay_head);
    /* (the game let the memory go: nothing written there. Unlike the textures' pool (tex_lock), nothing holds the region
       from here to the writes: the game frees its resources' regions on its own thread, with no hook here, so a region let
       go in that moment is not seen. A new run is still written only where the bytes are the file's (put_runs, guarded); the
       last change's bytes put back are not checked so) */
    if (!q || memcmp(q, lay_head, sizeof lay_head)) {
        hos_log("prompts: the layouts at %#llx are no longer there: they change at the next start", (unsigned long long)lay.base);
        lay.base = 0;
        return 0;
    }
    Layout o;
    if (!layout_runs(dir, face, bind, &o)) return 0;
    size_t written, put_back;
    const size_t outside = put_runs(&lay, o.runs, o.n, &written, &put_back);
    hos_log("prompts: now the Talk ring's A B X Y at places %d %d %d %d, the ocarina hint %s: %zu runs written, %zu put back%s",
            o.place[0], o.place[1], o.place[2], o.place[3], o.spread ? "spread" : "as is", written, put_back,
            outside ? " (some outside: at the next start)" : "");
    layout_free(&o);
    return 1;
}

/* the font with the face's icons on these controls (prompts/External.bfotf, cmap4_bind: hos_binding's), decrypted behind
   its header (key: the XOR key); bound: how many buttons. NULL none */
static uint8_t *font_bound(const char *dir, int face, long *n, uint32_t *key, int *bound) {
    char path[MAX_PATH];
    snprintf(path, sizeof path, "%sprompts/External.bfotf", dir);
    uint8_t *b = read_all(path, NULL, n);
    if (!b || *n < 16 || *n % 4) {
        hos_log("prompts: no %s: the game's own icons", path);
        free(b);
        return NULL;
    }
    *key = rd32le(b) ^ MAGIC;
    for (long i = 8; i < *n; i += 4) wr32le(b + i, rd32le(b + i) ^ *key);
    *bound = bind_font(b + 8, (size_t)*n - 8, face);
    return b;
}

/* the texts' icons after a change while the game runs. The game keeps each code's glyph drawn (by the code, whatever the font
   says now), so a text's Switch button codes are rewritten to codes of their own for the face and the pad input
   (nx2/prompt_assets.py BIND_BASE: never drawn before with another icon; in UTF-8 three bytes, as the game's), as the game
   lays the box out or updates it to draw it (nx2/oot_patches.py; box: the text box pane, its text at +0xf0 in UTF-8, often
   the layout's own, its length in bytes at +0x124), and the box laid out and drawn anew (its status, at +8: 0x20000 laid
   out, 0x100000 drawn, as the word after it allows). Until a change, the texts as the game has them: the font served for
   this start's face and controls */
#define BIND_BASE 0xF000
static volatile LONG font_changes;

/* each format 4 subtable of the decrypted font (otf, n bytes): code's own segment pointed at the glyph of code to; 0 when it has
   no glyph there (or code no segment of its own) */
static int font_point(uint8_t *otf, size_t n, unsigned code, unsigned to) {
    if (n < 12) return 0;
    const unsigned tables = rd16(otf + 4);
    int done = 0;
    for (unsigned t = 0; t < tables && 12 + 16 * (size_t)(t + 1) <= n; t++) {
        const uint8_t *rec = otf + 12 + 16 * t;
        if (memcmp(rec, "cmap", 4)) continue;
        const uint32_t off = rd32(rec + 8), len = rd32(rec + 12);
        if (off > n || len > n - off || len < 4) return 0;
        uint8_t *cmap = otf + off;
        for (unsigned s = 0; s < rd16(cmap + 2) && 4 + 8 * (size_t)(s + 1) <= len; s++) {
            const uint32_t so = rd32(cmap + 4 + 8 * s + 4);
            if (so > len || len - so < 14 || rd16(cmap + so) != 4) continue;
            const unsigned sl = rd16(cmap + so + 2), sn = sl <= len - so ? sl : len - so, segs = rd16(cmap + so + 6) / 2;
            const unsigned gid = 14 + 8 * (size_t)segs + 2 <= sn ? cmap4_glyph(cmap + so, sn, to) : 0;
            if (gid) done |= cmap4_point(cmap + so, segs, code, gid);   /* (platforms 0 and 3 share one: the same written twice) */
        }
        return done;
    }
    return 0;
}

/* the keyboard face's codes in texts: one for each Switch button and key shown, given out as they come (from SLOT_BASE: a code
   whose glyph the game has never drawn) and pointed at the key's icon where the game keeps the font; the font served again
   (live_font) keeps them (slots_point) */
static SRWLOCK slot_lock = SRWLOCK_INIT;
static uint8_t slot_button[SLOTS], slot_key[SLOTS];
static volatile int nslots;

static void slots_point(uint8_t *otf, size_t n) {   /* (slot_lock held) */
    for (int s = 0; s < nslots; s++) font_point(otf, n, SLOT_BASE + (unsigned)s, KEY_BASE + slot_key[s]);
}

/* the code of Switch button b on key k in a text; -1 when the key has no icon, the font is not found, or all are given out */
static int key_slot(int b, int k) {
    AcquireSRWLockExclusive(&slot_lock);
    int s = 0;
    while (s < nslots && !(slot_button[s] == b && slot_key[s] == k)) s++;
    if (s == nslots) {
        uint8_t *q = s < SLOTS && font.at ? (uint8_t *)hos_guest_range(font.at, font.n - 8) : NULL;
        if (q && font_point(q, font.n - 8, SLOT_BASE + (unsigned)s, KEY_BASE + (unsigned)k)) {
            slot_button[s] = (uint8_t)b, slot_key[s] = (uint8_t)k;
            nslots = s + 1;
        } else {
            static int full_said;   /* (said once: every text after asks again) */
            if (s == SLOTS && !full_said++)
                hos_log("prompts: the %d codes for buttons on keys are all given out: a key bound from now on shows the Switch's "
                        "icon in texts until the next start", SLOTS);
            s = -1;
        }
    }
    ReleaseSRWLockExclusive(&slot_lock);
    return s < 0 ? -1 : SLOT_BASE + s;
}

static int text_code(unsigned c, int face, const int *bind) {   /* the code that shows c's button now, else -1 */
    int b = -1, k = -1;
    for (int i = 0; i < 15 && keyboard_built; i++)   /* (a stick's: the keyboard's keys, or the game's own icon) */
        if (c == stick_code[i] || c == STICK_KEYS + (unsigned)i || c == STICK_GAME + (unsigned)i)
            return (face == HOS_PROMPT_KEYBOARD ? STICK_KEYS : STICK_GAME) + i;
    for (int i = 0; i < HOS_SW_N && b < 0; i++)
        if (c == switch_code[i]) b = i;
    if (c >= SLOT_BASE && c < SLOT_BASE + SLOTS && (int)(c - SLOT_BASE) < nslots) b = slot_button[c - SLOT_BASE];
    for (int i = 0; i < 3 && k < 0; i++)
        if (c == static_code[i]) k = i;
    if (c >= BIND_BASE && c < BIND_BASE + HOS_PROMPT_N * HOS_SW_N * HOS_PAD_ART_N) b = (int)((c - BIND_BASE) / HOS_PAD_ART_N % HOS_SW_N);
    if (c >= PAD_BASE + STATIC_AT && c < PAD_BASE + FACE_CODES * HOS_PROMPT_N && (c - PAD_BASE) % FACE_CODES >= STATIC_AT &&
        (c - PAD_BASE) % FACE_CODES < STATIC_AT + 3)
        k = (int)((c - PAD_BASE) % FACE_CODES - STATIC_AT);
    if (k >= 0) return PAD_BASE + FACE_CODES * face + STATIC_AT + k;
    if (b < 0) return -1;
    if (face == HOS_PROMPT_KEYBOARD && bind[HOS_SW_N + b]) {   /* (its key's code; none: the Switch's icon, as unbound) */
        const int s = key_slot(b, bind[HOS_SW_N + b]);
        if (s >= 0) return s;
    }
    const int pad = bind[b] >= 0 && bind[b] < HOS_PAD_ART_N ? bind[b] : switch_pad[b];   /* (a paddle: the default's icon) */
    return BIND_BASE + (face * HOS_SW_N + b) * HOS_PAD_ART_N + pad;
}

/* the box's codes for the buttons as they are now; 1 when it changed them */
static int text_rewrite(uint64_t box) {
    if (!font_changes) return 0;
    const int face = live_face_now;
    if (face < 0 || face >= HOS_PROMPT_N) return 0;
    int bind[BINDS];
    memcpy(bind, live_bind_now, sizeof bind);
    const uint64_t *text = (const uint64_t *)hos_guest_range(box + 0xf0, 8);
    const uint32_t *len = (const uint32_t *)hos_guest_range(box + 0x124, 4);
    if (!text || !len || !*text || *len < 3 || *len > 65536) return 0;
    uint8_t *s = (uint8_t *)hos_guest_range(*text, *len);
    if (!s) return 0;
    int changed = 0;
    for (uint32_t i = 0; i + 2 < *len; i++) {   /* (UTF-8: the codes here are 3 bytes, 1110xxxx 10xxxxxx 10xxxxxx) */
        if ((s[i] != 0xEE && s[i] != 0xEF) || (s[i + 1] & 0xC0) != 0x80 || (s[i + 2] & 0xC0) != 0x80) continue;
        const unsigned code = (unsigned)(s[i] & 0x0F) << 12 | (unsigned)(s[i + 1] & 0x3F) << 6 | (s[i + 2] & 0x3F);
        const int c = text_code(code, face, bind);
        if (c >= 0 && (unsigned)c != code) {
            s[i] = (uint8_t)(0xE0 | c >> 12), s[i + 1] = (uint8_t)(0x80 | (c >> 6 & 0x3F)), s[i + 2] = (uint8_t)(0x80 | (c & 0x3F));
            changed = 1;
        }
        i += 2;
    }
    return changed;
}

/* the box's status bits set (as the word after them allows) */
static void mark_box(uint64_t box, uint32_t bits) {
    const uint64_t *status = (const uint64_t *)hos_guest_range(box + 8, 8);
    uint32_t *word = status && *status ? (uint32_t *)hos_guest_range(*status, 8) : NULL;
    if (word) *word |= bits & word[1];
}

/* nx2/oot_patches.py, as the game lays a text box out (f_8152aec0, before it looks at the box's status: laid out now) */
void hos_prompts_text_layout(uint64_t box) {
    if (text_rewrite(box)) mark_box(box, 0x20000);
}

/* nx2/oot_patches.py, as the game draws a text box (f_8152b670, each time it shows it): a box rewritten is laid out again at
   the next update (its status) */
void hos_prompts_text_draw(uint64_t box) {
    if (text_rewrite(box)) mark_box(box, 0x20000 | 0x100000);
}

/* nx2/oot_patches.py, as FreeType draws a glyph (ft_smooth_render, f_81680c60; slot: its FT_GlyphSlot): the font the glyph
   is of (slot->face at +8, the face's stream at +192: its base and size), the one served? Then where the game keeps it,
   decrypted (FreeType and the text shaping read it there) */
void hos_prompts_glyph(uint64_t slot) {
    static volatile uint64_t last;   /* (the last face found to be another font: its glyphs not looked at again) */
    if (font.at || !font.n) return;
    const uint64_t *s = (const uint64_t *)hos_guest_range(slot + 8, 8);
    const uint64_t face = s ? *s : 0;
    if (!face || face == last) return;
    const uint64_t *st = (const uint64_t *)hos_guest_range(face + 192, 8);
    const uint64_t *base = st ? (const uint64_t *)hos_guest_range(*st, 16) : NULL;
    if (!base) return;   /* (not readable yet: looked at again) */
    if (base[1] != font.n && base[1] != font.n - 8) {
        last = face;   /* (another font: not looked at again while its glyphs follow each other) */
        return;
    }
    /* (a stream of the file's size may hold the font behind its 8-byte header, or from its start, as on native Windows: the
       font's own head says which) */
    uint64_t at = 0;
    int unread = 0;
    for (uint64_t a = base[0] + (base[1] == font.n ? 8 : 0);; a = base[0]) {
        const uint8_t *q = (const uint8_t *)hos_guest_range(a, font.n - 8);
        if (!q) unread = 1;
        else if (!memcmp(q, font.head, sizeof font.head)) { at = a; break; }
        if (a == base[0]) break;
    }
    if (!at) {
        if (!unread) last = face;   /* (another font of that size) */
        return;   /* (else not readable yet: looked at again) */
    }
    font.at = at;
    hos_log("prompts: the font at %#llx: the icons in texts follow at once", (unsigned long long)font.at);
}

static int live_font(const char *dir, int face) {
    long n;
    uint32_t key;
    int bound;
    uint8_t *b = font_bound(dir, face, &n, &key, &bound);
    uint8_t *q = b && (uint32_t)n == font.n ? (uint8_t *)hos_guest_range(font.at, (uint64_t)n - 8) : NULL;
    const int ok = q && !memcmp(q, b + 8, 64);   /* (still the font: its tables' directory, which a binding leaves as is) */
    if (ok) {
        AcquireSRWLockExclusive(&slot_lock);   /* (the keys' codes given out stay on their keys) */
        slots_point(b + 8, (size_t)n - 8);
        memcpy(q, b + 8, (size_t)n - 8);
        ReleaseSRWLockExclusive(&slot_lock);
        InterlockedIncrement(&font_changes);
        hos_log("prompts: now the %s icons in texts, %d buttons on these controls", faces[face], bound);
    } else if (q) {
        hos_log("prompts: the font is no longer where FreeType read it: the icons in texts at the next start");
    }
    free(b);
    return ok;
}

volatile int hos_prompts_row = -1;   /* nn_hid.c's live input ("!prompts <row>"): over the menu's row, -1 none */

/* the face the Button prompts row chooses now: Auto, the first controller's (none: the Switch's, as at the start) */
static int live_face(void) {
    const int choice = hos_prompts_row >= 0 ? hos_prompts_row : rx_option(HOS_OPT_PROMPTS, -1);
    if (choice >= 1 && choice <= HOS_PROMPT_N) return choice - 1;
    if (choice < 0) return live_face_now;   /* (no menu: the start's) */
    char name[128] = "";
    hos_sdl_pad_first_name(name, sizeof name);
    return auto_face(name);
}

/* nn_hid.c, as the game reads its controller (every frame; looked at every 100 ms): the face or the controls changed, or a
   part found since that still shows what was before? */
int hos_prompts_live_parts(void);
void hos_prompts_live(void) {
    static volatile LONG busy;
    static int64_t last;
    const int pool = __atomic_load_n(&tex.l.base, __ATOMIC_ACQUIRE) != 0;   /* (hos_prompts_pool: lo and hi set before) */
    if (live_face_now < 0 || (!pool && !lay_n && !font.at) || InterlockedCompareExchange(&busy, 1, 0)) return;
    const int64_t now = hos_now_ns();
    if (now - last >= 100000000) {
        last = now;
        find_layouts();
        int bind[BINDS];
        current_binds(bind);
        const int face = live_face();
        if ((hos_prompts_live_parts() & HOS_PROMPTS_LIVE_ALL) != HOS_PROMPTS_LIVE_ALL) {   /* (the menu asks for a restart then:
            nothing changes before, no part on another face than the rest) */
            InterlockedExchange(&busy, 0);
            return;
        }
        live_face_now = face;
        memcpy(live_bind_now, bind, sizeof live_bind_now);
        const int found[PARTS] = {0, lay.base != 0, font.at != 0};   /* (the textures: looked at under tex_lock) */
        char dir[MAX_PATH];
        int start, ready = -1;
        if (tex.l.base && (part_face[PART_TEXTURES] != face || memcmp(part_bind[PART_TEXTURES], bind, sizeof bind)) &&
            (ready = setup(dir, sizeof dir, &start)))
            texture_bin(dir, face);   /* (read before tex_lock) */
        for (int p = 0; p < PARTS; p++) {
            if (p == PART_TEXTURES) AcquireSRWLockExclusive(&tex_lock);   /* (the pool kept, or let go, till its writes end) */
            const int here = p == PART_TEXTURES ? tex.l.base != 0 : found[p];
            if (here && (part_face[p] != face || memcmp(part_bind[p], bind, sizeof bind))) {
                if (ready < 0) ready = setup(dir, sizeof dir, &start);
                if (ready && p == PART_TEXTURES) live_textures(dir, face, bind);
                if (ready && p == PART_LAYOUTS) live_layouts(dir, face, bind);
                if (ready && p == PART_FONT) live_font(dir, face);
                part_face[p] = face;   /* (also when it failed: not tried again until the next change) */
                memcpy(part_bind[p], bind, sizeof part_bind[p]);
            }
            if (p == PART_TEXTURES) ReleaseSRWLockExclusive(&tex_lock);
        }
    }
    InterlockedExchange(&busy, 0);
}

/* hos_prompts_romfs: the font, the textures, the layouts: 0 not built, 1 being built, 2 built, 3 none (a build without
   them, a mod's own file, a mod's size table that cannot take ours: the same whatever the face) */
static volatile LONG romfs_state[3];

/* what the Insert menu's Button prompts row can change at once (vk_api.h HOS_PROMPTS_LIVE_*): the parts the game has loaded
   from files served here, found, and the ones none was served for (a restart changes them no more); 0 with none found */
int hos_prompts_live_parts(void) {
    const int found = (tex.l.base ? HOS_PROMPTS_LIVE_TEXTURES : 0) | (lay.base ? HOS_PROMPTS_LIVE_LAYOUTS : 0) |
                      (font.at ? HOS_PROMPTS_LIVE_TEXTS : 0);
    if (!found) return 0;
    return found | (romfs_state[1] == 3 ? HOS_PROMPTS_LIVE_TEXTURES : 0) | (romfs_state[2] == 3 ? HOS_PROMPTS_LIVE_LAYOUTS : 0) |
           (romfs_state[0] == 3 ? HOS_PROMPTS_LIVE_TEXTS : 0);
}

/* the face the game shows (faces[]), -1 none: the Insert menu names the pad inputs as that controller does. -1 too
   until the game has asked for a prompt file: the menu asks from the present thread, and choose_face can wait for SDL */
int hos_prompts_face(void) {
    char dir[MAX_PATH];
    int face;
    if (live_face_now >= 0) return live_face_now;
    return setup_state >= 2 && setup(dir, sizeof dir, &face) ? face : -1;
}

/* the Talk ring's place for each of A B X Y (0 east, 1 south, 2 north, 3 west: nx2/prompt_assets.py RING_PLACES): where its pad
   input is among the face buttons, else its own place on the Switch when free, else the first one free */
static void ring_places(const int *bind, int place[4]) {
    static const int of_pad[4] = {1, 0, 3, 2};   /* (HOS_PAD_SOUTH, EAST, WEST, NORTH) */
    int taken[4] = {0};
    for (int s = 0; s < 4; s++) {
        const int p = bind[s] >= 0 && bind[s] < 4 ? of_pad[bind[s]] : -1;
        place[s] = p >= 0 && !taken[p] ? p : -1;
        if (place[s] >= 0) taken[place[s]] = 1;
    }
    for (int s = 0; s < 4; s++) {
        if (place[s] >= 0) continue;
        int p = s;   /* (A B X Y's own places are 0 1 2 3) */
        if (taken[p])
            for (p = 0; taken[p]; p++) {}
        place[s] = p;
        taken[p] = 1;
    }
}

/* prompts/Common.blarc.zs: the game's layouts (prompts/Common.blarc) with
   - the Talk ring's A B X Y where their pad inputs are (prompts/ring.bin, nx2/prompt_assets.py: "HPRG", u32 count; each: u8
     group, u8 Switch button placed, u8 place, u8, u32 runs, the runs);
   - the ocarina hint's icons moved apart for the wide ones bound (prompts/<face>/hint.bin, nx2/prompt_assets.py HINTS: "HPHN",
     u32 count of pad inputs, each one's icon's extra width (f32), u32 panes; each: u32 offset, u8 slot, 3 bytes, f32 x): each
     icon the one before's extra further on, the last one where it is;
   the game's own bytes when both are as the game has them (served all the same: a change while it runs is written where
   the game keeps it, find_layouts) */
static int build_layouts(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    int bind[BINDS];
    current_binds(bind);
    Layout o;
    if (!layout_runs(dir, face, bind, &o)) return 0;
    char keypath[MAX_PATH];
    snprintf(out, cap, "%sprompts/Common.blarc.zs", dir);
    snprintf(path, sizeof path, "%sprompts/Common.blarc", dir);
    snprintf(keypath, sizeof keypath, "%sprompts/Common.blarc.key", dir);
    FILE *f = fopen(path, "rb");
    long n = 0;
    const int headed = f && fread(lay_head, 1, sizeof lay_head, f) == sizeof lay_head && !in_runs(0, sizeof lay_head, o.runs, o.n) &&
                       !fseek(f, 0, SEEK_END) && (n = ftell(f)) > 0;
    if (f) fclose(f);
    const int ok = write_patched(path, out, keypath, o.h, o.runs, o.n);
    hos_log("prompts: the Talk ring's A B X Y at places %d %d %d %d, the ocarina hint %s%s", o.place[0], o.place[1],
            o.place[2], o.place[3], o.spread ? "spread" : "as is", ok ? "" : ": could not be written");
    if (ok) part_served(PART_LAYOUTS, face, bind);
    if (ok && headed) {   /* (for find_layouts: the file, and the runs served) */
        snprintf(lay.path, sizeof lay.path, "%s", path);
        lay.guarded = 1;
        lay_n = (uint64_t)n;
        lay_start = o;
    } else {
        layout_free(&o);
    }
    return ok;
}

/* the runs over the game's Common.blarc for this face and these controls (o: freed with layout_free); 0 without the file */
static int layout_runs(const char *dir, int face, const int *bind, Layout *o) {
    char path[MAX_PATH];
    memset(o, 0, sizeof *o);
    snprintf(path, sizeof path, "%sprompts/Common.blarc", dir);
    if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) return 0;
    int pads[HOS_SW_N];   /* (the keyboard face: no pad input, the Switch's places) */
    for (int b = 0; b < HOS_SW_N; b++) pads[b] = face == HOS_PROMPT_KEYBOARD ? -1 : bind[b];
    ring_places(pads, o->place);
    const int ring_moved = !(o->place[0] == 0 && o->place[1] == 1 && o->place[2] == 2 && o->place[3] == 3);
    size_t cap_runs = 64;
    o->runs = (Run *)malloc(cap_runs * sizeof *o->runs);
    o->h = fnv(0xcbf29ce484222325ull, o->place, sizeof o->place);
    long tn = 0, hn = 0;
    snprintf(path, sizeof path, "%sprompts/ring.bin", dir);
    uint8_t *t = o->ring = ring_moved ? read_all(path, "HPRG", &tn) : NULL;
    if (t) {
        const uint32_t count = rd32le(t + 4);
        size_t at = 8;
        for (uint32_t e = 0; e < count && at + 8 <= (size_t)tn; e++) {
            const int placed = t[at + 1], where = t[at + 2];
            const uint32_t nr = rd32le(t + at + 4);
            const int on = placed < 4 && o->place[placed] == where;
            at += 8;
            for (uint32_t r = 0; r < nr && at + 8 <= (size_t)tn; r++) {
                const uint32_t off = rd32le(t + at), len = rd32le(t + at + 4);
                if (len > (size_t)tn - at - 8) break;
                if (on) add_run(&o->runs, &o->n, &cap_runs, off, len, t + at + 8);
                at += 8 + len;
            }
        }
        o->h = fnv(o->h, t, (size_t)tn);
    }
    /* the hint's notes, in its order (Switch L R Y X A: up, down, left, right, A) */
    static const int notes[5] = {HOS_SW_L, HOS_SW_R, HOS_SW_Y, HOS_SW_X, HOS_SW_A};
    snprintf(path, sizeof path, "%sprompts/%s/hint.bin", dir, faces[face]);
    uint8_t *hint = o->hint = read_all(path, "HPHN", &hn);
    if (hint) {
        const uint32_t npads = rd32le(hint + 4);
        const size_t panes_at = 8 + 4 * (size_t)npads;
        float extra[5], shift[6] = {0};
        for (int i = 0; i < 5; i++) {
            const int p = face == HOS_PROMPT_KEYBOARD ? bind[HOS_SW_N + notes[i]] : bind[notes[i]];   /* (keyboard: by key) */
            extra[i] = 0;
            if (p >= 0 && (uint32_t)p < npads && panes_at <= (size_t)hn) memcpy(&extra[i], hint + 8 + 4 * p, 4);
        }
        for (int i = 3; i >= 0; i--) shift[i] = shift[i + 1] - extra[i];   /* (an icon is drawn from its pane's left) */
        shift[5] = shift[0];   /* (the text with the first icon) */
        o->spread = shift[0] != 0;
        const uint32_t count = panes_at + 4 <= (size_t)hn ? rd32le(hint + panes_at) : 0;
        if (o->spread && count <= ((size_t)hn - panes_at - 4) / 12) {
            o->xs = (float *)malloc((count ? count : 1) * sizeof *o->xs);
            for (uint32_t k = 0; k < count; k++) {
                const uint8_t *e = hint + panes_at + 4 + 12 * k;
                memcpy(&o->xs[k], e + 8, 4);
                o->xs[k] += shift[e[4] < 6 ? e[4] : 5];
                add_run(&o->runs, &o->n, &cap_runs, rd32le(e), 4, (const uint8_t *)&o->xs[k]);
            }
            o->h = fnv(fnv(o->h, shift, sizeof shift), hint, (size_t)hn);
        } else {
            o->spread = 0;
        }
    }
    return 1;
}

/* The game's table name, as nx2/prompt_assets.py selects it for the prompts. A mod can
   include other versions' tables too; those must not seed the one-time merge. */
static int game_table(char *out, size_t cap) {
    char pattern[MAX_PATH], name[MAX_PATH] = "";
    if (!hos_romfs_dir) return 0;
    snprintf(pattern, sizeof pattern, "%s/System/Resource/ResourceSizeTable.*.rsizetable.zs", hos_romfs_dir);
    WIN32_FIND_DATAA fd;
    HANDLE f = FindFirstFileA(pattern, &fd);
    if (f == INVALID_HANDLE_VALUE) return 0;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && (!name[0] || strcmp(fd.cFileName, name) < 0))
            snprintf(name, sizeof name, "%s", fd.cFileName);
    } while (FindNextFileA(f, &fd));
    FindClose(f);
    if (!name[0]) return 0;
    snprintf(out, cap, "System/Resource/%s", name);
    return 1;
}

/* builds prompts/External.bfotf.zs for this run; 1 when there is one */
static int build(char *out, size_t cap) {
    char dir[MAX_PATH], path[MAX_PATH];
    int face;
    if (!setup(dir, sizeof dir, &face)) return 0;
    /* A mod's size table is served with ours merged in. Check the same table even when the font
       is opened first; an unmergeable one cannot hold the font, which is larger than the game's. */
    char table[MAX_PATH], mod[MAX_PATH];
    if (game_table(table, sizeof table) && hos_mods_file(table, mod, sizeof mod) && sizes(dir, mod, path, sizeof path) < 0) {
        hos_log("prompts: a mod's resource size table cannot take the prompts' sizes: the game's own icons in texts");
        return 0;
    }
    long n;
    uint32_t key;
    int bound;
    uint8_t *b = font_bound(dir, face, &n, &key, &bound);
    if (!b) return 0;
    if (n >= 8 + 64) memcpy(font.head, b + 8, sizeof font.head);
    for (long i = 8; i < n; i += 4) wr32le(b + i, rd32le(b + i) ^ key);
    snprintf(out, cap, "%sprompts/External.bfotf.zs", dir);
    const int ok = write_zstd_raw(out, b, (size_t)n);
    if (ok) {
        int bind[BINDS];
        current_binds(bind);
        part_served(PART_FONT, face, bind);
        font.n = n >= 8 + 64 ? (uint32_t)n : 0;
    }
    free(b);
    hos_log("prompts: %s icons, %d buttons on these %s%s", faces[face], bound, face == HOS_PROMPT_KEYBOARD ? "keys" : "pad's controls",
            ok ? "" : ": could not be written");
    return ok;
}

/* mods.c: the file for a RomFS path the prompts replace (key: lower case, '/'), 1 with one. mod: a mod's file for it, or NULL.
   A mod's own wins, but for the size table: that gets our entries merged in (sizes) */
int hos_prompts_romfs(const char *key, const char *mod, char *out, size_t cap) {
    volatile LONG *const state = romfs_state;
    static char path[3][MAX_PATH];
    const size_t kn = strlen(key);
    if (!strncmp(key, SIZES_PATH, sizeof SIZES_PATH - 1) && kn > 14 && !strcmp(key + kn - 14, ".rsizetable.zs")) {
        char dir[MAX_PATH];   /* (the game's size table with room for the font, larger than its: nx2/prompt_assets.py) */
        int face;
        if (!setup(dir, sizeof dir, &face)) return 0;
        if (mod) return sizes(dir, mod, out, cap) > 0;
        snprintf(out, cap, "%sprompts/rsizetable.zs", dir);
        return GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES;
    }
    const int which = !strcmp(key, FONT_PATH) ? 0 : !strcmp(key, TEXTURES_PATH) ? 1 : !strcmp(key, LAYOUTS_PATH) ? 2 : -1;
    if (which < 0) return 0;
    if (mod) {   /* (the mod's file: the same whatever the face, as one none was served for) */
        InterlockedCompareExchange(&state[which], 3, 0);
        return 0;
    }
    static int (*const builders[3])(char *, size_t) = {build, build_textures, build_layouts};
    if (InterlockedCompareExchange(&state[which], 1, 0) == 0)
        InterlockedExchange(&state[which], builders[which](path[which], MAX_PATH) ? 2 : 3);
    while (state[which] == 1) Sleep(1);
    if (state[which] != 2) return 0;
    snprintf(out, cap, "%s", path[which]);
    return 1;
}
