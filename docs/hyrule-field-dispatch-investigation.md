# Investigating the Hyrule Field slowdown

This is the evidence and A/B plan for the Hyrule Field slowdown.
The original report describes adult Link riding Epona in Hyrule Field without an area transition. The run started around
30–35 FPS and fell to about 15 FPS. The cause is still open.

## Hookshot v3.0.0-rc.3

The saved Windows 11 log identifies v3.0.0-rc.3 (source fingerprint `cb01b5a3`, release tag commit `0c74253d`). Hardware was
an RTX 3070 8 GB (NVIDIA 621.14), Ryzen 7 5800X3D and 32 GB RAM. The issue reports 3440×1440, upscaling and frame
generation off, and `RX_VK_PASS_TIMING=1` for the diagnostic run. The slowdown was also reported with timing disabled.

These are three successive 600-present profiler reports from one saved run. FPS is the nearby 120-present summary,
and VRAM is the nearby status sample. They have different sampling windows.

| Present count | Nearby FPS | VRAM used / allowance (log GB) | Dispatch `26fe0c70ab9fc547` (ms/frame) | Dispatches/frame | Write-back (ms/frame) | CPU-to-mirror sync (ms/frame) |
|---:|---:|---:|---:|---:|---:|---:|
| 1,800 | 33.1 | 6.4 / 6.9 | 9.225 | 0.9 | 0.625 | 0.554 |
| 2,400 | 20.0 | 6.3 / 6.9 | 10.412 | 0.9 | 0.623 | 0.436 |
| 3,000 | 18.2 | 6.1 / 6.8 | 21.499 | 1.0 | 0.633 | 0.465 |

At 3,000 presents the renderer reported 44.06 ms of instrumented spans per frame in total. The profiler waits
at each timestamp, which can change overlap between GPU work. The 21.499 ms identifies where the time accumulated
in this run. It does not predict the FPS gain from a change, and the sampled VRAM trend does not establish buffer
residency or rule out transient memory pressure.

The prior Nsight review reported a `108 × 60 × 1` dispatch with 192 threads per group in three captures,
and a `3440 × 1440` colour input. The shader appeared to perform CMAA2-style edge detection and used atomics on a
small host-visible, host-coherent buffer. This is a working identification from the original capture review.
Capture files stay local. The dispatch count and grid alone do not explain the increase. Candidate volume,
contention, clocks, and the memory type and access path for the counter buffer remain to be measured.
Host-visible and host-coherent flags alone do not identify whether accesses reached device-local memory or crossed PCIe.

## Reproduce and compare

1. Start from a named current Hookshot commit using the [development A/B workflow](development.md#testing-a-change-against-a-base).
   From a local copy of one adult Hyrule Field save, ride the same loop with matching camera, weather, time of day and
   image settings. Include an early and a late fixed-view hold facing the same direction. Record visual workload,
   draw and dispatch counts, route duration, driver, and cache warm-up. Do the first pass on current main with
   normal settings.
2. Keep GPU pass timing off for FPS and frame-time comparisons. Use the [per-present CSV](development.md#time-and-profiles)
   and sample driver VRAM usage and allowance, dedicated and shared GPU memory, process private commit, and working set.
   Record average FPS, 1% low, median and p99 frame time, and frames over 50 ms for comparable early and late windows.
   Report both process memory metrics separately and align all samples to the same run.
3. Repeat the route separately with `RX_VK_PASS_TIMING=1`. Compare the same program's time per frame, time per dispatch,
   dispatch count, write-back and mirror sync. A program missing from the top-span list has no reported rank or
   timing in that block. Compare only the same instrumentation mode. If the key changes, match the dispatch by
   shader and resources locally before comparing.
4. If main reproduces the slowdown, compare one change at a time. Run at least two interleaved warm pairs on the
   same hardware and report the median and individual runs. First match scene content to distinguish camera-dependent
   workload from time in session. Then record clocks, dispatch dimensions, candidate/atomic activity if it can be
   measured, and whether the counter allocation is device-local. If a counter-placement experiment is warranted,
   preserve initialization, read-back and synchronization, and check image output on the same views.
5. Keep the Market return as a separate residency and traversal test. If needed, compare sparse mirrors on and
   off on the same current build and record actual mirror use. Sparse mirrors became the default on supported
   devices, and used-memory reporting and Windows heap commit changed since: both affect comparisons with previous
   builds. The separate capture-to-mirror sync path is measured on its own.

## Earlier Hookshot results

These were separate NVIDIA/Windows tests on older Hookshot builds. They help prioritize experiments for the
Market return and memory pressure, but are neither Hookshot measurements nor a diagnosis of the Field dispatch.

- [Excess eviction](https://github.com/NotRexed/Hookshot/pull/2826), backported to `0ec31ff6` with a runtime toggle,
  was the strongest repeatable change on the moving route. Two enabled runs averaged 47.35 and 48.82 FPS across
  Market, Castle, Field and Forest versus 35.99 FPS in the same-DLL off run and 42.82 and 43.78 FPS in earlier controls.
  Market return was 33.44 and 37.82 FPS enabled versus 19.70 FPS in the same-DLL off run. Fresh Market showed no
  consistent FPS gain. Allowance warnings persisted and private commit was 25.18–25.41 GiB across all three toggle runs.
- [Draw-cache work](https://github.com/NotRexed/Hookshot/pull/2774) moved a fixed fountain view from 43.69 to 44.63 FPS
  in one pair with unequal cache state. [The texture upload change](https://github.com/NotRexed/Hookshot/pull/2785)
  reduced some long moving frames. A warmed control had similar traversal tails and neither experiment established
  a repeatable gamewide improvement. Combined hazard memoization and target-trim pacing regressed one moving-route
  A/B sequence. Target reuse, import size, and preallocation experiments changed distinct parts of the workload.
- In a separate Castle route, 4 MiB imports improved the 45-second return hold from 26.4 to 39.4 FPS versus
  16 MiB imports. Peak private commit stayed near 32 GiB and both arms exceeded the driver's allowance. In a
  title-only test, skipping bulk shader and pipeline preparation lowered peak private commit from 22.42 to
  14.36 GiB without a corresponding drop in committed guest memory. The difference includes more than one
  runtime/driver allocation owner. Import backing may alias guest pages, so adding import bytes to process commit
  would double-count some memory.
- [Mirror trim](https://github.com/NotRexed/Hookshot/pull/2825) at `18a733a1` retained a retired source mirror on
  replacement allocation failure, but had already published the replacement imports. A local transaction patch
  staged all child imports and mirrors before changing the mapping. The old code failed on child mirror allocation
  failures. Nine fault-injection cases passed with the patch. Visible Field-to-Market gameplay under an API-19
  Hookshot host exercised successful trims in both arms. One Market sweep averaged 38.05 FPS before and 37.43 FPS
  after, with workload differences. This supports a separate correctness investigation. At Hookshot `cf605c85` the
  legacy `mirrors_trim` / `trims_settle` sequence still publishes host imports before the child mirrors are made.
  A Hookshot fault test should establish when that path is reached with sparse mode enabled and disabled and check
  mapping and data preservation before proposing a port.

Additional renderer work to check before opening code changes includes the restored fixes and the restored
performance work.
