"""The third_party rule (CONTRIBUTING.md): third_party/'s parts are unmodified upstream copies, with their notices in each
part's PROVENANCE.md. A change goes in a new folder with its own provenance note, not into a part.

    python -I scripts/check_third_party.py [--root <checkout>]
        every part's files against the tree-sha256 line its PROVENANCE.md ends with; a part with no PROVENANCE.md, or a note
        with no tree-sha256 line, fails too. A part that differs is named, with the files changed since its note was
    python -I scripts/check_third_party.py --base <ref>
        also: a pull request that changes files of a part without changing that part's PROVENANCE.md fails (against the merge
        base of <ref> and HEAD)
    python -I scripts/check_third_party.py --update [<part> ...]
        writes the tree-sha256 lines again (all parts, or the ones named), for a deliberate refresh from upstream: say what
        changed in the note in the same commit
    python -I scripts/check_third_party.py --upstream [<part> ...]
        downloads each part's upstream archive, where its note names one (upstream: <url>, upstream-sha256: <hex>,
        upstream-root: <folder in the archive>; local: <file> for a file of ours), checks the archive's SHA-256 and compares
        the kept files with the archive's byte for byte. A part with no archive line is skipped with a notice

The tree hash: SHA-256 over the part's files but its own PROVENANCE.md, in the order of their paths (relative, with /), each
as its path, whether it is executable, its size and its bytes. Files the repository ignores (build output such as
a64emit.dll, __pycache__) are not part of it. Standard library only; no game data is involved.
"""
import argparse
import hashlib
import io
import os
import re
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

NOTE = "PROVENANCE.md"
RULE = ("Don't modify third_party/: its parts are unmodified upstream copies with their notices in PROVENANCE.md. Add a new "
        "folder and a provenance note instead (CONTRIBUTING.md). For a deliberate refresh from upstream, change the part's "
        "PROVENANCE.md to say so and run python -I scripts/check_third_party.py --update <part>.")
TREE = re.compile(r"^tree-sha256: ([0-9a-f]{64})[ \t]*$", re.M)
FIELD = re.compile(r"^(upstream|upstream-sha256|upstream-root|local): *(\S.*?)[ \t]*$", re.M)


def git(root, *args):
    try:
        out = subprocess.run(["git", *args], cwd=root, capture_output=True, text=True, encoding="utf-8")
    except OSError:
        return None
    return out.stdout if out.returncode == 0 else None


def parts(root):
    tp = os.path.join(root, "third_party")
    return sorted(n for n in os.listdir(tp) if os.path.isdir(os.path.join(tp, n)) and not n.startswith("."))


def ignored(root, paths):
    """The paths (relative to root, with /) .gitignore leaves out: by its rules alone, tracked or not"""
    if not paths or git(root, "rev-parse", "--show-toplevel") is None:
        return set()
    # (-z and bytes, both ways: in text mode Windows sent each path with a \r, which matches no rule; and without -z git
    # quotes a path with a non-ASCII byte in its answer, "third_party/x/\303\251.dll", which matches none of ours. Either
    # way an ignored file was hashed with the part)
    out = subprocess.run(["git", "check-ignore", "--no-index", "-z", "--stdin"], cwd=root,
                         input="".join(p + "\0" for p in paths).encode("utf-8"), capture_output=True)
    return set(out.stdout.decode("utf-8").split("\0")) - {""}


def files(root, part):
    """[(path in the part, absolute path)] of its files, PROVENANCE.md at its top left out"""
    top = os.path.join(root, "third_party", part)
    found = []
    for d, dirs, names in os.walk(top):
        dirs[:] = sorted(x for x in dirs if x != "__pycache__")
        for n in names:
            full = os.path.join(d, n)
            rel = os.path.relpath(full, top).replace(os.sep, "/")
            if rel != NOTE and not n.endswith(".pyc"):
                found.append((rel, full))
    skip = ignored(root, ["third_party/%s/%s" % (part, rel) for rel, _ in found])
    return sorted(((rel, full) for rel, full in found if "third_party/%s/%s" % (part, rel) not in skip), key=lambda f: f[0])


def tree_hash(root, part):
    h = hashlib.sha256()
    for rel, full in files(root, part):
        with open(full, "rb") as f:
            data = f.read()
        executable = os.access(full, os.X_OK) and os.name != "nt"
        h.update(b"%s\0%s\0%d\0" % (rel.encode("utf-8"), b"x" if executable else b"-", len(data)))
        h.update(data)
    return h.hexdigest()


def note_path(root, part):
    return os.path.join(root, "third_party", part, NOTE)


def read_note(root, part):
    try:
        with open(note_path(root, part), encoding="utf-8") as f:
            return f.read()
    except OSError:
        return None


def recorded(text):
    m = TREE.findall(text or "")
    return m[-1] if m else None


