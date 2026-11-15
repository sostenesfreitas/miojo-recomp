# BAKE — offline NVN2 shader bake (developer tooling)

Branch `bake-merge` from `main` 15ec28d. Merges the four `bake-*` race
entries into one shippable, CPU-only, repo-only toolset: ROM/container
enumeration → offline seed-to-SPIR-V translation (no Vulkan device) → bank
emission (portable manifest **and** the live `hookshot_programs.bin` format)
→ read-back verify. No game files, no GPU, no pushes; outputs stay in
`work/` (gitignored) beside the repo, never committed.

Status: **working end-to-end, all tests pass** (results in §7).

## 0. What landed (best-of-breed)

| file | from | what it is |
|---|---|---|
| `gpu/shaderbatch.cpp` | bake-mimo c60fd1d (verbatim) | offline translator: seed → SPIR-V + exact runtime program keys (graphics/compute/task/mesh), no Vulkan device |
| `nx2/bake_mimo.py` | bake-mimo c60fd1d (+1 probe path, see below) | driver: `enumerate` / `translate` / `bank` / `all`; builds shaderbatch against the vendored recompiler, objects cached under the output dir |
| `nx2/bake.py` | bake-spark (manifest + `verify` + stamp-trap tests) | `bake` (seed → combo manifest `bake_bank.json`, resume cursor), `translate` (shaderbatch wrapper merging exact keys into the manifest), `verify` (portable read-back), `stamp`, `census` argv printer |
| `nx2/bake_ds/` (`romenum.py`, `bake_ds.cpp`, `build.sh`, `bake_all.py`, `fixture.py`) | bake-deepseek 6db36ff (verified per MiMo Appendix B, see §5.4) | container enumeration incl. zstd+dictionaries via libzstd; offline translator + **live-bank writer** (`hookshot_programs.bin` with full `disk_stamp`) + `verify` (a copy of the game's `program_cache_load` reader); end-to-end `bake_all.py` with `--target` feature profiles |
| `nx2/bake_glm/lower_probe.cpp` | bake-glm | Ampere encoding probe: hand-built EXIT / MOV.imm through the real `LowerToMaxwell` |
| `tests/test_bake_skeleton.py` | bake-spark | manifest determinism/dedupe/resume, exact-key translate + SPIR-V, stamp-trap, portable verify |
| `tests/test_bake_mimo.py` | bake-mimo c60fd1d | fixture archives → enumerate → translate → bank, FNV round-trip, `spirv-val` on every module |
| `tests/test_bake_ds.py` | bake-deepseek | census parity, Python key mirror, translate + 32 modules + bank read-back (needs `--tool`) |
| `docs/BAKE.md` | this file | the single merged doc (replaces `BAKE-SPARK.md`, `BAKE-MIMO.md`, `BAKE-DEEPSEEK.md`, `BAKE-GLM.md`) |

One-line change vs the sources: `nx2/bake_mimo.py:spirv_headers` also
probes `<repo>/buildext/cpm` (the in-tree CPM cache;
`spark#1` below). `nx2/bake.py`/`tests/test_bake_skeleton.py` reference the
mesh-capable translator (c60fd1d) and its status vocabulary. Everything else
is verbatim from its source branch.

Scope: developer tooling only. No `gpu/vk/*.cpp` hot path is touched
(`git diff main..bake-merge -- gpu/vk` is empty) and no default game behavior
changes — the game never calls this code; it consumes the artifacts on a
Vulkan machine (`prepare_run`/`precompile_run`) exactly as it consumes the
builder's seed today.

## 1. Background (why an offline bake is the same path)

Three ordered steps already run inside `started_up()`
(`gpu/vk/vk_draw.cpp:520-543`): `prepare_run` (translate every seed program,
`:5423`), `predict_run` (`:5653`), `precompile_run` (`:5537`). The seed is
`hookshot_shaders.bin` (`HKSEED01`, `nx2/shader_census.py:19-21,289-294`),
read by `seed_open` (`vk_shader.cpp:1844`) and translated by `seed_program`
(`:1893`) with a **blank `NvnState`** — the translator is already driven with
no game state along this path, so the offline tools mirror it exactly.

Identity (the whole bake is keyed on this):

| key | where | covers |
|---|---|---|
| stage `code_key` | `vk_shader.cpp:99-103,135-137` | FNV-1a 64 over `control[0,0xd8)` then `code[0,end)` |
| compute program key | `:1698` | the stage's `code_key` verbatim |
| graphics program key | `:733-743` (`graphics_key`) | `vs.key ^ fs.key*31`, FNV-mixed with used-generic attribute types + written colour types |
| mesh program key | `:744-752` (`mesh_key`) | `FNV("mesh")` + task/fs keys + primitive SPH + written types |
| variant | `:642-647` (`answers_hash`) | per texture/image answer actually bound |
| disk stamp | `:837-864`, `nx2/translator_files.py` | translator SHA-1 + depth bit + ABI sizes + env allowlist |

Deduped on `code_key` (not the census's advisory SHA-1: `seed_open`
recomputes FNV either way, `:1872`), a stage is the same two runs of bytes
from ROM archives and from guest RAM at draw time — so a ROM miner is exact.

## 2. Running it

All three drivers are independent entry points over the same seed format;
none reads the game folder unless `--romfs` points at a player dump, and
outputs go under `--out`/`--bank` (default `work/...`, gitignored):

```sh
# MiMo driver: ROM -> seed + manifest -> SPIR-V -> portable bank
python3 -I nx2/bake_mimo.py all --romfs <dump>/romfs --out work/bake
python3 -I nx2/bake_mimo.py enumerate --romfs <dump>/romfs --out work/bake
python3 -I nx2/bake_mimo.py translate --out work/bake [--limit N]
python3 -I nx2/bake_mimo.py bank --out work/bake

# Spark manifest driver: seed -> combos -> portable manifest -> verify
python3 nx2/bake.py bake --seed <hookshot_shaders.bin> --bank <dir> [--resume]
python3 nx2/bake.py translate --seed <hookshot_shaders.bin> --bank <dir>
python3 nx2/bake.py verify --bank <dir>
python3 nx2/bake.py stamp

# DeepSeek driver: tree -> seed -> live bank -> verify (needs build.sh first)
nx2/bake_ds/build.sh                       # -> ${TMPDIR:-/tmp}/hookshot-ds/bake_ds
python3 nx2/bake_ds/bake_all.py all --romfs <dir> --out <dir> --tool ${TMPDIR:-/tmp}/hookshot-ds/bake_ds [--target plain|tensor|tensor-software|rdna2|gfx1201|query]
python3 nx2/bake_ds/bake_all.py verify --romfs <seed-dir> --out <bank-dir> --tool <tool> [--target <profile>]

# Ampere encoding probe (needs a C++23 compiler + SPIR-V headers; cf. bake_mimo)
# hand-built EXIT / MOV.imm -> real LowerToMaxwell; exit 0 = all lowered
```

Toolchains: shaderbatch needs a C++23 compiler (`g++`/`clang++`, `-std=c++23`
or `-std=c++2b`) and the SPIR-V headers (`HOOKSHOT_SPIRV_HEADERS`, else the
`buildext/cpm` cache). bake_ds additionally needs the Vulkan headers plus
boost/fmt (same CPM externals the game build uses; `EXT=...` overrides).

Translator stamp (all three agree): `56b84e3925680030` on this tree. The
formula is `sorted(translator-tree) + [vk_shader.cpp, vk_bank.cpp,
vk_state.h]` appended **after** the sort (`nx2/translator_files.py`, which
`nx2/build.py` and the three bake tools all call; `vk_api.h` left it in
2026-11); see §6 trap #4.

## 3. What each leg does

**Enumerate** (`bake_mimo.enumerate_romfs` / `romenum.enumerate_tree`):
walks `Shader/`, `Layout2Shader/`, `Lib/`, `Movie/`, `Effect/` through BNSH,
BAHS, NDSR, NDVE, `.vsetb` BYML, SARC and zstd `.zs` (dictionaries from
`Pack/ZsDic.pack.zs`); dedupes stages on `code_key`; keeps per-stage
provenance (`file#container#program`). Writes byte-shape-identical
`HKSEED01` plus `manifest.json` (`HKBKM01`) / `enum.json`. Compressed files
without a decoder are *counted and reported*, never silently skipped.

**Translate** (`gpu/shaderbatch.cpp`, built by `bake_mimo.build_tool`):
per stage mirrors `parse_code` (capture → `LowerToMaxwell` → Maxwell words);
per program mirrors `seed_program`'s kind/input-variant choice and the
runtime's `RuntimeInfo` per stage (vertex `vrt`, fragment with the previous
stage's stores + `GuardFragmentOutputs`, compute `RuntimeInfo{}`, task/mesh
via `translate_mesh_into` with `prim=null` exactly as the seed path passes
it); emits SPIR-V per `<progkey>_<stage>.spv` with `stages.tsv`/`programs.tsv`.
Statuses are machine stop-lines: `OK`, `LOWER_ERROR` (+ the refusing opcode),
`TRANSLATE_ERROR`, `EMIT_ERROR`, `HEADER_ONLY` (graphics `len <= 0x80+16`),
`NOT_ATTEMPTED`, `INVALID_STAGE`. No mesh skip: task/mesh registers are
packed from the control sections (`mesh_registers_of`, mirror of
`vk_shader.cpp:1311-1326`) and the pair emits `MeshEXT`/`TaskEXT` modules.

