"""code_hot.c in pieces that compile side by side: 83 MB in one file took 79 minutes on one core, and every full build waited for it.

    python -I nx2/split_hot.py [gen folder] [pieces, default 8]

Functions that call each other stay together, heaviest calls first, so what used to inline still can. The pieces cost about the
same to compile (chunks.fn_cost). code_hot.c stays as it was, renamed code_hot.c.whole, and build.py compiles code_hot_*.c at the
hot flags. A profile from the whole file fits the pieces: its counters go by function name and shape.
"""
import collections
import glob
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit  # noqa: E402,F401  (a64-kit on the path, for chunks)
import chunks  # noqa: E402
import lifted_c  # noqa: E402

HEAD = lifted_c.HEAD
CALL = re.compile(r"\b(f_[0-9a-f]{8})\(c\)")
NAME = re.compile(r"\b(f_[0-9a-f]{8}|hos_\w+)\b")
DECL = re.compile(r"^void (f_[0-9a-f]{8})\(Ctx \*A64_NOESC\);$")
EXTERN = re.compile(r"^extern [^;(]*?\b(hos_\w+)\(")
STICKY_SLACK = 1.15


def fresh_split(bodies, edges, cost, pieces):
    """The pieces made afresh: groups joined along the calls, dealt out by their cost. Returns ([cost, names] per piece, the calls
    that stay in one group)"""
    # (groups joined along the calls: small callees first, those are what the optimiser inlines)
    limit = sum(len(b) for b in bodies.values()) * 1.1 / pieces
    parent = {f: f for f in bodies}
    size = {f: len(b) for f, b in bodies.items()}

    def root(f):
        while parent[f] != f:
            parent[f] = parent[parent[f]]
            f = parent[f]
        return f
    for (f, callee), n in sorted(edges.items(), key=lambda e: (len(bodies[e[0][1]]) / e[1], e[0])):
        a, b = root(f), root(callee)
        if a != b and size[a] + size[b] <= limit:
            parent[b] = a
            size[a] += size[b]

    groups = collections.defaultdict(list)
    for f in bodies:
        groups[root(f)].append(f)
    # (the groups dealt out by their compile cost, not their size: a piece's time goes with the square of its functions' sizes, and by
    # size one piece took 333 s and another 60)
    bins = [[0, []] for _ in range(pieces)]
    for g in sorted(groups.values(), key=lambda g: (-sum(cost[f] for f in g), min(g))):
        b = min(bins, key=lambda x: x[0])
        b[0] += sum(cost[f] for f in g)
        b[1].extend(g)
    kept = sum(n for (f, c), n in edges.items() if root(f) == root(c))
    return bins, kept


def read_pieces(path, pieces):
    """The last lift's {function: piece}, or {} (none, unreadable, another number of pieces)"""
    try:
        with open(path, encoding="utf-8") as f:
            rows = [line.split() for line in f]
    except (OSError, UnicodeDecodeError, TypeError):
        return {}
    out = {r[0]: int(r[1]) for r in rows if len(r) == 2 and r[1].isdigit()}
    return out if out and max(out.values()) < pieces and len(set(out.values())) == pieces else {}


def save_pieces(path, bins):
    if not path:
        return
    with open(path + ".new", "w", encoding="utf-8", newline="\n") as f:
        f.writelines("%s %d\n" % (name, i) for i, (_, names) in enumerate(bins) for name in sorted(names))
    os.replace(path + ".new", path)


def sticky_split(prev, bodies, edges, cost, pieces):
    """The pieces of the last lift (prev) with this lift's functions: those it had stay where they were, a new one goes to the piece
    of the function it shares the most calls with (one placed already), else to the cheapest piece. ([], 0) without prev"""
    if not prev:
        return [], 0
    bins = [[0, []] for _ in range(pieces)]
    where = {}
    for f in bodies:
        if f in prev:
            where[f] = prev[f]
    near = collections.defaultdict(collections.Counter)
    for (f, c), n in edges.items():
        near[f][c] += n
        near[c][f] += n
    new = sorted((f for f in bodies if f not in where), key=lambda f: (-cost[f], f))
    for f in new:
        for g, _ in sorted(near[f].items(), key=lambda e: (-e[1], e[0])):
            if g in where:
                where[f] = where[g]
                break
    for f, i in where.items():
        bins[i][0] += cost[f]
        bins[i][1].append(f)
    for f in new:
        if f not in where:
            b = min(range(pieces), key=lambda i: (bins[i][0], i))
            where[f] = b
            bins[b][0] += cost[f]
            bins[b][1].append(f)
    return bins, len(new)


