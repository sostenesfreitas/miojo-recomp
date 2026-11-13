"""The HDR10 pass (gpu/vk/shaders/hdr_encode.comp): hdr_encode_comp_spv.h names the sha256 of the shader it was compiled from, and
with glslangValidator (--glslang) the header's code is the shader compiled now. And the PQ curve and the highlight expansion it
uses, checked in Python: paper white lands where ST 2084 puts it, the picture's white reaches the peak, black stays black.
The HDR contrast row's grading, the shader's own grade() and shoulder() run in Python: nothing at 100%, and at any contrast
from 50% to 200% the light rises with the picture's, black stays black and its top lands on the peak, eased into it rather
than cut flat. Native is graded under the peak the game is told, moves on smoothly from 100%, and the game's light above that
peak is not cut flat (no peak limit for it). And its push constants are the ones framegen_hdr pushes.
Also what the HDR paths around it need, from the sources: Native HDR is told by the format the game presents alone, its
picture never blitted from its image, and at 100% contrast (exposure 1) the pass writes the game's PQ codes as they are.
While the game presents PQ (present_native_pq), NVIDIA DLSS is made for HDR colour and given the scene as linear light,
unclamped (dlss_prep.comp's kSquare and kColorMax), its picture taken back linear (dlss_out.comp's kSquare off). AMD FSR 4
and AMD FSR 3.1 on the chain take it the same way.

    python -I -B tests/test_hdr_encode.py [--glslang glslangValidator]
"""
import argparse, hashlib, re, subprocess, sys, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
VK = ROOT / "gpu" / "vk"
SHADERS = VK / "shaders"


def pq(nits):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    l = (min(max(nits / 10000.0, 0.0), 1.0)) ** m1
    return ((c1 + c2 * l) / (1 + c3 * l)) ** m2


def pq_decode(e):
    m1, m2, c1, c2, c3 = 0.1593017578125, 78.84375, 0.8359375, 18.8515625, 18.6875
    p = min(max(e, 0.0), 1.0) ** (1 / m2)
    return 10000.0 * (max(p - c1, 0.0) / (c2 - c3 * p)) ** (1 / m1)


def nits(y, white, peak, knee=0.6):
    over = min(max((y - knee) / (1 - knee), 0.0), 1.0)
    return y * white * (1 + (peak / white - 1) * over * over)


def function(source, start):
    """A function's text from its first line to its closing brace at the start of a line"""
    at = source.find(start)
    if at < 0:
        sys.exit("FAIL: %s is missing" % start.rstrip(" {("))
    body = source[at:]
    return body[:body.index("\n}\n")]


def glsl_functions(text, names, consts):
    """The shader's scalar functions (names) as Python: their statements are const float declarations, if (...) return ...
    and return ...; consts: what pc.<field> reads"""
    out = {}
    scope = dict(consts, min=min, max=max, abs=abs, pow=pow)
    for name in names:
        m = re.search(r"\nfloat %s\(([^)]*)\) \{(.*?)\n?\}\n" % name, text, re.S)
        if not m:
            sys.exit("FAIL: hdr_encode.comp has no float %s(...)" % name)
        params = [a.split()[-1] for a in m.group(1).split(",")]
        lines = ["def %s(%s):" % (name, ", ".join(params))]
        for stmt in re.sub(r"//[^\n]*", "", m.group(2)).split(";"):
            stmt = " ".join(stmt.split()).replace("||", "or").replace("&&", "and").replace("pc.", "pc_")
            if not stmt:
                continue
            cond = re.fullmatch(r"if \((.*)\) return (.*)", stmt)
            decl = re.fullmatch(r"const float (\w+) = (.*)", stmt)
            if cond:
                lines.append("    if %s: return %s" % cond.groups())
            elif decl:
                lines.append("    %s = %s" % decl.groups())
            elif stmt.startswith("return "):
                lines.append("    " + stmt)
            else:
                sys.exit("FAIL: %s(): a statement this check cannot read: %s" % (name, stmt))
        exec("\n".join(lines), scope)
        out[name] = scope[name]
    return out


