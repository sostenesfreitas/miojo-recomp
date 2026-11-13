"""The FPS counter's levels (gpu/vk/fps_stats.h, vk_fps.cpp, vk_menu.cpp): Off, FPS, Frame time, Graph and Breakdown on one
row, an options file's old fps_counter=1 (On) read as FPS; the ring of frames, the averages, the 1% low, the graph's columns
and scale (tests/test_fps_stats.cpp); and that the renderer and the host feed them: a frame at each present, the renderer's
entries and waits timed only while the Breakdown shows, the host's sleeps of the presenting thread. No game or GPU.

python -I -B tests/test_fps_stats.py [--cxx g++]
"""
import argparse
import re
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def body(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def need(cond, what):
    if not cond:
        raise SystemExit("FAIL: " + what)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--cxx", default="g++")
    a = p.parse_args()
    vk = ROOT / "gpu" / "vk"
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    row = re.search(r'\{TAB_GAMEPLAY, "FPS counter", &opts\.show_fps, 0, ([^,]+), 1, (\w+),', menu)
    need(row, "the Gameplay tab has no FPS counter row on opts.show_fps")
    need(row.group(1).strip() == "fps_stats::kLevels - 1" and row.group(2) == "tx_fps_level",
         "the FPS counter row is not the levels' (0 to fps_stats::kLevels - 1, tx_fps_level)")
    need("fps_stats::level_name(" in body(menu, "const char* tx_fps_level("), "tx_fps_level does not use fps_stats::level_name")
    # (the key the row saves under is its name's: fps_counter, as before the levels, so an old file's 1 is FPS)
    need("show_fps" not in body(menu, "const char* ini_key("), "the FPS counter's key was renamed: old options files would not load")
    hints = body(menu, "MENU_HINTS(hn_fps,")
    need(hints.count('"') // 2 >= 4, "the FPS counter's hints do not cover every level")
    need(re.search(r'\{TAB_GAMEPLAY, "FPS counter position", &opts\.fps_corner, 0, 3, 1, tx_fps_corner,', menu),
         "the Gameplay tab has no FPS counter position row")
    tick = menu[menu.index("void menu_tick()"):menu.index("void menu_record(")]
    need("fps_level_now.store(" in tick and "fps_texts(" in tick, "menu_tick does not tell vk_fps.cpp the level or write the lines")
    need("fps_level_now.store(" in body(menu, "void menu_init()"), "menu_init does not tell vk_fps.cpp the saved level")
    target = body(menu, "double fps_target_ms()")
    need("fps_stats::cap_rate(fps_cap_started" in target and "HOS_FPS_MAX" in target and "menu_vsync() == 1" in target,
         "the target is not the cap the game started with (HOS_FPS_MAX over it), or the display's with V-Sync On")
    draw = body(menu, "void draw_fps(")
    for use in ("fps_graph(", "fps_target_ms()", "fs::graph_top(", "fs::rate_grade(", "fps_part_text", "fps_ms_text"):
        need(use in draw, "draw_fps does not use %s" % use)

    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    present = body(replay, "void present(const nvn_tex* t, const int crop[4])")
    need("fps_presented();" in present.split("\n")[1], "present does not stamp the frame first, on the presenting thread")
    need("FpsRender" in body(replay, "void submit_q("), "the submits' replay is not timed as the renderer's")
    need("st_wait_ns +=" in body(replay, "Recording& begin_recording(") and "slot_wait_ns()" in body(
        (vk / "vk_fps.cpp").read_text(encoding="utf-8"), "void fps_presented()"), "the recording slot's wait is not a GPU wait")
    need("fps_wait(fps_stats::kPace" in body(replay, "void present_image("), "the swapchain's acquire is not the pacing's")
    # (a present that returned only once the frame's copy, not done when it was called, was done waited for the GPU, not vsync:
    # NVIDIA on Linux; read with two fence reads, nothing waited for)
    out = body(replay, "static bool present_out(uint32_t index, std::unique_lock<std::mutex>& qk) {")
    need("copy_was_done" in out and "waited_gpu ? fps_stats::kGpuWait : fps_stats::kPace" in out,
         "the present's wait is not split into the GPU's (the copy not done before, done after) and the pacing's")
    need("(::vkx::vkWaitForFences)(dev.device, 1, &copy, VK_TRUE, 0)" in out and out.count("vkWaitForFences") == 1,
         "present_out waits for the copy to time it (only a wait of no time, its state): the Breakdown would change the present")
    need("presenter.copy_done_at" in body(replay, "bool presenter_wait_idle(") and "presenter_wait_idle(lk)" in body(
        replay, "void present_hand_off(uint32_t index, VkFence done) {"), "the wait for the presenter thread is not split at the end of its wait for the copy")
    fps = (vk / "vk_fps.cpp").read_text(encoding="utf-8")
    # (off and FPS: nothing measured; the Breakdown's timers only while it shows)
    pres = body(fps, "void fps_presented()")
    need(pres.index("fs::kFrameTime") < pres.index("fps_clock()"), "fps_presented reads the clock below Frame time")
    need("fps_breakdown_on()" in body(fps, "FpsRender::FpsRender()"), "the renderer's timer runs while the Breakdown is off")
    need("fps_breakdown_on()" in (vk / "vk_fps.h").read_text(encoding="utf-8"), "FpsWait is not gated on the Breakdown")
    need("host->slept_ns" in pres, "the host's sleeps (pacing, OoT's present poll) are not read")

    api = (vk / "vk_api.h").read_text(encoding="utf-8")
    ver = int(re.search(r"#define MKWVK_API_VERSION (\d+)", api).group(1))
    need(ver >= 15 and "slept_ns" in api, "MkwVkHost has no slept_ns (API version 15)")
    backend = (ROOT / "host" / "rx_backend.c").read_text(encoding="utf-8")
    need(re.search(r"host_wide_menus,\s*host_slept_ns,\s*host_fps_timing", backend), "host/rx_backend.c does not give the back end its sleeps")
    need("host->fps_timing(" in pres, "the host is not told when to time its sleeps")
    threads = (ROOT / "host" / "hos_thread.c").read_text(encoding="utf-8")
    need("0x80590520" in body(threads, "static void slept_since(") and "slept_since(" in body(threads, "HLE(_ZN2nn2os11SleepThread")
         and "hos_poll_slept_ns" in backend and "hos_acquire_slept_ns" in backend,
         "the guest's sleeps are not counted, OoT's present poll apart")
    # (off: the host reads no clock for them)
    need("hos_fps_timing ? hos_now_ns() : 0" in body(threads, "HLE(_ZN2nn2os11SleepThread")
         and "if (hos_fps_timing)" in body((ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8"), "void hos_vi_native_acquire(")
         and "hos_fps_timing ? hos_now_ns() : 0" in (ROOT / "host" / "hos_svc.c").read_text(encoding="utf-8"),
         "the host times its sleeps while the Breakdown is off")
    need("hos_acquire_slept_ns" in body((ROOT / "host" / "nn_vi.c").read_text(encoding="utf-8"), "void hos_vi_native_acquire("),
         "the window acquire's pacing sleep is not counted")

    with tempfile.TemporaryDirectory(prefix="fps-stats-") as tmp:
        exe = Path(tmp) / "fps-stats"
        subprocess.run([a.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "test_fps_stats.cpp"), "-o",
                        str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print("PASS: the FPS counter's levels, its numbers and what feeds them")


if __name__ == "__main__":
    main()
