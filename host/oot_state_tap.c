/* (hookshot) OoT's read-only samples for the control bridge (docs/control.md).
   status.game holds the last snapshot; subscribe events:["oot"] samples every n presents, with an optional watch list
   and changed flags. There is no file channel. HOS_CONTROL unset costs one early return. With the bridge on, a cheap
   atomic deadline skips capture unless a status request is pending or a subscriber without a pending sample is due.

   The guest reads stay in hos_oot_lift_player, under cheats_lock: Link's update is the point at which the actor,
   managers and camera are used by the game, not pointers kept for a later socket-thread read. The lock serializes
   successive guest workers and debug tools; it does not freeze every game thread, so every individual read is also
   range-checked and missing/non-finite values are null. state_lock hands only copied JSON to the bridge. No lock is
   held over socket I/O. Samples stop when Link is not updated (title, pause, load); their frame and t expose staleness.

   pos is in metres; facing and camera in degrees, 0 toward +z and 90 toward +x; life/max_life in quarter hearts.
   A watched Enum is "#" followed by the hash of its value's name. Changed Bool/Int/Enum/UInt flags are keyed by hash;
   the first sample establishes a baseline, at most 32 changes are sent, and more counts any left out.

   GameDataMgr's typed containers have their count at +8, entries at +16, and a {u32 hash,u32 entry} table at +24,
   probed from hash % size (+36), as host/hos_main.c's debug tools use them. Both lookup and scan cap the table size. */
#include "hos.h"
#include "control.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <windows.h>   /* (LONG64, InterlockedCompareExchange64; on Linux host/posix's) */

extern volatile LONG64 hos_present_count;   /* (host/nvn_native.c: the game's presents) */

#define STATE_GAMEDATA_SLOT 0x84c9e180ULL   /* GameDataMgr's holder (host/hos_main.c OOT_GAMEDATA_SLOT) */
#define STATE_EVENTMGR_SLOT 0x84c9e090ULL   /* EventMgr's (OOT_EVENTMGR_SLOT) */
#define STATE_CAMERAS_SLOT 0x84ca6bc0ULL    /* the camera manager's (host/hos_main.c oot_gameplay_view) */
#define STATE_VIEW_VTABLE 0x84c18a88ULL     /* a camera's view (hos_oot_free_view) */
#define STATE_LINK_POS 0x31c                /* Link's position in the actor hos_oot_lift_player is given */

#define STATE_MAP 0x1d6189dau        /* Sequence_CurrentBanc (String64) */
#define STATE_LOADING 0xef2b60c2u    /* Sequence_IsLoadingBanc (Bool) */
#define STATE_CHILD 0xcea220d3u      /* Sequence_IsChildhood (Bool) */
#define STATE_ROT 0xa4aee8cdu        /* Player_Rot_For_Map (Float) */
#define STATE_LIFE 0x31ab5580u       /* PlayerStatus.Life (Int) */
#define STATE_MAX_LIFE 0xfbe01da1u   /* PlayerStatus.MaxLife (Int) */
#define STATE_RUPEES 0xa77921d7u     /* PlayerStatus.CurrentRupee (Int) */
#define STATE_PROMPT 0x9824206cu     /* the A button's action (String64; its name is not known) */
#define STATE_MUSIC 0xcad3ffe5u      /* the music playing (String64; its name is not known) */

enum { KIND_BOOL, KIND_INT, KIND_FLOAT, KIND_ENUM, KIND_TEXT, KIND_UINT, KINDS };
/* each container's place in GameDataMgr, the size of its entries and where an entry keeps its value (a String64's: a
   pointer to the text) */
static const struct { uint32_t at, stride, value; } state_kind[KINDS] = {
    {0x008, 0x18, 0x16}, {0x0c8, 0x28, 0x1c}, {0x188, 0x28, 0x1c}, {0x248, 0x38, 0x1c}, {0x608, 0x108, 0x68}, {0x788, 0x28, 0x1c}};

/* n guest bytes at a into dst: 0 for a null pointer's page or anything outside the guest window */
static int state_peek(uint64_t a, void *dst, size_t n) {
    const void *p = a >= 0x100000 ? hos_guest_range(a, n) : NULL;
    if (!p) return 0;
    memcpy(dst, p, n);
    return 1;
}
static uint64_t state_u64(uint64_t a) {
    uint64_t v = 0;
    return state_peek(a, &v, 8) ? v : 0;
}
static uint32_t state_u32(uint64_t a) {
    uint32_t v = 0;
    return state_peek(a, &v, 4) ? v : 0;
}

