# Contributing to Hookshot

Hookshot is a static recompilation of the Switch 2 *The Legend of Zelda: Ocarina of Time* remake: the game's AArch64
code is lifted to C, the Horizon OS and NVN2 are implemented natively, and the GPU work is replayed on Vulkan. No
emulator is involved, and the game is built on your PC from your own copy.

This page covers what to send, how to build and test it, and the rules that keep the project out of trouble. The
[development guide](docs/development.md) has the full pipeline and its commands; the [design notes](docs/design.md)
have the architecture and the lessons the code follows.

## The rule with no exceptions

**No game files, keys, or anything made from them leave your PC.** The repository does not contain the game and
never will. That includes:

- the `.nup`, `romfs`, `exefs`, `.nso`, `.nca`, `.nsp`, `.xci` or `.nsz`/`.xcz` files, and any title keys;
- `work/` and everything in it: the extracted dump, `image.bin`, the lifted C, test builds, runs and screenshots;
- `hookshot.exe`, `hookshot_vk.dll`, `hookshot_programs.bin` (and its `hookshot_programs.idx`), `hookshot_pipelines.bin`,
  `mkw_vk_pipelines.bin` and other build output;
- saves (`user/`), `hookshot_options.ini`, `hookshot_env.txt` and `language.txt`.

Do not open an issue or a PR asking where to get the game, how to decrypt it, or for someone else's files. A
screenshot, a video and `hookshot_log.txt` are fine and often necessary; the log holds no game data.

`.gitignore` covers most of this, but check `git status` before you commit. If you are not sure whether a file is
game data, leave it out.

## Ways to help

What to work on first, and the steps from claiming a problem to its release, are in the [workflow](docs/workflow.md):
priorities by severity and by how many players run the affected hardware, and the hardware tiers a change must not
break. Claim in Discord #dev-work before you start.

