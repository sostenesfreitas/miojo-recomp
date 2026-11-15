"""Windows Intel's compiler is entered by one thread, and a fault creating a shader module leaves that program out.

Intel UHD on Windows (igvk64) read address 0x170 during the first scene and the process ended. The stack was two frames
in hookshot_vk.dll and then the thread start, and the log had no "driver faulted compiling" line, so the call was not
inside guarded_graphics or guarded_compute. vkCreateShaderModule (translation, the movie and upscale helpers, a masked
clear) and gpl_link's vkCreateGraphicsPipelines were not guarded, and graphics pipelines were still built side by side.
NVIDIA, AMD, and Intel on other systems are not held. The Debug tab's Intel shader compiles row (Intel graphics on
Windows only), or RX_VK_NO_INTEL_SERIAL over it, leaves them side by side. The renderer's own compute pipelines (ASTC, BC
and unswizzle, write-back diff, reactive mask, FSR 3.1 and its inputs and interpolation, frame generation, the
cooperative-matrix check) are guarded and held too: one of them built beside a worker's is the same two threads in it.

python -I -B tests/test_intel_compiler_gate.py
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


def brace_block(text, open_at):
    """text[open_at] is '{'; return through its match, including the braces."""
    end = open_at + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[open_at:end]


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: " + why)


def main():
    internal = (ROOT / "gpu/vk/vk_internal.h").read_text(encoding="utf-8")
    gate = block(internal, "struct IntelDriverGate {")
    on = block(internal, "inline bool intel_gate_on() {")
    need("#ifdef _WIN32" in on, "the lock is not limited to Windows")
    need("!intel_compiles_side_by_side()" in on, "the Intel shader compiles row does not turn the lock off")
    need("dev.props.vendorID == 0x8086" in on, "a vendor other than Intel takes the lock")
    need("if (!intel_gate_on()) return;" in gate, "the lock is taken where it is off")
    # (an ask that compiles nothing never waits behind a worker's compile: it is answered cold, HumanBean's review of #252)
    need("std::try_to_lock" in gate and "busy = !hold.owns_lock();" in gate, "an ask waits for the lock")
    menu = (ROOT / "gpu/vk/vk_menu.cpp").read_text(encoding="utf-8")
    need("bool intel_compiles_side_by_side() { return opts.intel_compiles != 0; }" in menu,
         "the lock does not read the Intel shader compiles row")
    need('if (const char* e = getenv("RX_VK_NO_INTEL_SERIAL")) env_override(&opts.intel_compiles, !env_value_off(e));'
         in block(menu, "void menu_init() {"), "RX_VK_NO_INTEL_SERIAL does not turn the lock off")
    need('{TAB_DEBUG, "Intel shader compiles", &opts.intel_compiles, 0, 1, 1, tx_intel_compiles,' in menu,
         "no Intel shader compiles row")
    shown = block(menu, "bool intel_compiler_row() {")
    need("#ifdef _WIN32" in shown and "dev.props.vendorID == 0x8086" in shown, "the row shows where the lock never holds")
    need("(v != &opts.intel_compiles || intel_compiler_row())" in block(menu, "bool row_shown(int i) {"),
         "the row is not limited to Intel graphics on Windows")
    need('(value == &opts.intel_compiles && getenv("RX_VK_NO_INTEL_SERIAL"))' in block(menu, "bool row_fixed(const int* value) {"),
         "the row can be changed while RX_VK_NO_INTEL_SERIAL holds it")
    need("std::unique_lock<std::mutex>(mu)" in gate, "the compiler is not entered by one thread")
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    win = block(draw, "__declspec(noinline) VkResult guarded_shader_module(")
    need("__try" in win and "vkCreateShaderModule" in win and "return (VkResult)-77;" in win,
         "a fault in vkCreateShaderModule is not turned into a left-out program")
    linux_at = draw.index("#else", draw.index("__declspec(noinline) VkResult guarded_shader_module("))
    linux = block(draw, "VkResult guarded_shader_module(", linux_at)
    need("sigsetjmp(fault_jmp, 1)" in linux and "vkCreateShaderModule" in linux,
         "Linux does not guard vkCreateShaderModule")
    for fn in ("VkResult cached_graphics(", "VkResult cached_compute("):
        body = block(draw, fn)
        need(body.index("IntelDriverGate") < body.index("create_cached"), fn + " compiles before it takes the lock")
        need("IntelDriverGate gate(compiles_nothing(ci->flags));" in body and
             "if (gate.busy) return intel_gate_busy();" in body, fn + " makes an ask wait for a compile")
    graphics = block(draw, "VkResult graphics_with(")
    need("return cached_graphics" in graphics, "the play-cache path takes the lock twice")
    need("if (gate.busy) return intel_gate_busy();" in graphics, "graphics_with makes an ask wait for a compile")
    # (an ask answered cold because the gate was busy is counted and logged: the pipeline may be in a cache all the same)
    busy = block(draw, "VkResult intel_gate_busy()")
    need("++st_intel_gate_busy" in busy and "logf(" in busy and "return VK_PIPELINE_COMPILE_REQUIRED;" in busy,
         "an ask answered cold for a busy gate is not counted")
    # (a dispatch's module, for a program read without its code: made on the replay thread when the gate is free, so a
    # prepared pipeline is had at once, and left to the worker, not waited for, while a worker holds it. Its code is looked
    # at under the cache's lock once the CAS is won, not before)
    dispatch = draw[draw.index("bool on_worker = false;"):]
    dispatch = dispatch[:dispatch.index("jobs_push_serial")]
    need("(program_cache_complete_ask(p, &on_worker) || on_worker ||" in dispatch and "if (!program_cache_complete(q))" in dispatch,
         "on Intel a dispatch waits for the gate to make its module on the replay thread")
    need("code_words" not in dispatch, "the dispatch reads a program's code_words before its CAS is won")
    need("if (on_worker) intel_gate_busy();" in dispatch, "a dispatch's module left to the worker is not counted")
    # (the census: a pipeline the worker found in a cache is not counted as compiled)
    need("if (use.on && !on_worker) use.pipeline_made(cached ? 2 : 3);" in dispatch and
         "made_how->store(use_counters.made_how ? use_counters.made_how : 2" in dispatch,
         "a pipeline the worker took from a cache is counted as compiled")
    need("use.pipeline_made(made_how->load(" in draw, "the census never hears how the worker made it")
    cache = (ROOT / "gpu/vk/vk_progcache.cpp").read_text(encoding="utf-8")
    ask = block(cache, "bool program_cache_complete_ask(")
    need("complete_from(p, disk_in, &disk_mu, busy)" in ask, "the dispatch's ask waits for the gate")
    complete = block(cache, "bool complete_from(")
    need("make_module(spirv, p.key, busy)" in complete and "p.code_at = code_at, p.code_words = words;" in complete,
         "a busy gate fails the program, or loses its code")
    link = block(draw, "VkPipeline gpl_link(")
    need(link.index("IntelDriverGate") < link.index("guarded_graphics"), "gpl_link's pipeline is not guarded")
    need("vkCreateGraphicsPipelines" not in link, "gpl_link still calls the driver with no guard")
    # (a pipeline cache's data fetched while another thread builds: the same crash in igvk64. main's fetch lock covers it,
    # PcacheFetch, #190 (tests/test_pcache_fetch_lock.py). The gate stays outside that lock: a builder
    # takes the gate, then PcacheUse's shared side, so a fetch taking the gate inside PcacheFetch would wait for a builder
    # that waits for it)
    for fn in ("size_t play_file_write(", "void pcache_init()", "bool pcache_saved_now()", "void pipeline_cache_save(bool force)"):
        body = block(draw, fn)
        need("PcacheFetch" in body, fn + " fetches a cache's data outside the fetch lock")
        need("IntelDriverGate" not in body, fn + " takes Intel's gate around a fetch (the builders take it outside the fetch lock)")
    for fn in ("VkResult guarded_graphics_held(", "VkResult guarded_compute_held("):
        need("IntelDriverGate" not in block(draw, fn), fn + " takes Intel's gate inside the fetch lock")
    clear = block(draw, "VkPipeline clear_pipeline(")
    gate_at = clear.index("IntelDriverGate gate;")
    held = brace_block(clear, clear.rindex("{", 0, gate_at))
    need("cached_graphics(" not in held, "a masked clear holds the lock across cached_graphics (it takes the same lock)")
    need(clear.index(held) < clear.index("cached_graphics"), "the masked clear's modules are created after the pipeline")
    module = block((ROOT / "gpu/vk/vk_shader.cpp").read_text(encoding="utf-8"),
                   "VkShaderModule make_module(")
    need("IntelDriverGate" in module and "guarded_shader_module" in module, "translation still creates a module unguarded")
    need("IntelDriverGate gate(busy != nullptr);" in module and "*busy = true;" in module,
         "make_module waits for the gate where the dispatch only tries it")
    need(module.index("driver_faulted_program(program)") < module.index("IntelDriverGate"),
         "a program the driver faulted on is handed to it again")
    need("if (r == (VkResult)-77) driver_faulted_module(program);" in module,
         "a fault making a module is not kept in hookshot_driver_faults.txt")
    temporal = (ROOT / "gpu/vk/vk_temporal.cpp").read_text(encoding="utf-8")
    pipe = block(temporal, "bool make_pipeline(")
    actor = block(temporal, "bool make_actor_pipeline()")
    need("guarded_shader_module" in pipe and "guarded_compute" in pipe, "a temporal compute pipeline is unguarded")
    need("guarded_shader_module" in actor and "guarded_graphics" in actor, "the actor-motion pipeline is unguarded")
    need("vkCreateShaderModule" not in pipe and "vkCreateComputePipelines" not in pipe, "make_pipeline still calls the driver raw")
    need("vkCreateGraphicsPipelines" not in actor, "make_actor_pipeline still calls the driver raw")
    for name in ("vk_texture", "vk_memory", "vk_reactive", "vk_fsr3", "vk_fsr3_inputs", "vk_fsr3fi", "vk_framegen",
                 "vk_coopmat"):
        src = (ROOT / "gpu/vk" / (name + ".cpp")).read_text(encoding="utf-8")
        need("vkCreateShaderModule(" not in src and "vkCreateComputePipelines(" not in src,
             name + ".cpp still calls the compiler with no guard")
        need("IntelDriverGate gate;" in src, name + ".cpp does not take the lock")
    fsr3 = (ROOT / "gpu/vk/vk_fsr3.cpp").read_text(encoding="utf-8")
    for at in [i for i in range(len(fsr3)) if fsr3.startswith("IntelDriverGate gate;", i)]:
        held = brace_block(fsr3, fsr3.rindex("{", 0, at))
        need("shared_compute_pipeline(" not in held, "FSR 3.1 holds the lock across shared_compute_pipeline (it takes it too)")
    print("Intel's compiler on Windows is entered by one thread, and a fault leaves that shader out: ok")


if __name__ == "__main__":
    main()
