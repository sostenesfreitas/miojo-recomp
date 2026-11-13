/* The mouse in the game's own menus: the title and its lists, the pause menu, the panel of
   Key Items, Threads of Time and the Map, the options, the save files' dialogs. The mouse frees itself over them
   (display.c), points at their buttons (the focus as the pad moves it), clicks them (A), backs out (the right button:
   B) and scrolls their lists. nx2/oot_patches.py has two of the game's functions call here first (the supported build
   only):
   - f_802ed660 (in the pause menu's vtable at 0x84c0bc18), the pause menu's handler of a pane getting the focus: it
     matches the pane's name against the pages (PaItemMenu_00, PaEquipmentMenu_00, PaCollectMenu_00, PaSystemMenu_00)
     and hands the pane to the page (f_802ebd20 on the Items page: the info panel). The pause menu's page, for its tabs.
   - f_802f1380, every menu screen's update (each frame, before its UI controller at +56 runs the buttons' requests):
     the screens are tracked, and the mouse is for the one in front (hos_oot_menu_tick).
   The focus setter is f_8088ffc0(screen, button, flag), as the game's screens call it (f_80795090): it finds the button's
   entry in the screen's list (+136 the head, +144 the first, +156 the entry's offset from its node; entry+24 the button)
   and hands it to the focus manager (*(screen+32)+96, its vtable +48), when the manager takes the player's input.
   HOS_MENU_TRACE=1: the focus changes, the screens and what the mouse does logged. */
#include "hos.h"
#ifndef HOOKSHOT_ENV_H   /* (env_on: the switches' one rule) */
#include "../gpu/vk/compat/hookshot_env.h"
#endif
#if defined(_WIN32)
#include <windows.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../gpu/vk/vk_api.h"   /* (HOS_MOUSE_*) */

/* A guest read, of the pointers the game's objects hold: inside the guest's space (below 1 MB: a null pointer's), it is
   read as the lifted code reads it; a page there not committed yet is committed on the touch (mkw_glue.c
   commit_on_touch), so a stray pointer costs a page, not a fault. Not ReadProcessMemory: a call into the kernel each,
   hundreds a frame */
static int peek(uint64_t a, void *dst, size_t n) {
    if (a < 0x100000 || a >= HOS_GUEST_SPACE || n > HOS_GUEST_SPACE - a) return 0;
    memcpy(dst, GP(a), n);
    return 1;
}
static uint64_t peek64(uint64_t a) {
    uint64_t v = 0;
    return peek(a, &v, 8) ? v : 0;
}
/* a printable C string at a (else "-") */
static const char *text_at(uint64_t a, char *buf, size_t cap) {
    if (!peek(a, buf, cap - 1)) return "-";
    buf[cap - 1] = 0;
    for (size_t i = 0; buf[i]; i++)
        if ((unsigned char)buf[i] < 32 || (unsigned char)buf[i] > 126) return i > 2 ? (buf[i] = 0, buf) : "-";
    return buf[0] ? buf : "-";
}

static int trace = -1;
/* (the screen f_802ed660 ran for: written under mouse_lock, read by every screen's update under seen_lock or none,
   so read and written whole) */
static volatile LONG64 pause_screen;
static uint64_t pause_screen_now(void) { return (uint64_t)InterlockedCompareExchange64(&pause_screen, 0, 0); }
static uint64_t focus_page;     /* (the focused pane's page object: a change is a page switch) */
static volatile LONG ticks;     /* (the pause menu's updates since its focus came: for the trace only) */
static int tab_goal = -1, tab_pages, tab_from = -9;   /* (a clicked tab: the page to step to, and how many the menu has) */
static int64_t goal_ns;
static uint64_t focused_button;   /* (the last pane f_802ed660 was handed) */
static uint64_t asked;   /* (the button the mouse asked the focus for, until it has it) */
static int page_now = -1;   /* (the shown page: 0 Collection, 1 Equipment, 2 Items, 3 System; L and R step through them) */

/* the screen's list of entries: fn(entry, button) for each, at most max */
static int each_entry(uint64_t screen, int max, void (*fn)(int i, uint64_t entry, uint64_t button, void *arg), void *arg) {
    const uint64_t head = screen + 136;
    int32_t off = 0;
    peek(screen + 156, &off, 4);
    uint64_t node = peek64(screen + 144);
    int i = 0;
    for (; node && node != head && i < max; i++) {
        const uint64_t entry = node - (uint64_t)(int64_t)off;
        if (fn) fn(i, entry, peek64(entry + 24), arg);
        node = peek64(node + 8);
    }
    return i;
}

/* the game's focus setter on a button (on the game's thread, at a screen's update) */
static int set_focus(Ctx *c, uint64_t screen, uint64_t button) {
    A64Fn fn = a64_lookup(0x8088ffc0ULL);
    if (!fn) return -1;
    const Ctx saved = *c;
    c->x[0] = screen, c->x[1] = button, c->x[2] = 0;
    c->x[30] = 0;
    fn(c);
    const int r = (int)(c->x[0] & 1);
    *c = saved;
    return r;
}

/* A call of a guest function from the host, on the game's thread (x0, x1 in; x0 out, s0 and s1 too: a position) */
static uint64_t guest_call(Ctx *c, uint64_t fn_addr, uint64_t a0, uint64_t a1, float *s0, float *s1) {
    A64Fn fn = a64_lookup(fn_addr);
    if (!fn) return 0;
    const Ctx saved = *c;
    c->x[0] = a0, c->x[1] = a1, c->x[30] = 0;
    fn(c);
    const uint64_t r = c->x[0];
    if (s0) memcpy(s0, &c->vreg[0][0], 4);
    if (s1) memcpy(s1, &c->vreg[1][0], 4);
    *c = saved;
    return r;
}

/* Key Items' items where the page shows them, as the page finds them to place its cursor (f_833fdaa0): the item
   database (**0x84c9e0d0), its lists 4 and 3 (f_80a8f690: +8 the count, +16 the entries); an entry's key (f_8002a520,
   f_80d666a0(.., 0), f_8002a4c0: a string, its text first) and its pane's name (f_80a963c0: its text first); the pane in
   the screen's layout (f_8078e2b0(*(screen+40), name)) and its position (f_8078fcb0). fn(key, x, y) for each */
