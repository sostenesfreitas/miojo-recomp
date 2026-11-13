"""The Insert menu's scaling rows: the Upscaler (opts.dlss: the game's own, NVIDIA DLSS and DLAA, AMD FSR 3.1, AMD FSR 4 and
its native AA) and the Render resolution's steps for it (gpu/vk/upscaler_rows.h), their order on the Graphics tab, options
files from before still loading, and their help (why DLSS or FSR 4 is not offered, the Sharpness DLSS wants). No game or GPU
required.

python -I -B tests/test_upscaler_rows.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def function(source, start):
    """A function's body from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        raise SystemExit("%s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    menu = (ROOT / "gpu" / "vk" / "vk_menu.cpp").read_text(encoding="utf-8")
    # the picture's way, top to bottom: the output, what fills it, the 3D's size and its sharpening (Resolution and
    # upscaling); then the frame rows, Reflex with them (Frame generation and latency); then the window (Screen)
    order = ['"Output resolution", &opts.output_res', '"Upscaler", &opts.dlss', '"DLSS model", &opts.dlss_model',
             '"Render resolution", &opts.resolution', '"Sharpness", &opts.sharpness',
             '"Frame generation", &opts.framegen', '"Frame generation by", &opts.framegen_by',
             '"Frame pacing", &opts.pace', '"NVIDIA Reflex", &opts.reflex',
             '"Window scaling filter", &opts.upscale']
    at = [menu.find(row) for row in order]
    if -1 in at:
        raise SystemExit("vk_menu.cpp: a scaling row is missing: " + order[at.index(-1)])
    if at != sorted(at):
        raise SystemExit("vk_menu.cpp: the scaling rows are not in the picture's order")
    if '"NVIDIA DLSS", &opts.' in menu or '"AMD FSR 4", &opts.' in menu:
        raise SystemExit("vk_menu.cpp: DLSS or FSR has a row of its own beside the Upscaler")
    # saved options: the renamed rows keep their keys (data, so read from the source)
    key = function(menu, "const char* ini_key(const Row& r) {")
    for value, name in (("opts.resolution", "resolution"), ("opts.upscale", "upscaling"), ("opts.dlss", "dlss")):
        if not re.search(r"&%s\)\s*return\s*\"%s\"" % (re.escape(value), name), key):
            raise SystemExit("ini_key: %s is not saved as %s (an options file from before would lose it)" % (value, name))
    # the menu's rules are upscaler_rows.h's, checked by behaviour in test_upscaler_rows.cpp: here only that the menu uses them
    # (the Upscaler by engine, Native on the Render resolution row: render_fixed, step_engine, step_render)
    uses = {
        "row_fixed": ("ur::upscaler_fixed(", "ur::render_fixed(", "ur::model_fixed("),
        "row_shown": ("ur::upscaler_shown(upscalers_available())", "ur::model_shown(dlss_why() == DLSS_OK)"),
        "set_row": ("ur::step_engine(", "ur::step_render(", "ur::snap_render(", "row_fixed("),
    }
    for name, calls in uses.items():
        body = function(menu, "%s %s(" % ("void" if name == "set_row" else "bool", name))
        for call in calls:
            if call not in body:
                raise SystemExit("%s does not use %s (upscaler_rows.h)" % (name, call))
    # a row for what this PC cannot run is hidden, not shown with a "Not available here" line: Reflex and Anti-Lag 2
    # where the driver lacks them (the Upscaler and the DLSS model above), and HDR's levels while HDR is off
    shown = function(menu, "bool row_shown(int i) {")
    for call in ("v != &opts.reflex || reflex_available()", "v != &opts.antilag || antilag_available()",
                 "(v != &opts.hdr_white && v != &opts.hdr_peak && v != &opts.hdr_contrast) || opts.hdr != 0"):
        if call not in shown:
            raise SystemExit("row_shown: %s is not there (a row for what is not available here, or HDR's levels with HDR "
                             "off, would show)" % call)
    if "Not available here" in menu:
        raise SystemExit("vk_menu.cpp: a row still says Not available here: hide it in row_shown instead")
    fixed = function(menu, "bool row_fixed(")
    if "reflex_available()" in fixed or "antilag_available()" in fixed:
        raise SystemExit("row_fixed: Reflex or Anti-Lag 2 is fixed where it does not run, when row_shown hides it")
    # every place that walks a tab's rows asks row_shown, so the keys, the pad, the mouse, the scrolling and the help all
    # see the same rows; the selection comes back inside the tab when a row after it goes
    for walk in ("int tab_row_count(int t) {", "int tab_row_to_global(int t, int local_idx) {"):
        if "row_shown(i)" not in function(menu, walk):
            raise SystemExit("%s does not skip the hidden rows" % walk.split("(")[0])
    if "if (selected >= tab_count) selected = tab_count - 1;" not in function(menu, "void menu_tick() {"):
        raise SystemExit("menu_tick: the selection is not brought back when rows after it are hidden")
    # the help: a row's own, then a line for the value chosen (Row::hint). The Upscaler's says why DLSS is not offered, and
    # DLSS's values ask for more Sharpness (it looks softer than the game's own)
    upscaler = function(menu, "const char* hn_upscaler(int v, int variant) {")
    for why in ("not in this build", "RX_VK_DLSS=0", "NVIDIA RTX GPU", "newer NVIDIA driver", "runtime was not found",
                "(int)dlss_why()", "RX_VK_FSR4=0", "AMD's files were not found", "no FSR 4 for this GPU", "(int)fsr4_why()"):
        if why not in upscaler:
            raise SystemExit("hn_upscaler: the help no longer says why DLSS or FSR is not offered (%s)" % why)
    if upscaler.count("Sharpness to") < 2:
        raise SystemExit("hn_upscaler: NVIDIA DLSS and DLAA do not say to set Sharpness to around 80%")
    resolution = function(menu, "const char* hn_resolution(int v, int variant) {")
    if resolution.count("Sharpness to around 80%") < 5 or "upscaler()" not in resolution:
        raise SystemExit("hn_resolution: DLSS's modes do not say to set Sharpness to around 80%, or not for the Upscaler shown")
    measure = function(menu, "float measure_help(ui::Ctx& c, float width) {")
    if "r.hint(v, k)" not in measure:
        raise SystemExit("measure_help leaves the values' lines out: the help's room would jump as a value changes")
    for row, hint in (('"Upscaler", &opts.dlss', "hn_upscaler"), ('"DLSS model", &opts.dlss_model', "hn_dlss_model"), ('"Render resolution", &opts.resolution', "hn_resolution"),
                      ('"NVIDIA Reflex", &opts.reflex', "hn_reflex"), ('"Window scaling filter", &opts.upscale', "hn_filter")):
        entry = menu[menu.index(row):]
        if hint not in entry[:entry.index("},")]:
            raise SystemExit("vk_menu.cpp: %s has no %s" % (row.split(",")[0], hint))
    # the DLSS model row: not the Quality preset's (preset_exempt), no restart, and its preset is what DLSS is made with
    model = menu[menu.index('"DLSS model", &opts.dlss_model'):]
    if not model[:model.index("},")].rstrip().endswith("hn_dlss_model, false, true"):
        raise SystemExit("vk_menu.cpp: the DLSS model row takes a restart or turns the Quality preset to Custom")
    # the Window scaling filter's sizes: only from the game's frames. The start-up screen is drawn at the window's size
    # (present_image with no source): taken as the picture's, the row read "Not needed" while the output was another size
    replay = (ROOT / "gpu" / "vk" / "vk_replay.cpp").read_text(encoding="utf-8")
    present = function(replay, "void present_image(")
    note = present.index("menu_note_present_sizes(")
    if "if (src) {" not in present[:note] or present.rindex("if (src) {", 0, note) < present.rindex("\n    }", 0, note):
        raise SystemExit("present_image: the start-up screen's frames (no src) count as the game's picture for the filter row")
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    feature = function(dlss, "int prepare(")
    if "wanted_preset()" not in feature or "s.feature_preset == preset" not in feature:
        raise SystemExit("vk_dlss.cpp: DLSS's feature is not made again for another DLSS model")
    if "upscaler_rows::dlss_preset(opts.dlss_model" not in function(dlss, "unsigned wanted_preset() {"):
        raise SystemExit("vk_dlss.cpp: the DLSS model row (and RX_VK_DLSS_PRESET over it) is not the preset asked for")
    temporal = (ROOT / "gpu" / "vk" / "vk_temporal.cpp").read_text(encoding="utf-8")
    mode = function(temporal, "int temporal_mode() {")
    if "dlss_why() != DLSS_OK" not in mode or "fsr4_why() != FSR_OK" not in mode:
        raise SystemExit("temporal_mode asks for an engine where it cannot run (DLAA or FSR 4's native AA would hold the 3D at "
                         "full scale for nothing)")
    if "std::clamp(" in mode:
        raise SystemExit("temporal_mode clamps the row's value: FSR 3.1 (3) would run on the chain")
    if "fsr3_wanted()" not in mode:
        raise SystemExit("temporal_mode runs DLSS or FSR 4 while FSR 3.1 upscales (two upscalers, two jitters)")
    fsr3 = (ROOT / "gpu" / "vk" / "vk_fsr3.cpp").read_text(encoding="utf-8")
    if "!temporal_env_chosen()" not in function(fsr3, "bool fsr3_chosen() {"):
        raise SystemExit("fsr3_chosen: a saved FSR 3.1 drops RX_VK_DLSS / RX_VK_FSR4, which go over the row")
    # FSR 3.1 is an engine of the chain (the skinned draws' motion), unless RX_VK_FSR3_CHAIN=0 keeps its own inputs: the chain
    # takes it before its fallback to the game's own, fsr3_wanted (its own inputs) is then off, and the chain has its slot
    if "return !fsr3_on_chain() && fsr3_chosen();" not in function(fsr3, "bool fsr3_wanted() {"):
        raise SystemExit("fsr3_wanted: FSR 3.1 would upscale with its own inputs while it draws on the chain (two upscales)")
    on_chain = "if (fsr3_on_chain() && fsr3_chosen()) return ur::FSR3;"
    if on_chain not in mode or mode.index(on_chain) > mode.index("if (!ur::is_dlss(v) && !ur::is_fsr4(v)) return ur::GAME;"):
        raise SystemExit("temporal_mode does not run the row's FSR 3.1 on the chain before falling back to the game's own")
    if "fsr3_engine()" not in function(temporal, "Engine& engine_of("):
        raise SystemExit("engine_of has no slot for FSR 3.1's engine")
    # The chain's depth is reversed for FSR 3.1 (the prep's bit 3, from fsr_flip): the frame says so, for whatever else reads it
    upscale = function(temporal, "bool temporal_upscale(")
    if "(fsr_flip ? 8 : 0)" not in upscale or "f.depth_reversed = fsr_flip;" not in upscale:
        raise SystemExit("temporal_upscale reverses the depth without saying so in the frame (TemporalFrame::depth_reversed)")
    # DLSS Frame Generation (NGX) copies the chain's depth: it must hand NGX the depth convention the frame says
    dlss = (ROOT / "gpu" / "vk" / "vk_dlss.cpp").read_text(encoding="utf-8")
    if "g.depth_reversed = f.depth_reversed;" not in dlss or "o.depthInverted = g.depth_reversed;" not in dlss:
        raise SystemExit("DLSS Frame Generation tells NGX the chain's depth is not reversed while FSR 3.1 reverses it")
    chosen = function(temporal, "bool temporal_env_chosen() {")
    if "dlss_why() == DLSS_OK" not in chosen or "fsr4_why() == FSR_OK" not in chosen:
        raise SystemExit("temporal_env_chosen: a switch for an engine that cannot run here leaves the game's own, not the saved FSR 3.1")
    # FSR 3.1 and FSR 4 are offered where they run, and the row steps over what does not run here
    available = function(menu, "unsigned upscalers_available() {")
    if "fsr3::supported()" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr3.cpp whether FSR 3.1 runs here")
    if "fsr4_why() == FSR_OK" not in available:
        raise SystemExit("upscalers_available does not ask vk_fsr4.cpp whether FSR 4 runs here")
    # Off: offered on every PC, shown as what runs, and the game's network left out for it where DLSS's and FSR 4's are
    if "ur::bit(ur::OFF)" not in available:
        raise SystemExit("upscalers_available does not offer Off (no upscaler) everywhere")
    if "temporal_off()" not in function(menu, "int upscaler() {"):
        raise SystemExit("upscaler(): the row does not show Off while it runs")
    if "bool temporal_off() { return opts.dlss == ur::OFF && temporal_mode() == ur::GAME; }" not in temporal:
        raise SystemExit("temporal_off: Off is not the row's Off with no engine over it (RX_VK_DLSS, RX_VK_FSR4, FSR 3.1)")
    upscale = function(temporal, "bool temporal_upscale(")
    if "temporal_off() && stretch(" not in upscale:
        raise SystemExit("temporal_upscale: the Upscaler's Off does not stretch in place of the game's network")
    stretch = function(temporal, "bool stretch(")
    for need in ("s.linear, in_view", "s.out_layout", "c.valid", "in->width != c.width", "VK_FORMAT_B10G11R11_UFLOAT_PACK32"):
        if need not in stretch:
            raise SystemExit("stretch: %s is missing (Off's linear stretch of the scene only, into the picture)" % need)
    if "!temporal_wanted() && !temporal_off()" not in function(temporal, "void temporal_note_scene_draw("):
        raise SystemExit("temporal_note_scene_draw: no scene camera with Off, so the fixed shares' networks are never stretched")
    draw = (ROOT / "gpu" / "vk" / "vk_draw.cpp").read_text(encoding="utf-8")
    if "(!temporal_wanted() && !temporal_off())" not in function(draw, "bool temporal_stand_in("):
        raise SystemExit("temporal_stand_in: the game's network runs with Off")
    if "(temporal_wanted() || temporal_off() || fsr3_inputs::wanted()) && temporal_rejected_network(" not in function(draw, "bool upscale_stub("):
        raise SystemExit("upscale_stub: a network that did not translate is not stretched with Off, or not offered to FSR 3.1")
    rejected = function(draw[draw.rindex("static bool temporal_rejected_network("):], "static bool")   # (the definition)
    fsr3 = rejected.find("fsr3_upscale(cmd, key, false, src, in_view, dst, out_view, 0)")
    if fsr3 < 0 or fsr3 > rejected.find("temporal_upscale(cmd, key, false"):
        raise SystemExit("temporal_rejected_network: FSR 3.1 is not offered a network that did not translate (before the "
                         "temporal upscaler, as fsr3_stand_in comes before temporal_stand_in): its fixed Render resolution "
                         "steps are a plain stretch on cards that leave those networks out (issue #1829)")
    if "upscalers_available()" not in function(menu, "void set_row(") or "upscalers_available()" not in function(menu, "bool row_fixed("):
        raise SystemExit("the Upscaler row does not step over (or fix on) what runs here")
    with tempfile.TemporaryDirectory(prefix="upscaler-rows-") as tmp:
        exe = Path(tmp) / "upscaler-rows"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_upscaler_rows.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
