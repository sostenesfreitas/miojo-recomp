#!/bin/bash
# Promote: a release candidate (v2.1.0-rc.2) published as its version (v2.1.0), the release players get. The Release
# workflow runs this when it is started by hand (docs/versioning.md, "Promoting a release candidate"). Needs gh, with
# GH_TOKEN (contents: write) and GH_REPO set.
#
#   scripts/release_promote.sh prepare [<rc tag>]   the release candidate (the newest one when not given) checked, its
#                                                   version's tag made on the same commit, and that release made as a
#                                                   draft whose notes are those of the version's release candidates.
#                                                   Prints tag=, rc= and sha= lines for $GITHUB_OUTPUT. Run again
#                                                   after a failure, it goes on with the same tag and draft. Refused
#                                                   when the version is already published, when another Promote run
#                                                   is in progress, or when the tag has more than one release.
#   scripts/release_promote.sh record <tag> [<rc tag>]
#                                                   .release-please-manifest.json set to the version, when it still
#                                                   holds one of its release candidates, so the next release pull
#                                                   request proposes the next version's first release candidate (and
#                                                   .release-please-version); and the version's entry at the top of
#                                                   CHANGELOG.md. In the checkout it is run in; the caller
#                                                   commits it. Without the rc tag, the newest published release
#                                                   candidate of the version on the tag's commit. Run again, it
#                                                   changes nothing.
#
# The tag is made with GITHUB_TOKEN, so it starts no workflow: the Release workflow builds the files itself, as it does
# for a release candidate. They are built again from the same commit, not copied from the release candidate, because
# every file names the version it was built as (tools/version.txt, the AppImage's VERSION, the setup lists' tag) and
# the updaters compare that with the latest release's tag.
set -euo pipefail
shopt -s inherit_errexit
FILES=$(dirname "$0")/release_files.sh
MARK='<!-- release-files -->'
RC_TAG='^v([0-9]+\.[0-9]+\.[0-9]+)-rc\.([0-9]+)$'

fail() { echo "::error::$*" >&2; exit 1; }

commit_of() {   # tag -> the commit it names (a lightweight tag, or an annotated one)
    local type sha
    read -r type sha < <(gh api "repos/$GH_REPO/git/ref/tags/$1" --jq '"\(.object.type) \(.object.sha)"')
    if [[ $type == tag ]]; then
        sha=$(gh api "repos/$GH_REPO/git/tags/$sha" --jq .object.sha)
    fi
    echo "$sha"
}

rc_releases() {   # the published release candidates' tags, oldest first
    local all
    all=$(gh api "repos/$GH_REPO/releases?per_page=100" --paginate | jq -s 'add // []') ||
        fail "the releases could not be listed"
    jq -r '.[] | select(.draft | not) | .tag_name' <<<"$all" | grep -E "$RC_TAG" | sort -V || true
}

other_promotions() {   # the Release workflow's other workflow_dispatch runs in progress (their URLs)
    local wf
    [[ -n ${GITHUB_RUN_ID:-} ]] || return 0
    wf=$(gh api "repos/$GH_REPO/actions/runs/$GITHUB_RUN_ID" --jq .workflow_id)
    gh api "repos/$GH_REPO/actions/workflows/$wf/runs?event=workflow_dispatch&status=in_progress&per_page=100" \
        --jq ".workflow_runs[] | select(.id != $GITHUB_RUN_ID) | .html_url"
}