/* MurmurHash3 x86 32 with seed 0: the hash the game names its flags by */
static uint32_t state_murmur(const char *s, size_t n) {
    uint32_t h = 0, k;
    size_t i = 0;
    for (; i + 4 <= n; i += 4) {
        k = (uint32_t)(unsigned char)s[i] | (uint32_t)(unsigned char)s[i + 1] << 8 | (uint32_t)(unsigned char)s[i + 2] << 16 |
            (uint32_t)(unsigned char)s[i + 3] << 24;
        k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u;
        h ^= k, h = h << 13 | h >> 19, h = h * 5 + 0xe6546b64u;
    }
    k = 0;
    switch (n & 3) {
    case 3: k ^= (uint32_t)(unsigned char)s[i + 2] << 16; /* fall through */
    case 2: k ^= (uint32_t)(unsigned char)s[i + 1] << 8; /* fall through */
    case 1: k ^= (uint32_t)(unsigned char)s[i]; k *= 0xcc9e2d51u, k = k << 15 | k >> 17, k *= 0x1b873593u, h ^= k;
    }
    h ^= (uint32_t)n;
    h ^= h >> 16, h *= 0x85ebca6bu, h ^= h >> 13, h *= 0xc2b2ae35u, h ^= h >> 16;
    return h;
}

/* The entry of the flag hash in container kind of GameDataMgr mgr, 0 when it has none */
static uint64_t state_entry(uint64_t mgr, int kind, uint32_t hash) {
    const uint64_t box = mgr + state_kind[kind].at, entries = state_u64(box + 16), table = state_u64(box + 24);
    const uint32_t count = state_u32(box + 8), size = state_u32(box + 36);
    if (!mgr || !entries || !table || !size || size > (1u << 22) || !hash) return 0;
    for (uint32_t probe = 0, slot = hash % size; probe < size; probe++, slot = slot + 1 == size ? 0 : slot + 1) {
        uint32_t pair[2];
        if (!state_peek(table + slot * 8ULL, pair, sizeof pair) || !pair[0]) return 0;
        if (pair[0] == hash) return pair[1] < count ? entries + (uint64_t)pair[1] * state_kind[kind].stride : 0;
    }
    return 0;
}

/* ---- the line */

typedef struct {
    /* 64 watched strings, each at most 63 bytes escaped to six JSON bytes, plus names, core fields and 32 changes. */
    char text[HOS_CONTROL_OOT_JSON];
    size_t n;
} StateLine;

static void state_put(StateLine *l, const char *fmt, ...) {
    if (l->n + 1 >= sizeof l->text) return;
    va_list a;
    va_start(a, fmt);
    const int k = vsnprintf(l->text + l->n, sizeof l->text - l->n, fmt, a);
    va_end(a);
    if (k > 0) l->n = l->n + (size_t)k < sizeof l->text ? l->n + (size_t)k : sizeof l->text - 1;
}

/* Preserve valid UTF-8; invalid/truncated sequences become U+FFFD, never Latin-1 byte escapes. */
static void state_put_text(StateLine *l, const char *s) {
    state_put(l, "\"");
    while (*s) {
        const unsigned char ch = (unsigned char)*s++;
        if (ch == '"' || ch == '\\') state_put(l, "\\%c", ch);
        else if (ch < 0x20) state_put(l, "\\u%04x", ch);
        else if (ch < 0x80) state_put(l, "%c", ch);
        else {
            unsigned cp = ch & (ch < 0xe0 ? 31 : ch < 0xf0 ? 15 : 7);
            const int n = ch >= 0xc2 && ch <= 0xdf ? 1 : ch >= 0xe0 && ch <= 0xef ? 2 :
                          ch >= 0xf0 && ch <= 0xf4 ? 3 : 0;
            int i = 0;
            for (; i < n && ((unsigned char)s[i] & 0xc0) == 0x80; i++) cp = (cp << 6) | (s[i] & 63);
            if (!n || i != n || cp < (n == 1 ? 0x80u : n == 2 ? 0x800u : 0x10000u) ||
                cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) state_put(l, "\\ufffd");
            else { state_put(l, "%.*s", n + 1, s - 1); s += n; }
        }
    }
    state_put(l, "\"");
}