def check_grading():
    # (the row: 50 to 200%, 100% (none) out of the box, kept in the options file as hdr_contrast, its name's key)
    menu = (VK / "vk_menu.cpp").read_text(encoding="utf-8")
    if not re.search(r'\{TAB_GRAPHICS, "HDR contrast", &opts\.hdr_contrast, 50, 200, 5, tx_percent,', menu) or \
            "int hdr_contrast = 100;" not in (VK / "vk_menu.h").read_text(encoding="utf-8"):
        sys.exit("FAIL: the HDR contrast row is not 50 to 200% in steps of 5, 100% by default")
    text = (SHADERS / "hdr_encode.comp").read_text(encoding="utf-8")
    # (the push constants: the shader's, and framegen_hdr's struct in the same order, 48 bytes)
    block = re.sub(r"//[^\n]*", "", text[text.index("uniform P {") + 11:text.index("} pc;")])
    shader = [n.strip() for decl in block.split(";") if decl.strip() for n in decl.split(None, 1)[1].split(",")]
    hdr = function((VK / "vk_framegen.cpp").read_text(encoding="utf-8"), "VkImage framegen_hdr(")
    pushed = re.search(r"const struct \{ float uv\[4\]; float ([^;]*); \} pc", hdr)
    if shader != ["uv", "white", "peak", "knee", "mode", "contrast", "exposure", "pad0", "pad1"] or not pushed or \
            [n.strip() for n in pushed.group(1).split(",")] != shader[1:] or "{S, I}, 48)" not in hdr:
        sys.exit("FAIL: hdr_encode.comp's push constants %s are not the 48 bytes framegen_hdr pushes" % shader)
    main = text[text.index("void main()"):]
    # (Native at 100% contrast and exposure 1: the game's PQ codes written as they are, before any decode or encode)
    through = main.index("if (pc.mode >= 0.5 && pc.contrast == 1.0 && pc.exposure == 1.0) {")
    if "imageStore(dst, at, vec4(c, 1.0));\n        return;" not in main[through:through + 200] or \
            through > main.index("pq_decode(c)"):
        sys.exit("FAIL: hdr_encode.comp does not pass Native's PQ codes through as they are at 100% contrast")
    for line in ("if (y > 1e-6 && (pc.contrast != 1.0 || pc.exposure != 1.0)) {",
                 "const float top = pc.peak / pc.white;",
                 "nits *= shoulder(grade(y), grade(top), top) / y;", "if (pc.mode < 0.5 && yn > pc.peak) nits *= pc.peak / yn;"):
        if line not in main:
            sys.exit("FAIL: hdr_encode.comp's main does not grade as this check expects: %s" % line)
    for white, peak in ((200, 1000), (230, 800), (80, 400), (400, 400), (80, 4000), (400, 4000)):
        top = peak / white
        for percent in range(50, 205, 5):
            f = glsl_functions(text, ("grade", "shoulder"), {"pc_exposure": 1.0, "pc_contrast": percent / 100})
            # (Expanded: the SDR picture's luminance 0..1 as the pass expands it, relative to the paper white)
            ys = [nits(i / 2000, white, peak) / white for i in range(2001)]
            out = [f["shoulder"](f["grade"](y), f["grade"](top), top) for y in ys]
            where = "at %d%%, paper white %d, peak %d" % (percent, white, peak)
            if percent == 100 and any(abs(o - y) > 1e-9 * max(1.0, y) for o, y in zip(out, ys)):
                sys.exit("FAIL: 100% changes the picture " + where)
            if any(b < a - 1e-12 for a, b in zip(out, out[1:])) or out[0] != 0.0:
                sys.exit("FAIL: the graded light does not rise with the picture's from black " + where)
            if abs(out[-1] - top) > 1e-9 * top or max(out) > top * (1 + 1e-12):
                sys.exit("FAIL: the picture's top does not land on the peak " + where)
            # (eased into the peak, not cut flat: a plain limit put a fifth of the picture at the peak at 150%)
            if sum(o >= 0.999 * top for o in out) > 0.03 * len(out):
                sys.exit("FAIL: the highlights are cut flat at the peak " + where)
            # (light past the picture's top goes on rising: Native's above the peak row, not cut flat; Expanded's has none)
            m = f["grade"](top)
            if not f["shoulder"](m * 1.5, m, top) > f["shoulder"](m, m, top) >= top * (1 - 1e-9):
                sys.exit("FAIL: light past the picture's top does not keep rising " + where)
        # (Native: the game's own PQ picture, codes 0..1. Graded under the peak the game is told (the HDR peak row, which it stays
        # under), its top and the shoulder's target; light the game puts above that peak keeps rising, with no limit after it)
        for percent in range(50, 205, 5):
            f = glsl_functions(text, ("grade", "shoulder"), {"pc_exposure": 1.0, "pc_contrast": percent / 100})
            ys = [pq_decode(i / 2000) / white for i in range(2001)]
            out = [f["shoulder"](f["grade"](y), f["grade"](top), top) for y in ys]
            where = "with Native at %d%%, paper white %d, peak %d" % (percent, white, peak)
            if percent == 100 and any(abs(o - y) > 1e-9 * max(1.0, y) for o, y in zip(out, ys)):
                sys.exit("FAIL: 100% changes the game's picture " + where)
            if out[0] != 0.0 or any(b <= a for a, b in zip(out, out[1:])):
                sys.exit("FAIL: the game's light does not rise with each PQ code from black (above the peak too) " + where)
            if abs(f["shoulder"](f["grade"](top), f["grade"](top), top) - top) > 1e-9 * top:
                sys.exit("FAIL: the peak the game is told, graded, does not land on itself " + where)
            if any(o > top * (1 + 1e-12) for o, y in zip(out, ys) if y <= top):
                sys.exit("FAIL: the game's light under the peak row is graded past it " + where)
        # (continuous with 100%, where the picture is passed through as it is: at 95% and 105% the paper white and 0.9 of the
        # peak move by less than a fifth)
        graded = {}
        for percent in (95, 100, 105):
            f = glsl_functions(text, ("grade", "shoulder"), {"pc_exposure": 1.0, "pc_contrast": percent / 100})
            graded[percent] = [f["shoulder"](f["grade"](y), f["grade"](top), top) * white for y in (1.0, 0.9 * top)]
        for percent in (95, 105):
            if any(abs(g / n - 1) >= 0.2 for g, n in zip(graded[percent], graded[100])):
                sys.exit("FAIL: Native's grade jumps away from the picture at 100%%: %d%% gives %s nits for %s, paper white %d, "
                         "peak %d" % (percent, ["%.0f" % g for g in graded[percent]], ["%.0f" % n for n in graded[100]],
                                      white, peak))


