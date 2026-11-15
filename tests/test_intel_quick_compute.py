"""On Intel's one-at-a-time compute builds a dispatch's pipeline is made first without the optimizer, then with it.

A UHD Graphics (i3-1115G4, 2 cores, 4 threads, 16 GB shared) took 0.3 s to 6.5 min with the optimizer for each of 37 of the
game's dispatches, 22 minutes on the one serial thread, and every pass over the scene (its light, its exposure) was left
out until its turn: the Deku Tree's dialogue drawn blown out. The first build is made without the optimizer; the optimized
one is queued behind the builds asked for first and bound once made, the first kept for the session. The Debug tab's Intel
quick compute builds row (Intel graphics on Windows, as the Intel shader compiles row), or RX_VK_COMPUTE_QUICK_FIRST over
it, turns that off. The same tab's Set up for a weak laptop button (integrated graphics) sets that laptop's rows at once.

python -I -B tests/test_intel_quick_compute.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker, start_at=0):
    start = text.index(marker, start_at)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    dispatch = block(draw, "void dispatch(VkCommandBuffer cmd, NvnState& s, uint32_t x, uint32_t y, uint32_t z, uint64_t indirect) {")
    build = dispatch[dispatch.index("auto build = [job, on_worker, made_how] {"):dispatch.index("if (serial) jobs_push_serial(std::move(build)")]
    need("const bool quick = serial && intel_quick_compute() && !(ci.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT);" in build,
         "the quick build is not limited to the serial builds and the row")
    need("if (quick) ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;" in build, "the first build keeps the optimizer")
    made = build[build.index("q.compute_pipeline = made;"):]
    need(made.index("if (quick) jobs_push_serial([job] { compute_optimize(const_cast<Program&>(*job)); });") <
         made.index("const int64_t ms"), "the optimized build is not queued once the quick one is made")
    need("jobs_push_serial([job] { compute_optimize(const_cast<Program&>(*job)); }, " not in build,
         "the optimized build is queued in the front lane, ahead of the passes still to be built")
    # (bound once made, the quick one kept: a recording may hold it)
    need("const VkPipeline optimized = p.compute_optimized.load(std::memory_order_acquire);" in dispatch and
         "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, optimized ? optimized : p.compute_pipeline);" in dispatch,
         "the dispatch does not bind the optimized pipeline once made")
    opt = block(draw, "void compute_optimize(Program& q) {")
    need("compute_pipeline_info(q, true)" in opt and "DISABLE_OPTIMIZATION" not in opt, "the second build is not the optimized one")
    need("q.compute_optimized.store(made, std::memory_order_release);" in opt, "the optimized pipeline is not published")
    need("vkDestroyPipeline" not in opt and "compute_pipeline =" not in opt,
         "the pipeline without the optimizer is destroyed or replaced under a recording")
    shader = (ROOT / "gpu/vk/vk_shader.h").read_text(encoding="utf-8")
    need("std::atomic<VkPipeline> compute_optimized{VK_NULL_HANDLE};" in shader, "Program has no slot for the optimized pipeline")

    # (before play, the same: the preparation at the build and a start's precompile ask for the pipeline the dispatch's first
    # build asks for. With the optimizer one build took 389.5 s, past the preparation's 300 s with nothing finished: it was
    # stopped, hookshot_prepare_skipped left, and every later start built each shader in play)
    quick = block(draw, "bool precompile_compute_quick(VkComputePipelineCreateInfo& ci) {")
    need("if (!serial_builds_on() || !intel_quick_compute() || (ci.flags & VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT)) return false;"
         in quick and "ci.flags |= VK_PIPELINE_CREATE_DISABLE_OPTIMIZATION_BIT;" in quick,
         "the precompile's quick build is not limited to the serial builds and the row")
    warm = block(draw, "void precompile_warm(void (*progress)(size_t done, size_t total)) {")
    warm_computes = warm[warm.index("program_cache_borrow(computes[i]"):]
    need(warm_computes.index("compute_pipeline_info(p, true)") < warm_computes.index("precompile_compute_quick(ci);") <
         warm_computes.index("cached_compute(&ci, &made)"), "the warm builds the optimized pipeline on Intel")
    run = block(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {")
    run_computes = run[run.index("Program& p = *computes[i];"):run.index("} else if (i < computes.size() + fallbacks.size()) {")]
    need(run_computes.index("VkPipeline made = compute_pipeline_cached(p, false);") <
         run_computes.index("quick = precompile_compute_quick(ci);") < run_computes.index("cached_compute(&ci, &made)"),
         "the precompile does not take a cached optimized pipeline before building a quick one")
    owed = run_computes[run_computes.index("if (!pre->abandoned) {"):]
    need("if (made && quick) p.optimize_owed.store(true, std::memory_order_release);" in owed and
         owed.index("p.optimize_owed") < owed.index("} else if (made) vkDestroyPipeline"),
         "the precompile's quick pipeline owes no optimized build, or owes it once abandoned")
    need("jobs_push" not in run_computes, "the precompile queues optimized builds for programs never dispatched")
    need("std::atomic<bool> optimize_owed{false};" in shader, "Program has no flag for the owed optimized build")
    first = dispatch[dispatch.index("if (p.optimize_owed.load(std::memory_order_relaxed)"):]
    need(first.index("p.optimize_owed.exchange(false, std::memory_order_acq_rel)") < first.index(
         "jobs_push_serial([q] { compute_optimize(*q); });") < first.index("const VkPipeline optimized ="),
         "the first dispatch of a precompiled quick pipeline does not queue its optimized build once, before binding")

    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    need('{TAB_DEBUG, "Intel quick compute builds", &opts.intel_quick, 0, 1, 1, tx_onoff,' in menu, "no Intel quick compute builds row")
    need("(v != &opts.intel_quick || intel_compiler_row())" in block(menu, "bool row_shown(int i) {"),
         "the quick builds row is not limited to Intel graphics on Windows")
    need('if (const char* e = getenv("RX_VK_COMPUTE_QUICK_FIRST")) env_override(&opts.intel_quick, !env_value_off(e));'
         in block(menu, "void menu_init() {"), "RX_VK_COMPUTE_QUICK_FIRST does not set the row")
    need("bool intel_quick_compute() { return opts.intel_quick != 0 && intel_compiler_row(); }" in menu,
         "the quick builds do not follow the row, or run where it is hidden (Mesa's ANV ignores DISABLE_OPTIMIZATION)")
    header = (ROOT / "gpu/vk/vk_menu.h").read_text(encoding="utf-8")
    need("int intel_quick = 1;" in header, "quick compute builds are not on by default")

    # (the weak laptop button: integrated graphics only, the lightest rows, the restart asked for, saved)
    need('{TAB_DEBUG, "Set up for a weak laptop", nullptr, 0, 0, 0, nullptr,' in menu, "no Set up for a weak laptop button")
    need("(!is_laptop_row(rows[i]) || laptop_row())" in block(menu, "bool row_shown(int i) {") and
         "return dev.props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;" in block(menu, "bool laptop_row() {"),
         "the weak laptop button shows on a dedicated card")
    apply = block(menu, "void weak_laptop_apply() {")
    for want, why in (("apply_preset(1);", "Minimum's rows and memory budgets"),
                      ("opts.resolution = ur::snap_render(upscaler(), 6);", "Render resolution 33%"),
                      ("opts.upscale = 0, opts.sharpness = 0;", "the plain window scale"),
                      ("opts.framegen = 0;", "frame generation off"),
                      ("opts.intel_compiles = 0, opts.intel_quick = 1;", "Intel's shaders one at a time, quick compute builds"),
                      ("opts.preset = 0;", "the Quality preset left on Minimum"),
                      ("restart_state.note(now);", "the restart asked for"),
                      ("save();", "saved")):
        need(want in apply, "the weak laptop button does not set " + why)
    need(apply.index("apply_preset(1);") < apply.index("opts.resolution = "), "Minimum's 50% is set over the 33%")
    tick = block(menu, "void menu_tick(")
    need("else if (press[7] && is_laptop_row(r)) weak_laptop_apply(), press[7] = false, changed = true;" in tick,
         "Enter on the weak laptop button does nothing")
    rows = block(menu, "void draw_debug_rows(")
    need('choice_cell(c, va, vb, laptop_applied ? "Applied" : "Apply", selected, false, false);' in rows,
         "the weak laptop button is drawn without its value")
    auto = (ROOT / "gpu/vk/auto_settings.h").read_text(encoding="utf-8")
    need("RES_ULTRA_PERFORMANCE = 6" in auto, "the button's 33% is not auto_settings.h's RES_ULTRA_PERFORMANCE")
    print("PASS: Intel's serial compute builds draw without the optimizer first, and the weak laptop button sets the lightest rows")


if __name__ == "__main__":
    main()
