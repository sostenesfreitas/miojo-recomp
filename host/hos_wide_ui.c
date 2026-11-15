/* (hookshot) The HUD laid out for the picture's shape (hos_wide_ui_tick below): at its 16:9 size, what sits at a side or
   corner of its 16:9 canvas moved out (or in) to the screen's, as a layout made for that shape would place it; panes
   spanning the canvas drawn the screen's width. The pane planner is ozwaldorf's ("Extended" ultrawide menus,
   https://github.com/NotRexed/Hookshot/pull/2843), here for the HUD's layouts on every shape, by
   default, both ways and up and down; the menus are not moved (on a narrower screen they are drawn smaller to fit).

   The game's 2D layouts keep their panes in an array (the layout's +0x100, 0xb0 bytes each). f_8154cab0 makes a pane's
   final 3x4 transform (+0x28, the translation at +0x4c) from its own position, scale and rotation and its parent's
   transform, only when the pane is marked for it (dirty bit 6, f_8154caa0). nx2/oot_wide.py calls in here:

   - hos_wide_ui_parent: the parent f_8154cab0 multiplies by (0 for none), on the thread that is making the transform
   - hos_wide_ui_placed: the transform just stored. A layout's root pane is noted (its kind, by its layout's name), and
     so is every pane of the HUD's trees and of the loading fade's (its parent, and its place on the canvas as the game made
     it, without the moves): a pane made from one noted in such a tree. A menu's other panes are not (the plan never looks
     at them). If the plan moves a pane, its translation is moved, and its children follow, since theirs are made from it.
     A menu's root is made smaller here on a narrower screen
   - hos_wide_ui_size: a pane's size as drawn (f_8154ac30): a pane the plan widens is drawn the screen's width
   - hos_wide_ui_force (read in f_8154caa0): while set, every pane remakes its transform: after a change of the screen's
     shape, or of the plan

   The plan (hos_wide_ui_tick, each present) walks the HUD's noted panes as a tree, parts' own layouts included, with how
   far each one's contents reach across (as the ultrawide UI romfs mod's rules.py does from the layout files): a pane
   spanning the canvas is widened; one touching an edge, or the panes on one side of their parent together when they reach
   an edge or sit far out, move to that side; a container across the centre is looked into; what is off the canvas or
   wider than it (a scrolling strip) is left alone. Up and down the same, when the HUD is drawn smaller (visit_y).

   HOS_WIDE_UI=0 keeps the HUD in the middle 16:9 as before (on a narrower screen fitted to its width, as the menus);
   HOS_WIDE_UI_LOG=<n> logs the first n changes of the plan, each root layout, and the plan's time. */
#include "hos.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

extern volatile float hos_wide_ratio;

enum {
    LAYOUT_RES = 0xf8, RES_NAME = 0x18, RES_PANE_NAMES = 0x20,   /* (the layout file's UIL header: its name, its panes') */
    LAYOUT_PANES = 0x100, LAYOUT_COUNT = 0x13c, PANE_SIZE = 0xb0,
    PANE_LAYOUT = 0x18, PANE_XX = 0x28, PANE_YY = 0x38, PANE_X = 0x4c, PANE_Y = 0x50, PANE_W = 0x58, PANE_H = 0x5c,
    PANE_FLAGS = 0xa8,
    PANE_TYPE_NULL = 0, PANE_TYPE_SCISSOR = 4, PANE_TYPE_CAPTURE = 5, PANE_TYPE_PART = 6, PANE_TYPE_HOST = 7,   /* (7: holds other layouts, a list) */
    PANE_DIRTY_RECT = 4,
};
static const float CANVAS_HALF = 1920.0f;   /* (every layout's canvas is 3840 x 2160) */
static const float CANVAS_HALF_Y = 1080.0f;
static const float SIDE_Y = 620.0f;         /* (a group centred further up or down than this sits at the top or bottom) */
static const float EDGE_NEAR = 150.0f;      /* (a pane within this of an edge is anchored to it) */
static const float EDGE_TOL = 40.0f;        /* (a pane within this of both edges spans the canvas) */
static const float SIDE = 1100.0f;          /* (a group centred further out than this sits at a side) */
static const float CROSS = 0.25f;           /* (a pane with less than this share across the centre belongs to one side) */
static const float SIZE_CAP = 960.0f;       /* (wider panes count by their origin only: glows, hit boxes, backdrops) */

volatile uint32_t hos_wide_ui_force;   /* (read by nx2/oot_wide.py's site in f_8154caa0) */
static volatile int ui_on;

/* Every 2D screen not kept whole (nx2/oot_wide.py HUD) draws with the UI projection's across and up rows times hos_ui_qx,
   hos_ui_qy (0 before the first present: hos_wide_squeeze across then). The HUD's layouts, by their root layout's name
   (HUD_LAYOUTS), are placed for the screen's shape: ui_ex the canvas units a side pane moves out (less than 0: in, on a
   screen narrower than the HUD's 16:9 canvas), ui_ey the same up and down, ui_wx a spanning pane's width factor. Every
   other layout is a menu's: not moved, and on a screen narrower than 16:9 drawn smaller from its root pane, by ui_fit, to
   fit the screen's width */