def check_paths():
    # (Native told by the format the game presents, the view's, alone: the cached image's format is not the picture's when the
    # two share an image by format class, either way round)
    replay = (VK / "vk_replay.cpp").read_text(encoding="utf-8")
    present = function(replay, "void do_present(")
    native = present[present.index("present_native_pq.store("):]
    native = native[:native.index(";")]
    if "const VkFormat shown_format = t ? vk_format(t->d.format).format" not in present or \
            "shown_format == VK_FORMAT_A2B10G10R10_UNORM_PACK32" not in native or "tex->format" in native:
        sys.exit("FAIL: do_present does not tell Native HDR by the format the game presents alone")
    # (Native: never the game's image blitted in its image's format; the shot's scale pass told it is PQ as well)
    image = function(replay, "void present_image(")
    if "} else if (src && !native_pq) {" not in image or \
            "framegen_scale(f.cmd, view, src, sw, sh, crop, rect.w, rect.h, native_pq)" not in image:
        sys.exit("FAIL: present_image can blit Native HDR's picture from its image, or scales it as SDR")
    shot = present[present.index("const VkImage fx_img ="):]
    if "kShotW, kShotH,\n" not in shot[:shot.index(";")] or "present_native_pq.load(" not in shot[:shot.index(";")]:
        sys.exit("FAIL: the RX_VK_SHOT_FX shot scales Native HDR's picture as SDR")
    check_dlss()
    check_fsr4()
    check_fsr3()