**Bank, portable** (`bake_mimo.make_bank` / `bake.bake_seed`): `bank.json`
(`HKBANK01`: counts, `translator_stamp`, assumptions) + `stages.jsonl` /
`programs.jsonl` + `spv/`; every pipeline row `PENDING_GPU` (pipelines need a
`VkDevice`). `bake.py` adds the combo manifest: per-program input variants
× plausible pipeline states (tight cap, default 8/program), atomic-rename
`bake_bank.json` with a resume cursor (`done_programs`, idempotent), and
`translation.by_program` merging the exact runtime keys where translation ran.

**Bank, live** (`bake_ds translate`): the back end's own
`hookshot_programs.bin` (`kDiskMagic`, full `disk_stamp` incl.
`sizeof(Shader::Info)`/`sizeof(Program)`, stdlib marker, depth bit and env)
plus `bake_report.jsonl` (per-stage descriptor plans, workgroup/shared,
question counts, compute `CsLayout`) and `bake_manifest.json`. `bake_ds
verify` re-reads the file through a copy of `program_cache_load`'s
`Reader`/`get_info` and requires 0 bad records; it verifies *any*
`hookshot_programs.bin`, including one the game wrote.

**Verify, portable** (`bake.verify_bank`): seed sha round-trip, stamp
currency, SPIR-V magic + `spirv-val --target-env vulkan1.3` over every merged
module; writes `verify.json`. The analogue of `bake_ds verify` for the
portable format.

