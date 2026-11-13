"""The pipeline state lists (gpu/vk/vk_draw.cpp: hookshot_pipeline_states.bin, written as the game is played): show them, keep the
repository's sources of the list a release ships, and merge those into it.

The repository keeps the list a release ships as separate sources in assets/pipeline_states/ (README.md there):

    tour.bin.xz                 written only by the pipeline tour (scripts/pipeline_tour.py), replaced whole each time
    contrib/<name>.bin.xz       one per contributed list, added to and never edited by anyone else
    <source>.md                 beside each: where it comes from (who, GPU and driver, build, settings, route)

nx2/make_game.py merges them (the tour's first, then the contributed ones by name; each pipeline once) into the game folder's
hookshot_pipeline_states_shipped.bin, whose pipelines the first start's shader preparation builds before the title. The same
sources always merge into the same bytes.

    python -I nx2/pipeline_states.py <list>...                            what they hold
    python -I nx2/pipeline_states.py add <name> <list>... --who ... --gpu ... --build ... --settings ... --route ...
        a contributed list, contrib/<name>.bin.xz: the lists' pipelines that no other contributed list holds (to an existing
        <name>, its pipelines are kept and the new ones added), and its note contrib/<name>.md. The lists are someone else's
        files: each must be exactly a list (read_sent), or nothing is written
    python -I nx2/pipeline_states.py tour <list>... --gpu ... --build ... --settings ... --route ...
        tour.bin.xz and tour.md replaced by the lists' pipelines (the pipeline tour runs it)
    python -I nx2/pipeline_states.py merge --out <file>                    the merged list (xz when <file> ends in .xz)
    python -I nx2/pipeline_states.py check                                 each source whole, of this build's format, not empty,
                                                                          with its note; the merge the same twice (CI runs it)
    python -I nx2/pipeline_states.py --out <file> <list>...               lists merged, each pipeline once (any lists)
    python -I nx2/pipeline_states.py --v2 --out <file> <list>...          the same, version 1 lists converted to version 2
    python -I nx2/pipeline_states.py --check <list>...                    each list exactly a list, as add takes one (a list
                                                                          someone sent: read_sent), and what it holds

add, tour, merge and check take --dir <sources folder> (default: the repository's assets/pipeline_states).

A list may be xz-compressed (read by its magic; written so when its name ends in .xz, with fixed settings, so the same records
always give the same bytes): the repository's are, so that its history stays small. xz, not zstd: the AppImage's Python (3.12)
has lzma but not compression.zstd.

A list holds no game code: each record is a pipeline's stage code keys (hashes of the game's shaders, which the player's own
hookshot_shaders.bin turns back into the shaders), the program variant's hash, and the formats and draw state baked into the pipeline.
A record can also name a compute program the game dispatched, by its compute stage's code key alone (a compute pipeline has no
formats or draw state): the first start keeps those programs' pipelines ready with the others.

Version 2 (HKSTATE2, written by builds since it) holds what the game asked for, the same on every GPU and with any settings: the
targets' NVN formats, whether the draw was a mesh draw, the draw's own topology, and the program variant's texture answers with its
stages numbered without the mesh fallback's generated vertex stage. Each GPU's shader preparation maps them through the rules its
draws follow (the formats it makes of the NVN ones, its mesh shaders or the mesh fallback). Version 1 (HKSTATE1, older builds) held
the recording GPU's choices: the Vulkan formats it rendered in, and on a GPU with the mesh fallback the fallback's topology and
stage numbering; the game still reads it.

    <list>: "HKSTATE2", u32 record size (552), then the records:
            u64 code[4] (vertex or mesh, fragment, task, the header-only stage), u64 variant (of the answers below),
            u32 mesh, topology (a mesh draw's: triangle list), targets, samples, has depth, depth NVN format,
            u32 colour NVN texture formats[8] (0: none), u32 colour NVN view formats[8] (the format each is drawn in),
            u64 blend[8], u32 vertex attributes[16], u32 attribute count, u32 strides[16], u32 divisors[16],
            u32 blend enables, channel mask, polygon mode, depth clamp, alpha to coverage,
            u32 answers, u32 questions, u32 answers[24] (stage, descriptor, image, type), u8 questions[32], u32 kind, u32 pad
            (variant 0 with no answers and no questions: the variant nothing bound makes, each texture as its instruction
            declares it; lists converted from records without texture answers hold those)
            kind 0: a graphics or mesh pipeline, as above (every list from before the kinds: the word was padding, 0).
            kind 1 (COMPUTE): a compute program, code[0] its compute stage's code key and every other field 0. The game keeps
            the pipelines of the shipped list's ready before the title (vk_draw.cpp computes_plan), with the ones its own play
            dispatched (hookshot_computes.bin holds the same keys). A build from before the kinds leaves such a record out (0
            samples is no pipeline's); version 1 has none
    <list>: "HKSTATE1", u32 record size (528), then the records:
            u64 code[4], u64 variant,
            u32 mesh, topology, targets, samples, depth format, has depth, u32 color formats[8] (Vulkan), u8 color types[8],
            u64 blend[8], u32 vertex attributes[16], u32 attribute count, u32 strides[16], u32 divisors[16],
            u32 blend enables, channel mask, polygon mode, depth clamp, alpha to coverage, pad,
            u32 answers, u32 questions, u32 answers[24] (stage, descriptor, image, type), u8 questions[32] (each answer as asked)

Lists of both versions merge into version 2 (version 1 alone stays version 1; --v2 converts it too). A version 1 record is
converted: each Vulkan format back to an NVN format every GPU renders in it (V1_NVN: no format a GPU chooses itself), a mesh record
made with the mesh fallback numbered as the native one. A record that cannot be (a format the table lacks, a fallback record whose
answers were left out) is left out, and counted. When the record's format changes, every source is converted (or the tour's made
again): the layout of the folder stays.
"""
import argparse
import collections
import lzma
import os
import re
import struct
import sys

