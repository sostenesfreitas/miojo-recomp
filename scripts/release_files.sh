#!/bin/bash
# A release's files, found and changed by the release's id: the Release workflow's releases are drafts until its
# publish job, and GitHub's get-a-release-by-tag (and so `gh release view <tag>`, which falls back to a second lookup
# for drafts) finds only a published one. A failed lookup must never read as "no release", so the release of a tag is
# found here by listing every release, drafts included (with GH_TOKEN's contents: write), and a listing that fails
# stops the script. A tag with more than one release (a draft beside a published one, say) is refused: which one is
# meant is for a person to say, by deleting the other.
#
# What a release gets once its files are on it: big download links for Windows and Linux above its notes, a "Which file
# do I download?" table under them, and SHA256SUMS, the SHA-256 of every other file on it in sha256sum's format
# (sha256sum -c --ignore-missing SHA256SUMS checks a download). The sums are GitHub's own digests of the uploaded
# files; a file without one is downloaded and hashed. The Release workflow's publish job runs this for a release
# before publishing it, and scripts/release_upload.sh for a tag pushed by hand (and the nightly). Needs gh and jq, with
# GH_TOKEN (contents: write) and GH_REPO set.
#
#   scripts/release_files.sh find <tag>              "<id> <draft> <prerelease>" of the tag's release, drafts too;
#                                                    nothing when it has none. Fails when the listing fails or the tag
#                                                    has more than one release
#   scripts/release_files.sh upload <tag> <file> ... the files on the tag's release, by its id; a file of the same name
#                                                    already there is replaced
#   scripts/release_files.sh publish <tag>           the tag's release published, by its id: a version with a
#                                                    prerelease part (v2.1.0-rc.1) as a prerelease, never "latest";
#                                                    any other as the latest release
#   scripts/release_files.sh table <tag>             the table, in Markdown
#   scripts/release_files.sh links <tag>             the download links, in Markdown (from the files on the release)
#   scripts/release_files.sh finish <tag> [<out>]    the table under the notes (once), and SHA256SUMS uploaded (also
#                                                    written to <out>). Two runs at once (the Linux and Windows release
#                                                    jobs of a tag pushed by hand) leave the sums of every file: each
#                                                    one checks the uploaded SHA256SUMS against the files after
#                                                    uploading it.
#   scripts/release_files.sh check <tag> <name> ...  each named file and SHA256SUMS are on the release, and SHA256SUMS
#                                                    lists every other file on it, with its digest
set -euo pipefail
shopt -s inherit_errexit
MARK='<!-- release-files -->'
LINKS='<!-- release-links -->'

fail() { echo "::error::$*" >&2; exit 1; }

releases() {   # every release of the repository, drafts too, as one JSON array
    gh api "repos/$GH_REPO/releases?per_page=100" --paginate | jq -s 'add // []'
}

find_release() {   # tag -> "<id> <draft> <prerelease>", or nothing
    local all found n
    all=$(releases) || fail "$1: the releases could not be listed"
    found=$(jq -r --arg t "$1" '.[] | select(.tag_name == $t) | "\(.id) \(.draft) \(.prerelease)"' <<<"$all")
    n=$(grep -c . <<<"$found" || true)
    if (( n > 1 )); then
        fail "$1 has $n releases ($(jq -r --arg t "$1" '[.[] | select(.tag_name == $t) |
            "\(.id): \(if .draft then "draft" else "published \(.published_at)" end)"] | join(", ")' <<<"$all")).
            Delete the one that is not meant (a stray draft, or a release made by mistake), then run this again"
    fi
    if [[ -n $found ]]; then echo "$found"; fi
}

id_of() {   # tag -> its release's id; fails when it has none
    local info
    info=$(find_release "$1")
    [[ -n $info ]] || fail "$1 has no release"
    echo "${info%% *}"
}

assets() {   # release id -> its files, as one JSON array
    gh api "repos/$GH_REPO/releases/$1/assets?per_page=100" --paginate | jq -s 'add // []'
}