**Probe** (`lower_probe.cpp`): pins the hand-built Ampere encodings the
fixtures rely on (EXIT `w0=0x794d, w2=7<<23`; MOV.imm `0x802|dest<<16`;
MOV.cbuf `(off<<6)|(bank<<22)`) through the real `LowerToMaxwell`.

## 4. Offline assumptions (what is faked, and recorded)

| thing | offline answer | live source |
|---|---|---|
| texture/image variants | `bake_ds` expands compute answer variants like `seed_compute` (deduped, cap 64 — §5.4); portable legs keep the unbound baseline per program | `record_answers` on bound state (`:642-677`); `seed_compute` enumerates trials (`:1966-2082`) |
| driver banks c0/c2 | 0, except vertex c0+0x610 = `2047.9375f` (`vk_bank.cpp:405`) | per-draw `driver_bank_word` |
| user cbufs c3..c18 | 0 | bound guest memory |
| `min_ssbo_alignment` | 16 (Vulkan minimum; `--ssbo-align` on bake_ds) | device `dev.props.limits` |
| `convert_depth_mode` | false | `host->depth_minus_w()` |
| colour output types | all Float (`no_types`) | bound render targets |
| mesh `prim` header | null (as the seed path passes it, `:1945`) | bound pass-through geometry program |
| pipelines | `PENDING_GPU` everywhere | `VkDevice` builds (`build_graphics`) |

