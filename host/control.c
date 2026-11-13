/* (hookshot) The control bridge: HOS_CONTROL=<socket path> lets a tool on the same PC (a test, a script, a coding agent)
   drive the running game and read its state over a local socket, instead of a file the game polls (HOS_INPUT_LIVE) and its
   log scraped for the frame rate and draw counts. docs/control.md has the protocol; scripts/hookshot_ctl.py is a client.

   Off unless HOS_CONTROL is set. Linux: a Unix domain socket at that path, made 0600 (the umask around bind too), never
   over an existing file that is not a socket, and a client of another user is turned away (SO_PEERCRED). Windows: a named
   pipe (\\.\pipe\<name>; a name without the prefix gets it), its DACL the current user's alone, remote clients refused.
   There is no TCP. The guest's memory is read or written only with a second opt-in, HOS_CONTROL_MEM (ro, rw).

   Newline-delimited JSON: one request object a line ({"id": ..., "cmd": "status", ...}), one reply a line with the id
   given back, and the current frame (the game's presents so far) in every reply. A few clients at a time. A thread of its
   own does the socket's I/O and parses; what touches the game is applied where the game does it: presses in the
   controller's read (nn_hid.c read_input, hos_control_input), shots at a present on the renderer's thread
   (gpu/vk/vk_replay.cpp shot_ask), the Insert menu's settings through its own setter. The game never waits for the
   bridge: the controller's read takes one short lock that the bridge never holds over I/O.

   Commands: status, press, stick, sequence, release, shot, wait, option_get, option_set, saves, teleport and age (OoT),
   subscribe,
   unsubscribe, quit, and guest memory (mem_read, mem_read_many, mem_write) when HOS_CONTROL_MEM allows it.
   Inputs are timed in frames, from the first controller read after the request. The state (intro, menu, loading,
   play) is a guess from the draws a frame, as test scripts made it from the log: ~2 draws at the intro, 15-70 in menus,
   ~5 while loading, hundreds in play.

   tests/control_test.c builds this file with a fake game (HOS_CONTROL_NO_GAME) and drives it over a real socket. */
#include "control.h"

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <sddl.h>
#else
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>
#endif

#define CTL_LINE_MAX 65536            /* a request longer than this is refused (too_long) */
#define CTL_OUT_MAX (8u << 20)        /* a client that reads nothing while this much waits for it is let go */
#define CTL_CONNS 8
#define CTL_PENDING 64
#define CTL_INPUTS 256
#define CTL_LOG_RING 256
#define CTL_NODES 1024
#define CTL_MEM 8
#define CTL_MEM_RANGES 64
#define CTL_MEM_CAP (1u << 20)   /* 1 MB a request */
#define CTL_TICK_MS 4

volatile int hos_control_on;

/* ---- the platform: a lock, a thread, a clock ---- */
#ifdef _WIN32
typedef SRWLOCK ctl_mutex;
#define CTL_MUTEX_INIT SRWLOCK_INIT
static void ctl_lock(ctl_mutex *m) { AcquireSRWLockExclusive(m); }
static void ctl_unlock(ctl_mutex *m) { ReleaseSRWLockExclusive(m); }
static int64_t ctl_now_ns(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER c;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (int64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}
#else
typedef pthread_mutex_t ctl_mutex;
#define CTL_MUTEX_INIT PTHREAD_MUTEX_INITIALIZER
static void ctl_lock(ctl_mutex *m) { pthread_mutex_lock(m); }
static void ctl_unlock(ctl_mutex *m) { pthread_mutex_unlock(m); }
static int64_t ctl_now_ns(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}
#endif

/* ---- a growing text buffer, and JSON written into it ---- */
typedef struct { char *p; size_t n, cap; int failed; } Buf;

static void buf_put(Buf *b, const char *s, size_t n) {
    if (b->failed) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->n + n + 1 > cap) cap *= 2;
        char *p = (char *)realloc(b->p, cap);
        if (!p) { b->failed = 1; return; }
        b->p = p, b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void buf_puts(Buf *b, const char *s) { buf_put(b, s, strlen(s)); }
static void buf_printf(Buf *b, const char *fmt, ...) {
    char tmp[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof tmp, fmt, ap);
    va_end(ap);
    if (n > 0) buf_put(b, tmp, (size_t)n < sizeof tmp ? (size_t)n : sizeof tmp - 1);
}
static void buf_str(Buf *b, const char *s) {   /* a JSON string */
    buf_put(b, "\"", 1);
    for (const unsigned char *q = (const unsigned char *)s; *q; q++) {
        if (*q == '"' || *q == '\\') { char e[2] = {'\\', (char)*q}; buf_put(b, e, 2); }
        else if (*q == '\n') buf_put(b, "\\n", 2);
        else if (*q == '\r') buf_put(b, "\\r", 2);
        else if (*q == '\t') buf_put(b, "\\t", 2);
        else if (*q < 0x20) buf_printf(b, "\\u%04x", *q);
        else buf_put(b, (const char *)q, 1);
    }
    buf_put(b, "\"", 1);
}
static void buf_free(Buf *b) { free(b->p); memset(b, 0, sizeof *b); }

/* ---- JSON read: a request parsed into nodes; strings decoded into a copy of the line ---- */
enum { J_NULL, J_FALSE, J_TRUE, J_NUM, J_STR, J_ARR, J_OBJ };
typedef struct { int type; const char *key; const char *s; double num; int first, next; } JNode;
typedef struct {
    JNode n[CTL_NODES];
    int count;
    const char *p, *end;
    char *out;   /* the decoded strings (never longer than the text they come from) */
    const char *err;
} JDoc;

static void j_space(JDoc *d) { while (d->p < d->end && (*d->p == ' ' || *d->p == '\t' || *d->p == '\r' || *d->p == '\n')) d->p++; }
static const char *j_string(JDoc *d) {   /* at the opening quote: the decoded string, or NULL */
    char *start = d->out;
    d->p++;
    while (d->p < d->end && *d->p != '"') {
        unsigned char c = (unsigned char)*d->p++;
        if (c < 0x20) return d->err = "a control character in a string", NULL;
        if (c != '\\') { *d->out++ = (char)c; continue; }
        if (d->p >= d->end) break;
        c = (unsigned char)*d->p++;
        switch (c) {
        case '"': case '\\': case '/': *d->out++ = (char)c; break;
        case 'b': *d->out++ = '\b'; break;
        case 'f': *d->out++ = '\f'; break;
        case 'n': *d->out++ = '\n'; break;
        case 'r': *d->out++ = '\r'; break;
        case 't': *d->out++ = '\t'; break;
        case 'u': {
            if (d->end - d->p < 4) return d->err = "a short \\u escape", NULL;
            unsigned v = 0;
            for (int i = 0; i < 4; i++) {
                const char h = *d->p++;
                v = v * 16 + (unsigned)(h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 :
                                        h >= 'A' && h <= 'F' ? h - 'A' + 10 : 99);
                if (v > 0xFFFF) return d->err = "a bad \\u escape", NULL;
            }
            /* (UTF-8; a surrogate half stays as it is written, three bytes: the bridge has no use for such names) */
            if (v == 0) return d->err = "a NUL in a string", NULL;
            if (v < 0x80) *d->out++ = (char)v;
            else if (v < 0x800) *d->out++ = (char)(0xC0 | v >> 6), *d->out++ = (char)(0x80 | (v & 0x3F));
            else *d->out++ = (char)(0xE0 | v >> 12), *d->out++ = (char)(0x80 | (v >> 6 & 0x3F)), *d->out++ = (char)(0x80 | (v & 0x3F));
            break;
        }
        default: return d->err = "a bad escape in a string", NULL;
        }
    }
    if (d->p >= d->end) return d->err = "a string without its end", NULL;
    d->p++;
    *d->out++ = 0;
    return start;
}
static int j_value(JDoc *d, int depth) {
    j_space(d);
    if (d->p >= d->end) return d->err = "the text ends early", -1;
    if (depth > 32) return d->err = "nested too deep", -1;
    if (d->count >= CTL_NODES) return d->err = "too many values", -1;
    const int at = d->count++;
    JNode *n = &d->n[at];
    memset(n, 0, sizeof *n);
    n->first = n->next = -1;
    const char c = *d->p;
    if (c == '{' || c == '[') {
        n->type = c == '{' ? J_OBJ : J_ARR;
        d->p++;
        j_space(d);
        int last = -1;
        if (d->p < d->end && *d->p == (c == '{' ? '}' : ']')) { d->p++; return at; }
        for (;;) {
            const char *key = NULL;
            if (c == '{') {
                j_space(d);
                if (d->p >= d->end || *d->p != '"') return d->err = "an object key is not a string", -1;
                if (!(key = j_string(d))) return -1;
                j_space(d);
                if (d->p >= d->end || *d->p != ':') return d->err = "no ':' after a key", -1;
                d->p++;
            }
            const int v = j_value(d, depth + 1);
            if (v < 0) return -1;
            d->n[v].key = key;
            if (last < 0) d->n[at].first = v;
            else d->n[last].next = v;
            last = v;
            j_space(d);
            if (d->p < d->end && *d->p == ',') { d->p++; continue; }
            if (d->p < d->end && *d->p == (c == '{' ? '}' : ']')) { d->p++; return at; }
            return d->err = c == '{' ? "no ',' or '}' in an object" : "no ',' or ']' in an array", -1;
        }
    }
    if (c == '"') {
        n->type = J_STR;
        return (n->s = j_string(d)) ? at : -1;
    }
    if (c == '-' || (c >= '0' && c <= '9')) {
        char tmp[64];
        size_t len = 0;
        while (d->p + len < d->end && len < sizeof tmp - 1 && strchr("+-.eE0123456789", d->p[len])) len++;
        memcpy(tmp, d->p, len);
        tmp[len] = 0;
        char *e;
        n->num = strtod(tmp, &e);
        if (e != tmp + len || !isfinite(n->num)) return d->err = "a bad number", -1;
        n->type = J_NUM;
        d->p += len;
        return at;
    }
    static const struct { const char *word; int type; } words[] = {{"true", J_TRUE}, {"false", J_FALSE}, {"null", J_NULL}};
    for (int i = 0; i < 3; i++) {
        const size_t len = strlen(words[i].word);
        if ((size_t)(d->end - d->p) >= len && !memcmp(d->p, words[i].word, len)) {
            n->type = words[i].type;
            d->p += len;
            return at;
        }
    }
    return d->err = "not JSON", -1;
}
/* The line parsed into d (out: a buffer at least as long as the line). The root, or -1 with d->err */
static int j_parse(JDoc *d, const char *text, size_t len, char *out) {
    d->count = 0, d->p = text, d->end = text + len, d->out = out, d->err = NULL;
    const int root = j_value(d, 0);
    if (root < 0) return -1;
    j_space(d);
    if (d->p != d->end) return d->err = "text after the value", -1;
    return root;
}
static const JNode *j_get(const JDoc *d, int obj, const char *key) {
    if (obj < 0 || d->n[obj].type != J_OBJ) return NULL;
    for (int i = d->n[obj].first; i >= 0; i = d->n[i].next)
        if (!strcmp(d->n[i].key, key)) return &d->n[i];
    return NULL;
}
static int j_index(const JDoc *d, const JNode *n) { return (int)(n - d->n); }
/* a value written again as JSON (the request's id, given back as it came) */
static void j_write(Buf *b, const JDoc *d, int i) {
    const JNode *n = &d->n[i];
    switch (n->type) {
    case J_NULL: buf_puts(b, "null"); break;
    case J_FALSE: buf_puts(b, "false"); break;
    case J_TRUE: buf_puts(b, "true"); break;
    case J_NUM:
        if (n->num == floor(n->num) && fabs(n->num) < 9e15) buf_printf(b, "%lld", (long long)n->num);
        else buf_printf(b, "%.17g", n->num);
        break;
    case J_STR: buf_str(b, n->s); break;
    default: {
        buf_puts(b, n->type == J_OBJ ? "{" : "[");
        for (int k = n->first; k >= 0; k = d->n[k].next) {
            if (k != n->first) buf_puts(b, ",");
            if (n->type == J_OBJ) buf_str(b, d->n[k].key), buf_puts(b, ":");
            j_write(b, d, k);
        }
        buf_puts(b, n->type == J_OBJ ? "}" : "]");
    }
    }
}

/* ---- the bridge's state ---- */
typedef struct Conn {
    int used, gen;
#ifdef _WIN32
    HANDLE pipe;
    OVERLAPPED rov;
    char rbuf[4096];
    int reading;
#else
    int fd;
#endif
    char *in;
    size_t in_n;
    int discarding;   /* (the rest of a line that was too long) */
    Buf out;
    int closing;
    int eof;   /* (the client shut its sending side: no more requests; the connection lasts until its replies are out) */
    /* subscribe */
    int sub_on, sub_present, sub_state, sub_log, sub_map;
    int64_t sub_every, sub_next;
    char sub_id[256], sub_filter[256];
    unsigned log_seen;   /* (the log ring's count it has seen) */
} Conn;

enum { P_INPUT = 1, P_WAIT, P_SHOT, P_MEM, P_TELEPORT };
enum { W_FRAME = 1, W_STATE, W_LOADING_START, W_LOADING_END, W_CHANGE, W_MAP };
typedef struct {
    int used, conn, gen, kind;
    char id[256];   /* the request's id as JSON */
    int64_t deadline, issued_frame;
    int group;       /* P_INPUT: its inputs */
    int wait, from_changes; int64_t target; char state[16], map[80];
    int token; char path[1100];   /* P_SHOT (its result: G.shots), P_MEM */
    int mem_slot;   /* P_MEM */
    int dest;       /* P_TELEPORT: the destination (token: the game's ticket); -1 the age switch's */
    int adult_to;   /* P_TELEPORT for the age switch: 1 to the adult, 0 to the child */
} Pending;

typedef struct {
    int used, group, exclusive, stick_set;
    int conn, conn_gen, waited;  /* the client that sent it; waited: a request waits for its end (else it is freed then) */
    int64_t at, frames, start;   /* start: -1 until the first controller read after the request */
    uint64_t buttons;
    int32_t stick[4];
} Input;

static const char *const STATES[] = {"boot", "intro", "menu", "loading", "play", "unknown"};
enum { S_BOOT, S_INTRO, S_MENU, S_LOADING, S_PLAY, S_UNKNOWN };

typedef struct {
    int used, token, write, base64, nranges, done, ok;
    uint64_t addr[CTL_MEM_RANGES], size[CTL_MEM_RANGES], total;
    uint8_t *buf;   /* read: filled on the game thread; write: the bytes to write */
    char err[160];
} MemReq;

static struct {
    const HosControlGame *game;
    int mem_level;   /* 0 off, 1 ro, 2 rw (HOS_CONTROL_MEM) */
    MemReq mems[CTL_MEM];
    char path[1024], base_dir[1040], shots_dir[1060];   /* base_dir: relative shot paths and shots/ (with its separator) */
    ctl_mutex mu;     /* inputs, memory requests, shot results: the controller's read and the shot writer take it */
    Input inputs[CTL_INPUTS];
    struct { int token, done, ok; int64_t present; } shots[CTL_PENDING];   /* (by the shot's pending slot; G.mu) */
    int next_group, next_token, shot_count;
    Conn conns[CTL_CONNS];
    int conn_gen;
    Pending pending[CTL_PENDING];
    /* the frame, the rate, the state (the bridge's thread only) */
    int64_t started_ns, frame, last_frame_ns, last_frame_seen;
    struct { int64_t t, frame, replayed, draws; } samples[16];
    int nsamples, sample_at;
    int64_t next_sample;
    double fps, draws;
    int have_draws, state, candidate, candidate_n, seen_menu, state_changes, loading_ends, loading_starts;
    int width, height;
    int64_t replayed;   /* the renderer's presents (shots count in these) */
    char map[80];       /* the map the game is in, the last one known ("" before any); map_changes: how often it changed */
    int map_changes;
    int have_oot, oot_event, oot_loading, oot_child;   /* (with the map: hos_oot_state) */
    /* log lines for subscribers (any thread writes; the bridge's thread reads) */
    ctl_mutex log_mu;
    char log_ring[CTL_LOG_RING][400];
    unsigned log_count;
    volatile int log_wanted;
} G = {.mu = CTL_MUTEX_INIT, .log_mu = CTL_MUTEX_INIT};

static void ctl_log(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (G.game && G.game->log) G.game->log(line);
}

/* HOS_CONTROL_MEM: -1 a value that is not one, 0 none/empty, 1 ro (reads), 2 rw (reads and writes). ro: r, read; rw:
   readwrite; the canonical names and the aliases, case does not matter */
int hos_control_mem_level(const char *v) {
    if (!v || !*v) return 0;
    char w[16];
    size_t n = 0;
    for (; v[n] && n + 1 < sizeof w; n++) w[n] = (char)tolower((unsigned char)v[n]);
    if (v[n]) return -1;   /* longer than any name */
    w[n] = 0;
    if (!strcmp(w, "ro") || !strcmp(w, "r") || !strcmp(w, "read")) return 1;
    if (!strcmp(w, "rw") || !strcmp(w, "readwrite")) return 2;
    return -1;
}

/* (the game thread, each controller read) the queued memory reads and writes, done where the game runs so a read is one
   frame's memory and a write lands between frames. Each is bounded first (hos_mem_region through the game): out of range
   is an error, never a fault */
static void mem_service(void) {
    uint64_t wrote_at[CTL_MEM], wrote_n[CTL_MEM];
    int nwrote = 0;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_MEM; i++) {
        MemReq *m = &G.mems[i];
        if (!m->used || m->done) continue;
        m->ok = 1;
        for (int r = 0; r < m->nranges && m->ok; r++)
            if (!G.game->mem_region(m->addr[r], m->size[r])) {
                m->ok = 0;
                snprintf(m->err, sizeof m->err, "%#llx..%#llx is not mapped guest memory",
                         (unsigned long long)m->addr[r], (unsigned long long)(m->addr[r] + m->size[r]));
            }
        if (m->ok) {
            if (m->write) {
                G.game->mem_write(m->addr[0], m->buf, m->size[0]);
                wrote_at[nwrote] = m->addr[0], wrote_n[nwrote++] = m->size[0];
            } else {
                uint64_t off = 0;
                for (int r = 0; r < m->nranges; r++) {
                    G.game->mem_read(m->addr[r], m->buf + off, m->size[r]);
                    off += m->size[r];
                }
            }
        }
        m->done = 1;
    }
    ctl_unlock(&G.mu);
    for (int i = 0; i < nwrote; i++)   /* (logged after the lock: the log may wait for its file) */
        ctl_log("control: wrote %llu bytes to guest %#llx", (unsigned long long)wrote_n[i], (unsigned long long)wrote_at[i]);
}

