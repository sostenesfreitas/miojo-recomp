"""What the release workflows need from earlier releases, and the Hookshot Builder package folder made in CI
(.github/workflows/windows-builder.yml, linux-appimage.yml). No game data is involved: the parts hold the builder, its tools
and the play profile (addresses and counts made on the development PC).

    python -I scripts/release_parts.py previous <out list> --repo <owner/name> [--repo ...] [--skip-tag <tag>]
        the newest stable release's hookshot-setup.txt (of the first repository that has one; not --skip-tag's, the release
        being made); prints that release's tag. Exits 1 when no repository has one.
    python -I scripts/release_parts.py profile <setup list> <work folder>
        the play profile (tools/work of the list's app part) into <work folder>
    python -I scripts/release_parts.py llvm-mingw <folder> [--name]
        llvm-mingw as scripts/make_setup_parts.py names it (its official release, SHA-256 checked) unpacked into <folder>;
        prints its folder. --name: only prints its name (a cache key)
    python -I scripts/release_parts.py package <out folder> --publish <dotnet publish folder> --setup <setup exe>
                                       [--list <setup list>] [--python-packages <requirements>] [--cpm-add <CPM folder>]
        the package folder make_setup_parts.py splits: the published builder, the setup, LICENSE, the tracked gpu, host, nx2
        and scripts under tools/, the pipeline state list's sources (assets/pipeline_states/, which must be there and read),
        tools/work (the profile) and tools/third_party: the tracked files and, from the list's
        third_party part, the prebuilt files the repository does not carry (a64emit.dll, lld-link.exe, SDL2.dll, SDL3.dll,
        zbic.dll; rebuilt on the player's PC when their sources' stamps differ). Python, emsdk, CPM and wgpu-native are not
        in it: make_setup_parts.py --reuse takes their parts from the list. --python-packages: when the list's Python part
        lacks one of those pinned packages, the part goes into tools/python with them installed (by this Python's pip, for
        the part's version), and make_setup_parts.py makes a new Python part of it. --cpm-add: when the list's CPM part
        lacks one of the folder's <package>/<version> (dav1d, scripts/windows/dav1d.sh), the part goes into tools/cpm with
        them added, and make_setup_parts.py makes a new CPM part of it

GitHub's API is read with $GH_TOKEN when it is set. Downloads are retried (GitHub answers 5xx now and then). Progress goes to
stderr: what a command prints on stdout is its answer.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CACHE = os.path.join(os.environ.get("RUNNER_TEMP") or tempfile.gettempdir(), "release-parts")
# (the prebuilt files of the third_party part: what it holds and git does not)
PREBUILT_EXT = (".dll", ".lib", ".exe", ".sha1")


def get(url, api=False, tries=5):
    if not url.startswith("https://"):   # (only https: the token is never sent over plain http, nor a file: link read)
        raise ValueError("not an https link: " + url)
    headers = {"User-Agent": "hookshot-ci"}
    token = None
    if api:
        headers["Accept"] = "application/vnd.github+json"
        token = os.environ.get("GH_TOKEN")
    for i in range(tries):
        try:
            req = urllib.request.Request(url, headers=headers)
            if token:   # (not sent again on a redirect, which may lead to another host: urllib copies plain headers)
                req.add_unredirected_header("Authorization", "Bearer " + token)
            with urllib.request.urlopen(req, timeout=120) as r:
                return r.read()
        except urllib.error.HTTPError as e:
            if e.code < 500 or i == tries - 1:
                raise
        except (urllib.error.URLError, TimeoutError):
            if i == tries - 1:
                raise
        print("  %s: retrying in 10 s" % url, file=sys.stderr, flush=True)
        time.sleep(10)


def previous(a):
    for repo in a.repo:
        try:
            releases = json.loads(get("https://api.github.com/repos/%s/releases?per_page=30" % repo, api=True))
        except urllib.error.HTTPError as e:
            print("%s: %s" % (repo, e), file=sys.stderr)
            continue
        for r in releases:
            if r["draft"] or r["prerelease"] or r["tag_name"] == a.skip_tag:
                continue
            url = next((x["browser_download_url"] for x in r["assets"] if x["name"] == "hookshot-setup.txt"), None)
            if url:
                with open(a.out, "wb") as f:
                    f.write(get(url))
                print(r["tag_name"])
                return 0
    print("no release with a hookshot-setup.txt in %s" % ", ".join(a.repo), file=sys.stderr)
    return 1


def part_lines(path):
    with open(path, encoding="utf-8") as f:
        return [l.split() for l in f.read().splitlines() if l.startswith("part ")]


def fetch_part(sha, size, url):
    """A part downloaded once into the cache, its size and SHA-256 checked"""
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, url.rsplit("/", 1)[-1])
    if not (os.path.isfile(path) and os.path.getsize(path) == size and sha256(path) == sha):
        print("downloading %s (%d MB)" % (url, size >> 20), file=sys.stderr, flush=True)
        data = get(url)
        if len(data) != size or hashlib.sha256(data).hexdigest() != sha:
            sys.exit("%s: not the size and SHA-256 the list gives" % url)
        with open(path + ".new", "wb") as f:
            f.write(data)
        os.replace(path + ".new", path)
    return path


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while b := f.read(1 << 20):
            h.update(b)
    return h.hexdigest()


def named_part(list_path, name):
    for w in part_lines(list_path):
        if w[6].rsplit("/", 1)[-1].startswith("HookshotBuilder-%s-" % name):
            return fetch_part(w[1], int(w[2]), w[6])
    return None


def extract(zip_path, prefix, dest, keep=lambda rel: True):
    """The zip's members under prefix (HookshotBuilder/...), into dest without that prefix; how many"""
    n = 0
    with zipfile.ZipFile(zip_path) as z:
        for m in z.infolist():
            if m.is_dir() or not m.filename.startswith(prefix):
                continue
            rel = m.filename[len(prefix):]
            if ".." in rel.split("/") or not keep(rel):
                continue
            out = os.path.join(dest, *rel.split("/"))
            # (a name with a drive or a root, such as "C:/x" on Windows, would leave dest: such members are skipped)
            root = os.path.realpath(dest)
            if os.path.commonpath([os.path.realpath(out), root]) != root:
                continue
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(m) as src, open(out, "wb") as f:
                shutil.copyfileobj(src, f)
            n += 1
    return n