def split(gen, pieces, keep=None):
    path = os.path.join(gen, "code_hot.c")
    whole = path + ".whole"
    if os.path.exists(path):
        os.replace(path, whole)
    for old in glob.glob(os.path.join(glob.escape(gen), "code_hot_*.c")):
        os.remove(old)
    with open(whole, encoding="utf-8", newline="") as f:
        text = f.read()
    marks = list(HEAD.finditer(text))
    if not marks:
        raise SystemExit("split_hot: no functions in %s" % whole)
    preamble = text[:marks[0].start()]
    bodies = {m.group(1): text[m.start():(marks[i + 1].start() if i + 1 < len(marks) else len(text))] for i, m in enumerate(marks)}

    cost = {f: chunks.fn_cost(len(b)) for f, b in bodies.items()}
    edges = collections.Counter()
    for f, b in bodies.items():
        for callee in CALL.findall(b):
            if callee in bodies and callee != f:
                edges[(f, callee)] += 1
    bins, kept = fresh_split(bodies, edges, cost, pieces)
    # (sticky: the functions of the last lift stay in their pieces, the new ones go to a neighbour's piece. A small change to the game's
    # hot code (a patch, one function) moved functions between 6 of the 8 pieces, so every player compiled all eight again, the
    # longest third of a cold compile; now the pieces that changed. Used while its longest piece costs at most STICKY_SLACK more
    # than a fresh split's)
    sticky, placed = sticky_split(read_pieces(keep, pieces), bodies, edges, cost, pieces)
    if sticky and max(c for c, _ in sticky) <= max(c for c, _ in bins) * STICKY_SLACK:
        print("split_hot: the pieces of the last lift kept (%d functions placed anew)" % placed)
        bins = sticky
    elif sticky:
        print("split_hot: the pieces of the last lift are out of balance now: split afresh")
    save_pieces(keep, bins)
    decls = {}
    for line in preamble.split("\n"):
        m = DECL.match(line) or EXTERN.match(line)
        if m:
            decls[line] = m.group(1)
    costs = {}
    for i, (c, names) in enumerate(bins):
        # (each piece declares only the functions and host calls it names: a new one elsewhere left the other pieces' text, and so
        # their objects, as they were)
        named = set()
        for name in names:
            named.update(NAME.findall(bodies[name]))
        mine = "\n".join(line for line in preamble.split("\n") if line not in decls or decls[line] in named)
        with open(os.path.join(gen, "code_hot_%d.c" % i), "w", encoding="utf-8", newline="") as f:
            f.write(mine)
            for name in sorted(names):
                f.write(bodies[name])
        costs["code_hot_%d.c" % i] = len(mine) + c
        print("split_hot: piece %d: %d functions, %d MB, cost %d MB" % (i, len(names), sum(len(bodies[f]) for f in names) >> 20, c >> 20))
    chunks.write_costs(gen, costs)
    piece = {f: i for i, (_, names) in enumerate(bins) for f in names}
    together = sum(n for (f, c), n in edges.items() if piece[f] == piece[c])
    print("split_hot: %d of %d calls stay in one piece (%d in one group)" % (together, sum(edges.values()), kept))


def pieces_file(gen):
    """Where the pieces of the last lift are kept: <work>/image/hot_pieces.txt, beside the chunk boundaries (chunks.txt), which outlive
    the lifted C (<work>/sa/gen) as they do. None when there is no such folder"""
    image = os.path.normpath(os.path.join(gen, os.pardir, os.pardir, "image"))
    return os.path.join(image, "hot_pieces.txt") if os.path.isdir(image) else None


if __name__ == "__main__":
    here = os.path.dirname(os.path.abspath(__file__))
    gen = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "work", "sa", "gen")
    split(gen, int(sys.argv[2]) if len(sys.argv) > 2 else 8, pieces_file(gen))