static int item_panes(Ctx *c, uint64_t screen, void (*fn)(const char *key, float x, float y, void *arg), void *arg) {
    const uint64_t db = peek64(peek64(0x84c9e0d0ULL)), layout = peek64(screen + 40);
    if (!db || !layout) return 0;
    int n = 0;
    for (int which = 4; which >= 3; which--) {
        const uint64_t list = guest_call(c, 0x80a8f690ULL, db, (uint64_t)which, NULL, NULL);
        int32_t count = 0;
        peek(list + 8, &count, 4);
        const uint64_t entries = peek64(list + 16);
        for (int i = 0; i < count && i < 32; i++) {
            const uint64_t e = peek64(entries + 8ull * (unsigned)i);
            if (!e) continue;
            uint64_t k = guest_call(c, 0x8002a520ULL, e, 0, NULL, NULL);
            k = guest_call(c, 0x80d666a0ULL, k, 0, NULL, NULL);
            k = guest_call(c, 0x8002a4c0ULL, k, 0, NULL, NULL);
            char key[40];
            if (!k || !peek(peek64(k), key, sizeof key - 1)) continue;
            key[sizeof key - 1] = 0;
            const uint64_t named = guest_call(c, 0x80a963c0ULL, e, 0, NULL, NULL);
            const uint64_t pane = named ? guest_call(c, 0x8078e2b0ULL, layout, peek64(named), NULL, NULL) : 0;
            float x = 0, y = 0;
            if (!pane) continue;
            guest_call(c, 0x8078fcb0ULL, pane, 0, &x, &y);
            if (x == x && y == y) fn(key, x, y, arg), n++;
        }
    }
    return n;
}

/* Key Items' setter: the item's key into the page's key buffer (*(screen+760), its room at +768) and the page's refresh
   f_833fda00(screen, screen+760) (the cursor's key and the item's name and description; the page's update places the
   cursor from the key next frame), as the page does when it opens. Only while the page browses (+840: 0; 1 its
   details shown) */
static int set_item_key(Ctx *c, uint64_t screen, const char *key) {
    const uint64_t buf = peek64(screen + 760);
    int32_t room = 0, mode = 1;
    peek(screen + 768, &room, 4);
    peek(screen + 840, &mode, 4);
    const size_t n = strlen(key);
    A64Fn fn = a64_lookup(0x833fda00ULL);
    if (!fn || !buf || buf + n + 1 > HOS_GUEST_SPACE || room <= (int32_t)n || mode != 0) return 0;
    memcpy(GP(buf), key, n + 1);
    const Ctx saved = *c;
    c->x[0] = screen, c->x[1] = screen + 760, c->x[30] = 0;
    fn(c);
    *c = saved;
    return 1;
}

static SRWLOCK mouse_lock;
static volatile DWORD mouse_owner;
/* A focus change the hook could not hand over at once (mouse_lock busy: mouse_tick holds it on another of the game's
   threads, across its calls into the game): kept, the latest only, for mouse_tick to take. The hook never waits for
   mouse_lock, as that thread's game code might wait on this one. pending_lock is held only to copy these words */
static SRWLOCK pending_lock = SRWLOCK_INIT;
static struct { uint64_t screen, pane, lr; int set; } pending;
static void focus_locked(uint64_t screen, uint64_t pane, uint64_t lr);
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) {
    (void)c;
    if (trace < 0) trace = env_on("HOS_MENU_TRACE");
    if (mouse_owner == GetCurrentThreadId()) {   /* (inside mouse_tick's setter, on its thread: the lock is this thread's) */
        focus_locked(screen, pane, lr);
        return;
    }
    const int mine = TryAcquireSRWLockExclusive(&mouse_lock);
    AcquireSRWLockExclusive(&pending_lock);
    pending.screen = screen, pending.pane = pane, pending.lr = lr, pending.set = !mine;   /* (a newer one than any kept) */
    ReleaseSRWLockExclusive(&pending_lock);
    if (!mine) return;
    focus_locked(screen, pane, lr);
    ReleaseSRWLockExclusive(&mouse_lock);
}
/* (mouse_tick, under mouse_lock) the focus change kept for it, if any */
static void focus_pending(void) {
    AcquireSRWLockExclusive(&pending_lock);
    const int set = pending.set;
    const uint64_t screen = pending.screen, pane = pending.pane, lr = pending.lr;
    pending.set = 0;
    ReleaseSRWLockExclusive(&pending_lock);
    if (set) focus_locked(screen, pane, lr);
}
static void focus_locked(uint64_t screen, uint64_t pane, uint64_t lr) {
    if (screen != pause_screen_now()) InterlockedExchange64(&pause_screen, (LONG64)screen), InterlockedExchange(&ticks, 0);
    uint8_t flags = 0;
    peek(pane + 65, &flags, 1);
    const uint64_t at = (flags & 0x20) ? peek64(pane + 32) + 48 : pane + 32;
    const uint64_t obj = peek64(at);
    focus_page = obj;
    focused_button = pane, asked = 0;
    /* (f_807923a0's name: obj+128 -> +248 -> +24) */
    char a[96], b[64];
    {
        static const char *const pages[] = {"PaCollectMenu_00", "PaEquipmentMenu_00", "PaItemMenu_00", "PaSystemMenu_00"};
        const char *page = text_at(peek64(peek64(peek64(obj + 128) + 248) + 24), a, sizeof a);
        for (int i = 0; i < 4; i++)
            if (!strcmp(page, pages[i])) page_now = i;
    }
    if (!trace) return;
    hos_log("menu: focus (caller %#llx, tick %u) screen %#llx pane %#llx %s page %#llx %s; owner screen %#llx",
            (unsigned long long)lr, (unsigned)ticks, (unsigned long long)screen, (unsigned long long)pane,
            text_at(peek64(pane + 0x18), b, sizeof b), (unsigned long long)obj,
            text_at(peek64(peek64(peek64(obj + 128) + 248) + 24), a, sizeof a),
            (unsigned long long)peek64(peek64(pane + 32) + 40));
}

/* ---- the mouse. The layout's units: 3840 x 2160, the origin at the screen's centre, y up; the window shows it fit
   (the larger of the two scales) and centred */
void hos_mouse_game_menu(void);   /* (display.c) */
void hos_mouse_game_menu_gone(void);
int hos_mouse_game_pointer(int *x, int *y, int *w, int *h, uint32_t *pressed, int *wheel);
void hos_hid_tap(const char *press);   /* (nn_hid.c) */
void hos_hid_stick(int lx, int ly, int rx, int ry, int hold_ms);
void hos_hid_drag(int dx, int dy, int right);   /* (nn_hid.c: as the camera's mouse; the left stick or the right) */
/* The panel of Key Items, Threads of Time and Map (Minus opens it; L to the left, R to the right; pages 0, 1, 2), as
   the screens told apart by vtable: Key Items' screen 0x84c07798 and Threads of Time's
   0x84c04a18 (its list of threads: N_SlideList_00, the framework's buttons) each run only while their page is shown;
   the Map page has no screen of its own: the panel's frame 0x84c08aa0 runs alone (it runs under the pause menu too).
   The mouse is the page's while it is shown (no focused button on Key Items or the Map: the stones, the floors and the
   world map are not the framework's buttons). 0x84c08300, its 30 Pa_MelodyIcon_00, is the game's HUD of songs */