volatile float hos_ui_qx, hos_ui_qy;
static volatile float ui_ex, ui_ey, ui_wx = 1.0f, ui_fit = 1.0f;
/* (the loading fade's pane that spans its canvas, drawn the whole picture: ui_fill_x wider (a wider screen, its layout in the
   middle 16:9), ui_fill_y taller (a narrower one, its layout at the HUD's size). Hookshot #82,
   https://github.com/NotRexed/Hookshot/issues/2220: in the centred Ultrawide menus modes the fade covered the middle 16:9
   alone, the world still showing beside it. While its screen is not kept whole (fade_planned) on a narrower screen
   (fade_icon_planned), the rest of its layout, the loading icon, is placed as the HUD's: at the screen's corner, at the HUD's
   size. Left where the 16:9 canvas has it, the icon was cut off at the right there, where the canvas at the HUD's size is
   wider than the screen. On a wider screen it stays in the middle 16:9: the centred modes' blurred or black sides
   (gpu/vk/vk_framegen.cpp wide_sides, fg_scale.comp) cover what is beside it once the world is not drawn, as while loading) */
static volatile float ui_fill_x = 1.0f, ui_fill_y = 1.0f;
static volatile int fade_planned, fade_icon_planned;

/* A pane seen, by its guest address: its parent, kind, and place across the canvas without the moves (x0, x1 its rectangle,
   ox its origin); plan, how far the plan moves it (its own move);
   widen, the plan draws it the screen's width; moved, how far its stored transform was moved, its own and its ancestors'
   together, pushed the side moves among them. Open addressing; emptied when three quarters full, every pane then placed
   again. The game's threads write their panes; the plan reads them all */
typedef struct {
    uint64_t key, parent, layout, lname, pname;   /* (lname, pname: its layout's name and its own, guest strings) */
    float x0, x1, ox, plan, moved, pushed;
    float y0, y1, oy, plany, movedy, pushedy;   /* (the same up and down: y up, as the canvas) */
    float wide;            /* (its drawn width times this: 0 or 1, as the game draws it) */
    float tall;            /* (its drawn height times this, the same) */
    float from;            /* (its parent's ox when it was placed, NAN if its parent was not noted: its place in it) */
    uint32_t type;
    uint16_t lkind, rule;  /* (its layout's kind and its own rule, by name: LK_ and RULE_ below) */
    uint32_t in_hud;       /* (its root layout is the HUD's, by name: as the plan last saw it) */
    uint8_t hud, whole;    /* (its own layout is one of HUD_LAYOUTS; one drawn over the whole picture, never made smaller) */
    uint8_t fade;          /* (its layout is the loading fade's) */
    uint8_t tree;          /* (the panes made from it are noted: a HUD tree's or the loading fade's) */
    uint8_t as_menu;       /* (a HUD root drawn as a menu's: the map page, MapMenu_00 showing it. Set by the plan) */
    uint32_t dead;         /* (its layout let it go: its slot is taken by the next new pane) */
} Node;
enum { NODES = 1 << 16 };
static Node nodes[NODES];
static volatile int nodes_used;

static uint32_t node_slot(uint64_t key) { return (uint32_t)((key >> 4) * 0x9E3779B1u) & (NODES - 1); }

static Node *node_find(uint64_t key) {
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        const uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (k == key) return &nodes[i];
        if (!k) return NULL;
    }
    return NULL;
}

/* (a bit for each slot ever taken since the table was emptied: the plan walks these, not the whole table, whose 8 MB
   the presenting thread otherwise read each present) */
static uint64_t used_bits[NODES / 64];

static void nodes_clear(void) {
    memset(nodes, 0, sizeof nodes);
    memset(used_bits, 0, sizeof used_bits);
    __atomic_store_n(&nodes_used, 0, __ATOMIC_RELAXED);
    hos_wide_ui_force = 3;
}

static void node_fresh(Node *n) { memset((char *)n + sizeof n->key, 0, sizeof *n - sizeof n->key); }

/* A pane let go: drawn as the game makes it, its slot free for another */
static void node_drop(Node *n) {
    n->plan = n->plany = 0.0f, n->wide = n->tall = 1.0f, n->layout = 0;
    __atomic_store_n(&n->dead, 1, __ATOMIC_RELEASE);
}

static int log_each, log_left;
static void guest_copy(char *out, int cap, uint64_t g);

/* The pane's node: its own, else an empty slot or one a let-go pane had (a menu opened again puts its panes at new
   addresses: the old ones' slots are used again, so the table does not fill with them) */
static Node *node_take(uint64_t key) {
    Node *own = node_find(key);
    if (own) {
        if (own->dead) node_fresh(own);
        return own;
    }
    for (uint32_t i = node_slot(key), n = 0; n < 64; i = (i + 1) & (NODES - 1), n++) {
        uint64_t k = __atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE);
        if (!k && __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            __atomic_fetch_or(&used_bits[i / 64], 1ull << (i % 64), __ATOMIC_RELEASE);
            if (__atomic_add_fetch(&nodes_used, 1, __ATOMIC_RELAXED) > NODES * 3 / 4) {
                hos_log("wide ui: %d panes noted: noted afresh", NODES * 3 / 4);
                nodes_clear();
                return NULL;
            }
            return &nodes[i];
        }
        if (k && __atomic_load_n(&nodes[i].dead, __ATOMIC_ACQUIRE) &&
            __atomic_compare_exchange_n(&nodes[i].key, &k, key, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            node_fresh(&nodes[i]);
            return &nodes[i];
        }
    }
    static int told;
    if (!told) told = 1, hos_log("wide ui: no slot for pane %llx (64 taken in a row)", (unsigned long long)key);
    return NULL;
}

/* ---- names: the layouts and panes the ultrawide UI romfs mod's rules.py and menu.py treat by hand ---- */

enum { LK_PLAIN, LK_WORLD, LK_WHOLE, LK_HELD };   /* (a layout placed by code from the world; on a screen kept whole; held) */
enum { RULE_NONE, RULE_KEEP, RULE_KEEP_LOOK, RULE_WIDEN, RULE_LEFT, RULE_RIGHT, RULE_SLIDE, RULE_KEEP_ORIGIN };
       /* (RULE_SLIDE: a pane the game slides in from beside its parent, planned where it comes to rest) */