/* ---- the controller's read (nn_hid.c read_input, the game's thread) ---- */
int hos_control_input(uint64_t *buttons, int32_t stick[4]) {
    if (!hos_control_on) return 0;
    if (G.mem_level) mem_service();
    const int64_t frame = G.game->frame();
    int exclusive = 0;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_INPUTS; i++) {
        Input *in = &G.inputs[i];
        if (!in->used) continue;
        if (in->start < 0) in->start = frame + in->at;
        const int64_t end = in->start + in->frames;
        if (frame >= in->start && frame < end) {
            *buttons |= in->buttons;
            for (int k = 0; k < 4; k++)
                if (in->stick_set >> k & 1) stick[k] = in->stick[k];
        }
        if (in->exclusive && frame < end) exclusive = 1;
    }
    ctl_unlock(&G.mu);
    return exclusive;
}

/* ---- log lines (hos_log, any thread) ---- */
void hos_control_log(const char *line) {
    if (!hos_control_on || !G.log_wanted) return;
    ctl_lock(&G.log_mu);
    char *slot = G.log_ring[G.log_count % CTL_LOG_RING];
    snprintf(slot, sizeof G.log_ring[0], "%s", line);
    size_t n = strlen(slot);
    while (n && (slot[n - 1] == '\n' || slot[n - 1] == '\r')) slot[--n] = 0;
    G.log_count++;
    ctl_unlock(&G.log_mu);
}

/* ---- the frame rate, draws a frame, and the state guessed from them (the bridge's thread, every tick) ---- */
static int classify(double draws) { return draws < 11 ? (G.seen_menu ? S_LOADING : S_INTRO) : draws < 150 ? S_MENU : S_PLAY; }
static void stats_tick(int64_t now) {
    const int64_t frame = G.game->frame();
    if (frame != G.last_frame_seen) G.last_frame_seen = frame, G.last_frame_ns = now;
    G.frame = frame;
    if (now < G.next_sample) return;
    G.next_sample = now + 100000000;
    int64_t replayed = frame, draws = 0;
    int w = 0, h = 0;
    G.have_draws = G.game->frame_info && G.game->frame_info(&replayed, &draws, &w, &h);
    if (G.have_draws) G.width = w, G.height = h, G.replayed = replayed;
    char map[sizeof G.map];
    if (G.game->map && G.game->map(map, sizeof map) && strcmp(map, G.map)) {
        for (int i = 0; i < CTL_CONNS; i++) {   /* subscribers: the new map */
            Conn *c = &G.conns[i];
            if (!c->used || c->closing || !c->sub_on || !c->sub_map) continue;
            buf_printf(&c->out, "{\"event\":\"map\",\"sub\":%s,\"frame\":%lld,\"map\":", c->sub_id, (long long)frame);
            buf_str(&c->out, map);
            buf_puts(&c->out, ",\"from\":");
            if (G.map[0]) buf_str(&c->out, G.map);
            else buf_puts(&c->out, "null");
            buf_puts(&c->out, "}\n");
        }
        memcpy(G.map, map, sizeof G.map);
        G.map_changes++;
    }
    G.have_oot = G.game->oot_state && G.game->oot_state(&G.oot_event, &G.oot_loading, &G.oot_child);
    const int at = G.sample_at;
    G.samples[at].t = now, G.samples[at].frame = frame, G.samples[at].replayed = replayed, G.samples[at].draws = draws;
    G.sample_at = (at + 1) % 16;
    if (G.nsamples < 16) G.nsamples++;
    /* (the rate over the last second; the draws a frame over the last ~300 ms) */
    const int back_rate = G.nsamples > 10 ? 10 : G.nsamples - 1, back_draws = G.nsamples > 3 ? 3 : G.nsamples - 1;
    if (back_rate > 0) {
        const int o = (at - back_rate + 16) % 16;
        const double secs = (now - G.samples[o].t) / 1e9;
        if (secs > 0) G.fps = (frame - G.samples[o].frame) / secs;
    }
    if (back_draws > 0 && G.have_draws) {
        const int o = (at - back_draws + 16) % 16;
        const int64_t frames = replayed - G.samples[o].replayed;
        if (frames > 0) G.draws = (double)(draws - G.samples[o].draws) / (double)frames;
    }
    int next = G.state;
    if (frame == 0) next = S_BOOT;
    else if (!G.have_draws) next = S_UNKNOWN;
    else {
        /* (a new state holds for three samples, 300 ms, before it is taken: a frame or two of a menu over the game is no
           change. From boot the first one counts at once) */
        const int c = classify(G.draws);
        if (c == G.candidate) G.candidate_n++;
        else G.candidate = c, G.candidate_n = 1;
        if (G.state == S_BOOT || G.state == S_UNKNOWN || G.candidate_n >= 3) next = c;
    }
    if (next != G.state) {
        const int from = G.state;
        if (next == S_MENU || next == S_PLAY) G.seen_menu = 1;
        if (next == S_LOADING) G.loading_starts++;
        if (from == S_LOADING) G.loading_ends++;
        G.state = next;
        G.state_changes++;
        /* subscribers: the change */
        for (int i = 0; i < CTL_CONNS; i++) {
            Conn *c = &G.conns[i];
            if (!c->used || c->closing || !c->sub_on || !c->sub_state) continue;
            buf_printf(&c->out, "{\"event\":\"state\",\"sub\":%s,\"frame\":%lld,\"state\":\"%s\",\"from\":\"%s\",\"draws\":%.1f}\n",
                       c->sub_id, (long long)frame, STATES[next], STATES[from], G.draws);
        }
        ctl_log("control: state %s -> %s at frame %lld (%.1f draws a frame)", STATES[from], STATES[next], (long long)frame,
                G.draws);
    }
}

/* ---- replies ---- */
static Conn *conn_of(int index, int gen) {
    if (index < 0 || index >= CTL_CONNS) return NULL;
    Conn *c = &G.conns[index];
    return c->used && c->gen == gen && !c->closing ? c : NULL;
}
static void reply_start(Buf *b, const char *id, int ok) {
    buf_printf(b, "{\"id\":%s,\"ok\":%s,\"frame\":%lld", id, ok ? "true" : "false", (long long)G.frame);
}
static void reply_end(Conn *c, Buf *b) {
    buf_puts(b, "}\n");
    if (c && !b->failed) buf_put(&c->out, b->p, b->n);
    buf_free(b);
}
static void reply_error(Conn *c, const char *id, const char *code, const char *fmt, ...) {
    char msg[400];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    Buf b = {0};
    reply_start(&b, id, 0);
    buf_puts(&b, ",\"error\":");
    buf_str(&b, code);
    buf_puts(&b, ",\"message\":");
    buf_str(&b, msg);
    reply_end(c, &b);
}
static void reply_ok(Conn *c, const char *id) {
    Buf b = {0};
    reply_start(&b, id, 1);
    reply_end(c, &b);
}

/* ---- arguments ---- */
/* a JSON number as a whole number, held to +-2^62 first (a double past int64_t's range is undefined to convert) */
static int64_t whole(double v) { return v != v ? 0 : v > 4.6e18 ? (int64_t)4.6e18 : v < -4.6e18 ? -(int64_t)4.6e18 : (int64_t)v; }
static double num_arg(const JDoc *d, int req, const char *key, double otherwise, int *bad) {
    const JNode *n = j_get(d, req, key);
    if (!n || n->type == J_NULL) return otherwise;
    if (n->type == J_TRUE || n->type == J_FALSE) return n->type == J_TRUE;
    if (n->type != J_NUM) { *bad = 1; return otherwise; }
    return n->num;
}
static const char *str_arg(const JDoc *d, int req, const char *key) {
    const JNode *n = j_get(d, req, key);
    return n && n->type == J_STR ? n->s : NULL;
}
static int bool_arg(const JDoc *d, int req, const char *key, int otherwise) {
    const JNode *n = j_get(d, req, key);
    return !n ? otherwise : n->type == J_TRUE ? 1 : n->type == J_FALSE || n->type == J_NULL ? 0 : n->type == J_NUM ? n->num != 0 : otherwise;
}
/* the timeout in seconds (default, at most an hour) as a deadline */
static int64_t deadline_arg(const JDoc *d, int req, double fallback, int *bad) {
    double s = num_arg(d, req, "timeout", fallback, bad);
    if (s < 0) *bad = 1;
    if (s > 3600) s = 3600;
    return ctl_now_ns() + (int64_t)(s * 1e9);
}

/* ---- inputs: buttons by the names HOS_INPUT_SCRIPT uses (nn_hid.c), the Switch's bit order (vk_api.h HOS_SW_*) ---- */
static const struct { const char *name; int bit, axis, dir; } NAMES[] = {
    {"A", 0, 0, 0}, {"B", 1, 0, 0}, {"X", 2, 0, 0}, {"Y", 3, 0, 0}, {"LS", 4, 0, 0}, {"RS", 5, 0, 0}, {"L", 6, 0, 0}, {"R", 7, 0, 0}, {"ZL", 8, 0, 0}, {"ZR", 9, 0, 0},
    {"PLUS", 10, 0, 0}, {"MINUS", 11, 0, 0}, {"LEFT", 12, 0, 0}, {"UP", 13, 0, 0}, {"RIGHT", 14, 0, 0}, {"DOWN", 15, 0, 0},
    {"LUP", -1, 1, 1}, {"LDOWN", -1, 1, -1}, {"LLEFT", -1, 0, -1}, {"LRIGHT", -1, 0, 1},
    {"RUP", -1, 3, 1}, {"RDOWN", -1, 3, -1}, {"RLEFT", -1, 2, -1}, {"RRIGHT", -1, 2, 1}};

/* One token of a button list into in: a button, a stick direction, or LX=/LY=/RX=/RY=<-32767..32767>. 0: not one */
static int input_token(Input *in, const char *s, size_t n) {
    char t[24];
    if (n == 0 || n >= sizeof t) return 0;
    for (size_t i = 0; i < n; i++) t[i] = (char)toupper((unsigned char)s[i]);
    t[n] = 0;
    for (unsigned i = 0; i < sizeof NAMES / sizeof *NAMES; i++) {
        if (strcmp(t, NAMES[i].name)) continue;
        if (NAMES[i].bit >= 0) in->buttons |= 1ULL << NAMES[i].bit;
        else in->stick[NAMES[i].axis] = NAMES[i].dir * 32767, in->stick_set |= 1 << NAMES[i].axis;
        return 1;
    }
    if (n >= 4 && (t[0] == 'L' || t[0] == 'R') && (t[1] == 'X' || t[1] == 'Y') && t[2] == '=') {
        char *e;
        long v = strtol(t + 3, &e, 10);
        if (*e) return 0;
        v = v < -32767 ? -32767 : v > 32767 ? 32767 : v;
        const int axis = (t[0] == 'L' ? 0 : 2) + (t[1] == 'Y');
        in->stick[axis] = (int32_t)v, in->stick_set |= 1 << axis;
        return 1;
    }
    return 0;
}
/* "A+B" (or "A B"): 0 with the bad token in err */
static int input_names(Input *in, const char *s, char *err, size_t cap) {
    while (*s) {
        while (*s == '+' || *s == ' ') s++;
        const size_t n = strcspn(s, "+ ");
        if (!n) break;
        if (!input_token(in, s, n)) { snprintf(err, cap, "not a button: %.*s", (int)(n < 40 ? n : 40), s); return 0; }
        s += n;
    }
    return 1;
}
static int32_t axis_value(double v) { v = v < -1 ? -1 : v > 1 ? 1 : v; return (int32_t)lround(v * 32767); }

