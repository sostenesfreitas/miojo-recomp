# pmwork-personal-fritchii-recipe

Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).

- Who: pmwork-personal, converted from Fritchii's shader recipe (Frizti/Fritchii)
- GPU and driver: the recipe's recordings: NVIDIA, drivers 610.88 and 621.01; version 2 records, for every GPU
- Build: Fritchii v5.5's recipe for OoT v131072 (cache v54), converted against main at f915af9's lists
- Settings: none: target formats as the game asks for them, blend, masks, polygon mode and depth clamp from the recipe's render states
- Route: Fritchii's recorded sessions and predicted states; only pipelines whose vertex input and program variant main's lists already fix, or that take none (mesh draws, programs without textures)
- Pipelines: 1461

## How it was made

Not recorded by Hookshot: converted from the records another emulator keeps. Fritchii (a Lichii fork) ships a "recipe" for
OoT: the pipelines its testers' sessions drew, and states it predicts, with each shader named by a fingerprint and no code
(`nx2_recipe.h`, `nx2_shader_extract.h` on its `oot-gather-fix` branch). The fingerprint (SHA-256 of the stage's control
section with four bytes masked, then its code area) and Hookshot's code key (`vk_shader.cpp` `code_key`) hash the same bytes,
so the converter's own `hookshot_shaders.bin` names the recipe's shaders by code key (41,165 of 41,216). The render state
maps by meaning: target and depth formats (Maxwell render-target formats to NVN's), samples, topology class, blend factors and
equations (to `nvn_blend_st`), channel mask, polygon mode, depth clamp, alpha to coverage.

The recipe was recorded with dynamic vertex input, so it has no vertex formats, and Hookshot numbers texture answers by its
own translation, so it has no variants. A pipeline is here only when main's lists fix both for its program (one vertex input
and one variant there), or it needs neither: mesh draws take no vertex input, and a program none of whose stages asks about
a texture or image has variant 0. Pipelines main's lists hold already are left out.

Checked against main's lists: of the recipe's pipelines they hold too, converted the same way, 13,323 of 19,738 match one of
their records field for field. The rest differ in draw state the game uses more than one of (blend, write masks, depth
clamp), or in blend enables past the last target, which Lichii clears and Hookshot keys only on a GPU without dynamic blend
state. Point-topology states the recipe predicts for programs main's lists draw as triangles are left out.