| | What |
|---|---|
| **Test on other hardware** | NVIDIA cards are the most tested; the README's [status table](readme.md#project-status) says what works where, and [tested configurations](docs/tested-configurations.md) lists the runs behind it (build, hardware, driver, where, fps). Reports from AMD and Intel cards, other NVIDIA drivers, other distributions and SteamOS are useful even when everything works: add a row there. Pull requests that need a run on your hardware are queued as [test requests](docs/test-requests.md): pick one up, by hand or with an agent. |
| **Play further** | The tested area is the start of the game. Reaching new areas, cutscenes, menus and items finds problems the developer has not seen. |
| **Report what is broken** | Use the [issue forms](https://github.com/NotRexed/Hookshot/issues/new/choose): game problem, build problem or idea. Include the version, your hardware, where you were and `hookshot_log.txt`. |
| **Send a fix** | Fork, fix, test, open a pull request. Small, focused changes are easiest to review and merge. |
| **Improve the docs** | The README, the development guide and the design notes are part of the project. Corrections and clearer steps are welcome. |

## Before you open an issue or a PR

- Try the [newest release](https://github.com/NotRexed/Hookshot/releases/latest) first. It includes every fix before it.
- Search the open issues. If yours is already there, add your details instead of opening another.
- English, please.
- One topic per issue or pull request. Do not mix a fix with a refactor or a batch of unrelated changes.
- Report the version you actually ran: the release name, or the first line of `hookshot_log.txt`.
- Test without ReShade, shader-cache tools, overlays and frame-generation injectors when you report a graphics or
  performance problem. They change what Hookshot does.

## Development setup

You need your own supported dump: program `0400486024DEC000`, update v131072, build `e0f3ccfa`. Other versions do
not work, and the build reads the dump where it lies without changing it. Builds need about 60 GB free and a run uses
about 10 GB of RAM. Build on an SSD if you can: a hard drive works, but takes much longer, and the builder warns you
before it starts.

### Windows, from the command line

1. Install Python 3 from [python.org](https://www.python.org/downloads/) (not the Microsoft Store build: `python -I`
   does not see packages installed there) and the pipeline's packages:

   ```powershell
   python -m pip install -r requirements.txt
   ```

   (the second line: the button prompts' packages. Without them the game shows the Switch's icons)

2. Get [llvm-mingw](https://github.com/mstorsjo/llvm-mingw) (the `ucrt-x86_64` build) and set `HOOKSHOT_MINGW` to its
   folder. Without it the build falls back to emsdk's clang at `C:\emsdk` with Visual Studio's C++ tools and the
   Windows SDK (`HOOKSHOT_MSVC=1` forces that route).

3. Copy the pieces a clone does not carry from the `tools` folder Hookshot Builder installs. Run `HookshotBuilder.exe`
   from the latest release once (releases no longer carry a `HookshotBuilder.zip`): the folder is
   `%LOCALAPPDATA%\HookshotBuilder\versions\<version>\HookshotBuilder\tools`.

   | From `tools` | To | Variable |
   |---|---|---|
   | `tools\cpm` | anywhere | `RX_CPM` points at it (Vulkan headers and FFmpeg; without it, system headers or `HOOKSHOT_*_HEADERS`, the standalone build stops) |
   | `tools\wgpu-native` | anywhere | `WGPU_NATIVE` points at it (the WebGPU fallback renderer; without it the game is built without one) |
   | `tools\third_party` | `third_party\` | (its `.dll`, `.exe`, `.lib` and `.sha1` files, to the same places: `a64emit`, `lld-link`, `zbic` and SDL2) |
   | `tools\work` | `work\` | (hot-function list, link order, profile and icon; without it the game builds but runs slower) |

4. Run everything with `python -I` from the repository root.

### Linux

Either use the `Hookshot-<version>-x86_64.AppImage` from the Releases page, or build the environment and the AppImage
yourself:

```sh
scripts/linux/deps.sh       # LLVM, Python packages, FFmpeg with dav1d, SDL2, Vulkan headers
scripts/linux/appimage.sh   # the AppImage, in build-linux/
```

The scripts expect Ubuntu 22.04, the system CI builds the AppImage in: the AppImage carries that system's C library, so
the game it builds runs on glibc 2.35 and newer. On another distribution, build it in a container with the same system
(Docker or Podman, `DOCKER=podman`):

```sh
scripts/linux/docker.sh             # deps.sh, then appimage.sh, in scripts/linux/Dockerfile's Ubuntu 22.04
scripts/linux/docker.sh shell       # a shell in that environment, with the checkout at /src
```

The AppImage lands in `build-linux/` as above. The container keeps its own `build-linux/docker-deps` and
`build-linux/docker-vk-work`: parts built on the host link against its newer glibc and would not load on older systems.
The Dockerfile's packages are the workflow's (`tests/test_linux_docker_packages.py` checks that they stay the same).
Where SELinux is enforcing (Fedora, RHEL), the script runs the container with `--security-opt label=disable`: the
checkout is not labelled for containers, and relabelling it with `:z` would change it on the host.

The AppImage needs glibc 2.35 or newer and a Vulkan 1.3 driver with mesh shaders. `HOOKSHOT_JOBS` sets how many files
compile at once. The README's [Linux section](readme.md#on-linux) has the details and the runtime paths.

### NVIDIA DLSS (optional)

DLSS (`gpu/vk/vk_dlss.cpp`) is built in only with NVIDIA's DLSS SDK, which is NVIDIA's, under NVIDIA's own license, and
is never committed, bundled or uploaded: not in a PR, not in a release, not in a CI artifact. Fetch it for your own
builds with `scripts/ngx_sdk.py` (it prints NVIDIA's license notice and needs `--accept-nvidia-license` or a typed yes),
then build with `HOOKSHOT_NGX_SDK=<sdk folder>`:

```powershell
python -I scripts/ngx_sdk.py --platform windows C:\path\ngx-sdk     # headers, nvsdk_ngx_s.lib, nvngx_dlss(g).dll
$env:HOOKSHOT_NGX_SDK = "C:\path\ngx-sdk"
python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Hookshot-dlss"
```

On Windows the llvm-mingw build links NVIDIA's static `nvsdk_ngx_s.lib` (built for Microsoft's static runtime) into
`hookshot_vk.dll`, with `gpu/vk/vk_ngx_msvc_rt.cpp` and `host/mingw_msvc_shims.c` for what it needs of that runtime.
`make_game.py` copies `nvngx_dlss.dll` and DLSS Frame Generation's `nvngx_dlssg.dll` into the game folder's `ngx`
folder; never put NGX's core, `_nvngx.dll`, beside the game (it is the driver's). On Linux it is `--platform linux` (or
`scripts/linux/ngx-sdk.sh`), with `libnvsdk_ngx.a`, `libnvidia-ngx-dlss.so` and `libnvidia-ngx-dlssg.so`. A Windows
game with DLSS can be built on Linux too, with `RX_TARGET=windows` as below.
CI's `vulkan-dlss` workflow (`.github/workflows/vulkan-dlss.yml`) checks only that `hookshot_vk.dll` still links with the
SDK. It runs by hand, and on a push or pull request only when a file DLSS's build depends on changes; it accepts NVIDIA's
license for that run alone and uploads nothing.

Never share a game folder built with the SDK. A change to the pinned release updates the file table and SHA-256s in
`scripts/ngx_sdk.py` (and `tests/test_ngx_sdk.py`), and the release named in the builder's license text
(`builder/HookshotBuilder/MainWindow.xaml.cs`).

### Hookshot Builder

The builder is a .NET 8 WPF app. Open `builder/HookshotBuilder.sln` in Visual Studio 2022 with the .NET desktop
workload, or build it from a developer prompt:

```powershell
dotnet build builder/HookshotBuilder.sln
```

For a Windows installation that keeps its own state on an external SSD, see
[portable Builder setup and relocation](docs/windows-portable-builder.md).

You only need Visual Studio for the builder itself. The game build uses llvm-mingw.

The Windows app's **Build workers** choice applies to builds started from that window. **Automatic** keeps the existing
`HOOKSHOT_JOBS` override, or the build tools' free-memory policy when no override is set. A number overrides it for that
build's child process only; more workers need more memory and are not always faster. This choice is not saved: a new
window, including one started to update a game, uses Automatic again.

Its **Build memory** choice is saved with the build's other settings, and the updates use it too, as they show no menu.
**Automatic** keeps the free-memory policy. A number of GB sets
`HOOKSHOT_MEM_LIMIT_GB` for that build: the compilers may then reserve that much together, whatever is free now. The
compilers that always run (up to 4 on Windows) still run under a smaller number.

## Building

| Command | What it does |
|---|---|
| `python -I nx2/make_game.py --nup "C:\path\your-game.nup" --out "C:\Games\Hookshot" --language en-US` | Build a complete game folder from a `.nup`. A build takes about 40 minutes. |
| `python -I nx2/make_game.py --romfs "C:\path\romfs" --exefs "C:\path\exefs" --out "C:\Games\Hookshot"` | The same, from extracted folders. |
| `python -I nx2/make_game.py --check --nup "C:\path\your-game.nup"` | Check the game and its version without building. |
| `python -I nx2/dump.py --dump <folder with exefs/ and romfs/> --set` | Record the dump in `work/dump.txt`. |
| `python -I nx2/lift.py --standalone --write` | Lift the game to C in `work/sa/gen`. |
| `python -I nx2/build.py --standalone --jobs 4` | Compile `work/sa/build/hookshot.exe` and `hookshot_vk.dll`. |
| `powershell -File scripts/run-vk.ps1 -Window -Seconds 120` | Run the standalone build with the Vulkan back end and take screenshots in `work/runs/<name>/shots`. |
| `python -I scripts/prof.py <stacks file>` | Read a profile sampled with `HOS_PROFILE`, `HOS_SAMPLE` and `HOS_SAMPLE_FILE`. |

Use a separate `--work` folder or `OOT_WORK` if you keep more than one dump around. The lifted C and every build are
local artifacts; they are never committed.

Not measuring speed? [A faster build for development](docs/development.md#a-faster-build-for-development) compiles the
hot code at `-O1`, about a quarter less compile time. Never use it for fps, timings or a release.

## Testing

### Game-free checks

These need no dump and are what CI runs, on every pull request and every push to `main`. One command runs them all:

```powershell
python -I -B tests/run_all.py
```

It runs every `tests/test_*.py` and `nx2/test_*.py` in its own process, the C++ unit tests under `gpu/vk/tests` and a
byte-compile of every tracked Python file, goes on after a failure, and ends with a table (slowest first) and every
failure's output. Name a check to run only the ones whose path contains it (`python -I -B tests/run_all.py ub_`). A new
`test_*.py` file is picked up by itself: no workflow to edit. The slowest checks start first, by the times in
`tests/run_all_times.json`, which also split CI's Linux and Windows runs into two parts each (`--shard 1/2`,
`--shard 2/2`); a full run with `--write-times` refreshes this OS's times there.

Some checks compile a small C or C++ fixture and need a compiler (`g++`, `gcc` or `clang++` on PATH), the SPIR-V
validator (`spirv-val`), the SPIR-V or Vulkan headers (`HOOKSHOT_SPIRV_HEADERS` / `HOOKSHOT_VULKAN_HEADERS`, or the CPM
cache in `RX_CPM` or `build-linux/deps/cpm`), or the `zstandard` package. A check whose tool is missing is skipped and
says so; CI passes `--strict`, which fails it instead. A check that cannot run in CI (a GPU, the game, a Visual Studio
prompt), or runs on one OS only, says so in its own file with `RUN_ALL_SKIP`; `--no-skip` runs it anyway.

### Checks that need your dump or a run

These are slow and local, and they need `work/dump.txt` from `nx2/dump.py --set`:

| Command | What it checks |
|---|---|
| `python -I nx2/test_insn.py` | Every instruction form the game uses, Unicorn against the lifted C. |
| `python -I nx2/test_leaf.py --calls` | Whole functions and call trees on the relocated image, native against Unicorn. |
| `python -I nx2/census.py --nx2` | The instruction census per module. |
| `python -I nx2/lower_test.py work/lowfail` | Shader programs that failed to lower. |
| `powershell -File scripts/run-vk.ps1 -Window` | An in-game run: screenshots, the flash detector, frame dumps and the profiler. |

For a graphics fix, look at the screenshots in `work/runs/<name>/shots` and delete them afterwards; they reach
gigabytes and are derived from the game. The switches for scripted input, screenshots, frame dumps and profiles are in
the development guide's [reference](docs/development.md#switches-for-testing-and-debugging).

### When you add a fix or a check

- Add a game-free test where the behavior can be tested without the dump. The existing tests are small Python files
  under `tests/` or `nx2/` that read source files and run them, or compile a tiny C++ fixture.
- Name it `tests/test_*.py` or `nx2/test_*.py`: `tests/run_all.py` picks it up by itself and CI runs it on every pull
  request, so there is no workflow to edit. What it needs from the runner it declares in its own file, after its
  docstring and imports, as plain literals `tests/run_all.py` reads without importing the test (its docstring has
  the details); there is no central list to add a line to:

  ```python
  RUN_ALL_ARGS = ["--cxx", "{cxx}", "--headers", "{vulkan_headers}"]   # a compiler, headers, spirv-val ...
  RUN_ALL_MODULES = ["zstandard"]                                       # packages it imports
  RUN_ALL_SKIP = {"windows": "POSIX signals: the Linux guard"}          # cannot run in CI there (a GPU, the game,
                                                                        # one OS only), and why
  ```

  `{name}` is a tool the runner finds (`cxx`, `vulkan_headers`, `spirv_headers`, `spirv_val`, `glslang`, `dotnet`);
  a check whose tool is missing is skipped, or fails with `--strict`. A test that needs none of this declares nothing.
- Keep the test fast: every check shares one CI job with a 20-minute limit. Make it pass on both Ubuntu and Windows
  where it can.
- If the fix is in the renderer or the runtime, say what you saw before and after, on which GPU and driver, and in
  which part of the game.

## Code and data conventions

- Match the file you are editing: there is no formatter configuration, so use the indentation, comment style and
  naming around you. Do not reformat unrelated code in the same change.
- Keep OoT-specific code in new files where possible. `nx2/`, `host/` and `gpu/` are kept close to mkw-recomp so
  upstream fixes can be applied as a patch; keeping the paths and the shared files recognizable is what makes that
  work. Each sync names its new mkw-recomp base commit in the commit message.
- Do not modify `third_party/`. Its parts are unmodified upstream copies with their notices in `PROVENANCE.md`; add a
  new folder and a provenance note rather than editing one.
- Treat the local mkw-recomp and Eglantine checkouts as read-only references: read from them, run them, never edit
  them.
- New switches use the `RX_` prefix in the renderer and `HOS_` in the runtime. They are off, or neutral, unless
  asked for, and they do not cost anything while off. Document them where the surrounding switches are documented.
- The GPU replay rules in [design notes, section 4](docs/design.md#4-the-lessons-from-mkw-as-rules-here) are not
  suggestions: nothing the replay reads may come from live guest memory, waits are honoured per queue, and texture
  and sampler registrations stay in submission order. A change that reads guest memory at replay time is a
  regression, even if it looks right on one frame.
- Python runs with `-I`. Tests stay on the standard library so CI needs no install step; the pipeline's existing
  dependencies (lz4, numpy, capstone, unicorn, zstandard) are the exception, not a reason to add more. So are the
  button prompts' (fontTools, Pillow, astc-encoder-py, texture2ddecoder: `nx2/prompt_requirements.txt`), accepted for
  `nx2/prompt_assets.py` and `nx2/prompt_draw.py` alone, at build time. Maintainers' agents reviewing a change: check
  that these four are still used only there, and flag any other use. Keep line endings LF (`.gitattributes`).
- New code has to be GPL-compatible. See below.

## Commits and pull requests

1. Fork the repository and branch from `main`. Name the branch for the change: `fix/...`, `linux/...`, `builder/...`,
   `docs/...`.
2. Keep the pull request to one topic. Bug fixes do not carry refactors, and two unrelated fixes are two PRs.
3. Write the title as a Conventional Commit (see [Pull request titles](#pull-request-titles) below). Start the body
   with why the PR is needed, in one or two plain sentences a player could follow: the bug, the missing feature or the
   cost it removes ("The menu's text is cut off at 4K", "First starts spend 20 s waiting on a few shaders"). Then say
   what changed and how it was tested (GPU, driver, game area, fps before and after).
4. Link the issues the PR fixes, if there are any. Write `Fixes #123` in the body (one line per issue) so that merging
   the PR closes the issue. If the PR only addresses part of an issue, or is related to one, write `Part of #123` or
   `See #123` instead, so the issue stays open. Search the open issues before you open the PR: a report may already
   describe the bug you fixed.
5. Include screenshots when the change shows on screen: graphics, the menu, the start-up screen, an overlay, a
   message. Before and after for a fix, the new screen for a feature, and the sizes or settings it was checked at. A
   screenshot of the game is fine; the rule above is about game files, not pictures of the game.
6. Run the relevant checks. CI must pass; a change that only works on your machine is not ready. CI has no game, so
   also run the change in the game and put what you measured in the description: before and after on the same
   scene for a performance change, the bug gone for a fix. An untested change is opened as a draft.
7. Rebase or merge `main` when it has moved, and resolve conflicts in your branch rather than in the review.
8. Do not include build output, dumps, generated files or a `.gitignore` bypass. Check `git status`.

### Pull request titles

A pull request is squash-merged, so its title becomes the commit on `main`, and that commit decides the next release
and its changelog line. The title is a [Conventional Commit](https://www.conventionalcommits.org/): a type, an optional
scope in parentheses, a colon, and a short description. The *PR title* check fails until it is one; edit the title
and the check runs again.

| Type | For | Example from this repository |
|---|---|---|
| `feat` | something new for players or modders | `feat(menu): tabbed category layout for in-game options with bumper and Q/E navigation` |
| `fix` | a bug fix | `fix: preserve the changelog when release generation fails` |
| `perf` | faster, or less memory | `perf(linux): hand guest memory to the GPU as udmabuf dma-bufs, not host pointers` |
| `refactor` | a change with no effect on behaviour | `refactor(vk): ...` |
| `docs` | documentation only | `docs: document the built game's command-line arguments` |
| `test` | tests only | `test: cover pool-less NVN buffer mappings` |
| `ci` | the GitHub workflows | `ci: actions pinned by commit, least privilege, realistic timeouts, Dependabot and actionlint` |
| `build` | the build scripts, the AppImage, the builder package | `build: scope the shader cache stamp to its consumer` |
| `chore` | anything else | `chore: remove patch backup files added by mistake` |
| `revert` | undoing an earlier change | `revert: ...`, with the reverted PR in the body |

The scope is optional and names the part of the project: `vk`, `linux`, `builder`, `menu`, `video`, `runtime` and so
on. `feat` makes the next release a minor version, and `fix` and `perf` a patch (each first as a release candidate). A `!` after the type or scope (`feat(builder)!: ...`)
marks a breaking change. The other types make no release on their own. The commits inside a branch can be written
any way you like: only the title reaches `main`. The version rules are in [docs/versioning.md](docs/versioning.md).

Maintainer commits record AI assistance with `Assisted-by:` and `Co-Authored-By:` trailers. If you used a tool, add
the trailers that describe it; if you wrote the change yourself, there is nothing to add.

## Licensing

Hookshot is free software under the [GNU General Public License, version 3 or later](LICENSE). By submitting a change
you agree to license it under those terms. Only send code you can license that way. Do not paste code from a project
with an incompatible license, and do not paste decompiled game code or anything generated from the dump into a PR.

The third-party parts (Eden, suyu, Sirit, the Vulkan Memory Allocator, SDL, Opus, Dear ImGui, zstd, the Libertinus
fonts, zbic and the a64-kit tools) keep their own notices. Preserve them, and do not relicense their code.

NVIDIA's DLSS SDK is not one of them: it is never in the repository, and nothing of it may be added (see
[NVIDIA DLSS](#nvidia-dlss-optional)).

## Where to talk

- **Issues:** [use the forms](https://github.com/NotRexed/Hookshot/issues/new/choose).
- **Discord:** https://discord.gg/rexed. Use the `dev-work` channel for prs.
- **Newest release:** https://github.com/NotRexed/Hookshot/releases/latest

Thanks for testing, reporting and sending fixes. Every report from a different GPU and every area played further than
before moves the port forward.