static const char *const WORLD_LAYOUTS[] = {"PlayerStatus3D_00", "Pointer_00", "BalloonMessage_00"};
/* (layouts of screens nx2/oot_wide.py keeps whole (KEEP_VTABLES): their canvas already spans the screen, so nothing in
   them moves or widens. The item wheel's background picture widened to 5160 units mapped its capture over 3840 / 5160
   of the screen, black at the sides) */
static const char *const WHOLE_LAYOUTS[] = {"ShortCutBG_00", "EventFade_00"};
/* (held where the game puts them, at the HUD's size: never moved, never made smaller. The text box's choices, which sit over
   Talk_00's box: fitted as a menu's on a narrower screen they came out at another scale than the box) */
static const char *const HELD_LAYOUTS[] = {"MessageDialog_00"};
/* (the HUD's root layouts: the hearts, magic, rupees and buttons; the place's name; the minimap (the pause menu's map page is
   the same layout: drawn as a menu's while it shows, as_menu); the text box and its button guide ("Hide", "Conversation
   Log"), which stands where the minimap was; and the ones placed from the world, never moved. Kept the size of a picture
   over the whole screen: the fades and the system overlay, kept whole by their screens) */
static const char *const HUD_LAYOUTS[] = {"PlayerStatus_00", "Location_00", "MapMenu_00", "Talk_00", "MessageLog_01", "PlayerStatus3D_00",
                                          "Pointer_00", "BalloonMessage_00"};
static const char *const FULL_LAYOUTS[] = {"LoadingFade_00", "SystemOverlay_00"};
typedef struct { const char *layout, *pane; uint16_t rule; } PaneRule;
static const PaneRule RULES[] = {
    /* (the HUD: the left gauges are moved by name, their group stays where the pause menu places part of it) */
    {"PlayerStatus_00", "N_ScaleL_00", RULE_KEEP_LOOK}, {"PlayerStatus_00", "N_ScaleL_01", RULE_KEEP_LOOK},
    {"PlayerStatus_00", "Pa_HeartGauge_00", RULE_LEFT}, {"PlayerStatus_00", "Pa_MpGauge_00", RULE_LEFT},
    {"PlayerStatus_00", "Pa_AgonyStone_00", RULE_LEFT},
    /* (the Map page slides 20 to 80 units as it changes: planned at rest) */
    {"MapMenu_00", "N_MenuIn_00", RULE_SLIDE}, {"MapMenu_00", "N_InChallenge_00", RULE_SLIDE},
    /* (the map's icons, placed by code over the map, and the world map, wider than the screen with its location panels on
       it: kept whole with the map under them) */
    /* (MapMenu_00 is also the HUD's minimap: there the icons' group sits over the minimap and moves out with it. At the
       canvas's origin, where the map page puts it, it tells the map page: the layout is then drawn as a menu's (as_menu).
       The icons' part itself is never looked into: its icons move as one with their group) */
    {"MapMenu_00", "N_MapIcon_00", RULE_KEEP_ORIGIN}, {"MapMenu_00", "Pa_MapIcon_00", RULE_KEEP},
    {"MapMenu_00", "Pa_WorldMap_00", RULE_KEEP},
    {"Location_00", "P_DecoFrameLT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRT_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRTSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameLB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameLBSh_00", RULE_WIDEN},
    {"Location_00", "P_DecoFrameRB_00", RULE_WIDEN}, {"Location_00", "P_DecoFrameRBSh_00", RULE_WIDEN},
};

/* A guest string is s (64 characters at most) */
static int guest_is(uint64_t g, const char *s) {
    if (!g) return 0;
    for (int i = 0; i < 64; i++) {
        const char ch = (char)recomp_load8(NULL, g + (uint64_t)i);
        if (ch != s[i]) return 0;
        if (!ch) return 1;
    }
    return 0;
}

static int guest_in(uint64_t g, const char *const *list, int n) {
    for (int i = 0; i < n; i++)
        if (guest_is(g, list[i])) return 1;
    return 0;
}

/* A pane's layout kind and rule, from its layout's name and its own (the layout file's UIL header) */
static void classify(Node *n, uint64_t pane, uint64_t layout) {
    const uint64_t res = layout ? recomp_load64(NULL, layout + LAYOUT_RES) : 0;
    const uint64_t lname = res ? recomp_load64(NULL, res + RES_NAME) : 0;
    const uint64_t names = res ? recomp_load64(NULL, res + RES_PANE_NAMES) : 0;
    const uint64_t base = layout ? recomp_load64(NULL, layout + LAYOUT_PANES) : 0;
    const uint64_t pname = names && pane >= base ? recomp_load64(NULL, names + (pane - base) / PANE_SIZE * 8) : 0;
#define COUNT(a) (int)(sizeof a / sizeof a[0])
    n->lkind = guest_in(lname, WORLD_LAYOUTS, COUNT(WORLD_LAYOUTS)) ? LK_WORLD
             : guest_in(lname, WHOLE_LAYOUTS, COUNT(WHOLE_LAYOUTS)) ? LK_WHOLE
             : guest_in(lname, HELD_LAYOUTS, COUNT(HELD_LAYOUTS)) ? LK_HELD : LK_PLAIN;
    n->lname = lname, n->pname = pname, n->rule = RULE_NONE;
    n->hud = (uint8_t)guest_in(lname, HUD_LAYOUTS, COUNT(HUD_LAYOUTS));
    n->whole = (uint8_t)(n->lkind == LK_WHOLE || guest_in(lname, FULL_LAYOUTS, COUNT(FULL_LAYOUTS)));
    n->fade = (uint8_t)guest_is(lname, "LoadingFade_00");
    for (int i = 0; i < COUNT(RULES) && pname; i++)
        if (guest_is(lname, RULES[i].layout) && guest_is(pname, RULES[i].pane)) n->rule = RULES[i].rule;
#undef COUNT
}

