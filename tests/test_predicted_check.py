"""The start after a preparation checks that the driver kept the pipelines the prediction built for it.

predict_run (gpu/vk/vk_draw.cpp) builds the predicted pipelines into the driver's own disk cache alone and lets them go. On a driver
without such a cache, or with one too small, that work leaves nothing. predict_run now writes a sample of what it built
(hookshot_pipelines.bin.predicted, stamped with program_cache_stamp), and the next start asks the driver for each, without
compiling and with an empty pipeline cache of its own as play does, on a thread behind the game: "the driver's shader cache kept N
of M predicted pipelines". The process that built them does not ask (the driver would answer from memory). The driver cache
folders' size is logged at the start and after the prediction.

Source checks only. python -I -B tests/test_predicted_check.py
"""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[1]


def body(text, marker):
    if text.count(marker) != 1:
        raise SystemExit("FAIL: expected one %r" % marker)
    start = text.index(marker)
    at = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[at] == "{") - (text[at] == "}")
        at += 1
    return text[start:at]


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    main_cpp = (ROOT / "gpu/vk/vk_main.cpp").read_text(encoding="utf-8")
    predict = body(draw, "void predict_run(void (*progress)(size_t done, size_t total)) {")
    at = predict.find("predicted_sample_write(pre->predicted)")
    if at < 0 or "!prepare_stopped()" not in predict[at - 80:at] or "!gone_on" not in predict[at - 80:at]:
        raise SystemExit("FAIL: predict_run does not keep a sample of what it built (or keeps one when it was stopped or gave up)")
    write = body(draw, "void predicted_sample_write(const std::vector<std::unique_ptr<PipeNote>>& predicted) {")
    if "predicted_this_run = true" not in write or "program_cache_stamp()" not in write:
        raise SystemExit("FAIL: the sample is not stamped, or the building process would check itself")
    check = body(draw, "void predicted_check() {")
    for need, why in (("predicted_this_run", "the process that built them asks the driver, which answers from memory"),
                      ("if (!dev.vk13.pipelineCreationCacheControl) return;",
                       "the check asks under FAIL_ON_PIPELINE_COMPILE_REQUIRED on a device without pipelineCreationCacheControl"),
                      ('if (env_on("HOOKSHOT_PREPARE_ONLY")) return;',
                       "a --prepare-shaders run starts the check on a thread that would race its exit"),
                      ("head[0] == program_cache_stamp()", "a sample of another translator or cache format is asked"),
                      ("std::thread(", "the check holds up the start"),
                      ("THREAD_PRIORITY_IDLE", "the check competes with the game"),
                      ("vkCreatePipelineCache", "the check does not ask as play does (with a cache of its own)"),
                      ("driver_cache_only = true", "the check asks our pipeline cache, not the driver's"),
                      ("&cold)", "the check compiles what the driver does not have"),
                      ("program_cache_borrow(", "the check keeps the programs it reads")):
        if need not in check:
            raise SystemExit("FAIL: " + why)
    graphics = body(draw, "VkPipeline build_graphics(const Program& p, const NvnState& s, const Targets& at, VkPrimitiveTopology topo, int part, bool* cold) {")
    if "? pcache : driver_ask_cache" not in graphics:
        raise SystemExit("FAIL: build_graphics does not hand the driver the check's own cache")
    pre = body(draw, "void precompile_run(void (*progress)(size_t done, size_t total)) {")
    if "predicted_check();" not in pre:
        raise SystemExit("FAIL: the start does not check the predicted pipelines")
    started = body(main_cpp, "bool started_up() {")
    if 'driver_cache_log("at the start")' not in started or 'driver_cache_log("after the prediction")' not in predict:
        raise SystemExit("FAIL: the driver cache's size is not logged at the start and after the prediction")
    print("PASS: the next start asks the driver for a sample of the predicted pipelines")
    return 0


if __name__ == "__main__":
    sys.exit(main())