def check_fsr3():
    # (AMD FSR 3.1 on the chain the same way: while the game presents PQ it takes the chain's linear light (its passes are built
    # for HDR colour and work out their own exposure), a change of that starts a new history, and the engine tells the chain)
    fsr3 = (VK / "vk_fsr3.cpp").read_text(encoding="utf-8")
    prepare = function(fsr3, "int chain_prepare(")
    if "const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);" not in prepare or \
            'env_switch("RX_VK_FSR3_HDR")' not in prepare or "opts.hdr" in prepare or "c.hdr_changed = true" not in prepare:
        sys.exit("FAIL: FSR 3.1 on the chain does not take HDR colour while the game presents PQ (or keeps its history when that changes)")
    if "ai.reset = f.reset || c.hdr_changed;" not in function(fsr3, "bool chain_evaluate("):
        sys.exit("FAIL: FSR 3.1 on the chain keeps its history across a change of colour encoding")
    if ".hdr = chain_hdr," not in fsr3 or "bool chain_hdr() { return chain_state().hdr; }" not in fsr3:
        sys.exit("FAIL: vk_fsr3.cpp's chain engine does not tell the chain it takes HDR colour")
    if "-DFFX_FSR3UPSCALER_OPTION_HDR_COLOR_INPUT=1" not in (ROOT / "scripts" / "fsr3_shaders.py").read_text(encoding="utf-8"):
        sys.exit("FAIL: FSR 3.1's passes are not built for HDR colour")


def check_fsr4():
    # (AMD FSR 4 the same way: while the game presents PQ its context is made HDR, linear, with AMD's auto exposure, and again
    # when that changes; the dispatch then says linear colour, not sRGB, and the engine tells the chain it takes HDR colour)
    fsr4 = (VK / "vk_fsr4.cpp").read_text(encoding="utf-8")
    prepare = function(fsr4, "int prepare(")
    if "const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);" not in prepare or \
            'env_switch("RX_VK_FSR4_HDR")' not in prepare or "opts.hdr" in prepare or \
            "s.context->hdr != hdr" not in prepare or "make_context(ow, oh, hdr)" not in prepare:
        sys.exit("FAIL: FSR 4's context is not made for HDR colour while the game presents PQ (and again when that changes)")
    make = function(fsr4, "std::shared_ptr<Context> make_context(")
    if "FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE | FFX_UPSCALE_ENABLE_AUTO_EXPOSURE" not in make or \
            "FFX_UPSCALE_ENABLE_NON_LINEAR_COLORSPACE" not in make:
        sys.exit("FAIL: FSR 4's HDR context is not linear HDR with AMD's exposure (or its LDR one is not non-linear)")
    evaluate = function(fsr4, "bool evaluate(")
    if "d.flags = s.context->hdr ? 0u : (uint32_t)FFX_UPSCALE_FLAG_NON_LINEAR_COLOR_SRGB;" not in evaluate:
        sys.exit("FAIL: FSR 4's dispatch calls HDR colour sRGB (it is linear light there)")
    if "start, prepare, evaluate, release, memory, hdr}" not in fsr4:
        sys.exit("FAIL: vk_fsr4.cpp's engine does not tell the chain it takes HDR colour")


