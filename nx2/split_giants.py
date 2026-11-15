"""Giant lifted functions cut at their labels into piece functions and a dispatch loop, for the compile (nx2/build.py).

Why: clang's time on one function grows faster than its size (Machine LICM, register allocation, the IR passes on one flow
graph of tens of thousands of blocks): the lift's chunks of ~2 MB functions (code_023*/024*/025*) took ~30% of a first
build's lifted compile time at 4-7x the seconds per MB of ordinary chunks. The lifter's own cut (a64_lift.split_large: an
extra entry where no branch crosses) cannot cut these: each is one big loop nest.

Why it is exact: a lifted function keeps all guest state in the context (c->x[], c->vreg[], NZCV, FPCR/FPSR) and in guest
memory; each guest instruction is one top-level statement whose C locals live inside its own block, and labels are written
only at the top level (third_party/a64-kit/lift/a64_lift.py emit_function). So before any top-level label nothing but *c
and memory is live, and the body can be cut there:
  - a piece `static uintptr_t s_<addr>_<n>(Ctx *c, uintptr_t k)` starts at the label whose id is k (or at its first
    statement), runs its statements as written, and returns what comes next: 0 (the guest returned), a label id (a goto
    into another piece, with its interrupt poll kept), or a tail-call id (a guest branch into another function);
  - f_<addr> keeps its name and ABI: a loop calls the piece that owns k; a tail-call id leaves through TAILCALL(f_y),
    exactly as before (musttail: the guest call depth is unchanged).
Interrupt polls go with their gotos (every cycle through the loop holds one of the function's own back edges); page-mark
groups end at labels (a64_lift.mark_groups), so no mark moves across a cut; a setjmp-wrapped call is one statement.
A function is left whole when any of this is not shown on its text: a top-level declaration (A64_LOCAL_REGS/FLAGS), a
label inside a block, a goto to a label it lacks, a return or computed goto of another shape, string or char literals.

Pieces are cut at labels after ~piece_bytes of C, preferring a cut that splits no short loop (a back edge spanning less
than 4 pieces: one hop per turn there would cost most); a long loop costs one hop per piece per turn, small against the
piece's own work.
"""
import collections
import re

HEAD = re.compile(r"^void (f_[0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{$")
BRACES = re.compile(r"[{}]")
HEAD_TEXT = re.compile(r"^void (f_[0-9a-f]+)\(Ctx \*__restrict A64_NOESC c\) \{\n", re.M)
LABEL = re.compile(r"^L_([0-9a-f]+):;")
GOTO = re.compile(r"\bgoto (L_[0-9a-f]+);")
TAIL = re.compile(r"\bTAILCALL\((\w+)\);")
RETURN_OK = re.compile(r"\breturn\b(?! \d+;)")
INNER_LABEL = re.compile(r"\bL_[0-9a-f]+\s*:(?!:)")        # (a label anywhere but at the start of a top-level line)
ANY_GOTO = re.compile(r"\bgoto\b(?:\s+([A-Za-z_]\w*)\s*;)?")   # (group 1 None: a computed goto or another shape)
TOP_LABEL = re.compile(r"L_[0-9a-f]+")
DECL = re.compile(r"^\s*(?:const\s+|static\s+|volatile\s+|unsigned\s+|signed\s+|register\s+)*"
                  r"(?:u?int(?:8|16|32|64)_t|u?intptr_t|int|unsigned|long|short|char|float|double|_Bool|bool|size_t|"
                  r"void\s*\*|Ctx|GuestContext)\b(?!\s*\))")
PIECE_KB = 8       # (measured: see the stream report; 4-32 KB pieces all take a fraction of the whole function's time)
MIN_KB = 256       # (functions below this compile near-linearly: nothing to gain, a hop to pay)


class Refuse(Exception):
    pass


def _braces(line, depth):
    """the block depth after a line (comments skipped); literals refuse: braces inside them are not counted"""
    if "/" not in line and '"' not in line and "'" not in line:   # (no comment, no literal: the braces alone, found in C)
        for b in BRACES.findall(line):
            depth += 1 if b == "{" else -1
            if depth < 0:
                raise Refuse("unbalanced braces")
        return depth
    i, n = 0, len(line)
    while i < n:
        ch = line[i]
        if ch == "/" and line.startswith("/*", i):
            j = line.find("*/", i + 2)
            if j < 0:
                raise Refuse("a comment over several lines")
            i = j + 2
            continue
        if ch == "/" and line.startswith("//", i):
            break
        if ch == '"' or ch == "'":
            raise Refuse("a string or char literal")
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth < 0:
                raise Refuse("unbalanced braces")
        i += 1
    return depth


