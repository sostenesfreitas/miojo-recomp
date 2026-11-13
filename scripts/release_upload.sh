#!/bin/bash
# A tag's GitHub release with these files attached. The Linux AppImage and Windows builder workflows both call it. For
# a release from the Release workflow (RELEASE_FROM_WORKFLOW=true: it passed the tag), the release is there already, a
# draft made by release-please or Promote, and the files are only added; when it is not there, this stops and makes
# nothing. For a tag pushed by hand, the first to get there makes the release (from the tag, with GitHub's generated
# notes) and the other adds its files to it. A version with a prerelease part (v0.2.0-rc.1) is a prerelease and never
# "latest". The release is found by listing every release, drafts too, and the files go to it by its id
# (scripts/release_files.sh find and upload): GitHub's get-a-release-by-tag finds no draft, and a lookup that failed
# once made a published, "latest" v2.1.0 beside its draft while the draft's files were still being built.
# A file already on the release is replaced, so a run again on the same tag updates it. A release that is not a draft
# then gets its SHA256SUMS and the table of its files (scripts/release_files.sh finish), SHA256SUMS also written to
# $RELEASE_SUMS_OUT when that is set. Needs gh and jq, with GH_TOKEN (contents: write) and GH_REPO set. No game data
# is involved: a file whose name looks like the game's or a dump's, or like NVIDIA's runtime files, is refused.
#
# nightly is the latest development build: a prerelease, never "latest", whose tag only moves forward along main. A
# push to main's run (GITHUB_SHA) takes it when its commit comes after the one the tag holds, even when main has newer
# commits still building: with merges every few minutes, waiting for main's newest left the nightly where it was until
# main was quiet. The tag is moved as a fast-forward, which GitHub refuses when the other platform's run moved it
# meanwhile to a commit this one does not follow, so it never goes back; a run whose commit is older than the tag's
# (or a re-run of an old one) changes nothing. Each platform's runs go one at a time (the workflows' concurrency), so
# its files never go back either. The Linux files and the Windows files are each one commit's, named by it
# (Hookshot-<version>-x86_64.AppImage, Hookshot-<version>-Windows-x64.zip, the tag in hookshot-linux.txt and
# hookshot-setup.txt, which each platform's updaters read), but while main moves one platform's can be a commit or two
# behind the other's and the tag. Its files are replaced, an AppImage and .zsync named by an older commit are removed
# once the new ones are there, and SHA256SUMS and the table are made again (the table is in its notes). rc, the latest
# release candidate, is refused: it gets its files from a published release (scripts/release_rc.sh).
#
#   scripts/release_upload.sh <tag> <file> ...
set -euo pipefail
shopt -s inherit_errexit
files=$(dirname "$0")/release_files.sh
tag=$1
shift
for f in "$@"; do
    name=$(basename "$f")
    shopt -s nocasematch
    case $name in
        *.nso|*.nca|*.nsp|*.xci|*.nsz|*.xcz|*.nup|*.keys|*.bin|*romfs*|*exefs*|*nvngx*|*dlss*|*streamline*|sl.*.dll)
            echo "release_upload.sh: $name is not a release file (game data or NVIDIA files never go on a release)" >&2
            exit 1 ;;
    esac
    shopt -u nocasematch
done
if [[ $tag == rc ]]; then
    echo "release_upload.sh: rc holds a release's own files and is made by scripts/release_rc.sh, not uploaded to" >&2
    exit 1
