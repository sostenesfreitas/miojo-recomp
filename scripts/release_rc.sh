#!/bin/bash
# rc, the latest release candidate: a rolling prerelease, never "latest" and not a version, that holds the Linux AppImage
# and its .zsync of the newest published release candidate or release (semver: v2.1.0-rc.2 < v2.1.0-rc.3 < v2.1.0, as
# the builder's Release candidate Update track has it, builder/linux/linux_update.py). A release candidate's AppImage
# carries gh-releases-zsync|<owner>|<repo>|rc|... (the Linux AppImage workflow), so AppImageUpdate moves it on to the
# next release candidate, and to its version's release once that ships. Its own tag would keep it where it is, and
# latest (the newest release) could be older than it; latest-pre would take the nightly, also a prerelease.
#
# The Release workflow runs this after its publish job (a release candidate's files, and a promoted version's), and
# when a release is published by hand, one run at a time (its rc job's concurrency group). Each run makes rc hold the
# newest release there is, found by listing the releases, not the one that started it: a run that was replaced while
# it waited, or that finished before a newer release, changes nothing the next run would not. The files are copied
# from that release (the same files, so its SHA256SUMS and their build provenance are theirs too), the new ones
# uploaded before the older AppImage and .zsync are removed, and the rc tag is moved to its commit. Its notes are short
# and point to that release. Needs gh and jq, with GH_TOKEN (contents: write) and GH_REPO set.
#
#   scripts/release_rc.sh
set -euo pipefail
shopt -s inherit_errexit
FILES=$(dirname "$0")/release_files.sh
RC=rc
TITLE="Latest release candidate"

fail() { echo "::error::$*" >&2; exit 1; }

assets() {   # release id -> its files, as one JSON array
    gh api "repos/$GH_REPO/releases/$1/assets?per_page=100" --paginate | jq -s 'add // []'
}

commit_of() {   # tag -> the commit it names (a lightweight tag, or an annotated one)
    local type sha
    read -r type sha < <(gh api "repos/$GH_REPO/git/ref/tags/$1" --jq '"\(.object.type) \(.object.sha)"')
    if [[ $type == tag ]]; then
        sha=$(gh api "repos/$GH_REPO/git/tags/$sha" --jq .object.sha)
    fi
    echo "$sha"
}

# The published release candidates and releases, newest first: "<id> <tag>"
all=$(gh api "repos/$GH_REPO/releases?per_page=100" --paginate | jq -s 'add // []') || fail "the releases could not be listed"
candidates=$(jq -r '[.[] | select((.draft | not) and (.tag_name | test("^v[0-9]+\\.[0-9]+\\.[0-9]+(-rc\\.[0-9]+)?$")))
    | . + {key: (.tag_name | capture("^v(?<a>[0-9]+)\\.(?<b>[0-9]+)\\.(?<c>[0-9]+)(-rc\\.(?<r>[0-9]+))?$")
        | [(.a | tonumber), (.b | tonumber), (.c | tonumber), ((.r // "1000000000") | tonumber)])}]
    | sort_by(.key) | reverse | .[] | "\(.id) \(.tag_name)"' <<<"$all")

# The newest of them with its AppImage and .zsync (a release whose Linux files are missing is passed over)
src= tag= files=
while read -r id t; do
    [[ -n $id ]] || continue
    files=$(assets "$id" | jq -c --arg a "Hookshot-$t-x86_64.AppImage" \
        '[.[] | select(.name == $a or .name == ($a + ".zsync")) | {id, name, digest}] | sort_by(.name)')
    if [[ $(jq length <<<"$files") == 2 ]]; then src=$id tag=$t; break; fi
    echo "$t has no Hookshot-$t-x86_64.AppImage and .zsync: passed over"
done <<<"$candidates"
if [[ -z $src ]]; then
    echo "no published release candidate or release has its AppImage and .zsync: $RC is left as it is"
    exit 0
fi
echo "the newest release candidate or release: $tag ($src)"
sha=$(commit_of "$tag")
url="https://github.com/$GH_REPO/releases/tag/$tag"
notes="The Linux AppImage of the newest release candidate or release, [$tag]($url), and its \`.zsync\`: the same files, here for AppImageUpdate, so that a release candidate's AppImage moves on to the next release candidate and to its version's release. This is not a version.

Download from [$tag]($url): its notes, the Windows setup and \`SHA256SUMS\` are there. For playing, use the [latest release](https://github.com/$GH_REPO/releases/latest)."

# The tag on that release's commit, and the release (a prerelease, never "latest"), found by listing as the others are
if gh api "repos/$GH_REPO/git/ref/tags/$RC" >/dev/null 2>&1; then
    gh api -X PATCH "repos/$GH_REPO/git/refs/tags/$RC" -f sha="$sha" -F force=true >/dev/null
else
    gh api -X POST "repos/$GH_REPO/git/refs" -f ref="refs/tags/$RC" -f sha="$sha" >/dev/null
fi
info=$("$FILES" find "$RC")
if [[ -z $info ]]; then
    jq -n --arg t "$RC" --arg n "$TITLE" --arg b "$notes" \
        '{tag_name: $t, name: $n, body: $b, draft: false, prerelease: true, make_latest: "false"}' |
        gh api -X POST "repos/$GH_REPO/releases" --input - >/dev/null
    # (a release just made is not in the list of releases at once: the first rc of a repository was made, not found a
    # second later, and the job stopped with its release empty. Looked for a few times)
    for _ in 1 2 3 4 5 6; do
        info=$("$FILES" find "$RC")
        [[ -z $info ]] || break
        sleep 3
    done
    [[ -n $info ]] || fail "$RC: its release could not be made"
fi
read -r id _ <<<"$info"

# The files, unless the same ones are there already
have=$(assets "$id")
dir=$(mktemp -d)
trap 'rm -rf "$dir"' EXIT
upload=()
while read -r asset name digest; do
    there=$(jq -r --arg n "$name" '.[] | select(.name == $n) | .digest // ""' <<<"$have")
    if [[ $digest == sha256:* && $there == "$digest" ]]; then
        echo "$RC: $name is there already"
        continue
    fi
    gh api -H 'Accept: application/octet-stream' "repos/$GH_REPO/releases/assets/$asset" >"$dir/$name"
    if [[ $digest == sha256:* && sha256:$(sha256sum "$dir/$name" | cut -d' ' -f1) != "$digest" ]]; then
        fail "$tag: $name did not download whole (its SHA-256 is not GitHub's digest of it)"
    fi
    upload+=("$dir/$name")
done < <(jq -r '.[] | "\(.id) \(.name) \(.digest // "")"' <<<"$files")
# (the AppImage before its .zsync: the .zsync names the AppImage beside it)
if (( ${#upload[@]} )); then "$FILES" upload "$RC" "${upload[@]}"; fi
# (then the older ones, the .zsync first, and anything else that is not these two)
keep=$(jq -r '.[].name' <<<"$files")
assets "$id" | jq -r '.[] | "\(.id) \(.name)"' | sort -k2,2r | while read -r asset name; do
    grep -qxF "$name" <<<"$keep" && continue
    gh api -X DELETE "repos/$GH_REPO/releases/assets/$asset" >/dev/null
    echo "$RC: $name removed"
done
jq -n --arg t "$RC" --arg n "$TITLE" --arg b "$notes" '{tag_name: $t, name: $n, body: $b, draft: false, prerelease: true, make_latest: "false"}' |
    gh api -X PATCH "repos/$GH_REPO/releases/$id" --input - --jq '"\(.tag_name) (\(.id)): \(.html_url)"'
echo "$RC holds $tag"