/* HOS_WIDE_UI_LOG=<n>: the first n changes of the plan after each change of the shape, to tell what a pane that moves
   wrongly is (log_each, log_left above) */

static void guest_copy(char *out, int cap, uint64_t g) {
    int i = 0;
    for (; g && i < cap - 1; i++)
        if (!(out[i] = (char)recomp_load8(NULL, g + (uint64_t)i))) break;
    out[i] = 0;
}

static void decided(const Node *n, float plan, float widen, float plany) {
    if (log_left <= 0) return;
    log_left--;
    char ln[40], pn[40];
    guest_copy(ln, sizeof ln, n->lname), guest_copy(pn, sizeof pn, n->pname);
    hos_log("wide ui: %s:%s (pane %llx parent %llx, %s) type %u x %.1f [%.1f %.1f] y %.1f [%.1f %.1f] rule %u: %s %.1f, up %.1f, "
            "width x%.3f", ln, pn, (unsigned long long)n->key, (unsigned long long)n->parent,
            n->in_hud ? "HUD" : "menu", n->type, (double)n->ox, (double)n->x0, (double)n->x1, (double)n->oy, (double)n->y0,
            (double)n->y1, n->rule, plan < 0.0f ? "left" : plan > 0.0f ? "right" : "stays", (double)plan, (double)plany,
            (double)widen);
}

/* ---- the plan: the presenting thread, once a present ---- */

static int32_t first_child[NODES], next_sibling[NODES], parent_of[NODES];
static float ext_lo[NODES], ext_hi[NODES], plan_new[NODES], wide_new[NODES], tall_new[NODES], plany_new[NODES];
static float ext_ylo[NODES], ext_yhi[NODES];
static uint8_t ext_ydone[NODES], as_menu_new[NODES];
static int32_t used[NODES];   /* (the slots in use as the plan began, used_n of them) */
static int used_n;
#define FOR_USED(i) for (int u_ = 0, i = 0; u_ < used_n && (i = used[u_], 1); u_++)
static float rest[NODES];   /* (how far a pane is from where the plan sees it: its slide, or its page's along a strip) */
static uint8_t ext_done[NODES], rest_done[NODES];

static int drawn(const Node *n) { return n->type != PANE_TYPE_NULL && n->type != PANE_TYPE_PART; }

/* A pane noted is still one of its layout's: a layout let go (a menu closed) leaves its panes noted, and their addresses
   are used again by others */
static int alive(const Node *n) {
    if (!n->layout || recomp_load64(NULL, n->key + PANE_LAYOUT) != n->layout) return 0;
    const uint64_t base = recomp_load64(NULL, n->layout + LAYOUT_PANES);
    const uint32_t count = (uint32_t)recomp_load32(NULL, n->layout + LAYOUT_COUNT) & 0xffff;
    return n->key >= base && (n->key - base) % PANE_SIZE == 0 && (n->key - base) / PANE_SIZE < count;
}

/* How far a pane is from its place at rest: a sliding pane where its parent is, what is in it with it. The plan is the same throughout a slide.
   Each pane counts from its parent as they were when it was placed: the game's threads place a frame's panes while the
   plan reads them, and a page root from one frame with its panes from the last put them a frame's slide apart */
static float rest_of(int i, int depth) {
    if (rest_done[i] || depth > 64) return rest[i];
    const Node *n = &nodes[i];
    const int p = parent_of[i];
    float at = n->ox;   /* (its place at rest) */
    if (p >= 0) {
        const float up = nodes[p].ox - rest_of(p, depth + 1);
        at = n->rule == RULE_SLIDE ? up : up + n->ox - (isnan(n->from) ? nodes[p].ox : n->from);
    }
    rest[i] = n->ox - at;
    rest_done[i] = 1;
    return rest[i];
}

/* How far a pane and what is in it reach across: its origin, its own rectangle when it is drawn and not wider than
   SIZE_CAP, and its children's (as rules.py Extents) */
static void extents(int i, int depth) {
    if (ext_done[i]) return;
    const Node *n = &nodes[i];
    const float r = rest[i];
    float lo = n->ox - r, hi = n->ox - r;
    if (drawn(n) && n->x1 - n->x0 < SIZE_CAP) lo = fminf(lo, n->x0 - r), hi = fmaxf(hi, n->x1 - r);
    if (depth < 48)
        for (int c = first_child[i]; c >= 0; c = next_sibling[c]) {
            extents(c, depth + 1);
            lo = fminf(lo, ext_lo[c]), hi = fmaxf(hi, ext_hi[c]);
        }
    ext_lo[i] = lo, ext_hi[i] = hi, ext_done[i] = 1;
}

static void visit(int p, int depth);

/* The layouts showing their map page: one of their RULE_KEEP_ORIGIN panes at the canvas's origin. Their root is drawn as a
   menu's (as_menu): not moved, and on a narrower screen fitted with the rest of the pause menu */
enum { TAB_LAYOUTS = 16 };
static uint64_t tab_layouts[TAB_LAYOUTS];
static int tab_count;

static int in_tab(uint64_t layout) {
    for (int i = 0; i < tab_count; i++)
        if (tab_layouts[i] == layout) return 1;
    return 0;
}

static void push(int i, int side) { plan_new[i] = (float)side * ui_ex; }