#define PANEL_FRAME_VTABLE 0x84c08aa0ULL
static volatile int map_page;   /* (hos_oot_menu_tick: the frame runs, and neither other page nor the pause menu) */
static int panel_page_vt(uint64_t vt) {
    return vt == 0x84c07798ULL ? 0 : vt == 0x84c04a18ULL ? 1 : vt == PANEL_FRAME_VTABLE && map_page ? 2 : -1;
}
static int panel_page(uint64_t screen) { return panel_page_vt(peek64(screen)); }
/* The game's HUD screen (0x84c08300, the songs' icons; it runs through the play) holds the map page's floors (a
   search of memory, in Lord Jabu-Jabu's Belly): +0x1368 the floor chosen (2F 1, 1F 0, B1 -1), +0x136c how
   many floors, +0x1370 the lowest; the top is the lowest + the count - 1. Kept as the screens are seen */
#define HUD_SCREEN_VTABLE 0x84c08300ULL
static volatile uint64_t hud_screen;
static int map_floors(int *cur, int *top, int *bottom) {
    int32_t f[3];
    if (!hud_screen || !peek(hud_screen + 0x1368, f, sizeof f) || f[1] < 1 || f[1] > 12) return 0;
    *cur = f[0], *bottom = f[2], *top = f[2] + f[1] - 1;
    return *cur >= *bottom && *cur <= *top;
}

typedef struct { float x0, y0, x1, y1; } Rect;
/* (Key Items' items, in their grid's order: item_found puts the page's own positions of them in) */
static const char *const item_keys[9] = {"Item_Medal_Shadow", "Item_Medal_Light", "Item_Medal_Forest", "Item_Stone_Forest",
                                         "Item_Stone_Fire", "Item_Stone_Water", "Item_Medal_Spirit", "Item_Medal_Water",
                                         "Item_Medal_Fire"};
static void item_found(const char *key, float x, float y, void *arg) {
    struct { int *found; float *x, *y; } *a = arg;
    for (int i = 0; i < 9; i++) {
        /* (the game's strings hold a length: the text need not end in a NUL, so the key's end is where a name's can't go) */
        const size_t n = strlen(item_keys[i]);
        const char after = key[n];
        if (!strncmp(key, item_keys[i], n) && !isalnum((unsigned char)after) && after != '_')
            *a->found |= 1 << i, a->x[i] = x, a->y[i] = y;
    }
}
/* a button's rectangle in the layout's units (0 when it has none or is hidden) */
static int button_rect(uint64_t button, Rect *r);

typedef struct { float x, y; uint64_t hit; float area; int n; uint64_t button[128]; Rect rect[128]; } Hit;
static void shown_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)i, (void)entry;
    Hit *h = arg;
    if (h->n < 128 && button_rect(button, &h->rect[h->n])) h->button[h->n++] = button;
}

/* The boxes as the mouse takes them, from the game's: a box under 100 x 100 (the title's: 60 x 60, a cursor's mark at the
   middle of a line of text) made 1100 wide; then boxes one above the other (centres less than 60 apart across) grown to
   meet halfway across a gap under 240, and the column's ends by as much as the gap they have (no dead lines between
   the rows of a list, and its first and last rows as tall as the others) */
static void list_boxes(Hit *h) {
    for (int i = 0; i < h->n; i++) {
        Rect *r = &h->rect[i];
        if (r->x1 - r->x0 < 100 && r->y1 - r->y0 < 100) {
            const float cx = (r->x0 + r->x1) / 2;
            r->x0 = cx - 550, r->x1 = cx + 550;
        }
    }
    Rect out[128];
    for (int i = 0; i < h->n; i++) {
        const Rect *r = &h->rect[i];
        const float cx = (r->x0 + r->x1) / 2;
        float above = -1, below = -1;   /* (the gaps to the nearest box above and below in the column) */
        for (int j = 0; j < h->n; j++) {
            const Rect *o = &h->rect[j];
            const float ox = (o->x0 + o->x1) / 2;
            if (j == i || ox - cx > 60 || cx - ox > 60) continue;
            const float up = o->y0 - r->y1, down = r->y0 - o->y1;
            if (up >= 0 && up < 240 && (above < 0 || up < above)) above = up;
            if (down >= 0 && down < 240 && (below < 0 || down < below)) below = down;
        }
        out[i] = *r;
        if (above >= 0) out[i].y1 += above / 2;
        if (below >= 0) out[i].y0 -= below / 2;
        if (above >= 0 && below < 0) out[i].y0 -= above / 2;   /* (an end of the column) */
        if (below >= 0 && above < 0) out[i].y1 += below / 2;
    }
    memcpy(h->rect, out, (size_t)h->n * sizeof *out);
}

static void hit_test(uint64_t screen, Hit *h) {
    h->n = 0, h->hit = 0;
    each_entry(screen, 400, shown_entry, h);
    list_boxes(h);
    if (h->y > -1080 && h->y < -960 && h->x >= 1600 && h->x < 1900) return;   /* (the "Back" prompt's: no button's) */
    for (int i = 0; i < h->n; i++) {
        const Rect *r = &h->rect[i];
        if (h->x < r->x0 || h->x > r->x1 || h->y < r->y0 || h->y > r->y1) continue;
        const float area = (r->x1 - r->x0) * (r->y1 - r->y0);
        if (!h->hit || area < h->area) h->hit = h->button[i], h->area = area;   /* (the smallest: a slot inside a panel) */
    }
}

/* The tab bar across the top (no buttons of the screen's list: the pages' names and the L and R signs), in the layout's
   units, measured from the menu as shown (the Items page): the names centred at about -696, -228, 240, 708 (468 apart),
   L at -1002 and R at 1014; the boxes meet halfway between */
enum { TAB_L = 4, TAB_R = 5 };
static int tab_at(float x, float y) {
    if (y < 900 || y > 1080) return -1;
    if (x >= -1100 && x < -926) return TAB_L;
    if (x >= -926 && x < 942) return x < -462 ? 0 : x < 6 ? 1 : x < 474 ? 2 : 3;
    if (x >= 942 && x <= 1100) return TAB_R;
    return -1;
}

