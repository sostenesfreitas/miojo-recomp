"""The renderer's play profile (RX_VK_PGO, after ProjectReLink) is opt-in at every step: with no work/hookshot_vk.profdata the
renderer is built as it always was, a developer's own RX_VK_PGO (gen, or another profile) is never replaced, a profile this
compiler cannot read is left out rather than stopping the build, and the game module's counters keep their own variable.

What nx2/build.py does with RX_VK_PGO (which files count, the writer, the object keys) is checked on its real branch in
tests/test_shader_stamp_cache.py; that neither profile switch translates the game again, in tests/test_lift_cache.py.
No compiler, no game.

python -I -B tests/test_vk_pgo.py
"""
import importlib.util
import os
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("make_game_under_test", ROOT / "nx2" / "make_game.py")
make_game = importlib.util.module_from_spec(spec)
spec.loader.exec_module(make_game)


def check(what, ok):
    if not ok:
        raise SystemExit("FAIL: " + what)


def check_make_game():
    asked = []

    def readable(prof):
        asked.append(prof)
        return True

    import lzma
    with tempfile.TemporaryDirectory(prefix="hookshot-vk-pgo-") as tmp:
        root, work = os.path.join(tmp, "checkout"), os.path.join(tmp, "work")
        os.makedirs(os.path.join(root, "work"))
        os.makedirs(os.path.join(root, "assets", "profiles"))
        os.makedirs(work)

        def pick(environ=None, can_read=readable):
            return make_game.vk_profile_for(work, environ or {}, can_read, root=root)
        check("neither the committed profile nor the checkout's: RX_VK_PGO is not set", pick() is None)
        check("no profile: the compiler is not asked to read one", not asked)
        # (the committed one, xz: unpacked into the work folder under its own name)
        with lzma.open(os.path.join(root, make_game.VK_PROFILE_SHIPPED), "wb") as f:
            f.write(b"shipped profile")
        unpacked = os.path.join(work, make_game.VK_PROFILE_UNPACKED)
        check("the committed profile is unpacked and passed", pick() == unpacked and open(unpacked, "rb").read() == b"shipped profile")
        stamp = os.stat(unpacked).st_mtime_ns - 10**9
        os.utime(unpacked, ns=(stamp, stamp))
        pick()
        check("the same committed profile is not written again", os.stat(unpacked).st_mtime_ns == stamp)
        with lzma.open(os.path.join(root, make_game.VK_PROFILE_SHIPPED), "wb") as f:
            f.write(b"a newer one")
        check("a new committed profile replaces the unpacked one", pick() == unpacked and open(unpacked, "rb").read() == b"a newer one")
        # (a developer's work/hookshot_vk.profdata: main copies it into the work folder, and it is used in place of the committed one)
        own = os.path.join(work, make_game.VK_PROFILE)
        for path in (own, os.path.join(root, "work", make_game.VK_PROFILE)):
            with open(path, "wb") as f:
                f.write(b"developer's profile")
        check("the checkout's own profile comes first", pick() == own)
        os.remove(os.path.join(root, "work", make_game.VK_PROFILE))
        check("a copy left in the work folder from a profile the checkout no longer has is not used", pick() == unpacked)
        check("a counting build stays one (RX_VK_PGO=gen)", pick({"RX_VK_PGO": "gen"}) is None)
        check("a developer's own profile is not replaced", pick({"RX_VK_PGO": "/else.profdata"}) is None)
        check("a profile this compiler cannot read is left out, the renderer built without it", pick(can_read=lambda p: False) is None)
    shipped = ROOT / make_game.VK_PROFILE_SHIPPED
    check("the renderer's profile is committed (assets/profiles/hookshot_vk.profdata.xz)", shipped.is_file())
    with lzma.open(shipped) as f:
        check("it is an indexed LLVM profile", f.read(8) == b"\xfflprofi\x81")
    note = (ROOT / "assets" / "profiles" / "README.md").read_text(encoding="utf-8")
    check("its note says where it was recorded", "RX_VK_PGO=gen" in note and "recording" in note.lower())
    text = (ROOT / "nx2" / "make_game.py").read_text(encoding="utf-8")
    check("the committed profile is checked on every system (another LLVM than llvm-mingw's made it)",
          "profile_readable(p, any_os=True)" in text)
    check("the lift key leaves the renderer's profile switches out (a profile made or taken away translates nothing again)",
          {"RX_VK_PGO", "RX_VK_PGO_FILE"} <= set(make_game.COMPILE_ONLY_ENV))
    check("make_game.py copies the profile from the checkout's work/ with the game's", '"hookshot_pieces.prof", VK_PROFILE,' in text)
    check("make_game.py passes it to the compile", 'env["RX_VK_PGO"] = vk_profile' in text)


