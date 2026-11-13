"""The parts a Linux update downloads (builder/linux/linux_update.py), from an AppDir.

    python3 scripts/linux/make_parts.py <AppDir> <tag> <out folder> [--base <url>]

Each directory that changes on its own is one zip: the tools (every release), Python, clang, FFmpeg, the sysroot, and
the two libraries beside them. A zip is deterministic, so a directory whose files did not change has the same SHA-256
next time and the builder copies the one it already has. The zips are uploaded to the linux-parts release; the version's
own release carries hookshot-linux.txt, which names them.

The list is not inside a zip (it names the zips' checksums).
"""
import argparse
import concurrent.futures
import hashlib
import os
import sys
import zipfile

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                               "builder", "linux"))
from part_links import SYSROOT_PART, is_absolute_link, relative_sysroot_link  # noqa: E402

# (dest under the AppDir, and None for the whole directory or the file names when the directory holds other parts too)
PARTS = (
    ("usr/share/hookshot", None),
    ("usr/python", None),
    ("usr/lib/hookshot/emsdk", None),
    ("usr/lib/hookshot/ffmpeg", None),
    ("usr/lib/hookshot/sysroot", None),
    ("usr/lib/hookshot", ("hookshot_vk.so", "libSDL2-2.0.so.0")),
)


def _entries(root, dest, files):
    base = os.path.join(root, dest)
    if files is not None:
        return [(name, os.path.join(base, name)) for name in files]
    out = []
    for dirpath, dirnames, names in os.walk(base, followlinks=False):
        rel = os.path.relpath(dirpath, base)
        real = []
        for name in sorted(dirnames):
            link = os.path.join(dirpath, name)
            arc = name if rel == os.curdir else rel.replace(os.sep, "/") + "/" + name
            if os.path.islink(link):
                out.append((arc, link))
            else:
                real.append(name)
        dirnames[:] = real
        for name in sorted(names):
            full = os.path.join(dirpath, name)
            arc = name if rel == os.curdir else rel.replace(os.sep, "/") + "/" + name
            out.append((arc, full))
    return out


def write_zip(path, root, dest, files):
    entries = _entries(root, dest, files)
    with zipfile.ZipFile(path + ".new", "w", zipfile.ZIP_DEFLATED, compresslevel=6) as zipped:
        for arc, full in entries:
            info = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_DEFLATED
            info.create_system = 3
            if os.path.islink(full):
                info.external_attr = 0o120777 << 16
                link_to = os.readlink(full)
                if dest == SYSROOT_PART and is_absolute_link(link_to):
                    link_to = relative_sysroot_link(os.path.join(root, dest), full, link_to)
                zipped.writestr(info, link_to, compresslevel=6)
                continue
            info.external_attr = (0o755 if os.access(full, os.X_OK) else 0o644) << 16
            with open(full, "rb") as handle:
                zipped.writestr(info, handle.read(), compresslevel=6)
    os.replace(path + ".new", path)
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        while True:
            block = handle.read(1 << 20)
            if not block:
                break
            digest.update(block)
    return digest.hexdigest(), os.path.getsize(path)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("appdir")
    parser.add_argument("tag")
    parser.add_argument("out")
    parser.add_argument("--base", default="https://github.com/NotRexed/Hookshot/releases/download/linux-parts")
    args = parser.parse_args()
    root = os.path.abspath(args.appdir)
    if not os.path.isfile(os.path.join(root, "AppRun")):
        sys.exit("not an AppDir: %s" % root)
    os.makedirs(args.out, exist_ok=True)
    base = args.base.rstrip("/")
    lines = ["tag %s" % args.tag]
    for dest, files in PARTS:
        if not os.path.isdir(os.path.join(root, dest)):
            sys.exit("the AppDir has no %s" % dest)

    def zip_part(part):
        dest, files = part
        path = os.path.join(args.out, "HookshotLinux-%s.zip" % dest.replace("/", "-"))
        return write_zip(path, root, dest, files)
    # (the parts zipped at once, one thread each: zlib lets go of the GIL while it compresses, and each zip is the same bytes
    # as when they are made one after the other. The list keeps PARTS' order)
    with concurrent.futures.ThreadPoolExecutor(min(len(PARTS), os.cpu_count() or 1)) as pool:
        zipped = list(pool.map(zip_part, PARTS))
    for (dest, files), (digest, size) in zip(PARTS, zipped):
        leaf = dest.replace("/", "-")
        path = os.path.join(args.out, "HookshotLinux-%s.zip" % leaf)
        named = os.path.join(args.out, "HookshotLinux-%s-%s.zip" % (leaf, digest[:12]))
        os.replace(path, named)
        lines.append("part %s %d %s %s/%s" % (digest, size, dest, base, os.path.basename(named)))
        lines.append("tree" if files is None else "\n".join("file %s" % name for name in files))
        print("%-32s %5d MB  %s" % (dest, size >> 20, digest[:16]), flush=True)
    text = "\n".join(lines) + "\n"
    with open(os.path.join(args.out, "hookshot-linux.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)
    # (beside AppRun, not inside a hashed part: the list names those checksums)
    with open(os.path.join(root, "hookshot-linux.txt"), "w", encoding="utf-8", newline="\n") as handle:
        handle.write(text)


if __name__ == "__main__":
    main()
