"""The presenter thread (RX_VK_PRESENT_THREAD=1; off by default) keeps the presents in order, one out at most, and off the
swapchain's remaking.

present_out hands a present to the presenter thread only from present_image (it alone sets the copy's fence; frame generation's
present_commit paces its presents itself) and with NVIDIA Reflex off (its markers belong to the game's thread). The presenter waits
for the copy's fence before it presents, on a queue of its own (never the replay's lock), under FIFO until the present is shown
(VK_KHR_present_wait), and never without end. The device makes that queue only when it has a second one that can present; otherwise
the presents stay on the game's thread. The game's thread waits
while the frame before's present is still out; a present made directly waits for the one handed over with the queue's lock let go;
the swapchain is made again only with the presenter idle, and the wait for that comes before the queue's lock (the presenter takes
it). The presenter never makes a swapchain itself: an out-of-date or suboptimal present asks the next present_image for one.

python -I -B tests/test_present_thread.py
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def block(text, marker):
    start = text.index(marker)
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
    vk = ROOT / "gpu" / "vk"
    replay = (vk / "vk_replay.cpp").read_text(encoding="utf-8")
    main_cpp = (vk / "vk_main.cpp").read_text(encoding="utf-8")

    out = block(replay, "static bool present_out(uint32_t index, std::unique_lock<std::mutex>& qk) {")
    hand = out[: out.index("present_hand_off(")]
    need("present_copy_done && !pi.pNext && present_thread_on()" in hand,
         "a present is handed over from frame generation or with Reflex on")
    need("qk.unlock();" in hand, "the queue's lock is held while the game's thread hands a present over")
    rest = out[out.index("present_hand_off("):]
    need("qk.unlock();\n        present_thread_drain();\n        qk.lock();" in rest and
         rest.index("present_thread_drain();") < rest.index("vkQueuePresentKHR"),
         "a present made directly may go out before the one handed over, or waits for it with the queue's lock held")

    image = block(replay, "void present_image(")
    need("present_copy_done = f.done;\n    present_out(index, qk);\n    present_copy_done = VK_NULL_HANDLE;" in image,
         "present_image does not let its present (and only it) be handed over")
    for site in ("reflex_swapchain_stale() || hdr_changed || vsync_changed) {", "if (res == VK_ERROR_OUT_OF_DATE_KHR) {"):
        at = image[image.index(site):]
        need(at.index("present_thread_drain();") < at.index("std::lock_guard qk(dev.present_mu);"),
             "present_image remakes the swapchain without first waiting for the presenter, outside the queue's lock")
    commit = block(replay, "bool present_commit(")
    need("present_out(p.index, qk);" in commit and "present_copy_done" not in commit,
         "frame generation's paced presents are handed over")

    give = block(replay, "void present_hand_off(uint32_t index, VkFence done) {")
    idle = block(replay, "bool presenter_wait_idle(std::unique_lock<std::mutex>& lk, int limit_ms = 0) {")
    need("presenter.cv.wait(lk, idle);" in idle, "presenter_wait_idle does not wait for the presenter")
    need(give.index("presenter_wait_idle(lk);") < give.index("presenter.busy = true"),
         "a present is handed over before the one before is made")
    need("presenter_wait_idle(lk, 5000)" in block(replay, "void present_thread_drain() {"), "present_thread_drain does not wait, or without a limit")

    loop = block(replay, "void presenter_loop() {")
    need(loop.index("presenter_wait_copy(done)") < loop.index("vkQueuePresentKHR"),
         "the presenter presents before the copy is done (the driver's wait comes back)")
    copy = block(replay, "bool presenter_wait_copy(VkFence done) {")
    need("UINT64_MAX" not in copy and "100000000ull" in copy and "VK_ERROR_DEVICE_LOST" in copy,
         "the presenter's wait for the copy has no limit, or misses a device lost")
    need("vkQueuePresentKHR(dev.presenter_queue, &pi)" in loop and "std::lock_guard pq(dev.presenter_queue_mu);" in loop,
         "the presenter does not present on its own queue, under its own lock")
    need("dev.present_mu" not in loop and "dev.queue," not in loop, "the presenter takes the replay's queue or its lock")
    need("wait_for_present(dev.device, sc, id, 20000000ull)" in loop and "dev.swap_mode == VK_PRESENT_MODE_FIFO_KHR" in loop,
         "under FIFO the presenter does not wait (with a limit) for its present to be shown")
    need("UINT64_MAX" not in loop, "a wait without end on the presenter")
    wait_idle = block(main_cpp, "") if False else replay[replay.index("void device_wait_idle_locked() {"):]
    wait_idle = wait_idle[:wait_idle.index("\n}") + 2]
    need("std::lock_guard pr(dev.presenter_queue_mu);" in wait_idle, "vkDeviceWaitIdle runs with the presenter's queue in use")
    need("presenter.busy = false" in loop and "notify_all" in loop, "the presenter does not say it is done")
    need("create_swapchain" not in loop and "reflex_presented" not in loop,
         "the presenter makes a swapchain itself, or sets Reflex's present marker")
    finish = block(replay, "void present_finish(VkResult res, bool on_presenter) {")
    need("on_presenter && (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR)" in finish and "swap_resize" in finish,
         "an out-of-date present on the presenter does not ask for a new swapchain")

    need("on_presenter && res == VK_ERROR_SURFACE_LOST_KHR" in finish and "presenter_surface_lost.store(true" in finish,
         "a surface lost at the presenter's present is not handed to the next present_image")
    lost = image[image.index("if (presenter_surface_lost.exchange(false"):]
    need(lost.index("present_thread_drain();") < lost.index("std::lock_guard qk(dev.present_mu);") <
         lost.index("invalidate_lost_surface(VK_ERROR_SURFACE_LOST_KHR);") < lost.index("return;"),
         "a surface the presenter found lost is dropped without the presenter idle first, or outside the queue's lock")
    acq = image[image.index("    if (res == VK_ERROR_SURFACE_LOST_KHR) {"):]
    need(acq.index("present_thread_drain();") < acq.index("invalidate_lost_surface(res);"),
         "the acquire's lost surface is dropped with a present still out on the presenter")
    make = block(main_cpp, "bool create_swapchain(uint32_t width, uint32_t height) {")
    need(make[make.index("{") + 1:].lstrip().startswith("present_thread_drain();"),
         "create_swapchain does not wait for the presenter first")
    on = block(replay, "bool present_thread_on() {")
    need('getenv("RX_VK_PRESENT_THREAD")' in on and "return e && atoi(e) != 0;" in on, "the switch is not RX_VK_PRESENT_THREAD, off by default")
    need("const bool presenter_wanted = pt_env && atoi(pt_env) != 0;" in block(main_cpp, "bool create_device() {"),
         "the presenter's queue is made while the presenter thread is off")
    need("dev.presenter_queue && dev.presenter_supported" in on, "the presenter thread runs without a queue of its own that presents")
    dev_make = block(main_cpp, "bool create_device() {")
    swap = block(main_cpp, "bool create_swapchain(uint32_t width, uint32_t height) {")
    need("if (dev.presenter_supported) share(dev.presenter_family);" in swap and "VK_SHARING_MODE_CONCURRENT" in swap,
         "a presenter queue of another family presents images it does not share")
    choose = block(main_cpp, "static void choose_present_queue() {")
    need("vkGetPhysicalDeviceSurfaceSupportKHR(dev.physical, dev.presenter_family" in choose,
         "the presenter's family is not checked against the window's surface")
    # (the queue choice on NVIDIA's, RADV's and smaller layouts: tests/presenter_queue_test.cpp)
    import subprocess, tempfile, os
    need("presenter_queue_pick(" in dev_make, "create_device does not choose the presenter's queue with presenter_queue_pick")
    with tempfile.TemporaryDirectory() as tmp:
        exe = os.path.join(tmp, "presenter_queue_test")
        subprocess.run([os.environ.get("CXX", "c++"), "-std=c++17", "-Wall", "-Wextra", "-Werror", str(ROOT / "tests" / "presenter_queue_test.cpp"),
                        "-o", exe], check=True)
        out = subprocess.run([exe], capture_output=True, text=True)
        print(out.stdout.strip())
        need(out.returncode == 0, "the presenter's queue choice fails a family layout")
    print("PASS: the presenter thread waits for the copy, keeps the presents in order and off the swapchain's remaking")


if __name__ == "__main__":
    main()