def _local_gotos(text):
    """A statement's gotos are to the function's own top-level labels (L_<hex>), or to a label of another name defined
    inside this same statement, which stays whole in one piece (PR #38's fast float regions: `{ if (...) { ... goto
    L_ff_<pc>; ... } <exact copy> L_ff_<pc>:; }`); anything else (a computed goto, a label elsewhere) refuses"""
    for m in ANY_GOTO.finditer(text):
        t = m.group(1)
        if t is None:
            raise Refuse("a goto of another shape")
        if TOP_LABEL.fullmatch(t):
            continue
        if not re.search(r"(?<![\w.])%s\s*:(?!:)" % re.escape(t), text):
            raise Refuse("a goto of another shape")


def items_of(body_lines):
    """The body as top-level items: (label, None) for each label, (None, text) for each statement (one or more lines)"""
    items, cur, depth = [], [], 0
    for ln in body_lines:
        if depth == 0 and not cur:
            m = LABEL.match(ln)
            if m:
                items.append(("L_" + m.group(1), None))
                rest = ln[m.end():]
                if rest.strip():
                    if _braces(rest, 0):
                        raise Refuse("a block opened after a label")
                    if INNER_LABEL.search(rest):
                        raise Refuse("a label inside a block")
                    _local_gotos(rest)
                    items.append((None, rest.lstrip()))
                continue
            if DECL.match(ln):
                raise Refuse("a top-level declaration")
        if "L_" in ln and INNER_LABEL.search(ln):
            raise Refuse("a label inside a block")
        cur.append(ln)
        depth = _braces(ln, depth)
        if depth == 0:
            text = "\n".join(cur)
            if "goto" in text:
                _local_gotos(text)
            items.append((None, text))
            cur = []
    if cur or depth:
        raise Refuse("unbalanced body")
    return items


def plan(items, piece, loop_max):
    """Indexes of the items where a piece starts: 0, then labels about `piece` bytes apart (up to 3x), choosing among
    them the one that cuts the fewest back edges spanning at most loop_max bytes"""
    n = len(items)
    pos, size = {}, [0] * (n + 1)
    for i, (lab, text) in enumerate(items):
        size[i + 1] = size[i] + (len(text) + 1 if text is not None else len(lab) + 4)
        if lab:
            pos[lab] = i
    diff = [0] * (n + 2)
    for i, (lab, text) in enumerate(items):
        if not text or "goto" not in text:
            continue
        for t in GOTO.findall(text):
            if t not in pos:
                raise Refuse("a goto to a label the function does not have")
            j = pos[t]
            if j <= i and size[i + 1] - size[j] <= loop_max:
                diff[j + 1] += 1
                diff[i + 1] -= 1
    cross, run = [0] * (n + 1), 0
    for m in range(n + 1):
        run += diff[m]
        cross[m] = run
    cands = [m for m in range(1, n) if items[m][0]]
    starts, last, k = [0], 0, 0
    while True:
        lo, hi = size[last] + piece, size[last] + 3 * piece
        while k < len(cands) and size[cands[k]] < lo:
            k += 1
        if k >= len(cands) or size[n] - lo < piece // 2:
            break
        best, bk = None, k
        j = k
        while j < len(cands) and size[cands[j]] <= hi:
            if best is None or cross[cands[j]] < best:
                best, bk = cross[cands[j]], j
                if best == 0:
                    break
            j += 1
        starts.append(cands[bk])
        last = cands[bk]
        k = bk + 1
    return starts


