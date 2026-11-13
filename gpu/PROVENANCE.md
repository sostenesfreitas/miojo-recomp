# gpu/: the standalone build's renderer

`render/`, `compat/` and `third_party/suyu/` are TOTKPort's renderer (`a64-kit/gpu/`, commit `e88744a`,
2026-10-31): the native NVN layer's replay on WebGPU (wgpu-native on the PC), its texture, buffer, pipeline and
present code, and suyu's shader recompiler with TOTKPort's WGSL backend. `host/nvn_native.c` and `host/display.c`
are the API side and the window, from the same commit (see `host/PROVENANCE.md`).

Licensing as in TOTKPort's `NOTICE.md`: suyu's parts are GPL-2.0-or-later / GPL-3.0-or-later per file (texts in
`third_party/suyu/`); `render/nvn_api.h` (h1k421, as adapted by jhaizhou), the NVN layer's design (jhaizhou's
d2r-mobile / MacNativeGames) and the FSR 1 port in `render/rx_present_fx.cpp` are MIT; `render/renderdoc_app.h` is
RenderDoc's MIT header. wgpu-native (MIT or Apache-2.0) is not vendored: `nx2/build.py` takes TOTKPort's build of it
(`WGPU_NATIVE` for another).

mkw-recomp's changes, for the Switch 2:
- **Ampere shaders.** `third_party/suyu/shader_recompiler/frontend/ampere/` is Eglantine's Ampere-to-Maxwell
  lowering (header-only; Eden Emulator Project, GPL-3.0-or-later, from the `work/eg` clone). `environment.h` gains
  `TranslateProgramAddress` and the control flow passes indirect branch targets through it, as in Eglantine.
- **NVN2 shader packages** (`render/rx_shader.cpp` `read_nvn_code`): the control section's layout (magic
  `0x98761234`, code length `+0x38`, c1 constants `+0x40..+0x44`, stage `+0x56`, workgroup `+0x58`), the 128-byte
  graphics program header, and the lowering before the Maxwell frontend.
- **The Switch 2's GOB swizzle** (the Tegra X1's with offset bits 5 and 6 swapped, from Eglantine's native tiling):
  `third_party/suyu/video_core/textures/decoders.h` (the CPU swizzle masks), the two WGSL unswizzles in
  `render/rx_texture_gpu.cpp`, and `bl_offset` in `render/rx_nvn.cpp`.
- **The 64-bit guest window**: per-page tables in `render/rx_buffer.cpp` are committed untouched (`PageTable`).
- **The window crop** (`rx_present_crop`): MKW renders at a dynamic size into a 3840x2160 window texture and crops
  it; the crop is copied out and presented.

The renderer the game uses by default is no longer this one but the Vulkan back end in `vk/` (`vk/VULKAN.md`); this
one stays as the fallback (on Windows, in a build made with wgpu-native). mkw-recomp's earlier Vulkan stand-in
(`mkw_gpu.cpp`, `build-gpu.bat`) did not come over. `shadertool.cpp` (built by `build-shadertool.bat` against the
Eglantine clone) runs dumped NVN2 programs through Eglantine's recompiler offline (SPIR-V; it prints what each program
reads from the constant banks).

Hookshot's AMD FSR 4 (`vk/vk_fsr4.cpp`) was first written for Lichii's OoT branch (madebyjordan/Lichii-Hyrule,
`renderer_vulkan/vk_fsr4_upscaler.cpp`; GPL-3.0-or-later), with its Linux path (a Vulkan FSR 4 library through dlopen) by
N3oTraX. It draws with the frame chain of hckrman101's NVIDIA DLSS (#841), moved into `vk/vk_temporal.cpp` so both
upscalers share it. `third_party/fidelityfx/` is AMD's FSR API headers from FidelityFX SDK v2.4.0 (MIT, `LICENSE.md`);
AMD's signed DLLs are loaded from the user's PC at run time and are not vendored.

The HDR tone mapper's PsychoV35 (`vk/eden/shader_recompiler/ir_opt/psychov35.h`) is the scene tone map of ShortFuse's RenoDX
mod for this game (clshortfuse/renodx, `src/games/zelda-oot-oz/scene_tonemap.slangi`, and Carlos Lopez's
`src/shaders/tonemap/psychov/test35.hlsl`; MIT, `ir_opt/LICENSE-renodx.md`; `tests/test_psychov35.py`'s reference is a
port of the same files under it), reduced to the path the mod takes at its defaults and written over the IR
emitter. `ir_opt/scene_tonemap_pass.h`, which finds the game's fit in the IR and puts it there, is Hookshot's.
