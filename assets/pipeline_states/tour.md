# The pipeline tour

Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).

- Who: the pipeline tour (scripts/pipeline_tour.py)
- GPU and driver: NVIDIA GeForce RTX 3090, driver 620.70.06 (Linux)
- Build: main of 2026-11-10 with the pipeline tour (#2048)
- Settings: 640x360 output, game upscaler at 50%, no DLSS/FSR, HDR off, frame generation off, mesh shaders on (HKSTATE2 records map to any settings)
- Route: Pipeline tour runs 2-4 plus gap batches: opening (new save), child and adult overworld stops, all child and adult dungeons, Bottom of the Well, Ice Cavern, Gerudo Training Ground, Ganon's Castle, Zora areas, Gerudo Valley, Haunted Wasteland, Lake Hylia, Graveyard, Death Mountain. Partial: not yet bosses, underwater, Game Over, weather
- Pipelines: 11227