prepare() {
    local rc=${1:-} version n tag sha main status info id draft others notes dir old
    if [[ -z $rc ]]; then
        rc=$(rc_releases | tail -n 1)
        [[ -n $rc ]] || fail "there is no published release candidate to promote"
    fi
    [[ $rc =~ $RC_TAG ]] || fail "$rc is not a release candidate's tag (v<major>.<minor>.<patch>-rc.<n>)"
    version=${BASH_REMATCH[1]}
    n=${BASH_REMATCH[2]}
    tag=v$version
    # (a published release candidate: its builds passed and its files were checked)
    info=$("$FILES" find "$rc")
    [[ $info == *" false true" ]] ||
        fail "$rc is not a published prerelease: only a release candidate whose files are all there is promoted"
    sha=$(commit_of "$rc")
    # (a commit of main: never one from a branch that was not merged)
    main=${GITHUB_SHA:-$(gh api "repos/$GH_REPO/commits/main" --jq .sha)}
    status=$(gh api "repos/$GH_REPO/compare/$sha...$main" --jq .status)
    [[ $status == ahead || $status == identical ]] || fail "$rc ($sha) is not on main"

    # (one promotion of a version at a time: the Release workflow runs one Promote at a time, and this refuses a run
    # that would go on beside another, an older run started before that rule or a re-run of its jobs)
    others=$(other_promotions)
    [[ -z $others ]] || fail "another Promote run is in progress: $others. Wait for it to finish, or cancel it, then start this again"
    # (the version's release, found by listing, drafts too: GitHub's get-a-release-by-tag finds no draft)
    info=$("$FILES" find "$tag")
    read -r id draft _ <<<"$info"
    if [[ $draft == false ]]; then
        fail "$tag is already published ($(gh api "repos/$GH_REPO/releases/$id" --jq '"\(.html_url), \(.published_at)"')):" \
            "a version is promoted once. Its record (.release-please-manifest.json and CHANGELOG.md) is made by the" \
            "Release workflow's record job; see docs/versioning.md, \"When a promotion goes wrong\""
    fi
    if gh api "repos/$GH_REPO/git/ref/tags/$tag" >/dev/null 2>&1; then
        # (a promotion run again after a failure: the same tag, and its release still a draft or not made)
        old=$(commit_of "$tag")
        [[ $old == "$sha" ]] || fail "$tag already exists, on $old, not on $rc's commit $sha"
    else
        gh api -X POST "repos/$GH_REPO/git/refs" -f ref="refs/tags/$tag" -f sha="$sha" >/dev/null
    fi

    # The notes: the release candidates of this version up to this one, newest first, each with its own notes (their
    # changelog entries: each the changes since the release candidate before it), without their table of files
    dir=$(mktemp -d)
    trap 'rm -rf "$dir"' RETURN
    {
        printf 'Release candidate [%s](https://github.com/%s/releases/tag/%s), published as %s: the same commit, built again so its files carry this version. Its changes, release candidate by release candidate:\n' \
            "$rc" "$GH_REPO" "$rc" "$tag"
        rc_releases | grep -E "^v${version//./\\.}-rc\.[0-9]+$" | sort -rV | while read -r old; do
            (( ${old##*.} <= n )) || continue
            notes=$(gh release view "$old" --json body --jq .body)
            notes=${notes%%"$MARK"*}
            notes=${notes%"${notes##*[![:space:]]}"}   # (no blank lines before the table)
            printf '\n%s\n' "$notes"
        done
    } >"$dir/notes.md"
    cat "$dir/notes.md"
    if [[ -n $id ]]; then
        # (with its tag_name: scripts/release_files.sh publish says what a draft changed without it loses)
        jq -n --arg t "$tag" --rawfile body "$dir/notes.md" '{tag_name: $t, draft: true, body: $body}' |
            gh api -X PATCH "repos/$GH_REPO/releases/$id" --input - >/dev/null
    else
        gh release create "$tag" --verify-tag --draft --title "$tag" --notes-file "$dir/notes.md" >/dev/null
    fi
    # (exactly one release of the tag, the draft: find fails on two)
    info=$("$FILES" find "$tag")
    read -r id draft _ <<<"$info"
    [[ $draft == true ]] || fail "$tag: its draft release is not there after making it (${info:-none})"
    printf 'tag=%s\nrc=%s\nsha=%s\n' "$tag" "$rc" "$sha" >>"${GITHUB_OUTPUT:-/dev/stdout}"
}

record() {
    local tag=$1 rc=${2:-} version manifest date entry sha old
    version=${tag#v}
    [[ $tag =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] || fail "$tag is not a version's tag (v<major>.<minor>.<patch>)"
    if [[ -z $rc ]]; then
        # (the newest published release candidate of the version on the same commit: the one that was promoted)
        sha=$(commit_of "$tag")
        while read -r old; do
            if [[ -n $old && $(commit_of "$old") == "$sha" ]]; then rc=$old; break; fi
        done < <(rc_releases | grep -E "^v${version//./\\.}-rc\.[0-9]+$" | sort -rV)
        [[ -n $rc ]] || fail "$tag: no published release candidate of $version is on its commit $sha"
    fi
    manifest=$(jq -r '."."' .release-please-manifest.json)
    if [[ $manifest =~ ^${version//./\\.}-rc\.[0-9]+$ ]]; then
        jq --arg v "$version" '."." = $v' .release-please-manifest.json >.release-please-manifest.json.new
        mv .release-please-manifest.json.new .release-please-manifest.json
        echo ".release-please-manifest.json: $manifest -> $version"
        # (release-please's version file, as the release pull request writes it)
        if [[ -f .release-please-version ]]; then echo "$version" >.release-please-version; fi
    else
        # (a later version's release candidate is under way, 2.2.0-rc.1 while 2.1.0 is promoted: it goes on from there)
        echo ".release-please-manifest.json stays at $manifest"
    fi
    if ! grep -q "^## \[$version\]" CHANGELOG.md; then
        date=$(date -u +%Y-%m-%d)
        entry="## [$version](https://github.com/$GH_REPO/releases/tag/$tag) ($date)

Release candidate [$rc](https://github.com/$GH_REPO/releases/tag/$rc), published as $version. Its changes are in its release candidates' entries below."
        # (above the newest entry, where release-please puts its own)
        awk -v entry="$entry" '!done && /^##? ?\[?v?[0-9]/ { print entry; print ""; done = 1 } { print }
            END { if (!done) { print ""; print entry } }' CHANGELOG.md >CHANGELOG.md.new
        mv CHANGELOG.md.new CHANGELOG.md
    fi
}

case ${1:-} in
    prepare) prepare "${2:-}" ;;
    record) record "$2" "${3:-}" ;;
    *) sed -n '6,21p' "$0" >&2; exit 2 ;;
esac