static void focus_entry(int i, uint64_t entry, uint64_t button, void *arg);   /* (below: a focused button shown) */

/* The mouse's state below is one, and the screens' updates run on several of the game's threads: one at a time (a
   frame the lock is busy is passed, not waited for); the focus hook's writes take it too */
static SRWLOCK mouse_lock = SRWLOCK_INIT;
static void mouse_tick_locked(Ctx *c, uint64_t screen);
/* (mouse_owner: the thread in mouse_tick: the game's focus handler may run inside its setter) */
static void mouse_tick(Ctx *c, uint64_t screen) {
    if (!TryAcquireSRWLockExclusive(&mouse_lock)) return;
    mouse_owner = GetCurrentThreadId();
    focus_pending();
    mouse_tick_locked(c, screen);
    mouse_owner = 0;
    ReleaseSRWLockExclusive(&mouse_lock);
}
/* The R button's item choice (0x84c0d430, N_SlideList_00: a carousel of the items in a row, shown while R is held in
   play, turned by the D-pad's or the left stick's left and right, not wrapping; its items move from one end to the
   other as it turns, so no row stays the next). The mouse stays the camera's (the menu does not free it: no cursor, no
   pointing); the wheel turns it (hos_mouse_wheel_take, captured or not): the D-pad's right (the wheel down) or left, a
   notch each. Each press after the last one queued has been let go (60 ms down, 60 up: a notch the next frame would
   otherwise press while the last is held, and the game sees one press), at most 0.72 s ahead, so the row stops soon
   after the wheel */
#define ITEM_CHOICE_VTABLE 0x84c0d430ULL
int hos_mouse_wheel_take(void);   /* (display.c) */
static void item_choice_tick(void) {
    static int64_t dpad_free_ns;   /* (when the last press queued here has been let go) */
    const int wheel = hos_mouse_wheel_take();
    if (!wheel) return;
    const int64_t now = hos_now_ns();
    const int k = wheel < 0 ? -wheel : wheel;
    char taps[128];
    int n = 0;
    if (dpad_free_ns < now) dpad_free_ns = now;
    for (int i = 0; i < k && i < 6 && dpad_free_ns - now < 720000000; i++) {
        n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.3f:%s/60", n ? "," : "", (double)(dpad_free_ns - now) / 1e9,
                      wheel < 0 ? "RIGHT" : "LEFT");
        dpad_free_ns += 120000000;
    }
    if (n) hos_hid_tap(taps);
    if (trace) hos_log("menu: wheel %d: the item choice turned %s", wheel, wheel < 0 ? "right" : "left");
}