MAGIC = b"HKSTATE1"
SIZE = 528
MAGIC2 = b"HKSTATE2"
SIZE2 = 552
TRIANGLE_LIST = 3   # (VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST: what a mesh draw is given, vk_draw.cpp kMeshDrawTopology)

# A version 1 record's Vulkan format -> an NVN format vk_format (gpu/vk/vk_formats.cpp) makes it on every GPU: the formats a
# target can be, ASTC and BC (no GPU renders into them) left out. tests/test_pipeline_states.py checks it against vk_formats.cpp,
# gpu/render/nvn_api.h and the Vulkan headers
V1_NVN = {
    4: 0x95, 5: 0x3c, 7: 0x3a, 8: 0x3b, 9: 0x01, 10: 0x02, 13: 0x03, 14: 0x04, 16: 0x0d, 17: 0x0e, 20: 0x0f, 21: 0x10,
    23: 0x19, 24: 0x1a, 27: 0x1b, 28: 0x1c, 37: 0x6d, 38: 0x6e, 41: 0x6f, 42: 0x70, 43: 0x37, 44: 0x99, 50: 0x9b,
    64: 0x3d, 68: 0x3e, 70: 0x06, 71: 0x07, 74: 0x08, 75: 0x09, 76: 0x05, 77: 0x12, 78: 0x13, 81: 0x14, 82: 0x15,
    83: 0x11, 91: 0x72, 92: 0x73, 95: 0x74, 96: 0x75, 97: 0x29, 98: 0x0b, 99: 0x0c, 100: 0x0a, 101: 0x17, 102: 0x18,
    103: 0x16, 106: 0x22, 107: 0x77, 108: 0x78, 109: 0x76, 122: 0x3f, 123: 0x40, 124: 0x32, 125: 0x33, 126: 0x34,
    127: 0x31, 129: 0x35, 130: 0x36, 1000340001: 0x39,
}

