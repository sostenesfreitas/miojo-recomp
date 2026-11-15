"""The Insert menu's GPU device row (gpu/vk/gpu_choice.h and its use in vk_main.cpp's pick_device), no game or GPU
required.

python -I -B tests/test_gpu_choice.py [--cxx g++]
"""
import argparse
import subprocess
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def preset_exempt(menu, value):
    """the menu's row for opts.<value> leaves the quality preset alone (Row::preset_exempt, which set_row checks)"""
    start = menu.index("&opts.%s," % value, menu.index("Row rows[] = {"))
    row = menu[start:menu.index("},", start)]
    return row.rstrip().endswith(", true") and "if (!r.preset_exempt)" in menu


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cxx", default="g++")
    args = parser.parse_args()
    vk = ROOT / "gpu" / "vk"
    main_src = (vk / "vk_main.cpp").read_text(encoding="utf-8")
    pick = main_src[main_src.index("bool pick_device() {"):]
    pick = pick[:pick.index("\n}\n")]
    for need, why in (("menu_init()", "the saved choice is read before it is used"),
                      ("menu_set_gpu_count((int)valid_gpus.size())", "the row offers only the usable GPUs"),
                      ("p.apiVersion >= VK_API_VERSION_1_3", "only Vulkan 1.3 GPUs are listed"),
                      ('getenv("RX_VK_DEVICE")', "RX_VK_DEVICE wins over the row"),
                      ("gpu_choice::menu_device(opts.gpu_device, (int)valid_gpus.size())", "the row's choice is taken")):
        if need not in pick:
            raise SystemExit("pick_device: %s is missing (%s)" % (need, why))
    if pick.index("menu_init()") > pick.index("gpu_choice::menu_device"):
        raise SystemExit("pick_device reads the GPU row before menu_init")
    if pick.index('getenv("RX_VK_DEVICE")') > pick.index("gpu_choice::menu_device"):
        raise SystemExit("pick_device takes the GPU row before RX_VK_DEVICE")
    menu = (vk / "vk_menu.cpp").read_text(encoding="utf-8")
    if not preset_exempt(menu, "gpu_device"):
        raise SystemExit("vk_menu.cpp: changing the GPU row resets the quality preset (and its VRAM budget) to Custom")
    # (the row saved as the card itself, gpu_device_id=, and found where it is in this run's list: gpu_choice.h)
    if "gpu_device_cards.push_back({p.vendorID, p.deviceID})" not in pick or \
            pick.index("gpu_device_cards.push_back") > pick.index("menu_set_gpu_count("):
        raise SystemExit("pick_device: the usable GPUs' models (gpu_device_cards) are not listed before the row is counted")
    count = menu[menu.index("void menu_set_gpu_count(int count) {"):]
    count = count[:count.index("\n}\n")]
    if "gpu_choice::menu_of_card(gpu_device_cards, gpu_saved_card)" not in count or \
            count.index("menu_of_card") > count.index("r.hi = count") or count.index("menu_of_card") > count.index("restart_value"):
        raise SystemExit("menu_set_gpu_count: the saved card is not found in the list before the row and its restart value are set")
    save = menu[menu.index("void save() {"):]
    save = save[:save.index("\n}\n")]
    if 'key == "gpu_device_id"' not in save or "gpu_choice::card_id(gpu_device_cards, opts.gpu_device - 1)" not in save or \
            'fprintf(f, "gpu_device_id=%s\\n"' not in save or "gpu_card_missing ? gpu_saved_card" not in save:
        raise SystemExit("save: the GPU row's card is not written (gpu_device_id=), its old line kept, or a missing card dropped")
    if 'if (!strcmp(line, "gpu_device_id")) gpu_saved_card' not in menu:
        raise SystemExit("menu_init: gpu_device_id= is not read")
    with tempfile.TemporaryDirectory(prefix="gpu-choice-") as tmp:
        exe = Path(tmp) / "gpu-choice"
        subprocess.run([args.cxx, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                        str(ROOT / "tests" / "test_gpu_choice.cpp"), "-o", str(exe)], check=True)
        subprocess.run([str(exe)], check=True)


if __name__ == "__main__":
    main()
