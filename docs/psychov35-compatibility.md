# PsychoV35 automatic upscaler compatibility

Native HDR and PsychoV35 remain player choices; Vanilla remains the default tone mapper. Selecting PsychoV35 automatically enables its compatible upscaler input routes.

## Settings and rollback

The compatibility switch `RX_VK_PSYCHOV35_DLSS` defaults on when unset or empty, with explicit `0` retaining a troubleshooting rollback. The shared temporal chain and independent FSR FG path use the same policy. This does not opt into FSR frame generation, change its 2x limitation, or enable HDR on an SDR display. Off and the spatial/FXAA replacement remain independent of this switch.

Set `RX_VK_PSYCHOV35_DLSS=0` in `hookshot_env.txt` and restart to disable the shared compatibility route and independent FSR FG RGBA admission. Removing that line, or setting it to `1`, enables them again. The existing DLSS HDR and FSR4 HDR disable switches still prevent those engines from accepting the HDR scene.

Supported shared engines are DLSS/DLAA, FSR4/native AA and FSR3.1. The legacy `RX_VK_FSR3_CHAIN=0` upscaler remains unsupported. A selected menu row does not prove an engine ran: verify the RGBA16F route and actual drawn/generated counts in the log. Existing actor/HUD ghosting and reset-frame fallback depend on the selected backend and are not solved by automatic compatibility.

## Validation

Compare compatibility on/off in the same scene, retain normal format/camera/depth rejection, and check cold launch, saved settings, scale, tone, HDR and frame-generation transitions. Record the tested commit, GPU/driver and OS. The remaining Lake Hylia/Field transfer report is tracked in [issue344](https://git.notrexed.dev/Hookshot/Hookshot/issues/344); the merged scene-permutation fix addresses the confirmed ranch cause, not every possible transition fallback. See [format/history and FG rules](temporal-history.md) for the backend details.

The tone-mapper control appears only with Native HDR. Set HDR Native before editing that row; skip it while HDR is Off or Expanded. Hiding that inapplicable control is independent of the upscaler. In the menu, select DLSS and then Native render resolution for DLAA. An older control bridge cannot select that state through a numeric Upscaler step; a cold launch with the saved DLAA choice can test it without changing the menu's behavior.
