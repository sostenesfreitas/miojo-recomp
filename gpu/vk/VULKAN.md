# The Vulkan back end (hookshot_vk.dll)

> This is the back end's development log from mkw-recomp, kept for its reasons and not updated since. Names and
> defaults have moved: the DLL is `hookshot_vk.dll` (`hookshot_vk.so` on Linux) and the default renderer, with
> `RX_BACKEND` only to pick another; the image and upload code of `vk_image.cpp` and `vk_upload.cpp` is in
> `vk_texture.cpp`; a draw whose pipeline is not ready waits with `RX_VK_PIPE_SKIP=0` (there is no `RX_VK_PIPE_WAIT`);
> `RX_VK_DUMP` takes a present number and writes PNGs; `RX_VK_FRAMEGEN` only turns frame generation off (the Insert
> menu turns it on); the texture budget `RX_VK_VRAM_MB` defaults to 70% of what the driver allows (at most 6 GB),
> `RX_VK_TEX_MAX_MB` to 1536 and `RX_VK_MIRROR_MB` to a share of the card; and `eden/` carries Hookshot's changes.
> The current switches are in [docs/development.md](../../docs/development.md#switches-for-testing-and-debugging).

The game renders through `hookshot_vk.dll` (`RX_BACKEND` unset or `vulkan`) instead of the WebGPU renderer in the executable
(`gpu/render`). `host/rx_backend.c` owns the `rx_*` names the host calls. It loads the DLL, passes it the host
functions (`vk_api.h` `MkwVkHost`) and routes the calls to the table the DLL returns (`MkwVkRenderer`). If the DLL
or Vulkan is missing, the WebGPU renderer stays in place.

Why a DLL of its own: it carries Eden's shader recompiler (`gpu/vk/eden`, copied unmodified from Eglantine, with its
SPIR-V back end and NVN2 task/mesh support) and sirit (`gpu/vk/sirit`, v1.0.5 plus Eden's mesh shader ops patch).
Their C++ namespaces (`Shader::`, `Common::`) are those of the WebGPU renderer's suyu copy, so the two cannot be
linked into one binary.

Why Vulkan, after WebGPU:

- **Mesh shaders.** MKW draws high-detail geometry with NVN2 task/mesh programs, about 60 draws a frame. WebGPU has
  no mesh stage (the WebGPU renderer drops them: pop-in, low LODs). `VK_EXT_mesh_shader` runs them as they are.
- **Guest memory in place.** `VK_EXT_external_memory_host` imports the game's pools as buffers. GPU writes land in
  guest memory and CPU writes are seen by the GPU, with no mirrored buffer cache, write-back syncs or upload
  streams (the WebGPU renderer's biggest costs). Hot read-only ranges can later be mirrored to device memory, as
  totk-pc-recomp does.
- **SPIR-V straight from the IR.** No WGSL, no naga, and none of the WebGPU workarounds the WGSL back end carries
  (texture swizzle and border emulation, d16 quantization, storage format casts, vertex pulling).
- **No validation or usage tracking on every call**, and bindless descriptors like NVN's own pools.

Reference: totk-pc-recomp (MIT, `work/ext/totk-pc-recomp`, local clone, read only). It has an NVN-on-Vulkan back end
for TOTK: pools imported as host memory, a recorder thread, graphics pipeline libraries, and a uniform snapshot.

## Status

- Milestone 1 (done, 4649fa5): the device (Vulkan 1.3; the RTX 4060 Ti reports mesh shaders, host memory import and
  graphics pipeline libraries), the window's swapchain, and presents (a clear colour). The replay is a stub that
  completes submissions.
- Milestone 2 (in progress): the replay draws.
  1. Guest memory (done): each NVN pool imported as host memory with a buffer over it (`vk_memory.cpp`). MKW has
     ~2,400 pools at the title, under NVIDIA's ~4,096 allocation limit; merging adjacent pools into one import is
     the fix if that grows. GPU address -> (VkBuffer, offset, device address).
  2. NVN state (done): the record parser and state of `gpu/render/rx_nvn.cpp` carried over (`vk_replay.cpp`);
     submits recorded into command buffers from a ring and signalled on a timeline semaphore (the NVN fences);
     render targets as images by storage address (VMA, GENERAL layout, full barriers); colour / depth-stencil
     clears; the stream's memory writes on the GPU timeline; presents blit the window texture's image to the
     swapchain. Fixed on the way: the present waited on a timeline value another thread had counted but not yet
     submitted, which blocks a single queue for good.
     Open: with draws and dispatches not made, the game stops at the title after ~80 frames (its job system waits;
     no fence wait is outstanding): most likely a compute result it reads back. Draws and dispatches come next.
  3. Shaders: an `Environment` for Eden's recompiler like the WebGPU renderer's `NvnEnvironment`
     (`gpu/render/rx_shader.cpp`: NVN2 control section, code at +0x50, cbuf reads, texture types from the NVN
     texture pool), then the SPIR-V back end; bindings in Eden's layout (`backend/bindings.h`, as its
     renderer_vulkan does).
  4. Textures: block linear and ASTC decode on the CPU (suyu's `video_core/textures` decoders, copied into the
     DLL), BC formats uploaded as they are; render targets as GPU images found by address.
  5. Pipelines: dynamic rendering, NVN state -> VkPipeline, push descriptors.
  6. Present: the window texture's image blitted to the swapchain.
  Note: with guest memory shared, the stream's memory writes (UPDATE_UB, COPY_B2B, SIGNAL_EVENT, REPORT_COUNTER)
  must happen on the GPU timeline (vkCmdUpdateBuffer, vkCmdCopyBuffer, timestamp queries), not on the CPU at replay
  time as the WebGPU renderer does (its buffers are copies). RENDER_COND maps to VK_EXT_conditional_rendering.
- Milestone 3: compute, indirect draws/dispatches, mesh/task draws, copies and clears, counters and queries.
- Milestone 4: performance: device-local mirrors of hot ranges, pipeline libraries and a pipeline cache, a recorder
  thread. Then Vulkan becomes the default.

## Status (Nov 2026): it renders

The replay draws, dispatches, clears, copies and presents. A frame dumped with `RX_VK_DUMP=<present>` is the game's
map screen, right way up and in colour, with its textures crisp.

What it took, in the order it blocked a picture:

- **Pools were imported eagerly** on the first submit (~2,100 NVN pools, tens of seconds). They are imported on
  first use now (vk_memory.cpp).
- **`NVN_FORMAT_RGBA8` (37) was missing** from the table, so the 4K window texture had no `VkImage` at all. The
  RGBA8/RGB8/sRGB family is in vk_formats.cpp now.
- **The window texture is written through a view in another view format** (RGBA8 while the texture says RGBA8), so
  the present path keyed past the image the game actually rendered into: `rendered_at()` finds it by storage, and
  `present()` blits that (vk_image.cpp, vk_replay.cpp).
- **Transfers were not replayed.** `COPY_T2T/T2B/B2T` and `CLEAR_TEX` now use totk-pc-recomp's implementations
  (`sub_resolve`, `buffer_side`, `copy_buffer_image`, `copy_t2t`, `clear_tex`) in vk_xfer.cpp.
- **Textures were decoded into garbage.** The RTX 4060 Ti does not sample ASTC (`textureCompressionASTC_LDR` is
  NVIDIA's gap), and MKW's map, terrain and character art are ASTC, so every one is decoded on upload. The first
  version hand-rolled the block-linear unswizzle and used totk-pc-recomp's decoder: the result had the right colours
  in the right places but every detail shredded into block-sized noise (text unreadable, a dither lattice over flat
  areas). Dumping a decoded texture (a switch of the time, since removed) showed the corruption was already there, before
  any draw. The fix is to stop guessing: `gpu/vk/vk_upload.cpp` now calls suyu's own `Tegra::Texture::UnswizzleTexture`
  and `Tegra::Texture::ASTC::DecompressSerial` (`gpu/third_party/suyu/video_core/textures/`), the exact pair the
  working WebGPU back end decodes with in rx_texture.cpp. Those two files are built into `mkw_vk.dll` now
  (nx2/build.py `vk_sources()`, with suyu's root on the include path). The dumped map texture is pixel-crisp
  afterwards, "WORLD MAP" and the course names legible.
- **Backface culling threw away every triangle.** The viewport is flipped with a negative height to turn NVN's
  +y-up NDC into Vulkan's y-down, which already reverses the screen-space winding, so NVN's front face maps
  straight across; telling Vulkan the opposite (as the code did, "the flipped viewport mirrors the winding") culled
  the scene and left a black frame. `RX_VK_NOCULL` was what proved it: 199,300 distinct colours instead of 1.
- **Image cache is budgeted**: least-recently-used images are retired past `RX_VK_VRAM_MB` (2048 default) and
  destroyed a few frames later, when recorded work cannot still read them (vk_image.cpp `sweep_images`). Texture
  churn had taken the cache to 583 images and climbing.

## The crash at `mkw_vk.dll+0xeb45e`, solved

Not a race. The DLL's include path had suyu's root (`gpu/third_party/suyu`, added for its texture decoders) before
`gpu/vk/eden`, so `#include "shader_recompiler/shader_info.h"` found suyu's copy, whose `Shader::Info` lacks Eden's
`nvn_mesh_stage` and `uses_quad_shuffles`. Objects compiled before that change (Eden's recompiler) and after it
(`vk_draw.cpp`) disagreed on the layout of `Info`, and the descriptor walk read a garbage vector at `this+0x238`. The
object cache leaves `-I` folders out of its keys, which is why nothing rebuilt to show it. nx2/build.py now puts
Eden first and carries `-DMKWVK_INCLUDES=2` so the keys change with the order. (The "descriptor list is not host
memory" guards added while chasing it refused every draw and are gone.)

## The renderer after the rewrite (Nov 1 evening)

Designed after totk-pc-recomp's runtime/vk_*.c (MIT) and Eden's renderer_vulkan, both read in full for it:

- **Textures** (`vk_texture.cpp`): one `VkImage` per texture storage with every level and layer, keyed by storage
  address, size, levels, layout and format class (bytes per block for colour, the family for BC / ASTC, the format
  for depth): a texture rendered as RGBA8 and sampled as RGBA8_SRGB is one image (`MUTABLE_FORMAT`). Views per use:
  sampled (the shader's type: 1D/2D/3D/cube/arrays, NVN swizzles, X formats' alpha one), attachments (one level and
  layer; 3D slices through `2D_ARRAY_COMPATIBLE`), storage. Uploaded at first use and again when the CPU wrote the
  storage (page write generations, `vk_memory.cpp`, from the host's soft write watch); not once the GPU wrote the
  image, unless the texture was finalized since. ASTC is decoded on the GPU by Eden's `astc_decoder.comp`
  (`host_shaders/`, with the Switch 2 tiling), straight from the block-linear storage; other formats are unswizzled
  on the CPU into the staging memory. Huge textures leave out top levels (`RX_VK_TEX_MAX_DIM` 2048,
  `RX_VK_TEX_MAX_MB` 512: the terrain arrays are 8192 x 8192 x 68 ASTC, 24 GB as RGBA8). New images are cleared.
  An LRU budget (`RX_VK_VRAM_MB`, 4096, less when `VK_EXT_memory_budget` says the process is near its share).
  Images are VRAM only (`requiredFlags`): VMA's fallback to system memory is what ate the user's RAM once.
- **Uploads never end a pass**: each recording has a second command buffer, submitted right before it, for uploads,
  new images' layout changes and mirror syncs.
- **Passes** stay open while the targets stay; clears run inside them (`vkCmdClearAttachments`, cut by the scissor,
  a full-screen triangle for channel masks); a target the fragment shader does not write keeps its texels (write
  mask 0); duplicate attachments are left unattached; draws without targets get the viewport's area.
- **Descriptors**: one set in Eden's order, pushed (`VK_KHR_push_descriptor`) when it fits; storage images, texel
  buffers, custom border colours, min/max samplers. Handles resolve once a recording (`handle_hits`).
- **Pipelines**: whatever Vulkan 1.3 and `VK_EXT_extended_dynamic_state3` take as dynamic is dynamic; graphics
  programs are translated and pipelines built on worker threads (`vk_jobs.cpp`). A draw whose program or pipeline
  is not ready is left out when its targets hold an earlier frame's picture and waits otherwise (TOTK's
  `TOTK_PIPE_SKIP` rule; `RX_VK_PIPE_WAIT=1` always waits). The pipeline cache persists (`mkw_vk_pipelines.bin`).
- **Uniform updates** (`UPDATE_UB`) go to guest memory at the end of their recording; draws after one bind a copy
  of the range with it applied (`ub_overlay`).
- **The VRAM mirror** (`vk_memory.cpp`, TOTK's): pools bound as buffers get a device-local copy at the next
  submission; CPU-written pages are copied in at each submission's first use, GPU writes go back to guest memory at
  its end. `RX_VK_MIRROR=0` turns it off, `RX_VK_MIRROR_MB` (3072) is its budget.
- **Mesh shading**: `NOP_DRAW_MESH` records NVN2's DrawMeshTasks*; programs whose code writes the internal stage
  buffer are the task / mesh stages (marked in their headers as Eden's GraphicsEnvironment does). Unverified.
- **Frame interpolation** (`vk_framegen.cpp`, `RX_VK_FRAMEGEN=1`): the WebGPU renderer's, ported to GLSL
  (`shaders/`, compiled with glslang): made frames between the game's, shown evenly by a display thread.
- **Headless**: without a window presents render offscreen; `RX_VK_SHOTS=<dir>` writes every `RX_VK_SHOT_EVERY`-th
  as a PNG. `work/run-vk.ps1` runs it (refuses while Siege runs, stops past `-MaxGB` of working set);
  `work/vksum.py <run>` summarises the log.