/* One step of press, stick or sequence (its keys: buttons, frames, at, stick + x/y, lx ly rx ry) into in */
static int input_from(const JDoc *d, int obj, Input *in, int64_t default_frames, char *err, size_t cap) {
    memset(in, 0, sizeof *in);
    int bad = 0;
    const JNode *b = j_get(d, obj, "buttons");
    if (!b) b = j_get(d, obj, "button");
    if (b && b->type == J_STR && !input_names(in, b->s, err, cap)) return 0;
    if (b && b->type == J_ARR)
        for (int k = b->first; k >= 0; k = d->n[k].next)
            if (d->n[k].type != J_STR || !input_names(in, d->n[k].s, err, cap)) {
                if (!*err) snprintf(err, cap, "buttons: a list of names");
                return 0;
            }
    if (b && b->type != J_STR && b->type != J_ARR && b->type != J_NULL) return snprintf(err, cap, "buttons: a name or a list of names"), 0;
    const char *which = str_arg(d, obj, "stick");
    const JNode *x = j_get(d, obj, "x"), *y = j_get(d, obj, "y");
    if (which || x || y) {
        const int r = which && (which[0] == 'R' || which[0] == 'r');
        if (which && !r && which[0] != 'L' && which[0] != 'l') return snprintf(err, cap, "stick: L or R"), 0;
        const int ax = r ? 2 : 0;
        in->stick[ax] = axis_value(num_arg(d, obj, "x", 0, &bad));
        in->stick[ax + 1] = axis_value(num_arg(d, obj, "y", 0, &bad));
        in->stick_set |= 3 << ax;
    }
    static const char *const axes[4] = {"lx", "ly", "rx", "ry"};
    for (int k = 0; k < 4; k++)
        if (j_get(d, obj, axes[k])) in->stick[k] = axis_value(num_arg(d, obj, axes[k], 0, &bad)), in->stick_set |= 1 << k;
    in->frames = whole(num_arg(d, obj, "frames", (double)default_frames, &bad));
    in->at = whole(num_arg(d, obj, "at", 0, &bad));
    if (bad) return snprintf(err, cap, "frames, at, x, y: numbers"), 0;
    if (in->frames < 1 || in->frames > 216000) return snprintf(err, cap, "frames: 1 to 216000"), 0;
    if (in->at < 0 || in->at > 216000) return snprintf(err, cap, "at: 0 to 216000 frames"), 0;
    if (!in->buttons && !in->stick_set) return snprintf(err, cap, "nothing to press: buttons, or a stick"), 0;
    return 1;
}
/* HOS_INPUT_SCRIPT's syntax, "<at>:<button>[+<button>...][/<held>],...": frames, or with hos_units seconds and ms as there
   (60 frames a second) */
static int input_script(const char *s, Input *steps, int max, int hos_units, char *err, size_t cap) {
    int n = 0;
    while (*s) {
        while (*s == ' ' || *s == ',' || *s == '\t') s++;
        if (!*s) break;
        if (n >= max) return snprintf(err, cap, "more than %d steps", max), -1;
        Input *in = &steps[n];
        memset(in, 0, sizeof *in);
        char *e;
        const double at = strtod(s, &e);
        if (e == s || *e != ':' || at < 0) return snprintf(err, cap, "step %d: not <at>:<buttons>", n + 1), -1;
        s = e + 1;
        const size_t len = strcspn(s, "/,");
        char names[256];
        if (len >= sizeof names) return snprintf(err, cap, "step %d: too long", n + 1), -1;
        memcpy(names, s, len);
        names[len] = 0;
        if (!input_names(in, names, err, cap)) return -1;
        if (!in->buttons && !in->stick_set) return snprintf(err, cap, "step %d: nothing to press", n + 1), -1;
        s += len;
        double held = hos_units ? 200 : 8;
        if (*s == '/') {
            held = strtod(s + 1, &e);
            if (e == s + 1 || held <= 0) return snprintf(err, cap, "step %d: a bad hold", n + 1), -1;
            s = e;
        }
        if (at > 1e7 || held > 1e9) return snprintf(err, cap, "step %d: too long", n + 1), -1;   /* (llround past long: undefined) */
        in->at = hos_units ? (int64_t)llround(at * 60) : (int64_t)llround(at);
        in->frames = hos_units ? (int64_t)llround(held * 60 / 1000) : (int64_t)llround(held);
        if (in->frames < 1) in->frames = 1;
        if (in->at > 216000 || in->frames > 216000) return snprintf(err, cap, "step %d: too long", n + 1), -1;
        n++;
        if (*s && *s != ',') return snprintf(err, cap, "step %d: no ',' after it", n), -1;
    }
    if (!n) return snprintf(err, cap, "no steps"), -1;
    return n;
}

static Pending *pending_new(int conn, const char *id, int kind) {
    for (int i = 0; i < CTL_PENDING; i++) {
        Pending *p = &G.pending[i];
        if (p->used) continue;
        memset(p, 0, sizeof *p);
        p->used = 1, p->conn = conn, p->gen = G.conns[conn].gen, p->kind = kind, p->issued_frame = G.frame;
        snprintf(p->id, sizeof p->id, "%s", id);
        return p;
    }
    return NULL;
}
static void inputs_drop(int group) {   /* (G.mu) */
    for (int i = 0; i < CTL_INPUTS; i++)
        if (G.inputs[i].used && G.inputs[i].group == group) G.inputs[i].used = 0;
}

/* press, stick and sequence: steps queued for the controller's read; the reply when the last one is over (wait: false
   at once) */
static void cmd_input(int ci, const JDoc *d, int req, const char *id, const char *cmd) {
    Conn *c = &G.conns[ci];
    static Input steps[CTL_INPUTS];
    char err[200] = "";
    int n = 0;
    if (!strcmp(cmd, "sequence")) {
        const JNode *list = j_get(d, req, "steps");
        const char *script = str_arg(d, req, "script");
        const char *units = str_arg(d, req, "units");
        if (units && strcmp(units, "frames") && strcmp(units, "hos")) return reply_error(c, id, "bad_request", "units: frames or hos");
        if (script) n = input_script(script, steps, CTL_INPUTS, units && !strcmp(units, "hos"), err, sizeof err);
        else if (list && list->type == J_ARR) {
            for (int k = list->first; k >= 0 && n >= 0; k = d->n[k].next) {
                if (n >= CTL_INPUTS) n = -1, snprintf(err, sizeof err, "more than %d steps", CTL_INPUTS);
                else if (d->n[k].type != J_OBJ) n = -1, snprintf(err, sizeof err, "steps: objects");
                else if (!input_from(d, k, &steps[n], 8, err, sizeof err)) n = -1;
                else n++;
            }
            if (n == 0) n = -1, snprintf(err, sizeof err, "no steps");
        } else n = -1, snprintf(err, sizeof err, "sequence: steps (a list) or script (a string)");
    } else {
        if (!strcmp(cmd, "stick") && !j_get(d, req, "x") && !j_get(d, req, "y"))
            return reply_error(c, id, "bad_request", "stick: x and y, -1 to 1");
        if (!strcmp(cmd, "press") && !j_get(d, req, "buttons") && !j_get(d, req, "button"))
            return reply_error(c, id, "bad_request", "press: buttons");
        n = input_from(d, req, &steps[0], 8, err, sizeof err) ? 1 : -1;
    }
    if (n < 0) return reply_error(c, id, "bad_request", "%s", err);
    int bad = 0;
    const int exclusive = bool_arg(d, req, "exclusive", 0), wait = bool_arg(d, req, "wait", 1);
    int64_t longest = 0;
    for (int i = 0; i < n; i++)
        if (steps[i].at + steps[i].frames > longest) longest = steps[i].at + steps[i].frames;
    /* (the timeout: the inputs' own frames at 10 fps, and 30 s more) */
    const int64_t deadline = deadline_arg(d, req, 30.0 + longest / 10.0, &bad);
    if (bad) return reply_error(c, id, "bad_request", "timeout: seconds");
    Pending *p = wait ? pending_new(ci, id, P_INPUT) : NULL;
    if (wait && !p) return reply_error(c, id, "busy", "too many requests waiting");
    ctl_lock(&G.mu);
    int free_slots = 0;
    for (int i = 0; i < CTL_INPUTS; i++) free_slots += !G.inputs[i].used;
    const int group = ++G.next_group;
    if (free_slots >= n)
        for (int i = 0, k = 0; i < CTL_INPUTS && k < n; i++) {
            if (G.inputs[i].used) continue;
            G.inputs[i] = steps[k++];
            G.inputs[i].used = 1, G.inputs[i].group = group, G.inputs[i].exclusive = exclusive, G.inputs[i].start = -1;
            G.inputs[i].conn = ci, G.inputs[i].conn_gen = c->gen, G.inputs[i].waited = p != NULL;
        }
    ctl_unlock(&G.mu);
    if (free_slots < n) {
        if (p) p->used = 0;
        return reply_error(c, id, "busy", "too many inputs queued");
    }
    if (p) {
        p->group = group, p->deadline = deadline;
        return;
    }
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_printf(&b, ",\"queued\":%d,\"frames\":%lld", n, (long long)longest);
    reply_end(c, &b);
}
/* the inputs of a group are over (start set and the frame past the end of each): the reply */
static int input_done(Pending *p, Conn *c, int64_t now) {
    int64_t start = -1, end = -1;
    int left = 0, unread = 0;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_INPUTS; i++) {
        const Input *in = &G.inputs[i];
        if (!in->used || in->group != p->group) continue;
        if (in->start < 0) { unread = 1, left = 1; continue; }
        if (start < 0 || in->start < start) start = in->start;
        if (in->start + in->frames > end) end = in->start + in->frames;
        if (G.frame < in->start + in->frames) left = 1;
    }
    const int over = !left || now >= p->deadline;
    if (over) inputs_drop(p->group);
    ctl_unlock(&G.mu);
    if (!over) return 0;
    if (left)
        reply_error(c, p->id, "timeout", unread ? "the game did not read the controller" : "the inputs did not finish in time");
    else {
        Buf b = {0};
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"start\":%lld,\"end\":%lld", (long long)start, (long long)end);
        reply_end(c, &b);
    }
    return 1;
}

/* (the bridge's thread, every tick) inputs nobody waits for (sent with "wait": false) freed once over: they would
   otherwise fill the queue */
static void inputs_tick(void) {
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_INPUTS; i++) {
        Input *in = &G.inputs[i];
        if (in->used && !in->waited && in->start >= 0 && G.frame >= in->start + in->frames) in->used = 0;
    }
    ctl_unlock(&G.mu);
}
/* release: the caller's inputs, and those left by clients that have gone (sent without waiting); with all, everyone's.
   The presses waited on reply cancelled */
static void cmd_release(int ci, const JDoc *d, int req, const char *id) {
    const int all = bool_arg(d, req, "all", 0);
    static int groups[CTL_INPUTS];
    int n = 0, ngroups = 0;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_INPUTS; i++) {
        Input *in = &G.inputs[i];
        if (!in->used) continue;
        const int mine = in->conn == ci && in->conn_gen == G.conns[ci].gen;
        if (!all && !mine && conn_of(in->conn, in->conn_gen)) continue;   /* (another client's, still there) */
        if (in->waited && (!ngroups || groups[ngroups - 1] != in->group)) groups[ngroups++] = in->group;
        in->used = 0, n++;
    }
    ctl_unlock(&G.mu);
    for (int i = 0; i < CTL_PENDING; i++) {   /* (the presses waited for end now) */
        Pending *p = &G.pending[i];
        if (!p->used || p->kind != P_INPUT) continue;
        int hit = 0;
        for (int k = 0; k < ngroups && !hit; k++) hit = groups[k] == p->group;
        if (!hit) continue;
        reply_error(conn_of(p->conn, p->gen), p->id, "cancelled", "released");
        p->used = 0;
    }
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_printf(&b, ",\"released\":%d,\"all\":%s", n, all ? "true" : "false");
    reply_end(&G.conns[ci], &b);
}

/* ---- status ---- */
static int exclusive_now(int *active) {
    int ex = 0, n = 0;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_INPUTS; i++) {
        const Input *in = &G.inputs[i];
        if (!in->used) continue;
        n++;
        if (in->exclusive && (in->start < 0 || G.frame < in->start + in->frames)) ex = 1;
    }
    ctl_unlock(&G.mu);
    *active = n;
    return ex;
}
static void cmd_status(int ci, const char *id) {
    const int64_t now = ctl_now_ns();
    int active = 0, clients = 0;
    const int ex = exclusive_now(&active);
    for (int i = 0; i < CTL_CONNS; i++) clients += G.conns[i].used;
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_printf(&b, ",\"protocol\":1,\"fps\":%.1f,\"state\":\"%s\",\"stalled\":%s", G.fps, STATES[G.state],
               G.frame && now - G.last_frame_ns > 1000000000 ? "true" : "false");
    if (G.have_draws)
        buf_printf(&b, ",\"draws\":%.1f,\"replayed\":%lld,\"window\":{\"width\":%d,\"height\":%d}", G.draws,
                   (long long)G.replayed, G.width, G.height);
    else buf_puts(&b, ",\"draws\":null,\"replayed\":null,\"window\":null");
    buf_printf(&b, ",\"uptime\":%.1f,\"clients\":%d,\"input\":{\"queued\":%d,\"exclusive\":%s}", (now - G.started_ns) / 1e9,
               clients, active, ex ? "true" : "false");
    buf_puts(&b, ",\"map\":");
    if (G.map[0]) buf_str(&b, G.map);
    else buf_puts(&b, "null");
    if (G.have_oot)
        buf_printf(&b, ",\"game\":{\"event\":%s,\"loading\":%s,\"age\":\"%s\"}", G.oot_event ? "true" : "false",
                   G.oot_loading ? "true" : "false", G.oot_child ? "child" : "adult");
    else buf_puts(&b, ",\"game\":null");
    buf_puts(&b, ",\"version\":");
    buf_str(&b, G.game->version ? G.game->version : "unknown");
    const char *r = G.game->renderer ? G.game->renderer() : NULL;
    buf_puts(&b, ",\"renderer\":");
    if (r) buf_str(&b, r);
    else buf_puts(&b, "null");
    reply_end(&G.conns[ci], &b);
}