def profile(a):
    app = named_part(a.list, "app")
    if not app:
        sys.exit("no app part in %s" % a.list)
    n = extract(app, "HookshotBuilder/tools/work/", a.work)
    print("%d profile files into %s" % (n, a.work))
    return 0


def llvm_spec():
    spec = importlib.util.spec_from_file_location("make_setup_parts", os.path.join(ROOT, "scripts", "make_setup_parts.py"))
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m.LLVM   # (sha256, size, top folder in the zip, url)


def llvm_mingw(a):
    sha, size, name, url = llvm_spec()
    if a.name:
        print(name)
        return 0
    folder = os.path.join(a.folder, name)
    if not os.path.isfile(os.path.join(folder, "bin", "clang.exe")):
        z = fetch_part(sha, size, url)
        with zipfile.ZipFile(z) as zf:
            zf.extractall(a.folder)
    print(folder)
    return 0


def tracked(*paths):
    r = subprocess.run(["git", "ls-files", "-z", "--"] + list(paths), cwd=ROOT, check=True, stdout=subprocess.PIPE)
    return [f for f in r.stdout.decode("utf-8").split("\0") if f]


def copy(src, dst):
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copyfile(src, dst)


def package(a):
    out = os.path.abspath(a.out)
    if os.path.exists(out):
        shutil.rmtree(out)
    shutil.copytree(a.publish, out)
    copy(a.setup, os.path.join(out, "HookshotBuilder-Setup.exe"))   # (the builder's Repair runs it)
    copy(os.path.join(ROOT, "LICENSE"), os.path.join(out, "LICENSE"))
    tools = os.path.join(out, "tools")
    for f in tracked("gpu", "host", "nx2", "scripts", "third_party"):
        copy(os.path.join(ROOT, f), os.path.join(tools, *f.split("/")))
    if a.list:
        if not profile(argparse.Namespace(list=a.list, work=os.path.join(tools, "work"))) == 0:
            return 1
        tp = named_part(a.list, "third_party")
        if tp:
            known = set(tracked("third_party"))
            prebuilt = lambda rel: rel.startswith("third_party/") and rel not in known and rel.lower().endswith(PREBUILT_EXT)
            n = extract(tp, "HookshotBuilder/tools/", tools, keep=prebuilt)
            print("%d prebuilt third_party files from %s" % (n, os.path.basename(tp)))
        if a.python_packages and python_packages(a.list, a.python_packages, tools) != 0:
            return 1
        if a.cpm_add and cpm_add(a.list, a.cpm_add, tools) != 0:
            return 1
    else:
        print("no setup list: the package has no play profile and no prebuilt third_party files")
    if state_list(tools) != 0:
        return 1
    print("package folder: %s" % out)
    return 0