A bank baked with a different depth bit, env allowlist entry, or C++ ABI
loads-or-refuses whole (`disk_stamp`, `:837-864`) — never misreads. Live
banks are therefore same-toolchain artifacts (builder-integrated or a
player-PC step); the portable bank is the cross-toolchain handoff a GPU step
consumes.

## 5. What was cut, and what the alternatives are

1. **GLM `bake_translate.cpp` / `run_bake.py` / `bake_enumerate.py` /
   `bake_banks.py`** — cut as duplicate translators. `bake_translate.cpp`
   covers compute+graphics with its own JSONL layout and needs
   `<vulkan/vulkan.h>`; `shaderbatch.cpp` covers compute+graphics+task/mesh
   with the exact runtime keys and no Vulkan headers, and both agree on the
   stamp (`56b84e…`) and `min_ssbo_alignment = 16`. Kept from GLM: the
   encoding probe (`lower_probe.cpp`, unique harness value) and its findings
   (static-offset cbuf reads fold under ConstantPropagationPass; the
   `VK_SHADER_STAGE_ALL` bitmask note). Alternative recorded: if a JSONL
   per-emit log is ever preferred to TSV, `bake_glm/bake_translate.cpp`
   (bake-glm branch) is the drop-in.
2. **GLM `bake_banks.py` doc-shape banks** (`c0_<key>.bin` + `banks.json`) —
   cut, superseded by `bake_ds`'s live bank writer (real format + verify).
   Alternative recorded above.
3. **Spark approximate `program_key_skeleton`** — superseded wherever
   translation ran: `translation.by_program[].runtime_key` is the exact
   `graphics_key`/`mesh_key`/compute key. The skeleton key remains only for
   untranslated rows (resume/dedupe before translation).
4. **DeepSeek `--target` feature profiles** — ported from bake-deepseek
   6db36ff, verified per MiMo's Appendix B (bake-mimo ed927d9 — its §9.4
   R1-R5 all confirmed fixed; the confirmed semantics below match that
   appendix exactly). `--target plain|tensor|tensor-software|rdna2|gfx1201`
   (plus `query` in `bake_all`, which picks the profile and `ssbo-align`
   from the local Vulkan physical devices, instance only) folds the
   profile's switches (`EDEN_NX2_TENSOR` / `RX_VK_MMA_SOFTWARE`) into the
   cache stamp exactly as live's `disk_stamp` folds them
   (`vk_shader.cpp:837-864`), so the loader's `kept == stamp` check
   (`:902`) refuses a bank baked for another profile — and `verify
   --target` reproduces that refusal offline. The target never enters a
   key or a variant hash, only the stamp. Semantics, each confirmed:
   (a) the tensor path is live's own gate (`bake_ds.cpp:305` carries
   live's `!graphics`, `vk_shader.cpp:232`); `TensorMode::Software`
   assigns `Shader::Backend::SPIRV::mma_software` before any translation
   (`bake_ds.cpp:1087`), so tensor-software/rdna2/gfx1201 emit the
   software matrix function — 0 `OpCooperativeMatrixMulAddKHR` in either
   form (the fixture HMMA survives DCE via an STS store);
   (b) compute answer variants dedup by `answers_hash` per key, baseline
   first then insert-if-new (`bake_ds.cpp:1552-1560,1316`), i.e. live's
   `variants.insert` (`vk_shader.cpp:2073-2079`) — nuance: live pre-fills
   from the bank (`:1987`), a fresh bake is live's first-run state;
   (c) variants run for every profile including plain
   (`integer_variants` default true, `:110`), matching unconditional
   `seed_compute` (`vk_draw.cpp:5492`); off only via
   `--feature no-integer`;
   (d) default cap 64 (`:1045`) is live's prepare-run cap (`:5492`):
   `room() = kept.size() + 1 < cap` with the baseline emitted separately
   is 64 records, arithmetically equivalent to live's `kept.size() < cap`
   with the baseline inside kept (`:2024,:2036,:2048`);
   (e) `disk_stamp` folds `EDEN_*`/`RX_VK_*` in `environ` order, unsorted
   and undeduplicated (`bake_ds.cpp:710-714`, prefix list byte-identical
   to `vk_shader.cpp:859-862`) — the single-token stamps
   `eae37d29`/`cf0adaf6`/`8ad29707` are unchanged. ROM-scale rerun
   (tensor-software, cap 64, ssbo-align 4 — their measurement over
   ROM-derived data, not re-verifiable here): 52,807 records / 96,442
   modules / 1,654,020,773 words / 6,745,240,904 bytes, 0 bad, 602
   duplicate variants dropped, 97 programs skipped; 96,220/96,442 modules
   valid, all 70 tensor programs valid, 0 cooperative-matrix ops.
