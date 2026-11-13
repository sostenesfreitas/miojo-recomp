"""The sound, the window and the controllers load one SDL, the one beside the executable before the system's."""
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SDL = ROOT / "host" / "hos_sdl.c"
POSIX = ROOT / "host" / "posix" / "win32_posix.c"


class SdlAudioLibraryTests(unittest.TestCase):
    def test_the_bundled_sdl_first(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("static HMODULE load_one")
        fn = text[start:text.index("\n}", start)]
        self.assertLess(fn.index("GetModuleFileNameA"), fn.index("LoadLibraryA(dll)"))

    def test_sdl3_before_sdl2(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("int hos_sdl_load(void)")
        fn = text[start:text.index("\n}", start)]
        self.assertLess(fn.index('load_one("SDL3.dll")'), fn.index('load_one("SDL2.dll")'))

    def test_audio_takes_the_same_sdl(self):
        text = POSIX.read_text(encoding="utf-8")
        start = text.index("MMRESULT waveOutOpen")
        fn = text[start:text.index("MMRESULT waveOutPrepareHeader", start)]
        self.assertIn("hos_sdl_audio_open(", fn)
        self.assertNotIn("LoadLibraryA", fn)

    def test_audio_device_buffer_matches_audio_frame_quantum(self):
        text = SDL.read_text(encoding="utf-8")
        start = text.index("void *hos_sdl_audio_open(")
        fn = text[start:text.index("\n}", start)]
        self.assertIn('const char *env_samples = getenv("SDL_AUDIO_DEVICE_SAMPLE_FRAMES");', fn)
        self.assertIn('if (version == 3 && !env_samples) hos_sdl_hint("SDL_AUDIO_DEVICE_SAMPLE_FRAMES", "480");', fn)
        self.assertIn('want.samples = (uint16_t)(env_samples && atoi(env_samples) > 0 ? atoi(env_samples) : 512);', fn)


if __name__ == "__main__":
    raise SystemExit(unittest.main(verbosity=1))