static void mouse_tick_locked(Ctx *c, uint64_t screen) {
    if (peek64(screen) == ITEM_CHOICE_VTABLE) {
        item_choice_tick();
        return;
    }
    hos_mouse_game_menu();
    int px, py, w, h, wheel;
    uint32_t pressed;
    const int got = hos_mouse_game_pointer(&px, &py, &w, &h, &pressed, &wheel);
    if (!got) return;
    const int hidden = got == 2;   /* (the cursor hidden while the pad is in use: the wheel only, no pointing) */
    const int pause = screen == pause_screen_now();
    const float scale = 3840.0f / (float)w > 2160.0f / (float)h ? 3840.0f / (float)w : 2160.0f / (float)h;
    static Hit hit;
    hit.x = (float)(px - w / 2) * scale, hit.y = (float)(h / 2 - py) * scale, hit.hit = 0;
    static int last_x = -1, last_y = -1;
    int moved = !hidden && (px != last_x || py != last_y);
    if (!hidden) last_x = px, last_y = py;
    /* The panel's pages: the left button dragged pans the map and the tapestry (the left stick, against the drag: the
       picture follows the mouse) and turns Key Items' stone (the right stick, with the drag), as the camera's mouse
       turns it (hos_hid_drag); pressed and let go without a drag it is a click, at its release. The middle button is Y
       on the map (the world map and back). The wheel off a list zooms (the right stick up or down, 0.12 s a notch:
       the world map's two steps) */
    const int page = panel_page(screen), map = page == 2, keys = page == 0;
    /* The pause menu's Items page: the chosen item's model at the right, turned left and right by the right stick (as
       Key Items' stones are): a drag there turns it, off the buttons; the page's slots at the left (to x about 100),
       the tabs above (y 900), the prompts below (-960) */
    const int item_model = pause && page_now == 2, pan = page >= 0 || item_model;
    static int ldown, ldrag, lx0, ly0, llx, lly;
    if (pan) {
        const int held = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0;
        const int was_down = ldown;
        int grab = !item_model;
        if (item_model && (pressed & HOS_MOUSE_LEFT) && !ldown && hit.x > 300 && hit.x < 1900 && hit.y > -900 && hit.y < 880) {
            hit_test(screen, &hit);
            grab = !hit.hit;
        }
        if ((pressed & HOS_MOUSE_LEFT) && !ldown && grab) ldown = 1, ldrag = 0, lx0 = llx = px, ly0 = lly = py;
        if (ldown) pressed &= ~(uint32_t)HOS_MOUSE_LEFT;   /* (a click is at the release) */
        if (ldown && held) {
            if (!ldrag && (px - lx0 > 6 || lx0 - px > 6 || py - ly0 > 6 || ly0 - py > 6)) {
                ldrag = 1;
                if (trace) hos_log("menu: drag from (%d, %d)", lx0, ly0);
            }
            if (ldrag && (px != llx || py != lly)) {   /* (against the drag: the picture follows the mouse) */
                if (keys) hos_hid_drag(px - llx, -(py - lly), 1);
                else if (item_model) hos_hid_drag(px - llx, 0, 1);   /* (left and right only) */
                else hos_hid_drag(-(px - llx), py - lly, 0);
                static int64_t logged;
                if (trace && hos_now_ns() - logged > 250000000)
                    logged = hos_now_ns(), hos_log("menu: drag: %d, %d pixels", px - llx, py - lly);
            }
            llx = px, lly = py;
        } else if (ldown && was_down) {
            if (!ldrag) {   /* (a click: where it was pressed) */
                pressed |= HOS_MOUSE_LEFT;
                px = lx0, py = ly0;
                hit.x = (float)(px - w / 2) * scale, hit.y = (float)(h / 2 - py) * scale;
            }
            if (trace) hos_log("menu: left button up: %s", ldrag ? "the drag's end" : "a click");
            ldown = 0;
        }
        if (map && (pressed & HOS_MOUSE_MIDDLE)) hos_hid_tap("0:Y/80");
        if (ldown) moved = 0;   /* (no hover while dragging) */
    }
    /* the wheel: the pause menu's pages (L, R). Elsewhere it scrolls a list: the focus to the row shown last that way
       (the lowest for down), then the pad's down (up) a notch each, which moves the list on by a row as at its end;
       then the focus back to the row the mouse is over */
    static int64_t refocus_ns;
    static uint64_t scroll_row;   /* (the row the wheel gave the focus, out of view) */
    static float scroll_lo, scroll_hi;
    const int64_t now = hos_now_ns();
    if (wheel) {
        char taps[128];
        int n = 0;
        const int k = wheel < 0 ? -wheel : wheel;
        float at = 0;
        int list = !keys, pan_off = keys;   /* (the wheel scrolls a list, or turns the pages; nothing on Key Items) */
        /* The Map page, anywhere: a dungeon's floors (the pad's up and down, a notch each) and the world map's zoom (the
           right stick, below): each view takes its own and lets the other be (the world map has nothing on the pad's up
           and down). The view shown is not known */
        if (map) {
            const char *dir = wheel > 0 ? "UP" : "DOWN";
            for (int i = 0; i < k && i < 4; i++)
                n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.2f:%s/75", n ? "," : "", i * 0.25, dir);
            if (trace) hos_log("menu: map wheel %d: the floors (%s) and the zoom", wheel, dir);
            list = 0;
        }
        if ((map || page == 1) && !pan_off) {   /* (off the list, or no row of it focused: the world map's, the tapestry's: the zoom) */
            int focused = 0;
            each_entry(screen, 400, focus_entry, &focused);
            /* (Threads of Time: its list focused, the wheel scrolls it wherever the mouse is; the tapestry shown (the
               list without a focused row) zooms. The map: the list's rows under the mouse scroll it, else the zoom) */
            if (focused && page != 1) hit_test(screen, &hit);
            if (!focused || (page != 1 && !hit.hit)) {
                hos_hid_stick(0, 0, 0, wheel > 0 ? 32767 : -32767, 120 * (k < 6 ? k : 6)), list = 0;
                if (trace) hos_log("menu: map wheel %d: the zoom (right stick %s for %d ms)", wheel, wheel > 0 ? "up" : "down", 120 * (k < 6 ? k : 6));
            }
            hit.hit = 0;
        }
        if (list && !pause) {
            /* A list's rows in their order down the screen. The wheel's target is the row the pad's down would bring
               in from the lowest row shown (up: the highest), a notch each: the first rows off the screen that way (a
               scrolled list lays out all its items, or keeps a spare row for the next). Given the focus, the list
               scrolls it in, without the pad's sound; the focus goes back to the row under the mouse once the list has
               moved. With no row off the screen that way (a short list: a dungeon's floors, or a list's end) the focus
               steps from the focused row to the next, and stays */
            hit_test(screen, &hit);
            int order[128], n_rows = 0, at_focus = -1;
            for (int i = 0; i < hit.n; i++) {
                int j = n_rows++;
                const float cy = (hit.rect[i].y0 + hit.rect[i].y1) / 2;
                while (j > 0 && (hit.rect[order[j - 1]].y0 + hit.rect[order[j - 1]].y1) / 2 < cy) order[j] = order[j - 1], j--;
                order[j] = i;
            }
            for (int j = 0; j < n_rows; j++) {
                uint8_t st = 0;
                peek(hit.button[order[j]] + 56, &st, 1);
                if (st && at_focus < 0) at_focus = j;
            }
            const int step = wheel < 0 ? 1 : -1, want = (k < 3 ? k : 3) * step;
            /* (the rows in view: the screen's height, or Threads of Time's list frame, y -756 to 594 (from the page as
               shown): the game takes a row as shown once its centre is in it, and scrolls a focused row only that far) */
            const int threads = peek64(screen) == 0x84c04a18ULL;
            const float view_lo = threads ? -760.0f : -1080.0f, view_hi = threads ? 598.0f : 1080.0f;
            int first = -1, last = -1;   /* (the rows shown: their first and last in the order) */
            for (int j = 0; j < n_rows; j++) {
                const float cy = (hit.rect[order[j]].y0 + hit.rect[order[j]].y1) / 2;
                if (cy >= view_lo && cy <= view_hi) last = j, first = first < 0 ? j : first;
            }
            int to = -1;
            if (first >= 0 && step > 0 && last + 1 < n_rows) to = last + want < n_rows ? last + want : n_rows - 1;
            else if (first >= 0 && step < 0 && first > 0) to = first + want >= 0 ? first + want : 0;
            else if (at_focus >= 0) {
                to = at_focus + want;
                if (to >= n_rows) to = n_rows - 1;
                if (to < 0) to = 0;
            }
            if (to >= 0 && to != at_focus) {
                const Rect *r = &hit.rect[order[to]];
                const float cy = (r->y0 + r->y1) / 2;
                set_focus(c, screen, hit.button[order[to]]);
                if (trace) hos_log("menu: wheel %d: the focus from row %d to %d of %d (y %.0f)%s", wheel, at_focus, to, n_rows, cy,
                                   cy < view_lo || cy > view_hi ? ", out of view: the list moves" : "");
                if (cy < view_lo || cy > view_hi)   /* (then the row under the mouse, once the list has brought it into view) */
                    refocus_ns = now + 1200000000, scroll_row = hit.button[order[to]], scroll_lo = view_lo, scroll_hi = view_hi, asked = 0;
                else asked = hit.button[order[to]];   /* (the hover leaves it be until the mouse moves on) */
            }
            hit.hit = 0;
            list = 0;   /* (no pad presses) */
        }
        for (int i = 0; list && i < k && i < 6; i++)
            n += snprintf(taps + n, sizeof taps - (size_t)n, "%s%.2f:%s/60", n ? "," : "", at + i * 0.12,
                          pause ? (wheel > 0 ? "L" : "R") : (wheel > 0 ? "UP" : "DOWN"));
        if (n) hos_hid_tap(taps);
    }
    /* (the list has moved under the mouse: the row the wheel gave the focus is in view, as the list scrolls at its own
       pace; at most 1.2 s on. Taken back sooner, the scroll stopped short of it) */
    if (refocus_ns && scroll_row) {
        Rect r;
        const float cy = button_rect(scroll_row, &r) ? (r.y0 + r.y1) / 2 : 0;
        if (cy >= scroll_lo && cy <= scroll_hi) refocus_ns = now;
    }
    if (refocus_ns && now >= refocus_ns) refocus_ns = 0, scroll_row = 0, moved = 1, asked = 0;
    if (moved || (pressed & HOS_MOUSE_LEFT)) hit_test(screen, &hit);
    else hit.hit = 0;
    /* hover: the focus to the button under the mouse, only as it moves (the pad keeps the focus it moved) */
    int just = 0;
    uint8_t state = 0;
    if (hit.hit) peek(hit.hit + 56, &state, 1);   /* (0: not focused) */
    if (hit.hit && hit.hit != focused_button && hit.hit != asked && !state) {
        asked = hit.hit, just = 1;
        const int r = set_focus(c, screen, hit.hit);
        if (trace) {
            char name[64];
            hos_log("menu: mouse (%.0f, %.0f): the focus to %s (%d) on screen %#llx", hit.x, hit.y,
                    text_at(peek64(hit.hit + 0x18), name, sizeof name), r, (unsigned long long)screen);
        }
    }
    /* a click on the tabs: L or R pressed as many times as the pages between (one at a time, as the pages slide) */
    if (pause && (pressed & HOS_MOUSE_LEFT) && !hit.hit) {
        const int tab = tab_at(hit.x, hit.y);
        if (tab == TAB_L || tab == TAB_R) hos_hid_tap(tab == TAB_L ? "0:L/120" : "0:R/120");
        else if (tab >= 0 && page_now >= 0 && tab != page_now) tab_goal = tab, tab_pages = 4, goal_ns = now, tab_from = -9;
        if (trace && tab != -1) hos_log("menu: mouse (%.0f, %.0f): tab %d from page %d", hit.x, hit.y, tab, page_now);
    }
    /* A tab clicked (the pause menu's, the panel's): L or R one at a time toward it, the next once the page has come and
       settled (0.25 s: a press while a page slides in is lost); given up after 4 s */
    {
        static int last_cur = -9;
        static int64_t arrive_ns;
        const int cur = pause ? page_now : page;
        if (cur != last_cur) last_cur = cur, arrive_ns = now;
        if (tab_goal >= 0 && (cur < 0 || now - goal_ns > 4000000000LL)) tab_goal = -1;
        if (tab_goal >= 0) {
            if (cur == tab_goal) tab_goal = -1;
            else if (cur != tab_from && now - arrive_ns > 250000000) {
                const int d = tab_goal - cur;
                if (tab_pages == 4) hos_hid_tap(d < 0 ? "0:L/120" : "0:R/120");
                else hos_hid_tap(d < 0 ? "0:L/75" : "0:R/75");   /* (the panel: 75 ms, not into the next page) */
                if (trace) hos_log("menu: tab: page %d toward %d: %s", cur, tab_goal, d < 0 ? "L" : "R");
                tab_from = cur;
            }
        }
    }
    /* Key Items: the stones and the medallions (not the framework's buttons). The item chosen is the page's own key of
       it, a string at *(screen+760) (Item_Medal_Fire, Item_Stone_Water, ...; an empty slot has its key too). The page
       moves its choice with the left stick (A and D on the keyboard) on a 3 x 3 grid:
           Item_Medal_Shadow   Item_Medal_Light   Item_Medal_Forest
           Item_Stone_Forest   Item_Stone_Fire    Item_Stone_Water
           Item_Medal_Spirit   Item_Medal_Water   Item_Medal_Fire
       and a press off the grid's edge leaves the page (to the pause menu). With the medallions (an adult's page) the
       items sit in a ring round the stones; without (a child's) only the stones are shown, further apart. The key is
       the page's whole choice: each frame its update places the cursor from it, and its stick only works out the next
       key and copies it in. So the mouse moved onto an item sets that item's key and calls the page's own refresh
       (set_item_key: as the page does when it opens), silently; a click on an item is Details (A) for it, once it is
       the one chosen (given up after 2 s). Positions: the item's centre in the layout's units, from the pages as
       shown (1280 x 720 screenshots) */
    if (keys) {
        static const struct { const char *key; int row, col; float x, y, cx; } items[] = {
            {"Item_Medal_Shadow", 0, 0, -750, 294, 0}, {"Item_Medal_Light", 0, 1, 0, 660, 0}, {"Item_Medal_Forest", 0, 2, 744, 294, 0},
            {"Item_Stone_Forest", 1, 0, -429, -24, -640}, {"Item_Stone_Fire", 1, 1, 0, -15, 0}, {"Item_Stone_Water", 1, 2, 429, -6, 650},
            {"Item_Medal_Spirit", 2, 0, -756, -354, 0}, {"Item_Medal_Water", 2, 1, 0, -714, 0}, {"Item_Medal_Fire", 2, 2, 759, -354, 0}};
        enum { N_ITEMS = (int)(sizeof items / sizeof *items) };
        static int adult;   /* (the ring is shown; worked out again when the page has been away) */
        static int64_t keys_ns;
        static float px_[N_ITEMS], py_[N_ITEMS];
        static int found;   /* (the items' panes the game found) */
        if (now - keys_ns > 1000000000) {
            /* The page shown anew: the layout from the items' panes, found as the page finds them to place its glow;
               the Emerald's x tells them apart (a child's wider row: about -680, the ring's -430). The panes' positions
               are their parent's, about 200 below the items as drawn: the hit boxes stay those measured above */
            adult = 0, found = 0;
            struct { int *found; float *x, *y; } at_ = {&found, px_, py_};
            item_panes(c, screen, item_found, &at_);
            if (found & (1 << 3)) adult = px_[3] > -540.0f;
            if (trace) {
                char line[600];
                int n = snprintf(line, sizeof line, "menu: Key Items' panes (%s):", adult ? "the ring" : "the stones' row");
                for (int i = 0; i < N_ITEMS && n < (int)sizeof line - 40; i++)
                    n += found & (1 << i) ? snprintf(line + n, sizeof line - (size_t)n, " %s %.0f,%.0f", items[i].key + 5, px_[i], py_[i])
                                          : snprintf(line + n, sizeof line - (size_t)n, " %s -", items[i].key + 5);
                hos_log("%s", line);
            }
        }
        keys_ns = now;
        static int64_t click_ns;
        static int click_item = -1;
        char key[40] = "";
        int at = -1, want = -1;
        {
            const uint64_t p = peek64(screen + 760);
            if (p && peek(p, key, sizeof key - 1)) key[sizeof key - 1] = 0;
            for (int i = 0; i < N_ITEMS; i++)
                if (!strcmp(key, items[i].key)) at = i;
            if (at >= 0 && items[at].row != 1) adult = 1;
        }
        float best = 0;
        for (int i = 0; i < N_ITEMS; i++) {
            if (!adult && items[i].row != 1) continue;   /* (a child's page: the stones alone) */
            const float ix = adult || !items[i].cx ? items[i].x : items[i].cx, iy = items[i].y, r = adult ? 190.0f : 240.0f;
            const float dx = hit.x - ix, dy = hit.y - iy, d = dx * dx + dy * dy;
            if (d < r * r && (want < 0 || d < best)) want = i, best = d;
        }
        if ((pressed & HOS_MOUSE_LEFT) && want >= 0) {
            click_item = want, click_ns = now;
            pressed &= ~(uint32_t)HOS_MOUSE_LEFT;   /* (a click on an item is this, not the tabs' or the prompts') */
        }
        if (click_item >= 0 && now - click_ns > 2000000000LL) click_item = -1;
        if (click_item >= 0 && at == click_item) {
            hos_hid_tap("0:A/75");
            if (trace) hos_log("menu: Key Items: %s clicked: A", key);
            click_item = -1;
        }
        /* (the mouse moved onto an item, or clicked one: the stick may move the choice away while the mouse rests) */
        const int to = click_item >= 0 ? click_item : moved && !ldown ? want : -1;
        if (to >= 0 && at >= 0 && to != at) {
            const int r = set_item_key(c, screen, items[to].key);
            if (trace) hos_log("menu: Key Items: %s chosen, the mouse over %s: set %s", key, items[to].key, r ? "" : "(not now)");
        }
    }
    /* The map's page (no buttons of the screen's list for these; in the layout's units, from its screens as shown,
       measured): the prompts at the bottom right ("World Map" / "Current Location" with Y, "Back" with B) and the tab
       bar of its panel (Key Items, Threads of Time, Map; L and R): the map's screen has the mouse while the Map page
       is shown, so a tab is that many pages from it */
    if (page >= 0 && (pressed & HOS_MOUSE_LEFT) && !hit.hit) {
        char tap[256] = "";
        if (hit.y > -1080 && hit.y < -960) {   /* (the Y prompt: World Map / Current Location, See Tapestry) */
            if (page >= 1 && hit.x > 1040 && hit.x < 1570) strcpy(tap, "0:Y/75");
        } else if (map && hit.x > 1400 && hit.x < 1880 && hit.y > -960 && hit.y < -560) {
            strcpy(tap, "0:Y/75");   /* (an area's map, not a dungeon's: its World Map picture and prompt above Back) */
        } else if (map && hit.x > -1850 && hit.x < -1200 && hit.y < 700 && hit.y > -1000) {
            /* A dungeon's floors (not the framework's buttons): the rows 142 apart, the list centred at y -119 (from the
               screens as shown: Lord Jabu-Jabu's 3 floors' top row at y 23; the Water Temple's 4 sit half a row higher):
               the floor of the row clicked, and the pad's up or down as many times as it is from the floor chosen
               (map_floors). A row past the last floor is the last */
            int cur, top, bottom;
            if (map_floors(&cur, &top, &bottom)) {
                const float top_y = -119.0f + (float)(top - bottom) * 71.0f;
                int row = (int)((top_y - hit.y) / 142.0f + 0.5f);
                if (row < 0) row = 0;
                int want = top - row;
                if (want < bottom) want = bottom;
                const int steps = want - cur;
                for (int i = 0, n = 0; i < (steps < 0 ? -steps : steps) && n < (int)sizeof tap - 16; i++)
                    n += snprintf(tap + n, sizeof tap - (size_t)n, "%s%.2f:%s/60", n ? "," : "", i * 0.15, steps > 0 ? "UP" : "DOWN");
                if (trace) hos_log("menu: map floors %d..%d, floor %d; row %d clicked: floor %d", bottom, top, cur, row, want);
            }
        } else if (hit.y > 900 && hit.y < 1080) {   /* (the tabs: Key Items, Threads of Time, Map; L and R) */
            if (hit.x >= -1100 && hit.x < -850) strcpy(tap, "0:L/75");
            else if (hit.x >= 850 && hit.x < 1100) strcpy(tap, "0:R/75");
            else {   /* (L to the left, R to the right: stepped toward it, above) */
                const int to = hit.x >= -850 && hit.x < -300 ? 0 : hit.x >= -300 && hit.x < 345 ? 1 : hit.x >= 345 && hit.x < 850 ? 2 : page;
                if (to != page) tab_goal = to, tab_pages = 3, goal_ns = now, tab_from = -9;
            }
        }
        if (tap[0]) hos_hid_tap(tap);
        if (trace) hos_log("menu: panel page %d click (%.0f, %.0f): %s", page, hit.x, hit.y, tap[0] ? tap : "-");
    }
    /* The "Back" prompt at the bottom right (B), on every screen with one (the title's load list, the pause menu, the
       map; in the layout's units, measured from their screens): a click there off the buttons */
    if ((pressed & HOS_MOUSE_LEFT) && !hit.hit && hit.y > -1080 && hit.y < -960 && hit.x >= 1600 && hit.x < 1900) {
        hos_hid_tap("0:B/80");
        if (trace) hos_log("menu: the Back prompt clicked (%.0f, %.0f)", hit.x, hit.y);
    }
    /* a click: the button under it decided (A: after the focus has landed, at the next update, when it moved it), the
       right button back (B) */
    if ((pressed & HOS_MOUSE_LEFT) && hit.hit) hos_hid_tap(just || hit.hit == asked ? "0.05:A/80" : "0:A/80");
    if (pressed & HOS_MOUSE_RIGHT) hos_hid_tap("0:B/80");
}

