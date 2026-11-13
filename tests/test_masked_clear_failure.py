"""A masked clear whose layout, shader module or pipeline the driver did not make is not kept, and is tried again (#1787).

clear_pipeline (gpu/vk/vk_draw.cpp) makes the full-screen-triangle pipeline of a masked clear on its first use. It ignored
what vkCreatePipelineLayout and vkCreateShaderModule returned, so a failed layout or module went on into the next calls,
and it stored whatever cached_graphics gave back: a pipeline the driver refused was cached as VK_NULL_HANDLE, and that clear
was left out for the rest of the run, even when the failure was transient. Now each step's result is checked, a failure
keeps nothing (the vertex module is freed when the fragment module fails), and the key is tried again on its next clears,
kClearTries times, then once every kClearRetryEvery of its clears: a driver that always refuses it costs no compile every
frame, and a device that was short of memory for a while still gets the clear back.

python -I -B tests/test_masked_clear_failure.py
"""
import re
from pathlib import Path
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))   # (tests/srccut.py: python -I leaves this folder off the path)
from srccut import block   # noqa: E402

ROOT = Path(__file__).resolve().parents[1]
DRAW = ROOT / "gpu" / "vk" / "vk_draw.cpp"


def need(cond, why):
    if not cond:
        raise SystemExit("FAIL: clear_pipeline: " + why)


def main():
    draw = DRAW.read_text(encoding="utf-8")
    body = block(draw, "VkPipeline clear_pipeline(const Targets& at, int index, uint32_t mask, int kind) {")
    need(re.search(r"constexpr int kClearTries = \d+;", draw), "no bound on the tries of a key that failed")
    every = re.search(r"constexpr int kClearRetryEvery = (\d+);", draw)
    need(every and int(every.group(1)) > 1, "a key that backed off is never tried again, or is tried every clear")
    gate = "if (failed.tries >= kClearTries && ++failed.waited < kClearRetryEvery) return VK_NULL_HANDLE;"
    need(gate in body, "a key that failed kClearTries times is not backed off (or never tried again)")
    need(body.index(gate) < body.index("failed.waited = 0;") < body.index("vkCreateShaderModule"),
         "the back-off is checked after the work it should save, or its wait is not restarted on a try")
    need("++failed.tries == kClearTries" in body, "the failure is logged every try, or never")
    # (the gate itself, run: 3 failures, then one try in kClearRetryEvery clears)
    tries, waited, made, n = 0, 0, [], int(every.group(1))
    for clear in range(3 + 3 * n):
        if tries >= 3:
            waited += 1
            if waited < n:
                continue
        waited = 0
        made.append(clear)
        tries += 1
    need(made == [0, 1, 2, 2 + n, 2 + 2 * n, 2 + 3 * n], "the back-off does not try once every kClearRetryEvery clears")
    # (every creation's result is looked at, and a failure goes out through fail, which counts it)
    for call, check in (("vkCreatePipelineLayout(", "if (lr != VK_SUCCESS || !made) return fail("),
                        ("vkCreateShaderModule(dev.device, &mi, nullptr, &vm)", "if (vr != VK_SUCCESS || !vm) return fail("),
                        ("vkCreateShaderModule(dev.device, &mi, nullptr, &fm)", "if (fr != VK_SUCCESS || !fm) {"),
                        ("cached_graphics(&ci, &pl)", "if (pr != VK_SUCCESS || !pl) {")):
        need(call in body, "lost the call " + call)
        need(check in body, "the result of %s is not checked" % call)
        need(body.index(call) < body.index(check), "the result of %s is checked before the call" % call)
    need(re.search(r"VkResult \w+ = vkCreatePipelineLayout\(dev\.device, &li, nullptr, &made\);", body),
         "the layout is made straight into clear_layout: a failed one would be kept")
    need(body.index("clear_layout = made;") > body.index("if (lr != VK_SUCCESS || !made)"), "a failed layout is kept")
    frag = block(body, "if (fr != VK_SUCCESS || !fm) {")
    need("vkDestroyShaderModule(dev.device, vm, nullptr);" in frag, "the vertex module leaks when the fragment one fails")
    pipe = block(body, "if (pr != VK_SUCCESS || !pl) {")
    need("return fail(" in pipe, "a failed pipeline is not counted as a failure")
    need(body.index("if (pr != VK_SUCCESS || !pl)") < body.index("clear_pipes[key] = pl;"),
         "the pipeline is cached before its result is checked")
    need(body.count("clear_pipes[key] =") == 1, "a pipeline is cached on more than one path")
    need("clear_failures.erase(key);" in body, "a key that worked keeps its count of failures")
    print("a masked clear that failed is not kept and is tried again, then backs off: ok")


if __name__ == "__main__":
    main()