def check_dlss():
    # (the game's native HDR goes past 1 in the scene: DLSS made for HDR colour while the game presents PQ, and given the
    # scene as linear light (the square of its square-root encoding), what IsHDR and AutoExposure are made for, not clamped
    # to 1; its picture then taken back as it is, linear, where the LDR pair squares it: the same output either way)
    prep = (SHADERS / "dlss_prep.comp").read_text(encoding="utf-8")
    if "layout(constant_id = 0) const float kColorMax = 1.0;" not in prep or \
            "layout(constant_id = 1) const bool kSquare = false;" not in prep or \
            "const vec3 c = kSquare ? e * e : e;" not in prep or "imageStore(color, g, vec4(min(c, vec3(kColorMax)), 1.0));" not in prep:
        sys.exit("FAIL: dlss_prep.comp does not square its colour into linear light (kSquare, constant 1, off by default) "
                 "under its ceiling (kColorMax, constant 0, 1 by default)")
    out = (SHADERS / "dlss_out.comp").read_text(encoding="utf-8")
    if "layout(constant_id = 0) const bool kSquare = true;" not in out or "if (kSquare) c *= c;" not in out:
        sys.exit("FAIL: dlss_out.comp's square is not its kSquare (constant 0, on by default)")
    chain = (VK / "vk_temporal.cpp").read_text(encoding="utf-8")
    pipes = function(chain, "bool make_pipelines() {")
    for want in ("PrepSpec prep_hdr_data{16384.0f, VK_TRUE};",
                 "prep_hdr_entries[2] = {{0, 0, sizeof(float)}, {1, sizeof(float), sizeof(VkBool32)}};",
                 "out_hdr_square = VK_FALSE;", "out_hdr_entry{0, 0, sizeof(VkBool32)};",
                 "s.prep_hdr, &prep_hdr_spec))", "s.out_hdr, &out_hdr_spec))",
                 "if (s.prep && s.prep_hdr && s.out && s.out_hdr && s.point && s.linear) return true;"):
        if want not in pipes:
            sys.exit("FAIL: vk_temporal.cpp's HDR pipelines (linear light in, linear out) are not as this check expects: " + want)
    upscale = function(chain, "bool temporal_upscale(")
    for want in ("const bool hdr = engine.e->hdr && engine.e->hdr();", "prep_pipe = hdr ? s.prep_hdr : s.prep",
                 "out_pipe = hdr ? s.out_hdr : s.out", "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, prep_pipe);",
                 "vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, out_pipe);"):
        if want not in upscale:
            sys.exit("FAIL: temporal_upscale does not give an engine made for HDR colour the HDR pair: " + want)
    dlss = (VK / "vk_dlss.cpp").read_text(encoding="utf-8")
    prepare = function(dlss, "int prepare(")
    if "const bool hdr = hdr_allowed && present_native_pq.load(std::memory_order_relaxed);" not in prepare or \
            'getenv("RX_VK_DLSS_HDR")' not in prepare or "opts.hdr" in prepare or \
            "s.feature_hdr == hdr" not in prepare or \
            "NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure" not in prepare:
        sys.exit("FAIL: DLSS's feature is not made for HDR colour while the game presents PQ (and again when that changes)")
    if "start, prepare, evaluate, release, memory, hdr" not in dlss:   # (fields may follow it)
        sys.exit("FAIL: vk_dlss.cpp's engine does not tell the chain it takes HDR colour")


a = argparse.ArgumentParser()
a.add_argument("--glslang")
args = a.parse_args()
src = (SHADERS / "hdr_encode.comp").read_bytes()
header = (SHADERS / "hdr_encode_comp_spv.h").read_text()
first = header.splitlines()[0]
if hashlib.sha256(src).hexdigest() not in first:
    sys.exit("FAIL: hdr_encode_comp_spv.h was not made from hdr_encode.comp as it is (recompile it)")
assert abs(pq(100) - 0.5081) < 0.001 and abs(pq(1000) - 0.7518) < 0.001 and pq(0) < 1e-6, "PQ curve"
assert abs(nits(1.0, 200, 1000) - 1000) < 1e-6 and abs(nits(0.5, 200, 1000) - 100) < 1e-6 and nits(0, 200, 1000) == 0
assert all(nits(a / 100, 200, 1000) <= nits((a + 1) / 100, 200, 1000) for a in range(100)), "monotonic"
check_paths()
check_grading()
if args.glslang:
    with tempfile.TemporaryDirectory() as d:
        out = Path(d, "h.h")
        subprocess.run([args.glslang, "-V", "--target-env", "vulkan1.2", "--vn", "HDR_ENCODE_COMP_SPV", "-o", str(out),
                        str(SHADERS / "hdr_encode.comp")], check=True, capture_output=True)
        if out.read_text() != "\n".join(header.splitlines(True)[1:]) and out.read_text() != "".join(header.splitlines(True)[1:]):
            sys.exit("FAIL: hdr_encode_comp_spv.h differs from the shader compiled now")
print("PASS: the HDR10 pass's header is its shader, PQ, the highlight expansion and the contrast grading as intended, "
      "and the HDR paths around it")
