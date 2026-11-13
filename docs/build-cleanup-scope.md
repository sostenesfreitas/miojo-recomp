# Windows build cleanup ownership

Cleanup before a new build is best effort. It must not stop a process just because an argument contains the selected
work folder's name or because a Python process has lost its parent (#2142).

The process table includes every process, so the current process's full ancestor chain can be excluded even across
non-Python launchers. Eligible processes must have positive command-line ownership:

- `make_game.py` and `lift.py` require an explicit absolute `--work` argument matching the selected folder. The
  default is accepted only for this installation's absolute `make_game.py` entry point or the exact packaged shape
  `<default home>/versions/<version>/HookshotBuilder/tools/nx2/make_game.py`. Packaged versions share that default
  work folder, so cleanup must recognize an interrupted build from a previous version after an update. A different
  home, sibling prefix, extra path component or selected custom work is not inferred from that shape. `build.py` gets its
  folder through `OOT_WORK`; an orphan's environment is unavailable in this snapshot, so it is preserved. A direct
  `build.py --standalone` child of a proven, live `make_game.py` can be selected when its entry point matches that
  parent's installation: the builder launches it in ROOT with that work folder in the environment. This proof is
  not propagated to other descendants or to a different installation's script.
- Known Clang/LLD executables need an absolute output under the selected folder, with a path-component boundary.
  The builder emits `-o`; `lld-link /OUT:` and attached `clang-cl /Fo` are also recognized. Input, include and
  response-file paths alone do not prove output ownership. Unknown argument forms are preserved.
- Windows quoting is decoded before matching; entry-point names and options are complete arguments. Unmatched
  quotes, relative paths and conflicting output/work arguments are not ownership evidence.

Termination is individual, without `taskkill /T`: an owned parent does not authorize stopping unexamined children.
Successful stops alone are reported. Query and termination failures remain nonfatal. Linux still does no cleanup.

Tests use authored process tables and intercept every query/termination call; they never inspect or terminate real
processes. They cover the unrelated orphan and sibling-prefix regressions, positive cleanup, default and explicit
ownership, spaces/Unicode/quoting, ancestor chains, unknown descendants and nonfatal failures.

This is a conservative command-line selector, not process-handle ownership or a PID-reuse guarantee. Unknown orphan
`build.py` processes, relative outputs and outputs named only in response files may still need manual cleanup. Path
matching is lexical; it does not resolve junctions or short-name aliases. No game run is needed for this selector.