# the records as struct layouts (vk_draw.cpp StateRec, StateRec2)
_V1 = struct.Struct("<4QQ6I8I8s8Q16II16I16I6III24I32s4x")
_V2 = struct.Struct("<4QQ6I8I8I8Q16II16I16I5III24I32s2I")
assert _V1.size == SIZE and _V2.size == SIZE2
KIND_AT = SIZE2 - 8   # (a version 2 record's kind: vk_draw.cpp StateRec2::kind)
COMPUTE = 1           # (StateRec2::kCompute)
# The compute programs a shipped list may name before `check` stops, for someone to decide: the game keeps ready every one a
# shipped list names, never fewer (vk_draw.cpp computes_plan), so the bound is here and loud, not there and silent.
# Measured 2026-11-12 (RTX 3090, driver 620.70, Linux; a full prepare; the heap in use from the game's "vk: malloc:" line after
# the same short session): the game has 4,127 compute programs (code keys) in 9,925 texture variants. None kept ready: 7,667 MB.
# The 224 that play on that PC had dispatched (1,059 variants): 7,888 MB, so about 1 MB a listed program, and 4.8 s of
# precompile. All of them: 10,112 MB and 8.1 s. Play there reached 132 to 225 programs over 15 game folders. 1024 is four times
# that and about 1 GB: past it, measure again and raise this, or prune the list
COMPUTE_MOST = 1024
# One list someone sent: more than the game has programs (twice 4,127, rounded up) was not written by the game. The merge's bound
# above is the one that counts what a release costs; this one only keeps a made-up file small
SENT_COMPUTE_MOST = 8192
# A list someone sent, packed or unpacked: 256 MB is some 480,000 pipelines, ten times every source here together
SENT_MOST = 256 << 20


SOURCES = ("assets", "pipeline_states")   # (under the repository's root: the sources of the list a release ships)
TOUR = "tour.bin.xz"
CONTRIB = "contrib"
EXT = ".bin.xz"
XZ_MAGIC = b"\xfd7zXZ\x00"
NAME = re.compile(r"^[a-z0-9]+(?:[-_.][a-z0-9]+)*$")   # (a contributed list's name: the same order on every file system)
# (a source's note: "- <field>: <text>" lines; Pipelines is written by this tool and must match the list)
FIELDS = ("Who", "GPU and driver", "Build", "Settings", "Route")
ARG = {"Who": "who", "GPU and driver": "gpu", "Build": "build", "Settings": "settings", "Route": "route"}
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def load(path, most=None):
    """The list's bytes as the game reads them: unpacked when the file is xz. most: stops, before reading or unpacking more, on
    a file or an unpacked list of more bytes than that (a list someone sent: read_sent)"""
    if most is not None:
        if not os.path.isfile(path):
            raise SystemExit("%s: no such file" % path)
        if os.path.getsize(path) > most:
            raise SystemExit("%s: %d bytes, more than a pipeline state list holds (%d)" % (path, os.path.getsize(path), most))
    with open(path, "rb") as f:
        data = f.read() if most is None else f.read(most + 1)
    if data[:6] == XZ_MAGIC:
        try:
            if most is None:
                data = lzma.decompress(data)
            else:   # (unpacked no further than the limit: a small xz file can unpack to gigabytes)
                un = lzma.LZMADecompressor(format=lzma.FORMAT_XZ)
                data = un.decompress(data, max_length=most + 1)
                if len(data) > most:
                    raise SystemExit("%s: unpacks to more than a pipeline state list holds (%d bytes)" % (path, most))
                if not un.eof or un.unused_data:
                    raise SystemExit("%s: a damaged xz file (cut short, or with more after its end)" % path)
        except lzma.LZMAError as e:
            raise SystemExit("%s: a damaged xz file (%s)" % (path, e))
    return data


def parse(data, path, strict=False):
    """The records of a list's bytes (of either version: a record's length says which). Stops with the reason when they are not
    a list this build reads. A record cut short at the end (a game that ended while it wrote) is left out, as the game leaves it
    out; strict: stops there too"""
    head = (data[:8], struct.unpack_from("<I", data, 8)[0] if len(data) >= 12 else 0)
    if head not in ((MAGIC, SIZE), (MAGIC2, SIZE2)):
        found = data[:8].decode("ascii", "replace") if data[:7] == MAGIC[:7] else "no list"
        raise SystemExit("%s: not a pipeline state list this build reads (%s, %d-byte records, or %s, %d; found %s): make it "
                         "again with this build (docs/development.md, \"The pipeline state list\")"
                         % (path, MAGIC2.decode(), SIZE2, MAGIC.decode(), SIZE, found))
    size = head[1]
    over = (len(data) - 12) % size
    if over:
        if strict:
            raise SystemExit("%s: cut short (%d bytes over the last whole record)" % (path, over))
        print("%s: %d bytes of a record cut short at the end, left out" % (path, over), file=sys.stderr)
    body = data[12:len(data) - over]
    return [body[i:i + size] for i in range(0, len(body), size)]