/* A pane spanning the canvas: drawn the screen's width; what is in it is placed as usual */
static void widen(int i, int depth) {
    const float mid = (nodes[i].x0 + nodes[i].x1) * 0.5f - rest[i];
    if (drawn(&nodes[i]) && mid * mid < 1.0f) wide_new[i] = ui_wx;
    visit(i, depth + 1);
}

/* rules.py plan_layout's visit: each child of p by how far it and its contents reach */
static void visit(int p, int depth) {
    if (depth > 48) return;
    const float E = CANVAS_HALF;
    int group[2][64], count[2] = {0, 0};
    float glo[2] = {INFINITY, INFINITY}, ghi[2] = {-INFINITY, -INFINITY};
    for (int c = first_child[p]; c >= 0; c = next_sibling[c]) {
        const Node *n = &nodes[c];
        const float ox = n->ox - rest[c], x0 = n->x0 - rest[c], x1 = n->x1 - rest[c];
        if (n->lkind != LK_PLAIN || n->rule == RULE_KEEP || n->rule == RULE_LEFT || n->rule == RULE_RIGHT)
            continue;   /* (placed by code, by hand, or by name: below) */
        if (n->rule == RULE_KEEP_LOOK) {
            visit(c, depth + 1);
            continue;
        }
        if (n->rule == RULE_WIDEN) {
            widen(c, depth);
            continue;
        }
        if (n->type == PANE_TYPE_PART && fabsf(ox) < 1.0f) {   /* (a part at the canvas's origin: its own layout's panes) */
            visit(c, depth + 1);
            continue;
        }
        extents(c, 0);
        float lo = ext_lo[c], hi = ext_hi[c];
        if (drawn(n)) {
            if (x0 <= -E + EDGE_TOL && x1 >= E - EDGE_TOL) {
                widen(c, depth);
                continue;
            }
            if (first_child[c] < 0) lo = fminf(lo, x0), hi = fmaxf(hi, x1);
        }
        if (lo >= E || hi <= -E || (hi - lo > 2.5f * E && fabsf(ox) > 1.0f)) continue;   /* (off the canvas, a strip) */
        lo = fmaxf(lo, -E - EDGE_NEAR), hi = fminf(hi, E + EDGE_NEAR);
        if (lo <= -E + EDGE_TOL && hi >= E - EDGE_TOL) {
            visit(c, depth + 1);
            continue;
        }
        if (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > CROSS * (hi - lo)) {
            const int near_hi = hi >= E - EDGE_NEAR, near_lo = lo <= -E + EDGE_NEAR;
            if (near_hi != near_lo && (first_child[c] < 0 || hi - lo < E)) push(c, near_hi ? 1 : -1);
            else visit(c, depth + 1);
            continue;
        }
        if (lo == 0.0f || hi == 0.0f ||
            (lo < 0.0f && hi > 0.0f && (fabsf(lo + hi) * 0.5f < EDGE_NEAR || (first_child[c] >= 0 && fabsf(ox) < 1.0f)))) {
            visit(c, depth + 1);   /* (a centred container) */
            continue;
        }
        const int s = lo + hi >= 0.0f;
        if (count[s] < 64) group[s][count[s]++] = c;
        glo[s] = fminf(glo[s], lo), ghi[s] = fmaxf(ghi[s], hi);
    }
    for (int s = 0; s < 2; s++) {
        if (!count[s]) continue;
        const float outer = s ? ghi[s] : -glo[s];
        const int out = outer >= E - EDGE_NEAR || fabsf(glo[s] + ghi[s]) * 0.5f >= SIDE;
        for (int k = 0; k < count[s]; k++) {
            if (out) push(group[s][k], s ? 1 : -1);
            else visit(group[s][k], depth + 1);
        }
    }
}

/* How far a pane and what is in it reach up and down (as extents across, without the slides: those are across) */
static void extents_y(int i, int depth) {
    if (ext_ydone[i]) return;
    const Node *n = &nodes[i];
    float lo = n->oy, hi = n->oy;
    if (drawn(n) && n->y1 - n->y0 < SIZE_CAP) lo = fminf(lo, n->y0), hi = fmaxf(hi, n->y1);
    if (depth < 48)
        for (int c = first_child[i]; c >= 0; c = next_sibling[c]) {
            extents_y(c, depth + 1);
            lo = fminf(lo, ext_ylo[c]), hi = fmaxf(hi, ext_yhi[c]);
        }
    ext_ylo[i] = lo, ext_yhi[i] = hi, ext_ydone[i] = 1;
}

/* Up and down, for a HUD drawn smaller than its 16:9 size (a screen too narrow for it at that size: hud_scale): what sits at
   the top or bottom of the canvas, or a group far up or down, moves out to the screen's top or bottom; a container across
   the middle is looked into */
static void visit_y(int p, int depth) {
    if (depth > 48) return;
    const float E = CANVAS_HALF_Y;
    for (int c = first_child[p]; c >= 0; c = next_sibling[c]) {
        const Node *n = &nodes[c];
        if (n->lkind != LK_PLAIN || n->rule == RULE_KEEP) continue;
        if (n->type == PANE_TYPE_PART && fabsf(n->oy) < 1.0f) {
            visit_y(c, depth + 1);
            continue;
        }
        extents_y(c, 0);
        if (ext_ylo[c] >= E || ext_yhi[c] <= -E) continue;   /* (off the canvas) */
        const float lo = fmaxf(ext_ylo[c], -E - EDGE_NEAR), hi = fminf(ext_yhi[c], E + EDGE_NEAR);
        if ((lo <= -E + EDGE_TOL && hi >= E - EDGE_TOL) || (lo < 0.0f && hi > 0.0f && fminf(-lo, hi) > CROSS * (hi - lo))) {
            visit_y(c, depth + 1);
            continue;
        }
        const int up = lo + hi >= 0.0f;
        if ((up ? hi : -lo) >= E - EDGE_NEAR || fabsf(lo + hi) * 0.5f >= SIDE_Y) plany_new[c] = (up ? 1.0f : -1.0f) * ui_ey;
        else visit_y(c, depth + 1);
    }
}

