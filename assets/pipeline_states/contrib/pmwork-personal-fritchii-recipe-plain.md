# pmwork-personal-fritchii-recipe-plain

Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).

- Who: pmwork-personal, converted from Fritchii's shader recipe (Frizti/Fritchii)
- GPU and driver: the recipe's recordings: NVIDIA, drivers 610.88 and 621.01; version 2 records, for every GPU
- Build: Fritchii v5.5's recipe for OoT v131072 (cache v54), converted against main at f915af9's lists
- Settings: none: target formats as the game asks for them, blend, masks, polygon mode and depth clamp from the recipe's render states
- Route: Fritchii's recorded sessions and predicted states; pipelines whose vertex input main's lists fix but whose program variant they do not: variant 0 with no answers, the variant nothing bound makes
- Pipelines: 11985

## How it was made

Converted, not recorded, like `pmwork-personal-fritchii-recipe` (its note has the details): Fritchii's recipe names each shader by
a fingerprint of the same bytes `code_key` hashes, and its render states map to these records by meaning. The recipe holds no
texture answers. These are its pipelines that main's lists do not hold, whose vertex input those lists fix (one vertex input for
the program's vertex stage, or a mesh draw) but whose program variant they do not (the program is not in them, or with more than
one variant). Each has variant 0 and no answers or questions: state_list_prepare prepares it with the variant the program's
translation makes with nothing bound, each texture as its instruction declares it (`seed_plain_of`). Where the game binds other
texture types the draw makes its own variant as before, and the prepared pipeline is not used. A build without that rule finds
no such variant and leaves these records out ("whose program variant was not made").