def read(path, strict=False):
    return parse(load(path), path, strict)


def kind(r):
    """A record's kind: 0 a graphics or mesh pipeline (every version 1 record), COMPUTE a compute program"""
    return struct.unpack_from("<I", r, KIND_AT)[0] if len(r) == SIZE2 else 0


def compute_record(key):
    """The record of a compute program: its compute stage's code key and nothing else (vk_draw.cpp state_record_compute)"""
    r = bytearray(SIZE2)
    struct.pack_into("<Q", r, 0, key)
    struct.pack_into("<I", r, KIND_AT, COMPUTE)
    return bytes(r)


def compute_keys(recs):
    """The compute programs records name, each once, in the order first seen"""
    return list(dict.fromkeys(struct.unpack_from("<Q", r, 0)[0] for r in recs if kind(r) == COMPUTE))


def check_kinds(recs, path):
    """Each record of a kind this build knows, a compute program's exactly as the game writes it (its key, the rest 0: one
    program is then one record, which the merge keeps once). Stops with the reason"""
    for i, r in enumerate(recs):
        k = kind(r)
        if k == COMPUTE:
            key = struct.unpack_from("<Q", r, 0)[0]
            if not key or r != compute_record(key):
                raise SystemExit("%s: record %d names a compute program but holds more than its key (or no key): not one the "
                                 "game wrote" % (path, i))
        elif k:
            raise SystemExit("%s: record %d is of kind %d, which this build does not know: make the list again with this "
                             "build" % (path, i, k))


def read_sent(path):
    """The records of a list someone sent (the "Submit your pipelines" issue's zips, a pull request's list): a file nobody here
    made, so it is taken only when it is exactly a list. No more than SENT_MOST bytes, packed or unpacked; this build's magic
    and record size (either version); whole records and nothing after them; every record of a kind this build knows, a
    compute program's exactly as the game writes it; no more compute programs than the game could have (SENT_COMPUTE_MOST). The bytes
    are only ever compared and counted: nothing in them is run, followed or used as a name. Stops with the reason"""
    recs = parse(load(path, SENT_MOST), path, strict=True)
    if not recs:
        raise SystemExit("%s holds no pipelines" % path)
    check_kinds(recs, path)
    computes = len(compute_keys(recs))
    if computes > SENT_COMPUTE_MOST:
        raise SystemExit("%s: names %d compute programs, more than the game has (at most %d): not a list the game wrote"
                         % (path, computes, SENT_COMPUTE_MOST))
    return recs


def check_shipped(path):
    """A list a release ships: present, whole, of this build's format and not empty; its records. Stops with the reason"""
    if not os.path.isfile(path):
        raise SystemExit("%s is missing, and a release must ship it (docs/development.md, \"The pipeline state list\")" % path)
    recs = read(path, strict=True)
    if not recs:
        raise SystemExit("%s holds no pipelines" % path)
    check_kinds(recs, path)
    return recs


def answers_hash(answers):
    """Program::variant of packed answers (vk_shader.cpp answers_hash)"""
    h = 0
    for a in answers:
        stage, desc, image, typ = a >> 28, a >> 12 & 0xFFFF, a >> 8 & 1, a & 0xFF
        h = ((h ^ (stage << 32 | desc << 16 | typ << 1 | image)) * 1099511628211) & 0xFFFFFFFFFFFFFFFF
    return h


