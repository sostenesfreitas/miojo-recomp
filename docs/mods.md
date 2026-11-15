# RomFS mods: the existing file contract

This is the file-replacement path Hookshot already implements, the first tier proposed in
a mod-loader proposal. It is not a new manifest format or a promise of
chest tables, item/check events, native modules or Archipelago support. The example below uses invented text files,
not a dump, a seed or a playable mod.

## What a file mod contains

A mod is a folder with a `romfs` subfolder. Paths below it replace or add to the game's RomFS; the game still has to
understand their contents. There is no file-extension allowlist or format validator. A file mod does not supply host
code or callbacks. An `exefs` folder is noticed and logged, but its patches are not applied: Hookshot compiles the
game's instructions ahead of time. Scripts, native libraries and a manifest beside `romfs` are not executed or read
as a mod interface. This is not a security sandbox or a guarantee that a mod cannot damage a save.

Generate game-derived files from the player's own supported dump and keep them local. A recipe or an authored
example can be shared; generated game files cannot. Use separate copied saves when trying a real mod. Removing a
mod does not undo progress already saved with it, and this loader does not create per-mod or per-seed save profiles.

## Discovery, ordering and disabling

`host/mods.c` implements these rules in `hos_mods_init`, `find_mods`, `mod_cmp` and `index_romfs`:

- The default root is `mods` beside the game executable. `HOS_MODS` selects another root; it points at the folder
  **containing mods**, not a mod's `romfs` folder. Nothing is downloaded or unpacked.
- A directory with `romfs` or `exefs`, from one to four levels below that root, is a mod. For example,
  `mods/contents/<title id>/<name>/romfs` works. Discovery stops below a directory recognized as a mod. A `romfs`
  folder directly in the root is not itself the usual `<name>/romfs` layout.
- During discovery a directory starting with `.`, `-` or `_` is skipped, including all mods below it. Rename
  `02-example` to `_02-example` while the game is stopped to disable it. These prefixes do not disable files or
  folders *inside* an already discovered `romfs` tree.
- Mods are sorted by their **whole path relative to the root**, ignoring case, not just by their last folder name.
  The last mod supplying a path wins. Use distinct numbered names at the same level (`01-...`, `02-...`) when you
  need an obvious order. Avoid names that differ only by case: their order is not defined.
- RomFS lookup normalizes slashes and case. Do not ship two file paths that differ only by case; which is indexed
  last within one mod is not defined. Use the game's exact spelling for portable files and directory listings.
- Directory links inside a mod's RomFS, including a linked `romfs` root, are not followed. File links are supported;
  they are not a containment boundary. Keep paths short and the tree shallow: the loader has fixed path buffers and
  stops descending beyond 32 RomFS directory levels. These limits are not a package-validation service.
- Discovery and indexing happen once at startup. Stop the game before adding, removing, renaming or editing a mod,
  then restart; there is no hot-reload contract.

`hos_mods_romfs` selects the winning file; `host/nn_fs.c` uses it for read-only mounts and adds mod-only directory
entries through `hos_mods_dir_extras`. A missing replacement falls back to the game's file. A directory cannot be
opened as a replacement file. The testing-only `HOS_FS_OVERLAY` can override this selection; leave it unset when
checking a mod's order.

## Resource sizes and prompts

A replacement that needs more memory must also have a suitable resource size table, or the game may reject it or
wait at a loading screen. The loader selects **one** mod's table using the same order; it does not merge arbitrary
mods' tables together. Combine the required entries in the mod that wins.

`host/prompts.c` merges the button prompts' entries into that winning table, taking the larger size for each entry.
It uses the table name belonging to the game's own RomFS, not a different version's table in a mod. If the table
cannot be merged, it is used unchanged with the game's icons. The [README's Mods section](../README.md#mods) gives
the file and size details. `tests/test_mods_size_table.py` exercises this with made-up tables, including both open
orders, two versions and malformed tables. A successful fixture does not certify a real mod's assets or progression.

## An example exercised without the game

The JSON block is a readable recipe: each key is a relative file path and each value is its UTF-8 contents. It is
**test input, not a manifest understood by Hookshot**. `tests/test_mods_size_table.py` copies these exact authored
files into a temporary folder, runs the real `host/mods.c` and `host/prompts.c` through the existing Linux filesystem
fixture, and reads the selected files. The fixture never starts the game or touches a player's folders.

<!-- romfs-example -->
```json
{
  "mods/01-example/romfs/Example/message.txt": "first mod\n",
  "mods/01-example/romfs/Example/only-first.txt": "kept from the first mod\n",
  "mods/02-override/romfs/Example/message.txt": "second mod\n",
  "mods/_disabled/romfs/Example/off.txt": "not enabled\n",
  "mods/contents/demo-title/00-nested/romfs/Example/nested.txt": "nested mod\n",
  "mods/contents/demo-title/00-nested/romfs/Example/message.txt": "nested mod wins by its full path\n",
  "mods/03-code-only/exefs/ignored.txt": "not a real patch; not loaded\n"
}
```
<!-- /romfs-example -->

`demo-title` and every `Example/...` path are invented. Installing these files in a real game does not demonstrate
randomized rewards or visible changes: the game does not request them.

Run from a source checkout on Linux with a C compiler:

```sh
python -I -B tests/test_mods_size_table.py
```

The example has these independently checked results:

| Operation | Expected result |
|---|---|
| Open `Example/message.txt` | The nested mod wins: `contents/demo-title/00-nested` sorts after `02-override`, despite `00-nested` |
| Open `Example/only-first.txt` | The first mod's non-conflicting file remains |
| Open `Example/nested.txt` | A nested mod is discovered |
| Open `Example/off.txt` or `ignored.txt` | No replacement: disabled mods and ExeFS content are not loaded |
| Open `example\\MESSAGE.TXT` | The same winner after slash/case normalization |
| Open `Example` or `Example/missing.txt` | No replacement file |
| Use the same files beside a fake game executable, with `HOS_MODS` unset | The same results from the default `mods` root |
| Disable `contents` and start a fresh fixture | `second mod` wins; the nested mod is absent |
| Also disable `02-override` and start a fresh fixture | `first mod` wins |
| Also disable `01-example` and start a fresh fixture | No replacement; the original game file would be used |

A final negative case points `HOS_MODS` at a single mod instead of the collection and checks that it does not load
that mod. The existing size-table checks still run afterwards. On Windows and macOS this driver's filesystem
fixture is explicitly skipped; its table-merge checks are separate. Native Windows filesystem behavior and a real
Clawshot seed with controller prompts still need their own tests.

## What this does not decide

There is no compatibility/version field, dependency solver, catalog, per-seed profile, Mods tab, chest-table format,
item/check service or loadable native-module ABI in this contract. Those are separate decisions. In
particular a checked location must not be treated as a successful local item grant. Co-op's internal session hooks
are not a mod-loading interface. Documenting the working file route does not choose core-first versus module-first
co-op, and does not require either of the unmerged text-field or co-op changes.