static void plan(void) {
    const int on = ui_on;
    /* (the slots in use, once: every step walks only them, and only their entries are reset; walking the whole table
       took about 0.7 ms a present on the presenting thread) */
    used_n = 0;
    for (int w = 0; w < NODES / 64; w++)
        for (uint64_t b = __atomic_load_n(&used_bits[w], __ATOMIC_ACQUIRE); b; b &= b - 1) {
            const int i = w * 64 + __builtin_ctzll(b);
            if (__atomic_load_n(&nodes[i].key, __ATOMIC_ACQUIRE)) used[used_n++] = i;
        }
    FOR_USED(i) {
        first_child[i] = next_sibling[i] = parent_of[i] = -1;
        ext_done[i] = rest_done[i] = ext_ydone[i] = as_menu_new[i] = 0;
        rest[i] = plan_new[i] = plany_new[i] = 0.0f, wide_new[i] = tall_new[i] = 1.0f;
    }
    if (on) {
        FOR_USED(i) {
            if (!alive(&nodes[i])) {
                nodes[i].layout = 0;   /* (gone: not part of any tree; its slot free for another pane) */
                __atomic_store_n(&nodes[i].dead, 1, __ATOMIC_RELEASE);
                continue;
            }
            const Node *pn = nodes[i].parent ? node_find(nodes[i].parent) : NULL;
            const int p = pn && pn->layout ? (int)(pn - nodes) : -1;
            if (p < 0 || p == i) continue;
            parent_of[i] = p, next_sibling[i] = first_child[p], first_child[p] = i;
        }
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout) rest_of(i, 0);
        tab_count = 0;
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout && nodes[i].rule == RULE_KEEP_ORIGIN && fabsf(nodes[i].ox - rest[i]) < 1.0f &&
                !in_tab(nodes[i].layout) && tab_count < TAB_LAYOUTS)
                tab_layouts[tab_count++] = nodes[i].layout;
        FOR_USED(i) {   /* (a HUD's pane: its root layout is one of HUD_LAYOUTS, not showing the map page) */
            if (!nodes[i].key || !nodes[i].layout) continue;
            int top = i;
            for (int d = 0; parent_of[top] >= 0 && d < 64; d++) top = parent_of[top];
            as_menu_new[top] = (uint8_t)(nodes[top].hud && in_tab(nodes[top].layout));
            nodes[i].in_hud = (nodes[top].hud && !as_menu_new[top]) || (nodes[top].fade && fade_icon_planned);
        }
        /* (the roots: a HUD layout's root pane, each walked as the canvas's root; the map page's, drawn as a menu's, is not) */
        FOR_USED(i)
            if (nodes[i].key && nodes[i].layout && nodes[i].in_hud && parent_of[i] < 0 && nodes[i].lkind == LK_PLAIN) {
                /* (the HUD's: the menus are not moved) */
                visit(i, 0);
                if (ui_ey != 0.0f) visit_y(i, 0);
            }
        /* (by name: moved out, unless an ancestor already is) */
        FOR_USED(i) {
            const Node *n = &nodes[i];
            if (!n->key || !n->layout || !n->in_hud || (n->rule != RULE_LEFT && n->rule != RULE_RIGHT)) continue;
            int moved = 0;
            for (int a = parent_of[i], d = 0; a >= 0 && d < 64 && !moved; a = parent_of[a], d++) moved = plan_new[a] != 0.0f;
            if (!moved) push(i, n->rule == RULE_LEFT ? -1 : 1);
        }
        /* (the loading fade: its pane across the whole canvas, drawn over the whole picture. Kept whole (Full width), the
           game's projection already stretches it over the picture: left as it is) */
        FOR_USED(i) {
            const Node *n = &nodes[i];
            if (fade_planned && n->key && n->layout && n->fade && drawn(n) && n->x0 <= -CANVAS_HALF + EDGE_TOL && n->x1 >= CANVAS_HALF - EDGE_TOL)
                wide_new[i] = ui_fill_x, tall_new[i] = ui_fill_y;
        }
    }
    int changed = 0;
    FOR_USED(i) {
        Node *n = &nodes[i];
        if (!n->key || (n->plan == plan_new[i] && n->wide == wide_new[i] && n->tall == tall_new[i] && n->plany == plany_new[i] &&
                        n->as_menu == as_menu_new[i]))
            continue;
        decided(n, plan_new[i], wide_new[i], plany_new[i]);
        n->plan = plan_new[i], n->wide = wide_new[i], n->tall = tall_new[i], n->plany = plany_new[i], n->as_menu = as_menu_new[i];
        changed = 1;
    }
    if (changed && hos_wide_ui_force < 2) hos_wide_ui_force = 2;
}

/* The HUD's layout for the picture's shape (hos_wide_ratio: 0 at 16:9, the game's own). G is 16:9, r the picture's ratio.

   - The HUD keeps its 16:9 size, the size the game made it for: scaled with the picture's height as at 16:9, never
     stretched. What sits at a side or corner of its 16:9 canvas moves out to the screen's side (a wider screen) or in to it
     (a narrower one), as a layout made for that shape would place it; what spans the canvas is drawn the screen's width.
   - Wider than HUD_MAX (12:5, the widest "21:9" screens: 3840x1600) the side moves stop there, so on a 32:9 screen the
     hearts and the minimap stay where the eyes are; at 48:9, three 16:9 screens, the HUD stays on the middle one, as before.
   - Narrower than HUD_FULL (3:2, a 4:3 or 5:4 screen) the 16:9 HUD would take too much of the screen's width: it is drawn
     smaller, by the picture's ratio over HUD_FULL, and what sits at the top or bottom moves out to the screen's top or bottom.
   - The menus (every layout whose root is not the HUD's) keep their 16:9 shape as before on a wider screen (the Ultrawide
     menus row: in the middle, or stretched); on a narrower one they fit its width, in the middle, never stretched.
     HOS_WIDE_UI=0 turns the HUD's moves off.

   HOS_HUD_MAX_RATIO=<ratio> (0: the screen's edges at any width) and HOS_HUD_FULL_RATIO=<ratio> stand over the two limits. */