5. **Mesh `prim` pass-through, texture-variant rows, pipeline notes,
   distribution** — all deliberately out of scope (no game/GPU here); every
   leg marks them (`PENDING_GPU` / `NOT_TRANSLATED` / stop-line statuses)
   instead of faking them.

## 6. Traps the merge preserves (read before touching)

1. **Stamp sort trap**: `sorted(translator-tree)` then the four named files
   **appended** — merging them into the sort yields a plausible-but-wrong
   `158e661374b23b7a`. Guarded by `BakeStampTrapTests`.
2. **Header-only trap**: graphics `len <= 0x80+16` is a header alone
   (`parse_code`, `:175-179`) — tiny single-instruction fixtures report
   `HEADER_ONLY`, they are not mistranslations.
3. **Pipeline keys must fold the input variant** (`seed_program(i/3, i%3)`),
   or dedupe collapses the three bone variants into one.
4. **Resume cursors must retain prior combos** (`old_combos + combos`), not
   just skip done keys.
5. **`spirv_headers` probe list** must include the in-tree `buildext/cpm`
   (this merge's one-line fix); `bake_ds/build.sh` instead probes the
   azahar-build externals with `EXT=` override.
6. **CsLayout fallback** with `ssbo == 0` is base 0 / count 16 / end 0
   (`:1126-1128`), not `end()`.
7. **`bake verify` vs `bake_ds verify`**: the former checks the portable
   manifest, the latter the live bank — both must pass, they check different
   artifacts.

## 7. Test results (this machine, CPU-only, repo + `/tmp` only)

```
tests/test_bake_skeleton.py ............ 9 passed
tests/test_bake_mimo.py ................ PASS (3 programs, 5 stages, 6 modules spirv-val VALID)
tests/test_bake_ds.py .................. PASS (16 programs / 18 slots, 32 modules, 18 records / 0 bad;
                                         tensor profiles: plain LOWER_ERROR -> translated under
                                         tensor + tensor-software, cross-profile stamps refused,
                                         2 env orders fold process-order, --target query -> gfx1201)
python3 -I -B tests/run_all.py bake .... 3 passed, 0 failed, 0 skipped in 22 s
```

`spirv-val --target-env vulkan1.3` VALID on every emitted module;
`MeshShadingEXT`/`TaskEXT` entry points present on the mesh pair;
`spirv-dis` shows `LocalSize 8 1 1` from the control section's workgroup.
`bake_ds` was rebuilt standalone from the committed tree
(`OUT=${TMPDIR:-/tmp}/hookshot-ds-merge nx2/bake_ds/build.sh` → 163 recompiler
sources, `built .../bake_ds`) and `tests/test_bake_ds.py` passes against
that fresh binary as well as the prebuilt one. The repo runner's bake scope
(`tests/run_all.py bake`: each `test_bake_*.py` in its own process) passes
whole; the runner invokes `test_bake_ds.py` without `--tool`, so its
translate/bank half is covered by the two direct runs above, not by the
runner. The game directory, saves and caches were never read or written; no
GPU was used; nothing game-derived is committed (`work/`, `*.bin` stay
ignored).

## 8. ROM-STATUS verification appendix (bake-deepseek d01bb19)

ROM-STATUS reports the first real-ROM bake: 42,856 stages / 38,809 programs,
46,500 slots translated, 167 `LOWER_ERROR` programs excluded (38,642 banked),
90,135 SPIR-V modules of which only 2.86% are strict-valid — one systematic
class (cbuf `Block` + `ArrayStride` needs `uniformBufferStandardLayout`;
40/40 sampled failures pass with the flag). Two questions, answered with
file:line evidence from this tree.

### 8.1 The shipping build DOES enable uniformBufferStandardLayout — the bank stands

- `gpu/vk/vk_main.cpp:253`: `VkPhysicalDeviceVulkan12Features f12{...}`.
- `:256`: chained into the device-create chain
  (`f.pNext = &f11, f11.pNext = &f12, f12.pNext = &f13`).
- `:360`: `ci.pNext = &f` — the whole chain, f12 included, goes to
  `vkCreateDevice`. Nothing in the function clears any f12 member, unlike the
  features the build deliberately gates (`robust2`/`null_descriptor`,
  `provokingVertex`, `conditionalRendering`, `customBorderColor`,
  `depthClipControl`, `graphicsPipelineLibrary`, eds3 bits, `coop_matrix`,
  `:309-331`). Every Vulkan 1.2 feature the GPU reports is therefore
  requested — including `uniformBufferStandardLayout`.
- Floor: loader 1.3 (`:85-87`), device 1.3 (`:137-140`), so 1.2 features are
  core on every accepted GPU. The only consumer of `dev.vk12` (`:331`) is the
  `shaderFloat16` check (`vk_coopmat.cpp:377`) — no path turns the UBO layout
  back off.
- CI says it outright (`.forgejo/workflows/ci.yml`):
  "the back end's device has uniformBufferStandardLayout ... so constant
  buffers are u32 arrays of stride 4", and validates the back end's own
  modules with `spirv-val --target-env vulkan1.3
  --uniform-buffer-standard-layout`.

So strict-validity was the wrong gate; the 2.86% number measures the wrong
property. The bank stands as emitted. `bake verify` now judges SPIR-V WITH
`--uniform-buffer-standard-layout` (the flag exists in this machine's
SPIRV-Tools, verified), records `"spirv_layout":
"uniformBufferStandardLayout"` in `verify.json`, and `--strict` additionally
records the strict count for information only — it never fails the gate.
 deeper parity note: the live path emits the identical cbuf layout through
the same recompiler, so bank-vs-live agree regardless; the flag only aligns
the validator with the device both were built for.

### 8.2 Partial banks are the designed case — the game boots and fills the 167 live

38,642/38,809 programs (99.57%) banked is not a degraded bank; the loader
and the runtime treat a missing program exactly like a no-bank run:

- `program_pick` (`gpu/vk/vk_shader.cpp:1685-1700`): `program_cache_load`
  finds nothing for the key → falls through to live `translate_into`
  (compute/sync path) or a worker translation job (graphics). Bank miss ==
  existing cold-start path; no boot dependency on the bank anywhere.
- Precompile notes for missing programs are skipped, not fatal:
  `program_find` null → `if (p)` guard (`gpu/vk/vk_draw.cpp:5605-5607`);
  the pipeline is then built at first draw by the normal worker path.
- `prepare_run` tolerates failures by counting, not stopping: `st_failed`
  is logged and `hookshot_prepared.bin` is still stamped
  (`gpu/vk/vk_draw.cpp:5526-5533`); a driver that faults mid-warmup is
  handled the same way (`:5442-5455` warming guard).
- Stamp mismatch refuses the whole bank safely (`vk_shader.cpp:896-927`
  header check), so a partial bank strictly dominates no bank.
- Cost of each of the 167 misses: one first-meet live translation
  (historical 2–18 s stall on RX 770S, `tests/test_shader_census.py:4`) —
  stutter, then kept via `program_cache_save` like any play-built program.
  `RX_VK_SEED_CHECK=1` + `miss_note` (`vk_shader.cpp:1287-1296`) is the
  coverage oracle that names them.

`bake verify` now reports coverage explicitly (`coverage_programs`,
`seed_programs`, `coverage_fraction`, `missing_programs`) and stays green on
a partial bank as long as everything present is valid — proven by
`BakeVerifyTests.test_verify_partial_bank_stays_ok` (limit=1 translate over
a 2-program seed: coverage 0.5, `spirv_invalid` 0, ok). For the ROM bank the
same shape reads: 38,642 covered, 167 missing-by-design, gate on validity
not completeness.