/* The button's boxes (button+0x58, 0xb0 apart). The first is its hit box: its global matrix at +0x28 (3 x 4, by column:
   the translation at +0x4c, +0x50; the layout's units, moving with a page as it slides; all 0 while its page is not shown)
   and its rectangle at +0x84 (x, y, w, h, about the box's origin). The second is centred on the slot's picture (its
   translation at +0xfc, +0x100; its own rectangle a fixed 700 x 700). A few slots' hit boxes cover only a side of the
   picture (Pa_ItemIcon_05: 100 wide on the right of a slot 260 wide, its centre 80 to the right of the picture's): such
   a box is mirrored about the picture's centre and both taken */
static int button_rect(uint64_t button, Rect *r) {
    float m[12], b[4], c[2];
    const uint64_t box = peek64(button + 0x58);
    if (!peek(box + 0x28, m, sizeof m) || !peek(box + 0x84, b, sizeof b)) return 0;
    const float sx = m[0], sy = m[4], tx = m[9], ty = m[10];
    if (!(sx > 1e-3f) || !(sy > 1e-3f) || !(b[2] > 0) || !(b[3] > 0)) return 0;
    r->x0 = tx + sx * b[0], r->x1 = tx + sx * (b[0] + b[2]);
    r->y0 = ty + sy * b[1], r->y1 = ty + sy * (b[1] + b[3]);
    float s2;
    if (peek(box + 0xd8, &s2, 4) && s2 > 1e-3f && peek(box + 0xfc, c, sizeof c)) {
        const float w = r->x1 - r->x0, h = r->y1 - r->y0, cx = (r->x0 + r->x1) / 2, cy = (r->y0 + r->y1) / 2;
        if (c[0] - cx < w && cx - c[0] < w && c[1] - cy < h && cy - c[1] < h) {   /* (the picture's centre near the box) */
            const float x0 = 2 * c[0] - r->x1, x1 = 2 * c[0] - r->x0, y0 = 2 * c[1] - r->y1, y1 = 2 * c[1] - r->y0;
            if (x0 < r->x0) r->x0 = x0;
            if (x1 > r->x1) r->x1 = x1;
            if (y0 < r->y0) r->y0 = y0;
            if (y1 > r->y1) r->y1 = y1;
        }
    }
    return 1;
}

