# Using a save from an emulator

Hookshot and the emulators and ports that run the Switch 2 Ocarina of Time on a PC all keep the game's saves as the
**same files**. You can carry on in Hookshot from a save made in Lichii (or Eglantine, Orchid, NX2navi, ...), or take
your Hookshot save to one of them, at the same spot: nothing is converted, the files are copied across.
`scripts/oot_saves.py` finds the folders and does the copy safely.

You need your own copy of the game. Nothing here
comes with game files.

## The files

Every one of them keeps one folder per user profile, holding:

```
account.sav
option.sav            (the game's own settings)
slot_00/ ... slot_NN/ (one per save slot)
  progress.sav
  caption.sav
```

Only the folder they are in differs.

## Where the saves are

| Program | Linux | Windows | How sure |
|---|---|---|---|
| Hookshot | `~/.local/share/hookshot/save/<user id>/` | `<game folder>\user\save\<user id>\` | Hookshot's source; Linux tested |
| [Lichii](https://github.com/zortos293/Lichii) | `~/.local/share/lichii/` + Eden's save path below (before build 150: `~/.local/share/eden/`) | `%APPDATA%\lichii\` + Eden's save path (before build 150: `%APPDATA%\eden\`) | Lichii's source; the older folder tested on Linux, the new one and Windows not |
| [Frizti (Fritchii)](https://github.com/Frizti/Lichii), a Lichii fork | (Windows only) | `%APPDATA%\eden\` + Eden's save path | its source (`fritchii-v5.5`); not tried |
| [Orchid](https://github.com/MythosOfs/Orchid) | `~/.local/share/eden/` + Eden's save path | `%APPDATA%\eden\` + Eden's save path | its source; not tried |
| [PokeDinner](https://git.eden-emu.dev/Franseven/PokeDinner-a-Eglantine-Fork) | `~/.local/share/eden/` + Eden's save path | `%APPDATA%\eden\` + Eden's save path | its source; not tried |
| [Eglantine](https://github.com/Djihads80/Eglantine) | `~/.local/share/eglantine/` + Eden's save path | `%APPDATA%\eglantine\` + Eden's save path | its source; not tried |
| [NX2navi](https://github.com/tangerineturtle1/NX2navi) | `<game folder>/profile/save/<account>/` (its AppImage builds into `~/Games/Ocarina`) | `<game folder>\profile\save\<account>\` (the launcher's game folder is `%LOCALAPPDATA%\Programs\NX2navi\game` unless you chose another) | its documentation (`docs/launcher.md`, `docs/installer.md`, `docs/README-BuildKit.md`); not tried |

Eden's save path, under that data folder: `nand/user/save/0000000000000000/<profile id>/0400486024DEC000/`.

- `0400486024DEC000` is the game's title id. Eden's `<profile id>` is the Switch user profile you play with;
  Hookshot's `<user id>` and NX2navi's `<account>` are one folder with a 32-character hex name.
- **Frizti, Orchid and PokeDinner share one folder**, Eden's (`eden`): a save there is the same save in all of
  them. Eglantine has its own (`eglantine`), and so has Lichii (`lichii`) since its build 150 (2026-11-10), which can
  import Eden's data into it. A Lichii from before that build shares Eden's folder: the script looks in `lichii` first
  and uses `eden` when there is no save in it.
- The emulators use a `user` folder beside the program (Linux: in the folder you start it from) instead of the paths
  above when that folder exists (their "portable" mode). Point the script at it with `--from`/`--to`.
- Newer Eden code also reads a save from `nand/user/save/account/<profile>/0400486024DEC000/0/` when that folder
  exists, and then uses it first. The script looks there too.
- NX2navi writes a save to `profile/save-tx` first and moves it into place after. While something is waiting there,
  that is the save the game loads next, so the script refuses to copy until NX2navi has been started and quit (or
  `--force`). NX2navi's launcher also has its own **Import Saves**, which takes an emulator's save folder.
- `$XDG_DATA_HOME` replaces `~/.local/share` when it is set. Hookshot's `--user <folder>` puts its saves in
  `<folder>/save/<user id>/` ([command line arguments](../readme.md#running-the-game-from-the-command-line)).
- The emulator paths come from each one's `src/common/fs/fs_paths.h`, `path_util.cpp` and
  `src/core/file_sys/savedata_factory.cpp`. Official Eden does not run Switch 2 games; its name is accepted because
  the forks above use its folder.
- Not listed: Ryujinx and its forks (Ryubing, ...) do not run Switch 2 games. Any other program that keeps these files
  works through `--from`/`--to` with the folder that holds them.

## Copy with the script

`scripts/oot_saves.py` (Python 3, nothing to install) copies the slots, `account.sav` and `option.sav` one way, after
backing up the side it overwrites. From the Hookshot folder (or a clone):

```sh
python -I scripts/oot_saves.py list              # every save it finds, its slots and when each was saved
python -I scripts/oot_saves.py import lichii     # Lichii -> Hookshot
python -I scripts/oot_saves.py export lichii     # Hookshot -> Lichii
python -I scripts/oot_saves.py import --from ~/Downloads/my-save   # any folder holding a save -> Hookshot
python -I scripts/oot_saves.py export --to ~/Lichii/user           # Hookshot -> a portable emulator
```

The names are `lichii`, `frizti` (or `fritchii`), `orchid`, `pokedinner`, `eden`, `eglantine` and `nx2navi`.
`to-lichii` and `to-hookshot` still work: they are `export lichii` and `import lichii`.

On Windows Hookshot's folder is your game folder, which the script cannot guess, so name it:

```powershell
python -I scripts\oot_saves.py import lichii --hookshot "C:\Games\Hookshot"
python -I scripts\oot_saves.py export nx2navi --hookshot "C:\Games\Hookshot"
```

| Option | What it does |
|---|---|
| `--dry-run` | Says what it would copy and where, and changes nothing |
| `--keep-settings` | Leaves the target's `option.sav` alone, so each program keeps its own settings |
| `--profile <id>` | The emulator's profile to use. Needed when more than one profile has saves: the script lists them and stops rather than pick one |
| `--from <folder>`, `--to <folder>` | The emulator's side: the save folder itself, or a folder above it to search (a data folder, a portable `user` folder, NX2navi's game folder). With a name, it replaces where that emulator is looked for; without one, any folder works |
| `--hookshot <folder>` | Hookshot's data folder (Windows: the game folder), or its `save\<user id>` folder |
| `--backup-dir <folder>` | Where the backup goes, outside the target save folder |
| `--force` | Copies even if a game seems to be running, or NX2navi has a save waiting |

The script refuses to run while it sees Hookshot or any of the programs above running. The side being overwritten is
copied to `oot-save-backups/<import or export>-<name>-<date>-<time>/` first: in `~/.local/share` on Linux, in
`%LOCALAPPDATA%\Hookshot` on Windows. To undo a copy, put the backup's contents back.

## Copy by hand

Close both programs, then copy the **contents** of the folder you are leaving into the folder you are going to (see
the table above). Keep a copy of the target first. Delete the target's old `slot_*` folders before copying, so a slot
that only exists on that side does not stay mixed in with the new save.

## Before you copy

- **Close both programs first.** A game that is running can write its save again on exit and undo the copy.
- **Never run two of them on the same save at the same time.** Pointing two at one folder with a symlink may work,
  since the files are the same, but it has not been tried.
- **Settings follow the save.** `option.sav` holds the game's own settings (camera, text speed, ...) and is copied too.
  Use `--keep-settings` (or skip that file by hand) if you want different settings in each.
- **Profiles.** If your emulator has several user profiles, make sure you copy to and from the one you play on.
- **Backups.** The script keeps one for every copy. Delete old ones when you no longer need them.
- Start each program once and save in it before the first copy, so that its save folder exists.

## What was tested

- Linux (Arch-based), Hookshot v2.0.5 and Lichii built from source on the same day.
- A Hookshot save copied to Lichii loads in Lichii at the same spot (Kokiri Forest, the spawn by the flower patch).
- A Lichii save copied to Hookshot arrives identical, but has not been played through on the Hookshot side.
- The script's discovery for every program above, its copies and backups are covered by `tests/test_oot_saves.py`, on
  temporary folders.
- **Not tested:** Windows, and every program but Lichii. Their folders come from their source or, for NX2navi, its
  documentation, as the table says.
