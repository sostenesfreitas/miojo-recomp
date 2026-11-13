# zbic

ZBIC is the NSO segment compression of Switch firmware 22.0.0+ (NSO flags bit 7, "UseZbicCompression"): zstd
frames with the magic `ZBIC` (0x4349425A) and entropy tables written with binary interpolative coding.

- `zstd.c`, `zstd.h`, `zstd_errors.h`: Atmosphere-NX's single-file zstd with `ZSTD_ZBIC_SUPPORT`, as copied into
  <https://github.com/kinnay/zbic> (`src/`, master, fetched 2026-11-04), unmodified. zstd is BSD-or-GPLv2;
  the ZBIC parts come from Atmosphere-NX (GPLv2). `LICENSE` is that repository's GPLv2 text.
- `zbic_api.c` (ours): two C functions for ctypes.

It is built into `zbic.dll` by `nx2/zbic.py` on first use, and used only to unpack the dump's modules
(`nx2/unpack.py`). It is a separate tool: none of it is linked into the game, except that the Vulkan back end
also builds `zstd.c` as `vk_zstd.o` (`nx2/build.py`, for the program cache's `RX_VK_PROGRAM_CACHE_ZSTD` and the
game's font files) with `ZSTD_ZBIC_SUPPORT` off: that object has only the stock zstd code (BSD), not the ZBIC
parts from Atmosphere-NX (GPLv2). `zbic_api.c` is not built there.