/* ---- wait ---- */
static void name_norm(char *out, size_t cap, const char *in);
static int state_of(const char *s) {
    for (int i = 0; i < (int)(sizeof STATES / sizeof *STATES); i++)
        if (!strcmp(s, STATES[i])) return i;
    return -1;
}
static void cmd_wait(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    const char *event = str_arg(d, req, "event");
    if (!event) event = str_arg(d, req, "for");
    if (!event) return reply_error(c, id, "bad_request", "wait: event (present, frame, in_play, state, loading_start, loading_end, change, map)");
    int bad = 0, kind = 0, state = -1;
    int64_t target = 0;
    if (!strcmp(event, "present") || !strcmp(event, "frames")) {
        const double n = num_arg(d, req, "frames", num_arg(d, req, "n", 1, &bad), &bad);
        if (n < 0 || n > 1e9) bad = 1;
        kind = W_FRAME, target = G.frame + whole(n);
    } else if (!strcmp(event, "frame")) {
        const JNode *f = j_get(d, req, "frame");
        if (!f || f->type != J_NUM) return reply_error(c, id, "bad_request", "wait frame: frame, the frame number");
        kind = W_FRAME, target = whole(f->num);
    } else if (!strcmp(event, "in_play")) kind = W_STATE, state = S_PLAY;
    else if (!strcmp(event, "state")) {
        const char *s = str_arg(d, req, "state");
        if (!s || (state = state_of(s)) < 0) return reply_error(c, id, "bad_request", "wait state: state, one of boot intro menu loading play");
        kind = W_STATE;
    } else if (!strcmp(event, "loading_start")) kind = W_LOADING_START;
    else if (!strcmp(event, "loading_end")) kind = W_LOADING_END;
    else if (!strcmp(event, "change")) kind = W_CHANGE;
    else if (!strcmp(event, "map")) {
        const JNode *m = j_get(d, req, "map");
        if (m && m->type != J_STR && m->type != J_NULL) return reply_error(c, id, "bad_request", "wait map: map, a map's name");
        if (!G.game->map) return reply_error(c, id, "unavailable", "this game tells no map");
        kind = W_MAP;
    } else return reply_error(c, id, "bad_request", "wait: no event %.60s", event);
    const int64_t deadline = deadline_arg(d, req, 60, &bad);
    if (bad) return reply_error(c, id, "bad_request", "frames, timeout: numbers");
    Pending *p = pending_new(ci, id, P_WAIT);
    if (!p) return reply_error(c, id, "busy", "too many requests waiting");
    p->wait = kind, p->target = target, p->deadline = deadline;
    p->from_changes = kind == W_LOADING_START ? G.loading_starts : kind == W_LOADING_END ? G.loading_ends
                    : kind == W_MAP ? G.map_changes : G.state_changes;
    if (state >= 0) snprintf(p->state, sizeof p->state, "%s", STATES[state]);
    if (kind == W_MAP) name_norm(p->map, sizeof p->map, str_arg(d, req, "map") ? str_arg(d, req, "map") : "");
}
static int wait_done(Pending *p, Conn *c, int64_t now) {
    int met = 0;
    switch (p->wait) {
    case W_FRAME: met = G.frame >= p->target; break;
    case W_STATE: met = !strcmp(STATES[G.state], p->state); break;
    case W_LOADING_START: met = G.loading_starts != p->from_changes; break;
    case W_LOADING_END: met = G.loading_ends != p->from_changes; break;
    case W_CHANGE: met = G.state_changes != p->from_changes; break;
    case W_MAP: {   /* (that map, in any case as teleport takes it; or without one, another) */
        char now_map[sizeof G.map];
        name_norm(now_map, sizeof now_map, G.map);
        met = p->map[0] ? !strcmp(now_map, p->map) : G.map_changes != p->from_changes;
        break;
    }
    }
    if (!met && now < p->deadline) return 0;
    if (!met) {
        Buf b = {0};
        reply_start(&b, p->id, 0);
        buf_printf(&b, ",\"error\":\"timeout\",\"message\":\"not met in time\",\"state\":\"%s\"", STATES[G.state]);
        reply_end(c, &b);
        return 1;
    }
    Buf b = {0};
    reply_start(&b, p->id, 1);
    buf_printf(&b, ",\"state\":\"%s\",\"waited\":%lld", STATES[G.state], (long long)(G.frame - p->issued_frame));
    if (p->wait == W_MAP) buf_puts(&b, ",\"map\":"), buf_str(&b, G.map);
    reply_end(c, &b);
    return 1;
}

/* ---- shot ---- */
/* (the renderer's writer thread) a shot written or not: kept by its token in G.shots, which only G.mu guards (the pending
   requests are the bridge thread's own). A shot whose request timed out finds no token and is dropped */
static void shot_done(void *ctx, int64_t present, int ok) {
    const int token = (int)(intptr_t)ctx;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_PENDING; i++)
        if (G.shots[i].token == token) G.shots[i].done = 1, G.shots[i].ok = ok, G.shots[i].present = present;
    ctl_unlock(&G.mu);
}
#ifdef _WIN32
static int path_absolute(const char *p) { return (p[0] && p[1] == ':') || p[0] == '\\' || p[0] == '/'; }
#define CTL_SEP '\\'
#else
static int path_absolute(const char *p) { return p[0] == '/'; }
#define CTL_SEP '/'
#endif
/* The folder shots go to when no path is given: shots/ beside the socket, made the first time (this user's only) */
static int shots_dir_ready(char *err, size_t cap) {
#ifdef _WIN32
    CreateDirectoryA(G.shots_dir, NULL);
    const DWORD a = GetFileAttributesA(G.shots_dir);
    if (a == INVALID_FILE_ATTRIBUTES || !(a & FILE_ATTRIBUTE_DIRECTORY) || (a & FILE_ATTRIBUTE_REPARSE_POINT))
        return snprintf(err, cap, "%.200s is not a folder", G.shots_dir), 0;
#else
    mkdir(G.shots_dir, 0700);
    struct stat st;
    if (lstat(G.shots_dir, &st) || !S_ISDIR(st.st_mode) || st.st_uid != getuid())
        return snprintf(err, cap, "%.200s is not a folder of this user's", G.shots_dir), 0;
#endif
    return 1;
}
static void cmd_shot(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    const char *path = str_arg(d, req, "path");
    char full[1100], err[300] = "";
    int bad = 0;
    if (path && *path) {
        /* (a relative path is the run folder's (the socket's), not the game's: the game runs in its own folder) */
        if (path_absolute(path)) snprintf(full, sizeof full, "%s", path);
        else {
            snprintf(full, sizeof full, "%s", G.base_dir);
            if (strlen(full) + strlen(path) + 1 > sizeof full) return reply_error(c, id, "bad_request", "path: too long");
            strcat(full, path);
        }
        if (strlen(path) > 900) return reply_error(c, id, "bad_request", "path: too long");
    } else {
        if (!shots_dir_ready(err, sizeof err)) return reply_error(c, id, "failed", "%s", err);
        snprintf(full, sizeof full, "%s%cshot_%04d_f%06lld.png", G.shots_dir, CTL_SEP, ++G.shot_count, (long long)G.frame);
    }
    const JNode *after_n = j_get(d, req, "after");
    if (after_n && after_n->type != J_NUM && after_n->type != J_NULL) return reply_error(c, id, "bad_request", "after: a frame number");
    const int64_t after = after_n && after_n->type == J_NUM ? whole(after_n->num) : G.frame;
    const int64_t deadline = deadline_arg(d, req, 30, &bad);
    if (bad) return reply_error(c, id, "bad_request", "timeout: seconds");
    if (!G.game->shot) return reply_error(c, id, "unavailable", "no renderer that takes shots");
    Pending *p = pending_new(ci, id, P_SHOT);
    if (!p) return reply_error(c, id, "busy", "too many requests waiting");
    const int slot = (int)(p - G.pending);
    ctl_lock(&G.mu);
    p->token = ++G.next_token;
    G.shots[slot].token = p->token, G.shots[slot].done = 0;
    ctl_unlock(&G.mu);
    p->deadline = deadline;
    snprintf(p->path, sizeof p->path, "%s", full);
    if (!G.game->shot(full, after, shot_done, (void *)(intptr_t)p->token)) {
        ctl_lock(&G.mu);
        G.shots[slot].token = 0;
        ctl_unlock(&G.mu);
        p->used = 0;
        return reply_error(c, id, "unavailable", "the renderer takes no shot now (not started, no Vulkan, or too many waiting)");
    }
}
static int shot_finished(Pending *p, Conn *c, int64_t now) {
    const int slot = (int)(p - G.pending);
    ctl_lock(&G.mu);
    const int done = G.shots[slot].token == p->token && G.shots[slot].done, ok = G.shots[slot].ok;
    const int64_t present = G.shots[slot].present;
    if (done || now >= p->deadline) G.shots[slot].token = 0;   /* (answered: a late writer finds nothing) */
    ctl_unlock(&G.mu);
    if (!done && now < p->deadline) return 0;
    if (!done) reply_error(c, p->id, "timeout", "no present came in time");
    else if (!ok) reply_error(c, p->id, "failed", "the PNG could not be written to %s", p->path);
    else {
        Buf b = {0};
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"present\":%lld,\"path\":", (long long)present);
        buf_str(&b, p->path);
        unsigned char h[24];   /* (its size from the PNG's header: 1280x720 unless RX_VK_SHOT_W / RX_VK_SHOT_H) */
        FILE *f = fopen(p->path, "rb");
        if (f && fread(h, 1, sizeof h, f) == sizeof h && !memcmp(h, "\x89PNG\r\n\x1a\n", 8) && !memcmp(h + 12, "IHDR", 4))
            buf_printf(&b, ",\"width\":%lu,\"height\":%lu",
                       (unsigned long)h[16] << 24 | (unsigned long)h[17] << 16 | (unsigned long)h[18] << 8 | h[19],
                       (unsigned long)h[20] << 24 | (unsigned long)h[21] << 16 | (unsigned long)h[22] << 8 | h[23]);
        if (f) fclose(f);
        reply_end(c, &b);
    }
    return 1;
}

/* ---- the Insert menu's settings ---- */
static void setting_json(Buf *b, const HosControlSetting *s) {
    buf_puts(b, "{\"key\":");
    buf_str(b, s->key);
    buf_puts(b, ",\"name\":");
    buf_str(b, s->name);
    buf_puts(b, ",\"tab\":");
    buf_str(b, s->tab);
    buf_printf(b, ",\"value\":%d,\"min\":%d,\"max\":%d,\"step\":%d,\"restart\":%s,\"fixed\":%s,\"text\":", s->value, s->lo,
               s->hi, s->step, s->restart ? "true" : "false", s->fixed ? "true" : "false");
    buf_str(b, s->text);
    buf_puts(b, "}");
}
/* a name as a client gives it (the ini key, or the row's name; any case, ' ' or '-' for '_') against a row */
static void name_norm(char *out, size_t cap, const char *in) {
    size_t n = 0;
    for (const char *p = in; *p && n + 1 < cap; p++) out[n++] = *p == ' ' || *p == '-' ? '_' : (char)tolower((unsigned char)*p);
    out[n] = 0;
}
static int name_match(const char *want, const HosControlSetting *s) {
    char a[64], k[64], key[64];
    name_norm(a, sizeof a, want);
    name_norm(k, sizeof k, s->name);
    name_norm(key, sizeof key, s->key);
    return !strcmp(a, key) || !strcmp(a, k);
}
/* the row a name means, listed now (the key is what setting_set takes: rows come and go, an index is no name). 0: none */
static int setting_find(const char *name, HosControlSetting *out) {
    for (int i = 0; i < 512 && G.game->setting && G.game->setting(i, out); i++)
        if (name_match(name, out)) return 1;
    return 0;
}
static void cmd_option_get(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    const char *name = str_arg(d, req, "name");
    HosControlSetting s;
    if (!G.game->setting || !G.game->setting(0, &s)) return reply_error(c, id, "unavailable", "no Insert menu (the Vulkan renderer has not started)");
    Buf b = {0};
    if (name) {
        if (!setting_find(name, &s)) return reply_error(c, id, "not_found", "no setting %.100s", name);
        reply_start(&b, id, 1);
        buf_puts(&b, ",\"option\":");
        setting_json(&b, &s);
    } else {
        reply_start(&b, id, 1);
        buf_puts(&b, ",\"options\":[");
        for (int i = 0; i < 512 && G.game->setting(i, &s); i++) {
            if (i) buf_puts(&b, ",");
            setting_json(&b, &s);
        }
        buf_puts(&b, "]");
    }
    reply_end(c, &b);
}
static void cmd_option_set(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    const char *name = str_arg(d, req, "name");
    const JNode *v = j_get(d, req, "value");
    if (!name || !v || (v->type != J_NUM && v->type != J_TRUE && v->type != J_FALSE))
        return reply_error(c, id, "bad_request", "option_set: name, and value (a number, or true or false)");
    if (v->type == J_NUM && (v->num < -1e9 || v->num > 1e9)) return reply_error(c, id, "bad_request", "value: out of range");
    HosControlSetting s, now;
    if (!G.game->setting || !G.game->setting(0, &s)) return reply_error(c, id, "unavailable", "no Insert menu (the Vulkan renderer has not started)");
    if (!setting_find(name, &s)) return reply_error(c, id, "not_found", "no setting %.100s", name);
    const int value = v->type == J_NUM ? (int)lround(v->num) : v->type == J_TRUE;
    memset(&now, 0, sizeof now);
    const int r = G.game->setting_set ? G.game->setting_set(s.key, value, &now) : 0;
    if (r == -2) return reply_error(c, id, "failed", "%s: no step reaches %d on this PC (left as it was)", s.key, value);
    if (r < 0) return reply_error(c, id, "failed", "%s: nothing to choose on this PC (the menu shows it greyed)", s.key);
    if (r == 0) return reply_error(c, id, "not_found", "no setting %.100s (gone meanwhile)", name);
    ctl_log("control: setting %s %d -> %d", now.key, s.value, now.value);
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_printf(&b, ",\"was\":%d,\"option\":", s.value);
    setting_json(&b, &now);
    reply_end(c, &b);
}