static const float GAME = 16.0f / 9.0f, HUD_MAX = 12.0f / 5.0f, HUD_FULL = 1.5f, SURROUND = 48.0f / 9.0f;

static float env_ratio(const char *name, float otherwise) {
    const char *e = getenv(name);
    return e && *e ? (float)atof(e) : otherwise;
}

/* The HUD's width reach (the ratio its side panes reach out to) on a picture of ratio r */
float hos_hud_reach(float r, float max) {
    if (r >= SURROUND * 0.99f && max == HUD_MAX) return GAME;   /* (three screens: the middle one's) */
    return max > 0.0f && r > max ? (max > GAME ? max : GAME) : r;
}

/* The HUD's size against 16:9's (1: the same) on a picture of ratio r */
float hos_hud_scale(float r, float full) { return r > 0.0f && full > 0.0f && r < full ? r / full : 1.0f; }

/* Each present (host/rx_backend.c host_wide_menus): the scales and moves for the picture's shape, and the plan */
void hos_wide_ui_tick(void) {
    static const char *env;
    static int looked;
    static float hud_max, hud_full, last[8];
    if (!looked) {
        looked = 1, env = getenv("HOS_WIDE_UI");
        const char *e = getenv("HOS_WIDE_UI_LOG");
        log_each = e ? atoi(e) : 0;
        hud_max = env_ratio("HOS_HUD_MAX_RATIO", HUD_MAX), hud_full = env_ratio("HOS_HUD_FULL_RATIO", HUD_FULL);
        for (int i = 0; i < 8; i++) last[i] = NAN;
    }
    const float r = hos_wide_ratio;
    const int hud = !(env && *env && atoi(env) == 0);
    float qx = 0.0f, qy = 0.0f, fit = 1.0f, ex = 0.0f, ey = 0.0f;
    if (r > GAME * 1.001f) {   /* (wider: the middle 16:9 at the height's scale, as before) */
        qx = GAME / r, qy = 1.0f;
        if (hud) ex = CANVAS_HALF * (hos_hud_reach(r, hud_max) / GAME - 1.0f);
    } else if (r > 0.0f && r < GAME * 0.999f) {   /* (narrower: never stretched) */
        const float k = hud ? hos_hud_scale(r, hud_full) : r / GAME;   /* (the HUD's size: off, the width's, as the menus) */
        qx = k * GAME / r, qy = k, fit = r / GAME / k;
        if (hud) ex = CANVAS_HALF * (r / (k * GAME) - 1.0f), ey = CANVAS_HALF_Y * (1.0f / k - 1.0f);
    }
    hos_ui_qx = qx, hos_ui_qy = qy;
    const float fill_x = qx > 0.0f && qx < 1.0f ? 1.0f / qx : 1.0f, fill_y = qy > 0.0f && qy < 1.0f ? 1.0f / qy : 1.0f;
    extern volatile uint32_t hos_wide_keep_mask;   /* (bit 3: the loading fade's screen kept whole, the Full width row) */
    const int fade_free = !(hos_wide_keep_mask & 8u);
    const int on = ex != 0.0f || ey != 0.0f || fit != 1.0f || fill_x != 1.0f || fill_y != 1.0f;
    const float now[8] = {ex, ey, fit, (float)on, r, fill_x, fill_y, (float)fade_free};
    if (memcmp(now, last, sizeof now)) {
        ui_ex = ex, ui_ey = ey, ui_wx = 1.0f + ex / CANVAS_HALF, ui_fit = fit, ui_fill_x = fill_x, ui_fill_y = fill_y, ui_on = on;
        fade_planned = fade_free, fade_icon_planned = fade_free && r > 0.0f && r < GAME * 0.999f;   /* (narrower only) */
        nodes_clear();   /* (every pane placed again, noted afresh) */
        hos_wide_ui_force = 3;   /* (a few frames: the layouts not drawn this frame catch up when they are) */
        log_left = log_each;
        hos_log("wide ui: picture %.4f: the HUD at %.0f%% of its 16:9 size, its sides moved %.0f and its top and bottom %.0f canvas "
                "units; the menus at %.0f%%", (double)r, (double)(qy > 0.0f ? qy * 100.0f : 100.0f), (double)ex, (double)ey,
                (double)(qy > 0.0f ? qy * fit * 100.0f : 100.0f));
        memcpy(last, now, sizeof last);
    } else if (hos_wide_ui_force) {
        hos_wide_ui_force--;
    }
    if (!on) return;
    const int64_t t0 = log_each > 0 ? hos_now_ns() : 0;
    plan();
    if (log_each > 0) {   /* (the plan's time a present, every 600 presents) */
        static int64_t spent;
        static int count;
        spent += hos_now_ns() - t0;
        if (++count == 600) {
            hos_log("wide ui: the plan took %.3f ms a present over the last 600", (double)spent / 600.0 / 1e6);
            spent = 0, count = 0;
        }
    }
}

/* ---- the game's threads ---- */

static _Thread_local uint64_t ui_parent;