upload() {   # tag file ... -> the files on its release, each replacing a file of the same name
    local tag=$1 id f name old i
    shift
    id=$(id_of "$tag")
    for f in "$@"; do
        name=$(basename "$f")
        for i in 1 2 3; do
            old=$(assets "$id" | jq -r --arg n "$name" '.[] | select(.name == $n) | .id')
            if [[ -n $old ]]; then gh api -X DELETE "repos/$GH_REPO/releases/assets/$old" >/dev/null || true; fi
            if gh api -X POST "https://uploads.github.com/repos/$GH_REPO/releases/$id/assets?name=$(jq -rn --arg n "$name" '$n | @uri')" \
                -H 'Content-Type: application/octet-stream' --input "$f" >/dev/null; then
                echo "$tag ($id): $name"
                continue 2
            fi
            sleep $(( i * 5 ))
        done
        fail "$tag ($id): $name could not be uploaded"
    done
}

publish() {   # tag -> its release published
    local tag=$1 id
    # (a version's release only: nightly and rc are rolling prereleases, never "latest", and are not published here)
    [[ $tag =~ ^v[0-9] ]] || fail "$tag is not a version's tag: only a version's release is published here"
    id=$(id_of "$tag")
    # (tag_name with every change of a release: a draft changed without it loses its tag for an "untagged-..." one, as
    # v2.2.0-rc.1's did when its notes were written, and publishing that would have tagged the commit so)
    if [[ $tag == *-* ]]; then
        jq -n --arg t "$tag" '{tag_name: $t, draft: false, prerelease: true, make_latest: "false"}'
    else
        jq -n --arg t "$tag" '{tag_name: $t, draft: false, prerelease: false, make_latest: "true"}'
    fi | gh api -X PATCH "repos/$GH_REPO/releases/$id" --input - --jq '"\(.tag_name) (\(.id)) published: \(.html_url)"'
}

