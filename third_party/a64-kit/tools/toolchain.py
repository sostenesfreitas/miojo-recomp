import os, shutil

HERE = os.path.dirname(os.path.abspath(__file__))


def emsdk():
    return os.environ.get("EMSDK") or (r"C:\emsdk" if os.name == "nt" else os.path.expanduser("~/emsdk"))


def windows():
    """(hookshot) the game is built for Windows: on Windows, or with RX_TARGET=windows on another system (a cross build, with
    llvm-mingw's release for that system in HOOKSHOT_MINGW)"""
    return os.name == "nt" or os.environ.get("RX_TARGET", "").lower() == "windows"


def mingw(host=False):
    """(hookshot) HOOKSHOT_MINGW=<llvm-mingw folder>: Windows builds with that toolchain's clang, headers and libraries (the GNU ABI
    on the UCRT), as TotK Recomp's installer does: nothing of Visual Studio or the Windows SDK is needed. Empty: the MSVC target.
    host: for a tool the build itself runs (zbic, a64emit), which a cross build makes for this PC, not for Windows"""
    # (hookshot) the variable is HOOKSHOT_MINGW, as the rest of Hookshot's build sets it (Hookshot's was HOOKSHOT_MINGW)
    return os.environ.get("HOOKSHOT_MINGW", "") if windows() and not (host and os.name != "nt") else ""


def find(tool, host=False):
    if mingw(host) and (tool in ("clang", "clang++", "lld") or tool.startswith("llvm-")):
        path = os.path.join(mingw(host), "bin", ("ld.lld" if tool == "lld" else tool) + (".exe" if os.name == "nt" else ""))
        if os.path.isfile(path):
            return path
    root = emsdk()
    exts = (".exe", ".bat") if os.name == "nt" else ("",)
    native = tool in ("clang", "clang++", "lld") or tool.startswith("llvm-")
    folder = os.path.join(root, "upstream", "bin") if native else os.path.join(root, "upstream", "emscripten")
    for ext in exts:
        path = os.path.join(folder, tool + ext)
        if os.path.isfile(path):
            return path
    return shutil.which(tool) or os.path.join(folder, tool + exts[0])


def need(path):
    if not os.path.isfile(path):
        raise SystemExit("%s not found: set EMSDK to an emsdk checkout (now %s), or put %s on PATH"
                         % (path, emsdk(), os.path.splitext(os.path.basename(path))[0]))
    return path


def lld_link():
    """emsdk's lld under the name lld-link (lld picks its COFF linker from its own file name), made on first use"""
    path = os.path.join(HERE, "lld-link-mingw.exe" if mingw() else "lld-link.exe")
    if not os.path.exists(path):
        shutil.copyfile(need(find("lld")), path)
    return path


def native_target(host=False):
    if mingw(host):   # (-fms-extensions: __try/__except and __declspec, which clang takes on this target too)
        return ["--target=x86_64-w64-mingw32", "-fms-extensions", "-D_CRT_SECURE_NO_WARNINGS"]
    return ["--target=x86_64-pc-windows-msvc", "-fms-extensions", "-D_CRT_SECURE_NO_WARNINGS"] if os.name == "nt" else []


def native_link(host=False):
    # (-fuse-ld=lld, found through -B: clang ignores --ld-path for the MSVC target and ran Microsoft's link.exe, which
    # cannot read the renderer's ThinLTO bitcode)
    if mingw(host):   # (its own lld; the C++ runtime and winpthread linked in, so no DLLs of the toolchain go beside the game)
        # (/nodefaultlib: libraries built with Microsoft's compiler, the CPM cache's FFmpeg, ask in their objects for its C runtime;
        # the MinGW one stands in, with host/mingw_msvc_shims.c for what it lacks)
        return ["-fuse-ld=lld", "-static"] + ["-Wl,-Xlink=/nodefaultlib:" + l for l in ("libcmt", "oldnames", "libvcruntime", "msvcrt")]
    # (issue 439: clang names oldnames.lib as a default library for the MSVC target, and recent Windows SDKs no longer
    # ship it (an older SDK has it in ucrt\x64; Visual Studio has its own copy in its lib folder). The game and host use
    # no legacy non-underscore name; the ones the prebuilt static FFmpeg libraries ask for are host/msvc_oldnames.c)
    return (["-fuse-ld=lld", "-B" + os.path.dirname(lld_link()), "-Wl,/nodefaultlib:oldnames"]
            if os.name == "nt" else [])


def link_opt(opt):
    """A link.exe-style option ("/map:x"): as it is for the MSVC target, through lld's MinGW driver (-Xlink) for llvm-mingw"""
    return "-Wl,-Xlink=" + opt if mingw() else "-Wl," + opt


def default_jobs(gb_per_job=3.5):
    """Parallel compiles that fit in memory: a -O3 compile of one of the game's big lifted chunks takes 3-8 GB, and one
    job per logical core ran 16 GB machines out of memory (22 threads on an Ultra 7 155H). 80% of the physical memory
    at gb_per_job each, at most one a core. RX_JOBS=<n> sets it. (After SkyboundTotk PR 14 by leonelalej.)"""
    if os.environ.get("RX_JOBS"):
        return max(1, int(os.environ["RX_JOBS"]))
    total = 0
    try:
        if os.name == "nt":
            import ctypes

            class MemoryStatus(ctypes.Structure):
                _fields_ = [("length", ctypes.c_ulong), ("load", ctypes.c_ulong)] + \
                           [(f, ctypes.c_ulonglong) for f in ("phys", "avail_phys", "page", "avail_page", "virt",
                                                              "avail_virt", "avail_ext")]
            m = MemoryStatus()
            m.length = ctypes.sizeof(m)
            if ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(m)):
                total = m.phys
        else:
            total = os.sysconf("SC_PAGE_SIZE") * os.sysconf("SC_PHYS_PAGES")
    except (OSError, ValueError, AttributeError):
        pass
    cores = os.cpu_count() or 1
    if not total:
        return cores
    return max(1, min(cores, int(total / 2 ** 30 * 0.8 / gb_per_job)))
