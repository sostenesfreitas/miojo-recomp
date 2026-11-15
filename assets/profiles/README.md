# Play profiles

Profiles the build compiles with, committed so every build of a checkout uses the same ones: the release's builders,
a player's own build (with DLSS or FSR 4 too), and a developer's. Names of Hookshot's own functions, a hash of each
one's shape and how often its branches ran: no game code or data.

## hookshot_vk.profdata.xz: the renderer's

The Vulkan back end's play profile (nx2/build.py `RX_VK_PGO`; docs/development.md, "The renderer's play profile"),
compressed with xz. `nx2/make_game.py` unpacks it into the work folder and builds the renderer with it, unless the
checkout's `work/hookshot_vk.profdata` or `RX_VK_PGO` says otherwise; the Linux AppImage carries it for the player's
build, which compiles the renderer (the AppImage has no built one). A compiler that can't read it builds the renderer
without it.

One profile for every build for now. It ages rather than breaks: a function changed since the recording is compiled as
if there were no profile, the rest keep theirs. Record it again before a release, and when the renderer has changed much.

Three recordings of an `RX_VK_PGO=gen` renderer, merged with `llvm-profdata merge` (LLVM 23.1.5); 1,744 of the
renderer's functions ran in at least one of them:

1. 2026-11-14, main 0caf0618, NVIDIA GeForce RTX 3090 (mesh shaders), Linux, Ryzen 9 7900X: one session, counters
   cleared once Kokiri Forest had loaded and written at the end of the Market walk (Kokiri Forest standing, the
   teleport, the Market walk). 1,660 functions ran.
2. 2026-11-14, main 5d3ecc78, AMD Radeon RX 580 (RADV POLARIS10, no mesh shaders: the compute fallback), Linux,
   i7-9700K: child Link from the opening through the debug teleport's overworld destinations, each loaded, a few
   seconds standing, the camera turned, a short run (532 s; Hyrule Castle and Gerudo Fortress, adult only, did not
   teleport). 1,573 functions ran.
3. The same PC and build: the dungeons and Desert Colossus the same way (265 s; the Spirit Temple did not teleport).
   1,581 functions ran.

Recordings 2 and 3 add 76 functions recording 1 never ran, among them the compute fallback's draw path
(`fallback_record`, `fallback_buffers`, `fallback_task_groups`, `fallback_pipelines_ready` in vk_draw.cpp).

- Compiler: the Linux builder's clang (LLVM 23.1.5), so C++ standard library names are libstdc++'s: Windows builds
  (llvm-mingw, libc++) match the renderer's own functions and leave the library's out
- Not yet: a Windows recording, adult Link's areas, the pipeline tour's full route
- Measured with recording 1 alone (the 3090, from the Market save, 3 sessions each, median frame): standing 13.53 /
  13.49 / 13.70 ms without, 12.96 / 13.37 / 12.94 ms with; running in the square 15.89 / 16.13 / 15.73 ms without,
  15.13 / 15.67 / 15.52 ms with: about -4 to -5 % standing and -3 % running. The merged profile is not measured yet.