def check_dev_packs():
    spec = importlib.util.spec_from_file_location("dev_packs_under_test", ROOT / "scripts" / "dev_packs.py")
    packs = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(packs)
    with tempfile.TemporaryDirectory(prefix="hookshot-vk-pgo-base-") as work:
        prof = os.path.join(work, make_game.VK_PROFILE)
        shipped = os.path.join(work, make_game.VK_PROFILE_UNPACKED)
        check("a base with no profile: a renderer pack is built with none", packs.renderer_profile(work, {}) == {})
        # (a base of main: make_game.py unpacked the committed profile and built with it)
        with open(shipped, "wb") as f:
            f.write(b"committed profile")
        check("a base built with the committed profile: a renderer pack is built with it too, by the base's path",
              packs.renderer_profile(work, {}) == {"RX_VK_PGO": shipped})
        with open(prof, "wb") as f:
            f.write(b"profile")
        check("a base whose checkout had its own profile: that one, as make_game.py chose it",
              packs.renderer_profile(work, {}) == {"RX_VK_PGO": prof})
        check("a counting pack stays one", packs.renderer_profile(work, {"RX_VK_PGO": "gen"}) == {})
    # (what a pack builds: a profile switch set for it builds, even with no file changed)
    for how, env, want, why in ((None, {}, None, "no change, no switch: nothing to build"),
                                (None, {"RX_VK_PGO": "gen"}, "renderer", "a counting renderer of an unchanged tree is built"),
                                (None, {"RX_VK_PGO": "/other.profdata"}, "renderer", "another renderer profile is built"),
                                ("game", {"RX_VK_PGO": "gen"}, "game", "a game change still builds both modules"),
                                ("renderer", {}, "renderer", "a renderer change, as before"),
                                (None, {"RX_PGO": "gen"}, "game", "the game's counting build of an unchanged tree is built"),
                                ("renderer", {"RX_PGO": "/p.prof"}, "game", "a game profile builds the game")):
        check("pack_route: " + why, packs.pack_route(how, env) == want)
    text = (ROOT / "scripts" / "dev_packs.py").read_text(encoding="utf-8")
    check("the renderer's own build takes it", '**renderer_profile(b["work"], os.environ)' in text)
    check("cmd_pack routes through pack_route", "how = pack_route(route(changed, tree), os.environ)" in text)


def check_writer():
    text = (ROOT / "host" / "pgo_rt.c").read_text(encoding="utf-8")
    check("the game module's counters are still driven by HOS_PGO", '#ifndef PGO_ENV\n#define PGO_ENV "HOS_PGO"\n#endif' in text)
    check("the variable is read through PGO_ENV", "getenv(PGO_ENV)" in text and 'getenv("HOS_PGO")' not in text)
    for section in ("cnts", "data", "names"):   # (Linux: the ELF sections' ends, this module's own)
        for end in ("start", "stop"):
            name = "__%s___llvm_prf_%s" % (end, section)
            check("%s is declared weak and hidden" % name, ("%s[] __attribute__((weak, visibility(\"hidden\")));" % name) in text)
    windows = text[text.index("#ifdef _WIN32"):text.index("#else")]
    check("the COFF sections are Windows' alone", ".lprfc$A" in windows and ".lprfc$A" not in text.replace(windows, ""))


def check_release():
    appimage = (ROOT / "scripts" / "linux" / "appimage.sh").read_text(encoding="utf-8")
    check("the AppImage builds no renderer of its own (every player's build compiles it, with the profile)",
          "RX_VK_ONLY" not in appimage and "RX_VK_PGO" not in appimage)
    check("the AppImage carries assets/ (the committed profile, for every player's own renderer)",
          "cp -a nx2 host scripts LICENSE readme.md assets " in appimage)
    check("and a developer's own profile when the checkout has one",
          "for f in hot_funcs.txt hot_order.txt hookshot_pieces.prof hookshot_vk.profdata; do" in appimage)
    docs = (ROOT / "docs" / "development.md").read_text(encoding="utf-8")
    for switch in ("RX_VK_PGO=gen", "RX_VK_PGO_FILE=<file>"):
        check("%s is in docs/development.md's switch tables" % switch, switch in docs)


if __name__ == "__main__":
    check_make_game()
    check_dev_packs()
    check_writer()
    check_release()
    print("OK: the renderer's play profile is opt-in, and the game's counters keep their own switch")