/* ---- subscribe ---- */
static void log_wanted_update(void) {   /* (log lines are copied only while some client subscribed to them) */
    int any_log = 0;
    for (int i = 0; i < CTL_CONNS; i++)
        any_log |= G.conns[i].used && !G.conns[i].closing && G.conns[i].sub_on && G.conns[i].sub_log;
    G.log_wanted = any_log;
}
static void cmd_subscribe(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    int present = 1, state = 1, log = 0, map = 0, bad = 0;
    const JNode *ev = j_get(d, req, "events");
    if (ev && ev->type == J_ARR) {
        present = state = 0;
        for (int k = ev->first; k >= 0; k = d->n[k].next) {
            const char *s = d->n[k].type == J_STR ? d->n[k].s : "";
            if (!strcmp(s, "present")) present = 1;
            else if (!strcmp(s, "state")) state = 1;
            else if (!strcmp(s, "log")) log = 1;
            else if (!strcmp(s, "map")) map = 1;
            else return reply_error(c, id, "bad_request", "events: present, state, log, map");
        }
    } else if (ev) return reply_error(c, id, "bad_request", "events: a list (present, state, log, map)");
    const char *filter = str_arg(d, req, "log");
    if (filter) log = 1;
    const int64_t every = whole(num_arg(d, req, "every", 60, &bad));
    if (bad || every < 1 || every > 216000) return reply_error(c, id, "bad_request", "every: 1 to 216000 frames");
    c->sub_on = 1, c->sub_present = present, c->sub_state = state, c->sub_log = log, c->sub_map = map, c->sub_every = every;
    c->sub_next = G.frame + every;
    snprintf(c->sub_id, sizeof c->sub_id, "%s", id);
    snprintf(c->sub_filter, sizeof c->sub_filter, "%s", filter ? filter : "");
    ctl_lock(&G.log_mu);
    c->log_seen = G.log_count;
    ctl_unlock(&G.log_mu);
    log_wanted_update();
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_puts(&b, ",\"events\":[");
    const char *names[4] = {"present", "state", "log", "map"};
    const int on[4] = {present, state, log, map};
    for (int k = 0, n = 0; k < 4; k++)
        if (on[k]) buf_printf(&b, "%s\"%s\"", n++ ? "," : "", names[k]);
    buf_printf(&b, "],\"every\":%lld", (long long)every);
    reply_end(c, &b);
}
static void subs_tick(void) {
    for (int i = 0; i < CTL_CONNS; i++) {
        Conn *c = &G.conns[i];
        if (!c->used || c->closing || !c->sub_on) continue;
        if (c->sub_present && G.frame >= c->sub_next) {
            c->sub_next = G.frame + c->sub_every;
            buf_printf(&c->out, "{\"event\":\"present\",\"sub\":%s,\"frame\":%lld,\"fps\":%.1f,\"state\":\"%s\"", c->sub_id,
                       (long long)G.frame, G.fps, STATES[G.state]);
            if (G.have_draws) buf_printf(&c->out, ",\"draws\":%.1f", G.draws);
            if (G.map[0]) buf_puts(&c->out, ",\"map\":"), buf_str(&c->out, G.map);
            buf_puts(&c->out, "}\n");
        }
        if (c->sub_log) {
            ctl_lock(&G.log_mu);
            const unsigned count = G.log_count;
            if (count - c->log_seen > CTL_LOG_RING) {   /* (more than the ring holds came since: said, and the rest sent) */
                buf_printf(&c->out, "{\"event\":\"log_dropped\",\"sub\":%s,\"lines\":%u}\n", c->sub_id, count - c->log_seen - CTL_LOG_RING);
                c->log_seen = count - CTL_LOG_RING;
            }
            for (; c->log_seen != count; c->log_seen++) {
                const char *line = G.log_ring[c->log_seen % CTL_LOG_RING];
                if (c->sub_filter[0] && !strstr(line, c->sub_filter)) continue;
                buf_printf(&c->out, "{\"event\":\"log\",\"sub\":%s,\"frame\":%lld,\"line\":", c->sub_id, (long long)G.frame);
                buf_str(&c->out, line);
                buf_puts(&c->out, "}\n");
            }
            ctl_unlock(&G.log_mu);
        }
    }
}

/* ---- (OoT) the debug teleport by destination: the Cheats page's Teleport list's. The game takes it at the player's next
   update, in play: the reply says whether it was sent (and to which map) or refused (and why), unless "wait": false.
   wait map then tells when it arrived ---- */
static void destination_json(Buf *b, int index, const char *child, const char *adult) {
    buf_printf(b, ",\"teleport\":%d,\"child\":", index);
    if (child) buf_str(b, child);
    else buf_puts(b, "null");
    buf_puts(b, ",\"adult\":");
    if (adult) buf_str(b, adult);
    else buf_puts(b, "null");
}
static void cmd_teleport(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    if (!G.game->destination || !G.game->teleport) return reply_error(c, id, "unavailable", "this game has no teleport");
    const JNode *to = j_get(d, req, "to");
    const char *child, *adult;
    Buf b = {0};
    if (!to || to->type == J_NULL) {   /* the destinations */
        reply_start(&b, id, 1);
        buf_puts(&b, ",\"destinations\":[");
        for (int i = 0; i < 256 && G.game->destination(i, &child, &adult); i++) {
            buf_printf(&b, "%s{\"index\":%d,\"child\":", i ? "," : "", i);
            if (child) buf_str(&b, child);
            else buf_puts(&b, "null");
            buf_puts(&b, ",\"adult\":");
            if (adult) buf_str(&b, adult);
            else buf_puts(&b, "null");
            buf_puts(&b, "}");
        }
        buf_puts(&b, "]");
        return reply_end(c, &b);
    }
    int index = -1;
    if (to->type == J_NUM && to->num == floor(to->num) && to->num >= 0 && to->num < 256) index = (int)to->num;
    else if (to->type == J_STR) {   /* (a map's name, the child's or the adult's, in any case) */
        char want[80], name[80];
        name_norm(want, sizeof want, to->s);
        for (int i = 0; i < 256 && index < 0 && G.game->destination(i, &child, &adult); i++)
            for (int age = 0; age < 2 && index < 0; age++) {
                const char *m = age ? adult : child;
                if (m && (name_norm(name, sizeof name, m), !strcmp(name, want))) index = i;
            }
    }
    if (index < 0 || !G.game->destination(index, &child, &adult))
        return reply_error(c, id, "not_found", "to: a destination's index or map (teleport with no to lists them)");
    int bad = 0;
    const int wait = bool_arg(d, req, "wait", 1) && G.game->teleport_result;
    const int64_t deadline = deadline_arg(d, req, 10, &bad);
    if (bad) return reply_error(c, id, "bad_request", "timeout: seconds");
    Pending *p = wait ? pending_new(ci, id, P_TELEPORT) : NULL;
    if (wait && !p) return reply_error(c, id, "busy", "too many requests waiting");
    const int ticket = G.game->teleport(index);
    if (ticket <= 0) {
        if (p) p->used = 0;
        return reply_error(c, id, "not_found", "no destination %d", index);
    }
    ctl_log("control: teleport to destination %d asked for", index);
    if (p) {
        p->token = ticket, p->dest = index, p->deadline = deadline;
        return;
    }
    reply_start(&b, id, 1);
    destination_json(&b, index, child, adult);
    buf_puts(&b, ",\"sent\":null");
    reply_end(c, &b);
}
/* ---- (OoT) the Debug tab's Switch age: Link's age flipped and the map reloaded in the other age, as the menu does it,
   taken at the player's next update as a teleport is (the same tickets: a later teleport or age ask replaces one not taken
   yet). to: "child" or "adult", none the other age (from oot_state's sample). The age asked for goes to the game, which
   checks it when it takes it (the sample can be old): already that age, nothing is done and the reply says so. The reply
   says whether it was sent (and the map it reloads), not needed, or refused (and why), unless "wait": false ---- */
static void cmd_age(int ci, const JDoc *d, int req, const char *id) {
    Conn *c = &G.conns[ci];
    if (!G.game->age || !G.game->oot_state || !G.game->teleport_result)
        return reply_error(c, id, "unavailable", "this game has no age switch");
    int event = 0, loading = 0, child = 0;
    if (!G.game->oot_state(&event, &loading, &child))
        return reply_error(c, id, "unavailable", "Link's age is not known yet: a save is loaded in play first");
    const JNode *to = j_get(d, req, "to");
    int adult_to = child;   /* (none: the other age) */
    if (to && to->type != J_NULL) {
        if (to->type != J_STR || (strcmp(to->s, "child") && strcmp(to->s, "adult")))
            return reply_error(c, id, "bad_request", "to: \"child\" or \"adult\" (none: the other age)");
        adult_to = !strcmp(to->s, "adult");
    }
    Buf b = {0};
    int bad = 0;
    const int wait = bool_arg(d, req, "wait", 1);
    const int64_t deadline = deadline_arg(d, req, 10, &bad);
    if (bad) return reply_error(c, id, "bad_request", "timeout: seconds");
    Pending *p = wait ? pending_new(ci, id, P_TELEPORT) : NULL;
    if (wait && !p) return reply_error(c, id, "busy", "too many requests waiting");
    const int ticket = G.game->age(adult_to);
    if (ticket <= 0) {
        if (p) p->used = 0;
        return reply_error(c, id, "unavailable", "the game did not take the age switch");
    }
    ctl_log("control: age switch to the %s asked for", adult_to ? "adult" : "child");
    if (p) {
        p->token = ticket, p->dest = -1, p->adult_to = adult_to, p->deadline = deadline;
        return;
    }
    reply_start(&b, id, 1);
    buf_printf(&b, ",\"age\":\"%s\",\"sent\":null", adult_to ? "adult" : "child");
    reply_end(c, &b);
}

/* (pending_tick) what came of a teleport or an age switch: sent, refused, replaced, or not taken in time (then cancelled) */
static int teleport_done(Pending *p, Conn *c, int64_t now) {
    char why[128] = "", map[80] = "";
    int accepted = 0;
    const int r = G.game->teleport_result(p->token, why, sizeof why, map, sizeof map, &accepted);
    if (r == 0 && now < p->deadline) return 0;
    const char *what = p->dest < 0 ? "age switch" : "teleport";
    if (r == 0) {
        if (G.game->teleport_cancel) G.game->teleport_cancel(p->token);
        reply_error(c, p->id, "timeout", "the game did not take the %s: it is taken at Link's next update, in play "
                    "(not on the title, the file select, a pause menu or a load)", what);
    } else if (r == -1) reply_error(c, p->id, "refused", "the game refused the %s: %s", what, why[0] ? why : "no reason given");
    else if (r == -2) reply_error(c, p->id, "cancelled", "a later teleport or age switch replaced it before the game took it");
    else if (r == 2) {   /* (the age switch: Link was that age when the game took it, nothing done) */
        Buf b = {0};
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"age\":\"%s\",\"sent\":false", p->adult_to ? "adult" : "child");
        reply_end(c, &b);
    } else if (p->dest < 0) {
        Buf b = {0};
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"age\":\"%s\",\"sent\":true,\"map\":", p->adult_to ? "adult" : "child");
        if (map[0]) buf_str(&b, map);
        else buf_puts(&b, "null");
        buf_printf(&b, ",\"accepted\":%s", accepted ? "true" : "false");
        reply_end(c, &b);
    } else {
        const char *child = NULL, *adult = NULL;
        G.game->destination(p->dest, &child, &adult);
        Buf b = {0};
        reply_start(&b, p->id, 1);
        destination_json(&b, p->dest, child, adult);
        buf_puts(&b, ",\"sent\":true,\"map\":");
        if (map[0]) buf_str(&b, map);
        else buf_puts(&b, "null");
        buf_printf(&b, ",\"accepted\":%s", accepted ? "true" : "false");
        reply_end(c, &b);
    }
    return 1;
}

static void conn_flush_now(Conn *c);
static void transport_cleanup(void);
static void cmd_quit(int ci, const char *id) {
    Conn *c = &G.conns[ci];
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_puts(&b, ",\"quitting\":true");
    reply_end(c, &b);
    conn_flush_now(c);
    ctl_log("control: quit asked by a client");
    transport_cleanup();
    G.game->quit();
}

/* ---- guest memory (host/control.c's HOS_CONTROL_MEM, docs/control.md): mem_read, mem_read_many, mem_write ---- */
static void buf_hex(Buf *b, const uint8_t *p, uint64_t n) {
    static const char hex[] = "0123456789abcdef";
    buf_put(b, "\"", 1);
    for (uint64_t i = 0; i < n; i++) {
        char two[2] = {hex[p[i] >> 4], hex[p[i] & 15]};
        buf_put(b, two, 2);
    }
    buf_put(b, "\"", 1);
}
static void buf_base64(Buf *b, const uint8_t *p, uint64_t n) {
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    buf_put(b, "\"", 1);
    for (uint64_t i = 0; i < n; i += 3) {
        const uint32_t a = p[i], c0 = i + 1 < n ? p[i + 1] : 0, c1 = i + 2 < n ? p[i + 2] : 0;
        const uint32_t v = a << 16 | c0 << 8 | c1;
        char q[4] = {t[v >> 18 & 63], t[v >> 12 & 63], i + 1 < n ? t[v >> 6 & 63] : '=', i + 2 < n ? t[v & 63] : '='};
        buf_put(b, q, 4);
    }
    buf_put(b, "\"", 1);
}
/* a byte value from two hex digits, or -1 */
static int hexval(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}
static int64_t decode_hex(const char *s, uint8_t *out, uint64_t cap) {
    uint64_t n = 0;
    for (; s[0] && s[1]; s += 2) {
        const int hi = hexval(s[0]), lo = hexval(s[1]);
        if (hi < 0 || lo < 0 || n >= cap) return -1;
        out[n++] = (uint8_t)(hi << 4 | lo);
    }
    return s[0] ? -1 : (int64_t)n;   /* an odd digit left over is bad */
}
static int64_t decode_base64(const char *s, uint8_t *out, uint64_t cap) {
    static signed char inv[256];
    if (!inv[(unsigned char)'A']) {
        memset(inv, -1, sizeof inv);
        const char *t = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; i++) inv[(unsigned char)t[i]] = (signed char)i;
    }
    uint32_t acc = 0;
    int bits = 0;
    uint64_t n = 0;
    for (; *s && *s != '='; s++) {
        const signed char v = inv[(unsigned char)*s];
        if (v < 0) return -1;
        acc = acc << 6 | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (n >= cap) return -1;
            out[n++] = (uint8_t)(acc >> bits);
        }
    }
    return (int64_t)n;
}

/* A memory request, made whole on the bridge's thread, then handed to the game's (mem_service) under G.mu: the game thread
   takes a slot only once it is complete. Its slot, or -1 when all are in use (req's buffer is then still the caller's) */
static int mem_publish(const MemReq *req) {
    int slot = -1;
    ctl_lock(&G.mu);
    for (int i = 0; i < CTL_MEM && slot < 0; i++)
        if (!G.mems[i].used) {
            G.mems[i] = *req;
            G.mems[i].used = 1, G.mems[i].done = 0, G.mems[i].token = ++G.next_token;
            slot = i;
        }
    ctl_unlock(&G.mu);
    return slot;
}
/* an address or a size: a whole JSON number, or a string of decimal digits or 0x and hex digits (a number past 2^53
   loses bits as a double: give it as a string) */