def split_function(name, body_lines, piece, loop_max, stats):
    """The text of f_<addr>'s pieces and dispatcher (raises Refuse when the function must stay whole)"""
    items = items_of(body_lines)
    starts = plan(items, piece, loop_max)
    if len(starts) < 2:
        raise Refuse("too small to cut")
    addr = name[2:]
    labels = [lab for lab, _ in items if lab]
    lid = {lab: i + 1 for i, lab in enumerate(labels)}
    tails = {}   # tail-call target -> id (after the label ids)
    bounds = starts + [len(items)]
    owner = {}
    for p in range(len(starts)):
        for lab, _ in items[bounds[p]:bounds[p + 1]]:
            if lab:
                owner[lab] = p
    pieces, ranges = [], []
    for p in range(len(starts)):
        mine = items[bounds[p]:bounds[p + 1]]
        mylabs = [lab for lab, _ in mine if lab]
        out = ["static __attribute__((noinline)) uintptr_t s_%s_%d(Ctx *__restrict A64_NOESC c, uintptr_t k) {" % (addr, p)]
        if mylabs:
            out.append("    switch (k) { %s default: break; }" % " ".join("case %d: goto %s;" % (lid[l], l) for l in mylabs))

        def go(m, p=p):
            t = m.group(1)
            if owner[t] == p:
                return m.group(0)
            stats["cross_gotos"] += 1
            return "return %d;" % lid[t]

        def tail(m):
            stats["tail_calls"] += 1
            return "return %d;" % tails.setdefault(m.group(1), len(labels) + 1 + len(tails))
        for lab, text in mine:
            if lab:
                out.append(lab + ":;")
                continue
            if "goto *" in text or "&&L_" in text or "__label__" in text or "setjmp(" in text and "recomp_sj_setjmp(" not in text:
                raise Refuse("a computed goto or a setjmp of another shape")
            t = GOTO.sub(go, text) if "goto" in text else text
            if "return" in t or "TAILCALL" in t:
                t = TAIL.sub(tail, t).replace("return;", "return 0;")
                if "TAILCALL" in t:
                    raise Refuse("a tail call of another shape")
                if RETURN_OK.search(t):
                    raise Refuse("a return of another shape")
            out.append(t)
        out.append("return %d;" % lid[items[bounds[p + 1]][0]] if p + 1 < len(starts) else "return 0;")
        out.append("}")
        pieces.append("\n".join(out))
        if mylabs:
            ranges.append((lid[mylabs[0]], lid[mylabs[-1]], p))
    d = ["void %s(Ctx *__restrict A64_NOESC c) {" % name,
         "    uintptr_t k = s_%s_0(c, 0);" % addr,
         "    for (;;) {",
         "        switch (k) {",
         "        case 0: return;"]
    for a, b, p in ranges:
        d.append("        case %d%s: k = s_%s_%d(c, k); continue;" % (a, "" if a == b else " ... %d" % b, addr, p))
    for f, i in sorted(tails.items(), key=lambda x: x[1]):
        d.append("        case %d: TAILCALL(%s);" % (i, f))
    d += ["        }", "        return;   /* (never: every id is a case) */", "    }", "}"]
    stats["pieces"] += len(starts)
    stats["functions_split"] += 1
    return "\n".join(pieces) + "\n" + "\n".join(d)


def split_text(src, piece_kb=PIECE_KB, min_kb=MIN_KB, only=None, notes=None):
    """A lifted C file with each function of at least min_kb KB of C (or the ones named in `only`) split: (text, stats).
    notes: a list that gets a line per function left whole and why"""
    stats = collections.Counter()
    piece, min_bytes = piece_kb * 1024, min_kb * 1024
    out, pos = [], 0
    # (each definition's body runs from its head's line to the first "}" at the start of a line: only the giants are read
    # line by line; the rest of the file is copied as it is, 40 s -> a few s over a whole lift)
    for m in HEAD_TEXT.finditer(src):
        start = m.end()
        if start < pos:
            continue
        end = src.find("\n}\n", start - 1)
        if end < 0:
            end = len(src) - 2 if src.endswith("\n}") else -1
        if end < 0:
            break
        name, size = m.group(1), end + 1 - start
        if not ((only is None and size >= min_bytes) or (only is not None and name in only)):
            continue
        try:
            done = split_function(name, src[start:end].split("\n") if end >= start else [], piece, 4 * piece, stats)
        except Refuse as e:
            stats["functions_left_whole"] += 1
            if notes is not None:
                notes.append("%s (%d KB) left whole: %s" % (name, size >> 10, e))
            continue
        out.append(src[pos:m.start()])
        out.append(done)
        stats["bytes_split"] += size
        pos = end + 2
    if not out:
        return src, stats
    out.append(src[pos:])
    return "".join(out), stats


FLAG = "-DHOOKSHOT_SPLIT_GIANTS="


def flag(piece_kb=PIECE_KB):
    """nx2/build.py: the flag on the lifted chunks' compiles: the piece size and this file's hash (a change of either builds the
    chunks again: the flags are in their object keys)"""
    import hashlib
    with open(__file__, "rb") as f:
        return FLAG + "%d_%s" % (piece_kb, hashlib.sha1(f.read(), usedforsecurity=False).hexdigest()[:10])


def compile_copy(src, obj, fl):
    """nx2/build.py compile_one: what to compile for a job whose flags carry FLAG: a copy of src beside the object with its
    giants split (obj's name + .split.c; compile_one removes it), or src itself when it has none"""
    kb = next((int(f[len(FLAG):].split("_")[0]) for f in fl if f.startswith(FLAG)), 0)
    if not kb:
        return src
    with open(src, encoding="utf-8", newline="") as f:
        text = f.read()
    out, st = split_text(text, kb)
    if not st["functions_split"]:
        return src
    copy = obj[:-2] + ".split.c"
    with open(copy, "w", encoding="utf-8", newline="") as f:
        f.write(out)
    return copy
