# a64-kit (vendored subset)

From NotRexed's a64-kit (Rexcomp's AArch64 kit, `Porting Tools\Rexcomp\a64-kit`, a link to
`Ports\TOTK\TOTKPort\a64-kit`) at TOTKPort commit `60de030` (2026-10-30), **with changes**: Hookshot's, most of them
marked `(hookshot)`, and Hookshot's, marked `(hookshot)` (listed below). Only the parts the lifter pipeline needs:
`lift/` (lifter, suyu's translator and its ctypes binding), `rt/` (guest model), `tools/nso.py`, `tools/toolchain.py`,
`tools/survey.py`, `hos/nx_image.py`, `tests/insn_test.py`.

It is vendored, not referenced in place, so that building `a64emit.dll` and `lld-link.exe` (both written next to
their sources) never writes into the TOTKPort checkout, and so that this project is pinned to one version.
Most of the project's changes live outside this folder (`nx2/`): they wrap these modules instead of editing them.

Changes made here after the copy (against Hookshot's first import of it, commit `04407384`):

- Hookshot (NotRexed/Hookshot, before the fork): `lift/a64_lift.py` (game patches after an instruction, setjmp owners,
  page marks grouped per straight-line run of stores), `lift/build_emit.py`, `lift/suyu/arm64_to_c.h` (the FMUL/FMLA-family
  sticky-flag fast path, a64_fpx), `rt/a64.h` and `rt/a64_rt.c` (setjmp in lifted code, the BR hook, an exclusive store's
  page marked after the store), `tools/toolchain.py` (Windows builds with llvm-mingw, cross builds with
  RX_TARGET=windows), `hos/nx_image.py` (the addend of GLOB_DAT and JUMP_SLOT relocations), `README.a64-kit.md`. The
  history is Hookshot's: `git log 04407384.. -- third_party/a64-kit`.
- Hookshot: `tools/toolchain.py` reads `HOOKSHOT_MINGW`, as the rest of Hookshot's build sets it, where Hookshot read
  `HOOKSHOT_MINGW` (marked `(hookshot)`). The `(hookshot)` markers and comments stay as Hookshot wrote them.

So the copy can no longer be refreshed by copying the files again: a refresh keeps these changes, and the tree-sha256
below is this folder's, not upstream's (python -I scripts/check_third_party.py).

Licensing: `lift/suyu/arm64_to_c.h` is suyu's (GPL-2.0-or-later, texts in `lift/suyu/`), so `a64emit.dll`, the
generated runtime and all lifted C are GPL. See `NOTICE.TOTKPort.md`.

tree-sha256: d116e883d51a5a03c3005afb268136314f2351ccc5786904f5915561b699bc81