static uint64_t mem_u64(const JDoc *d, int obj, const char *key, int *bad) {
    const JNode *n = j_get(d, obj, key);
    if (!n) return *bad = 1, 0;
    if (n->type == J_NUM) {
        if (n->num < 0 || n->num > 9.2e18 || n->num != floor(n->num)) return *bad = 1, 0;
        return (uint64_t)n->num;
    }
    if (n->type == J_STR) {
        const char *q = n->s;
        const int hex = q[0] == '0' && (q[1] == 'x' || q[1] == 'X');
        if (hex) q += 2;
        if (!*q || strspn(q, hex ? "0123456789abcdefABCDEF" : "0123456789") != strlen(q) || strlen(q) > (hex ? 16u : 20u))
            return *bad = 1, 0;   /* (no sign, no octal, no spaces, no overflow past 64 bits) */
        errno = 0;
        const unsigned long long v = strtoull(q, NULL, hex ? 16 : 10);
        if (errno) return *bad = 1, 0;
        return (uint64_t)v;
    }
    return *bad = 1, 0;
}

/* a request made whole (its buffer malloc'd) to the game thread, and a pending reply for it; the buffer freed when it is
   not taken */
static void mem_submit(Conn *c, int ci, const char *id, MemReq *req, int64_t deadline) {
    Pending *p = pending_new(ci, id, P_MEM);
    if (!p) return free(req->buf), reply_error(c, id, "busy", "too many requests waiting");
    const int slot = mem_publish(req);
    if (slot < 0) return p->used = 0, free(req->buf), reply_error(c, id, "busy", "too many memory requests");
    ctl_lock(&G.mu);
    p->token = G.mems[slot].token;
    ctl_unlock(&G.mu);
    p->mem_slot = slot, p->deadline = deadline;
}
static int mem_off(const char *cmd) { return !strcmp(cmd, "mem_read") || !strcmp(cmd, "mem_read_many") || !strcmp(cmd, "mem_write"); }

static void cmd_mem(int ci, const JDoc *d, int req, const char *id, const char *cmd) {
    Conn *c = &G.conns[ci];
    if (!mem_off(cmd))   /* symbols, or any other mem_* */
        return reply_error(c, id, "unavailable", "no such memory command (named values are not served; docs/control.md)");
    if (!G.mem_level)
        return reply_error(c, id, "forbidden",
                           "guest memory is off: set HOS_CONTROL_MEM=ro for reads or rw for reads and writes");
    if (!G.game->mem_region)
        return reply_error(c, id, "unavailable", "this build does not serve guest memory");
    const int write = !strcmp(cmd, "mem_write");
    if (write && G.mem_level < 2)
        return reply_error(c, id, "forbidden", "writes are off (HOS_CONTROL_MEM=ro): set HOS_CONTROL_MEM=rw for writes");
    const char *fmt = str_arg(d, req, "format");
    const int base64 = fmt && !strcmp(fmt, "base64");
    if (fmt && strcmp(fmt, "hex") && strcmp(fmt, "base64"))
        return reply_error(c, id, "bad_request", "format: hex or base64");
    int bad = 0;
    const int64_t deadline = deadline_arg(d, req, 10, &bad);
    if (bad) return reply_error(c, id, "bad_request", "timeout: seconds");

    if (write) {
        const uint64_t addr = mem_u64(d, req, "addr", &bad);
        const char *hex = str_arg(d, req, "hex"), *b64 = str_arg(d, req, "base64");
        if (bad || (!hex && !b64)) return reply_error(c, id, "bad_request", "mem_write: addr, and hex or base64");
        static uint8_t bytes[CTL_MEM_CAP];
        const int64_t n = hex ? decode_hex(hex, bytes, sizeof bytes) : decode_base64(b64, bytes, sizeof bytes);
        if (n < 0) return reply_error(c, id, "bad_request", "the bytes are not %s, or over %u", hex ? "hex" : "base64", CTL_MEM_CAP);
        if (n == 0) return reply_error(c, id, "bad_request", "no bytes to write");
        static MemReq w;
        memset(&w, 0, sizeof w);
        w.write = 1, w.nranges = 1, w.addr[0] = addr, w.size[0] = (uint64_t)n, w.total = (uint64_t)n;
        w.buf = (uint8_t *)malloc((size_t)n);
        if (!w.buf) return reply_error(c, id, "failed", "out of memory");
        memcpy(w.buf, bytes, (size_t)n);
        return mem_submit(c, ci, id, &w, deadline);
    }

    /* a read: one range (mem_read), or a list (mem_read_many, one snapshot) */
    uint64_t addrs[CTL_MEM_RANGES], sizes[CTL_MEM_RANGES], total = 0;
    int nr = 0;
    if (!strcmp(cmd, "mem_read_many")) {
        const JNode *list = j_get(d, req, "ranges");
        if (!list || list->type != J_ARR) return reply_error(c, id, "bad_request", "mem_read_many: ranges, a list of {addr, size}");
        for (int k = list->first; k >= 0; k = d->n[k].next) {
            if (nr >= CTL_MEM_RANGES) return reply_error(c, id, "bad_request", "more than %d ranges", CTL_MEM_RANGES);
            if (d->n[k].type != J_OBJ) return reply_error(c, id, "bad_request", "ranges: objects");
            addrs[nr] = mem_u64(d, k, "addr", &bad);
            sizes[nr] = mem_u64(d, k, "size", &bad);
            if (bad) return reply_error(c, id, "bad_request", "each range: addr and size");
            if (sizes[nr] > CTL_MEM_CAP) return reply_error(c, id, "bad_request", "at most %u bytes a request", CTL_MEM_CAP);   /* (no sum wraps) */
            total += sizes[nr];
            nr++;
        }
        if (!nr) return reply_error(c, id, "bad_request", "no ranges");
    } else {
        addrs[0] = mem_u64(d, req, "addr", &bad);
        sizes[0] = mem_u64(d, req, "size", &bad);
        if (bad) return reply_error(c, id, "bad_request", "mem_read: addr and size");
        nr = 1, total = sizes[0];
    }
    if (total == 0) return reply_error(c, id, "bad_request", "size: at least one byte");
    if (total > CTL_MEM_CAP) return reply_error(c, id, "bad_request", "at most %u bytes a request", CTL_MEM_CAP);
    static MemReq r;
    memset(&r, 0, sizeof r);
    r.base64 = base64, r.nranges = nr, r.total = total;
    memcpy(r.addr, addrs, sizeof addrs[0] * nr);
    memcpy(r.size, sizes, sizeof sizes[0] * nr);
    r.buf = (uint8_t *)malloc((size_t)total);
    if (!r.buf) return reply_error(c, id, "failed", "out of memory");
    mem_submit(c, ci, id, &r, deadline);
}
/* (pending_tick) the memory op is done on the game thread, or its time ran out */
static int mem_finished(Pending *p, Conn *c, int64_t now) {
    MemReq *m = &G.mems[p->mem_slot];
    ctl_lock(&G.mu);
    const int done = m->done, ok = m->ok, used = m->used && m->token == p->token;
    ctl_unlock(&G.mu);
    if (!used) return reply_error(c, p->id, "failed", "the memory request was dropped"), 1;
    if (!done && now < p->deadline) return 0;
    Buf b = {0};
    if (!done) reply_error(c, p->id, "timeout", "the game did not read the controller in time");
    else if (!ok) reply_error(c, p->id, "out_of_range", "%s", m->err);
    else if (m->write) {
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"wrote\":%llu,\"addr\":%llu", (unsigned long long)m->size[0], (unsigned long long)m->addr[0]);
        reply_end(c, &b);
    } else {
        reply_start(&b, p->id, 1);
        buf_printf(&b, ",\"format\":\"%s\"", m->base64 ? "base64" : "hex");
        if (m->nranges == 1) {
            buf_printf(&b, ",\"addr\":%llu,\"size\":%llu,\"data\":", (unsigned long long)m->addr[0], (unsigned long long)m->size[0]);
            (m->base64 ? buf_base64 : buf_hex)(&b, m->buf, m->size[0]);
        } else {
            buf_puts(&b, ",\"ranges\":[");
            uint64_t off = 0;
            for (int r = 0; r < m->nranges; r++) {
                if (r) buf_puts(&b, ",");
                buf_printf(&b, "{\"addr\":%llu,\"size\":%llu,\"data\":", (unsigned long long)m->addr[r], (unsigned long long)m->size[r]);
                (m->base64 ? buf_base64 : buf_hex)(&b, m->buf + off, m->size[r]);
                buf_puts(&b, "}");
                off += m->size[r];
            }
            buf_puts(&b, "]");
        }
        reply_end(c, &b);
    }
    ctl_lock(&G.mu);
    free(m->buf);
    m->used = 0;
    ctl_unlock(&G.mu);
    return 1;
}

/* ---- saves: the save folder the game uses, listed on the bridge's thread (a slot being written may show a size or time
   between two writes: it is read as it is, never locked) ---- */
typedef struct { char name[64]; int64_t size, mtime; } SaveFile;
typedef struct { char name[16]; SaveFile files[8]; int nfiles; int64_t mtime; } SaveSlot;
/* the entries of a folder: name, size, modification time (Unix seconds), whether a folder. -1 when it cannot be read */
static int dir_list(const char *dir, void (*fn)(void *, const char *, int64_t, int64_t, int), void *arg) {
#ifdef _WIN32
    char pat[1200];
    snprintf(pat, sizeof pat, "%s\\*", dir);
    WIN32_FIND_DATAA f;
    HANDLE h = FindFirstFileA(pat, &f);
    if (h == INVALID_HANDLE_VALUE) return -1;
    do {
        if (!strcmp(f.cFileName, ".") || !strcmp(f.cFileName, "..")) continue;
        const uint64_t ft = (uint64_t)f.ftLastWriteTime.dwHighDateTime << 32 | f.ftLastWriteTime.dwLowDateTime;
        fn(arg, f.cFileName, (int64_t)((uint64_t)f.nFileSizeHigh << 32 | f.nFileSizeLow),
           (int64_t)(ft / 10000000ULL) - 11644473600LL, (f.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0);
    } while (FindNextFileA(h, &f));
    FindClose(h);
#else
    DIR *d = opendir(dir);
    if (!d) return -1;
    for (struct dirent *e; (e = readdir(d));) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1300];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        struct stat st;
        if (stat(p, &st)) continue;   /* (gone meanwhile) */
        fn(arg, e->d_name, (int64_t)st.st_size, (int64_t)st.st_mtime, S_ISDIR(st.st_mode));
    }
    closedir(d);
#endif
    return 0;
}
typedef struct { SaveSlot slots[16]; int nslots; int account, option; } SaveList;
static void top_entry(void *arg, const char *name, int64_t size, int64_t mtime, int is_dir) {
    SaveList *l = (SaveList *)arg;
    (void)size, (void)mtime;
    if (!is_dir) {
        l->account |= !strcmp(name, "account.sav");
        l->option |= !strcmp(name, "option.sav");
    } else if (!strncmp(name, "slot_", 5) && strlen(name) < sizeof l->slots[0].name && l->nslots < 16) {
        snprintf(l->slots[l->nslots++].name, sizeof l->slots[0].name, "%s", name);
    }
}
static void slot_entry(void *arg, const char *name, int64_t size, int64_t mtime, int is_dir) {
    SaveSlot *s = (SaveSlot *)arg;
    if (is_dir || s->nfiles >= 8) return;
    SaveFile *f = &s->files[s->nfiles++];
    snprintf(f->name, sizeof f->name, "%s", name);
    f->size = size, f->mtime = mtime;
    if (mtime > s->mtime) s->mtime = mtime;
}
static int slot_newer(const void *a, const void *b) {
    const int64_t x = ((const SaveSlot *)a)->mtime, y = ((const SaveSlot *)b)->mtime;
    return x < y ? 1 : x > y ? -1 : strcmp(((const SaveSlot *)a)->name, ((const SaveSlot *)b)->name);
}
/* caption.sav (the file select's preview): its scene and map names and its thumbnail (a JPEG: offset and size), when the
   file has the layout seen in this game's saves (magic 04 03 02 01; the names at 684 and 748, the JPEG's length at 1392
   and the JPEG at 1396). Nothing is added when it does not */
static void caption_json(Buf *b, const char *slot_dir) {
    char p[1300];
    snprintf(p, sizeof p, "%s%ccaption.sav", slot_dir, CTL_SEP);
    FILE *f = fopen(p, "rb");
    if (!f) return;
    unsigned char h[1400];
    const size_t n = fread(h, 1, sizeof h, f);
    fseek(f, 0, SEEK_END);
    const long total = ftell(f);
    fclose(f);
    if (n < sizeof h || memcmp(h, "\x04\x03\x02\x01", 4)) return;
    char scene[65] = "", map[65] = "";
    for (int which = 0; which < 2; which++) {
        const unsigned char *q = h + (which ? 748 : 684);
        char *out = which ? map : scene;
        int k = 0;
        for (; k < 64 && q[k]; k++) {
            if (q[k] < 0x20 || q[k] > 0x7e) return;   /* (not text: another layout) */
            out[k] = (char)q[k];
        }
        out[k] = 0;
    }
    const uint32_t jpeg = (uint32_t)h[1392] | (uint32_t)h[1393] << 8 | (uint32_t)h[1394] << 16 | (uint32_t)h[1395] << 24;
    buf_puts(b, ",\"caption\":{\"scene\":");
    buf_str(b, scene);
    buf_puts(b, ",\"map\":");
    buf_str(b, map);
    if (h[1396] == 0xff && h[1397] == 0xd8 && (long)(1396 + jpeg) <= total)
        buf_printf(b, ",\"thumbnail\":{\"format\":\"jpeg\",\"offset\":1396,\"size\":%u}", jpeg);
    buf_puts(b, "}");
}
static void cmd_saves(int ci, const char *id) {
    Conn *c = &G.conns[ci];
    char root[1100];
    if (!G.game->save_root || !G.game->save_root(root, sizeof root))
        return reply_error(c, id, "unavailable", "the game has not opened its saves yet");
    static SaveList l;
    memset(&l, 0, sizeof l);
    if (dir_list(root, top_entry, &l) < 0) return reply_error(c, id, "failed", "%.300s could not be read", root);
    for (int i = 0; i < l.nslots; i++) {
        char dir[1200];
        snprintf(dir, sizeof dir, "%s%c%s", root, CTL_SEP, l.slots[i].name);
        dir_list(dir, slot_entry, &l.slots[i]);
    }
    qsort(l.slots, (size_t)l.nslots, sizeof l.slots[0], slot_newer);
    Buf b = {0};
    reply_start(&b, id, 1);
    buf_puts(&b, ",\"path\":");
    buf_str(&b, root);
    buf_printf(&b, ",\"account\":%s,\"option\":%s,\"slots\":[", l.account ? "true" : "false", l.option ? "true" : "false");
    for (int i = 0; i < l.nslots; i++) {
        const SaveSlot *s = &l.slots[i];
        buf_printf(&b, "%s{\"slot\":\"%s\",\"mtime\":%lld,\"newest\":%s,\"files\":[", i ? "," : "", s->name + 5,
                   (long long)s->mtime, i == 0 ? "true" : "false");
        for (int k = 0; k < s->nfiles; k++) {
            buf_puts(&b, k ? ",{\"name\":" : "{\"name\":");
            buf_str(&b, s->files[k].name);
            buf_printf(&b, ",\"size\":%lld,\"mtime\":%lld}", (long long)s->files[k].size, (long long)s->files[k].mtime);
        }
        buf_puts(&b, "]");
        char dir[1200];
        snprintf(dir, sizeof dir, "%s%c%s", root, CTL_SEP, s->name);
        caption_json(&b, dir);
        buf_puts(&b, "}");
    }
    buf_puts(&b, "]");
    reply_end(c, &b);
}