/* A String64 flag's text into buf (64 bytes), 0 when it cannot be read */
static int state_text(uint64_t mgr, uint32_t hash, char buf[64]) {
    const uint64_t e = state_entry(mgr, KIND_TEXT, hash), p = e ? state_u64(e + state_kind[KIND_TEXT].value) : 0;
    buf[0] = 0;
    if (!p || !state_peek(p, buf, 64)) return 0;
    buf[63] = 0;
    return 1;
}

/* A number flag (Bool, Int, Enum or UInt) into *v, 0 when it has none of these */
static int state_number(uint64_t mgr, int kind, uint32_t hash, int64_t *v) {
    const uint64_t e = state_entry(mgr, kind, hash), at = e + state_kind[kind].value;
    uint8_t b;
    uint32_t u;
    if (!e) return 0;
    if (kind == KIND_BOOL) return state_peek(at, &b, 1) && (*v = b, 1);
    if (!state_peek(at, &u, 4)) return 0;
    *v = kind == KIND_INT ? (int64_t)(int32_t)u : (int64_t)u;
    return 1;
}

static int state_float(uint64_t mgr, uint32_t hash, float *f) {
    const uint64_t e = state_entry(mgr, KIND_FLOAT, hash);
    return e && state_peek(e + state_kind[KIND_FLOAT].value, f, 4) && isfinite(*f);
}

/* ,"<key>":<number flag> or null */
static void state_put_number(StateLine *l, uint64_t mgr, const char *key, int kind, uint32_t hash) {
    int64_t v;
    if (state_number(mgr, kind, hash, &v)) state_put(l, ",\"%s\":%lld", key, (long long)v);
    else state_put(l, ",\"%s\":null", key);
}

/* ,"<key>":"<String64 flag>" or null */
static void state_put_flag_text(StateLine *l, uint64_t mgr, const char *key, uint32_t hash) {
    char t[64];
    state_put(l, ",\"%s\":", key);
    if (state_text(mgr, hash, t)) state_put_text(l, t);
    else state_put(l, "null");
}

/* a flag's value in whichever container has it: a number, a text, an Enum's value as "#<hash>", null */
static void state_put_any(StateLine *l, uint64_t mgr, uint32_t hash) {
    int64_t v;
    float f;
    char t[64];
    if (state_number(mgr, KIND_BOOL, hash, &v) || state_number(mgr, KIND_INT, hash, &v)) state_put(l, "%lld", (long long)v);
    else if (state_float(mgr, hash, &f)) state_put(l, "%.3f", f);
    else if (state_number(mgr, KIND_ENUM, hash, &v)) state_put(l, "\"#%08x\"", (unsigned)v);
    else if (state_text(mgr, hash, t)) state_put_text(l, t);
    else if (state_number(mgr, KIND_UINT, hash, &v)) state_put(l, "%lld", (long long)v);
    else state_put(l, "null");
}

/* An angle in degrees in [-180, 180) */
static double state_wrap(double d) {
    d = fmod(d + 180.0, 360.0);
    return (d < 0 ? d + 360.0 : d) - 180.0;
}

/* The gameplay camera's yaw (degrees, 0 toward +z, 90 toward +x), 0 when it cannot be read */
static int state_camera(double *yaw) {
    const uint64_t manager = state_u64(state_u64(STATE_CAMERAS_SLOT));
    const uint64_t cameras = manager && state_u32(manager + 32) ? state_u64(manager + 40) : 0;
    const uint64_t camera = cameras ? state_u64(cameras) : 0, view = camera ? state_u64(camera + 120) : 0;
    double eye[3], at[3];
    if (!view || state_u64(view) != STATE_VIEW_VTABLE || !state_peek(view + 96, eye, sizeof eye) ||
        !state_peek(view + 120, at, sizeof at))
        return 0;
    const double x = at[0] - eye[0], z = at[2] - eye[2];
    if (!(x * x + z * z > 1e-9) || !isfinite(x) || !isfinite(z)) return 0;
    *yaw = atan2(x, z) * 57.29577951308232;
    return 1;
}

/* ---- shared, bounded changes scan (only Link's serialized update touches these baselines) ---- */