/* ---- The screens f_802f1380 runs for (the HUD's, the menus', the title's: each frame each). The mouse is for the one
   in front: of those that ran in the last 100 ms with a focused button shown (or one gone for under 0.5 s: the map's
   list loses it for 6 to 24 frames as it moves), the one that came last (a dialog over the menu that opened it). Whether a screen has a focused button is looked at 10 times a second (each look reads its list) */
typedef struct { uint64_t screen, vt; int64_t first_ns, seen_ns, lost_ns; int focus, running; unsigned n; } Seen;
static Seen seen[24];
static SRWLOCK seen_lock = SRWLOCK_INIT;   /* (the screens' updates run on several of the game's threads at once) */

static void focus_entry(int i, uint64_t entry, uint64_t button, void *arg) {
    (void)i, (void)entry;
    uint8_t state = 0;
    Rect r;
    if (!*(int *)arg && peek(button + 56, &state, 1) && state && button_rect(button, &r)) *(int *)arg = 1;
}
static Seen *screen_seen(uint64_t screen, int64_t now) {
    Seen *free_one = NULL;
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++) {
        if (seen[i].screen == screen) return &seen[i];
        if (!free_one || seen[i].seen_ns < free_one->seen_ns) free_one = &seen[i];   /* (the one gone longest) */
    }
    memset(free_one, 0, sizeof *free_one);
    free_one->screen = screen, free_one->first_ns = now, free_one->vt = peek64(screen);
    if (free_one->vt == HUD_SCREEN_VTABLE) hud_screen = screen;
    if (trace) {
        const uint64_t mgr = peek64(peek64(screen + 32) + 96);
        hos_log("menu: a screen: %#llx (vtable %#llx) entries %d, focus manager %#llx%s", (unsigned long long)screen,
                (unsigned long long)peek64(screen), each_entry(screen, 400, NULL, NULL), (unsigned long long)mgr,
                screen == pause_screen_now() ? " (the pause menu)" : "");
    }
    return free_one;
}

