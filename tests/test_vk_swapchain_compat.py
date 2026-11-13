"""Linux Vulkan startup uses only surface-supported swapchain settings (four images, or the surface's maximum when lower).

Run with: python -I -B tests/test_vk_swapchain_compat.py
"""
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "gpu" / "vk" / "vk_main.cpp"


def function(text, marker):
    start = text.index(marker)
    brace = text.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def check_swapchain(source):
    body = function(source, "bool create_swapchain(")
    required = (
        "vkGetPhysicalDeviceSurfaceCapabilitiesKHR",
        "if (result != VK_SUCCESS)",
        "ci.minImageCount = std::max(caps.minImageCount, 4u)",
        "ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount)",
        "ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT",
        "caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
        "ci.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT",
        "caps.supportedCompositeAlpha & ci.compositeAlpha",
        "VK_PRESENT_MODE_FIFO_KHR",
        "vkCreateSwapchainKHR failed",
    )
    for item in required:
        if item not in body:
            raise SystemExit("create_swapchain no longer handles %s" % item)
    floor = body.index("ci.minImageCount = std::max(caps.minImageCount, 4u)")
    cap = body.index("ci.minImageCount = std::min(ci.minImageCount, caps.maxImageCount)")
    if cap < floor or "if (caps.maxImageCount)" not in body[floor:cap]:
        raise SystemExit("create_swapchain's four-image floor is not capped by the surface's maximum")


def check_linux_start(source):
    body = function(source, "int start(void* hwnd)")
    linux = body[body.index("#else"):body.index("#endif")]
    factory = function(source, "bool open_window_surface(bool log_failure) {")
    for item in ("w->create_surface", "queue-family present support failed"):
        if item not in factory:
            raise SystemExit("Vulkan surface creation no longer checks %s" % item)
    for diagnostic in ("SDL_Vulkan_CreateSurface failed", "queue-family present support failed"):
        before = factory[:factory.index('logf("vk: ' + diagnostic)].rstrip()
        if not before.endswith("if (log_failure)"):
            raise SystemExit("Surface recovery repeats the startup diagnostic: %s" % diagnostic)
    swapchain = function(source, "bool create_swapchain(")
    if "open_window_surface(false)" not in swapchain:
        raise SystemExit("Swapchain recovery must suppress repeated surface startup diagnostics")
    for item in (
        "open_window_surface(true)",
        "w->drawable_size",
        "SDL returned zero drawable size",
        "swapchain creation failed for drawable",
    ):
        if item not in linux:
            raise SystemExit("Linux Vulkan startup no longer checks %s" % item)


def main():
    source = SOURCE.read_text(encoding="utf-8")
    check_swapchain(source)
    check_linux_start(source)
    print("PASS: Vulkan swapchain respects surface limits and Linux startup reports failures")


if __name__ == "__main__":
    main()
