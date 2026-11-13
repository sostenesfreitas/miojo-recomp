# Hookshot (oot-recomp): design

A native x86-64 Windows and Linux build of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake: the game's AArch64
code translated to C ahead of time from the user's own decrypted dump and compiled with clang, the Horizon OS and
NVN2 implemented natively, and the GPU work replayed on Vulkan. No CPU or GPU emulation at any point: the same
model as mkw-recomp (Mario Kart World), which this project forks.

This document is the plan. The numbers in it that depend on the binaries are filled in from the dump (section 9);
until it exists they are marked *dump*.

## 1. Short answer

**The game is a Switch 2 title, so the base is mkw-recomp, not TOTKPort.** Public reports before its release
(2026-11-05) describe a Switch 2 exclusive, by Nintendo, 1080p to 1440p docked and 720p handheld, with the ocarina
playable by humming into the console's microphone. A Switch 2 executable means:

| | Switch 2 (this game, expected) | Switch 1 (TOTK, TOTKPort's base) |
|---|---|---|
| CPU code | ARMv8.2+ (Cortex-A78C): PAC returns, LSE atomics, CAS, LDAPR | ARMv8.0 (Cortex-A57) |
| Relocations | DT_RELR packed + RELA | RELA |
| GPU API | NVN2 (API 231.x): native images, DLSS, mesh/task programs | NVN 55.x |
| Shaders | Ampere SASS (SM86) in `Nin_Ounce_NVN2` packages | Maxwell SASS in `Nin_NX_NVN` packages |
| Services | nx2 gs/gpu/vi2 sessions, no nvdrv in the npdm | nvdrv, vi:u |

mkw-recomp already handles every row of the left column, from the lifter (`nx2/insn.py`, `nx2/nso2.py`) to the
Vulkan back end with Eden's SM86 recompiler (`gpu/vk`), and its standalone runtime plays MKW races at 45-55 fps.
TOTKPort's NVN1 layer and Maxwell path would have to be rebuilt for Switch 2 first.

`nx2/dump.py` checks this on the real dump: six independent signals (program id prefix, ACID layout, nvdrv in the
services, DT_RELR, ARMv8.1+ instructions, NVN2 proc names) must all say Switch 2. If they say Switch 1, section 3
changes: TOTKPort's `hos/nvn_native.c` (NVN 55.15) would sit under this repository's Vulkan back end instead.

## 2. What is reused (checked 2026-11-04)

| From | What | Where here |
|---|---|---|
| **mkw-recomp** `7336f0a` (`..\mkw-recomp`, local) | lift driver and its fixes, Switch 2 forms, DT_RELR, Eden's image layout, 64-bit guest window, loop-header polls, stable chunks, Unicorn tests, build; the standalone runtime; the native NVN2 layer; the Vulkan back end | `nx2/`, `host/`, `gpu/`, `third_party/` (commit `86871a1`, byte-identical) |
| **a64-kit** (Rexcomp's AArch64 kit, TOTKPort `60de030`, via mkw-recomp) | NSO loader, image builder, lifter, suyu's translator, instruction harness | `third_party/a64-kit` (unmodified) |
| **Eden's shader recompiler** (Eglantine build 181, via mkw-recomp) | SM86 -> IR -> SPIR-V, task/mesh programs | `gpu/vk/eden` |
| **TOTKPort** (`Desktop\Ports\TOTK\TOTKPort`) | the original HLE runtime and NVN layer that mkw-recomp's are forks of | reference only |
| **Eglantine** (`..\Eglantine`, read and run only) | Switch 2 service protocols, the reference renderer, an A/B oracle | reference only; its CPM cache supplies the Vulkan/SPIR-V headers, VMA, FFmpeg |

Keeping the fork's paths identical to mkw-recomp's is deliberate: MKW is the reference port and gets fixes every
day (the queue/event ordering and pool captures of 2026-11-04 among them). A later mkw-recomp commit comes over with

```
git -C ../mkw-recomp diff 7336f0a <new> -- nx2 host gpu third_party > mkw.patch
git apply -3 mkw.patch
```

which works because the base blobs are in this repository. Each sync names its new mkw-recomp base in its commit
message. OoT-specific code goes into new files where it can, so the shared files stay easy to patch.

## 3. Architecture

```
 exefs/{rtld,main,subsdk*,sdk} + main.npdm      (the user's dump, read in place: work/dump.txt)
        │  a64-kit tools/nso.py + nx2/nso2.py (DT_RELR)
        ▼
 modules: LZ4 segments, MOD0, dynamic section, symbols, RELA + RELR, .eh_frame_hdr starts
        │  nx2/image.py (Eden's layout from 0x80000000: rtld, main, subsdk*, sdk; we are rtld)
        ▼
 image: relocated, imports bound across modules; the HLE set hooked (nx2/standalone.py)
        │  a64-kit lift/a64_lift.py + nx2/insn.py + nx2/lift_fixes.py + nx2/poll.py + nx2/chunks.py
        ▼
 one C function per guest function (work/sa/gen, ~1 GB, local), 64-bit guest window (nx2/rtgen.py)
        │  clang (llvm-mingw, emsdk or the system's) -O1, the hot functions -O2 with the play profile, lld (nx2/build.py --standalone)
        ▼
 hookshot.exe = lifted game + host/ (Horizon: threads, SVCs, memory, fs, hid, audio, vi, gs, NVN2)
 hookshot_vk.dll = the Vulkan back end (gpu/vk), loaded by host/rx_backend.c unless RX_BACKEND names another renderer
```

- **Standalone from the start.** MKW went emulator-hosted first (Eglantine with a recompiled CPU backend), then
  standalone. That detour is not needed any more: the standalone runtime exists and is the user's rule anyway
  ("100% native"). Eglantine stays the reference to compare against, never part of the process.
- **The SDK is replaced at the nn:: boundary** (TOTKPort's model): `HLE(<mangled name>)` functions in `host/` stand
  in for the SDK's OS-facing functions; the rest of the SDK (libc, nn::util, nn::mem, the NVN2 driver's front end)
  runs as lifted code. Unimplemented OS-facing imports get generated stubs that log their first call.
- **Memory.** A 512 GB reservation; the image at Eden's `0x80000000` (the lift's addresses), the heap from 64 GB,
  address regions growing down from 512 GB; committed as handed out or on first touch (`host/hos.h`).
- **GPU.** `host/nvn_native.c` implements the NVN2 procs over guest memory in place: objects live where the game
  puts them, command buffers record an op stream (`gpu/render/rx_nvn.h`), queue submits hand it to the back end.
  `gpu/vk` replays it on Vulkan 1.3: pools imported as host memory, SM86 programs through Eden's recompiler to
  SPIR-V, mesh/task programs with `VK_EXT_mesh_shader`. The WebGPU renderer (`gpu/render`) stays in the Windows exe as the
  fallback when Vulkan is missing, as in mkw-recomp, when the build has wgpu-native (`WGPU_NATIVE`).

### Graphics pipeline state

The mesh compute fallback draws its generated records through a vertex shader without vertex buffers.
Its graphics pipelines keep dynamic primitive topology and primitive restart, but do not require a
dynamic vertex binding stride (#2494, tracked in #1001). Ordinary vertex-input pipelines still use
dynamic stride; native mesh pipelines do not use these three input-assembly/vertex-input states.
Binding the fallback's static-stride pipeline invalidates dynamic strides. The draw path drops both
vertex-bind skip caches at that bind, so the next ordinary draw sets its strides again even if the
buffers are unchanged. Resolved buffer references remain cached; buffer handles, offsets and sizes
are not invalidated by pipeline binding. Runtime fallback programs do not use pipeline libraries.

## 4. The lessons from MKW, as rules here

Each of these cost days in mkw-recomp; the fork has the fixes, and these are the checks that keep them true for a
second game.

1. **Cross-queue `nvnCommandBufferWaitEvent` is honoured.** MKW's queues pass stages back and forth with events;
   replaying them in arrival order on one Vulkan queue flashed. `gpu/vk/vk_replay.cpp` `submit_job` replays each
   submission as far as its waits allow, with per-queue state (`RX_VK_EVENT_WAITS=0` turns it off, for A/B only).
   *Check early:* log OoT's queues and event waits on the first boot (which queues, how many waits per frame).
2. **Nothing the replay reads comes from live guest memory.** Games write per-frame data ahead of the GPU: every
   read is from submit-time captures, and pools are captured from their creation, not their first GPU use
   (`gpu/vk/vk_memory.cpp`, `RX_VK_CAPTURE_POOL_MB`). *Check:* the mirror sync log's live-sync count stays ~0.
3. **Texture and sampler registrations are queued in order with the submits**, never applied out of band.
4. **GPU timestamps drive dynamic resolution.** OoT is reported at 1080p-1440p docked, so it has a resolution
   controller fed by timestamp reports: the fake GPU time (the counter reports the replay writes, `gpu/vk/vk_replay.cpp`
   `NOP_REPORT_COUNTER`: the 31.25 MHz tick clock since the frame's present, times `RX_VK_TS_SCALE`, 0.1 by default) must be
   scaled so the controller settles where the PC is, not at its floor. *Check:* the render size it picks over time.
5. **Clears after draws, and draws that sample a target their pass already wrote (or write one an earlier draw of the
   pass sampled), get passes of their own** (`vk_draw.cpp` `clear_after_draws`, `draw_hazards`). A draw that samples
   a target it writes itself stays in the pass.
6. **2D views of 3D textures need `VK_EXT_image_2d_view_of_3d`** (the RTX 4060 Ti has it).
7. **The CPU saturates first.** Ryzen 5 7600X, 6 cores / 12 threads: the game's threads, the replay, captures and
   pipeline compiles share it, and extra helper threads have lowered fps before. Thread counts are measured, never
   raised by default (`HOS_VCORES`, `RX_VK_CAPTURE_THREADS`, `HOS_THREAD_TIMES=<seconds>`, the `HOS_SAMPLE` profiler
   with `scripts/prof.py`).

Also inherited, and game-specific in MKW: MKW's frame-pacer hooks at fixed guest addresses are gated on MKW's build
id (`hos_game_is_mkw`); OoT gets its own once its pacer is found, never by address coincidence.

## 5. What OoT needs that MKW did not (checked on the dump, 2026-11-04)

| | Found | Plan |
|---|---|---|
| Dump format | one `.nup` package (base v65536 + update v131072, each an exefs/ and romfs/ tree, data stored as is) | `nx2/nup.py` extracts the newest title to `work/dump` |
| NSO compression | `main` is ZBIC (NSO flags bit 7, firmware 22.0.0+: zstd with its own magic and BIC-coded tables); rtld, sdk, subsdk0 are LZ4 | `nx2/unpack.py` rewrites ZBIC NSOs as plain ones (hashes checked), decoder `third_party/zbic` |
| SDK / NVN2 version | 22.2.8 (MKW: 21.4.0); same 46 services and 49 SVCs as MKW | 26 NVN proc names MKW never asked for, 14 not in `host/nvn_native.c` yet (stubs that stop the game only when called; DLSS parameter procs, window acquire timeout, HDR metadata, `nvnProgramBuilderGetShaders`, ...); the device limits in `nvn_native.c` are MKW sdk's (`devinfo[]`) and may need OoT's |
| Instructions | 4 UDOT (FEAT_DotProd) sites, the only forms suyu's translator lacked | `nx2/insn.py` `dot()` |
| Engine | romfs laid out like TOTK's (agl, gsys, Phive, Banc, Bake), Havok 2025.2, NintendoWare G3d2/Vfx3/Ui, SMAA, `.zs` (zstd) files | romfs read from `work/dump/romfs` |
| Microphone (the ocarina by humming) | to check at runtime (nn::audio input; `audsmx:u`, `auddmg:u` are in the services, as in MKW) | first a stub that hears silence; then a host microphone (WASAPI) or a key that plays the note |
| Cutscene video / voice | `Movie/` in the romfs | MKW's paths; `HOS_NO_MOVIES=1` to skip while booting |
| DLSS | NVN2 DLSS procs present (66 DLSS / mesh-task names) | a bilinear stand-in first (as Eglantine), FSR or NVIDIA's own DLSS later |

## 6. Stages and milestones

| | What | Done when |
|---|---|---|
| **S0** | the dump located and characterized (`nx2/dump.py --set`) | the six signals agree; program id, build ids, module sizes, SDK and NVN versions, romfs size recorded in section 9 |
| **S1** | loader + RELR, image in Eden's layout, discovery, coverage, census; instruction test (every form the game uses) and function tests (self-contained and call trees) against Unicorn | 100 % of discovered instructions translated (UDF aside), no BR without a jump table, every form bit-exact, every comparable function case identical |
| **S2** | the standalone lift (`lift.py --standalone --write`) and a full build (`build.py --standalone`): hookshot.exe and hookshot_vk.dll link | the boot runs `_init_libc*`, `nnosInitialize`, `nninitStartup`, the modules' DT_INIT, `nnMain` and reaches `nvnBootstrapLoader` |
| **S3** | NVN2 for this game: the procs it asks for, device limits, its window; the Vulkan replay | **the first frame**: a `run-vk.ps1` screenshot shows what the game draws first (a logo or loading screen) |
| **S4** | title screen and menus: romfs, save data, hid, audio (muted in tests), movies, the stubs it hits | the title screen and the file select are reachable by scripted input (`HOS_INPUT_SCRIPT` / `HOS_INPUT_LIVE`) |
| **S5** | gameplay: correctness (the flash detector `RX_VK_FLASH=1`, frame dumps) and speed (the profiler) | Kokiri Forest plays without flashes at the game's own frame rate |
| **S6** | PC: resolution, frame rate, controls, the microphone, DLSS/FSR | the user's call |

## 7. Testing

- **Instruction forms** (`nx2/test_insn.py`): every form the game's code contains, random states, Unicorn's
  ARMv8.x "max" CPU against the lifted C: X0-X30, SP, NZCV, V0-V31, FPSR and memory compared.
- **Whole functions** (`nx2/test_leaf.py`, `--calls` for call trees): random functions of every module on the real
  relocated image, natively and in Unicorn, on the 64-bit window with polls firing; registers, V registers and
  every changed page compared.
- **The production build** (`nx2/test_dll.py`, when a `hookshot_game.dll` is built).
- **In the game**: `scripts/run-vk.ps1` screenshots, the flash detector, frame dumps (`RX_VK_DUMP*`), the sampled
  profiler (`HOS_SAMPLE` + `scripts/prof.py`), and Eglantine as the A/B reference for what a frame should look like.

## 8. Rules

- No game files in the repository: decompressed modules, the image, lifted C, builds, romfs copies, dumps and keys
  stay in `work/` (ignored) or in the dump's own folder. Nothing is pushed anywhere.
- Commits end with `Assisted-by: Claude Opus 5.5` and `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `..\Eglantine` is read and run, never modified; mkw-recomp and the other ports are read and copied from, never
  edited.
- One game run at a time on this PC (mkw-recomp's runs take ~10 GB): `run-vk.ps1` refuses while `mkw.exe` or
  another `hookshot.exe` runs, and while Rainbow Six Siege is open; no heavy builds while Siege is open either. Runs are
  muted (`HOS_AUDIO=0`), at the priority `-Priority` gives (High by default), and screenshot folders are deleted once looked at.
- The lifted C and every build are GPL (suyu's translator is GPL-2.0-or-later; Eden's code is GPL-3.0).

## 9. Status

2026-11-04 (before the dump): fork base committed (`86871a1` = mkw-recomp `7336f0a`), the tools made to take any
dump, the runtime made game-neutral, the stage-1 tools checked on MKW's dump as a stand-in.

2026-11-04, the dump (`gamefile/...[0400486024dec000][v131072].nup`, 25.6 GB; extracted to `work/dump`, 26.2 GB):
- **S0 done.** Switch 2 by all seven signals. Program id 0400486024DEC000, update v131072 (base v65536).
  SDK and NVN2 22.2.8. 39-bit address space, main thread priority 44 core 0, 1 MB stack, system resource 48 MB.

  | module | build id | .text | .rodata | .data | .bss | eh starts |
  |---|---|---|---|---|---|---|
  | rtld | 8e213f228d12c7b0f8c21c3ce81b95e214417369 | 10,048 | 2,532 | 49,160 | 65,528 | 42 |
  | main | e0f3ccfaf50065ae4ea01eb5aeb13b1a45da3348 | 63,201,744 | 12,603,520 | 5,217,152 | 1,414,272 | 247,520 |
  | subsdk0 | b0110087a7b339ef74588d3bd0b4dd5d5c7abd0f | 542,576 | 234,548 | 69,648 | 65,520 | 1,039 |
  | sdk | 1643a838814f9c10c696f930ed681e7fa3fb9af8 | 8,906,832 | 6,093,948 | 435,448 | 1,862,408 | 35,786 |

  romfs: 186,583 files, 24.4 GiB (Tex 8.8, TerrainArc 5.6, Model 2.6, Sound 2.0 GiB), 10,154 `Nin_Ounce_NVN2`
  shader files.
- **S1:** the lift: 295,178 functions, 17,668,191 instructions, 100.0000 % translated (after UDOT), 1,475 jump
  tables, no BR without one. Instruction test: 742 forms, 44,172 cases, all pass.
- **S2:** the standalone lift: 294,450 functions in 57 chunks (1.4 GB of C), 485 HLE functions, 243 OS imports
  stubbed, 22 sdk NVN functions routed to `host/nvn2.c`.
  The build links (`hookshot.exe`, 426 MB; ~25 min for a full build with 4 jobs, a few minutes after a relift).
- **S3 done (2026-11-04, boot13).** The title screen renders at 60 fps: black for ~23 s (~1,400 presents) while it
  loads, then the logo, smoke and the menu (New Game / Options / ZELDA NOTES Setting), through the Vulkan back end.
  It runs the 150 s out with no crash. What it took, in boot order:
  - TLS: the owner handle at TLR+0x110 too (SDK 22). NVN API version 231.40 (main 0x8348c0f0 checks minor >= 40).
  - svcGetInfo 20 (UserExceptionContextAddress); the address arbiter (svc 0x34/0x35, WaitIfEqual64 too).
  - StandardSteadyClock::now returns in x0. ADR / ADRP+ADD code addresses as entries, but only outside the
    functions .eh_frame covers (all of them: 1.68 M duplicated instructions and a 122 MB chunk; now 79 entries),
    for the formatter callback 0x824c5560 that nothing else reaches.
  - SDK 22 overloads left to the guest SDK, which reached kernel calls the runtime does not have:
    CreateThread(..., prio, idealCore), ReleaseLightSemaphore(sem, count), MountRom(name),
    GetDirectoryEntryCount.
  - nn::os fibers on Win32 fibers, each with its own guest context (nn::async's FiberTaskCoordinator on the
    nn.npln threads); a fiber function's NULL return goes back to the thread.
  - Offline answers: ConnectToNamedPort gives a session whose requests are answered empty (sm:);
    nsd::GetEnvironmentIdentifier "lp1"; socket::detail::Poll nothing ready (gRPC calls it through a pointer);
    EnsureNetworkServiceAccountIdTokenCacheAsync fails.
  - nvnQueueWaitSync (an ordered no-op), SetCopyImageStride; no virtual core held through host file I/O
    (mkw-recomp 16bc3bc: the ARes loaders' 250 ms stalls are gone, longest wait now 0.7 ms).
- **Open at S3:** nvnCommandBufferPushComputeConstants is not implemented; one NVN2 program (e409733c45530d75,
  stage 5) does not lower (an unsupported native opcode at +0x4330); 70 imports answered by the generic stub
  (audio effects, prepo, nifm, socket/ssl setup, vi refresh rate, hid gyro drift mode, pl::RequestLoadNxFont).
  Next: input to get past the title (S4: New Game into Kokiri Forest), then the open items above.
- **S4 (2026-11-05): gameplay.** New Game → the movies (AV1 through Media Foundation) → Kokiri Forest, with the
  HUD, at 30-36 fps. What it took (commits through 7e88949 and the ones after):
  - Push constants (nvnCommandBufferPushComputeConstants) in c0 after the grid; ClearBuffer; prepo stubs.
  - setjmp/longjmp: the lifter wraps every call of a function that calls setjmp in a host setjmp of its own
    (host/hos_setjmp.c, assembly, returns_twice: clang's `__builtin_longjmp` restored rbp to the frame address,
    128 bytes off Win64's frame pointer), and longjmp's BR comes back through a64_ijump_hook.
  - HMMA on the host's tensor cores: HMMA_nx2 → IR MatrixMulAddF16 → OpCooperativeMatrixMulAddKHR 16x8x16 f16.
    gpu/vk/vk_coopmat.cpp measures the device's element order (NVIDIA: A's registers 1 and 2 swapped) and checks
    a multiply. OoT's networks (the movie upscaler, the gameplay TAAU) run at full speed (the movie at 60 fps).
  - Write-backs of GPU-written textures keep the pages the CPU wrote since (SceneLowPriority read texels as
    pointers). Storage buffer tables: GetCbuf(i, x*16 + base) addresses get a 16-entry descriptor array that the
    global memory fallbacks search (the froxel fog); the 32-bit global atomics got fallbacks too.
  - Program variants remember the storage images' integer-ness too: OoT writes R11G11B10 and RGBA8 targets
    through R32UI views, and a variant made while the slot held a float texture wrote packed words as floats
    (the yellow cast: NaN scene before the TAAU).
  - Extended registers: programs using ~249 GPRs (vertex and mesh) keep their high GPRs in IR registers 256 + n
    (REGMAP_nx2 prefixes, lower_to_maxwell.h RegisterMapPrefix; R246+ and R239 renamed to markers first).
    222 of the 240 dumped failing programs lower now (the offline harness: `nx2/lower_test.py work/lowfail`).
- **Open at S4:** an intermittent Vulkan device loss (nvlddmkm event 153) shortly after the first gameplay frames,
  never with RX_VK_CHECKPOINTS=1 (five runs) and every time without (four); mesh output clamps did not change it.
  Six programs still do not lower (CALL linkage 0x944, a texture gather mode, an LDG mode). Gameplay is CPU-bound
  at 30-36 fps (the replay thread ~9 ms a frame, the GPU ~10 ms): the game's threads next.

## 10. Findings, 2026-11-06

- White flash: one NaN in the scene target goes through the bloom's averages to the whole picture for a frame, the HUD over it.
  `GuardFragmentOutputs` keeps float colour outputs finite (RX_VK_NAN_GUARD=0: off). Where the NaN comes from is not known.
- Discards: a nested early return after a demote wrote its colours (`structured_control_flow.cpp`, from Lichii). That was the box
  around Link (`1e26755d`, the per-character shadow projector, drawn as a box that discards outside) and most of the milky look.
- The OoT fog (0.25) and exposure (0.65) factors are still needed: with them off the fog is far too thick. Fog's cause is open.
- Fps is CPU-bound: RX_VK_NO_WRITEBACK cut GPU time from 10 to 3.6 ms and left fps alone. HOS_PIN_BY_CORE 35 fps, HOS_SPIN_US=200
  slower, narrow barriers no change, RX_VK_WAIT_GPU no help.
- Tools: RX_VK_FRAMELOG + scripts/framelog.py (every frame shown), RX_VK_PROBE=2 (the passes at speed), RX_VK_SLOW_LOG, HOS_WATCH.
- A release build (`RX_RELEASE=1` at build time) precompiles before the game starts; test builds do not unless RX_VK_PRECOMPILE=1.
  hookshot_programs.bin (with hookshot_programs.idx, the list of its records), hookshot_pipelines.bin and mkw_vk_pipelines.bin go
  beside the executable.
- A program the driver's compiler faulted on (guarded_graphics) is never asked of the driver again (gpu/vk/driver_faults.h). On AMD's Windows
  driver the caught fault left the program's compile unfinished inside it, and every later build of that program waited there
  forever: the pause-menu freeze of issue 996, seen in a full dump. On AMD one program's builds also go to the driver one at a
  time (RX_VK_DRIVER_FAULTS=2: on every driver); other drivers keep building a program's variants side by side.
  The faults are kept per driver version in hookshot_driver_faults.txt beside the executable.

## 11. Findings, 2026-11-06, day

- Builds: `nx2/split_hot.py` puts code_hot.c in eight files that compile side by side, 79 minutes to about 6. `RX_PGO=gen` counts branches
  (host/pgo_rt.c writes LLVM raw format 11, the toolchain has no compiler-rt profile library), `HOS_PGO=<path>` with `<path>.reset` and
  `<path>.dump` files drives a run, `nx2/pgo_pieces.py` fits the merged profile to the pieces, `RX_PGO=<file>` uses it. `RX_VK_ONLY=1`: the renderer alone.
- Dark frames, open: lit surfaces come out black for one frame, geometry and depth intact. 5% with `RX_VK_FRAMEGEN=0`, 2.9% with the menu's Off, about 0.1% at 4x.
  Not the cause: render conditions, clear flags, ClearBuffer order, GPU lag, write-back, the real-time step, the ambient cubemap.
  `HOS_COND_RECORD` and `RX_VK_COND_ALWAYS` look like fixes by count and are not: the picture goes white. Tools: the frame log's dark trap dumps probes, passes and conditions; `RX_VK_PROBE_MIN`, `RX_VK_PROBE_UBO_HEX`.
  Also not the cause: colour write masks (`wm` in the pass trace: f on every lit pass), constants (`RX_VK_PROBE_UBO_HEX`), queue order (`RX_VK_EVENT_WAITS=0`), and the capture in
  synchronous mode (`RX_VK_SYNC_CAPTURE=1`, which does take 380,000 pages per 600 submits). `RX_VK_QUEUE_STATE=0` stops the game at the Load menu, and `RX_VK_MIRROR=0` is 8 fps: neither says anything.
  What holds: geometry and depth are right (black shapes against the fog), the prepass and the lit passes run with the same draw counts, and the lit passes output black.
  The lit materials' four storage buffers (c0+0x510..0x540) keep their addresses, sizes and non-zero counts in dark frames as seen in guest memory; the 720 KB one is all zeros in good
  frames too, so it is no cause, though its compute shader (4b03b304) runs every frame. Guest memory is not what the GPU reads, so these need comparing on the GPU's side.
  Next: their mirrors' contents read back on the GPU's timeline, and the bound textures past the centre texel. `RX_VK_SUBMIT_DELAY_US=3000` shows none, but at 33 fps, where they never show:
  repeat with a delay that keeps ~45 fps. The MKW session's mirror page skip fired 0 times here and was removed.
  Not an instrument's doing: with nothing attached, photographed from outside the window (37 s, ~2200 photographs): 16 with `RX_VK_FRAMEGEN=0`, none at the user's 4x. With
  `RX_VK_SUBMIT_DELAY_US`: 7 at 300, 0 at 1000; a repeat at 1000 with shots still had 7 events against 50-60. So it is a race with the game's threads: less overlap, fewer. The delay is a test, not a fix.
- The builder's first full build: 43 minutes at below-normal priority, 0 failures; the folder starts from a double click and reaches the title at 60 fps.
- Render conditions here gate single lights and read reports earlier in the same stream, so they cannot be read at record time.


## 12. Findings, 2026-11-11: what a frame waits for

- `HOS_CHAIN` + `scripts/chain.py` walk each frame back from its present (RTX 3090, Ryzen 9 7900X, Linux, NVIDIA 620.70, Kokiri
  Forest standing, 2560x1440, ~1250 draws, recording on: 17.8 ms a frame). The frame is one loop through two threads, nothing else:
  - the presentation thread, from the main thread's message to its present: 12.5 ms (the replay of the frame's commands);
  - the present call, to the event that releases the main thread: 2.7 ms, ~2.4 of them inside `vkQueuePresentKHR` (in the driver,
    in `poll`);
  - the main thread, from that event to its next message: 2.6 ms, ~2 of them the renderer's work on its own submits (the submit's
    capture copies, `memory_collect_written`, its replay).
  The game's job threads (ModuleSystemWorker, BaseProcExecutor) and the background threads are on the path under 0.1 ms a frame;
  waits for a virtual core 0.27 ms (the presentation thread taking one back after its present); hand-offs ~10 us each. The main
  thread waits ~4.5 ms a frame on the presentation thread. So the game's own CPU work is not what holds the frame here: the
  replay, the present and the submits on the game's threads are. ("Gameplay is CPU-bound: the game's threads next", section 9,
  is answered: the renderer's work on them.)
- The present's wait is the GPU finishing the frame: with `RX_VK_PRESENT_THREAD=1` the presenter thread waits 1.75-1.92 ms a
  present for the copy's fence and then 0.18-0.22 ms in `vkQueuePresentKHR`. The game's present call then returns in 0.07-0.10 ms
  (median) instead of 2.0-2.1, and the frame (recording on, alternating runs) took 20.1 and 19.0 ms against 22.1 and 22.9. With the
  present off the path, the main thread's job waits (lr 0x80070bfc) start to show on it. (`RX_VK_NO_WRITEBACK=1`, the 1280x720
  output, a 240 Hz compositor and `RX_VK_IMMEDIATE=1` left the wait as it was.)
- Measuring on this PC: two runs of the same build vary by 10% and more (other work on the machine); fps differences under that
  need many alternating runs. `chain.py`'s stretches are steadier: they say which part moved.