void hos_wide_ui_parent(uint64_t parent) { ui_parent = parent; }

static float load_f(Ctx *c, uint64_t a) {
    const uint32_t u = (uint32_t)recomp_load32(c, a);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static void store_f(Ctx *c, uint64_t a, float f) {
    uint32_t u;
    memcpy(&u, &f, 4);
    recomp_store32(c, a, u);
}

void hos_wide_ui_placed(Ctx *c, uint64_t pane) {
    const uint64_t parent = ui_parent;
    ui_parent = 0;
    if (!ui_on) return;   /* (off: the transforms are the game's) */
    const Node *pn = parent ? node_find(parent) : NULL;
    if (parent && !(pn && pn->layout && !pn->dead && pn->tree)) {   /* (a menu's pane, or a HUD part in one: not noted. A pane
                                                                       noted before at its address no longer counts) */
        Node *old = node_find(pane);
        if (old && old->layout) node_drop(old);
        return;
    }
    Node *n = node_take(pane);
    if (!n) return;
    const float inherited = pn ? pn->moved : 0.0f, pushed = pn ? pn->pushed : 0.0f;
    const float inherited_y = pn ? pn->movedy : 0.0f, pushed_y = pn ? pn->pushedy : 0.0f;
    const uint32_t flags = (uint32_t)recomp_load32(c, pane + PANE_FLAGS);
    const float x = load_f(c, pane + PANE_X) - inherited, y = load_f(c, pane + PANE_Y) - inherited_y;
    const float w = load_f(c, pane + PANE_W) * fabsf(load_f(c, pane + PANE_XX));
    const float h = load_f(c, pane + PANE_H) * fabsf(load_f(c, pane + PANE_YY));
    const uint32_t ox = ((flags >> 11) & 0x7f) % 3;   /* (0 left, 1 centre, 2 right: f_8154a820) */
    const uint32_t oy = ((flags >> 11) & 0x7f) / 3 % 3;   /* (0 top, 1 centre, 2 bottom) */
    const uint64_t layout = recomp_load64(c, pane + PANE_LAYOUT);
    if (n->layout != layout) classify(n, pane, layout);
    n->tree = (uint8_t)(n->lkind == LK_PLAIN && (parent || n->hud || n->fade));
    n->parent = parent, n->layout = layout, n->type = (flags >> 22) & 0xf, n->ox = x, n->from = pn ? pn->ox : NAN;
    n->x0 = x - (ox == 1 ? w * 0.5f : ox == 2 ? w : 0.0f), n->x1 = n->x0 + w;
    n->oy = y, n->y1 = y + (oy == 1 ? h * 0.5f : oy == 2 ? h : 0.0f), n->y0 = n->y1 - h;
    const float side = pushed != 0.0f ? 0.0f : n->plan;   /* (made from a parent moved out: already moved with it) */
    n->moved = inherited + side, n->pushed = pushed + side;
    if (side != 0.0f) store_f(c, pane + PANE_X, x + inherited + side);
    const float up = pushed_y != 0.0f ? 0.0f : n->plany;
    n->movedy = inherited_y + up, n->pushedy = pushed_y + up;
    if (up != 0.0f) store_f(c, pane + PANE_Y, y + inherited_y + up);
    if (!parent && (!n->hud || n->as_menu) && !n->whole && n->lkind != LK_HELD && ui_fit != 1.0f) {
        /* (a menu's root on a narrower screen: smaller to fit its width, about the canvas's middle; what is in it is made
           from it) */
        for (int k = 0; k < 9; k++) store_f(c, pane + PANE_XX + 4 * (uint64_t)k, load_f(c, pane + PANE_XX + 4 * (uint64_t)k) * ui_fit);
        store_f(c, pane + PANE_X, load_f(c, pane + PANE_X) * ui_fit), store_f(c, pane + PANE_Y, load_f(c, pane + PANE_Y) * ui_fit);
    }
    if (!parent && log_each > 0) {   /* (each root layout, once: the HUD's or a menu's) */
        static uint64_t told[64];
        static int told_n;
        int known = 0;
        for (int i = 0; i < told_n && !known; i++) known = told[i] == n->lname;
        if (!known && told_n < 64) {
            char name[40];
            told[told_n++] = n->lname;
            guest_copy(name, sizeof name, n->lname);
            hos_log("wide ui: root layout %s: %s", name, n->hud ? "the HUD's" : n->whole ? "over the whole picture"
                    : n->lkind == LK_HELD ? "held at the HUD's size" : "a menu's");
        }
    }
    if (n->wide != 1.0f && n->wide > 0.0f && n->type == PANE_TYPE_CAPTURE) {   /* (a capture is not drawn by f_8154ac30: its
                                                                               across axis, wider or narrower) */
        for (int k = 0; k < 3; k++) store_f(c, pane + PANE_XX + 4 * (uint64_t)k, load_f(c, pane + PANE_XX + 4 * (uint64_t)k) * n->wide);
    }
}

/* The drawn size of a pane, its width in the low lane of v0 and its height in v1's (f_8154ac30: ldp s0, s1) */
void hos_wide_ui_size(Ctx *c, uint64_t pane) {
    if (!ui_on) return;
    const Node *n = node_find(pane);
    if (!n || n->dead || n->type == PANE_TYPE_CAPTURE) return;   /* (a capture: widened above) */
    float v;
    if (n->wide != 1.0f && n->wide > 0.0f) memcpy(&v, c->vreg[0], 4), v *= n->wide, memcpy(c->vreg[0], &v, 4);
    if (n->tall != 1.0f && n->tall > 0.0f) memcpy(&v, c->vreg[1], 4), v *= n->tall, memcpy(c->vreg[1], &v, 4);
}
