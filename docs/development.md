# Hookshot

**Hookshot** is the port's name (as Skybound is TOTKPort's and BlueShell is mkw-recomp's); the game builds as
`hookshot.exe` + `hookshot_vk.dll` (`hookshot` + `hookshot_vk.so` on Linux).

A static recompilation of the game's Switch 2 remake to native x86-64 Windows and Linux:
the game's code lifted to C (NotRexed's a64-kit, Rexcomp's AArch64 kit), the Horizon OS and NVN2 implemented
natively, the GPU work replayed on Vulkan. It is a fork of mkw-recomp (`..\mkw-recomp`, Mario Kart World), whose
paths it keeps so MKW's fixes apply here.

**No game files are in this repository.** The dump is read where it lies (`work/dump.txt`); everything made from it
(modules, image, lifted C, builds, runs) goes to `work/`, which git ignores.

Status: the game plays; the README's [status table](../readme.md#project-status) says how far. The design, its stages and
the lessons carried over from MKW: [design.md](design.md).

## Use

Requirements: Python 3 with `pip install -r requirements.txt` (`lz4`, `numpy`, `capstone`, `unicorn` 2.1,
`zstandard`, and the button-prompt packages); a compiler: on Windows llvm-mingw
(`HOOKSHOT_MINGW`, or an `llvm-mingw` folder in the clone), else emsdk's clang at `C:\emsdk` (or `EMSDK`) with Visual
Studio's C++ tools; on Linux clang and lld. For the Vulkan back end: distro Vulkan / SPIR-V / VMA headers under
`/usr`, or `HOOKSHOT_VULKAN_HEADERS` / `HOOKSHOT_SPIRV_HEADERS` / `HOOKSHOT_VMA_HEADERS`, or a CPM package cache
(`RX_CPM`, else `cpm` in the clone, else the old caches in mkw-recomp's `work/eg` or `..\Eglantine\.cache\cpm`).
Without those the standalone build stops. [CONTRIBUTING.md](../CONTRIBUTING.md#development-setup) has the rest.

On Linux, `make_game.py` checks the mandatory Python modules, compilers/linker, renderer headers and FFmpeg
development libraries before reading or translating the dump, and lists every missing prerequisite in one error.
It uses the compile's lookup order: CPM caches first, then `HOOKSHOT_*_HEADERS`, then `/usr/local` and `/usr`;
FFmpeg comes from `HOOKSHOT_FFMPEG` when it holds `lib/libavcodec.a`, otherwise from `pkg-config`. The C probe
uses the selected compiler, lld and `HOOKSHOT_SYSROOT` if set. Every build compiles the Vulkan back end, the AppImage's
too, so the renderer headers and clang++ (for it and the native translator) are always needed.
The AppImage uses its bundled tools,
and `RX_TARGET=windows` keeps the Windows toolchain checks rather than requiring Linux development libraries.
The button-prompt packages in `nx2/prompt_requirements.txt` remain optional; this check installs nothing.

Run everything with `python -I` from this folder.

| Command | What |
|---|---|
| `python -I nx2/nup.py <file.nup> --extract work/dump` | the `.nup` dump package to `work/dump/exefs` + `romfs` (newest title; ZBIC NSOs made plain by `nx2/unpack.py`) |
| `python -I nx2/dump.py --dump <folder with exefs/ and romfs/> --set` | what the dump is (Switch 1 or 2, ids, modules, SDK, NVN); `--set` records it in `work/dump.txt` |
| `python -I nx2/lift.py [--write]` | image + discovery + coverage (`work/image/coverage.txt`); `--write` emits the C to `work/gen` |
| `python -I nx2/census.py --nx2` | instruction census per module, after the Switch 2 forms |
| `python -I nx2/test_insn.py [--per 6 --states 12]` | every instruction form the game uses, Unicorn vs lifted C |
| `python -I nx2/test_leaf.py [--count 3000 --states 4] [--calls]` | random whole functions (or call trees), Unicorn vs recompiled native code on the real image |
| `python -I nx2/lift.py --standalone --write` then `python -I nx2/build.py --standalone --jobs 4` | the standalone game: `work/sa/build/hookshot.exe` + `hookshot_vk.dll` |
| `powershell -File scripts/run-vk.ps1 [-Window] [-Seconds 120] [-Name x]` | a muted test run on Vulkan, with screenshots in `work/runs/<name>/shots` |
| `python -I scripts/prof.py <stacks file>` | a profile sampled with `HOS_PROFILE=<seconds>` + `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<stacks file>` (the sampler runs only with `HOS_PROFILE` set), named from the linker maps |
| `python -I scripts/tested_configurations.py [--output work/tested-configurations.md]` | Read the [tested configurations](tested-configurations.md), combining new run files with the frozen table; UTF-8 stdout by default, or a local Markdown file that is not committed |

The recorder's game-free regression is `python -I -B tests/test_recorder_sync.py --headers <Vulkan-Headers/include>`.
It compares fake-driver calls and rejects deliberately broken recorder variants without creating a Vulkan device.
Its destruction case keeps the final copy unpublished until a boundary flushes it, so a skipped destruction wait is
caught even if the producer is paused long enough for every already-published command to finish.

The dump can also come from `OOT_DUMP` (its folder) or `OOT_EXEFS`; `OOT_WORK` moves the work folder.

The built game's own arguments (`image.bin`, `--romfs`, `--user`, `--trace`, `--prepare-shaders`) are in the README's
[Running the game from the command line](../readme.md#running-the-game-from-the-command-line).

Scripted test input (`HOS_INPUT_SCRIPT` and `HOS_INPUT_LIVE`) is applied before the window-focus check. A log saying
the game window is not in front does not mean those presses were discarded. `HOS_HEADLESS` and
`HOS_WINDOW_NOACTIVATE` change window creation, not this input routing. The game must still reach a state that reads
and responds to the supplied controller input.

Physical keyboard and mouse input stays with the foreground window. Physical controllers can also play in the
background when the existing "Controller in background" option or `HOS_INPUT_BACKGROUND=1` enables them;
`HOS_INPUT_BACKGROUND=0` overrides that option. With `HOS_INPUT_EXCLUSIVE=1` the script
alone drives the game until 5 s after its last press; without it the player's devices add to the script's presses. It
is not a focus override.

When an SDL controller connects, its `hid: controller` log line includes the hardware `guid` and active
`mapping-name` (names are limited to 128 bytes). The `mapping-string` follows in GUID-labelled parts of at most
512 bytes, up to 1024 bytes in total, with a note when truncated. The GUID and mapping describe a device model's
button layout; no serial, path, user or account call is made. A mapping the player supplied himself, by environment
or file, is logged as he wrote it, within those limits.
`mapping=available` means SDL returned a mapping, not that its origin is known; `mapping=none` means SDL returned
none, and `unavailable` means the loaded SDL lacks a required diagnostic call. A zero GUID is `unknown`.
These queries do not change bindings, load a mapping file, or change which controllers are read.

## Switches for testing and debugging

The running game reads its switches from the environment or from `hookshot_env.txt` beside `image.bin` (one
`NAME=VALUE` a line, `#` for notes; `HOS_ENV_FILE=<path>` reads that file instead). The environment goes first: a name
already set when the game starts keeps its value and the file's line for it is left out (the log says so). A switch
that stands in for a menu row lasts for its run; the options file keeps the player's value (`env_override` in
`gpu/vk/vk_menu.cpp`). Switches the shader translation reads are part of the program cache's stamp
(`disk_stamp` in `gpu/vk/vk_shader.cpp`; `tests/test_program_stamp_knobs.py` checks the translator's). Its other part is the
translator's own sources, as `nx2/translator_files.py` names them (`gpu/vk/{eden,sirit,compat}/`, `vk_shader.cpp`,
`vk_bank.cpp`, `vk_state.h`; not `vk_api.h`, which the translator reads nothing from): any change to one of them, a comment
included, has every player translate every program again at their next start, so what the translator does not read belongs
elsewhere. The build reads its own from the
environment. The README has the ones players are asked to try (`RX_VK_ASTC_RGBA8`, `RX_VK_LOW_SPEC`,
`RX_VK_MOVIE_UPSCALE`, `RX_VK_SKIP_CODE`, `HOS_OOT_CHEATS`). There are several hundred more, most of them one-off
experiments: `grep -rnE '(getenv|env_on)\("' host gpu/vk` lists them, and the comment where each is read says what it
does. An on/off switch ("(set)" below) is on for `1` or any other value, and off when it is unset, empty, `0`, `false`,
`off` or `no` (`env_on` in `gpu/vk/compat/hookshot_env.h`, `kit.env_on` in Python); it used to be that any value turned
one on, `0` included. A switch that takes a number or a list reads its value as it says.

`RX_VK_MESH_NOCULL=<prefix,...>` turns off the culling in the task shaders whose native code key starts with one of
the prefixes. On a Steam Deck, `RX_VK_MESH_NOCULL=2c56dc1006942f74` avoids the same hang without leaving anything out:
that task shader walks a coarse depth grid cell by cell for every meshlet and ends with
`visible = (result & 1) | (c3[2320] & 4)`, bit 4 being the game's own "draw everything". The switch makes that answer
true, and dead code elimination removes the grid walk, so every meshlet is drawn and the rasterizer clips what is off
screen. It costs some GPU time on cards that cull quickly, so it is off unless set. The log says
"culling off, every meshlet drawn" for each task shader it changed.

The game culls by occlusion: its z-prepass draws each candidate object's bounds between a samples-passed reset and a
report, reads the reports back on the CPU a frame or two later, and leaves out the objects whose count was 0. The
replay writes every report as 4096 ("visible") as it is recorded and gives each depth-only draw from a reset to its
report a Vulkan occlusion query; as each recording begins, it reads every waiting report's queries at once (no wait)
and writes the sum over the reports whose queries have all ended. A count not known for sure stays "visible": a late
or unknown count costs culling, a wrong 0 would hide an object in view. So a report stays 4096 when its run has a draw
that writes colour or more than 256 draws, when one of its draws did not draw (it returned before its draw command,
its vertex input could not all be bound, or the mesh fallback split it into task batches: its query is never begun),
when another queue's queries came in the middle of its run (the queries are numbered from one counter for all the
game's queues), and when the slot was reported into again before the count came. Work held back at an event wait has
its reports written as 4096 at once, so the game never reads an older count there before the replay gets to it.
`RX_VK_OCC_QUERIES=0` turns this off (every report reads 4096, as before), as does setting `RX_VK_OCC_FIXED=<n>`
(every report reads n, whatever `RX_VK_OCC_QUERIES` says); the render thread (`RX_VK_ASYNC=1`) keeps the queries, and
the game, a frame ahead of it, reads counts a frame older;
`RX_VK_OCC_QUERIES=2` counts the runs with colour draws too (a query around a real draw cost ~0.9 ms of GPU time a
frame in the Market on an RTX 3090, for a few more objects left out).
`RX_VK_OCC_LOG` (set) logs every ~5,000 reports how many read 0, how many were left visible and why, how late they
were written and what the reading cost;
`RX_VK_OCC_DRY` (set, a test) makes and reads the queries but leaves the reports at 4096, to time the queries alone.

A CPU read of GPU results waits for the write-back of the submission that writes them (`invalidate`, `vk_replay.cpp`).
`RX_VK_READBACK_LATE=<rule>[,<rule>...]` names the ranges read without that wait, which then hold the frame before's
values: `newest` (the range's writer is the newest submission handed out: the game reads it right after submitting the
work that fills it, so it cannot count on that work being done) and a hex size (the ranges of exactly that many bytes);
either rule is enough (`gpu/vk/readback_late.h`). Off unless set. On a Steam Deck OLED `newest` alone gives nearly all of
the gain (a heavy view: 24.3 fps with no rule, 30.1 with `newest`, 24.9 with `3980`, 30.3 with both): two 0x3980-byte
ranges are 40 to 45% of the readbacks' wait there, larger ranges the rest. It is not a rule for every range: the exposure
meter read late is the white flashes the wait is there for. `RX_VK_LOG_READBACK_RANGES=1` logs the readbacks by range
every 3,000 calls (calls, waits and their time, how many had the newest submission as their writer, how many were read
late), which is how the ranges worth a rule are found on a PC.

Native HDR PsychoV35 automatically enables the compatible RGBA16F scene path for DLSS/DLAA, AMD FSR 4 (and its Native AA), and FSR 3.1 on the shared chain.
`RX_VK_PSYCHOV35_DLSS=0` disables that route for troubleshooting; unset, empty or nonzero enables it. The value is read
once per process, so restart after changing `hookshot_env.txt`. The existing flag name is retained for existing
configurations. The successful upscale supplies NVIDIA frame generation with depth, camera and actor
motion as usual. It requires the engine's native-HDR context: DLSS's HDR feature (`RX_VK_DLSS_HDR=0` prevents this
path), FSR 4's HDR context (`RX_VK_FSR4_HDR=0` prevents it), or FSR 3.1's already-HDR-capable passes. FSR 3.1 keeps
Vanilla's existing colour path; its linear HDR path is limited to Native PsychoV35. Its legacy own-input route
(`RX_VK_FSR3_CHAIN=0`) is not enabled by this switch. Off and the TAAU replacement have independent format support.
See [the format and history rules](temporal-history.md#psychov35-with-temporal-upscalers) for validation and shader regeneration.

Upscaler Off supports Native HDR PsychoV35 independently of that switch: matching RGBA16F views and a typed output pass bypass
the game's network at 100% render resolution, or spatially scale its input below 100%. The output pass decodes the
network input once; it supplies neither temporal anti-aliasing nor the temporal engines' NVIDIA FG input handoff.

The gameplay TAAU replacement supports Native HDR PsychoV35 directly. It is the spatial/FXAA fallback used where the
game network cannot run, not restored temporal AA. Its RGBA16F shader and history match the scene's format and decode
the perceptual input once. `RX_VK_TAAU_KEYS=fd029a317a9a4b94` exercises that replacement on hardware that otherwise
runs the native network; this is a test override, not a requirement for GPUs already using the fallback.



For **FSR 3.1 frame generation**, use Frame generation **2x** and `RX_VK_FSR_FG=1`. Native HDR PsychoV35
automatically enables the compatible independent RGBA16F depth/motion input path unless `RX_VK_PSYCHOV35_DLSS=0`.
This does not enable the legacy FSR
upscaler. To isolate FSR generation on NVIDIA hardware, select Frame generation by **Built-in FG** or use
`RX_VK_DLSS_FG=0`; confirm actual `fsr3fi` made-frame counts, not only the menu selection. FSR generation remains off by default.
The captured present's actual PQ/sRGB encoding and HDR peak are given to both AMD interpolation and optical flow;
format, tone mapper, encoding and luminance changes reset the corresponding histories. See
[FSR frame generation](temporal-history.md#native-hdr-and-fsr-31-frame-generation) for limits and validation.

### Running a test

| Switch | What it does |
|---|---|
| `HOS_HEADLESS` (set) | No window: the renderer draws offscreen, for screenshots and frame dumps. The menu's Save my pipeline list then opens nothing (no browser, folder or clipboard) |
| `HOS_WINDOW_SIZE=<w>x<h>` | The window's size in pixels (1280x720 by default, wider for an ultrawide picture) |
| `HOS_WINDOW_NOACTIVATE` (set) | The window opens behind the others and does not take the focus (Windows) |
| `HOS_FULLSCREEN=1` | Starts in fullscreen; the Insert menu's Fullscreen row is then left alone |
| `HOS_AUDIO=0` | No sound device is opened (the mixer still runs); `scripts/run-vk.ps1` sets it unless `-Sound` |
| `HOS_LANGUAGE=<code>` | The game's language for this run, a code from `language.txt` |
| `HOS_OUTPUT_RES=<width>x<height>`, `HOS_HANDHELD=1`, `HOS_ASPECT=<ratio>` | Over the options file: the output resolution (a size, as `output_res`), the handheld profile, a picture wider than 16:9 (2.3889 for 3440x1440) |
| `HOS_DISPLAY_MODES=<w>x<h>,<w>x<h>,...` | The screen's display modes for this run, in place of Windows' (`gpu/vk/display_modes.h`): the Insert menu's Aspect ratio and Output resolution rows offer these sizes and the common sizes of their shapes (`display_modes.h` `common_sizes`), for checking a screen you do not have |
| `HOS_WIDE_MAX_PX=<pixels>` | The most pixels a picture wider or narrower than 16:9 takes (`host/hos_wide_size.h`): 5120x2160's by default; 8294400 is 3840x2160's, the cap before |
| `HOS_WIDE_UI=0` | The HUD on a screen that is not 16:9 as before (`host/hos_wide_ui.c`): in the middle 16:9 of a wider screen, fitted to a narrower one's width, instead of at the screen's edges. `HOS_WIDE_UI_LOG=<n>` logs the plan's first n changes (each pane with its layout and move), each root layout (the HUD's, a menu's, over the whole picture, or held at the HUD's size) and the plan's time |
| `HOS_HUD_MAX_RATIO=<ratio>`, `HOS_HUD_FULL_RATIO=<ratio>` | The HUD's limits (`host/hos_wide_ui.c`): its sides reach a screen's edges up to 12:5 (2.4; `0`: at any width), and it keeps its 16:9 size down to 3:2 (1.5), smaller below |
| `HOS_DYNRES=0`, `HOS_REALTIME_STEP=0`, `HOS_TEXT_SPEED=<0..3>` | Over the options file: Resolution 100% (non-zero: Auto), Game speed Per frame (1: Real time), Text speed |
| `RX_VK_FRAMEGEN=0`, `RX_VK_FRAMEGEN_MULT=<2..8>`, `RX_VK_PACE=0`, `RX_VK_UPSCALE=<0..2>` | No frame generation whatever the menu says; over the options file: its multiplier, frame pacing, the upscaling filter |
| `RX_VK_DEVICE=<n>` | The graphics card listed as `vk: device <n>` in the log, over the menu's GPU device |
| `HOOKSHOT_NO_UPDATE_CHECK=1` | No look for a newer release at the start |
| `HOS_TRACE=1` | Every Horizon call logged, as `--trace` does |
| `HOS_CV_SIGNAL_ALWAYS` (set) | SignalConditionVariable takes its lock and wakes even when no thread waits, as before it returned at once for no waiter (`host/nn_os_sync.c`); a control for a hang or a movie stutter |
| `RX_BACKEND=<name>` | Unset or `vulkan`: `hookshot_vk.dll`; any other value: the WebGPU renderer, when the exe was built with one |
| `HOS_GPU_ALLOC_RETRY=0` | A model's material-block allocations from a full heap (the OptimizeChangeBank thread's two CPU buffers and two GPU-memory calls in f_804566e0) not tried in the heap's parents, as shipped (`host/oot_heap_retry.c`, issue 1697) |
| `HOS_GPU_ALLOC_TEST_FAIL=<n>` or `<gpu>,<cpu>` | A test: the first n of those allocations of each kind (or that many of the GPU memory and of the CPU buffers) refused by the heap they name, as a full heap would, so the retry runs |
| `HOS_MEMINFO_OLD=1` | The used memory the game is told (`QueryMemoryInfo`, svc `GetInfo` 7 and 22) is the program and the system's share alone, as before. By default it adds the heap and the pages the game mapped through `AllocateMemoryPages`, at most the console's total (`hos_used_memory`, `host/hos_mem.c`): told less, the game kept mapping pages for its caches (7.9 GB in a long session). For A/B |
| `HOS_HEAP_COMMIT=1` | Windows: the whole heap committed at `SetMemoryHeapSize`, as before. By default it is only reserved and committed 64 KB at a time as it is first touched (`commit_on_touch`, `host/mkw_glue.c`), and host calls that hand guest memory to the kernel (`nn_fs.c`'s reads and writes) commit their range first; a commit refused at Windows' commit limit then shows mid-play instead of at the heap's setup. For A/B |
| `HOS_PAGES_LIMIT=1` | A diagnostic: `AllocateMemoryPages` is refused past the console's total (heap and mapped pages), as on the console. Keep it off: the game retries a refused allocation forever |

### Crash and unclean-end records

The game keeps a local text record in `bug-reports/pending/crash-<UTC time>.txt`
after an unhandled exception, Linux SIGSEGV/SIGBUS/SIGILL/SIGFPE/SIGABRT, the
Windows heap-corruption/fail-fast codes already reported by the host, `hos_fatal`,
or a fatal renderer exit (device loss has its own reason). A same-second collision
adds the process id and a counter; an older record is never overwritten.
The existing `CRASH:` log lines remain unchanged. Nothing is uploaded or opened.
Fatal-message collection runs in the shared host logger before its thread and level tags are
added. Both ordinary and levelled host logging keep their existing formatting.
Each start gets a random `run_id`, logged immediately after the version line, before
the cache/log migration summary and record-pruning notes, and copied into its records.
Match that id, not a rotated log's current filename, to find the right run.
An unclean-end record keeps the previous marker's id.

The text has the failed run's version, UTC time, uptime, exception/signal code,
fault-address class (not the address), thread, module basenames and offsets,
bounded host and guest backtraces, and the guest registers already printed in
the log. It does not copy the memory near those registers, saves, full paths or
environment values. Last-present age is `unknown` when the host has no timestamp.
Offsets are relative to the module's load base, the same numbers as the existing
`CRASH:` lines, not offsets in the file. Ordinary namespace-qualified thread
names such as `sead::MainThread` are retained; addresses, paths and identities
are still redacted.
The Linux host trace is a checked frame-pointer chain, and may stop early in
optimized code; the existing log still attempts its longer glibc backtrace.
Linux core-dump settings are neither requested nor changed. The record's pid and
time can locate a core the system kept itself; no core is copied into a report.
Real hardware faults re-fault with their original signal information, so their
system core dumps are unchanged. Only externally sent signals are re-raised.

Each process owns an OS-locked `bug-reports/running/<pid>.running` marker.
At the next start, an unlocked marker without a completed crash record produces
an **unclean-end**, not a crash, record: `unclean-<UTC time>.txt`. It means only
that the last run did not end cleanly. It cannot distinguish a power cut,
forced termination, an out-of-memory kill, or a handler that could not run.
Only file size/time and named trouble flags from the last log's tail are added,
not arbitrary log text. Recovery runs before log rotation. The private marker keeps
the host's selected destination (`logs/hookshot_log.txt`, or its beside-game fallback)
or an externally redirected stderr file, rather than assuming a loose log.
Live markers are skipped, even when two games use one folder; process-id liveness is not the test.
SIGTERM, SIGINT and SIGHUP are deliberately not crash signals here. Logout,
Ctrl-C or a test timeout can therefore leave an unclean-end record. Its fixed
note says: "ended from outside or lost power, not necessarily a crash".
Normal returns, control quit and window close/restart remove only their own
marker and empty staging file, never earlier reports. A read-only
folder disables recording without preventing play. Disk errors or an exhausted
faulting stack can still prevent a record; this is not a crash-recovery facility.

At startup, only the newest **15** crash/unclean text records are retained, by
the UTC in their filenames (oldest first removed). A later crash can add one:
at most 16 for sequential runs, or 17 with two games running together. Only
regular files matching these exact names in `pending/` are pruned. Symlinks,
directories, stall records, local-only dumps and completed menu reports are
untouched. If removal fails, the extra file stays and one fixed warning follows
the version/run-id banner. There is no pruning or cross-process wait in a crash.

On Windows, `HOS_CRASH_MINIDUMP=1` additionally asks a startup-created worker for
a dump in **`bug-reports/local-only/`**, after the text record is durable.
Only an exception filter requests a dump; `hos_fatal` and renderer-fatal records
remain text-only. The `vk: device lost:` prefix is captured before later driver
queries, even if the terminal `vk: FATAL:` never arrives. The existing opt-in
`RX_VK_FAULT_INFO` query before that prefix can still hang before capture.
It is off by default, for a developer debugging their own build, and **must not
be shared**: thread stacks can contain fragments of game data and private data.
The text says `dump: off` or `dump: requested locally, not for sharing`; a request
is not proof that DbgHelp succeeded. The failing thread waits at most two seconds.
The worker writes a one-line `.outcome.txt` beside its local-only dump: written,
failed, timed out waiting, or skipped because another report worker was using
DbgHelp. The shared guard is non-blocking and is taken only on workers, before
any thread suspension. The public text is never changed after publication.

The exact flags are `MiniDumpNormal | MiniDumpFilterMemory |
MiniDumpFilterModulePaths | MiniDumpWithoutOptionalData |
MiniDumpWithoutAuxiliaryState`. Microsoft's
[type reference](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/ne-minidumpapiset-minidump_type)
defines Normal as the information for all threads' stack traces; the filters keep
stack pointers needed for unwinding, remove module-path information, and reduce
optional/auxiliary memory. The callback also excludes module code/data sections
and CodeView/miscellaneous records (which can hold build paths). No full-memory,
heap/private-memory, indirectly referenced memory, or data-segment flag is used.
These filters **do not guarantee privacy**. Size depends on the threads and their
stacks; a typical game-dump size has not been measured. The game-free Windows
fixture prints its tiny child's actual dump size, not a game-size estimate.
[Microsoft recommends a separate process, or a dedicated thread](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump);
even the dedicated worker can fail or stall after heap/loader corruption.

### The window after a crash

Right after the game crashes, at start or in play, a window says so: **Hookshot closed unexpectedly** (or **Hookshot
couldn't start**), when (during the start, while it prepared the shaders, while playing; nothing when the run's log
isn't found), what went wrong where that is known, and **Report**, **Open the log folder**, **Close** (`host/hos_crash_dialog.c`; what decides is
`host/hos_crash_dialog.h`).

It is shown by a helper the game starts early in `game_main`, once the crash record and its run id are set up and before
the guest, the renderer, the sound and the input: the same executable, `hookshot --crash-watch <pid> <run id> <game
folder> <log>`. `main()` hands that command line to `hos_crash_helper_main` first thing, so the helper starts nothing of
the game; it loads SDL3 or SDL2 for its message box (Windows' `MessageBoxW` without one) and nothing else. On Windows it
is started with `CREATE_NO_WINDOW`, no inherited handles and out of the launcher's job where the job allows it; on Linux
its input and output are `/dev/null` (never the game's log or a launcher's pipe) and it closes any other file it
inherited. It waits for the game's process to end
(Windows: the process handle, with its exit code; Linux: a pidfd, else its parent changing), turns a marker left behind
into an unclean-end record as a start would, then decides:

| How the run ended | The window |
|---|---|
| a crash record of the run (`bug-reports/pending/crash-*.txt`), a quit before it included | yes |
| the log says the start could not go on, before a save was loaded: the game's files (`cannot open`, `usage:`, `lift and image do not match`), Vulkan (`did not load ... nothing is drawn`, `vkCreateInstance failed`, `no Vulkan 1.3 GPU`, ...), the window, `cannot reserve the guest window` | yes, **Hookshot couldn't start**, a quit included (a black window closed) |
| the marker left with no crash record (killed, out of memory, a handler that could not run) | yes, unless the player had asked to quit, or Windows' exit code is 0 |
| a quit, then the process ended while it closes (a second close of a window that stopped answering) | no: `rx_close_asked` notes the quit first in the marker, and the unclean record says `quit_asked: yes` (the same rule as the renderer's crash markers, [PR 498](https://git.notrexed.dev/Hookshot/Hookshot/pulls/498)) |
| a clean end: a quit, a return, the menu's restart (Windows: a new process with its own helper; Linux: the same process, whose new run's marker ends the old helper) | no |
| Windows is signing out or shutting down | no (a window would hold it up); a crash record of the run is shown at the next start |

The cause is the first that holds: the game's files, Vulkan, the window (before play), memory (`cannot reserve`,
Windows out of memory, `0xC0000017`, or an unclean end after the start's low-memory warning), the graphics driver
(`device lost`, a Vulkan fatal, a crash in the driver's own module), ended from outside; else "an unexpected error".

**Report** builds a plain-text report: what happened, the version and its channel, the GPU and driver (from the log's
`vk:` lines), CPU, RAM and system, what the game was built for, the log's first DLSS and FSR 4 lines,
`hookshot_options.ini`'s settings and the switches set (a value only when it is a number, a size or on/off, never for a
name with TOKEN, KEY, ROOM, SECRET, PASS, SERVER or USER; not the controls' bindings), the crash and unclean records of
the last day, the run's crash record, and the last 200 lines of the run's log with the home folder and the user's name
taken out as the Insert menu's Bug report does. Never saves, game files, the executable or caches, memory dumps, keys,
the options file itself or the home folder's path. It is put on the clipboard (Windows' own; on Linux through `wl-copy` on
Wayland, else `xclip` or `xsel`, which keep it after the helper ends, and SDL's X11 clipboard only as a last resort, given
while the next box is open: Wayland takes a selection only from a focused window, which the helper no longer has), written to `bug-reports/pending/report-<UTC>.txt` (the newest ten kept), and the game-problem issue form
opens in the browser with the version, GPU, system and a title filled in. The box that follows says whether the report is
on the clipboard, never that it is when nothing took it, and names the saved file, which **Open the report** opens to copy it
from. Nothing is sent by the game: the player pastes the report.

Under gamescope (the Steam Deck's Game Mode: `GAMESCOPE_WAYLAND_DISPLAY` set, or `XDG_CURRENT_DESKTOP=gamescope`) there
is no helper and no box of an earlier crash at the start, until someone has tested that gamescope shows a child process's
box: one nobody sees would keep the helper, and so the game in Steam's eyes, running. The records are still written.

A start that finds a crash record of the last day that no helper showed (the helper could not run, or was ended with the
game) shows it once, before it goes on (`--crash-show`); `bug-reports/shown.txt` lists the records shown. The first
start with this window lists what was pending already. Unclean ends are shown only by the helper, never at a start: a
launcher or a logout that stopped the game is no crash.

| Switch | What it does |
|---|---|
| `HOS_CRASH_WATCH=0` | No helper and no window after a crash, for development runs; on by default. None either for `--prepare-shaders`, `HOS_HEADLESS`, a profile-guided training run (`HOS_PGO`) or under gamescope |
| `HOS_CRASH_TEST=start` or `=<seconds>` | A developer's test: the game crashes on purpose (a write to address 16) just after its crash handlers are set up, before the renderer, or that many seconds after the start, on a thread of its own. Off unless set; taken from the environment only, never from `hookshot_env.txt` |

To test it: start the game with `HOS_CRASH_TEST=start` (the window says *during the start*), with `HOS_CRASH_TEST=60`
and a save loaded within the minute (*while you were playing*), end it from Task Manager or with `kill -9` (*ended
without a crash report*), and quit normally and with the Insert menu's Restart now (no window). `tests/test_crash_dialog.py`
checks the decisions game-free, and on Linux runs the real helper against real processes with the window stood in for.

### Input for scripted runs

| Switch | What it does |
|---|---|
| `HOS_INPUT_SCRIPT=<presses>` | Presses timed from the game's first controller read |
| `HOS_INPUT_LIVE=<file>` | A file the test appends lines of presses to, read every 30 ms; each line's times count from when it is read. A line `!cheat <n>` sends a Cheats page action, `!synclog` logs the NVN syncs (`!dump`, `!shots`, `!rdoc` and `!flasharm` are the WebGPU renderer's) |
| `HOS_INPUT_FRAME_CLOCK=1` | The script's time runs at 60 game frames a second instead of the wall clock |
| `HOS_INPUT_EXCLUSIVE`, `HOS_INPUT_BACKGROUND`, `HOS_INPUT_SCRIPT_BACKGROUND` (set) | As above; the last lets every device count while the window is behind another one |
| `RX_VK_MENU_SCRIPT=<seconds>:<key>,...` | Timed Insert menu keys: `INS`, `UP`, `DOWN`, `LEFT`, `RIGHT`, `ESC`, `ENTER`, `TABL`, `TABR`, the pad's face buttons `PADS`, `PADE`, `PADW`, `PADN`; and for a text field, `TYPE:<text>` (typed as the keyboard would, up to the next comma) and, while it has the keyboard, `HOME`, `END`, `BACK`, `DEL` besides `LEFT`, `RIGHT`, `ENTER`, `ESC` |
| `RX_VK_UI_PROMPTS=pad` or `keys` | The menu's button prompts for a controller or the keys, whatever was used last (screenshots) |
| `RX_VK_UI_GAME_FONT=0` | The menu, the counter and the start-up screen in the included Libertinus, not the game's own font |

The presses are `<seconds>:<button>[+<button>...][/<ms>]`, separated by commas, for example `2:A,5.5:LUP/1500,9:ZL+R`.
The buttons are `A B X Y L R ZL ZR PLUS MINUS LS RS LEFT UP RIGHT DOWN`, and the sticks pushed to their edge `LUP
LDOWN LLEFT LRIGHT RUP RDOWN RLEFT RRIGHT`. A press is held for 200 ms unless `/<ms>` says otherwise. The names are
case-sensitive, and an unknown one is ignored.

### Pictures and frames

| Switch | What it does |
|---|---|
| `RX_VK_SHOTS=<dir>` | A PNG every `RX_VK_SHOT_EVERY` presents (300) as `vk_<present>.png`, at `RX_VK_SHOT_W` x `RX_VK_SHOT_H` (1280x720) |
| `RX_VK_FLASH=1` | The flash detector, with `RX_VK_SHOTS` and `RX_VK_SHOT_EVERY=1`: writes the frames around a flash and dumps the next one |
| `RX_VK_FRAMELOG=<file>` | Every frame shown, as pictures at 320x180, while `<file>.go` exists; `python -I scripts/framelog.py <file>` finds the flashes. Not `RX_VK_FRAME_LOG` (with the underscore: frame times, below) |
| `RX_VK_DUMP=<present>[+<count>][,...]` | Every step of the frame after that present as PNGs, in `dump_<present>` under `RX_VK_DUMP_DIR` (or `RX_VK_SHOTS`, or the current folder); `RX_VK_DUMP_STEPS=<lo>-<hi>` and `RX_VK_DUMP_PROGS=<key>,...` narrow it |
| `RX_VK_PASS_TRACE=1` | The passes of the last frames, logged at a flash or a dark frame (`RX_VK_PASS_TRACE_AT=<n>`: every n-th present) |
| `RX_VK_PROBE=1` or `2` | The centre texel after every pass: `probe_<frame>.txt` at a flash (1), or `probe_all_<n>.txt` at speed (2); `python -I scripts/probe.py` reads them |
| `RX_VK_DUMP_FAILED=<dir>` | The native code of every program that does not translate, for `nx2/lower_test.py` (game data: keep it local) |

### Time and profiles

| Switch | What it does |
|---|---|
| `HOS_PROFILE=<seconds>` | Every interval, the busiest threads in the log; it also starts the samplers below |
| `HOS_SAMPLE=<thread>` + `HOS_SAMPLE_FILE=<file>` | One thread's stacks, sampled every millisecond (needs `HOS_PROFILE`); `python -I scripts/prof.py <file>` reads them |
| `HOS_SAMPLE_ALL=<file>` | Every game and renderer thread every 2 ms while `<file>.go` exists (needs `HOS_PROFILE`); `scripts/crit.py`, `scripts/vkprof.py`, `scripts/hotlist.py` and `scripts/cpu_report.py` (the [CPU profile](cpu-profile.md): CPU time by category, thread and function, and what limits the frame) read it (on Linux with `RX_MAPS=<game folder>`: they name frames from its `hookshot` and `hookshot_vk.so`) |
| `HOS_CHAIN=<file>` | While `<file>.go` exists, every wait of a game thread that blocks, every signal that can end one, every sleep and every present, with the time, the thread, the object and the caller; written to `<file>` when `<file>.go` goes away (about 2 million records at most). `python -I scripts/chain.py <file> [<HOS_SAMPLE_ALL file>]` walks each frame back from its present (the thread that ran, the wait that stopped it, the thread whose signal ended it) and prints the frame's critical path by thread: running, wake-ups, sleeps, and, with a sample file of the same run, the functions on it |
| `HOS_HLE_STATS=1` | About every 5 s, calls of each HLE function and host call (`hle:` lines, most called first), with measured monotonic clock boundaries. `scripts/cpu_report.py --calls` joins complete blocks to native frames in that clock span; per-call occupancy estimates require the same run's timestamped stacks. Off by default; take performance numbers from a run without counting |
| `HOS_THREAD_TIMES=<seconds>` | Every interval, the threads that used over 5% of a core |
| `HOS_FPS_LOG` (set), `HOS_PRESENT_TRACE=<csv>` | The game's frame limiter, VRR flags and nominal rate every 300 presents, and the game clock's speed (1.000: real time); a CSV line per game present with host-clock times before queue flushing, around the renderer call, after virtual-core reacquisition, and at the return counter. These are CPU wall times, not GPU completion or scanout; async rendering measures the enqueue call. Unset/empty trace adds no clock reads |
| `HOS_FPS_MAX=30` or `<61-1000>` | A frame rate cap for one run, over the Game frame rate cap row (1000 is Uncapped's) |
| `HOS_MOVIE_PACE=0` | Leave the pacing at the Game frame rate cap row (60 or 30) while a movie plays, instead of raising it to 240 for the movie's pictures |
| `RX_VK_FRAME_LOG=<csv>` | A CSV line per present: frame, seconds, frame time in ms. Also works with `HOS_HEADLESS`: CPU intervals after submitting the offscreen copy, without swapchain/display waits. Not `RX_VK_FRAMELOG` (no underscore: pictures, above) |
| `RX_VK_PRESENTS_AHEAD=<1-8>` | How many presents the game may hand over ahead of the render thread (`RX_VK_ASYNC=1`; 1 by default). Not `RX_VK_PRESENT_AHEAD` |
| `RX_VK_PRESENT_AHEAD=0` | Frame generation's copies into the window submitted each at its own time, as before #1702 (default on: a set's copies go to the GPU together). Not `RX_VK_PRESENTS_AHEAD` |
| `RX_VK_PASS_TIMING=1` or `2` | GPU time per pass (2: per program too), logged every 600 presents |
| `RX_VK_SLOW_LOG=<ms>` | The replay's times for each frame that took longer than this |
| `RX_VK_LOG_PROGRAM_LIFE=1` | The programs' life: a log line at each program the game makes (its shaders set) or frees and at each program's first draw or dispatch (what it needed: a program read from the cache, a translation, a pipeline from a cache or compiled, left out, and how long after its create call), and a `program census` line every 600 presents |
| `RX_VK_PRESENT_THREAD=1` | On (off by default: the game's thread presents). Where the device has a second queue that can present to the window (a second queue of the replay's family, else a compute family's): each present is made on a thread and queue of its own once the frame's copy into the window is done, and the game's thread goes on at once (with NVIDIA's driver on Wayland the present otherwise waits ~2.4 ms a frame for the GPU there). One present out at most; under V-Sync On (FIFO) with `VK_KHR_present_wait`, until it is shown. The log has the fence's and the present's time (and how long after it the frame was shown) every 600 presents |
| `HOS_PGO=<file>` | In a run of an `RX_PGO=gen` build: `<file>.reset` clears the counters, `<file>.dump` writes them to `<file>`. It is read before `hookshot_env.txt`, so set it in the environment |
| `RX_VK_PGO_FILE=<file>` | The same in a run of an `RX_VK_PGO=gen` renderer, for the renderer's own counters ([the renderer's play profile](#the-renderers-play-profile)); set it in the environment too |

The periodic `GPU-bound wait` log value is **recording-slot reuse only** (the line says so), not a bottleneck test.
`host time in GPU resource waits` separately sums host-clock time inside nonzero-timeout Vulkan fence and timeline
wait calls and queue/device-idle calls routed through the replay. This covers capture late-page waits, buffer and
texture readbacks, recording/display-copy fences and sparse-binding waits. Recorder draining happens before the timer;
nested waits count once and zero-timeout polls are omitted. Reflex's latency semaphore is pacing and is excluded.
The sum is **thread wall time**, which can overlap across threads and exceed a frame; do not add the old slot value or
containing replay/capture/texture phases to it. It excludes the startup cooperative-matrix probe, CPU locks and
publication waits, display acquisition/pacing, and blocking hidden inside driver submit/present calls. A low value
still does not prove the GPU is idle: read GPU timestamps, present intervals and CPU profiles together.
Query-result reads with `VK_QUERY_RESULT_WAIT_BIT` count separately as `queries` (the optional draw-statistics read);
ordinary timestamp-result polls without that bit do not count.

The game-free `test_gpu_wait_stats`, `test_recorder_sync` and `test_recorder_own_idle` checks use the real accounting
header. The recorder's source check ignores comments, but still requires every boundary macro to drain first and
call its own driver function, including sparse-memory release.

### The GPU

Pipeline-cache file ownership, temporary storage and failure behavior are described in
[pipeline cache storage](pipeline-cache-storage.md).

| Switch | What it does |
|---|---|
| `RX_VK_VALIDATION` (set) | The Khronos validation layer |
| `RX_VK_CHECKPOINTS=0` | No NVIDIA checkpoints (on by default; after a lost device they name the draw it stopped at) |
| `RX_VK_HANG_DUMP=<file>` | Every thread's stack to the file once no frame was shown for `RX_VK_HANG_SEC` seconds (20) (Windows) |
| `RX_VK_GPU_HANG_SEC=<s>` | Work sent to the GPU that finishes nothing for this long (30) is a hang: the lost-device report, then the game ends. `0`: the stall is logged and the game waits |
| `RX_VK_NOOPT` (set) | Pipelines built without the driver's optimisations |
| `RX_VK_SKIP_PROG=<prefix>,...` | Leaves out the draws of these programs (default `53509044401d78d2`; `-` leaves out none) |
| `RX_VK_PIPE_SKIP=0`, `1` or `2` | Default/`1`: wait for shader translation (with `RX_VK_TRANSLATE_WAIT_MS` set, a draw whose targets hold an earlier frame waits at most that long a frame in all, then is left out). A pending pipeline can be skipped only for attachment-only draws whose targets hold an earlier frame. `0`: wait for every translation and pipeline. `2`: restore the old skip for untranslated draws with persistent targets at once; this can omit writes before the shader is known. Translated memory-writing draws still wait |
| `RX_VK_TRANSLATE_WAIT_MS=<ms>` | Off by default: each shader still translating is waited out, as #1573 does. Set (`8` is the tested value): how long, in all, each recording thread waits in a frame for shaders still translating before leaving their draws on earlier frames' targets out. For first-visit pauses of 0.2-3 s on a cold or changed program bank; a translation slower than the bound can lose a memory write, since its writes are not known before it is translated. A negative value is off; anything but a number is ignored with a warning; the value in use is logged. Compute dispatches still wait their translation out |
| `RX_VK_JOB_STEAL=0` | A draw that must wait for its shader translation or pipeline build waits for a worker to reach it, behind every job queued before it, as before. By default a job it waits for that no worker has started yet is taken out of the queue and run on the waiting thread (it runs once either way; one a worker has started is waited for). The log's `vk: a draw's ... ran where it waited` lines (the first 10) say when it happened. Off follows the on/off rule above (`0`, `false`, `off`, `no`) |
| `RX_VK_QUICK_MS=<ms>` | Quick pipelines (ported from Mako, by sharkiix): a draw whose pipeline is still being built by a worker first asks the caches for the full one without compiling (where the driver has `pipelineCreationCacheControl`), else gets one made at once without the driver's optimizer, into no pipeline cache, and is drawn with it until the worker's full build replaces it. Up to this many ms of those a frame on each recording thread for draws that could be left out: a build is not started where the thread's last one would not fit in what is left, but a frame's first always is, so a frame runs past the budget by at most one build. Draws that must wait get one whatever the budget. Off by default on every driver until measured in the game (it is for AMD's proprietary driver, where the optimizer took 5-13 s a pipeline: Mako, Volvagia's intro a black silhouette); `8` is the suggested value (half a 60 fps frame, as `RX_VK_TRANSLATE_WAIT_MS`; Mako's 1000 trades a missing draw for a hitch of up to a second). Where a quick build takes longer than that (tens of ms on AMD's proprietary driver), `8` means about one quick build a frame on each recording thread, each a hitch of that build's length; `0` or unset: off. Not made where the full build is made without the optimizer already (`RX_VK_NOOPT`, AMD's task and mesh programs); while the driver-fault gate has another build of the program in the driver (AMD, often the entry's own full build) none is made then, and a later draw tries again. The gate is per program, and a worker waiting at it takes it back before a later draw's try, so on AMD quick pipelines mostly come for programs no worker has reached yet (a backlog, as in a new area): a draw whose program is already building is left out or waits for the whole build, as without quick pipelines. Not with `RX_VK_PIPE_SKIP=0`, which waits for every pipeline. Deliberately, a full build that fails (or that the driver faults on) leaves the quick pipeline drawn with for the session, where the draw was left out before. Memory: a quick pipeline the full build replaced is not destroyed (nothing tells when the recordings that used it are done on the GPU), so each entry that got one keeps one more pipeline, unoptimised, for the session; the log's quick pipeline lines count them. Quick builds are not counted in the stats' pipelines. Anything but a number is ignored with a warning; the value in use is logged. Untested in the game in Hookshot |
| `RX_VK_PIPE_WAIT_MS=<ms>` | Off by default. Set (Mako uses 100): a draw on earlier frames' targets whose pipeline is still being built, with no quick one, has its build moved to the front of the workers' queue and waits for it at most this long a frame in all, on each recording thread, before it is left out. The build is not run on the waiting thread (one started cannot be stopped, and took seconds on AMD). Pipelines only: shaders still translating follow `RX_VK_TRANSLATE_WAIT_MS`. Anything but a number is ignored with a warning |
| `RX_VK_TEST_SLOW_MS=<ms>` | A test, off by default: each draw's full pipeline build (a worker's) is held this long (at most 60000) inside the driver-fault gate, as AMD's optimizer on a cold cache, to watch quick pipelines and the brief wait on any GPU. The gate holds one build of a program at a time only on AMD: elsewhere add `RX_VK_DRIVER_FAULTS=2` to see a quick build meet the program's full build in the driver, as on AMD. Anything but a number is ignored with a warning |
| `RX_VK_LOAD=create` or `start` | When programs and their pipelines are loaded from what the preparation kept. Without the switch: `create` where the GPU has mesh shaders, `start` where it draws mesh programs through the compute fallback; and `start` on an install where first uses waited for loads for over 2 s in one session, or for over 500 ms in two sessions running (only waits longer than 20 ms count; one such session alone leaves `hookshot_load_start.txt.waited`, which the next start takes away), or where the loads found over 500 listed pipelines in no cache (draws left out for pipelines no list names are not counted: they happen in `start` too) (`hookshot_load_start.txt` in the game folder's `cache`, removed by a preparation or by hand). The first start of the game after a preparation builds every noted pipeline and every kept compute program's pipeline into the pipeline cache once, before the title, and lets each go; the preparation's own process does the same to fill the driver's cache but leaves no marker, since its pipeline cache does not answer the game's process on NVIDIA. With the compressed program cache (`RX_VK_PROGRAM_CACHE_ZSTD=1`) always `start`, whatever this switch says: its records are only read whole. The log says which and why. `start`: every listed one before the title, as before. `create`: none by a list; each program's kept variants and noted pipelines are loaded by a background job when the game makes the program (from the caches only: never a translation or a compile there), and a first draw that comes before its job ran runs it itself. The first start after a preparation builds each noted pipeline into the pipeline cache once and lets it go (before the title, with the precompile's progress), and only then loads at create. `RX_VK_LOAD_WORKERS=<n>` (2): the jobs that run at once (0, to measure: none, a draw loads what it needs itself); `RX_VK_LOAD_UNLISTED=1`: programs no list has a pipeline of are loaded at their create call too (otherwise at their first draw, as with `start`); `RX_VK_LOAD_WORKERS_LOADING=<n>`: while no frame was presented for 100 ms (the same unless set) |
| `RX_VK_MODULE_IDS=0` | Shader module identifiers off (`VK_EXT_shader_module_identifier`). With them a compute program's module is learned by its identifier when it is first made from its code (`hookshot_module_ids.bin`, for one program cache and one driver), and in a later run the load at the program's create call makes that variant's pipeline from the caches without reading its code or making a module; where no cache has the pipeline the code is read as before |
| `RX_VK_PRECOMPILE=0` or `1` | The shader preparation before the title off or on (on in builds made with `RX_RELEASE=1`, as the builders' are) |
| `RX_VK_MESH_SHADERS=native` or `compute` | How task and mesh programs run, read as the device is made (`gpu/vk/mesh_mode.h`; it was the Graphics tab's Mesh shaders row). Unset or any other value: Automatic, the GPU's own mesh shaders where it has them; the compute fallback where it has not, on Linux AMD (RADV, issue 2245), and for the task + mesh draws of the GPUs `RX_VK_TASK_MESH_FALLBACK`'s default names. `native`: the GPU's own for every draw (a GPU without them still gets the fallback). `compute`: the fallback on every GPU. `RX_VK_FORCE_MESH_FALLBACK=1` is `compute` over it. The log's `vk: mesh shaders:` line says which and where it came from. A change translates the programs again, as the program cache's stamp holds the GPU's mesh path (and every `RX_VK_MESH*` switch's text). The old `mesh_shaders=` line in `hookshot_options.ini` is no longer read: the log names it, and the menu's next save drops it |
| `RX_VK_PIPELINE_TRACE=1` | Log graphics program keys before pipeline construction and compute program keys before background compilation, so a long-running build can be identified before it returns. Off by default. |
| `RX_VK_TASK_MESH_FALLBACK=0` or `1` | Task + mesh draws through the compute fallback (1) or the driver's mesh shaders (0). By default the fallback is on for AMD's Navi 21 (RX 6800, 6800 XT, 6900 XT, 6950 XT, Radeon Pro W6800, W6900X, V620), Navi 22 (RX 6700, 6700 XT, 6750 XT, 6700M to 6850M XT) and Navi 23 (RX 6600, 6600 XT, 6650 XT, their mobile models, Radeon Pro W6600, W6600M) on Windows: each chip's every ID that pci-ids or amdgpu.ids name a card for, where the native path timed out the driver ([Hookshot #996](https://github.com/NotRexed/Hookshot/issues/996)). Navi 24 (RX 6300, 6400, 6500 XT, Radeon Pro W6300 to W6500M) is not on by default: no report yet; `RX_VK_TASK_MESH_FALLBACK=1` to try. The device IDs: `mesh_rule::task_mesh_fallback_default` in `gpu/vk/mesh_mode.h` |
| `RX_VK_FALLBACK_LOADED=0` | Where mesh programs are drawn through the compute fallback: the start makes the compute pipelines only of the fallback programs it reads in for that, as before, and not of those read in earlier in the same start (the state lists' programs, in the start that prepares the shaders), whose first draws are then left out until their pipelines are compiled in play |
| `RX_VK_FALLBACK_PHI_LIMIT=<n>` | RADV on GCN 1-4 cards (RX 400/500, R9 2xx/3xx/Fury): a mesh fallback compute stage with more than this many phis is not built and its draws are left out (default 50000; `0`: no limit; anything but decimal digits is ignored with a warning; the limit in use is logged). Its compiler did not finish the two biggest mesh programs (80,000 phis) and grew past 25 GB (`gpu/vk/fallback_size_policy.h`) |
| `RX_VK_DRIVER_FAULTS=0` or `2` | 0: programs the driver's compiler faulted on are asked of it again. By default they are left out for the rest of the run and, through `hookshot_driver_faults.txt` in the game folder's `cache`, on later runs with the same driver, and on AMD one program's builds go to the driver one at a time (issue 996). 2: one at a time on every driver |
| `RX_VK_NO_INTEL_SERIAL=1` | On Windows, Intel's compiler (UHD and Arc) is entered by one thread, and a fault creating a shader or pipeline leaves that one out. This builds them side by side, as on every other GPU and on Intel elsewhere, for the run: the Debug tab's Intel shader compiles row shows it and is not changed (`hookshot_options.ini` keeps the row's own, `intel_shader_compiles=`). The log says which |
| `RX_VK_COMPUTE_QUICK_FIRST=0` | With Intel's compute pipelines built one at a time on Windows (Intel's own driver; not on Linux, where Mesa's ANV ignores the optimizer flag), a dispatch's pipeline is built first without the driver's optimizer (seconds where the optimizer took up to minutes on a UHD Graphics) and the optimized one is bound once it is built behind the others. This builds the optimized one only, as before, for the run: the Debug tab's Intel quick compute builds row shows it (`hookshot_options.ini` keeps the row's own, `intel_quick_compute_builds=`) |
| `HOS_VIDEO_NO_CATCHUP=1` | A movie does not skip pictures that are behind its sound. By default, pictures already decoded are skipped, and a decoder that keeps up decodes ahead to skip a late one (at most one picture's time in a frame). A decoder slower than the gap between pictures, timed over its recent pictures, shows the late one instead of decoding the next in order to drop it |
| `HOS_VIDEO_ACQUIRE_TRIES=1` | The movie waits for a picture by 50 sleeps of 2 ms, as before. By default the wait is by the clock: about 100 ms for each picture, the first too, and none after a Stop or Flush with nothing submitted since |
| `RX_VK_GM_LOOP=0` or `1` | The storage buffer tables of a global memory access searched in a loop (1) or written out entry by entry (0). On by default on Intel, whose compiler did not finish the culling programs with the entries written out |
| `RX_VK_IMPORT_CHUNK_MB=<MB>` | The size game memory is imported for the GPU by, and so a pool's VRAM mirror's least size (default 4 where the pools have VRAM mirrors, discrete cards; 16 without them, and before). Larger: fewer imports, more of each mirror outside the game's pools |
| `RX_VK_NO_PROGRAM_CACHE` (set) | Neither reads nor writes `hookshot_programs.bin` and the pipeline notes |
| `RX_VK_PCACHE_LOAD=0` | Starts with an empty in-memory pipeline cache; neither the base cache nor rolling play-cache files are loaded or saved. Required pipelines are still made, and the driver can still use its own disk cache: this is not a driver-cold test. Old temporary-file cleanup still runs. Unset or `1`: load and save as usual |
| `RX_VK_PCACHE_STAMP=0` | Keeps `hookshot_vk_pipelines.bin` and its play files whatever translator made them. By default they are dropped at a start when `hookshot_vk_pipelines.stamp` says another translator made them (as the program cache starts over), since their pipelines can no longer be hit |
| `RX_VK_PCACHE_FETCH_LOCK=0` or `1` | A pipeline cache's data fetched (`vkGetPipelineCacheData`, the saves) only while no pipeline is being built with a cache, and a build waiting for a fetch: Intel's Windows driver crashes in `igvk64.dll` when the two overlap. The lock is held around `vkGetPipelineCacheData` alone, never across a file's write nor a wait for the builds. The background save waits up to 2 s for a moment with no build, else is put off to the next one (after 3 in a row the next one queues, up to 10 s); the forced saves wait; the window's close waits up to 10 s for the builds under way, then leaves the play file unwritten rather than hang (and up to 10 s more for each of its two fetches). With it off every wait is as before: the close waits for the builds. Default 1 with Intel's Windows driver, 0 elsewhere |
| `RX_VK_FETCH_BOUNDS=0` or `1`, `RX_VK_NAN_BITS=0` or `1`, `RX_VK_GATHER_OFFSET=0` or `1` | The translation's vendor and driver workarounds (`gpu/vk/vk_vendor.h`) off or on for one run, whatever the GPU; the log's `vk: vendor fixes` line says which are on. By default: out-of-bounds texel fetches answered with 0 on every GPU but NVIDIA's; NaN tests on a value's bits on AMD's older Windows shader compiler (RX 5000 and RX 6000); gathers nudged by 1/512 of a texel on AMD's and Intel's drivers. They change the shaders' translation (one preparation of the shaders) |
| `RX_VK_NATIVE_FADD=0` or `1` | The shaders' float adds to storage buffers: 1 the GPU's own atomic where it has `VK_EXT_shader_atomic_float`'s `shaderBufferFloat32AtomicAdd`, 0 a compare-exchange loop on every GPU, the extension not enabled. By default the GPU's own on NVIDIA's driver, on Intel's Windows driver from 101.9175 with the Meteor Lake and Arrow Lake iGPUs (PCI device 0x7Dxx), and from 101.9154 with the Arc B-series cards (Battlemage, 0xE2xx; the loop lost the device on entering Kokiri Forest on a B580): what it was tried on in the game. Changing it, or the first start after an update that changes it, empties the whole program cache and `mkw_vk_pipelines.bin` (the translator's stamp holds it): everything is translated and prepared again, one long start on an iGPU. Elsewhere (AMD, Mesa's Intel driver, Intel's other GPUs and older drivers on Windows) the loop until runs there show the native add works (the log's `vk: float atomic add on storage buffers:` line says which, and why). The loop gives up after 1024 attempts, and an add that gives up is lost |
| `RX_VK_UB_WORD_HASH=0` | A/B test: use the old low-address-bit index for cached uniform words (default: mix upper address bits to reduce aligned-buffer collisions) |
| `RX_VK_SPLIT_UBFLUSH=0` or `1` | A submission cut into parts (`RX_VK_CHUNK`): 1 each part writes the uniform updates recorded so far into game memory at its end, as before; 0 only the last part writes them all, as an unsplit submission does. Default 0 on Intel without VRAM mirrors, the iGPUs (white blocks and flashes in Kokiri Forest otherwise, https://github.com/NotRexed/Hookshot/issues/999), 1 elsewhere, Arc discrete included (mirrors on), where it was not measured |
| `RX_VK_MIRROR=0`, `RX_VK_MIRROR_MB=<MB>` | No VRAM mirrors of the game's memory pools (on for discrete cards); their budget (by default a quarter of the card, at most 3 GB, an eighth on cards of 4.5 GB or less, and a Quality preset's never more than that: `gpu/vk/mirror_budget.h`) |
| `RX_VK_MIRROR_SPARSE=0` | Whole VRAM mirrors, as before. By default they are sparse (`gpu/vk/sparse_mirror.h`): memory only for a mirror's 64 KB blocks that one of the game's pools is in, and blocks the game's pools have left given back. A block with none is reached in guest memory (the page table, and a binding until the next submission gives it memory); a binding whose blocks with none are all outside every game pool (a guessed size run past a pool's end) is served from the mirror. The log's `vk: sparse mirrors:` line counts them every 1200 submissions. Needs strict sparse residency for buffers: off on its own where the device lacks it, and the log says so (`vk: sparse VRAM mirrors (RX_VK_MIRROR_SPARSE): not on this device`). Off follows the on/off rule above (`0`, `false`, `off`, `no`) |
| `RX_VK_SPARSE_WAIT=1` | Each bind of the sparse mirrors' blocks waited for on the CPU, as before. By default the binds signal a timeline semaphore that the submissions of recordings wait for on the GPU (`sparse_bind_infos`, `gpu/vk/vk_memory.cpp`); waited for on the CPU, an area change's longest frame was 1.2 s against 0.9 |
| `RX_VK_MIRROR_TRIM=0` | An import whose mirror is mostly outside every game pool is no longer made again smaller (it is while VRAM is over 80% full: the previous scene's large pools otherwise keep their mirrors for the session; `gpu/vk/mirror_trim.h`) |
| `RX_VK_CAPTURE_KEEP_OLD=1`, `RX_VK_CAP_BLOCKS=1` | A/B switches for capture buffers: keep the former size classes and 64 MB reuse limit; allocate large buffers from shared blocks instead of dedicated memory. Both are off by default and follow the on/off rule above, including `0`, `false`, `off` and `no` |
| `RX_VK_VRAM_MB=<MB>` | The texture cache's budget in MiB, over a preset's and over the default (70% of what the driver allows the process, 45% where that is 8.5 GB or less, at most 6 GB; on an integrated GPU less, by the system's RAM: 1 GB with 8 GB of RAM, 2 GB with 12, 2.5 GB with 16, 3.5 GB with 24, 4 GB with 32, and up to 6 GB above that). What a texture counts: BC and natively sampled ASTC by whole blocks at each mip level, partial blocks included; decoded ASTC by its output format; and never less than the allocator's size (`RX_VK_COUNT_BY_ESTIMATE=1`: the estimate alone). The log prints these MiB as `MB` |
| `RX_VK_AUTO_PRESET=0`, `RX_VK_AUTO_PRESET_VRAM_MB=<MB>` | A first start keeps the defaults instead of starting on the Quality preset Auto (this PC) (`gpu/vk/auto_settings.h`); the card's video memory taken as that many MB, to see what Auto chooses for a smaller or larger card |
| `RX_VK_AUTO_TUNE=0` | Auto (this PC) keeps its Render resolution instead of measuring the GPU in play and stepping it: down while frames are missed with the GPU over 90% busy, up while the cap holds with it under half busy (`auto_settings.h` `tune`) |
| `RX_VK_COMPACT=0`, `RX_VK_COMPACT_PCT=<50..90>`, `RX_VK_COMPACT_MB=<MB>` | VRAM compaction (on by default; `=0` turns it off): the textures that are not render targets in a pool of their own, moved out of sparsely used blocks a few a present so that the blocks go back to the driver, and the texture budget holding the driver's count at that share of what it allows (85) by letting go of the render targets drawn over since and of textures unused for ten seconds, with a reserve five points under it (80) where those unused for thirty seconds go (what the budget's size, the 90% rule and the driver's limit ask for on their own still goes from textures unused for a second, as with it off); the free space that starts a run (512; 192 while the count is over 90%) |
| `RX_VK_EVICT_EXCESS=0` | The 90% rule and the driver's limit cut the texture cache to 7/8 of their budget again, as before; by default they let go of their excess and 64 MB more (`vram_compact::rule_target`), so a cache a few MB over the line does not lose an eighth of its textures |
| `RX_VK_IMAGE_PREALLOC=<blocks>` | Initial 128 MiB image blocks: by default 15% of the real device-local heap, from 2 to 24 blocks (9 on an 8 GiB card); `0` makes them only as needed. They belong to the sampled-texture pool, or the images' pool with `RX_VK_COMPACT=0`, never both. A failed initial allocation retries with no initial blocks. VMA keeps these blocks even when empty: compaction counts only space it can return, and the 97% squeeze waits while only kept texture blocks remain. This avoids charging an unused utility-image pool against the texture budget (#2690); it does not lower the driver's actual memory count in a scene with few textures |
| `RX_VK_TEST_ALLOWED_MB=<MiB>` | Diagnostic upper bound on the allowance reported to the renderer's eviction policy; does not reduce the real heap or emulate OS paging. To model #2690 on a larger card use `7000` with `RX_VK_IMAGE_PREALLOC=9`, since the automatic initial-block count still follows the real heap. Other heap-based defaults, including the mirror budget, also remain real-card defaults unless explicitly overridden |
| `RX_VK_COMMIT_RESERVE_MB=<MB>` | Windows only: the commit the texture cache leaves free before Windows' commit limit (RAM plus the page file; default 2560, `0` turns the rule off). Every VRAM block counts against the process's commit, so below that much headroom (`GlobalMemoryStatusEx`'s `ullAvailPageFile`) the shortfall comes off the textures' budget and eviction and compaction give blocks back; the log says `vk: commit limit near:` at most every 600 frames while it is on. At the limit itself, `hos_commit` and the first-touch handler wait up to 5 s for commit (error 1455) before giving up |
| `RX_VK_IMPORT_HIGH=<percent>`, `RX_VK_IMPORT_LOW=<percent>`, `RX_VK_IMPORT_CAP_MB=<MB>` | The valve that gives idle imports of the game's memory back to the driver (`imports_give_back`, `gpu/vk/vk_memory.cpp`): past HIGH percent (85) of the system heap's budget the imports bound least lately go, down to LOW percent (75); at a 12 GB budget and above the budget is capped at `RX_VK_IMPORT_CAP_MB` (4500; `0`: no cap; setting HIGH or LOW without it uses the full budget). On by default on Linux and on Windows NVIDIA; off by default on Windows AMD and Intel (device losses there, issues 17, 309 and 571), where `RX_VK_IMPORT_HIGH=85` turns it on and its pools wait `RX_VK_RETIRE_SECS` before they are freed. `RX_VK_IMPORT_HIGH=0`: off everywhere. HIGH and LOW also set the retired imports' valve's watermarks |
| `RX_VK_RETIRE_SECS=<s>`, `RX_VK_RETIRE_MIN_SECS=<s>` | Windows AMD and Intel: how long a retired pool's import (and, with the valve above on there, an idle pool given back) is held after it leaves the page table before its memory is freed (30); under pressure past `RX_VK_IMPORT_HIGH` a retired pool waits `RX_VK_RETIRE_MIN_SECS` (2) instead. Not read elsewhere |
| `RX_VK_IMAGE_PREALLOC=<blocks>` | Initial 128 MiB image blocks: by default 15% of the real device-local heap, from 2 to 24 blocks (9 on an 8 GiB card); `0` makes them only as needed. They belong to the sampled-texture pool, or the images' pool with `RX_VK_COMPACT=0`, never both. A failed initial allocation retries with no initial blocks. VMA keeps these blocks even when empty: compaction counts only space it can return, and the 97% squeeze waits while only kept texture blocks remain. This avoids charging an unused utility-image pool against the texture budget ([Hookshot issue 2690](https://github.com/NotRexed/Hookshot/issues/2690)); it does not lower the driver's actual memory count in a scene with few textures |
| `RX_VK_TEST_ALLOWED_MB=<MiB>` | Diagnostic upper bound on the allowance reported to the renderer's eviction policy; does not reduce the real heap or emulate OS paging. To model [Hookshot issue 2690](https://github.com/NotRexed/Hookshot/issues/2690) on a larger card use `7000` with `RX_VK_IMAGE_PREALLOC=9`, since the automatic initial-block count still follows the real heap. Other heap-based defaults, including the mirror budget, also remain real-card defaults unless explicitly overridden |
| `RX_VK_REACTIVE=1`, `RX_VK_REACTIVE_SCALE=<0..64>`, `RX_VK_REACTIVE_MAX=<0..1>`, `RX_VK_REACTIVE_DEBUG=1` | A reactive mask for DLSS and FSR 3.1 only, not FSR 4 (`gpu/vk/vk_reactive.h`, off by default: not yet checked in the game): the scene's colour is copied before its blended draws that test depth without writing it (water, smoke, fire, glows), and where the scene differs from the copy at the upscale the upscaler leans on the current frame, so they smear and ghost less. The mask is the difference times the scale (1), at most the maximum (0.9); the debug switch shows it in the picture in magenta. The mask is the whole difference since the copy, so a blended draw after the last transparent run (a full-screen fade) is in it too. The log's `vk: reactive mask:` lines say whether one is made |
| `RX_VK_NO_DRAWN_WRITEBACK=1` | Small mip-chained textures the GPU drew are not written into guest memory a few frames later (where that memory is still zero, or still their own last write-back), as before: the game's ground garbles after some minutes in one area (its copies of drawn terrain tiles are all zero) |
| `RX_VK_NO_DRAWN_PAGE_MAP=1` | A texture in a virtual pool that the GPU writes before anything uploads it has no page map noted at that write, as before: once the game has finalized it, its next check takes all of its storage for remapped and uploads it whole from guest memory over what the GPU drew (the game's HUD button labels go blank after a zone change) |
| `RX_VK_SKIP_COMPRESSED_OVER_RT=1` | A compressed (ASTC, BC, ETC2/EAC) texture made over storage render targets used is not uploaded, as before: bosses and new enemies all blue the first time |
| `RX_VK_NO_WRITEBACK` (set) | GPU writes to mirrored pools are not copied back to guest memory (an experiment, for A/B) |
| `RX_VK_WB_DIFF=0` | Write-backs copy every byte again instead of only what changed against a VRAM shadow (the diff is on by default; v2.0.9 and v2.0.10 had it off, issue 1161) |
| `RX_VK_MSYNC_PASS=0` | A mirror sync copies the pages a capture holds into the mirror as copy regions again, as before. By default they go through the write-back diff's pass (`shaders/wb_diff.comp`, with the mirror as its own shadow, so a copy): one dispatch a sync, and one for every pool synced before a parallel replay's segments. A capture's buffer is host memory, and its regions, a page or a few each, were read over PCIe one after another (`msync_pass_add` in `gpu/vk/vk_memory.cpp`; the log's `vk: mirror syncs' pass` line every 600 submissions). A GPU without the write-back diff's pipeline (no subgroup votes) keeps the copies |
| `RX_VK_LATE_FIX=0` | A capture whose pages a submission still on the GPU writes back (late pages) waits for that submission and copies them again, as before, instead of the GPU writing the write-backs' words into the capture (`shaders/late_fix.comp`; on by default, off under `RX_VK_PARALLEL` > 1) |
| `RX_VK_SHADOW_FOLLOWS_SYNC=0` | The write-back shadow is filled from guest memory at the write-back, as before, instead of taking the same bytes as the mirror's sync (with it off the diff can write a capture's older words over the CPU's newer ones) |
| `RX_VK_WB_OBSERVE=1` | Opt-in diagnostics for actual mirror/shadow differences in the write-back diff pass. Reports completed chunks and bytes covered by changed chunks every 600 submissions; does not change copy decisions. See [write-back observations](writeback-observations.md) for interpretation and tests |
| `RX_VK_DLSS=0/1/2`, `RX_VK_DLSS_PRESET=<letter>`, `RX_VK_DLSS_LOG=1`, `RX_VK_REFLEX`, `RX_VK_REFLEX_LOG=1`, `RX_VK_REFLEX_SUBMIT_IDS=0`, `RX_VK_ANTILAG=0/1` | NVIDIA DLSS and Reflex, and AMD Anti-Lag 2, for one run (`=-` on Reflex or Anti-Lag 2: its extensions not even enabled): the readme's NVIDIA DLSS section. `RX_VK_DLSS_TRACE=<frames>` logs one frame's large draws every that many frames, `RX_VK_DLSS_DEBUG=motion` shows the motion vectors, `=alternate` the game's own upscale every other frame. `RX_VK_DLSS_HDR=0` keeps DLSS's LDR feature and clamped colour with Native HDR too, for comparisons. `RX_VK_REFLEX_SUBMIT_IDS=0` sends the queue submits without their game frame (`VkLatencySubmissionPresentIdNV`, `gpu/vk/vk_reflex.cpp`), for comparisons: with frame generation, Reflex's "to the GPU's end" timing then goes wrong by hundreds of ms. Actor motion (skinned models' own motion, drawn over the camera's for every temporal upscaler, `gpu/vk/vk_temporal.cpp`): `RX_VK_DLSS_ACTORS=0` turns it off, `RX_VK_DLSS_ACTOR_LIVE=1` copies each pose from the game's memory at the upscale as before rather than reading it at the draw (for an A/B), and `RX_VK_DLSS_ACTOR_REACH=<units a second>` (default 60, over at most 1/30 s: 2 units) is how far a copy of a model may move between two frames and still pair with itself by place |
| `RX_VK_FSR4=0/1/2`, `RX_VK_FSR_DIR=<folder>`, `RX_VK_FSR4_HDR=0` | AMD FSR 4 for one run (0 never, 1 FSR 4, 2 its Native AA), and another folder for AMD's DLLs (not from `hookshot_env.txt`): the readme's AMD FSR 4 section. `RX_VK_FSR4=1/2` and `RX_VK_DLSS=1/2` go over the Upscaler row, a saved FSR 3.1 too where the engine they ask for runs; `RX_VK_FSR=1` goes over them (one upscaler at a time). The DLSS trace and debug switches above (and `RX_VK_DLSS_JITTER=0`) apply to FSR 4 too, and to FSR 3.1 on the chain, on the frame chain they share. `RX_VK_FSR4_HDR=0`: with HDR on Native, FSR 4 keeps its LDR context and the colour clamped at the paper white, as before (by default it takes the game's HDR colour as linear light while the game presents PQ). `RX_VK_FSR4_TUNE=0`: AMD's passes as AMD made them; by default, on AMD GPUs, the network's layers run 64 lanes wide, the weight tables the layers and the postpass copy into per-thread arrays are read from a tail of the library's storage buffer, and the small layers with weight tables are unrolled, built one at a time in the background (`gpu/vk/fsr4_tune.h`, `vk_fsr4.cpp`); checked bit for bit against AMD's output on an RX 6700 XT with Adrenalin 26.10.1 (other drivers: the same maths, not necessarily the same bits). Where the library binds its buffer without the tail, or destroys it, AMD's own shaders are bound for that context and the log says so (`AMD's own shaders in place of the tuned ones`). `RX_VK_FSR4_UNROLL=0`: all of that but the unrolling |
| `RX_VK_FSR3_HDR=0` | With HDR on Native, AMD FSR 3.1 on the chain gets the colour clamped at the paper white, as before, for comparisons (by default it takes the game's HDR colour as linear light while the game presents PQ: the log says `vk: fsr3 on the chain: HDR colour, linear`). Its own inputs (`RX_VK_FSR3_CHAIN=0`) are LDR either way |
| `RX_VK_FSR3_CHAIN=0` | AMD FSR 3.1 upscales with inputs of its own (`vk_fsr3_inputs.cpp`: the camera's motion alone) instead of as an engine of `vk_temporal.cpp`'s chain (the skinned draws' own motion, as DLSS and FSR 4 get it), for comparisons. The log says `vk: fsr3 on the chain: N frames drawn` on the chain, `vk: fsr3 ... with assembled depth and camera motion` without |
| Threaded rendering (the Insert menu's row, `threaded_rendering=` in `hookshot_options.ini`) | The player's one setting for the two threads (`gpu/vk/threaded_rendering.h`): Off (2) is the replay on the game's submitting thread; On (1) is the render thread, exactly as `RX_VK_ASYNC=1` below, and the recorder thread, as `RX_VK_RECORDER=thread`, together; Auto (0, the default) picks On where the process may run on 12 or more logical processors (its affinity mask, `usable_cpus`, not the PC's count) and Off with fewer, and Off where the mode is not safe: `RX_VK_CAPTURE=0`, `RX_VK_LOW_SPEC`, `RX_VK_DUMP`, `RX_VK_BENCH`, or buffer pools with no mirrors (`RX_VK_MIRROR=0`, an integrated GPU, GCN). One constant, `threaded::kAutoEnabled` in that header, is the gate over Auto's rule: it is `true`, and turning it to `false` makes Auto Off on every PC again, with nothing else to change. Still open with it on: GPU-written tables over 16 KiB that the game reads back without waiting (Hyrule Field's culling tables) can be a frame old behind the game. Not yet in Auto's rule: a pool refused its mirror, or over the mirrors' budget, while the game runs (it needs a fall-back at run time, which is not written). The row is read once, at the start (`menu_threaded_rendering`, logged as `vk: Threaded rendering: ...`): a change applies at the next start. The switches stay over it, for testing: `RX_VK_ASYNC` set chooses the replay's thread and `RX_VK_RECORDER` set the recorder, each for its own half, whatever the row says; unset, the row decides. The host learns the replay's half from the renderer (`HOS_OPT_RENDER_THREAD`, `host/nvn_native.c`'s present poll), not from `RX_VK_ASYNC` |
| `RX_VK_ASYNC=1` | The render thread (off by default; the Threaded rendering row's On turns it on too, with this unset; any value but `0` turns it on, and there is no `=2` any more: the older replay thread that `=1` used to mean, on which the game still waited for the replay at every frame, is removed): the replay on a thread of its own instead of the game's submitting thread, with the game let run one frame ahead of it, so a frame costs the larger of the game's work and the replay's instead of their sum. The game's present poll is answered at once and the present waits for room instead. It replays whole frames: the present is handed over after the main thread's signal for its frame, and the render thread starts a frame's jobs when that present has been handed over, so it never replays a frame the game is still writing (that drew the lit scene black for one frame at some of the moments it caught up with the game); what a game thread waits for (a queue finish, a texture read back, a fence of the frame in the making) goes on at once, and so does a job with no present behind it after `RX_VK_FRAME_GATE_MS` (1000); the log's `vk: the render thread's whole frames (600 presents)` line counts those (`RX_VK_FRAME_GATE=0`, a test: jobs replayed as they come, as before; `RX_VK_PRESENT_AFTER_SIGNAL=0`/`1`, a test: the present's part alone). Readbacks of small ranges (meters and counters, up to 16 KiB) wait for the write-backs recorded so far, not for the queue, as do larger ones: a read the game makes without waiting for the GPU gets the latest result that has landed, as on the console (`RX_VK_READBACK_EXACT=1`, a test: every range is read after everything handed over before the read is replayed; `=2`: only the ranges over 16 KiB; `RX_VK_LOG_READBACK_RANGES=2` logs the ranges, largest first, with how many frames old what each read got is); nothing but a present or `RX_VK_HOLD_MS` forces a stopped queue; occlusion queries stay on. GPU results the game reads without waiting for the GPU reach it up to a frame later than with the default; for the large GPU-written tables (over 16 KiB: Hyrule Field's culling tables) whether that is what the console gives is an open TODO, and this must not become the default until it is closed. The log's `vk: the render thread (600 presents ...)` line says what its jobs took and what the game waited for. `HOS_PRESENT_NOWAIT=0` with it: the poll as before (the game then waits for each frame's GPU work) |
| `RX_VK_REPLAY_PIN=0` | A/B switch: a thread that replays a submission (the default, `RX_VK_ASYNC` off) stays on its own processors instead of moving to the core kept for the replay (`HOS_RESERVE_CORE`) while it replays, on Windows and Linux. The log says `vk: replays run on logical processors ...` when the move is on |
| `RX_VK_RECORDER_OWN` (set), `RX_VK_RECORDER_IDLE=wait` | With `RX_VK_RECORDER=thread` (the recorder thread, `gpu/vk/vk_recorder.h`): a recording slot's own command pool and descriptor pool resets at its reuse made without waiting for the recorder; and the recorder's idle a 20 us spin, then a wait on its ring (`WaitOnAddress`, a futex on Linux) instead of the adaptive spin. Both off by default; each named in the recorder's start line |
| `RX_VK_DESC_REUSE=0/1/never` | Over the Descriptor reuse row for a run, with its values: 0 its Off (a draw keeps the descriptors of the draw before it while no registration at all was made), 1 its On (through registrations of slots it does not sample, the default); `never`: every draw builds its own |
| `RX_VK_DESCRIPTOR_SETS=1` | Use pooled descriptor sets for game programs, including those that fit the push-descriptor limit. Restart to change it; changing layouts can require fresh driver pipeline compilation. Off by default; for comparing CPU descriptor-binding costs on a driver. Renderer helper passes retain their push descriptors. Sets use the existing per-recording pools, recycled after their GPU work completes; this can require more pool storage than pushing descriptors. |
| `RX_VK_NAN_GUARD=0` | No NaN guard on float colour outputs (on by default on every card: one NaN texel spreads through bloom into glowing blocks) |
| `RX_VK_FIFO=1`, `RX_VK_IMMEDIATE=1` | V-Sync On (FIFO) or Off (immediate) for one run, over the menu's V-Sync row |
| `HOS_VCORES=0`, `HOS_CORES=<1..8>`, `HOS_RESERVE_CORE=0` | No virtual cores; the cores the game sees (6); no core kept for the replay (one is, with 8 or more logical processors) |

### Building

A build reads and writes tens of gigabytes. When the game folder or the build files folder is on a hard drive,
`make_game.py` warns that an SSD is much faster and builds anyway (the builders ask before they start).

| Switch | What it does |
|---|---|
| `HOOKSHOT_JOBS=<n>` (or `--jobs`) | Compilers at once (default one per CPU thread, each started when the free memory holds it; `HOOKSHOT_MEM_GATE=0` turns that check off) |
| `HOOKSHOT_LIFT_JOBS=<n>` | Processes that write the lifted C and make its edits (default the cores, at most 16, one per 2 GB free; on Windows, where they start as new processes, one per 0.75 GB free) |
| `HOOKSHOT_KEEP_BUILD_FILES=1` | Keeps the lifted C and the objects after a build, so that the next one compiles only what changed |
| `HOOKSHOT_PROMPT_JOBS=<n>` | Processes that draw the button prompts' faces (default one a face, at most the CPU threads and one per GB free) |
| `HOOKSHOT_OBJ_STORE=<dir>` | One store of compiled objects for every work folder (`nx2/object_store.py`): a build in a new `--work` folder compiles only what no other folder built with the same compiler and headers. Hard links (a copy across drives), so keep it on the work folders' drive. Off when unset, empty or `0`; `scripts/dev_packs.py` turns it on (below) |
| `HOOKSHOT_LIFT_STORE=<dir>` | Translations kept for every work folder (`nx2/lift_store.py`): after a lift, its C, image, chunk boundaries and hot pieces are saved under the lift key without the work and game folders' paths (`make_game.py lift_share_key`), and a new `--work` folder whose key matches copies them (copy-on-write clones on btrfs and XFS) instead of translating the game. Never hard links. `HOOKSHOT_LIFT_STORE_KEEP=<n>` keeps the newest n (3). Off when unset, empty or `0`; `scripts/dev_packs.py` turns it on |
| `HOOKSHOT_OBJ_STORE_GB=<n>`, `HOOKSHOT_OBJ_STORE_DAYS=<n>` | The store's own clean-up after each `make_game.py` build: an object no work folder holds goes once nobody used it for 14 days, and the oldest first while those come to more than 8 GB. One a work folder holds stays: it costs no space of its own |
| `HOOKSHOT_THIRD_PARTY_OBJECTS=0` or `=<dir>` | The third-party code's objects a release ships compiled (`nx2/third_party_objects.py`: Eden's shader recompiler, sirit, Dear ImGui, zstd, libopus, and on Windows suyu's code under the WebGPU renderer), which a build copies into its object cache instead of compiling them. Each is taken only by its object key, which holds its source, flags and headers and, for these objects, the compiler's version and what the system headers, the C++ library and the Vulkan, SPIR-V and VMA headers preprocess to: anything else (another compiler, a checkout's edit, a CPU below `x86-64-v3`) compiles as before. Default: the tools' own `third_party_objects/` (the AppImage's and the Windows zip's; a checkout has none). `0` turns it off; a folder uses that set (`python -I nx2/third_party_objects.py make <dir>` makes one with this PC's tools). A player's dry run says how many of them the set holds (`scripts/linux/check_player_build.sh` requires all) |
| `HOOKSHOT_FORCE_PROMPTS=1` | Regenerates button-prompt assets even when unchanged. Unset, empty, `0`, `false`, `off` and `no` (case-insensitive, surrounding spaces ignored) are off |
| `HOOKSHOT_MINGW=<dir>`, `HOOKSHOT_MSVC=1` | The llvm-mingw to build with; the Visual Studio route instead |
| `RX_CPM=<dir>`, `WGPU_NATIVE=<dir>` | The CPM cache (Vulkan and SPIR-V headers, VMA, FFmpeg) and wgpu-native |
| `RX_TARGET=windows` | On Linux: the Windows game, with llvm-mingw's Linux build in `HOOKSHOT_MINGW` |
| `RX_RELEASE=1` | A release build (`make_game.py` sets it): no console window, the shaders prepared before the title |
| `RX_PGO=gen` or `RX_PGO=<file>` | The hot code counting its branches for a profile, or built with one (`make_game.py` passes the shipped profile) |
| `RX_VK_PGO=gen` or `RX_VK_PGO=<file>` | The Vulkan back end's own files counting their branches for a profile, or built with one (`make_game.py` passes `work/hookshot_vk.profdata` when it is there, else the committed `assets/profiles/hookshot_vk.profdata.xz`; a compiler that can't read it builds without): [the renderer's play profile](#the-renderers-play-profile) |
| `RX_HOT_OPT=<level>` | The hot pieces' optimisation level (default `O2`); `O1` for [a faster build for development](#a-faster-build-for-development) |
| `HOOKSHOT_SPLIT_GIANTS=<KB>` | Compiles each lifted function of 256 KB of C or more as pieces of about `<KB>` (8 is the measured best) cut at its labels, with a dispatch loop under its own name (`nx2/split_giants.py`). Measured on a Ryzen 5 5600, 16 GB: the 15 giant-function chunks compile 2.6x faster one at a time (their largest compile's peak memory 2,263 -> 586 MB; a single 2-6 MB function's peak falls to about a fifth), and a whole first build's compile (`--jobs 4`) takes ~21% less time, with ~6% less summed compiler memory. The lifted files are not changed (`build.py` compiles a copy). On by default at 8; `0` turns it off |
| `RX_MARCH=<level>` | The CPU level to build for. Default: `native` (this PC's own CPU) on a CPU with `x86-64-v3`'s features, else `x86-64-v2`, else `x86-64`; `x86-64-v3` if clang can't say what `native` is here. With `native` the CPU clang finds goes into the object keys (`-DHOOKSHOT_NATIVE_CPU=`, in the build log), so another CPU compiles again ([issue 182](https://git.notrexed.dev/Hookshot/Hookshot/issues/182)). `RX_MARCH=x86-64-v3` builds as before. `native` also tunes for this CPU (the Steam Deck's Zen 2 too), so there is no tuning switch. The third-party code is built for `x86-64-v3` on every CPU that has it, not `native`, so the objects a release ships fit (`HOOKSHOT_THIRD_PARTY_OBJECTS`); with an `RX_MARCH`, or below v3, it is built as the rest |
| `RX_VK_ONLY=1` | Only the Vulkan back end, with no lift |
| `OOT_WORK`, `OOT_DUMP`, `OOT_EXEFS`, `OOT_ROMFS` | The work folder and the dump's folders, over `work/dump.txt` |
| `HOS_VIDEO_INPUT_LIMIT=<n>` | How many submissions a video decoder holds before it refuses the next, as the console's does when it is full (default 16); `0`: none, every submission taken at once, as before. `HOS_VIDEO_LOG=1` adds the time between a movie's pictures to its line in the log |
| `RX_VK_JOB_STEAL=0` | A draw that must wait for its shader translation or pipeline build waits for a worker to reach it, behind every job queued before it, as before. By default a job it waits for that no worker has started yet is taken out of the queue and run on the waiting thread (it runs once either way; one a worker has started is waited for). The log's `vk: a draw's ... ran where it waited` lines (the first 10) say when it happened. Off follows the on/off rule above (`0`, `false`, `off`, `no`) |
| `RX_VK_QUICK_MS=<ms>` | Quick pipelines (ported from Mako, by sharkiix): a draw whose pipeline is still being built by a worker first asks the caches for the full one without compiling (where the driver has `pipelineCreationCacheControl`), else gets one made at once without the driver's optimizer, into no pipeline cache, and is drawn with it until the worker's full build replaces it. Up to this many ms of those a frame on each recording thread for draws that could be left out: a build is not started where the thread's last one would not fit in what is left, but a frame's first always is, so a frame runs past the budget by at most one build. Draws that must wait get one whatever the budget. Off by default on every driver until measured in the game (it is for AMD's proprietary driver, where the optimizer took 5-13 s a pipeline: Mako, Volvagia's intro a black silhouette); `8` is the suggested value (half a 60 fps frame, as `RX_VK_TRANSLATE_WAIT_MS`; Mako's 1000 trades a missing draw for a hitch of up to a second). Where a quick build takes longer than that (tens of ms on AMD's proprietary driver), `8` means about one quick build a frame on each recording thread, each a hitch of that build's length; `0` or unset: off. Not made where the full build is made without the optimizer already (`RX_VK_NOOPT`, AMD's task and mesh programs); while the driver-fault gate has another build of the program in the driver (AMD, often the entry's own full build) none is made then, and a later draw tries again. The gate is per program, and a worker waiting at it takes it back before a later draw's try, so on AMD quick pipelines mostly come for programs no worker has reached yet (a backlog, as in a new area): a draw whose program is already building is left out or waits for the whole build, as without quick pipelines. Not with `RX_VK_PIPE_SKIP=0`, which waits for every pipeline. Deliberately, a full build that fails (or that the driver faults on) leaves the quick pipeline drawn with for the session, where the draw was left out before. Memory: a quick pipeline the full build replaced is not destroyed (nothing tells when the recordings that used it are done on the GPU), so each entry that got one keeps one more pipeline, unoptimised, for the session; the log's quick pipeline lines count them. Quick builds are not counted in the stats' pipelines. Anything but a number is ignored with a warning; the value in use is logged. Untested in the game in Hookshot |
| `RX_VK_PIPE_WAIT_MS=<ms>` | Off by default. Set (Mako uses 100): a draw on earlier frames' targets whose pipeline is still being built, with no quick one, has its build moved to the front of the workers' queue and waits for it at most this long a frame in all, on each recording thread, before it is left out. The build is not run on the waiting thread (one started cannot be stopped, and took seconds on AMD). Pipelines only: shaders still translating follow `RX_VK_TRANSLATE_WAIT_MS`. Anything but a number is ignored with a warning |
| `RX_VK_TEST_SLOW_MS=<ms>` | A test, off by default: each draw's full pipeline build (a worker's) is held this long (at most 60000) inside the driver-fault gate, as AMD's optimizer on a cold cache, to watch quick pipelines and the brief wait on any GPU. The gate holds one build of a program at a time only on AMD: elsewhere add `RX_VK_DRIVER_FAULTS=2` to see a quick build meet the program's full build in the driver, as on AMD. Anything but a number is ignored with a warning |
| `RX_VK_COMPILE_SHADERS=all` or `most` | What a preparation builds, over `compile_shaders` in `hookshot_options.ini` (the game's `--compile-shaders` sets it): `all` the predicted pipelines too (`predict_run`), `most` without them. The log's `vk: Compile shaders:` line says which and why. Most also removes what an earlier preparation on All left (`hookshot_pipelines.bin.predicting`, `.predicted`, `.predicted_keys`) |
| `RX_VK_HANDLE_TABLES=0` | A texture handle a shader reads from a table at an index of its own (OoT's clustered lights: each light's shadow map, read per pixel from the light table) translated as before instead of as a descriptor array of the table's first 64 entries (`handle_tables_on` in `gpu/vk/vk_shader.cpp`, the texture pass's handle tables): those shadow tests then read no texture (the dark and green blocks of issue 52). Off, the texture pass's other changes that came with the tables are off too (its search skipping texture results and float math, words of 12- and 16-byte loads, the shared handle programs, the pointer words), so the translation is the one before them. On by default where the GPU indexes sampled image arrays per pixel (`shaderSampledImageArrayNonUniformIndexing`) and has at least 256 sampled images, samplers and resources a stage and 512 sampled images and samplers a set; the log says `vk: handle tables as descriptor arrays: on`, or which check turned them off. Part of the program cache's stamp when off; `nx2/bake_ds` bakes with them on unless `--feature no-handle-tables` |
| `RX_VK_COMMIT_RESERVE_MB=<MB>` | Windows only: the commit the texture cache leaves free before Windows' commit limit (RAM plus the page file; default 2560, `0` turns the rule off). Every VRAM block counts against the process's commit, so below that much headroom (`GlobalMemoryStatusEx`'s `ullAvailPageFile`) the shortfall comes off the textures' budget and eviction and compaction give blocks back; the log says `vk: commit limit near:` at most every 600 frames while it is on. At the limit itself, `hos_commit` and the first-touch handler wait up to 5 s for commit (error 1455) before giving up |
| `RX_VK_DESC_COUNTS=0` | A/B switch: a draw's descriptor build (`build_descriptors`, `gpu/vk/vk_draw.cpp`) counts the program's descriptors before every build, as before; by default the three counts are taken once and kept with the program (`Program::desc_counts`), once it is ready. The same writes and reserves either way |

Builds keep the button prompts when their source, fonts, game assets, button tables, image overrides,
Python, package/library versions and generation options still match, and every generated file still has its recorded
size and content hash. The inputs are checked before and after generation; changed or unreadable inputs leave no reusable
stamp. The log says `prompts: unchanged, kept` or why it regenerated. Packaged builders keep them too: the stamp has the builder's release, so the first build after a builder update draws
them again whatever changed in it. `HOOKSHOT_FORCE_PROMPTS=1` draws them in any build.
Generation uses a fresh directory and publishes only a complete set. A failed generator still removes the prompts, so the game shows its own
icons, as before. An interrupted builder can leave the old set without a stamp; the next build regenerates it.
The files the game derives at run time (`host/prompts.c`) are kept with them: the stamp covers `host/prompts.c`, so they
were made by the same code, and a game running from the folder keeps its files.
Before checking the cache, builds sweep `.prompts-*` staging directories without following links or Windows reparse
points. POSIX uses checked directory descriptors and owner locks, with a 24-hour grace period for unmarked legacy
directories; it skips cleanup if descriptor-safe removal is unavailable. Windows uses only the 24-hour age rule,
even for directories with an owner marker, and never opens a leftover's owner file or compares file identities.
Removal is best effort: a locked, in-use or otherwise unremovable directory is kept with a message, without failing
the build.

#### A faster build for development

The eight hot pieces (`code_hot_*.c`, the functions the game spends its time in) are built at `-O2` with the play
profile, their hot callees inlined. They are 81 MB of the 1.4 GB of lifted C but about a third of its compile time,
and each is one of the longest single compiles, so a build after a new lift cannot end before the slowest of them.
When you are checking that something works (the game starts, a menu, a screenshot, a log) and not how fast it runs,
build them as every other lifted file is built, at `-O1` and without the profile:

```
RX_HOT_OPT=O1 python -I nx2/build.py --standalone --jobs 8
```

`build.py` run on its own uses no profile unless `RX_PGO` is set. `make_game.py` always passes the shipped profile
(`work/hookshot_pieces.prof`), so `RX_HOT_OPT=O1` through `make_game.py` gives `-O1` with the profile, which saves less.
Run on its own, `build.py` also lacks the rest of `make_game.py`'s environment (`RX_RELEASE`, `A64_HOT_CHUNK`), so its
runtime objects differ from a `make_game.py` build's.

The objects are cached by their flags, so `-O1` and `-O2` objects can sit side by side, and `make_game.py` leaves
`RX_HOT_OPT` and `RX_PGO` out of its lift key. In the checkout's own `work/` folder, or with
`HOOKSHOT_KEEP_BUILD_FILES=1`, switching back and forth therefore neither translates the game again nor recompiles
anything built before. In any other `--work` folder, `make_game.py`'s clean-up keeps only the last build's objects and a
translation that fits them, so each switch translates the game again and recompiles the hot pieces.

What it saves, from the per-file compile times (`build_times.json`) of six earlier full builds on one 24-thread
desktop, with other work running (not a measured `-O1` build):

| | Hot pieces (`-O2`, profile) | The 72 other lifted files (`-O1`) |
|---|---|---|
| Compile time, all of them | 1,270-1,810 s | 2,820-3,750 s |
| Per MB of C | 16-22 s | 2.2-2.9 s |
| Longest single file | 300-800 s | ~360 s (`code_021_1.c`, giant functions) |
| Peak memory per compiler | 0.8-1.7 GB | 1.4-1.6 GB for a 24 MB file |

At the other files' rate the hot pieces should take about 200 s instead of about 1,500 s of compile time, about
a quarter less in all, and the build's longest file becomes an ordinary one, so a build after a new lift can end several
minutes sooner on a CPU with many threads. The peak memory hardly changes: the ordinary files set it already.
The hot code runs slower at `-O1`; the game still plays (the profile-counting build, `RX_PGO=gen`, already builds
the hot pieces at `-O1` and is played through to record the profile), but check that the frame rate is good enough for
what you test.

**Never use it for numbers:** no fps or frame-time measurements, build timings, hitch or stutter checks, profiles,
and no release or player build. Build those with the defaults.

## Draw statistics

Set `RX_VK_DRAW_STATS=<directory>` and create `<directory>/draw_stats.now` during play to measure one future frame.
The report keeps its per-program counts and adds `code` rows with the native stage keys copied when each query was
recorded, including programs loaded from the cache. The four keys are vertex/mesh/compute, fragment, task and the
header-only stage; zero means no key was available. Distinct tuples for the same program stay separate. These are
recorded-query identities, not a complete draw replay or display timestamps. The queries can perturb frame timing.
#### The renderer's play profile

The Vulkan back end is built with a play profile of its own (after
[ProjectReLink](https://github.com/pixl27/ProjectReLink)): `assets/profiles/hookshot_vk.profdata.xz`, committed, with
its note in `assets/profiles/README.md` (where and how it was recorded, and what it measured: about -4 to -5 % frame
time standing and -3 % running in the Market on an RTX 3090). Every build that compiles the renderer uses it: a
player's (with DLSS or FSR 4 too), the AppImage's, a developer's. A checkout's own `work/hookshot_vk.profdata` is used in
its place, and `RX_VK_PGO` set by hand (`gen`, or another file) in place of both. Recording a new one:

1. **Build a renderer that counts**, `RX_VK_PGO=gen`: the back end's own files (`gpu/vk/*.cpp` and suyu's texture
   decoders; not Eden's recompiler, sirit, Dear ImGui or zstd) are built with `-fprofile-instr-generate`, still at
   `-O2`, and the module gets its own copy of `host/pgo_rt.c`. As a pack of a change (`base` takes it the same way;
   `pack` builds the renderer for `RX_VK_PGO` even when no file changed since the base), or the renderer alone:
   ```
   RX_VK_PGO=gen python -I scripts/dev_packs.py pack <dir> vk-count
   RX_VK_PGO=gen OOT_WORK=<work> RX_RELEASE=1 RX_VK_ONLY=1 python -I nx2/build.py --standalone
   ```
   It runs slower than the ordinary one (every branch of those files adds to a counter in memory, from every thread):
   it is for recording, never for numbers.
2. **Record a run**: start the game with `RX_VK_PGO_FILE=<file>` in the environment. Create `<file>.reset` once the
   place has loaded (the counters start again), play the places the profile should know, then create `<file>.dump`:
   the counters are written to `<file>` within half a second and the `.dump` file goes. Several places can each be
   dumped to a file of their own. [The pipeline tour](#the-pipeline-tour) does this on its whole route (on Linux), with
   a runtime of the counting pack as its game folder:
   ```
   RX_VK_PGO=gen python -I scripts/dev_packs.py pack <dir> vk-count     (step 1, if not done)
   python -I scripts/dev_packs.py rt <dir> <runtime> vk-count
   python -I scripts/pipeline_tour.py tour --game <runtime> --out <tour dir> --name play --vk-profile <tour dir>/vk
   ```
   Each of its games runs with `RX_VK_PGO_FILE` set, clears the counters once it has control of Link (the loading, the
   prepare and the title left out) and writes them at its end, failed or not, as `<tour dir>/vk/<game>.profraw`, with a
   `.json` beside it saying where it was recorded. A renderer that doesn't count is refused before the game starts, and
   one that takes neither request fails the run, so a recording never ends with nothing in it.
3. **Merge** with the `llvm-profdata` of the compiler that built it:
   `llvm-profdata merge -o work/hookshot_vk.profdata <file> [<file> ...]`, or for the tour's
   `python -I scripts/pipeline_tour.py merge-profile <tour dir>/vk` (the toolchain's `llvm-profdata`, or
   `--llvm-profdata <path>`; `--out <file>` for another place than `work/hookshot_vk.profdata`). With `--publish` it also
   writes `assets/profiles/hookshot_vk.profdata.xz` (xz -9e) and the lines for `assets/profiles/README.md` (when, the
   GPU and driver, the build, the route's parts, the compiler) to `<tour dir>/vk/README-note.md`.
4. **Build with it**: `make_game.py` copies `work/hookshot_vk.profdata` to its work folder and passes it
   (`RX_VK_PGO=<file>`), unless `RX_VK_PGO` is set already; without one it unpacks the committed profile there
   (`hookshot_vk.shipped.profdata`) and passes that. Either only when this compiler reads it, on every system (a
   profile from a newer LLVM than the compiler's would stop the build): else the renderer is built without one.
   To ship it: `xz -9e -c work/hookshot_vk.profdata > assets/profiles/hookshot_vk.profdata.xz`, the note in
   `assets/profiles/README.md` updated, in a pull request. `nx2/build.py` run on its own takes it only from
   `RX_VK_PGO`. `dev_packs.py` builds a pack of the renderer alone with its base's profile. The lift key leaves
   `RX_VK_PGO` out, and only the back end's own objects (35 files) are named by the profile's contents: another profile
   builds those again and nothing else, and the shader caches stay (their stamp is the translator's sources).

A profile fits the compiler target it was recorded with: llvm-mingw's for Windows players, clang's for Linux (C++
names differ between the two wherever a type's width does, and the code differs where it asks which system it is on).
Runs from both can be merged into one file: a function is matched by its name and the hash of its shape, and what
matches nothing is left out. For the same reason a profile ages rather than breaks: a function whose branches changed
since the recording, or a new one, is compiled as if there were no profile (clang's warning for it is among those
the build silences), the rest keep theirs. The committed profile is one for every build for now, recorded on Linux:
a Windows build matches the renderer's own functions and leaves the C++ library's out. On Linux the AppImage carries no
built back end (since 2026-11-14): every player's build compiles it, with or without DLSS or FSR 4, with the AppImage's copy
of the profile (`assets/profiles/`) and for the player's own CPU. The AppImage workflow compiles it once as a build check.

The file holds the names of the back end's functions (Hookshot's own source), a hash of each one's shape and how often
each branch ran: nothing of the game's code or data. It says which code paths the recorded places exercised, no more.

#### Testing a change against a base

A full build of a new game folder is more than an hour on some PCs, most of it the shader preparation, which starts
cold in a new folder. `scripts/dev_packs.py` builds the game once as a **base**, then builds a change as a **pack**
of what differs from the base (the game, the renderer, files the build generates) and runs it from a copy of the
base. It calls `make_game.py` and `build.py` as they are, with the defaults, and changes nothing in what players
build.

**A base, a pack and a runtime never leave your PC.** They are built from the game: a pack holds the built modules,
with the lifted game linked in, and files generated from the game; a runtime is a whole game folder. Never attach,
upload or send them to a tester or the chat ([CONTRIBUTING.md](../CONTRIBUTING.md#the-rule-with-no-exceptions)).
What you share is the branch; a tester builds their own.

```
python -I scripts/dev_packs.py base ../base-main --romfs <romfs> --exefs <exefs> [--seed <an older game folder>]
python -I scripts/dev_packs.py pack ../base-main my-fix --tree <the change's checkout>
python -I scripts/dev_packs.py rt   ../base-main ../rt-my-fix my-fix
```

- **`base`** builds a checkout at the commit to compare against (with `HOOKSHOT_KEEP_BUILD_FILES=1`).
  `--seed` copies an older game folder's shader caches first, as an update keeps them: the caches are stamped
  by the shader translator, so the prepare redoes only what changed instead of starting cold.
  `--predict-s N` stops building the whole game's predicted pipelines after N seconds (`RX_VK_PREDICT_S`); the
  rest are built in play when first drawn, so warm the base for the scenes you test (below). For development
  only, never for a player's build: 0, the default, builds them all.
- **`pack`** needs a checkout whose history contains the base's commit (rebase it first). Its commits, its
  uncommitted changes and its untracked files all count.
  - **How it builds:** a change to renderer files alone (`gpu/vk/`, `imgui`, FidelityFX) that the game is not
    built from builds the renderer alone (`RX_VK_ONLY`, no lift: seconds to minutes). Any other change builds the
    game with `make_game.py` (`HOOKSHOT_NO_PREPARE=1`), both modules and the game folder. The renderer files the
    game is built from are found in the checkout: what `host/` includes from `gpu/`, what `build.py` adds to the
    game's dependencies, and what those include.
  - **What it keeps:** a module when one of the objects it is linked from differs from the base's (its link list,
    `objs.rsp` / `vk_objs.rsp`). Bytes can't tell: both modules hold the version, a fingerprint of every source
    (`nx2/version.py`), which changes the code around it, so the object holding it (`hos_main.c`, `vk_menu.cpp`)
    counts only when its own source or a header changed. Both modules are kept when something reaches them besides
    their objects: the build's own scripts (`make_game.py`, `build.py` and what it imports, such as `toolchain.py`:
    flags, link options, the hot order, the Windows manifest), a data file under `nx2/` that is not the translator's, or a non-C file under
    `host/`, `gpu/` or `third_party/`. It also keeps every other file of the built game folder that differs from
    the base's (the shader seed, the shipped pipeline list, the button prompts). A file the base's own build made
    (`base` records them) that the change's build no longer makes is removed from the runtime; saves, settings and
    caches are never touched, and an adopted base records none.
  - **Prepare:** the runtime prepares again when the shader translator (`gpu/vk/{eden,sirit,compat}/`,
    `vk_shader.cpp`, `vk_bank.cpp`, `vk_state.h`: what `build.py` stamps the shader caches with, as
    `nx2/translator_files.py` names them) or the shader seed changed; `--prepare` / `--no-prepare` decide instead.
    A base prepares once its outputs are recorded, as `make_game.py` does: `HOOKSHOT_NO_PREPARE` (set and not `0`)
    skips it, and a preparation that fails (no usable Vulkan, a driver that stopped responding) keeps the base, with
    the reason in `base.json`'s `not_prepared`, and leaves the work to its first start.
  - **Where:** game packs share one work folder per base (`<base>/game-work`). The lift key holds that folder's
    path, so the first game pack translates the game once (or copies the base's translation from the lift store,
    below) and the later ones skip the lift while the translator, the HLE functions and the hot list stay the same;
    the base's compiled objects are linked in. Build one pack at a time.
- **`rt`** makes a game folder to run: the base's, with `romfs` hard-linked (it is never written), the other files
  copied (the game writes its caches), and the packs' files over them. Two packs that bring the same file are
  refused (merge the branches and build one pack); a pack built on another base is refused too, so the game and
  the renderer always come from one base (`MKWVK_API_VERSION`).
- **`warm`** plays a runtime with no pack once per `--run` command (`{runtime}` in it is the runtime's folder),
  copies the caches play made or grew (pipelines, play files, programs) back into the base's game folder, and
  deletes the runtime, so every later runtime starts warm for those scenes. Only cache files are copied.
- **One job at a time per base:** `base`, `pack`, `rt` and `warm` hold `<base>/dev_packs.busy` while they run.
- **Objects shared by every base:** its builds use one [object store](#building) (`HOOKSHOT_OBJ_STORE`, by default
  `~/.cache/hookshot-objects`, `%LOCALAPPDATA%\Hookshot\objects` on Windows), so a new base compiles only the files no
  earlier base or pack on this PC compiled. On another drive than the bases it copies instead of linking: set
  `HOOKSHOT_OBJ_STORE` to a folder on the bases' drive, or to `0` for none.
- **Translations shared by every base:** the same for the lift (`HOOKSHOT_LIFT_STORE`, by default
  `~/.cache/hookshot-lifts`, `%LOCALAPPDATA%\Hookshot\lifts`): a new base, or a base's first game pack, copies the
  translation an earlier base or pack made from the same game, translator, HLE functions, hot list and settings
  instead of translating the game again. `0` for none.

For numbers, compare the runtime with one made by `rt` from no pack: same base, same caches, one difference.

## The pipeline state list

Every release ships a list of pipelines, merged from the sources in `assets/pipeline_states/`:

- `tour.bin.xz`, which only the pipeline tour (`scripts/pipeline_tour.py`) writes, replaced whole each time;
- `contrib/<name>.bin.xz`, one per contributed list, which people add.

Each has a note beside it (`<name>.md`) that says who recorded it, on what GPU, driver, build and settings, and the
route. [`assets/pipeline_states/README.md`](../assets/pipeline_states/README.md) has how to record and add a list. Each
record is a pipeline the game asked for: its shaders' hashes, the program variant's hash, and the target formats,
vertex formats and draw state it was made with (`nx2/pipeline_states.py` has the layout). It holds no game code: a
player's build turns the hashes back into shaders from their own `hookshot_shaders.bin`.

A record can also name a compute program the game ran, by the hash of its shader alone (the record's kind says which;
the file format is the same, and lists from before read as they did). The game writes one the first time a run
dispatches a program. Every start keeps the pipelines of the listed compute programs ready before the title: the
shipped list's, and the ones in the game folder's `hookshot_computes.bin`, which holds what this install's play has
dispatched (the same programs its own state list names). Without a list entry, a program is loaded and its pipeline
made at its first dispatch, and a dispatch whose pipeline takes more than 50 ms is skipped. The log's
`precompile: compute programs` and `compute pipelines` lines say how many were listed, from where, and how many
pipelines came from the caches or were compiled. A first start, before this install's play has dispatched anything,
keeps every compute program the game has, as it did before the list named any: a program the list misses is then ready
too, at the cost of that start's memory. `RX_VK_PRECOMPILE_COMPUTES=listed` keeps only the listed ones on a first
start as well; `learned` leaves the shipped list's out, to compare; `all` keeps every one at every start and `none`
keeps none. The game keeps every compute program a shipped list names and never drops one: the bound is
`pipeline_states.py check`, which stops on a merged list that names more than `COMPUTE_MOST`, for someone to decide.

`nx2/make_game.py` merges the sources at every build into the game folder's `hookshot_pipeline_states_shipped.bin`. It
takes the tour's list first, then the contributed ones by name, and keeps each pipeline once. The same sources always
give the same bytes, and the first start's shader preparation builds those pipelines before the title. Without them a
new install builds them in play, and draws left out while they compile wash the first field load white for seconds
(v2.0.8 to v2.0.14 shipped none). The sources are xz-compressed, written with fixed settings, so that the repository's
history stays small. The current ones hold 11527 pipelines in about 175 KB.

Every release carries them:

- the AppImage with `assets/`: `scripts/linux/appimage.sh` stops without them, and `scripts/linux/check_player_build.sh`
  checks them with the image's Python;
- the Windows builder's `tools/assets/pipeline_states`: `scripts/release_parts.py package` stops when there are none or
  one does not read.

xz rather than zstd: the AppImage's Python is 3.12, whose standard library has `lzma` but not `compression.zstd`.

A game run writes `hookshot_pipeline_states.bin` beside the executable, adding each pipeline it makes in play. To add a
list, play on a build of the current source and add that file as a contributed list. `add` keeps only the pipelines no
other contributed list holds:

```sh
python -I nx2/pipeline_states.py add <name> <game folder>/hookshot_pipeline_states.bin --who ... --gpu ... --build ... --settings ... --route ...
python -I nx2/pipeline_states.py tour <tour lists>... --gpu ... --build ... --settings ... --route ...   # the tour's, replaced
python -I nx2/pipeline_states.py check                      # every source, and the merge, as CI and the release checks read them
python -I nx2/pipeline_states.py merge --out merged.bin     # the list make_game writes beside the game
```

A list depends on the settings it was recorded with: its records hold the target formats and draw state, so one recorded
at 1440p with DLSS shares few pipelines with one recorded at 360p.

Players send theirs to issue #2034 with **Save my pipeline list** (the menu's Debug tab, under Developer: `gpu/vk/pipeline_share.h` and
`vk_menu.cpp` `share_pipeline_list`; the builders: `nx2/pipeline_share.py`, which the Linux builder imports and the Windows
one runs with `--json`). It checks the list as `pipeline_states.py --check` does and writes
`hookshot-pipelines-<date>.zip` to the Downloads folder with exactly two files, the list and `hookshot_pipeline_report.txt`
(the issue's template filled in from the run or from the game folder's log and `hookshot_options.ini`), then copies the
template, shows the folder and opens the issue; never with `HOS_HEADLESS`. The list is zipped as the game wrote it, compute
records included (the report's `Pipelines` line counts them); `pipeline_states.py --check` and `add` take a sent list only
when it is exactly a list (`read_sent`). `tests/test_pipeline_share.py` checks the zip,
the template and the refusals for both. By hand: `python -I nx2/pipeline_share.py <game folder> [--out-dir <folder>]`.

Refresh the tour's list before each release, and after a change to the shaders' translation or to the state a pipeline
is made from. A record that no longer matches what play asks for builds a pipeline nothing uses, and the one play wants
compiles in play. When the record's format changes (its magic and record size, `MAGIC` and `SIZE` in
`nx2/pipeline_states.py`), the old lists no longer read and the release checks stop. Convert each source in place, or
record it again, and make a new tour list. The folder's layout stays the same.

### The pipeline tour

`scripts/pipeline_tour.py` plays the game for the tour's list, unattended, on Linux, and writes `tour.bin.xz` with its note
(ported from Hookshot's draft [pull request 2048](https://github.com/NotRexed/Hookshot/pull/2048), now driven through the
[control bridge](control.md)):

```sh
python -I scripts/pipeline_tour.py tour --game <game folder> --out <tour dir> --name play --publish
python -I scripts/pipeline_tour.py run --game <game folder copy> --out <dir> --part child-overworld --first "Market" --stops 3 --skip-opening
python -I scripts/pipeline_tour.py run --game x --out x --dry-run          # the route, without the game
python -I scripts/pipeline_tour.py compare <new tour dir>/play.bin <old tour dir>/play.bin   # what the new list lost, and gained
```

`tour` runs its parts (the opening; the child overworld; the dungeons at both ages; the adult overworld) one after another,
one game at a time, in a reflinked copy of the game folder (`<game folder>-tour`: on btrfs or XFS, and refused when one is
there from an earlier tour), at nice 19 (on `--cpus` if given), in games of `--chunk` stops
(8; a fresh game each, as one game's memory only grows), and merges their lists; `--publish` then runs
`nx2/pipeline_states.py tour` with the GPU and build the bridge reported and the settings and route the runs used
(`merge` merges any lists). Each game runs under a memory guard (the script's `MEM`, `--mem key=value`): it starts only with
8 GB available and at most 16 GB of swap in use (else it waits, 30 minutes at most); a game holding more than 9 GB after a
stop ends there and a fresh one goes on from the next stop; past 12 GB, under 3 GB available or past 20 GB of swap it is
stopped at once and a fresh game goes on from the stop it was at (a stop that does it twice is left out). With systemd the
game also runs in a scope of its own with `MemoryMax` (16 GB).

Each game starts with no save, in a data and `--user` folder of its own and with none of the shell's or `hookshot_env.txt`'s
switches (`HOS_*`, `RX_*`, `A64_*`, `OOT_*`; an empty `HOS_ENV_FILE`), only the run's: the title, New Game and the opening (watched whole by the
opening part, skipped by the game itself, `HOS_SKIP_OPENING`, in the others), until the bridge's `status` says Link is in a
map with no event playing and nothing loading; then every item from the Debug tab's inventory but the story's (medallions,
stones and songs start scenes that wait for the player), and each Teleport destination: the camera all around, at Lake
Hylia a dive (and, as the adult, the iron boots and the Zora tunic on the lake's bottom), and on to the next once no new
pipeline came for 60 frames, a second of game time (`--settle-quiet`; 600 frames at most, `--settle-max`; it was 150, 2.5 s).
The sword, the shield and an item from the pause menu's Items page are used once an age (the script's `ITEMS_ONCE`: Kokiri
Forest as the child, Hyrule Field as the adult) and at the stops with passes of their own an item's draws could combine with
(`ITEMS_PASSES`: Zora's Domain and the Water Temple for water, the Shadow Temple for the Lens of Truth, Death Mountain Crater
and the Fire Temple for the heat haze), a different pause menu pick at each: a pipeline is a program and its fixed draw state, and a
scene's light and fog are inputs to the same pipelines, so the sword drawn in one more field is no new pipeline. A few stops
are left out: one starts a scene the tour cannot finish, others added almost nothing (the script's `LEFT_OUT`). `value <run
dirs>` measures that: each step's pipelines that neither the players' lists (`contrib/`) nor any other step has, its
seconds, and the steps under `--floor` (10) cut (`--sub`: a stop's arrival, look around, item use and settling apart).

It is driven through the bridge (`HOS_CONTROL`, a socket in the run's folder): `teleport` by the destination's index and
`age`, asked again while the game answers `refused` (an event playing, a map loading; another teleport in the last 8 s of wall
time, which is waited out and never pressed at),
`timeout` or `cancelled`, with A or B now and then when a conversation holds the game; given up on when the refusal is one
asking again cannot change (no map for Link's age) or a request is sent five times without Link moving (the Haunted
Wasteland, visited last). Arrival is `wait map` (`loading_end` after the age switch and the teleport to the lake's bottom, the map Link is in), then
`status` until the arrival's event is over in a fresh snapshot (`game.frame` within 10 presents of the reply's); the presses are `sequence` in game frames, the waits `wait present`. Only the Debug tab's inventory, which
the bridge does not do, goes through `HOS_INPUT_LIVE`'s `!cheat` lines (`HOS_OOT_CHEATS=1`). No controller is opened
(`HOS_NO_SDL`) and nothing reaches the desktop. It runs as fast as the game goes, with settings that change no pipeline's
key (formats and draw state, not sizes or rates): headless, 640x360 with the game's own upscaler at 50%, the cap Uncapped
(`HOS_FPS_MAX=1000`) with every frame a 60th of a second of game time (`HOS_FPS_STEP=0`, Game speed Per frame), pacing and
V-Sync off, text at once, no frame generation, sharpening or HDR, in the run's own copy of the options file (restored
after, or removed when the folder had none). Every wait on the game counts its frames, so a faster game waits less wall time; the inventory's `!cheat` lines alone
are also 32 ms apart (the game reads the live file every 30 ms), then three frames. `run` is one part (`--part`, `--first <stop>`
to start again at a stop, `--stops N`, `--dry-run`); `--env` and
`--ini` change the settings (a list for the mesh fallback: `--env RX_VK_FORCE_MESH_FALLBACK=1`), and `--window WxH` shows
it. Each run writes `states.bin`, `report.txt` and `report.json` (each step's pipelines, the new ones it added, its frames,
seconds and the game's memory: a flattening curve says the list is nearly whole; and the settle's frames), `log.txt`,
`steps.txt` (each request, at
its frame) and a small picture every 600 frames (`shots/`). The game folder's pipeline notes and lists, and
`hookshot_computes.bin` (the compute programs earlier play dispatched, which the game writes into a new list), are put
aside for a run and back after it, failed, stopped (Ctrl+C or SIGTERM: a tour gives its run two minutes to end its game
and put them back) or not; a file that could not go back is named in the run's output. `tests/test_pipeline_tour.py` checks it without the game,
against a fake bridge.

A changed route or wait is checked by the lists, not the clock alone: a tour each way on the same PC and game folder, then
`compare <new tour dir>/<name>.bin <old tour dir>/<name>.bin` (or `tour --compare-to <old list>`, after its merge), which
prints how many pipelines the old list has that the new lacks (lost; none, for a change that only saves time) and the other
way round. The wait as it was is `--settle-quiet 150`; `report.txt` says which a run used.

`--vk-profile <dir>` (`run` and `tour`) records [the renderer's play profile](#the-renderers-play-profile) on the same
route, with a counting renderer (`RX_VK_PGO=gen`): each game's counters, from control of Link to its end, go to
`<dir>/<game>.profraw`, and `merge-profile <dir> [--publish]` merges them. The lists are made as without it.

## The options file

A new row on the Graphics, Audio, Gameplay or Controls tab needs a line in the README's option table too, in the
menu's order, with its choices and default: `tests/test_readme_menu_rows.py` fails without it.

`hookshot_options.ini`, in the game folder, holds the Insert menu's rows as `<key>=<number>`: the row's name in lower
case with `_` for spaces (`frame_generation=2`, `invert_camera_x=1`), except Output resolution, which is `output_res`.
`gpu/vk/vk_menu.cpp` writes the whole file at every change (`save`), with `options_version=5`,
`microphone_device=<name>` when a microphone is chosen by name, and the controls (`pad_<button>` and `key_<button>`:
the README's [Controls](../readme.md#controls)). A value outside a row's range is clamped when it is read. Where the
`microphone_device=<name>` when a microphone is chosen by name, and the controls (`pad_<button>`, `key_<button>` and `toggle_<button>`:
the README's [Controls](../README.md#controls)). A value outside a row's range is clamped when it is read. A text field
(Custom output size: printable ASCII, with full-width digits and `ｘ` folded to ASCII) is saved as its setting is:
Custom output size as `output_res`. A pad can edit it without a keyboard: D-pad Left/Right moves the caret, Up/Down
cycles the character, the right shoulder inserts a digit and the left shoulder removes a character. The end caret
can also append with Up/Down. Confirm commits, Back cancels, and the shown Paste button still pastes. No platform
screen keyboard is requested. Where the
number is not the obvious one:

`toggle_<button>=0|1` is the Controls page's Toggle column: 0 Off (default), 1 press once to hold and again to release.
It applies after remapping to either pad binding, keys and mouse buttons. Only Switch buttons accept it, excluding
Plus and Minus; directions, Walk and ocarina notes do not. Other values mean Off. Saving keeps the flags, not live latches.
Opening Insert, Plus/Minus, focus loss, rebinding and Reset to defaults clear latches. Background pad presses can
latch again when Controller in background permits them. Linux's other game dialogs are not guaranteed resets;
Insert always releases the latch. Text fields use physical pad edges, not latched gameplay buttons: a held confirm
or shoulder does not commit or add another character when editing starts. Fresh presses and D-pad repeats still edit,
and keyboard editing keys reach the field while the game receives none.

Custom output size's restart state tracks the actual width and height, not just the custom choice's list index.
Audio output keeps its separate channel-layout state: Mono and Stereo share a restart value, so switching between
them does not renew a size change's declined restart prompt. Choosing a different channel layout still does.
`tests/test_menu_text_field.py` exercises these changes together, including restoring the original size.

| Key | Values |
|---|---|
| `quality_preset` | 0 Custom, 1 Minimum (4GB), 2 Low (6GB), 3 Balanced (8GB), 4 High (12GB), 5 Ultra (16GB+) |
| `gpu_device` | 0 Auto, n the n-th card with Vulkan 1.3 as the row lists them |
| `frame_generation` | 0 Off, 2 to 8 the multiplier |
| `resolution` | 0 Auto, 1 100%, 2 75%, 3 67%, 4 50% |
| `output_res` | A size, `<width>x<height>`: 2048x1152 (the game's own reduced size), one of the screen's display modes (the menu's Aspect ratio and Output resolution rows), or any other with each side a multiple of 8 (rounded down), at least 640x360, at most 5120x2880's pixels, 5:4 to 48:9; anything else is 2048x1152. No line: 2560x1440, the game's own picture, or the screen's size where that has fewer pixels, at the screen's shape (2560x1080 and 2256x1504 start at their own size, as 1280x800 and 1920x1200 did before). The menu marks 2048x1152 "(reduced)". A size more than 1 % off 16:9 sets the picture's shape (`host/hos_wide_size.h` `hos_output_own_shape`). The Aspect ratio row saves `ultrawide=` 1 for the monitor's own shape, else 0; another shape is `output_res`'s own. The numbers saved before (0 2048x1152, 1 2560x1440, 2 3840x2160, 3 1920x1080, 4 5120x1440, 5 5120x2880) are still read |
| `upscaling` | 0 Off, 1 Catmull-Rom, 2 Lanczos |
| `compile_shaders` | No menu row: the Builder's, `make_game.py --compile-shaders`'s and the launcher's Compile shaders, read by a preparation (`vk_draw.cpp` `compile_all`). 0 Most (the default), 1 All (the predicted pipelines too). A file with only this line still makes the game's first start a first start |
| `sharpness`, `rumble_strength`, `motion_sensitivity_x`, `motion_sensitivity_y`, `walk_speed` | percent |
| `mouse_sensitivity` | hundredths: 100 is 1.00 |
| `field_of_view` | 50 to 150 percent of the current vertical gameplay FOV; 100 is the original. Applies immediately, before projection and culling. The resulting vertical angle stays within 5 to 140 degrees |
| `robust_buffers` | 0 Auto (on), 1 Off, 2 On |
| `damage_taken` | 0 Half, 1 Normal (default), 2 Double, 3 Quadruple; Debug tools Off uses Normal without changing the saved choice. Half rounds the full hit up to a quarter heart, even on lethal hits; Heart protection wins |
| `game_speed` | 0 Per frame, 1 Real time |
| `text_speed` | 0 Normal, 1 2x, 2 4x, 3 Instant |
| `microphone` | 0 Off, 1 Default, 2 and up a microphone (the name in `microphone_device` decides which) |
| `mouse_camera` | 0 As the stick, 1 Native, 2 Off (the mouse is never captured) |
| `walk_mode` | 0 Hold, 1 Toggle |
| `layout_preset` | 0 Nintendo, 1 Xbox / PC, 2 Swap Face, 3 Custom |

The other rows are 0 Off and 1 On. Some are read at the start, before the menu exists, by the code that needs them:
`handheld_profile` and `output_res` (`host/nn_applet.c`), `ultrawide` (`host/hos_main.c`), `robust_buffers`
(`gpu/vk/vk_main.cpp`), the controls (`host/nn_hid.c`) and `skip_title_screen` (`host/oot_autoload.c`).

These named keys, including `walk_mode` and `walk_speed`, are independent of the internal host/renderer option IDs in
`gpu/vk/vk_api.h`; renumbering an option does not require changing a player's saved options file.
Walk mode and Walk speed use IDs 49 and 50 respectively; 52 is Skip opening's. The input regression check compiles the real header
with the host's reads and checks the renderer's answers and menu rows against those names.
Hold-to-toggle uses ID 54 and latches the final Switch-button word. Walk reads its raw bound key earlier and scales
only the movement-key stick, so it never receives a latched Switch button as another toggle input. `toggle_walk` is
not accepted; choose `walk_mode` instead. Walk's Toggle selection survives Insert, focus loss and Switch-button latch
resets; a key held across Insert/focus return must be released before it can switch walking again.

`HOS_CONTROL=<socket path>` turns on the control bridge: a local socket a tool or coding agent drives the running game
through (inputs timed in frames, screenshots, waits, the Insert menu's settings, quit), with `scripts/hookshot_ctl.py`
as its client. `status.game` holds Link's position/facing, the camera's yaw, event/loading/age, life, maximum life,
rupees, A-button prompt and music. `subscribe` with `events:["oot"]`, `every`, `flags` and `changes` streams those
samples and optional watched/changed flags. There is no separate state file; off unless `HOS_CONTROL` is set.
See [control.md](control.md) for the fields, sampling, bounds and examples.

`HOS_CONTROL_PEER_SID=<SID>` (Windows) also lets one other local user account open the bridge's pipe: the Test Farm
agent sets it when it runs the game as its low-rights user. Only a local user account's SID is taken (`S-1-5-21-...`, a
RID of 1000 or more); a group such as Everyone or Users is refused and logged. Unset, the pipe is the game's user's
alone. See [control.md](control.md#security).

## Layout

| Path | What |
|---|---|
| `docs/design.md` | why mkw-recomp's pipeline, the architecture, MKW's lessons as rules, stages, testing |
| `nx2/` | the lift pipeline and its tests (mkw-recomp's, made game-neutral; `kit.py` has the paths), and `make_game.py`, which the builders run |
| `host/` | the standalone Horizon runtime (TOTKPort's `hos/` as adapted by mkw-recomp), with the native NVN2 layer `nvn_native.c`; `host/posix` has the Win32 calls on Linux |
| `gpu/vk/` | the Vulkan back end (`hookshot_vk.dll`, `hookshot_vk.so`), with Eden's shader recompiler and sirit |
| `gpu/render/`, `gpu/third_party/` | the WebGPU renderer the Windows exe falls back to when it was built with wgpu-native, its op stream (`rx_nvn.h`), suyu's decoders |
| `builder/` | Hookshot Builder: the Windows app (.NET 8 WPF), its setup and MSI, and the Linux builder and launcher scripts |
| `scripts/` | test runs, the profile and log readers, the save copier (`oot_saves.py`), release tooling, `scripts/linux` (dependencies, AppImage) |
| `tests/` | the game-free checks, all run by `tests/run_all.py` |
| `third_party/` | a64-kit (Hookshot's few changes marked `(hookshot)`), libopus, zbic (Atmosphere's zstd with ZBIC, for unpacking only), SDL2's licence, Dear ImGui and its Vulkan backend, zstd's decoder, the Libertinus fonts |

License: GPL-3.0-or-later, as mkw-recomp: a64-kit's translator is suyu's (GPL-2.0-or-later) and Eden's code is
GPL-3.0.