static const int state_tracked[] = {KIND_BOOL, KIND_INT, KIND_ENUM, KIND_UINT};
#define STATE_TRACKED (int)(sizeof state_tracked / sizeof state_tracked[0])
/* Smaller slices reduce work per present but delay detection. Judge changes by measured frame cost and the
   full-pass delay reported below, not the slot budget alone. */
#define STATE_SCAN_SLOTS 4096u
typedef struct { uint32_t key, value; int kind; } StateChange;
typedef struct { StateChange item[32]; unsigned n, more; } StateChanges;
typedef struct { uint64_t entries, table; uint32_t count, size; } StateTable;
typedef struct {
    StateTable table;
    struct StatePair { uint32_t key, value; } *slot;
    uint32_t capacity, cursor;
    int valid;
    StateChanges changes;
} StateSeen;
static StateSeen state_seen[STATE_TRACKED];
static int state_scan_kind;

static void state_log_scan(void) {
    static uint32_t reported[STATE_TRACKED];
    uint32_t sizes[STATE_TRACKED], total = 0;
    for (int t = 0; t < STATE_TRACKED; t++) {
        if (!state_seen[t].valid) return;
        sizes[t] = state_seen[t].table.size;
        total += sizes[t];
    }
    if (!memcmp(reported, sizes, sizeof sizes)) return;
    memcpy(reported, sizes, sizeof sizes);
    hos_log("control: oot changes scan: Bool=%u Int=%u Enum=%u UInt=%u slots=%u slice=%u presents=%u",
            sizes[0], sizes[1], sizes[2], sizes[3], total, STATE_SCAN_SLOTS,
            (total + STATE_SCAN_SLOTS - 1) / STATE_SCAN_SLOTS);
}

static void state_change(StateChanges *to, StateChange value) {
    for (unsigned i = 0; i < to->n; i++)
        if (to->item[i].key == value.key) { to->item[i] = value; return; }
    if (to->n < 32) to->item[to->n++] = value;
    else if (to->more != UINT32_MAX) to->more++;
}

static void state_merge(StateChanges *to, const StateChanges *from) {
    for (unsigned i = 0; i < from->n; i++) state_change(to, from->item[i]);
    to->more = UINT32_MAX - to->more < from->more ? UINT32_MAX : to->more + from->more;
}

static int state_table(uint64_t mgr, int kind, StateTable *h) {
    const uint64_t box = mgr + state_kind[kind].at;
    if (!mgr || !state_peek(box + 16, &h->entries, 8) || !state_peek(box + 24, &h->table, 8) ||
        !state_peek(box + 8, &h->count, 4) || !state_peek(box + 36, &h->size, 4)) return 0;
    return h->entries >= 0x100000 && h->table >= 0x100000 && h->size && h->size <= (1u << 22) &&
        h->count <= h->size && hos_guest_range(h->table, (uint64_t)h->size * 8) &&
        (!h->count || hos_guest_range(h->entries, (uint64_t)h->count * state_kind[kind].stride));
}

static int state_same_table(const StateTable *a, const StateTable *b) {
    return a->entries == b->entries && a->table == b->table && a->size == b->size && a->count == b->count;
}

/* At most one slice per present, shared by every watch. No bridge lock, and no allocation unless size changes.
   A changed/unreadable header or slot discards that table's unfinished diff and establishes a fresh baseline.
   The checked local size bounds allocation even if the guest changes its header during a save load. */
