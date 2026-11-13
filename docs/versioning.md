# Versioning and releases

Hookshot follows [Semantic Versioning 2.0.0](https://semver.org/). Releases are made with
[release-please](https://github.com/googleapis/release-please): the version and the changelog come from the
[Conventional Commit](https://www.conventionalcommits.org/) titles of the pull requests merged into `main`.

The git tag is the source of truth. The game reads it through `nx2/version.py`, and the builder and the AppImage are
stamped from the same tag when they are packaged. `.release-please-manifest.json` holds the last released version for
release-please, a release candidate's or a promoted one's; it is changed only by the release pull request and by
the Release workflow's record job, once a version is published. The release pull request also writes the version to
`.release-please-version` (release-please's version file, named so: a `version.txt` at the root would be read by
`nx2/version.py` as the release name of every build from the checkout).

## The version number

`MAJOR.MINOR.PATCH`, with an optional prerelease suffix: `2.0.7`, `2.1.0`, `2.1.0-rc.1`. A release is tagged `v2.0.7`:
the `v` belongs to the tag, not to the version. Build metadata is not used in tags.

One version covers the whole project: the game (`hookshot.exe` / `hookshot`), the Vulkan back end, the lift pipeline
and its scripts, Hookshot Builder and the Linux AppImage. They are built from the same commit and released together,
so they never carry different versions.

The "public" surface the rules protect is the command line of `make_game.py` and the builder, the `RX_` and `HOS_`
switches, the files a build writes into a game folder (`hookshot_options.ini`, `hookshot_env.txt`, `language.txt`,
`hookshot_version.txt`), saves, and the format of `hookshot_log.txt`. The C sources and internal headers are not an
API and may change in any release.

## Pull request titles decide the version

Pull requests are squash-merged, so each one becomes a single commit on `main` whose subject is the pull request's
title. The *PR title* workflow (`.github/workflows/pr-title.yml`) checks that the title is a Conventional Commit:

```
type(scope): description
type(scope)!: description        (a breaking change)
```

`scope` is optional. A `!` before the colon, or `BREAKING CHANGE:` in the commit body, marks a breaking change. The
description becomes the changelog line. The allowed types and examples are in
[CONTRIBUTING.md](../CONTRIBUTING.md#pull-request-titles).

Every release is first a release candidate (below), so the next version is a release candidate's:

| Commit | After `2.0.7` (released) | After `2.0.8-rc.1` (a release candidate) | Changelog section |
|---|---|---|---|
| any commit with `!` or `BREAKING CHANGE:` | `3.0.0-rc.1` | `3.0.0-rc.1` | its own section, and its type's |
| `feat` | `2.1.0-rc.1` | `2.1.0-rc.1` | Features |
| `fix` | `2.0.8-rc.1` | `2.0.8-rc.2` | Bug Fixes |
| `perf` | `2.0.8-rc.1` | `2.0.8-rc.2` | Performance Improvements |
| `revert` | `2.0.8-rc.1` | `2.0.8-rc.2` | Reverts |
| `refactor`, `build`, `ci`, `docs`, `test`, `style`, `chore` | `2.0.8-rc.1` | `2.0.8-rc.2` | its own section (Code Refactoring, Build System, Continuous Integration, Documentation, Tests, Styles, Miscellaneous Chores) |

A release candidate of a minor version (`2.1.0-rc.1`) goes on to `2.1.0-rc.2` for a `feat` too, and one of a major
version (`3.0.0-rc.1`) for any commit. One exception, from release-please's prerelease versioning
(`versioning: prerelease`): a version that moves up during a series keeps the release candidate number, so a `feat`
after `2.0.8-rc.2` proposes `2.1.0-rc.2`, not `2.1.0-rc.1`. To start the new version at `rc.1`, put
`Release-As: 2.1.0-rc.1` in that pull request's squash-merge description.

A release takes the highest bump among its commits. A commit whose subject is not a Conventional Commit (a merge
commit, or a pull request merged without squashing) is not read, so its change makes no release and is not in the
changelog.

To choose the version yourself, put a `Release-As:` footer in the body of a commit on `main` (the squash-merge's
description works). Name a release candidate, so the release still goes through Promote:

```
fix(vk): keep the swapchain when the window is moved

Release-As: 2.1.0-rc.1
```

## How a release is made

The [Release workflow](../.github/workflows/release.yml) makes every release in two steps: a release candidate, a
prerelease for testing, and then, when one is good, Promote, which publishes it as its version.

### Release candidates

On every push to `main`:

1. release-please reads the commits since the last release candidate. If any of them makes a release, it opens a pull
   request titled `chore: release <version>` (or updates the open one), for the next release candidate:
   `2.0.8-rc.1` after the release `2.0.7`, `2.0.8-rc.2` after `2.0.8-rc.1`. It changes `.release-please-manifest.json`
   and puts the new entry at the top of `CHANGELOG.md`: the changes since the release candidate before it. The entry
   can be edited in that pull request before it is merged.
2. Merging the release pull request (squash or not; it is the one exception) makes the release candidate. On that
   push, release-please tags the merge commit `v<version>` (`v2.0.8-rc.1`) and makes its GitHub release as a draft,
   with the changelog entry as its notes.
3. The same workflow calls the Linux AppImage and Windows builder workflows on the tag. The jobs that build the files
   sign their build provenance (below), and the release jobs attach them with `scripts/release_upload.sh`:
   `Hookshot-v<version>-x86_64.AppImage`, its `.zsync` and `hookshot-linux.txt`; `HookshotBuilder.exe` (the setup),
   `hookshot-setup.txt` and the RecompLauncher zip. New update parts go to the `linux-parts` and `builder-parts`
   releases.
4. The publish job adds `SHA256SUMS` and the "Which file do I download?" table under the notes
   (`scripts/release_files.sh finish`), checks that all the files are there and that `SHA256SUMS` lists every file
   with its digest, and publishes the draft as a prerelease. It is never "latest". Players on the Release track do not
   see it; see *Update tracks* below. A release candidate's `HookshotBuilder.exe` installs that release candidate, and
   its AppImage's update information names the `rc` release (*The rc prerelease* below), so AppImageUpdate moves it on
   to the next release candidate and to its version's release, and never takes it back to an older release.
5. The rc job copies the newest release candidate's or release's AppImage and `.zsync` to the `rc` prerelease.

The next release pull request proposes the next release candidate of the same version, `2.0.8-rc.2`, with the changes
since `rc.1`. There is no step to start a series: the first fix after a release proposes the next version's `rc.1`.

### Promoting a release candidate

When a release candidate is good, start the Release workflow by hand on `main` (Actions > Release > Run workflow), with
its tag (`v2.0.8-rc.2`), or empty for the newest release candidate. From the GitHub CLI:
`gh workflow run release.yml -R NotRexed/Hookshot -f rc=v2.0.8-rc.2`.

Start it once. A Promote run is one at a time from its first job to its last, the builds and uploads included (the
workflow's concurrency group): a second one started meanwhile waits, and once the first has published the version it
stops with "v2.0.8 is already published". A third one replaces the second while it waits.

1. `scripts/release_promote.sh prepare` checks that the release candidate is a published prerelease on `main`, tags
   its version (`v2.0.8`) on the same commit, and makes that release as a draft. Its notes are those of the version's
   release candidates up to that one, newest first, so they add up to every change since the last release.
2. The Linux AppImage and Windows builder workflows build the version's files from that commit, and the publish job
   checks them and publishes the release as "latest", as above. The files are built again, not copied from the release
   candidate: every file names the version it was built as (the tools' `version.txt`, the AppImage's `VERSION` and
   name, the setup lists' `tag` line), and the builder, the game and the Linux updater compare that with the latest
   release's tag, so a `2.0.8-rc.2` file on the `v2.0.8` release would be offered as an update forever. The source is
   the same commit, the dependencies are the same checked downloads, and the update parts that did not change are the
   same files on `builder-parts` and `linux-parts`, so a player who tried the release candidate downloads only the
   parts that name the version. Then the rc job puts the version's AppImage on the `rc` prerelease, unless a newer
   release candidate (the next version's) is out already.
3. The record job sets `.release-please-manifest.json` to `2.0.8` (`scripts/release_promote.sh record`) and adds the
   version's entry to `CHANGELOG.md`, then has release-please make the release pull request again. It runs whenever
   `v2.0.8` ends up published: after the publish job, after a failed job when the release was published anyway, and
   when someone publishes the release by hand (that starts the Release workflow, `release: published`; a release
   published with the workflow's own token starts nothing, so the Promote run's record job is the one for those). It
   changes nothing when the manifest and the changelog already have the version. The release pull request now proposes
   `2.0.9-rc.1` (or `2.1.0-rc.1`), with the changes since the promoted commit. The record job pushes that commit to `main` with
   the workflow's token. If `main` takes no direct push (a branch protection rule or ruleset), it opens a small pull
   request `chore(release): record 2.0.8 as released` instead. Merge it before the release pull request, or the
   release pull request still proposes another `2.0.8` release candidate.

If a build fails, the release stays a draft: re-run the failed jobs of that Release run. Files already on the release
are replaced, so a re-run never fails because of them. Promote can also be started again with the same release
candidate: it goes on with the tag and the draft it made. With the repository variable `CI_RELEASE_WINDOWS` set to
`false`, the Windows files are left for you to upload, and the draft for you to publish: upload them, then run
`scripts/release_files.sh finish v<version>` (with `GH_REPO` set) before publishing, so `SHA256SUMS` lists them too.
Publishing it by hand then starts the record job.

### When a promotion goes wrong

The scripts find a release by listing the repository's releases, drafts included, and change it by its id
(`scripts/release_files.sh find`): GitHub's get-a-release-by-tag returns only a published release, and `gh release
view <tag>` finds a draft only through a second lookup. A listing that fails stops the job; it is never taken as "no
release". The release jobs of the Linux and Windows workflows never make a release for a tag the Release workflow
passed them: no draft there stops them.

- **A tag has two releases** (a stray draft beside the published one, say): every script refuses it and names both ids.
  Delete the one that is not meant (on its release page, or `gh api -X DELETE repos/NotRexed/Hookshot/releases/<id>`),
  then re-run the failed jobs.
- **Promote says the version is already published**: it is. Nothing is built again; check that the record job ran
  (next item). A fix after it goes to the next release candidate.
- **Promote says another Promote run is in progress**: wait for that run, or cancel it, then start Promote again.
- **The release is published but the manifest still holds a release candidate** (the next release pull request
  proposes `2.0.8-rc.<n+1>` instead of `2.0.9-rc.1`): re-run the failed jobs of the Promote run, which runs its record
  job. By hand: `scripts/release_promote.sh record v2.0.8` in a checkout of `main` (with `GH_REPO` set; the release
  candidate is found from the tag's commit), then a pull request with the change, merged before the release pull
  request. The next push to `main` makes the release pull request again from the new manifest.

What these guard against, from `v2.1.0`: two Promote runs started at once both took the same draft, and their Linux
and Windows uploads ran at the same time. The second run's Linux upload did not find the draft (`gh release view`
failed) and took the path for a tag pushed by hand, `gh release create --generate-notes --latest`: a published,
"latest" `v2.1.0` with GitHub's generated notes, before the first run's Windows files were on it. Both jobs then
replaced `SHA256SUMS` on it at once, and the first run's upload failed (HTTP 404), so its publish and record jobs were
skipped and the manifest stayed at `2.1.0-rc.3`.

The releases are made with the workflow's `GITHUB_TOKEN`, and the builds are called from the same workflow, so no
personal access token is needed. A pull request opened by `GITHUB_TOKEN` does not start other workflows, though, so
the release pull request has no checks of its own: it changes only the changelog and the manifest.

## The nightly prerelease

Every push to `main` also updates `nightly`, the latest development build. Once the Linux AppImage and Windows builder
workflows have built that commit, their release jobs move the `nightly` tag to it and replace the files of the `nightly`
prerelease with the same files a release has (`scripts/release_upload.sh nightly ...`), `SHA256SUMS` and the build
provenance included. It is a prerelease, never "latest", and not a version: `nightly` is not a `v*` tag, so
release-please and `git describe` ignore it; the builders and the game read it only on the Nightly track. The tag only
moves forward: a run takes the nightly when its commit comes after the one the tag holds, even while newer pushes are
still building, so it keeps up during a run of merges instead of waiting for `main` to be quiet. The tag is moved as a
fast-forward, which GitHub refuses when the other platform's run has moved it past this commit meanwhile; a run of an
older commit (or a re-run of an old one) leaves it alone. Each platform's files are one commit's and named by it (the
AppImage and the RecompLauncher zip by their version, `hookshot-linux.txt` and `hookshot-setup.txt` by their tag, which
is what each platform's updaters read), but the two builds finish at different times, so while `main` moves one
platform's files can be a commit or two behind the other's; the tag is at the newest. Its new parts go to `linux-parts`
and `builder-parts`, like a release's. Its AppImage's update information names the `nightly` release instead of
`latest`, so AppImageUpdate updates a nightly AppImage to the next nightly. (Gear Lever reads only `latest`,
`latest-pre` and `latest-all`, and offers the latest release for any other name.)

## The rc prerelease

`rc`, titled "Latest release candidate", holds the Linux AppImage and its `.zsync` of the newest published release
candidate or release, in semver order as on the Release candidate track below (`v2.1.0-rc.2` < `v2.1.0-rc.3` <
`v2.1.0`). A release candidate's AppImage names it in its update information, so AppImageUpdate updates it to the next
release candidate, and to its version's release once that is promoted; from there the AppImage names `latest` and
follows the releases. Its own tag would leave it where it is, `latest` can be older than it (the newest release is
before its release candidates), and `latest-pre` and `latest-all` would take the `nightly`, which is a prerelease too.
(Gear Lever offers the latest release for `rc`, as it did for the release candidate's own tag; the builder's own update
follows the track.)

The Release workflow's rc job (`scripts/release_rc.sh`) updates it after the publish job, a release candidate's or a
Promote's, and when a release is published by hand. It runs one at a time (its concurrency group `release-rc`), and each
run makes `rc` hold the newest release there is, found by listing the releases, not the one of its own run: a run that
waited behind another, or was replaced while it waited, leaves it right. It copies that release's two files, the same
files, so the release's `SHA256SUMS` and their build provenance check them; uploads them before it removes the older
ones; moves the `rc` tag to its commit; and keeps the notes short, a link to that release, where the other files and
the notes are. A release without its AppImage and `.zsync` is passed over. It is a prerelease, never "latest", and not a
version: the builders, the setup, the game's update check and the parts lookup read only `v<major>.<minor>.<patch>` and
`-rc.<n>` tags, or skip prereleases, so they never take it for a release. `scripts/release_upload.sh rc` and
`scripts/release_files.sh publish rc` are refused, and a tag named `rc` pushed by hand builds nothing.

## Update tracks

The builders (Windows and Linux) have an **Update track** menu: Release, Release candidate or Nightly. By default it
is the track of the build installed, read from its version: `v2.0.7` is a release, `v2.1.0-rc.1` a release candidate,
and git describe's `v2.1.0-rc.1-3-g1a2b3c4` the nightly (named with `--long`, so a nightly built on a tagged commit is
`-0-g...` and still says so). A choice is saved with the builder's settings.

| Track | Updates to | Offered when |
|---|---|---|
| Release | the latest release | it is another version (as before) |
| Release candidate | the newest of the release candidates and releases (semver: `rc.2` > `rc.1`, `2.1.0` > `2.1.0-rc.2`) | it is newer: never an older release, and the release of its own version once that ships |
| Nightly | the `nightly` release's build (its list's `tag` line) | it is another build |

Choosing another track installs that track's newest build, an older version too (the builder asks first). The game's
own update check (`host/hos_update.c`) follows the track of the folder's version. The same rules are in
`builder/HookshotBuilder/Updater.cs`, `builder/linux/linux_update.py` and `tests/test_update_tracks.py`. A release
candidate's and the nightly's `HookshotBuilder.exe` are built with `HOOKSHOT_SETUP_RELEASE`, so started with no `--tag`
they install their own release (`builder/setup/setup.c`).

## The files of a release

The names never change: the builder's update check, the setup and the Linux updater look for them by name.

| File | For |
|---|---|
| `HookshotBuilder.exe` | Windows: the setup players download. It reads `hookshot-setup.txt` and downloads the rest |
| `Hookshot-v<version>-x86_64.AppImage` | Linux and Steam Deck |
| `hookshot-setup.txt` | the setup's and the Windows builder's update list (`builder/setup/setup.c`, `builder/HookshotBuilder/Updater.cs`) |
| `hookshot-linux.txt` | the Linux updater's list (`builder/linux/linux_update.py`) |
| `Hookshot-v<version>-x86_64.AppImage.zsync` | AppImageUpdate and Gear Lever, which update the AppImage itself |
| `Hookshot-v<version>-Windows-x64.zip` | RecompLauncher: `HookshotBuilder.exe` and its `recomplauncher.json` (`.github/workflows/windows-builder.yml`). Windows players download `HookshotBuilder.exe` |
| `SHA256SUMS` | the SHA-256 of every other file, in `sha256sum`'s format |

The release notes end with a short table of which file to download, and what the other files are for
(`scripts/release_files.sh table`).

The AppImage carries update information (`scripts/linux/appimage.sh`):
`gh-releases-zsync|NotRexed|Hookshot|latest|Hookshot-*-x86_64.AppImage.zsync` for a release, `rc` in place of `latest`
for a release candidate and `nightly` for the nightly, the repository being the one CI runs in. appimagetool writes it into the image and makes the `.zsync`, whose file URL is the AppImage's name beside it on the
same release. `./Hookshot-v<version>-x86_64.AppImage --appimage-updateinformation` prints it.

To check a download, put it beside `SHA256SUMS` and run `sha256sum -c --ignore-missing SHA256SUMS`. Every file,
`SHA256SUMS` too, also has a build provenance attestation (`actions/attest-build-provenance`), signed by the workflow
run that built it. With the GitHub CLI:

```
gh attestation verify HookshotBuilder.exe -R NotRexed/Hookshot
```

It passes only for a file built by this repository's workflows, and shows the commit and the run.

## Tags pushed by hand

A `v*` tag pushed by hand (`git tag v2.0.8-rc.9 <commit>`, `git push origin v2.0.8-rc.9`) still starts the Linux
AppImage and Windows builder workflows, which make its release, with GitHub's generated notes, and attach the files,
`SHA256SUMS` and the table. A tag with a prerelease part is a prerelease. release-please is not involved, and the next
release pull request does not change, so this is only for a one-off test build; releases go through the release pull
request and Promote.

## Hotfixes

Fix on `main` with a `fix:` pull request, merge the release pull request (the next release candidate) and promote
it. The project does not keep maintenance
branches for older versions: only the newest release is supported. A tag pushed by hand on another branch is built
and released the same way as a release candidate, but release-please only follows `main`. Merge that commit into
`main` afterwards, or the next release pull request does not know about the tag.

## Where the version appears

| Place | Where it comes from |
|---|---|
| git tag | release-please (a release candidate), Promote (its version), or a tag pushed by hand; the source of truth |
| `.release-please-manifest.json` | the release pull request, and the Release workflow's record job once a version is published |
| `CHANGELOG.md` | the release pull request, and the record job |
| `hookshot_log.txt` and the Insert menu | `nx2/version.py`: `HOOKSHOT_VERSION`, then `VERSION`/`version.txt`, then `git describe --tags --match "v[0-9]*"` |
| `hookshot_version.txt` in a build | the `VERSION` file written when the tools are packaged (`nx2/make_game.py`) |
| AppImage file name | a release's tag (`HOOKSHOT_VERSION`), else `git describe --tags --match "v[0-9]*"` (`scripts/linux/appimage.sh`) |
| `HookshotBuilder.exe` | the MSBuild `Version` passed at publish time (`dotnet publish -p:Version=<version>`, the Windows builder workflow); the `csproj`'s `0.1.0` is only the default for a local build |

`nx2/version.py` also appends a fingerprint of the source files, `(source 1a2b3c4d)`. It is not a version: it tells
two builds of the same release apart, and it is the only identifier a build from a modified checkout has.

## The changelog

`CHANGELOG.md` is written by release-please, newest first, one section per release candidate, each with the changes
since the one before. A promoted version's section is a line naming the release candidate it is, above them. Do not
rewrite a released entry;
a correction is a new pull request and a new release. `changelog.txt` is the earlier changelog in Factorio's format,
written by hand for `0.1.0`. It is kept for history and is no longer updated.

## History

Tags from before versioning (`Release`, `amd`, `text-hotfix`, and so on) are snapshots of the old workflow. They stay
in the repository for history but are not releases: the tooling matches only `v[0-9]*` tags.

## Rules

- Do not edit a version number in a file, move a tag or reuse a version. The release pull request and Promote change
  the manifest; nothing else does.
- Do not put game files, keys, builds or anything made from a dump in a release. `scripts/release_upload.sh` refuses
  file names that look like them, or like NVIDIA's runtime files.