def changed_files(root, part):
    """The part's files changed since the commit that last changed its note (and not committed yet), when git can say"""
    last = git(root, "log", "-1", "--format=%H", "--", "third_party/%s/%s" % (part, NOTE))
    if not last or not last.strip():
        return []
    out = git(root, "diff", "--name-only", last.strip(), "--", "third_party/%s" % part) or ""
    new = git(root, "ls-files", "--others", "--exclude-standard", "--", "third_party/%s" % part) or ""
    return sorted(set(out.split()) | set(new.split()))


def check(root):
    problems = []
    for part in parts(root):
        text = read_note(root, part)
        if text is None:
            problems.append("third_party/%s has no %s: every part says where it comes from. %s" % (part, NOTE, RULE))
            continue
        want = recorded(text)
        if want is None:
            problems.append("third_party/%s/%s has no tree-sha256 line (python -I scripts/check_third_party.py --update %s)"
                            % (part, NOTE, part))
            continue
        have = tree_hash(root, part)
        if have != want:
            names = changed_files(root, part)
            problems.append("third_party/%s differs from its note (tree-sha256 %s, recorded %s)%s. %s"
                            % (part, have, want, ": " + ", ".join(names) if names else "", RULE))
    return problems


def check_base(root, base):
    mb = git(root, "merge-base", base, "HEAD")
    if not mb:
        return ["no merge base of %s and HEAD (fetch it first)" % base]
    changed = set((git(root, "diff", "--name-only", mb.strip(), "--", "third_party") or "").split())
    by_part = {}
    for path in changed:
        bits = path.split("/")
        if len(bits) >= 3:
            by_part.setdefault(bits[1], set()).add("/".join(bits[2:]))
    return ["third_party/%s changed (%s) and its %s did not. %s" % (part, ", ".join(sorted(names)), NOTE, RULE)
            for part, names in sorted(by_part.items()) if NOTE not in names]


def update(root, which):
    for part in which or parts(root):
        text = read_note(root, part)
        if text is None:
            raise SystemExit("third_party/%s has no %s: write one first (where it comes from, its version and licence)"
                             % (part, NOTE))
        line = "tree-sha256: %s" % tree_hash(root, part)
        body = TREE.sub("", text).rstrip("\n")
        with open(note_path(root, part), "w", encoding="utf-8", newline="\n") as f:
            f.write(body + "\n\n" + line + "\n")
        print("third_party/%s: %s" % (part, line))


def archive_files(data):
    """{path: bytes} of a .tar(.gz/.xz/.bz2) or .zip archive's files"""
    if data[:4] == b"PK\x03\x04":
        with zipfile.ZipFile(io.BytesIO(data)) as z:
            return {i.filename: z.read(i) for i in z.infolist() if not i.is_dir()}
    out = {}
    with tarfile.open(fileobj=io.BytesIO(data)) as t:
        for m in t.getmembers():
            if m.isfile():
                out[m.name] = t.extractfile(m).read()
    return out


def upstream(root, which, fetch=None):
    fetch = fetch or (lambda url: urllib.request.urlopen(urllib.request.Request(url, headers={"User-Agent": "hookshot-check"}),
                                                         timeout=120).read())
    problems = []
    for part in which or parts(root):
        fields = {}
        for k, v in FIELD.findall(read_note(root, part) or ""):
            fields.setdefault(k, []).append(v)
        if "upstream" not in fields or "upstream-sha256" not in fields:
            print("third_party/%s: no upstream archive in its note, skipped" % part)
            continue
        url, want = fields["upstream"][0], fields["upstream-sha256"][0].lower()
        prefix = fields.get("upstream-root", [""])[0].strip("/")
        data = fetch(url)
        if hashlib.sha256(data).hexdigest() != want:
            problems.append("third_party/%s: %s is not the archive its note names (sha256 %s, want %s)"
                            % (part, url, hashlib.sha256(data).hexdigest(), want))
            continue
        archive = archive_files(data)
        ours = set(fields.get("local", []))
        differ = []
        for rel, full in files(root, part):
            if rel in ours:
                continue
            with open(full, "rb") as f:
                mine = f.read()
            theirs = archive.get((prefix + "/" if prefix else "") + rel)
            if theirs is None:
                differ.append(rel + " (not in the archive)")
            elif theirs != mine:
                differ.append(rel)
        if differ:
            problems.append("third_party/%s differs from upstream %s: %s. %s" % (part, url, ", ".join(differ), RULE))
        else:
            print("third_party/%s: the same as %s" % (part, url))
    return problems


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("--root", default=os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    p.add_argument("--base")
    p.add_argument("--update", nargs="*")
    p.add_argument("--upstream", nargs="*")
    a = p.parse_args(argv)
    root = os.path.abspath(a.root)
    if a.update is not None:
        update(root, a.update)
        return 0
    problems = upstream(root, a.upstream) if a.upstream is not None else check(root)
    if a.base:
        problems += check_base(root, a.base)
    for line in problems:
        print("error: " + line, file=sys.stderr)
    if not problems:
        print("third_party: %d parts checked" % len(a.upstream or parts(root)) if a.upstream is not None
              else "third_party: %d parts as their notes say" % len(parts(root)))
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