def v1_to_v2(r):
    """A version 1 record as version 2, or None when it cannot be (vk_draw.cpp state_v1_portable does the same in the game)"""
    f = list(_V1.unpack(r))
    code, variant = f[0:4], f[4]
    mesh, topo, n, samples, depth_format, has_depth = f[5:11]
    colors = f[11:19]
    blend, vattr, nvattr = f[20:28], f[28:44], f[44]
    stride, divisor = f[45:61], f[61:77]
    blend_en, chmask, poly, clamp, a2c = f[77:82]
    nanswers, nquestions = f[83], f[84]
    answers, questions = list(f[85:109]), f[109]
    if nanswers > 24 or nquestions > 32 or n > 8:
        return None
    if mesh:   # (made with the mesh fallback: its generated vertex stage after (task,) mesh, and the fallback's topology)
        g = 2 if code[2] else 1
        fallback = topo != TRIANGLE_LIST or any(a >> 28 > g for a in answers[:nanswers])
        if fallback:
            if not nanswers:
                return None
            for i in range(nanswers):
                if answers[i] >> 28 > g:
                    answers[i] = (answers[i] & 0x0FFFFFFF) | ((answers[i] >> 28) - 1) << 28
            variant, topo = answers_hash(answers[:nanswers]), TRIANGLE_LIST
    nvn = []
    for k in range(8):
        if k >= n or not colors[k]:
            nvn.append(0)
        elif colors[k] in V1_NVN:
            nvn.append(V1_NVN[colors[k]])
        else:
            return None
    depth_nvn = 0
    if has_depth and depth_format:
        if depth_format not in V1_NVN:
            return None
        depth_nvn = V1_NVN[depth_format]
    return _V2.pack(*code, variant, mesh, topo, n, samples, has_depth, depth_nvn, *nvn, *nvn, *blend, *vattr, nvattr,
                    *stride, *divisor, blend_en, chmask, poly, clamp, a2c, nanswers, nquestions, *answers, questions, 0, 0)


def as_v2(lists, force=False):
    """Lists of records of one version: as they are when all are version 1 (and not force), else each version 1 record converted
    (those that cannot be left out). (lists, how many were left out)"""
    if not force and all(len(r) == SIZE for recs in lists for r in recs):
        return lists, 0
    out, lost = [], 0
    for recs in lists:
        conv = []
        for r in recs:
            r = v1_to_v2(r) if len(r) == SIZE else r
            if r is None:
                lost += 1
            else:
                conv.append(r)
        out.append(conv)
    return out, lost


def pack(recs):
    """A list's bytes, unpacked: what the game reads. Version 2 when any record is (the version 1 ones converted)"""
    (recs,), lost = as_v2([recs])
    if lost:
        print("%d version 1 pipelines could not be converted to version 2: left out" % lost, file=sys.stderr)
    if recs and len(recs[0]) == SIZE2:
        return MAGIC2 + struct.pack("<I", SIZE2) + b"".join(recs)
    return MAGIC + struct.pack("<I", SIZE) + b"".join(recs)


def compress(data):
    # (fixed settings, and xz stores no time or name: the same records always give the same file. Preset 6: an 8 MB window, as
    # much as a list needs; 9's 64 MB would only cost the reader memory)
    return lzma.compress(data, format=lzma.FORMAT_XZ, check=lzma.CHECK_CRC64, preset=6 | lzma.PRESET_EXTREME)


def write(path, recs):
    data = pack(recs)
    if path.endswith(".xz"):
        data = compress(data)
    with open(path + ".new", "wb") as f:
        f.write(data)
    os.replace(path + ".new", path)


def unique(lists):
    """The records of lists of records, each once, in the order first seen"""
    out, seen = [], set()
    for recs in lists:
        for r in recs:
            if r not in seen:
                seen.add(r)
                out.append(r)
    return out


def describe(recs):
    versions = collections.Counter(1 if len(r) == SIZE else 2 for r in recs)
    programs = collections.Counter()
    kinds = collections.Counter()
    for r in recs:
        code = struct.unpack_from("<4Q", r, 0)
        mesh = struct.unpack_from("<I", r, 40)[0]
        compute = kind(r) == COMPUTE
        programs[(code, mesh, compute)] += 1
        kinds["compute" if compute else "mesh" if mesh else "graphics"] += 1
    return "%d pipelines (%s) of %d programs%s" % (len(recs), ", ".join("%s %d" % kv for kv in sorted(kinds.items())), len(programs),
                                                  "".join(", version %d" % v for v in sorted(versions)))


# ---- the repository's sources ----

def sources_dir(root=None):
    return os.path.join(root or ROOT, *SOURCES)


def note_path(source):
    return source[:-len(EXT)] + ".md"


