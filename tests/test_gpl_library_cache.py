"""The stage libraries are kept in Hookshot's own pipeline cache, and a warm pass only counts once that cache is on disk
(gpu/vk/vk_draw.cpp build_graphics, warm_libraries; source checks, no game/GPU).

Asked for under the compile-required flag, the driver answers from a cache it is handed: a library the warm pass built into
nothing and let go was cold again at its first draw. So parts 1 and 2 are built into the cache, the cache exists before the
warm pass's workers start (the start that only warms had not made it), what is compiled counts towards a save, and the
warmed stamp is not written before the file holds the libraries.
python -I -B tests/test_gpl_library_cache.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def between(text, start, end):
    a = text.index(start)
    return text[a:text.index(end, a)]


def main():
    draw = (ROOT / "gpu/vk/vk_draw.cpp").read_text(encoding="utf-8")
    build = between(draw, "VkPrimitiveTopology topo, int part, bool* cold) {", "\n}\n")
    for line, why in (
            ("const bool lib_cached = (part == 1 || part == 2) && gpl_libs_cached();", "only the two shader libraries are kept"),
            # (driver_ask_cache: none, except predicted_check's empty scratch cache under driver_cache_only)
            ("const VkPipelineCache cache = lib_cached || (part < 0 && !driver_cache_only) ? pcache : driver_ask_cache;",
             "the libraries are not built into the pipeline cache"),
            # (graphics_with: pcache through the play caches, tests/test_pipeline_play_cache.py)
            ("VkResult r = graphics_with(cache, &ci, &pl);", "pipeline creation bypasses the chosen cache"),
            ("pcache_built_since_save += lib_cached && !cold;", "a compiled library does not count towards a save")):
        if line not in build:
            raise SystemExit(why)
    if "thread_local VkPipelineCache driver_ask_cache = VK_NULL_HANDLE;" not in draw:
        raise SystemExit("a part built outside the pipeline cache is handed a cache other than none by default")
    warm = between(draw, "bool warm_libraries(", "\n}\n")
    if "pcache_init();" not in warm or warm.index("pcache_init();") > warm.index("std::thread"):
        raise SystemExit("the warm pass's workers start before the pipeline cache is there")
    saved = warm.index("const bool saved = pcache_saved_now();")
    if "if (!saved) return false;" not in warm[saved:] or warm.index("return true;", saved) < warm.index("if (!saved) return false;"):
        raise SystemExit("a warm pass whose libraries are not on disk is taken for done")
    prep = between(draw, "bool prepare_run(", "\n}\n")
    for call in ("if (!warm_libraries(progress)) {",):
        at = 0
        while (at := prep.find(call, at)) >= 0:
            if prep.index("return false;", at) > prep.index("prepared_for.warmed = warmed;", at):
                raise SystemExit("the warmed stamp is written although the warm pass did not finish")
            at += 1
    if "kWarmRecipe = 1;" in draw or "gpl_libs_cached() ? 1u : 3u" not in between(draw, "uint64_t warmed_stamp(", "\n}\n"):
        raise SystemExit("a folder warmed before the libraries were kept would read as warmed")
    print("PASS: the stage libraries go into the pipeline cache, and the warm pass counts once they are saved")


if __name__ == "__main__":
    main()
