# New tested runs

Each new measured run is one file in this directory. The [existing table](../tested-configurations.md#earlier-runs-frozen)
stays frozen; do not append to it.

Name the file `YYYY-MM-DD-topic-tester-run.md`, for example `2026-11-13-cache-jane-run01.md`.
**Include the tester's name and a distinct run identifier.** Use lowercase letters, digits and hyphens after the date,
with at least three parts (topic, tester, run); a multiword topic or tester can add more parts. New files are ordered by
date and then filename descending, not by an inferred time of day.

The file is exactly three lines: this header, separator and one row. This is a format example, not a test result:

```markdown
| Date | Build | OS / desktop | GPU / driver | CPU / RAM | Settings | Where | Result |
|---|---|---|---|---|---|---|---|
| 2026-11-13 | tested commit or version | OS and desktop | GPU and driver | CPU and RAM | relevant settings | scene and method | what was observed, with a run identifier |
```

- Fill every cell; use `unknown` where a detail was not recorded, not zero.
- The date is `YYYY-MM-DD`, optionally followed by `UTC`, and must match the filename.
- Keep the row on one line. Escape a literal pipe as `\|`, including inside backticks.
- Repeat the context in each file: no `as above`, `same host` or `ditto`. Unrelated runs can appear between files.
- Keep repeated runs distinguishable by a time or run identifier in the row, rather than copying an identical row.
  New duplicates, including copies of the frozen table, are rejected; the frozen table's existing repeats are retained.
- Name the builds actually tested and keep the detailed logs, comparison and tester comments on the pull request.
  A well-formed row is not a check that its measurements are true.

Run `python -I -B tests/test_tested_configurations.py` from the repository root before committing the new file.
`python -I scripts/tested_configurations.py` prints the combined Markdown as UTF-8 when piped or redirected;
`--output work/tested-configurations.md` writes it to a file instead. Do not commit that assembled view.
This README is the directory's guide, not a run fragment.