def sources(d):
    """The sources of the shipped list in merge order: the tour's (when there is one), then the contributed lists by name. Stops
    on a file in contrib/ that is not a list with a name the order can rely on"""
    out = []
    if os.path.isfile(os.path.join(d, TOUR)):
        out.append(os.path.join(d, TOUR))
    c = os.path.join(d, CONTRIB)
    names = sorted(os.listdir(c)) if os.path.isdir(c) else []
    for n in names:
        if n.endswith(".md") and n[:-3] + EXT in names:
            continue
        if not n.endswith(EXT) or not NAME.match(n[:-len(EXT)]):
            raise SystemExit("%s: not a contributed list (contrib/ holds <name>%s and <name>.md, the name in lower-case letters, "
                             "digits, '-', '_' and '.')" % (os.path.join(c, n), EXT))
        out.append(os.path.join(c, n))
    return out


def merged(d, strict=True):
    """The shipped list's records: the sources', each pipeline once, in merge order. strict: stops on a source that does not
    read; else it is left out with a line saying so (the build's way: nx2/make_game.py)"""
    lists = []
    for path in sources(d):
        try:
            lists.append(read(path, strict=True))
        except SystemExit as e:
            if strict:
                raise
            print("left out of the shipped pipeline state list: %s" % e, flush=True)
    lists, lost = as_v2(lists)
    if lost:
        print("%d version 1 pipelines could not be converted to version 2: left out" % lost, flush=True)
    return unique(lists)


def read_note(path):
    fields = {}
    try:
        with open(path, encoding="utf-8") as f:
            for line in f:
                m = re.match(r"^- ([^:]+): (.*\S)\s*$", line)
                if m:
                    fields[m.group(1)] = m.group(2)
    except OSError:
        return None
    return fields


def write_note(path, title, fields, recs):
    lines = ["# %s" % title, "", "Hashes, formats and draw state only: no game code or data (assets/pipeline_states/README.md).", ""]
    lines += ["- %s: %s" % (k, fields[k]) for k in FIELDS]
    lines += ["- Pipelines: %d" % len(recs), ""]
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines))
    os.replace(path + ".new", path)


def check_sources(d):
    """Each source whole, of this build's format, holding pipelines each once, with its note (every field, and the count); the
    merge giving the same bytes twice, and every source's pipelines. The merged records. Stops with the reasons"""
    problems = []
    paths = sources(d)
    if not paths:
        raise SystemExit("%s holds no pipeline state lists, and a release must ship one (docs/development.md, \"The pipeline "
                         "state list\")" % d)
    seen = set()
    v2 = False   # (a version 2 source: the merge converts the version 1 ones)
    for path in paths:
        try:
            v2 |= load(path)[:8] == MAGIC2
        except SystemExit:
            pass   # (said below)
    for path in paths:
        rel = os.path.relpath(path, d)
        try:
            with open(path, "rb") as f:
                if f.read(6) != XZ_MAGIC:
                    problems.append("%s: not xz-compressed (write it with this tool)" % rel)
            recs = check_shipped(path)
        except SystemExit as e:
            problems.append(str(e))
            continue
        if len(set(recs)) != len(recs):
            problems.append("%s: %d pipelines more than once" % (rel, len(recs) - len(set(recs))))
        note = read_note(note_path(path))
        if note is None:
            problems.append("%s: no note beside it (%s): where its pipelines come from" % (rel, os.path.basename(note_path(path))))
        else:
            for k in FIELDS:
                if not note.get(k):
                    problems.append("%s: its note has no \"- %s: ...\" line" % (rel, k))
            if note.get("Pipelines") != str(len(recs)):
                problems.append("%s: its note says %s pipelines, the list holds %d" % (rel, note.get("Pipelines"), len(recs)))
        conv = set(as_v2([recs], force=v2)[0][0])   # (as the merge has them)
        new = len(conv - seen)
        seen |= conv
        print("%s: %s, %d not in the sources before it" % (rel, describe(recs), new))
    if problems:
        raise SystemExit("\n".join(problems))
    a, b = pack(merged(d)), pack(merged(d))
    recs = parse(a, d, strict=True)
    if a != b or set(recs) != seen or len(recs) != len(seen):
        raise SystemExit("%s: the merge is not reproducible, or misses pipelines of its sources" % d)
    computes = len(compute_keys(recs))
    if computes > COMPUTE_MOST:
        raise SystemExit("%s: the merged list names %d compute programs, more than COMPUTE_MOST (%d). The game keeps every one "
                         "of them ready at every start, about 1 MB each: measure what the list costs now and raise COMPUTE_MOST "
                         "(nx2/pipeline_states.py has the last measurement), or prune the list. Nothing is dropped for you"
                         % (d, computes, COMPUTE_MOST))
    print("merged: %s" % describe(recs))
    return recs


