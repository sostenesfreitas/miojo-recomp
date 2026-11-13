"""A headless run ends on SIGTERM.

SDL turns SIGINT/SIGTERM into its quit event unless told not to, and only the window's event loop (display_sdl.c)
reads that event. With HOS_HEADLESS=1 there is no window, but the controllers still start SDL (pad_sdl.c): a
`timeout`'s or a service manager's SIGTERM was swallowed and the run went on until SIGKILL. pad_sdl.c sets
SDL_NO_SIGNAL_HANDLERS before SDL starts when there is no window.
python -I -B tests/test_headless_sigterm.py
"""
import re
from pathlib import Path

PAD = Path(__file__).resolve().parents[1] / "host" / "pad_sdl.c"


def main():
    text = PAD.read_text(encoding="utf-8")
    m = re.search(r"static int pads_start\(void\) \{(.*?)\n\}", text, re.S)
    if not m:
        raise SystemExit("host/pad_sdl.c: no pads_start")
    body = m.group(1)
    # (sdl.SetHint/sdl.Init, or since PR 328 the SDL2/SDL3 wrappers hos_sdl_hint/hos_sdl_init)
    hint = max(body.find('sdl.SetHint("SDL_NO_SIGNAL_HANDLERS", "1")'), body.find('hos_sdl_hint("SDL_NO_SIGNAL_HANDLERS", "1")'))
    init = max(body.find("sdl.Init("), body.find("hos_sdl_init("))
    if hint < 0:
        raise SystemExit("pads_start does not turn off SDL's signal handlers: a headless run ignores SIGTERM")
    if init < 0 or hint > init:
        raise SystemExit("pads_start sets SDL_NO_SIGNAL_HANDLERS after SDL starts: SDL reads it when it starts")
    line = body[body.rfind("\n", 0, hint):hint]
    if 'env_on("HOS_HEADLESS")' not in line:
        raise SystemExit("SDL_NO_SIGNAL_HANDLERS is not limited to HOS_HEADLESS: a window's close on SIGTERM needs them")
    print("PASS: a headless run leaves SIGINT/SIGTERM to end it")


if __name__ == "__main__":
    main()