static int state_scan(uint64_t mgr, StateChanges *out) {
    unsigned budget = STATE_SCAN_SLOTS;
    for (; state_scan_kind < STATE_TRACKED && budget; state_scan_kind++) {
        const int kind = state_tracked[state_scan_kind];
        StateSeen *seen = &state_seen[state_scan_kind];
        StateTable h, after;
        if (!state_table(mgr, kind, &h) || !state_table(mgr, kind, &after) || !state_same_table(&h, &after))
            goto invalid;
        if (!state_same_table(&seen->table, &h)) {
            seen->valid = 0, seen->cursor = 0, seen->changes = (StateChanges){0};
            if (seen->capacity != h.size) {
                struct StatePair *p = realloc(seen->slot, (size_t)h.size * sizeof *p);
                if (!p) goto invalid;
                seen->slot = p, seen->capacity = h.size;
            }
            seen->table = h;
        }
        while (seen->cursor < h.size && budget) {
            const uint32_t slot = seen->cursor++;
            uint32_t pair[2], again[2], v = 0;
            uint8_t b;
            budget--;
            if (!state_peek(h.table + slot * 8ULL, pair, sizeof pair)) goto invalid;
            if (pair[0]) {
                if (pair[1] >= h.count) goto invalid;
                const uint64_t at = h.entries + (uint64_t)pair[1] * state_kind[kind].stride + state_kind[kind].value;
                if (kind == KIND_BOOL) {
                    if (!state_peek(at, &b, 1)) goto invalid;
                    v = b;
                } else if (!state_peek(at, &v, 4)) goto invalid;
                if (!state_peek(h.table + slot * 8ULL, again, sizeof again) || memcmp(pair, again, sizeof pair))
                    goto invalid;
            }
            if (seen->valid && pair[0] && seen->slot[slot].key == pair[0] && seen->slot[slot].value != v)
                state_change(&seen->changes, (StateChange){pair[0], v, kind});
            seen->slot[slot] = (struct StatePair){pair[0], v};
        }
        if (!state_table(mgr, kind, &after) || !state_same_table(&h, &after)) goto invalid;
        state_merge(out, &seen->changes);
        seen->changes = (StateChanges){0};
        if (seen->cursor < h.size) return 1;
        seen->valid = 1, seen->cursor = 0;
        continue;
    invalid:
        seen->valid = 0, seen->cursor = 0, seen->changes = (StateChanges){0};
    }
    if (state_scan_kind < STATE_TRACKED) return 1;
    state_scan_kind = 0;
    state_log_scan();
    return 0;
}

static void state_put_changes(StateLine *l, const StateChanges *changes) {
    state_put(l, ",\"changed\":{");
    for (unsigned i = 0; i < changes->n; i++) {
        const StateChange *c = &changes->item[i];
        if (c->kind == KIND_INT) state_put(l, "%s\"0x%08x\":%d", i ? "," : "", c->key, (int)(int32_t)c->value);
        else if (c->kind == KIND_ENUM) state_put(l, "%s\"0x%08x\":\"#%08x\"", i ? "," : "", c->key, c->value);
        else state_put(l, "%s\"0x%08x\":%u", i ? "," : "", c->key, c->value);
    }
    state_put(l, "}");
    if (changes->more) state_put(l, ",\"more\":%u", changes->more);
}

static SRWLOCK state_lock = SRWLOCK_INIT;
static StateLine state_latest;
static struct {
    HosControlOotWatch watch;
    uint32_t hashes[HOS_CONTROL_OOT_FLAGS];
    StateChanges changes;
    uint64_t generation;
    StateLine line;
    int active, ready;
    int64_t last;
} state_sub[HOS_CONTROL_OOT_CLIENTS];

/* state_lock protects requests and watches. The game thread reads only this deadline when no work is due. */
static int64_t state_due = INT64_MAX, state_last = -1;
static uint64_t state_serial, state_requested, state_published;
static unsigned state_waiters;
static uint64_t state_watch_serial;
static int state_scanning;

static void state_schedule(void) {   /* state_lock held */
    int64_t due = state_waiters ? state_last + 1 : INT64_MAX;
    for (int i = 0; i < HOS_CONTROL_OOT_CLIENTS; i++) {
        if (!state_sub[i].active) continue;
        if (state_scanning && state_sub[i].watch.changes) due = state_last + 1;
        if (state_sub[i].ready) continue;
        int64_t next = state_sub[i].last < 0 ? 0 : state_sub[i].last + state_sub[i].watch.every;
        if (next <= state_last) next = state_last + 1;
        if (next < due) due = next;
    }
    __atomic_store_n(&state_due, due, __ATOMIC_RELEASE);
}

uint64_t hos_oot_sample_ask(void) {
    AcquireSRWLockExclusive(&state_lock);
    if (!state_waiters) state_requested = ++state_serial;
    state_waiters++;
    const uint64_t ticket = state_requested;
    state_schedule();
    ReleaseSRWLockExclusive(&state_lock);
    return ticket;
}

void hos_oot_sample_cancel(uint64_t ticket) {
    if (!ticket || __atomic_load_n(&state_published, __ATOMIC_ACQUIRE) >= ticket) return;
    AcquireSRWLockExclusive(&state_lock);
    if (state_waiters && ticket == state_requested) {
        state_waiters--;
        state_schedule();
    }
    ReleaseSRWLockExclusive(&state_lock);
}

