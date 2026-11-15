# Saves in the repository

Saves the Test Farm, the tests and everyone can start the game from, so a run begins at the same spot on every PC:
`python -I scripts/hookshot_ctl.py launch <game folder> --repo-save <name>`. They are the one exception to "no game
files" in [CONTRIBUTING.md](../../CONTRIBUTING.md#the-rule-with-no-exceptions), and only under these rules.

## The rules

- **Made fresh for the farm.** A repo save starts from a new profile (an empty data folder, or `--user` with a new
  folder) and is played only as far as the save needs. Never one of your own adventures.
- **Scrubbed** with `python -I scripts/oot_saves.py scrub`: its Load-screen picture is replaced by a plain mid-grey
  one of the same size, and the date it was saved (in `caption.sav`) is set to 2026-01-01 00:00. The game's saves hold
  no player name.
- **Only these files:** `progress.sav` and `caption.sav` of one slot per save. No `account.sav` or `option.sav`, shared
  or not: the game makes both itself when it loads a slot found alone, and a shipped one could carry a player's
  settings. `hookshot_options.ini`, `hookshot_env.txt`, `language.txt` and everything else in `user/` stay banned.
- **Checked:** `python -I scripts/oot_saves.py check-repo-saves` passes (the layout, no links, no other file and no
  `account.sav`, `option.sav` or `_profile/` anywhere, the sizes, every picture grey, every date reset, a README for each
  save).
- **Added only by a reviewed pull request that two maintainers approve**, one save or a change to one per pull request,
  saying how it was made.

## The layout

```
assets/saves/
  README.md                 this page
  <name>/                   one save: a-z, 0-9 and -, e.g. kokiri-forest-start
    README.md               first line: where the save stands (hookshot_ctl.py saves --repo shows it)
    slot_00/
      progress.sav
      caption.sav           its picture mid-grey, its date 2026-01-01 00:00
```

`launch --repo-save <name>` copies only `slot_00/` into the run's empty save folder; the game makes `account.sav` and
`option.sav` there when it loads the slot.

## The saves

| Name | Where it stands |
|---|---|
| `kokiri-start` | Kokiri Forest, Link's house, right after the opening (a new game, control just given) |

How to make one: `scrub` and `check-repo-saves` in [scripts/oot_saves.py](../../scripts/oot_saves.py).
