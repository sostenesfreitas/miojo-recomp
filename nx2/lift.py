"""The lift driver: image (we are rtld) -> function discovery -> C, on the vendored a64-kit plus the Switch 2
additions (nso2: DT_RELR, insn: ARMv8.1-8.5 forms, lift_fixes: jump tables, suyu_rt_fixes: FPX1's binary32 FMA).

Usage:  python -I nx2/lift.py [--exefs <dir>] [--work <dir>] [--write] [--max-func N] [--layout eden|a64] [--aslr 0]
  --layout eden (default): Eglantine's addresses, rtld included, from 0x80000000 + --aslr (image.py; an emulator
           run must use the same offset: rng_seed_enabled=true, rng_seed=<aslr >> 12>);
  --layout a64: a64-kit's own layout from 0x08000000 (its native runtime, below 4 GB)
  Always: builds <work>/image (image.bin, image.json), discovers every function of rtld-less main + subsdk* + sdk,
          and prints coverage: functions, instructions, jump tables, indirect branches, and the functions whose every
          instruction translates (coverage.txt has the untranslated words by site count).
  --write: also writes the lifted C to <work>/gen (about a gigabyte; game code: stays local, never committed).
  --standalone: the standalone build's lift (<work>/sa/image, <work>/sa/gen): the SDK's OS-facing functions hooked
           to the native runtime in host/ or to generated stubs, plus hos_image.c (nx2/standalone.py). It shares the
           chunk boundaries of <work>/image, so its unhooked chunks are the emulator build's, byte for byte.
"""
import argparse
import collections
import glob
import json
import os
import struct
import sys
import time

import capstone

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))   # (python -I leaves the script folder off)
import kit
import nso2  # noqa: F401  (DT_RELR)
import insn
import lift_fixes
import suyu_rt_fixes
import chunks
import standalone
import adr_entries  # noqa: E402
import nso
import poll
import nx_image
import image as eden_image
import a64emit
import a64_lift
import a64_rtgen

insn.install()
lift_fixes.install()
suyu_rt_fixes.install()


def module_of(img, pc):
    for lo, hi, name in img.code:
        if lo <= pc < hi:
            return name
    return "?"


def coverage(L, img, out_dir):
    """Per-module: functions, instructions, untranslated words; and the share of functions that translate fully."""
    keys = {}
    for body, _ in L.funcs.values():
        for pc, (k, w) in body.items():
            if k[0] == "op":
                keys.setdefault(w, pc)
    words = list(keys)
    _, flags = a64emit.translate([(keys[w], w) for w in words])
    bad = {w for w, f in zip(words, flags) if f & a64emit.UNHANDLED}
    # UDF and unallocated encodings trap on the hardware too (code falls into them after noreturn calls): the
    # lifter's trap is their exact translation, so they do not count against coverage
    md = capstone.Cs(capstone.CS_ARCH_ARM64, capstone.CS_MODE_ARM)
    undefined = set()
    for w in bad:
        ins = next(md.disasm(struct.pack("<I", w), 0), None)
        if ins is None or ins.mnemonic == "udf":
            undefined.add(w)
    bad -= undefined
    per = collections.defaultdict(collections.Counter)
    sites = collections.Counter()
    for e, (body, _) in L.funcs.items():
        m = module_of(img, e)
        n_bad = 0
        for pc, (k, w) in body.items():
            if k[0] == "op" and w in bad:
                n_bad += 1
                sites[w] += 1
            elif k[0] == "op" and w in undefined:
                per[m]["undefined"] += 1
        kinds = collections.Counter(k[0] for k, _ in body.values())
        c = per[m]
        c["functions"] += 1
        c["instructions"] += len(body)
        c["untranslated"] += n_bad
        c["complete"] += n_bad == 0
        c["leaf"] += not (kinds["bl"] or kinds["blr"] or kinds["svc"])
        c["indirect_br"] += sum(1 for pc, (k, _) in body.items() if k[0] == "br" and pc not in L.jt)
        c["blr"] += kinds["blr"]
    total = collections.Counter()
    lines = ["module    functions  complete   leaf      instructions  untranslated  BLR       BR(not table)"]
    for m in sorted(per, key=lambda m: -per[m]["instructions"]):
        c = per[m]
        total.update(c)
        lines.append("%-8s  %9d  %6.2f%%  %8d  %12d  %12d  %8d  %8d" % (
            m, c["functions"], 100.0 * c["complete"] / c["functions"], c["leaf"], c["instructions"], c["untranslated"],
            c["blr"], c["indirect_br"]))
    lines.append("%-8s  %9d  %6.2f%%  %8d  %12d  %12d  %8d  %8d" % (
        "all", total["functions"], 100.0 * total["complete"] / total["functions"], total["leaf"],
        total["instructions"], total["untranslated"], total["blr"], total["indirect_br"]))
    lines.append("undefined encodings reached (UDF, unallocated: trap, as on the hardware): %d sites" % total["undefined"])
    lines.append("translated instructions: %.4f%% of %d (shared tails counted once per function)" % (
        100.0 - 100.0 * total["untranslated"] / max(1, total["instructions"]), total["instructions"]))
    print("\n".join(lines))
    with open(os.path.join(out_dir, "coverage.txt"), "w") as f:
        f.write("\n".join(lines) + "\n\nuntranslated words by site count\n")
        f.write("".join("%08x %d\n" % (w, n) for w, n in sites.most_common()))
    return total