table() {
    local appimage="the \`.AppImage\`" zip="the \`-Windows-x64.zip\`"
    if [[ $1 =~ ^v[0-9] ]]; then
        appimage="\`Hookshot-$1-x86_64.AppImage\`"
        zip="\`Hookshot-$1-Windows-x64.zip\`"
    fi
    cat <<EOF
$MARK
### Which file do I download?

| On | Download |
|---|---|
| Windows | \`HookshotBuilder.exe\`, the setup |
| Linux, Steam Deck | $appimage |

You need your own copy of the game. \`hookshot-setup.txt\`, \`hookshot-linux.txt\` and the \`.zsync\` are for the updaters, and $zip (the setup and its \`recomplauncher.json\`) for RecompLauncher: on Windows, download \`HookshotBuilder.exe\` instead. To check a download: \`sha256sum -c --ignore-missing SHA256SUMS\`, or \`gh attestation verify <file> -R $GH_REPO\`.
EOF
}

links() {   # tag [id] -> big links to the Windows setup and the Linux AppImage on the release (what is there of them)
    local id=${2:-} names exe appimage base="https://github.com/$GH_REPO/releases/download/$1"
    [[ -n $id ]] || id=$(id_of "$1")
    names=$(assets "$id" | jq -r '.[].name')
    exe=$(grep -xF HookshotBuilder.exe <<<"$names" || true)
    appimage=$(grep -E '\.AppImage$' <<<"$names" | head -n1 || true)
    [[ -n $exe || -n $appimage ]] || return 0
    echo "$LINKS"
    if [[ -n $exe ]]; then echo "## [⬇ Download for Windows]($base/$exe)"; fi
    if [[ -n $appimage ]]; then echo "## [⬇ Download for Linux / Steam Deck]($base/$appimage)"; fi
    echo
    echo '---'
}

sums() {   # release id -> SHA256SUMS of the release's other files, sorted by name
    local list name digest asset dir
    list=$(assets "$1" | jq -r '.[] | select(.name != "SHA256SUMS") | "\(.name)\t\(.digest // "")\t\(.id)"' | LC_ALL=C sort)
    [[ -n $list ]] || return 0
    while IFS=$'\t' read -r name digest asset; do
        if [[ ! $digest =~ ^sha256:[0-9a-f]{64}$ ]]; then
            dir=$(mktemp -d)
            gh api -H 'Accept: application/octet-stream' "repos/$GH_REPO/releases/assets/$asset" >"$dir/file"
            digest=sha256:$(sha256sum "$dir/file" | cut -d' ' -f1)
            rm -rf "$dir"
        fi
        printf '%s  %s\n' "${digest#sha256:}" "$name"
    done <<<"$list"
}

uploaded() {   # release id -> the SHA256SUMS on the release (nothing when there is none)
    local asset i file
    file=$(mktemp)
    # (looked up again when its read fails: the nightly's other platform's run replaces it, under a new id, between
    # this run's listing and its read, and an empty answer here made each run upload it again, over the other's read)
    for i in 1 2 3; do
        asset=$(assets "$1" | jq -r '.[] | select(.name == "SHA256SUMS") | .id')
        [[ -n $asset ]] || break
        if gh api -H 'Accept: application/octet-stream' "repos/$GH_REPO/releases/assets/$asset" >"$file"; then
            cat "$file"
            break
        fi
    done
    rm -f "$file"
}

finish() {
    local tag=$1 out=${2:-} id body dir i
    id=$(id_of "$tag")
    body=$(gh api "repos/$GH_REPO/releases/$id" --jq '.body // ""')
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' RETURN
    # (the links are made again each time: the nightly's AppImage is named after its build)
    body=$(awk -v m="$LINKS" '$0 == m { skip = 1; next } skip && $0 == "---" { skip = 0; drop = 1; next }
        drop && $0 == "" { drop = 0; next } { drop = 0 } !skip' <<<"$body")
    {
        links "$tag" "$id"; echo
        if [[ -n $body ]]; then printf '%s\n' "$body"; fi
        if [[ $body != *"$MARK"* ]]; then printf '\n'; table "$tag"; fi
    } >"$dir/notes.md"
    # (with its tag_name: a draft whose notes are changed without it comes back with an "untagged-..." tag, and is then
    # no longer found by its tag: publish, above)
    jq -n --arg t "$tag" --rawfile body "$dir/notes.md" '{tag_name: $t, body: $body}' |
        gh api -X PATCH "repos/$GH_REPO/releases/$id" --input - >/dev/null
    for i in 1 2 3 4 5; do
        sums "$id" >"$dir/SHA256SUMS"
        if [[ -s $dir/SHA256SUMS ]] && cmp -s "$dir/SHA256SUMS" <(uploaded "$id"); then
            cat "$dir/SHA256SUMS"
            if [[ -n $out ]]; then cp "$dir/SHA256SUMS" "$out"; fi
            return 0
        fi
        (( i == 1 )) || sleep 5
        upload "$tag" "$dir/SHA256SUMS" >/dev/null
    done
    echo "release_files.sh: $tag: SHA256SUMS still differs from the files after 5 uploads" >&2
    return 1
}

check() {
    local tag=$1 id names want ok=0
    shift
    id=$(id_of "$tag")
    names=$(assets "$id" | jq -r '.[].name')
    echo "$names"
    for want in "$@" SHA256SUMS; do
        grep -qxF "$want" <<<"$names" || { echo "::error::$tag has no $want"; ok=1; }
    done
    if (( ok == 0 )) && ! cmp -s <(sums "$id") <(uploaded "$id"); then
        echo "::error::$tag: SHA256SUMS does not list every file with its digest"
        ok=1
    fi
    return $ok
}

case ${1:-} in
    find) find_release "$2" ;;
    upload) shift; upload "$@" ;;
    publish) publish "$2" ;;
    table) table "$2" ;;
    links) links "$2" ;;
    finish) finish "$2" "${3:-}" ;;
    check) shift; check "$@" ;;
    *) sed -n '16,32p' "$0" >&2; exit 2 ;;
esac