int hos_oot_sample(uint64_t ticket, char *out, size_t cap) {
    if (ticket && __atomic_load_n(&state_published, __ATOMIC_ACQUIRE) < ticket) return 0;
    AcquireSRWLockShared(&state_lock);
    const int have = state_latest.n != 0 && state_latest.n < cap;
    if (have) memcpy(out, state_latest.text, state_latest.n + 1);
    ReleaseSRWLockShared(&state_lock);
    return have;
}

void hos_oot_watch(int slot, const HosControlOotWatch *watch) {
    if (slot < 0 || slot >= HOS_CONTROL_OOT_CLIENTS) return;
    AcquireSRWLockExclusive(&state_lock);
    memset(&state_sub[slot], 0, sizeof state_sub[slot]);
    state_sub[slot].generation = ++state_watch_serial;
    if (watch) {
        state_sub[slot].watch = *watch;
        state_sub[slot].active = 1;
        state_sub[slot].last = -1;
        for (int i = 0; i < watch->nflags; i++) {
            const char *name = watch->flags[i];   /* validated by the bridge */
            state_sub[slot].hashes[i] = name[0] == '0' && name[1] == 'x' ?
                (uint32_t)strtoul(name + 2, NULL, 16) : state_murmur(name, strlen(name));
        }
    }
    state_schedule();
    ReleaseSRWLockExclusive(&state_lock);
}

int hos_oot_poll(int slot, char *out, size_t cap) {
    if (slot < 0 || slot >= HOS_CONTROL_OOT_CLIENTS) return 0;
    AcquireSRWLockExclusive(&state_lock);
    const int have = state_sub[slot].ready && state_sub[slot].line.n < cap;
    if (have) {
        memcpy(out, state_sub[slot].line.text, state_sub[slot].line.n + 1);
        state_sub[slot].ready = 0;
        state_schedule();
    }
    ReleaseSRWLockExclusive(&state_lock);
    return have;
}

