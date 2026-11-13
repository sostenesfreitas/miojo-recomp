"""Run the release scripts (scripts/release_files.sh, release_upload.sh, release_promote.sh, release_rc.sh) against a
stand-in gh.

No network: the stand-in keeps the repository's releases, tags and runs in a JSON file and answers the REST calls the
scripts make, a listing of releases with the drafts in it, as GitHub does for a token that can push. It checks what
v2.1.0's promotion got wrong: a release found by listing, drafts too, so a failed lookup is never "no release" and a
draft never gets a published release made beside it; the files go to the release by its id; Promote refuses a
version already published, a tag with two releases and a second run in progress; and the record is made once. Also
that the table names every file (the RecompLauncher zip too), and that the rc prerelease, which release candidates'
AppImages update from, holds the newest release candidate or release and is never "latest". And that the nightly
moves forward along main while newer pushes are still building, never back, also when the Linux and Windows runs of
two commits race each other, and that each platform's files stay one commit's.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

# (Linux only: bash and jq)
RUN_ALL_SKIP = {"windows": "the release scripts are bash, run by Linux runners; exercised on Linux"}

ROOT = Path(__file__).resolve().parents[1]
REPO = "owner/repo"
COMMIT = "c" * 40

# The stand-in gh: `gh api` for the endpoints the scripts use, `gh release create` and `gh release view --json body`.
# --jq is run with the real jq. FAKE_GH_FAIL names calls that fail as GitHub's do now and then (HTTP 502).
STUB = r'''#!/usr/bin/env python3
import hashlib, json, os, subprocess, sys
from urllib.parse import urlsplit, parse_qs
STATE = os.environ["FAKE_GH_STATE"]
REPO = os.environ["GH_REPO"]
import fcntl
LOCK = open(STATE + ".lock", "w")
fcntl.flock(LOCK, fcntl.LOCK_EX)   # (one call at a time, as GitHub's are to its own state; held until this call exits)
with open(STATE) as f:
    s = json.load(f)
def save():   # (whole or not at all: release_files.sh check runs two gh calls at once, cmp <(sums) <(uploaded))
    tmp = "%s.%d" % (STATE, os.getpid())
    with open(tmp, "w") as f:
        json.dump(s, f)
    os.replace(tmp, STATE)
def out(value, jq=None):
    text = json.dumps(value)
    if jq:
        text = subprocess.run(["jq", "-r", jq], input=text, capture_output=True, text=True, check=True).stdout
        sys.stdout.write(text)
    else:
        print(text)
def fail(code, message):
    save()
    sys.stderr.write("gh: %s (HTTP %d)\n" % (message, code))
    sys.exit(1)
def release(rid):
    for r in s["releases"]:
        if r["id"] == int(rid):
            return r
    fail(404, "Not Found")
def public(r):
    return {k: v for k, v in r.items() if k != "assets"}
def asset(a):
    return {k: v for k, v in a.items() if k != "content"}
def compare(base, head):   # (GitHub's compare status of head against base, on main's history, oldest first)
    history = s.get("history")
    if history is None:
        return "ahead"
    if base not in history or head not in history:
        return "diverged"
    b, h = history.index(base), history.index(head)
    return "identical" if b == h else "ahead" if h > b else "behind"
argv = sys.argv[1:]
s["calls"].append(argv)
fails = os.environ.get("FAKE_GH_FAIL", "").split(",")
if argv[:2] == ["release", "create"]:
    tag = argv[2]
    if "release-create" in fails:
        fail(502, "Bad Gateway")
    draft = "--draft" in argv
    if not draft and any(r["tag_name"] == tag and not r["draft"] for r in s["releases"]):
        fail(422, "Validation Failed: already_exists")
    body = "generated notes" if "--generate-notes" in argv else ""
    if "--notes-file" in argv:
        body = open(argv[argv.index("--notes-file") + 1]).read()
    if "--notes" in argv:
        body = argv[argv.index("--notes") + 1]
    s["next_id"] += 1
    s["releases"].append({"id": s["next_id"], "tag_name": tag, "draft": draft, "prerelease": "--prerelease" in argv,
                          "published_at": None if draft else "now", "html_url": "https://example/" + tag,
                          "body": body, "assets": []})
    save()
    sys.exit(0)
if argv[:2] in (["release", "view"], ["release", "edit"], ["release", "delete-asset"]):
    tag = argv[2]
    rs = [r for r in s["releases"] if r["tag_name"] == tag and not r["draft"]]
    if not rs:
        fail(404, "release not found")
    r = rs[0]
    if argv[1] == "view":
        out({"body": r["body"], "assets": [{"name": a["name"]} for a in r["assets"]]},
            argv[argv.index("--jq") + 1] if "--jq" in argv else None)
    elif argv[1] == "edit":
        if "--notes" in argv:
            r["body"] = argv[argv.index("--notes") + 1]
        if "--prerelease" in argv:
            r["prerelease"] = True
    else:
        r["assets"] = [a for a in r["assets"] if a["name"] != argv[3]]
    save()
    sys.exit(0)
assert argv[0] == "api", argv
args = argv[1:]
path = None
method, jq, data, fields = "GET", None, None, {}
i = 0
while i < len(args):
    a = args[i]
    if a == "-X": method = args[i + 1]; i += 2
    elif a == "--jq": jq = args[i + 1]; i += 2
    elif a == "-H": i += 2
    elif a == "--paginate": i += 1
    elif a == "--input":
        src = args[i + 1]
        data = sys.stdin.buffer.read() if src == "-" else open(src, "rb").read()
        i += 2
    elif a in ("-f", "-F"):
        k, v = args[i + 1].split("=", 1); fields[k] = v; i += 2
    elif path is None and not a.startswith("-"): path = a; i += 1
    else: raise SystemExit("stand-in gh: unknown argument " + a)
url = urlsplit(path)
query = parse_qs(url.query)
parts = url.path.strip("/").split("/")
assert parts[:3] == ["repos"] + REPO.split("/"), path
p = parts[3:]
if p == ["releases"] and method == "GET":
    if "releases-list" in fails:
        fail(502, "Bad Gateway")
    out([public(r) for r in s["releases"]], jq)
elif p[:2] == ["releases", "tags"]:
    rs = [r for r in s["releases"] if r["tag_name"] == p[2] and not r["draft"]]
    if not rs:
        fail(404, "Not Found")
    out(public(rs[0]), jq)
elif p[:2] == ["releases", "assets"] and method == "DELETE":
    for r in s["releases"]:
        r["assets"] = [a for a in r["assets"] if a["id"] != int(p[2])]
elif p[:2] == ["releases", "assets"]:
    for r in s["releases"]:
        for a in r["assets"]:
            if a["id"] == int(p[2]):
                # (FAKE race: the other run of the nightly replaced this file, under a new id, since it was listed)
                if s.get("race", {}).get(a["name"]):
                    s["race"][a["name"]] -= 1
                    s["next_id"] += 1
                    a["id"] = s["next_id"]
                    fail(404, "Not Found")
                sys.stdout.write(a["content"])
                save()
                sys.exit(0)
    fail(404, "Not Found")
elif len(p) == 3 and p[0] == "releases" and p[2] == "assets" and method == "POST":
    r = release(p[1])
    name = query["name"][0]
    if any(a["name"] == name for a in r["assets"]):
        fail(422, "already_exists")
    s["next_id"] += 1
    r["assets"].append({"id": s["next_id"], "name": name, "content": data.decode(),
                        "digest": "sha256:" + hashlib.sha256(data).hexdigest()})
elif len(p) == 3 and p[0] == "releases" and p[2] == "assets":
    out([asset(a) for a in release(p[1])["assets"]], jq)
elif p == ["releases"] and method == "POST":
    change = json.loads(data)
    if any(r["tag_name"] == change["tag_name"] for r in s["releases"]):
        fail(422, "Validation Failed: already_exists")
    s["next_id"] += 1
    r = {"id": s["next_id"], "html_url": "https://example/" + change["tag_name"], "assets": [], "body": "",
         "published_at": None if change.get("draft") else "now"}
    r.update(change)
    s["releases"].append(r)
    out(public(r), jq)
elif len(p) == 2 and p[0] == "releases" and method == "PATCH":
    r = release(p[1])
    change = json.loads(data)
    # (as GitHub does: a draft changed without its tag_name comes back with an "untagged-..." one. v2.2.0-rc.1's draft
    # lost its tag so when its notes were written, and was then not found by its tag for the publish)
    if r["draft"] and "tag_name" not in change:
        r["tag_name"] = "untagged-%s" % p[1]
    r.update(change)
    if change.get("draft") is False and not r.get("published_at"):
        r["published_at"] = "now"
    out(public(r), jq)
elif len(p) == 2 and p[0] == "releases":
    out(public(release(p[1])), jq)
elif p[:3] == ["git", "ref", "tags"]:
    sha = s["tags"].get(p[3])
    if not sha:
        fail(404, "Not Found")
    out({"object": {"type": "commit", "sha": sha}}, jq)
elif p[:3] == ["git", "refs", "tags"] and method == "PATCH":
    if p[3] not in s["tags"]:
        fail(422, "Reference does not exist")
    # (FAKE race: the other platform's run moves the tag between this run's look at it and its move)
    race = s.get("race", {}).pop(p[3], None)
    if race:
        s["tags"][p[3]] = race
    if fields.get("force") != "true" and compare(s["tags"][p[3]], fields["sha"]) not in ("ahead", "identical"):
        fail(422, "Update is not a fast forward")
    s["tags"][p[3]] = fields["sha"]
elif p == ["git", "refs"] and method == "POST":
    ref = fields["ref"].removeprefix("refs/tags/")
    if ref in s["tags"]:
        fail(422, "Reference already exists")
    s["tags"][ref] = fields["sha"]
elif p == ["commits", "main"]:
    out({"sha": s["main"]}, jq)
elif p[0] == "compare":
    base, head = p[1].split("...")
    out({"status": compare(base, head)}, jq)
elif p[:2] == ["actions", "runs"]:
    out({"workflow_id": 7}, jq)
elif p[:2] == ["actions", "workflows"]:
    out({"workflow_runs": [r for r in s["runs"] if r["status"] == query["status"][0]]}, jq)
else:
    raise SystemExit("stand-in gh: no endpoint " + method + " " + path)
save()
'''


def rel(rid, tag, draft, prerelease, body="", assets=()):
    return {"id": rid, "tag_name": tag, "draft": draft, "prerelease": prerelease,
            "published_at": None if draft else "then", "html_url": "https://example/" + tag, "body": body,
            "assets": [{"id": rid * 100 + n, "name": name, "content": name,
                        "digest": "sha256:" + hashlib.sha256(name.encode()).hexdigest()}
                       for n, name in enumerate(assets)]}


@unittest.skipIf(os.name == "nt" or not shutil.which("jq") or not shutil.which("bash"), "bash and jq")
class ReleaseScripts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="release-scripts-")
        self.dir = Path(self.temp.name)
        tools = self.dir / "bin"
        tools.mkdir()
        (tools / "gh").write_text(STUB, encoding="utf-8")
        (tools / "gh").chmod(0o755)
        # (no waiting between retries)
        (tools / "sleep").write_text("#!/bin/sh\n", encoding="utf-8")
        (tools / "sleep").chmod(0o755)
        self.state_file = self.dir / "state.json"
        self.files = self.dir / "dist"
        self.files.mkdir()
        for name in ("Hookshot-v2.1.0-x86_64.AppImage", "hookshot-linux.txt"):
            (self.files / name).write_text("built " + name, encoding="utf-8")
        self.env = dict(os.environ, PATH=str(tools) + os.pathsep + os.environ["PATH"], GH_REPO=REPO,
                        GH_TOKEN="x", FAKE_GH_STATE=str(self.state_file))
        for k in ("GITHUB_RUN_ID", "GITHUB_SHA", "GITHUB_OUTPUT", "RELEASE_SUMS_OUT", "RELEASE_FROM_WORKFLOW"):
            self.env.pop(k, None)

    def tearDown(self):
        self.temp.cleanup()

    def given(self, releases, tags=None, runs=(), **more):
        self.state_file.write_text(json.dumps(dict({
            "releases": releases, "tags": tags or {}, "runs": list(runs), "main": COMMIT, "next_id": 1000,
            "calls": []}, **more)), encoding="utf-8")

    def state(self):
        return json.loads(self.state_file.read_text(encoding="utf-8"))

    def run_script(self, script, *args, cwd=None, **env):
        result = subprocess.run(["bash", str(ROOT / "scripts" / script), *args], capture_output=True, text=True,
                                env=dict(self.env, **env), cwd=cwd or self.dir, timeout=60)
        return result

    def upload(self, tag="v2.1.0", **env):
        return self.run_script("release_upload.sh", tag, *sorted(str(p) for p in self.files.iterdir()), **env)

    def creates(self):
        return [c for c in self.state()["calls"] if c[:2] == ["release", "create"]]

    # release_upload.sh

    def test_draft_gets_the_files_by_id_and_stays_a_draft(self):
        self.given([rel(5, "v2.1.0", True, False, "promote notes"), rel(4, "v2.1.0-rc.2", False, True)])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertEqual(r.returncode, 0, r.stderr)
        draft = self.state()["releases"][0]
        self.assertTrue(draft["draft"])
        self.assertEqual(draft["body"], "promote notes")
        self.assertEqual(sorted(a["name"] for a in draft["assets"]), ["Hookshot-v2.1.0-x86_64.AppImage", "hookshot-linux.txt"])
        self.assertFalse(self.creates())
        uploads = [a for c in self.state()["calls"] for a in c if "uploads.github.com" in a]
        self.assertTrue(uploads and all("/releases/5/assets?" in u for u in uploads), uploads)

    def test_a_file_again_replaces_the_one_there(self):
        self.given([rel(5, "v2.1.0", True, False, assets=["hookshot-linux.txt"])])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertEqual(r.returncode, 0, r.stderr)
        linux = [a for a in self.state()["releases"][0]["assets"] if a["name"] == "hookshot-linux.txt"]
        self.assertEqual([a["content"] for a in linux], ["built hookshot-linux.txt"])

    def test_listing_that_fails_makes_no_release(self):
        # (v2.1.0: a lookup of the draft that failed read as "no release", and a published "latest" one was made)
        for from_workflow in ("true", "false"):
            with self.subTest(from_workflow=from_workflow):
                self.given([rel(5, "v2.1.0", True, False)])
                r = self.upload(RELEASE_FROM_WORKFLOW=from_workflow, FAKE_GH_FAIL="releases-list")
                self.assertNotEqual(r.returncode, 0)
                self.assertFalse(self.creates())
                self.assertEqual(len(self.state()["releases"]), 1)
                self.assertTrue(self.state()["releases"][0]["draft"])

    def test_release_workflow_tag_without_its_draft_stops(self):
        self.given([])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has no release", r.stderr)
        self.assertFalse(self.creates())
        self.assertFalse(self.state()["releases"])

    def test_tag_pushed_by_hand_gets_its_release_and_sums(self):
        self.given([], tags={"v2.1.0": COMMIT})
        r = self.upload(RELEASE_SUMS_OUT=str(self.dir / "SUMS"))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(len(self.creates()), 1)
        (made,) = self.state()["releases"]
        self.assertFalse(made["draft"])
        names = sorted(a["name"] for a in made["assets"])
        self.assertEqual(names, ["Hookshot-v2.1.0-x86_64.AppImage", "SHA256SUMS", "hookshot-linux.txt"])
        sums = next(a["content"] for a in made["assets"] if a["name"] == "SHA256SUMS")
        self.assertEqual(sums, (self.dir / "SUMS").read_text(encoding="utf-8"))
        self.assertIn("Which file do I download?", made["body"])
        check = self.run_script("release_files.sh", "check", "v2.1.0", "hookshot-linux.txt")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)

    def test_sums_replaced_between_listing_and_read_are_read_again(self):
        # (CI, the nightly's two runs at once: the other platform's run replaced SHA256SUMS after this run listed it;
        # the failed read counted as none there, and each run uploaded it again, over the other's read, five times)
        self.given([], tags={"v2.1.0": COMMIT})
        r = self.upload()
        self.assertEqual(r.returncode, 0, r.stderr)
        state = self.state()
        state.update(race={"SHA256SUMS": 2}, calls=[])
        self.state_file.write_text(json.dumps(state), encoding="utf-8")
        check = self.run_script("release_files.sh", "check", "v2.1.0", "hookshot-linux.txt")
        self.assertEqual(check.returncode, 0, check.stdout + check.stderr)
        r = self.run_script("release_files.sh", "finish", "v2.1.0")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["race"], {"SHA256SUMS": 0})
        self.assertFalse([c for c in self.state()["calls"] if any("name=SHA256SUMS" in a for a in c)])

    # the nightly (scripts/release_upload.sh nightly): main's history Z, A, B, the newest last

    Z, A, B = ("1" * 40, "a" * 40, "b" * 40)

    def nightly_files(self, sha, platform):
        """This run's files: the Linux AppImage, its .zsync and list, or the Windows setup, its list and zip."""
        out = self.dir / ("dist-" + platform + "-" + sha[:7])
        out.mkdir(exist_ok=True)
        version = "v2.1.0-rc.3-%d-g%s" % ({self.Z: 1, self.A: 2, self.B: 3}.get(sha, 9), sha[:8])
        names = (["Hookshot-%s-x86_64.AppImage" % version, "Hookshot-%s-x86_64.AppImage.zsync" % version,
                  "hookshot-linux.txt"] if platform == "linux" else
                 ["HookshotBuilder.exe", "hookshot-setup.txt", "Hookshot-%s-Windows-x64.zip" % version])
        for name in names:
            (out / name).write_text("%s %s %s" % (platform, version, name), encoding="utf-8")
        return [str(out / name) for name in names]

    def given_nightly(self, held, linux_at=None, windows_at=None, **more):
        """The nightly release with the Linux files of linux_at and the Windows files of windows_at, its tag at held."""
        nightly = rel(90, "nightly", False, True, "old notes")
        for sha, platform in ((linux_at, "linux"), (windows_at, "windows")):
            for f in self.nightly_files(sha, platform) if sha else ():
                data = Path(f).read_bytes()
                nightly["assets"].append({"id": 9000 + len(nightly["assets"]), "name": os.path.basename(f),
                                          "content": data.decode(),
                                          "digest": "sha256:" + hashlib.sha256(data).hexdigest()})
        tags = {"nightly": held} if held else {}
        self.given([nightly], tags=tags, history=[self.Z, self.A, self.B], **dict({"main": self.B}, **more))

    def nightly(self, sha, platform="linux", **env):
        return self.run_script("release_upload.sh", "nightly", *self.nightly_files(sha, platform), GITHUB_SHA=sha,
                               **env)

    def nightly_assets(self):
        (r,) = [x for x in self.state()["releases"] if x["tag_name"] == "nightly"]
        return {a["name"]: a["content"] for a in r["assets"]}

    def test_nightly_moves_forward_while_main_has_newer_commits(self):
        # (run 43112700938: 57ed3aa8 was built after main had moved on, and the nightly stayed where it was)
        self.given_nightly(self.Z, linux_at=self.Z)
        r = self.nightly(self.A)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.A)
        files = self.nightly_assets()
        self.assertIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-x86_64.AppImage", files)
        self.assertIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-x86_64.AppImage.zsync", files)
        self.assertNotIn("Hookshot-v2.1.0-rc.3-1-g11111111-x86_64.AppImage", files)
        self.assertNotIn("Hookshot-v2.1.0-rc.3-1-g11111111-x86_64.AppImage.zsync", files)
        self.assertEqual(files["hookshot-linux.txt"], "linux v2.1.0-rc.3-2-gaaaaaaaa hookshot-linux.txt")
        self.assertIn("SHA256SUMS", files)
        # (no force: a fast-forward, which GitHub refuses when the tag moved past this commit)
        moves = [c for c in self.state()["calls"] if "repos/%s/git/refs/tags/nightly" % REPO in c]
        self.assertEqual(len(moves), 1)
        self.assertIn("force=false", moves[0])
        (body,) = [x["body"] for x in self.state()["releases"] if x["tag_name"] == "nightly"]
        self.assertIn("Each file is named by the commit it was built from", body)
        self.assertIn("Which file do I download?", body)

    def test_nightly_never_goes_back(self):
        # (an older commit's run, or a re-run of one, finishing after a newer commit's)
        for platform in ("linux", "windows"):
            with self.subTest(platform=platform):
                self.given_nightly(self.B, linux_at=self.B, windows_at=self.B)
                before = self.nightly_assets()
                r = self.nightly(self.A, platform)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                self.assertIn("comes after", r.stdout)
                self.assertEqual(self.state()["tags"]["nightly"], self.B)
                self.assertEqual(self.nightly_assets(), before)
                self.assertFalse(self.uploads())

    def test_nightly_other_platform_of_the_same_commit_adds_its_files(self):
        self.given_nightly(self.A, linux_at=self.A, windows_at=self.Z)
        r = self.nightly(self.A, "windows")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.A)
        files = self.nightly_assets()
        self.assertIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-Windows-x64.zip", files)
        self.assertNotIn("Hookshot-v2.1.0-rc.3-1-g11111111-Windows-x64.zip", files)
        self.assertIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-x86_64.AppImage", files)
        self.assertFalse([c for c in self.state()["calls"] if "PATCH" in c and any("git/refs" in a for a in c)])

    def test_nightly_platforms_move_on_their_own(self):
        # (Windows of B first, then Linux of B: between the two, Linux's files are A's, which its list says)
        self.given_nightly(self.A, linux_at=self.A, windows_at=self.A)
        self.assertEqual(self.nightly(self.B, "windows").returncode, 0)
        files = self.nightly_assets()
        self.assertEqual(self.state()["tags"]["nightly"], self.B)
        self.assertIn("Hookshot-v2.1.0-rc.3-3-gbbbbbbbb-Windows-x64.zip", files)
        self.assertIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-x86_64.AppImage", files)
        r = self.nightly(self.B, "linux")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        files = self.nightly_assets()
        self.assertIn("Hookshot-v2.1.0-rc.3-3-gbbbbbbbb-x86_64.AppImage", files)
        self.assertNotIn("Hookshot-v2.1.0-rc.3-2-gaaaaaaaa-x86_64.AppImage", files)

    def test_nightly_race_lost_to_a_newer_commit_changes_nothing(self):
        # (Linux of A looked at the tag (Z) before Windows of B moved it; its fast-forward from B to A is refused)
        self.given_nightly(self.Z, linux_at=self.Z, windows_at=self.Z, race={"nightly": self.B})
        before = self.nightly_assets()
        r = self.nightly(self.A, "linux")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.B)
        self.assertEqual(self.nightly_assets(), before)
        self.assertFalse(self.uploads())

    def test_nightly_race_with_an_older_commit_still_moves_forward(self):
        # (Windows of B looked at the tag (Z) before Linux of A moved it: B comes after A, so the move goes on)
        self.given_nightly(self.Z, linux_at=self.Z, windows_at=self.Z, race={"nightly": self.A})
        r = self.nightly(self.B, "windows")
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.B)
        self.assertIn("Hookshot-v2.1.0-rc.3-3-gbbbbbbbb-Windows-x64.zip", self.nightly_assets())

    def test_nightly_two_runs_at_once_end_at_the_newer_commit(self):
        for first in ("A", "B"):
            with self.subTest(first=first):
                self.given_nightly(self.Z, linux_at=self.Z, windows_at=self.Z)
                order = [(self.A, "linux"), (self.B, "windows")]
                if first == "B":
                    order.reverse()
                env = dict(self.env)
                runs = [subprocess.Popen(["bash", str(ROOT / "scripts/release_upload.sh"), "nightly",
                                          *self.nightly_files(sha, platform)], cwd=self.dir, text=True,
                                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                         env=dict(env, GITHUB_SHA=sha)) for sha, platform in order]
                for run in runs:
                    output = run.communicate(timeout=120)[0]
                    self.assertEqual(run.returncode, 0, output)
                self.assertEqual(self.state()["tags"]["nightly"], self.B)
                files = self.nightly_assets()
                self.assertIn("Hookshot-v2.1.0-rc.3-3-gbbbbbbbb-Windows-x64.zip", files)
                # (Linux's files are A's, or still Z's when it lost the race: never a mix)
                linux = sorted(n for n in files if ".AppImage" in n)
                self.assertIn(linux, [["Hookshot-v2.1.0-rc.3-%s-x86_64.AppImage" % v,
                                       "Hookshot-v2.1.0-rc.3-%s-x86_64.AppImage.zsync" % v]
                                      for v in ("1-g11111111", "2-gaaaaaaaa")])
                self.assertIn(linux[0].split("-x86_64")[0].split("Hookshot-")[1], files["hookshot-linux.txt"])

    def test_nightly_made_when_there_is_no_tag(self):
        self.given_nightly(None)
        r = self.nightly(self.A)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.A)

    def test_nightly_after_main_was_rewritten(self):
        # (the tag holds a commit no longer on main: main's newest commit takes it; an older one leaves it)
        gone = "9" * 40
        self.given_nightly(gone, main=self.B)
        r = self.nightly(self.A)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], gone)
        self.assertFalse(self.uploads())
        r = self.nightly(self.B)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        self.assertEqual(self.state()["tags"]["nightly"], self.B)

    def test_two_releases_of_a_tag_are_refused(self):
        self.given([rel(5, "v2.1.0", True, False), rel(6, "v2.1.0", False, False)])
        r = self.upload(RELEASE_FROM_WORKFLOW="true")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has 2 releases", r.stderr)
        self.assertTrue(all(not x["assets"] for x in self.state()["releases"]))

    # release_files.sh publish

    def test_publish_by_id(self):
        for tag, prerelease, latest in (("v2.1.0-rc.3", True, "false"), ("v2.1.0", False, "true")):
            with self.subTest(tag=tag):
                self.given([rel(9, tag, True, prerelease)])
                r = self.run_script("release_files.sh", "publish", tag)
                self.assertEqual(r.returncode, 0, r.stderr)
                (done,) = self.state()["releases"]
                self.assertEqual((done["draft"], done["prerelease"], done["make_latest"]), (False, prerelease, latest))

    # release_promote.sh prepare

    def prepare(self, rc="v2.1.0-rc.2", **env):
        return self.run_script("release_promote.sh", "prepare", rc, GITHUB_SHA=COMMIT,
                               GITHUB_OUTPUT=str(self.dir / "out"), **env)

    def rcs(self):
        return [rel(1, "v2.1.0-rc.1", False, True, "rc.1 notes"), rel(2, "v2.1.0-rc.2", False, True, "rc.2 notes")]

    def test_prepare_makes_the_tag_and_one_draft(self):
        self.given(self.rcs(), tags={"v2.1.0-rc.1": "a" * 40, "v2.1.0-rc.2": COMMIT})
        r = self.prepare()
        self.assertEqual(r.returncode, 0, r.stderr)
        state = self.state()
        self.assertEqual(state["tags"]["v2.1.0"], COMMIT)
        (draft,) = [x for x in state["releases"] if x["tag_name"] == "v2.1.0"]
        self.assertTrue(draft["draft"])
        self.assertIn("rc.2 notes", draft["body"])
        self.assertIn("tag=v2.1.0", (self.dir / "out").read_text(encoding="utf-8"))

    def test_prepare_again_goes_on_with_its_draft(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", True, False, "old")], tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertFalse(self.creates())
        (draft,) = [x for x in self.state()["releases"] if x["tag_name"] == "v2.1.0"]
        self.assertEqual(draft["id"], 3)
        self.assertIn("rc.2 notes", draft["body"])

    def test_prepare_refuses_a_published_version(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", False, False)], tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("v2.1.0 is already published", r.stderr)
        self.assertEqual(self.state()["releases"][-1]["body"], "")

    def test_prepare_refuses_beside_another_promote_run(self):
        self.given(self.rcs(), tags={"v2.1.0-rc.2": COMMIT},
                   runs=[{"id": 11, "status": "in_progress", "html_url": "https://example/runs/11"},
                         {"id": 12, "status": "in_progress", "html_url": "https://example/runs/12"}])
        r = self.prepare(GITHUB_RUN_ID="12")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("another Promote run is in progress: https://example/runs/11", r.stderr)
        self.assertNotIn("v2.1.0", self.state()["tags"])

    def test_prepare_refuses_a_tag_with_two_releases(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", True, False), rel(4, "v2.1.0", True, False)],
                   tags={"v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        r = self.prepare()
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("has 2 releases", r.stderr)

    # release_promote.sh record

    def test_record_finds_the_release_candidate_and_is_made_once(self):
        self.given(self.rcs() + [rel(3, "v2.1.0", False, False)],
                   tags={"v2.1.0-rc.1": "a" * 40, "v2.1.0-rc.2": COMMIT, "v2.1.0": COMMIT})
        work = self.dir / "checkout"
        work.mkdir()
        (work / ".release-please-manifest.json").write_text('{".": "2.1.0-rc.3"}\n', encoding="utf-8")
        (work / "CHANGELOG.md").write_text("# Changelog\n\n## [2.1.0-rc.3](x) (2026-11-11)\n\n* a fix\n",
                                           encoding="utf-8")
        r = self.run_script("release_promote.sh", "record", "v2.1.0", cwd=work)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(json.loads((work / ".release-please-manifest.json").read_text(encoding="utf-8")), {".": "2.1.0"})
        changelog = (work / "CHANGELOG.md").read_text(encoding="utf-8")
        self.assertIn("Release candidate [v2.1.0-rc.2]", changelog)
        self.assertLess(changelog.index("## [2.1.0]"), changelog.index("## [2.1.0-rc.3]"))
        again = self.run_script("release_promote.sh", "record", "v2.1.0", cwd=work)
        self.assertEqual(again.returncode, 0, again.stderr)
        self.assertEqual((work / "CHANGELOG.md").read_text(encoding="utf-8"), changelog)

    # release_files.sh table

    def test_table_names_every_file(self):
        # (the RecompLauncher zip is on every release: the table says what it is for, and what Windows players want)
        for tag, zip_name in (("v2.1.0-rc.3", "`Hookshot-v2.1.0-rc.3-Windows-x64.zip`"), ("nightly", "the `-Windows-x64.zip`")):
            with self.subTest(tag=tag):
                r = self.run_script("release_files.sh", "table", tag)
                self.assertEqual(r.returncode, 0, r.stderr)
                for name in ("`HookshotBuilder.exe`", "`hookshot-setup.txt`", "`hookshot-linux.txt`", "`.zsync`",
                             zip_name, "RecompLauncher", "SHA256SUMS"):
                    self.assertIn(name, r.stdout)
                self.assertIn("for RecompLauncher: on Windows, download `HookshotBuilder.exe` instead", r.stdout)

    def test_finish_puts_the_table_with_the_zip_under_the_notes_once(self):
        zip_name = "Hookshot-v2.1.0-Windows-x64.zip"
        self.given([rel(5, "v2.1.0", True, False, "notes", assets=["HookshotBuilder.exe", zip_name])])
        for _ in range(2):
            r = self.run_script("release_files.sh", "finish", "v2.1.0")
            self.assertEqual(r.returncode, 0, r.stderr)
        body = self.state()["releases"][0]["body"]
        self.assertEqual(body.count("Which file do I download?"), 1)
        self.assertIn("`%s` (the setup and its `recomplauncher.json`) for RecompLauncher" % zip_name, body)
        self.assertLess(body.index("notes"), body.index("Which file do I download?"))

    def test_publish_refuses_a_rolling_release(self):
        for tag in ("rc", "nightly"):
            with self.subTest(tag=tag):
                self.given([rel(9, tag, True, True)])
                r = self.run_script("release_files.sh", "publish", tag)
                self.assertNotEqual(r.returncode, 0)
                self.assertIn("not a version's tag", r.stderr)
                self.assertTrue(self.state()["releases"][0]["draft"])

    # release_rc.sh

    def linux(self, tag):
        return ["Hookshot-%s-x86_64.AppImage" % tag, "Hookshot-%s-x86_64.AppImage.zsync" % tag]

    def released(self, rid, tag, prerelease=None, files=None):
        prerelease = "-" in tag if prerelease is None else prerelease
        names = self.linux(tag) + ["HookshotBuilder.exe", "SHA256SUMS"] if files is None else files
        return rel(rid, tag, False, prerelease, tag + " notes", assets=names)

    def rc_release(self):
        (r,) = [x for x in self.state()["releases"] if x["tag_name"] == "rc"]
        return r

    def sync(self, **env):
        r = self.run_script("release_rc.sh", **env)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r

    def uploads(self):
        return [a for c in self.state()["calls"] for a in c if "uploads.github.com" in a]

    def test_rc_holds_the_newest_release_candidate(self):
        self.given([rel(90, "nightly", False, True, assets=self.linux("v2.1.0-rc.3-4-gabcdef0")),
                    rel(91, "linux-parts", False, True, assets=["HookshotLinux-a.zip"]),
                    self.released(2, "v2.1.0-rc.2"), self.released(3, "v2.1.0-rc.3"), self.released(1, "v2.0.14"),
                    rel(4, "v2.1.0", True, False, assets=self.linux("v2.1.0"))],
                   tags={"nightly": "e" * 40, "v2.1.0-rc.2": "a" * 40, "v2.1.0-rc.3": "b" * 40, "v2.0.14": "d" * 40,
                         "v2.1.0": "b" * 40})
        self.sync()
        rc = self.rc_release()
        # (a prerelease, never "latest", with only the AppImage and its .zsync, the same files as on the release)
        self.assertEqual((rc["draft"], rc["prerelease"], rc["make_latest"]), (False, True, "false"))
        self.assertEqual(rc["name"], "Latest release candidate")
        self.assertEqual(sorted(a["name"] for a in rc["assets"]), self.linux("v2.1.0-rc.3"))
        self.assertEqual(sorted(a["content"] for a in rc["assets"]), self.linux("v2.1.0-rc.3"))
        self.assertEqual(self.state()["tags"]["rc"], "b" * 40)
        self.assertIn("[v2.1.0-rc.3](https://github.com/%s/releases/tag/v2.1.0-rc.3)" % REPO, rc["body"])
        self.assertLess(len(rc["body"]), 800)
        self.assertNotIn("Which file do I download?", rc["body"])
        # (the others are as they were)
        for x in self.state()["releases"]:
            if x["tag_name"] != "rc":
                self.assertNotIn("make_latest", x, x["tag_name"])
        # (run again: nothing is uploaded again, and there is still one rc release)
        before = len(self.uploads())
        self.sync()
        self.assertEqual(len(self.uploads()), before)
        self.assertEqual(sorted(a["name"] for a in self.rc_release()["assets"]), self.linux("v2.1.0-rc.3"))

    def test_rc_moves_on_to_the_next_release_candidate_and_the_release(self):
        tags = {"v2.1.0-rc.3": "b" * 40, "v2.1.0-rc.4": "c" * 40, "v2.1.0": "c" * 40, "rc": "b" * 40}
        rc = rel(50, "rc", False, True, "old", assets=self.linux("v2.1.0-rc.3"))
        rc["make_latest"] = "false"
        self.given([rc, self.released(3, "v2.1.0-rc.3"), self.released(4, "v2.1.0-rc.4")], tags=dict(tags))
        self.sync()
        self.assertEqual(sorted(a["name"] for a in self.rc_release()["assets"]), self.linux("v2.1.0-rc.4"))
        self.assertEqual(self.rc_release()["id"], 50)
        self.assertEqual(self.state()["tags"]["rc"], "c" * 40)
        # (promoted: v2.1.0 is newer than its release candidates)
        state = self.state()
        state["releases"].append(self.released(5, "v2.1.0"))
        self.state_file.write_text(json.dumps(state), encoding="utf-8")
        self.sync()
        rc = self.rc_release()
        self.assertEqual(sorted(a["name"] for a in rc["assets"]), self.linux("v2.1.0"))
        self.assertIn("[v2.1.0]", rc["body"])
        self.assertEqual((rc["prerelease"], rc["make_latest"]), (True, "false"))
        self.assertEqual([x["tag_name"] for x in self.state()["releases"]].count("rc"), 1)

    def test_rc_never_goes_back_to_an_older_version(self):
        # (v2.1.0 promoted while v2.2.0-rc.1 is out: rc stays with the newer release candidate)
        self.given([self.released(6, "v2.2.0-rc.1"), self.released(5, "v2.1.0"), self.released(4, "v2.1.0-rc.10"),
                    self.released(3, "v2.1.0-rc.9")],
                   tags={"v2.2.0-rc.1": "f" * 40, "v2.1.0": "c" * 40, "v2.1.0-rc.10": "c" * 40, "v2.1.0-rc.9": "b" * 40})
        self.sync()
        self.assertEqual(sorted(a["name"] for a in self.rc_release()["assets"]), self.linux("v2.2.0-rc.1"))

    def test_rc_passes_over_a_release_without_its_appimage(self):
        self.given([self.released(4, "v2.1.0-rc.4", files=["HookshotBuilder.exe"]), self.released(3, "v2.1.0-rc.3")],
                   tags={"v2.1.0-rc.4": "c" * 40, "v2.1.0-rc.3": "b" * 40})
        r = self.sync()
        self.assertIn("v2.1.0-rc.4 has no", r.stdout)
        self.assertEqual(sorted(a["name"] for a in self.rc_release()["assets"]), self.linux("v2.1.0-rc.3"))

    def test_rc_listing_that_fails_changes_nothing(self):
        self.given([self.released(3, "v2.1.0-rc.3")], tags={"v2.1.0-rc.3": "b" * 40})
        r = self.run_script("release_rc.sh", FAKE_GH_FAIL="releases-list")
        self.assertNotEqual(r.returncode, 0)
        self.assertEqual([x["tag_name"] for x in self.state()["releases"]], ["v2.1.0-rc.3"])
        self.assertNotIn("rc", self.state()["tags"])

    def test_rc_takes_no_upload(self):
        self.given([rel(50, "rc", False, True)])
        r = self.upload("rc")
        self.assertNotEqual(r.returncode, 0)
        self.assertIn("scripts/release_rc.sh", r.stderr)
        self.assertFalse(self.uploads())

    # the workflow

    def test_whole_promote_run_is_one_at_a_time(self):
        flow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        top = flow[:flow.index("\njobs:")]
        self.assertIn("\nconcurrency:\n  group: ${{ github.event_name == 'workflow_dispatch' && 'release-promote'", top)
        # (a job in the same group as its own run would wait for itself)
        self.assertEqual(flow.count("release-promote'"), 1)
        self.assertIn("  release:\n    types: [published]", top)
        self.assertNotIn("gh release edit", flow)
        for name in ("linux-appimage.yml", "windows-builder.yml"):
            text = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
            self.assertIn("RELEASE_FROM_WORKFLOW: ${{ inputs.tag && 'true' || 'false' }}", text, name)

    def test_release_candidates_update_from_rc(self):
        flow = (ROOT / ".github/workflows/release.yml").read_text(encoding="utf-8")
        job = flow[flow.index("\n  rc:\n"):flow.index("\n  record:\n")]
        self.assertIn("needs: [publish]", job)
        self.assertIn("group: release-rc\n", job)
        self.assertIn("run: scripts/release_rc.sh", job)
        self.assertIn("startsWith(github.event.release.tag_name, 'v')", job)
        linux = (ROOT / ".github/workflows/linux-appimage.yml").read_text(encoding="utf-8")
        self.assertIn("|| contains(env.RELEASE_TAG, '-') && 'rc' || 'latest' }}", linux)
        # (a tag named rc is never built: the Release workflow moves it)
        for name in ("linux-appimage.yml", "windows-builder.yml"):
            text = (ROOT / ".github/workflows" / name).read_text(encoding="utf-8")
            self.assertEqual(text.count("github.ref_name != 'nightly'"), text.count("github.ref_name != 'rc'"), name)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source", type=Path, default=ROOT)
    args, remaining = parser.parse_known_args()
    ROOT = args.source
    unittest.main(argv=[sys.argv[0]] + remaining)