/* ---- one request line ---- */
static void request(int ci, const char *line, size_t len) {
    Conn *c = &G.conns[ci];
    static JDoc d;
    static char strings[CTL_LINE_MAX + 2];
    while (len && (line[len - 1] == '\r' || line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
    size_t skip = 0;
    while (skip < len && (line[skip] == ' ' || line[skip] == '\t')) skip++;
    if (skip == len) return;   /* (an empty line) */
    const int req = j_parse(&d, line, len, strings);
    if (req < 0) return reply_error(c, "null", "bad_json", "%s", d.err ? d.err : "not JSON");
    char id[256] = "null";
    if (d.n[req].type == J_OBJ) {
        const JNode *idn = j_get(&d, req, "id");
        if (idn) {
            Buf b = {0};
            j_write(&b, &d, j_index(&d, idn));
            const int fits = !b.failed && b.n < 200;   /* (kept short: it goes back in every reply and event line) */
            if (fits) memcpy(id, b.p, b.n + 1);
            buf_free(&b);
            if (!fits) return reply_error(c, "null", "bad_request", "id: too long (at most 200 characters as JSON)");
        }
    }
    if (d.n[req].type != J_OBJ) return reply_error(c, id, "bad_request", "a request is a JSON object");
    const char *cmd = str_arg(&d, req, "cmd");
    if (!cmd) return reply_error(c, id, "bad_request", "no cmd");
    if (!strcmp(cmd, "status")) cmd_status(ci, id);
    else if (!strcmp(cmd, "press") || !strcmp(cmd, "stick") || !strcmp(cmd, "sequence")) cmd_input(ci, &d, req, id, cmd);
    else if (!strcmp(cmd, "release")) cmd_release(ci, &d, req, id);
    else if (!strcmp(cmd, "saves")) cmd_saves(ci, id);
    else if (!strcmp(cmd, "teleport")) cmd_teleport(ci, &d, req, id);
    else if (!strcmp(cmd, "age")) cmd_age(ci, &d, req, id);
    else if (!strcmp(cmd, "shot")) cmd_shot(ci, &d, req, id);
    else if (!strcmp(cmd, "wait")) cmd_wait(ci, &d, req, id);
    else if (!strcmp(cmd, "option_get")) cmd_option_get(ci, &d, req, id);
    else if (!strcmp(cmd, "option_set")) cmd_option_set(ci, &d, req, id);
    else if (!strcmp(cmd, "subscribe")) cmd_subscribe(ci, &d, req, id);
    else if (!strcmp(cmd, "unsubscribe")) {
        c->sub_on = 0;
        log_wanted_update();
        reply_ok(c, id);
    } else if (!strcmp(cmd, "quit")) cmd_quit(ci, id);
    else if (!strncmp(cmd, "mem_", 4) || !strcmp(cmd, "symbols")) cmd_mem(ci, &d, req, id, cmd);
    else reply_error(c, id, "unknown_cmd", "no command %.60s", cmd);
}

/* bytes from a client: whole lines are requests; a line past CTL_LINE_MAX is refused and the rest of it skipped */
static void conn_feed(int ci, const char *data, size_t n) {
    Conn *c = &G.conns[ci];
    while (n && !c->closing) {
        const char *nl = (const char *)memchr(data, '\n', n);
        const size_t part = nl ? (size_t)(nl - data) : n;
        if (c->discarding) {
            if (nl) c->discarding = 0;
        } else if (c->in_n + part > CTL_LINE_MAX) {
            reply_error(c, "null", "too_long", "a request is at most %d bytes", CTL_LINE_MAX);
            c->in_n = 0;
            c->discarding = !nl;
        } else {
            if (!c->in) c->in = (char *)malloc(CTL_LINE_MAX + 1);
            if (!c->in) { c->closing = 1; return; }
            memcpy(c->in + c->in_n, data, part);
            c->in_n += part;
            if (nl) {
                request(ci, c->in, c->in_n);
                c->in_n = 0;
            }
        }
        if (!nl) break;
        data = nl + 1, n -= part + 1;
    }
}

static void conn_reset(Conn *c) {
    free(c->in);
    buf_free(&c->out);
    const int gen = c->gen;
    memset(c, 0, sizeof *c);
    c->gen = gen;
}
/* a request whose client went: its inputs let go, its memory request's buffer freed, its teleport not taken any more */
static void pending_drop(Pending *p) {
    if (p->kind == P_INPUT) {
        ctl_lock(&G.mu);
        inputs_drop(p->group);
        ctl_unlock(&G.mu);
    } else if (p->kind == P_MEM) {
        ctl_lock(&G.mu);
        MemReq *m = &G.mems[p->mem_slot];
        if (m->used && m->token == p->token) free(m->buf), m->used = 0;
        ctl_unlock(&G.mu);
    } else if (p->kind == P_TELEPORT && G.game->teleport_cancel) G.game->teleport_cancel(p->token);
    p->used = 0;
}
/* a client gone: what it waited for is dropped, its inputs let go */
static void conn_gone(int ci) {
    Conn *c = &G.conns[ci];
    for (int i = 0; i < CTL_PENDING; i++) {
        Pending *p = &G.pending[i];
        if (p->used && p->conn == ci && p->gen == c->gen) pending_drop(p);
    }
    c->sub_on = 0;
    log_wanted_update();
}

static void pending_tick(int64_t now) {
    for (int i = 0; i < CTL_PENDING; i++) {
        Pending *p = &G.pending[i];
        if (!p->used) continue;
        Conn *c = conn_of(p->conn, p->gen);
        if (!c) {   /* (its client is going) */
            pending_drop(p);
            continue;
        }
        const int done = p->kind == P_INPUT ? input_done(p, c, now) : p->kind == P_WAIT ? wait_done(p, c, now)
                       : p->kind == P_MEM ? mem_finished(p, c, now) : p->kind == P_TELEPORT ? teleport_done(p, c, now)
                       : shot_finished(p, c, now);
        if (done) p->used = 0;
    }
}

#ifndef _WIN32   /* (a pipe's client cannot shut one side: it closes, or not) */
/* a client that shut its sending side (a pipe from a shell: printf ... | socat): no more requests, and no events; it is
   closed once what it waits for is answered and sent */
static void conn_eof(Conn *c) {
    c->eof = 1, c->sub_on = 0;
    log_wanted_update();
}
static int conn_waiting(int ci) {
    for (int i = 0; i < CTL_PENDING; i++)
        if (G.pending[i].used && G.pending[i].conn == ci && G.pending[i].gen == G.conns[ci].gen) return 1;
    return 0;
}
#endif

/* ---- the transport: a Unix domain socket (Linux), a named pipe (Windows) ---- */
#ifndef _WIN32
static int listen_fd = -1;

/* The socket's folder: this user's, and writable by no one else (no other user can put a file, a link or a socket of their
   own where the bridge binds, unlinks a stale socket or writes shots by a relative path). 0 when it is not (logged) */
static int folder_private(const char *path) {
    char dir[1100];
    snprintf(dir, sizeof dir, "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) snprintf(dir, sizeof dir, ".");
    else if (slash == dir) dir[1] = 0;   /* ("/x.sock": the root) */
    else *slash = 0;
    struct stat st;
    if (stat(dir, &st) || !S_ISDIR(st.st_mode))
        return ctl_log("control: %s: the socket's folder %s is not there", path, dir), 0;
    if (st.st_uid != getuid() || (st.st_mode & (S_IWGRP | S_IWOTH)))
        return ctl_log("control: %s: the socket's folder %s must be this user's and writable by no one else (mode %03o): "
                       "use a folder of your own, as hookshot_ctl.py launch makes (0700, in $XDG_RUNTIME_DIR)",
                       path, dir, (unsigned)(st.st_mode & 0777)), 0;
    return 1;
}
static int transport_open(const char *path) {
    struct sockaddr_un a;
    memset(&a, 0, sizeof a);
    a.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof a.sun_path) return ctl_log("control: %s: the socket's path is too long", path), 0;
    if (!folder_private(path)) return 0;
    strcpy(a.sun_path, path);
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISSOCK(st.st_mode)) return ctl_log("control: %s is there and is not a socket: not replaced", path), 0;
        const int probe = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        const int live = probe >= 0 && connect(probe, (struct sockaddr *)&a, sizeof a) == 0;
        if (probe >= 0) close(probe);
        if (live) return ctl_log("control: %s: another game listens there", path), 0;
        unlink(path);   /* (one left by a game that ended) */
    }
    listen_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (listen_fd < 0) return ctl_log("control: socket: %s", strerror(errno)), 0;
    const mode_t old = umask(077);
    const int bound = bind(listen_fd, (struct sockaddr *)&a, sizeof a);
    umask(old);
    if (bound || chmod(path, 0600) || listen(listen_fd, 4)) {
        ctl_log("control: %s: %s", path, strerror(errno));
        close(listen_fd);
        listen_fd = -1;
        return 0;
    }
    return 1;
}
static void transport_cleanup(void) {
    if (listen_fd >= 0) close(listen_fd), listen_fd = -1, unlink(G.path);
}
static void conn_close(int ci) {
    Conn *c = &G.conns[ci];
    conn_gone(ci);
    if (c->fd >= 0) close(c->fd);
    conn_reset(c);
}
static void conn_write(Conn *c) {
    while (c->out.n) {
        const ssize_t w = send(c->fd, c->out.p, c->out.n, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (w <= 0) {
            if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
            c->closing = 1;
            return;
        }
        memmove(c->out.p, c->out.p + w, c->out.n - (size_t)w);
        c->out.n -= (size_t)w;
    }
    if (c->out.n > CTL_OUT_MAX || c->out.failed) c->closing = 1;
}
static void conn_flush_now(Conn *c) {   /* (quit: the reply out before the process ends, a second at most) */
    const int64_t until = ctl_now_ns() + 1000000000;
    while (c->out.n && !c->closing && ctl_now_ns() < until) {
        conn_write(c);
        struct pollfd p = {c->fd, POLLOUT, 0};
        if (c->out.n) poll(&p, 1, 10);
    }
}
static void transport_wait(int ms) {
    struct pollfd fds[CTL_CONNS + 1];
    int idx[CTL_CONNS + 1], n = 0;
    fds[n].fd = listen_fd, fds[n].events = POLLIN, idx[n++] = -1;
    for (int i = 0; i < CTL_CONNS; i++) {
        Conn *c = &G.conns[i];
        if (!c->used) continue;
        fds[n].fd = c->fd, fds[n].events = (short)((c->eof ? 0 : POLLIN) | (c->out.n ? POLLOUT : 0)), idx[n++] = i;
    }
    if (poll(fds, (nfds_t)n, ms) <= 0) return;
    for (int k = 1; k < n; k++) {
        Conn *c = &G.conns[idx[k]];
        if (fds[k].revents & POLLOUT) conn_write(c);
        if (c->eof) {   /* (only its replies left to send: gone altogether, they cannot be) */
            if (fds[k].revents & (POLLHUP | POLLERR)) c->closing = 1;
        } else if (fds[k].revents & (POLLIN | POLLHUP | POLLERR)) {
            char buf[16384];
            const ssize_t r = recv(c->fd, buf, sizeof buf, MSG_DONTWAIT);
            if (r > 0) conn_feed(idx[k], buf, (size_t)r);
            else if (r == 0) conn_eof(c);
            else if (errno != EAGAIN && errno != EINTR) c->closing = 1;
        }
    }
    if (fds[0].revents & POLLIN) {
        const int fd = accept4(listen_fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (fd < 0) return;
        struct ucred cred;
        socklen_t len = sizeof cred;
        if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cred, &len) || cred.uid != getuid()) {
            ctl_log("control: a client of another user turned away");
            close(fd);
            return;
        }
        int slot = -1;
        for (int i = 0; i < CTL_CONNS && slot < 0; i++)
            if (!G.conns[i].used) slot = i;
        if (slot < 0) {
            static const char full[] = "{\"id\":null,\"ok\":false,\"error\":\"busy\",\"message\":\"too many clients\"}\n";
            (void)!send(fd, full, sizeof full - 1, MSG_NOSIGNAL | MSG_DONTWAIT);
            close(fd);
            return;
        }
        Conn *c = &G.conns[slot];
        conn_reset(c);
        c->used = 1, c->fd = fd, c->gen = ++G.conn_gen;
    }
}
static void transport_flush(void) {
    for (int i = 0; i < CTL_CONNS; i++) {
        Conn *c = &G.conns[i];
        if (c->used && c->out.n && !c->closing) conn_write(c);
        if (c->used && c->eof && !c->closing && !c->out.n && !conn_waiting(i)) c->closing = 1;   /* (all answered) */
        if (c->used && c->closing) conn_close(i);
    }
}
#else
/* Windows: overlapped pipe instances, one waiting for a client and one for each client, all on the bridge's thread */
static char pipe_name[1100];
static HANDLE listen_pipe = INVALID_HANDLE_VALUE;
static OVERLAPPED listen_ov;
static SECURITY_ATTRIBUTES pipe_sa;
static int first_instance = 1, listen_connected;   /* listen_connected: the client came before ConnectNamedPipe was asked */
/* (a refused client's instance, its "busy" reply written: closed a little later, so the reply is read before the instance
   goes. The same for quit's reply below) */
static HANDLE lingering[CTL_CONNS];
static int64_t linger_until[CTL_CONNS];
static void linger(HANDLE h) {
    for (int i = 0; i < CTL_CONNS; i++)
        if (!lingering[i]) {
            lingering[i] = h, linger_until[i] = ctl_now_ns() + 2000000000;
            return;
        }
    CloseHandle(h);
}