def contribute(d, name, lists, fields):
    """contrib/<name>.bin.xz: the lists' pipelines no other contributed list holds (not the tour's, which is replaced whole: a
    pipeline left out for it would be lost when the tour no longer finds it), after the ones it holds already"""
    if not NAME.match(name):
        raise SystemExit("%s: a name of lower-case letters, digits, '-', '_' and '.' (e.g. <github user>-<what was played>)" % name)
    c = os.path.join(d, CONTRIB)
    os.makedirs(c, exist_ok=True)
    path = os.path.join(c, name + EXT)
    old = read(path, strict=True) if os.path.isfile(path) else []
    others = set()
    for p in sources(d):
        if os.path.dirname(p) == c and p != path:
            others.update(read(p, strict=True))
    given = [read_sent(p) for p in lists]   # (lists from other people's PCs)
    (old, given, others), _ = as_v2([old, unique(given), list(others)])   # (one version: version 2 when any is)
    others = set(others)
    recs = unique([old, [r for r in given if r not in others]])
    note = read_note(note_path(path)) or {}
    for k in FIELDS:
        if fields.get(k):
            note[k] = fields[k]
        if not note.get(k):
            raise SystemExit("%s: say where the list comes from (--%s)" % (name, ARG[k]))
    if len(recs) == len(old):
        print("%s: none of the %d pipelines are new" % (name, len(given)))
        return recs
    write(path, recs)
    write_note(note_path(path), name, note, recs)
    print("%s: %d new of %d given; %s" % (os.path.relpath(path, d), len(recs) - len(old), len(given), describe(recs)))
    return recs


def tour(d, lists, fields):
    """tour.bin.xz and its note replaced by the lists' pipelines"""
    recs = unique(read(p) for p in lists)
    if not recs:
        raise SystemExit("the tour's lists hold no pipelines")
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, TOUR)
    write(path, recs)
    write_note(note_path(path), "The pipeline tour", dict(fields, Who="the pipeline tour (scripts/pipeline_tour.py)"), recs)
    print("%s: %s" % (TOUR, describe(recs)))
    return recs



def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    if argv and argv[0] in ("add", "tour", "merge", "check"):
        p = argparse.ArgumentParser(prog="pipeline_states.py " + argv[0])
        p.add_argument("--dir", default=sources_dir())
        if argv[0] == "add":
            p.add_argument("name")
        if argv[0] in ("add", "tour"):
            p.add_argument("lists", nargs="+")
            for k, a in ARG.items():
                if not (argv[0] == "tour" and k == "Who"):
                    p.add_argument("--" + a, default="", help=k)
        if argv[0] == "merge":
            p.add_argument("--out", required=True)
        a = p.parse_args(argv[1:])
        fields = {k: getattr(a, v, "").strip() for k, v in ARG.items()}
        if argv[0] == "add":
            contribute(a.dir, a.name, a.lists, fields)
        elif argv[0] == "tour":
            for k in FIELDS[1:]:
                if not fields[k]:
                    raise SystemExit("tour: say what it was made with (--%s)" % ARG[k])
            tour(a.dir, a.lists, fields)
        elif argv[0] == "merge":
            recs = merged(a.dir)
            write(a.out, recs)
            print("%s: %s" % (a.out, describe(recs)))
        else:
            check_sources(a.dir)
        return 0
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("lists", nargs="+")
    p.add_argument("--out")
    p.add_argument("--check", action="store_true", help="each list must be whole and hold pipelines")
    p.add_argument("--v2", action="store_true", help="version 2 out even from version 1 lists alone (converted)")
    a = p.parse_args(argv)
    lists = []
    for path in a.lists:
        recs = read_sent(path) if a.check else read(path)
        print("%s: %s" % (path, describe(recs)))
        lists.append(recs)
    if a.out:
        lists, lost = as_v2(lists, force=a.v2)
        if lost:
            print("%d version 1 pipelines not converted (a format no table entry has, or a mesh fallback record without its "
                  "answers): left out" % lost)
        recs = unique(lists)
        write(a.out, recs)
        print("%s: %s" % (a.out, describe(recs)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