fi
# The nightly tag at sha when sha comes after (or is) the commit it holds; returns 1, changing nothing, when it holds a
# newer one. (called with ||, so set -e is off in it: each call's failure is handled here)
advance_nightly() {
    local sha=$1 held status head try
    for try in 1 2 3 4 5; do
        (( try == 1 )) || sleep $(( try * 2 ))
        # (a lookup that fails reads as no tag: making it then fails as well, as it is there, and it is looked up again)
        held=$(gh api "repos/$GH_REPO/git/ref/tags/nightly" --jq .object.sha 2>/dev/null) || held=
        if [[ -z $held ]]; then
            if gh api -X POST "repos/$GH_REPO/git/refs" -f ref=refs/tags/nightly -f sha="$sha" >/dev/null; then
                echo "nightly: made at $sha"
                return 0
            fi
            continue
        fi
        if [[ $held == "$sha" ]]; then
            echo "nightly: at $sha already (the other platform's run of this commit moved it)"
            return 0
        fi
        status=$(gh api "repos/$GH_REPO/compare/$held...$sha" --jq .status) || continue
        case $status in
            ahead | identical)
                # (a fast-forward only: refused when the tag moved meanwhile, and then looked at again)
                if gh api -X PATCH "repos/$GH_REPO/git/refs/tags/nightly" -f sha="$sha" -F force=false >/dev/null; then
                    echo "nightly: moved forward from $held to $sha"
                    return 0
                fi ;;
            behind)
                echo "nightly: it holds $held, which comes after $sha: left to that commit's files"
                return 1 ;;
            diverged)
                # (main was rewritten, and the tag holds a commit no longer on it: main's newest commit takes it)
                head=$(gh api "repos/$GH_REPO/commits/main" --jq .sha)
                if [[ $head != "$sha" ]]; then
                    echo "nightly: it holds $held, not before $sha, which is not main's newest ($head): left as it is"
                    return 1
                fi
                gh api -X PATCH "repos/$GH_REPO/git/refs/tags/nightly" -f sha="$sha" -F force=true >/dev/null ||
                    continue
                echo "nightly: moved from $held, not on main's history, to main's newest commit $sha"
                return 0 ;;
            *) echo "nightly: comparing $held with $sha gave '$status'" ;;
        esac
    done
    echo "::error::nightly: could not be moved to $sha" >&2
    exit 1
}
if [[ $tag == nightly ]]; then
    sha=${GITHUB_SHA:?}
    advance_nightly "$sha" || exit 0
    # (the same notes from every run: the two platforms' runs write them at different times, and each file names the
    # commit it was built from)
    notes="The latest development build of main, made by CI after every push to main. Each file is named by the commit it was built from (\`Hookshot-<version>-x86_64.AppImage\`, \`Hookshot-<version>-Windows-x64.zip\`; the version is \`git describe\`'s, ending in \`-g<commit>\`). The Windows and Linux builds finish at different times, so while changes are being merged one can be a commit or two behind the other; the \`nightly\` tag is at the newest of them.

It is untested and may break your saves or settings. For playing, use the [latest release](https://github.com/$GH_REPO/releases/latest).

$("$files" table nightly)"
    title="Latest development build"
    if gh release view nightly >/dev/null 2>&1; then
        gh release edit nightly --prerelease --latest=false --title "$title" --notes "$notes" >/dev/null
    else
        gh release create nightly --verify-tag --prerelease --latest=false --title "$title" --notes "$notes" ||
            gh release edit nightly --prerelease --latest=false --title "$title" --notes "$notes" >/dev/null
        # (a release just made is not in the list of releases at once: a repository's first nightly was made here and
        # "had no release" a moment later. Looked for a few times before its files go to it)
        for _ in 1 2 3 4 5 6; do
            [[ -z $("$files" find nightly) ]] || break
            sleep 3
        done
    fi
    "$files" upload nightly "$@"
    new=()
    for f in "$@"; do new+=("$(basename "$f")"); done
    # (the AppImage and its .zsync, or the RecompLauncher zip, of an older commit, once this run has uploaded the new ones)
    gh release view nightly --json assets --jq '.assets[].name' | while read -r old; do
        case $old in
            Hookshot-*-x86_64.AppImage | Hookshot-*-x86_64.AppImage.zsync) want="Hookshot-.*-x86_64\\.AppImage${old##*.AppImage}" ;;
            Hookshot-*-Windows-x64.zip) want='Hookshot-.*-Windows-x64\.zip' ;;
            *) continue ;;
        esac
        printf '%s\n' "${new[@]}" | grep -qxF "$old" && continue
        printf '%s\n' "${new[@]}" | grep -qx "$want" || continue
        gh release delete-asset nightly "$old" --yes
    done
    "$files" finish nightly "${RELEASE_SUMS_OUT:-}"
    exit 0
fi
if [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+- ]]; then
    kind=(--prerelease --latest=false)
else
    kind=(--latest)
fi
info=$("$files" find "$tag")
if [[ -z $info ]]; then
    if [[ ${RELEASE_FROM_WORKFLOW:-false} == true ]]; then
        echo "::error::$tag has no release. The Release workflow makes its draft before it builds the files: start it again (Promote, or re-run its failed jobs) rather than this job" >&2
        exit 1
    fi
    # (the other workflow can make it between the find and the create: its release is then the one used)
    gh release create "$tag" --verify-tag --title "$tag" --generate-notes "${kind[@]}" >/dev/null || true
    # (a release just made is not in the list of releases at once: scripts/release_rc.sh. Looked for a few times)
    for _ in 1 2 3 4 5 6; do
        info=$("$files" find "$tag")
        [[ -z $info ]] || break
        sleep 3
    done
    [[ -n $info ]] || { echo "::error::$tag: its release could not be made" >&2; exit 1; }
fi
"$files" upload "$tag" "$@"
# (a draft is the Release workflow's: its publish job does this once both builds' files are there)
info=$("$files" find "$tag")
read -r _ draft _ <<<"$info"
if [[ $draft != true ]]; then
    "$files" finish "$tag" "${RELEASE_SUMS_OUT:-}"
fi