/* Each update of Link (actor: the one whose +0x31c is his position). Only this function reads guest memory. */
void hos_oot_state_tap(uint64_t actor) {
    if (!hos_control_on) return;
    const int64_t due = __atomic_load_n(&state_due, __ATOMIC_ACQUIRE);
    if (due == INT64_MAX) return;
    const int64_t frame = __atomic_load_n(&hos_present_count, __ATOMIC_RELAXED);
    if (frame < due) return;
    AcquireSRWLockExclusive(&state_lock);
    /* A cancellation/replacement can move the deadline while this worker waits for the lock. */
    if (frame < __atomic_load_n(&state_due, __ATOMIC_RELAXED)) {
        ReleaseSRWLockExclusive(&state_lock);
        return;
    }
    state_last = frame;
    const uint64_t requested = state_requested;
    /* Small immutable work descriptions; replacement/disconnect is checked again at publication. */
    static struct {
        HosControlOotWatch watch;
        uint32_t hashes[HOS_CONTROL_OOT_FLAGS];
        StateChanges changes;
        uint64_t generation;
        int active, due, started;
    } work[HOS_CONTROL_OOT_CLIENTS];
    int scan = 0, watching_changes = 0;
    for (int slot = 0; slot < HOS_CONTROL_OOT_CLIENTS; slot++) {
        work[slot].watch = state_sub[slot].watch;
        memcpy(work[slot].hashes, state_sub[slot].hashes, sizeof work[slot].hashes);
        work[slot].changes = state_sub[slot].changes;
        work[slot].generation = state_sub[slot].generation;
        work[slot].active = state_sub[slot].active;
        work[slot].started = state_sub[slot].last >= 0;
        work[slot].due = 0;
        const HosControlOotWatch *w = &work[slot].watch;
        if (!state_sub[slot].active) continue;
        watching_changes |= w->changes;
        scan |= state_scanning && w->changes;
        if (state_sub[slot].ready) continue;
        if (state_sub[slot].last >= 0 && frame - state_sub[slot].last < w->every) continue;
        work[slot].due = 1;
        scan |= w->changes;
    }
    ReleaseSRWLockExclusive(&state_lock);
    StateChanges changes = {0};
    const uint64_t mgr = state_u64(state_u64(STATE_GAMEDATA_SLOT));
    const int scanning = scan ? state_scan(mgr, &changes) : 0;
    if (!watching_changes) {
        state_scan_kind = 0;
        for (int t = 0; t < STATE_TRACKED; t++)
            state_seen[t].valid = 0, state_seen[t].cursor = 0, state_seen[t].changes = (StateChanges){0};
    }

    static StateLine line;
    StateLine *l = &line;
    l->n = 0;
    state_put(l, "{\"t\":%.3f,\"frame\":%lld", (double)hos_now_ns() / 1e9, (long long)frame);
    state_put_flag_text(l, mgr, "map", STATE_MAP);
    float p[3], rot;
    if (state_peek(actor + STATE_LINK_POS, p, sizeof p) && isfinite(p[0]) && isfinite(p[1]) && isfinite(p[2]))
        state_put(l, ",\"pos\":[%.2f,%.2f,%.2f]", p[0], p[1], p[2]);
    else state_put(l, ",\"pos\":null");
    if (state_float(mgr, STATE_ROT, &rot)) state_put(l, ",\"facing\":%.1f", state_wrap(rot + 180.0));
    else state_put(l, ",\"facing\":null");
    double yaw;
    if (state_camera(&yaw)) state_put(l, ",\"camera\":%.1f", yaw);
    else state_put(l, ",\"camera\":null");
    const uint64_t events = state_u64(state_u64(STATE_EVENTMGR_SLOT));
    uint64_t event;
    state_put(l, ",\"event\":%s", events && state_peek(events + 104, &event, sizeof event) ?
              (event ? "true" : "false") : "null");
    int64_t value;
    state_put(l, ",\"loading\":%s", state_number(mgr, KIND_BOOL, STATE_LOADING, &value) ?
              (value ? "true" : "false") : "null");
    state_put(l, ",\"age\":%s", state_number(mgr, KIND_BOOL, STATE_CHILD, &value) ?
              (value ? "\"child\"" : "\"adult\"") : "null");
    state_put_number(l, mgr, "life", KIND_INT, STATE_LIFE);
    state_put_number(l, mgr, "max_life", KIND_INT, STATE_MAX_LIFE);
    state_put_number(l, mgr, "rupees", KIND_INT, STATE_RUPEES);
    state_put_flag_text(l, mgr, "prompt", STATE_PROMPT);
    state_put_flag_text(l, mgr, "music", STATE_MUSIC);
    state_put(l, "}");

    AcquireSRWLockExclusive(&state_lock);
    state_latest.n = l->n;
    memcpy(state_latest.text, l->text, l->n + 1);
    state_scanning = scanning;
    if (state_waiters && state_requested == requested) {
        __atomic_store_n(&state_published, requested, __ATOMIC_RELEASE);
        state_waiters = 0;
    }
    ReleaseSRWLockExclusive(&state_lock);
    for (int slot = 0; slot < HOS_CONTROL_OOT_CLIENTS; slot++) {
        const HosControlOotWatch *w = &work[slot].watch;
        if (!work[slot].active) continue;
        if (w->changes && work[slot].started) state_merge(&work[slot].changes, &changes);
        static StateLine sample;
        StateLine *s = &sample;
        if (work[slot].due) {
            s->n = l->n - 1;   /* append optional fields inside the copied object */
            memcpy(s->text, l->text, s->n);
            if (w->nflags) {
                state_put(s, ",\"flags\":{");
                for (int i = 0; i < w->nflags; i++) {
                    state_put(s, "%s\"%s\":", i ? "," : "", w->flags[i]);
                    state_put_any(s, mgr, work[slot].hashes[i]);
                }
                state_put(s, "}");
            }
            if (w->changes) state_put_changes(s, &work[slot].changes);
            state_put(s, "}");
        }
        AcquireSRWLockExclusive(&state_lock);
        if (state_sub[slot].generation == work[slot].generation) {
            state_sub[slot].changes = work[slot].changes;
            if (work[slot].due) {
                state_sub[slot].line.n = s->n;
                memcpy(state_sub[slot].line.text, s->text, s->n + 1);
                state_sub[slot].changes = (StateChanges){0};
                state_sub[slot].last = frame;
                state_sub[slot].ready = 1;
            }
        }
        ReleaseSRWLockExclusive(&state_lock);
    }
    AcquireSRWLockExclusive(&state_lock);
    state_schedule();
    ReleaseSRWLockExclusive(&state_lock);
}