def main():
    kit.wait_for_quiet('the lift')
    p = argparse.ArgumentParser()
    p.add_argument("--exefs", default=kit.EXEFS)
    p.add_argument("--work", default=kit.WORK)
    p.add_argument("--write", action="store_true")
    p.add_argument("--max-func", type=int, default=0)
    p.add_argument("--layout", choices=("eden", "a64"), default="eden")
    p.add_argument("--aslr", type=lambda v: int(v, 0), default=0)
    p.add_argument("--no-polls", action="store_true", help="no loop-header interrupt polls (nx2/poll.py)")
    p.add_argument("--rechunk", action="store_true", help="chunk boundaries from the sizes again (nx2/chunks.py)")
    p.add_argument("--standalone", action="store_true", help="the standalone build's lift (nx2/standalone.py)")
    p.add_argument("--entries", help="extra entry addresses, one hex address a line (e.g. the misses ArmRecomp logged)")
    p.add_argument("--no-adr-entries", action="store_true",
                   help="no extra entries at the code addresses ADR / ADRP+ADD take (nx2/adr_entries.py)")
    a = p.parse_args()
    a.exefs = a.exefs or kit.need_exefs()
    t0 = time.time()
    last = [t0]

    def took(what):   # (each step's seconds, and the time since the start: where a lift's minutes go)
        now = time.time()
        print("lift: %s in %.1fs (%.0fs)" % (what, now - last[0], now - t0), flush=True)
        last[0] = now
    hle = ()
    if a.standalone:
        hle, stubbed = standalone.hle_set(nso.load_exefs(a.exefs))
        print("standalone: %d HLE functions in host/, %d OS-facing imports stubbed" % (len(hle), len(stubbed)))
        hle = hle | stubbed
    img = eden_image.EdenImage(a.exefs, a.aslr, hle=hle) if a.layout == "eden" else nx_image.NxImage(a.exefs)
    out_dir = os.path.join(a.work, "sa") if a.standalone else a.work
    img_dir = os.path.join(out_dir, "image")
    meta = img.write(img_dir)
    print("image: %s; %d entries, %d trampolines (%d unresolved non-weak imports) (%.0fs)" % (
        ", ".join("%s@%#x+%#x" % (m["name"], m["base"], m["size"]) for m in meta["modules"]),
        len(img.entries), len(img.tramp), len(img.unresolved), time.time() - t0))
    if img.unresolved:
        print("  unresolved: " + ", ".join(sorted(img.unresolved)[:16]))
    if a.entries:
        extra = {int(l.split()[0], 16) for l in open(a.entries) if l.strip() and not l.startswith("#")}
        new = {e for e in extra if img.in_code(e)} - img.entries
        img.entries |= new
        print("extra entries: %d of %d listed are new code addresses" % (len(new), len(extra)))
    if not a.no_adr_entries:   # (hookshot) callbacks reached only through a pointer the code builds
        spans = adr_entries.fde_spans(img.modules)
        refs = adr_entries.code_address_refs(open(os.path.join(img_dir, "image.bin"), "rb").read(), meta, spans=spans)
        new = {e for e in refs if img.in_code(e)} - img.entries
        img.entries |= new
        print("ADR / ADRP+ADD code addresses outside the %d unwind-covered functions: %d, %d of them new entries" % (
            len(spans), len(refs), len(new)))
    L = a64_lift.Lifter(img)
    if a.standalone:   # (hookshot) widescreen and ultrawide: C after a few of the game's instructions
        import oot_wide
        oot_wide.install(L, img)
        import oot_fov   # (the gameplay lens, before projection and culling are built)
        oot_fov.install(L, img, next((m["build_id"] for m in meta["modules"] if m["name"] == "main"), ""))
        import oot_jitter   # (the temporal upscalers' camera jitter, DLSS's and FSR 3.1's: none unless one runs, host/rx_backend.c)
        oot_jitter.install(L, img)
        import oot_occlusion   # (no occlusion queries when HOOKSHOT_OCCLUSION_OFF=1: Fritchii's Occlusion Off)
        oot_occlusion.install(L, img)
    L.discover(max_insns=a.max_func)
    print("discovered %d functions (%.0fs)" % (len(L.funcs), time.time() - t0))
    last[0] = time.time()
    total = coverage(L, img, img_dir)
    took("coverage")
    jt = L.stats
    print("jump tables %d (bounded %d, bounded by function %d, ADR table %d), BR without a table %d, PLT stubs %d" % (
        len(L.jt), jt["jt_bounded"], jt["jt_unbounded"], jt["jt_adr_table"], len(L.jt_missing), len(L.plt)))
    if jt["jt_table_reg_reused"]:
        print("jump tables whose table register is reused before the br: %d" % jt["jt_table_reg_reused"])
    for pc, reg in sorted(L.jt_missing.items()):
        print("  BR without a table: %#x (x%d)" % (pc, reg))
    with open(os.path.join(img_dir, "functions.json"), "w") as f:
        json.dump({"%x" % e: [len(b), module_of(img, e)] for e, (b, _) in L.funcs.items()}, f)
    if a.write:
        if not a.no_polls:
            poll.install()
        gen = os.path.join(out_dir, "gen")
        files = chunks.write(L, gen, os.path.join(a.work, "image"), a.rechunk and not a.standalone,
                             counters=[] if a.no_polls else [poll.stats], took=took)
        import oot_patches
        main_build = next((m["build_id"] for m in meta["modules"] if m["name"] == "main"), "")
        def tables():   # (the runtime tables and the standalone files: they read the image, not the lifted C)
            lines = []
            a64_rtgen.write(gen)
            if a.standalone:
                start, exit_ = standalone.write_image_c(gen, img, meta, a.exefs)
                lines.append("standalone: hos_image.c (thread hooks %#x, %#x), %d import stubs" % (
                    start, exit_, standalone.gen_stubs(gen, meta)))
                nvn = standalone.nvn_exports(nso.load_exefs(a.exefs))
                standalone.write_nvn_exports(gen, nvn)
                lines.append("standalone: nvn2_exports.c (%d sdk NVN functions routed to host/nvn2.c)" % len(nvn))
            return lines
        # (made in a process of their own while the C is edited: 4-8 s that waited for the edits. Where there is no fork, after them)
        side = chunks.start_side(tables)
        chunks.edit_c(gen, L.stats, a.standalone and main_build == oot_patches.LIFE_BUILD_ID, whole=not a.max_func, took=took)
        print("\n".join(chunks.end_side(side, tables)), flush=True)
        took("runtime tables and standalone files (beside the edits)" if side else "runtime tables and standalone files")
        L.report()
        print("interrupt polls: %d loop headers in %d functions" % (poll.stats["polls"], poll.stats["functions"]))
        print("wrote %d code files to %s (%.0fs)" % (len(files), gen, time.time() - t0))
    print("done (%.0fs)" % (time.time() - t0))


if __name__ == "__main__":
    main()
