# gumminaomi-tour-every-map-actor-cutscene

Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).

- Who: GummiNaomi
- GPU and driver: RTX 4070 SUPER, driver 617.42, Windows 11
- Build: test/play at af15c4a5: main at f3fe1763 (#260) with opt-in renderer branches (the late pages' fix on, the rest off) and the tour's opt-in host change (HOS_OOT_TELEPORT_EXTRA, HOS_OOT_TOUR_SKIP_EVENTS), which changes no shaders
- Settings: no mods; the game's defaults on this PC (Auto preset: DLSS, 2048x1152, 60 fps), headless; RX_VK_OCC_QUERIES=0 (every draw drawn), recorded cold (RX_VK_PRECOMPILE=0)
- Route: a scripted tour through the control bridge: every map at each of its start positions (370), child and adult, a camera turn each, the overworld at noon and at midnight; every actor type with a model (2,086), 16 at a time beside Link; the 480 cutscenes by name; the menus; a second pass of the maps
- Pipelines: 22987