void hos_oot_menu_tick(Ctx *c, uint64_t screen) {
    if (!screen) return;
    if (trace < 0) trace = env_on("HOS_MENU_TRACE");
    const int64_t now = hos_now_ns();
    AcquireSRWLockExclusive(&seen_lock);
    Seen *me = screen_seen(screen, now);
    if (!me->running) {
        me->running = 1;
        if (trace) hos_log("menu: screen %#llx (vtable %#llx) runs", (unsigned long long)screen, (unsigned long long)peek64(screen));
    }
    me->seen_ns = now;
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++)   /* (a screen with no update for 200 ms has stopped) */
        if (seen[i].running && now - seen[i].seen_ns > 200000000) {
            seen[i].running = 0;
            if (trace) hos_log("menu: screen %#llx stopped", (unsigned long long)seen[i].screen);
        }
    if (me->n++ % 6 == 0) {
        const int had = me->focus;
        me->focus = 0;
        if (peek64(peek64(screen + 32) + 96)) each_entry(screen, 400, focus_entry, &me->focus);
        if (had && !me->focus) me->lost_ns = now;
        if (trace && me->focus != had)
            hos_log("menu: screen %#llx %s a focused button (%u frames in)", (unsigned long long)screen,
                    me->focus ? "has" : "no longer has", me->n);
    }
    {   /* (the Map page: the panel's frame running alone) */
        int frame = 0, other = 0;
        for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++) {
            if (!seen[i].screen || !seen[i].running) continue;
            if (seen[i].vt == PANEL_FRAME_VTABLE) frame = 1;
            if (seen[i].vt == 0x84c07798ULL || seen[i].vt == 0x84c04a18ULL || seen[i].screen == pause_screen_now()) other = 1;
        }
        const int was = map_page;
        map_page = frame && !other;
        if (trace && map_page != was) hos_log("menu: the panel's Map page %s", map_page ? "shown" : "gone");
    }
    const Seen *front = NULL;
    for (int i = 0; i < (int)(sizeof seen / sizeof *seen); i++)
        if (seen[i].screen && (seen[i].focus || now - seen[i].lost_ns < 500000000 || panel_page_vt(seen[i].vt) >= 0) &&
            seen[i].running && now - seen[i].seen_ns < 100000000 &&
            (!front || seen[i].first_ns > front->first_ns))   /* (its focus gone for under 0.5 s: a list's moment between rows) */
            front = &seen[i];
    static uint64_t front_was;
    if (trace && front && front->screen != front_was) hos_log("menu: the mouse is for screen %#llx", (unsigned long long)front->screen);
    front_was = front ? front->screen : 0;
    const int mine = front == me;
    /* (no screen has held the mouse for 50 ms of frames, the HUD's among them in play: the game's menus have closed,
       and the window gives the camera its mouse back now, not after display.c's 300 ms) */
    static int64_t none_since;
    const int gone = !front && none_since && now - none_since > 50000000;
    none_since = front ? 0 : none_since ? none_since : now;
    ReleaseSRWLockExclusive(&seen_lock);
    if (gone) hos_mouse_game_menu_gone();
    if (mine) mouse_tick(c, screen);
    if (screen == pause_screen_now()) InterlockedIncrement(&ticks);
}
#else
void hos_oot_menu_focus(Ctx *c, uint64_t screen, uint64_t pane, uint64_t lr) { (void)c, (void)screen, (void)pane, (void)lr; }
void hos_oot_menu_tick(Ctx *c, uint64_t screen) { (void)c, (void)screen; }
#endif