static int pipe_security(void) {   /* a DACL with the current user alone */
    HANDLE token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) return 0;
    char info[256];
    DWORD len = 0;
    const BOOL got = GetTokenInformation(token, TokenUser, info, sizeof info, &len);
    CloseHandle(token);
    if (!got) return 0;
    char *sid = NULL;
    if (!ConvertSidToStringSidA(((TOKEN_USER *)info)->User.Sid, &sid)) return 0;
    char sddl[300];
    snprintf(sddl, sizeof sddl, "D:P(A;;GA;;;%s)", sid);
    LocalFree(sid);
    PSECURITY_DESCRIPTOR sd = NULL;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorA(sddl, SDDL_REVISION_1, &sd, NULL)) return 0;
    pipe_sa.nLength = sizeof pipe_sa, pipe_sa.lpSecurityDescriptor = sd, pipe_sa.bInheritHandle = FALSE;
    return 1;
}
static int pipe_listen(void) {
    listen_pipe = CreateNamedPipeA(pipe_name, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first_instance ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
                                   PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                   PIPE_UNLIMITED_INSTANCES, 65536, 65536, 0, &pipe_sa);
    if (listen_pipe == INVALID_HANDLE_VALUE) return 0;
    first_instance = 0;
    if (!listen_ov.hEvent) listen_ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    ResetEvent(listen_ov.hEvent);
    listen_connected = 0;
    if (!ConnectNamedPipe(listen_pipe, &listen_ov)) {
        const DWORD e = GetLastError();
        if (e == ERROR_PIPE_CONNECTED) listen_connected = 1, SetEvent(listen_ov.hEvent);
        else if (e != ERROR_IO_PENDING) return CloseHandle(listen_pipe), listen_pipe = INVALID_HANDLE_VALUE, 0;
    }
    return 1;
}
static int transport_open(const char *path) {
    if (!strncmp(path, "\\\\.\\pipe\\", 9)) snprintf(pipe_name, sizeof pipe_name, "%s", path);
    else snprintf(pipe_name, sizeof pipe_name, "\\\\.\\pipe\\%s", path);
    if (!pipe_security()) return ctl_log("control: the pipe's security could not be made (%lu)", GetLastError()), 0;
    if (!pipe_listen()) return ctl_log("control: %s: %lu (another game listens there?)", pipe_name, GetLastError()), 0;
    return 1;
}
static void transport_cleanup(void) {
    if (listen_pipe != INVALID_HANDLE_VALUE) CloseHandle(listen_pipe), listen_pipe = INVALID_HANDLE_VALUE;
}
static void conn_read_start(int ci) {
    Conn *c = &G.conns[ci];
    ResetEvent(c->rov.hEvent);
    if (!ReadFile(c->pipe, c->rbuf, sizeof c->rbuf, NULL, &c->rov) && GetLastError() != ERROR_IO_PENDING) c->closing = 1;
    else c->reading = 1;
}
static void conn_close(int ci) {
    Conn *c = &G.conns[ci];
    conn_gone(ci);
    if (c->reading) {   /* (the read's OVERLAPPED and buffer are this Conn's: the cancel finishes before they go) */
        DWORD n;
        CancelIoEx(c->pipe, &c->rov);
        GetOverlappedResult(c->pipe, &c->rov, &n, TRUE);
    }
    DisconnectNamedPipe(c->pipe);
    CloseHandle(c->pipe);
    CloseHandle(c->rov.hEvent);
    conn_reset(c);
}
static void conn_write(Conn *c) {   /* (blocking, two seconds at most: the bridge's thread, never the game's) */
    if (!c->out.n || c->closing) return;
    OVERLAPPED ov;
    memset(&ov, 0, sizeof ov);
    ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
    DWORD wrote = 0;
    BOOL ok = WriteFile(c->pipe, c->out.p, (DWORD)c->out.n, NULL, &ov);
    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(ov.hEvent, 2000) != WAIT_OBJECT_0) CancelIoEx(c->pipe, &ov);
        ok = GetOverlappedResult(c->pipe, &ov, &wrote, TRUE);
    } else if (ok) GetOverlappedResult(c->pipe, &ov, &wrote, TRUE);
    CloseHandle(ov.hEvent);
    if (!ok || wrote == 0) { c->closing = 1; return; }
    memmove(c->out.p, c->out.p + wrote, c->out.n - wrote);
    c->out.n -= wrote;
    if (c->out.n > CTL_OUT_MAX || c->out.failed) c->closing = 1;
}
static DWORD WINAPI flush_main(void *h) { FlushFileBuffers((HANDLE)h); return 0; }
static void conn_flush_now(Conn *c) {   /* (quit: written, then read by the client before the process ends, a second at most) */
    conn_write(c);
    HANDLE t = c->closing ? NULL : CreateThread(NULL, 0, flush_main, c->pipe, 0, NULL);
    if (t) WaitForSingleObject(t, 1000), CloseHandle(t);
}
static void transport_wait(int ms) {
    HANDLE events[CTL_CONNS + 1];
    int idx[CTL_CONNS + 1], n = 0;
    if (listen_pipe != INVALID_HANDLE_VALUE) events[n] = listen_ov.hEvent, idx[n++] = -1;
    for (int i = 0; i < CTL_CONNS; i++)
        if (G.conns[i].used && G.conns[i].reading) events[n] = G.conns[i].rov.hEvent, idx[n++] = i;
    if (!n) { Sleep((DWORD)ms); return; }
    const DWORD w = WaitForMultipleObjects((DWORD)n, events, FALSE, (DWORD)ms);
    if (w >= WAIT_OBJECT_0 + (DWORD)n) return;
    const int k = (int)(w - WAIT_OBJECT_0);
    if (idx[k] < 0) {   /* a client: its own instance, and a new one to wait for the next */
        DWORD unused;
        const BOOL connected = listen_connected || GetOverlappedResult(listen_pipe, &listen_ov, &unused, FALSE);
        int slot = -1;
        for (int i = 0; i < CTL_CONNS && slot < 0; i++)
            if (!G.conns[i].used) slot = i;
        if (connected && slot < 0) {   /* (said, then closed: a closed server end keeps what it wrote readable) */
            static const char full[] = "{\"id\":null,\"ok\":false,\"error\":\"busy\",\"message\":\"too many clients\"}\n";
            OVERLAPPED ov;
            memset(&ov, 0, sizeof ov);
            ov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            DWORD wrote = 0;
            if (!WriteFile(listen_pipe, full, sizeof full - 1, NULL, &ov) && GetLastError() == ERROR_IO_PENDING &&
                WaitForSingleObject(ov.hEvent, 200) != WAIT_OBJECT_0)
                CancelIoEx(listen_pipe, &ov);
            GetOverlappedResult(listen_pipe, &ov, &wrote, TRUE);
            CloseHandle(ov.hEvent);
            linger(listen_pipe);
        } else if (!connected) {
            DisconnectNamedPipe(listen_pipe);
            CloseHandle(listen_pipe);
        } else {
            Conn *c = &G.conns[slot];
            conn_reset(c);
            c->used = 1, c->pipe = listen_pipe, c->gen = ++G.conn_gen;
            c->rov.hEvent = CreateEventA(NULL, TRUE, FALSE, NULL);
            conn_read_start(slot);
        }
        listen_pipe = INVALID_HANDLE_VALUE;
        pipe_listen();
        return;
    }
    Conn *c = &G.conns[idx[k]];
    DWORD got = 0;
    c->reading = 0;
    if (!GetOverlappedResult(c->pipe, &c->rov, &got, FALSE) || !got) {
        c->closing = 1;
        return;
    }
    conn_feed(idx[k], c->rbuf, got);
    if (!c->closing) conn_read_start(idx[k]);
}
static void transport_flush(void) {
    for (int i = 0; i < CTL_CONNS; i++) {
        Conn *c = &G.conns[i];
        if (c->used && c->out.n && !c->closing) conn_write(c);
        if (c->used && c->closing) conn_close(i);
    }
    const int64_t now = ctl_now_ns();
    for (int i = 0; i < CTL_CONNS; i++)
        if (lingering[i] && now >= linger_until[i]) CloseHandle(lingering[i]), lingering[i] = NULL;
    if (listen_pipe == INVALID_HANDLE_VALUE) pipe_listen();
}
#endif

/* ---- the bridge's thread ---- */
#ifdef _WIN32
static DWORD WINAPI serve_main(void *unused) {
#else
static void *serve_main(void *unused) {
#endif
    (void)unused;
    for (;;) {
        transport_wait(CTL_TICK_MS);
        const int64_t now = ctl_now_ns();
        stats_tick(now);
        pending_tick(now);
        inputs_tick();
        subs_tick();
        transport_flush();
    }
#ifdef _WIN32
    return 0;   /* (never reached: MinGW gcc wants it all the same, -Werror=return-type) */
#else
    return NULL;
#endif
}

int hos_control_serve(const char *path, const HosControlGame *game) {
    if (G.game) return 0;
    G.game = game;
    G.started_ns = ctl_now_ns();
    snprintf(G.path, sizeof G.path, "%s", path);
    {   /* the run folder: the socket's own (Linux); a pipe has none, so a folder of this user's in the temporary folder,
           named after the pipe (Windows) */
#ifdef _WIN32
        const char *name = !strncmp(path, "\\\\.\\pipe\\", 9) ? path + 9 : path;
        char tmp[MAX_PATH + 1];
        const DWORD n = GetTempPathA(sizeof tmp, tmp);
        snprintf(G.base_dir, sizeof G.base_dir, "%.*shookshot-control-", (int)(n && n < sizeof tmp ? n : 0), tmp);
        size_t k = strlen(G.base_dir);
        for (const char *q = name; *q && k + 2 < sizeof G.base_dir; q++)   /* (a pipe name may hold characters a folder cannot) */
            G.base_dir[k++] = isalnum((unsigned char)*q) || *q == '-' || *q == '_' || *q == '.' ? *q : '_';
        G.base_dir[k++] = '\\', G.base_dir[k] = 0;
        CreateDirectoryA(G.base_dir, NULL);
#else
        snprintf(G.base_dir, sizeof G.base_dir, "%s", path);
        char *slash = strrchr(G.base_dir, '/');
        if (slash) slash[1] = 0;
        else G.base_dir[0] = 0;
#endif
        snprintf(G.shots_dir, sizeof G.shots_dir, "%sshots", G.base_dir);
    }
    G.state = S_BOOT;
    G.mem_level = hos_control_mem_level(getenv("HOS_CONTROL_MEM"));
    if (G.mem_level < 0) {
        ctl_log("control: HOS_CONTROL_MEM=%s is not ro or rw: guest memory stays off", getenv("HOS_CONTROL_MEM"));
        G.mem_level = 0;
    } else if (G.mem_level) {
        ctl_log("control: guest memory %s (HOS_CONTROL_MEM): reads expose all game memory%s", G.mem_level == 2 ? "read and write" : "read only",
                G.mem_level == 2 ? "; writes can corrupt the game and the runtime (use a throwaway save)" : "");
    }
    if (!transport_open(path)) {
        G.game = NULL;
        return 0;
    }
#ifdef _WIN32
    HANDLE t = CreateThread(NULL, 0, serve_main, NULL, 0, NULL);
    if (!t) return transport_cleanup(), G.game = NULL, 0;
    CloseHandle(t);
#else
    pthread_t t;
    if (pthread_create(&t, NULL, serve_main, NULL)) return transport_cleanup(), G.game = NULL, 0;
    pthread_detach(t);
#endif
    hos_control_on = 1;
    return 1;
}

#ifndef HOS_CONTROL_NO_GAME
/* ---- the game's side ---- */
void hos_log(const char *fmt, ...);
void hos_log_flush(void);
#ifndef _WIN32
void ExitProcess(unsigned int code);   /* (host/posix/win32_posix.c; Windows: windows.h) */
#endif
extern volatile int64_t hos_present_count;   /* (display.c, posix/display_sdl.c: the game's presents; LONG64) */
const char *rx_describe(void);
int rx_control_frame_info(int64_t *presents, int64_t *draws, int *width, int *height);
int rx_control_shot(const char *path, int64_t after, void (*done)(void *ctx, int64_t present, int ok), void *ctx);
int rx_control_setting(int index, HosControlSetting *out);   /* (rx_backend.c: MkwVkSetting's fields) */
int rx_control_setting_set(const char *key, int value, HosControlSetting *out);
int hos_mem_region(uint64_t addr, uint64_t size);           /* (hos_mem.c) */
uint8_t *hos_guest_base(void);                              /* (rx_backend.c) */
void hos_mark_written(const void *p, uint64_t n);          /* (hos_mem.c: the GPU mirror picks the pages up) */

static int game_mem_region(uint64_t addr, uint64_t size) { return hos_mem_region(addr, size); }
static void game_mem_read(uint64_t addr, void *dst, uint64_t size) { memcpy(dst, hos_guest_base() + addr, (size_t)size); }
static void game_mem_write(uint64_t addr, const void *src, uint64_t size) {
    memcpy(hos_guest_base() + addr, src, (size_t)size);
    hos_mark_written(hos_guest_base() + addr, size);   /* (the soft write watch: synced into the GPU mirror next frame) */
}
int hos_fs_save_root(char *out, size_t cap);   /* (nn_fs.c) */
static int game_save_root(char *out, size_t cap) {
    if (!hos_fs_save_root(out, cap)) return 0;
#ifndef _WIN32
    for (char *p = out; *p; p++)   /* (nn_fs.c writes Windows separators; the POSIX layer takes either) */
        if (*p == '\\') *p = '/';
#endif
    return 1;
}
static int64_t game_frame(void) { return __atomic_load_n(&hos_present_count, __ATOMIC_RELAXED); }
static void game_log(const char *line) { hos_log("%s", line); }
void rx_close_asked(void);   /* (rx_backend.c) */
static void game_quit(void) {   /* (as the window's close: the pipelines made since the last save kept, then out) */
    rx_close_asked();
    hos_log("control: exiting");
    hos_log_flush();
    ExitProcess(0);
}
static const char *game_renderer(void) { return rx_describe(); }
int hos_oot_current_map(char *out, size_t cap);                              /* (hos_main.c) */
int hos_oot_destination(int index, const char **child, const char **adult);
int hos_oot_teleport_ask(int index);
int hos_oot_teleport_result(int ticket, char *why, size_t why_cap, char *map, size_t map_cap, int *accepted);
void hos_oot_teleport_cancel(int ticket);
int hos_oot_age_ask(int adult);
int hos_oot_state(int *event, int *loading, int *child);
static HosControlGame game = {
    .frame = game_frame, .frame_info = rx_control_frame_info, .shot = rx_control_shot, .setting = rx_control_setting,
    .setting_set = rx_control_setting_set, .mem_region = game_mem_region, .mem_read = game_mem_read,
    .mem_write = game_mem_write, .save_root = game_save_root, .log = game_log, .quit = game_quit,
    .renderer = game_renderer, .map = hos_oot_current_map, .destination = hos_oot_destination,
    .teleport = hos_oot_teleport_ask, .teleport_result = hos_oot_teleport_result,
    .teleport_cancel = hos_oot_teleport_cancel, .age = hos_oot_age_ask, .oot_state = hos_oot_state};

void hos_control_start(const char *version) {
    const char *path = getenv("HOS_CONTROL");
    if (!path || !*path) return;
    game.version = version;
    if (hos_control_serve(path, &game))
        hos_log("control: listening on %s (HOS_CONTROL: this user only, no TCP; guest memory %s; docs/control.md)", path,
                G.mem_level == 2 ? "read and write" : G.mem_level == 1 ? "read only" : "off");
    else hos_log("control: off (HOS_CONTROL=%s could not be served)", path);
}
#endif
