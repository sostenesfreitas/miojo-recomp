// AMD FSR 4, one of the temporal upscaler's engines (vk_temporal.h; vk_fsr4.cpp), through the AMD FSR API. Only the API's MIT
// headers are built in (third_party/fidelityfx), and only in a build with HOOKSHOT_FSR (make_game.py --fsr, which also puts
// the camera jitter into the game: nx2/oot_jitter.py). AMD's signed amd_fidelityfx_loader_vk.dll and
// amd_fidelityfx_upscaler_vk.dll are loaded from the user's PC at run time; none ship with Hookshot. Linux: a Vulkan FSR 4
// library (such as fsr4-vk's amd_fidelityfx_upscaler_vk.so) in their place.
#pragma once

namespace mkwvk {

struct TemporalEngine;

/// Whether this build has FSR in it (HOOKSHOT_FSR)
bool fsr4_built();
/// Why FSR 4 cannot be chosen here (the Upscaler row says it), FSR_OK when it can or is not known yet
enum FsrWhy { FSR_OK, FSR_NOT_BUILT, FSR_ENV_OFF, FSR_NO_LIBRARY, FSR_NO_MODEL, FSR_WHY_COUNT };
FsrWhy fsr4_why();
/// Loads AMD's library once and asks it for an FSR 4 upscaler for this device, so fsr4_why knows before the first upscale
/// (the menu, when it opens). Nothing in a build without FSR, or before the device is made
void fsr4_probe();
/// The window closing (mkwvk_close_asked, before the pipeline cache's save): the tuned layers' background builds stopped,
/// the one in the driver waited for (10 s at most). Nothing in a build without FSR
void fsr4_close();
/// (linux) a Vulkan FSR 4 library is there and records with descriptor buffers: the device has to enable them
bool fsr4_wants_descriptor_buffer();
/// FSR 4 as the temporal upscaler's engine (FSR 4 and its native AA on the Upscaler row); nullptr in a build without it
const TemporalEngine* fsr4_engine();

}  // namespace mkwvk