def state_list(tools):
    """The sources of the pipeline state list a release ships (assets/pipeline_states/: the tour's list, the contributed ones and
    their notes), into tools/ at the same path: nx2/make_game.py merges them beside the game. Required: without them a new
    install's first field load is drawn white for seconds while its pipelines compile (v2.0.8 to v2.0.14 shipped none). 1, with
    the reason, when there are none or one does not read (nx2/pipeline_states.py check)"""
    spec = importlib.util.spec_from_file_location("pipeline_states", os.path.join(ROOT, "nx2", "pipeline_states.py"))
    states = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(states)
    src = states.sources_dir(ROOT)
    try:
        recs = states.check_sources(src)
    except SystemExit as e:
        print("error: %s" % e, file=sys.stderr)
        return 1
    dst = states.sources_dir(tools)
    shutil.rmtree(dst, ignore_errors=True)
    shutil.copytree(src, dst)
    print("the pipeline state list: %s" % states.describe(recs))
    return 0


def python_packages(list_path, requirements, tools):
    """The list's Python part with requirements' pins (name==version) installed into tools/python, if it lacks one: by this
    Python's pip, for the part's version and win_amd64 (the part has no pip)"""
    with open(requirements, encoding="utf-8") as f:
        pins = [p for p in (l.split("#")[0].strip() for l in f.read().splitlines()) if p]
    part = named_part(list_path, "python")
    if not part:
        sys.exit("no python part in %s" % list_path)
    with zipfile.ZipFile(part) as z:
        names = [m.split("/") for m in z.namelist()]
    have = {m[-2].lower() for m in names if len(m) > 1 and m[-2].endswith(".dist-info")}
    missing = [p for p in pins if "%s-%s.dist-info" % tuple(x.strip().lower().replace("-", "_") for x in p.split("==")) not in have]
    if not missing:
        print("the Python part has %s" % ", ".join(pins))
        return 0
    dll = [m[-1] for m in names if m[:-1] == ["HookshotBuilder", "tools", "python"] and m[-1].lower().startswith("python3")
           and m[-1][7:-4].isdigit()]
    if len(dll) != 1:
        sys.exit("%s: no pythonXY.dll to give the version" % os.path.basename(part))
    version = "3." + dll[0][7:-4]
    py = os.path.join(tools, "python")
    n = extract(part, "HookshotBuilder/tools/python/", py)
    print("%d files of %s (Python %s) into %s, with %s" % (n, os.path.basename(part), version, py, ", ".join(missing)))
    subprocess.run([sys.executable, "-m", "pip", "install", "--no-deps", "--only-binary", ":all:", "--no-compile", "--no-cache-dir",
                    "--disable-pip-version-check", "-q", "--platform", "win_amd64", "--python-version", version,
                    "--implementation", "cp", "--target", os.path.join(py, "Lib", "site-packages")] + missing, check=True)
    return 0


def cpm_add(list_path, add, tools):
    """The list's CPM part with the packages of the CPM folder `add` (<name>/<version>/...: dav1d for the AV1 movies,
    scripts/windows/dav1d.sh) into tools/cpm, if it lacks one of them: make_setup_parts.py then makes a new CPM part of it"""
    want = sorted((n, v) for n in os.listdir(add) if os.path.isdir(os.path.join(add, n))
                  for v in os.listdir(os.path.join(add, n)) if os.path.isdir(os.path.join(add, n, v)))
    if not want:
        sys.exit("%s: no <package>/<version> folders" % add)
    part = named_part(list_path, "cpm")
    if not part:
        sys.exit("no cpm part in %s" % list_path)
    with zipfile.ZipFile(part) as z:
        have = {tuple(m.split("/")[3:5]) for m in z.namelist() if m.startswith("HookshotBuilder/tools/cpm/")}
    missing = [nv for nv in want if nv not in have]
    if not missing:
        print("the CPM part has %s" % ", ".join("/".join(nv) for nv in want))
        return 0
    cpm = os.path.join(tools, "cpm")
    n = extract(part, "HookshotBuilder/tools/cpm/", cpm)
    for name, version in missing:
        shutil.copytree(os.path.join(add, name, version), os.path.join(cpm, name, version))
    print("%d files of %s into %s, with %s" % (n, os.path.basename(part), cpm, ", ".join("/".join(nv) for nv in missing)))
    return 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("previous")
    s.add_argument("out")
    s.add_argument("--repo", action="append", required=True)
    s.add_argument("--skip-tag", default="")
    s = sub.add_parser("profile")
    s.add_argument("list")
    s.add_argument("work")
    s = sub.add_parser("llvm-mingw")
    s.add_argument("folder", nargs="?", default="")
    s.add_argument("--name", action="store_true")
    s = sub.add_parser("package")
    s.add_argument("out")
    s.add_argument("--publish", required=True)
    s.add_argument("--setup", required=True)
    s.add_argument("--list")
    s.add_argument("--python-packages")
    s.add_argument("--cpm-add")
    a = p.parse_args()
    return {"previous": previous, "profile": profile, "llvm-mingw": llvm_mingw, "package": package}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
