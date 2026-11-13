"""NVIDIA's DLSS SDK files for a local build with NVIDIA DLSS Super Resolution (HOOKSHOT_NGX_SDK, gpu/vk/vk_dlss.cpp).

NVIDIA's files are not part of Hookshot and are never committed or bundled: they are under NVIDIA's own license (the DLSS
SDK's LICENSE.txt, fetched beside them; read it before using them). This fetches them from NVIDIA's DLSS repository at one
pinned release, each file checked against its SHA-256, into <sdk folder>:
  - LICENSE.txt and the SDK's headers (include/), the same for every platform;
  - the library the build links (lib/): libnvsdk_ngx.a on Linux, nvsdk_ngx_s.lib (the /MT one) on Windows;
  - the runtimes NGX loads (runtime/): DLSS's, libnvidia-ngx-dlss.so.<version> on Linux and nvngx_dlss.dll on Windows, and
    DLSS Frame Generation's, libnvidia-ngx-dlssg.so.<version> and nvngx_dlssg.dll. Without the latter beside DLSS's, NGX
    takes the driver's own copy, which may be older: one that makes 1 frame at most, or refuses every set.
The NGX core itself comes with NVIDIA's driver (libnvidia-ngx.so on Linux; _nvngx.dll on Windows, which is loaded from the
driver's own folder and is never to be put beside the game).

Usage:  python -I scripts/ngx_sdk.py [--accept-nvidia-license] [--platform linux|windows] [--game <game folder>]
                                     [--runtime-dir <folder>] [--build-only] [<sdk folder>]
  <sdk folder>: default build-linux/ngx-sdk on Linux, work/ngx-sdk on Windows
  --game: the DLSS runtimes copied into <game folder>/ngx too, where the game looks first (nx2/make_game.py does the same
          for the folder it builds when HOOKSHOT_NGX_SDK is set)
  --runtime-dir: the runtimes copied into that folder instead (Linux: $XDG_DATA_HOME/hookshot/ngx; Windows:
          %LOCALAPPDATA%\\Hookshot\\ngx. RX_VK_DLSS_DIR names another when the game runs)
  --build-only: only what the build reads, not the runtimes (CI's link check)
  --accept-nvidia-license: NVIDIA's license accepted without the question (scripts, the Hookshot Builder's DLSS tick, CI)
Then build with HOOKSHOT_NGX_SDK=<sdk folder>.
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request

COMMIT = "9981c6350dc58c0ce21f024fd65e31e851c31689"   # (v310.9.16)
BASE = "https://raw.githubusercontent.com/NVIDIA/DLSS/" + COMMIT
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LICENSE_URL = "https://github.com/NVIDIA/DLSS/blob/%s/LICENSE.txt" % COMMIT

# (path in NVIDIA's repository, its SHA-256): what every platform's build reads
COMMON = [
    ("LICENSE.txt", "d4216e39ebef5f9b50a6712ebb37beeb5379862a67733a9999c651f21592aaf0"),
    ("include/nvsdk_ngx_defs_dlssd.h", "2e97fe1595b1334f71b51724f2cb67496e8c296a7a2eec5606da53055882e716"),
    ("include/nvsdk_ngx_defs_dlssg.h", "c29765ab9e0125f9d724ae9811cc2abe03738d17486ec8eee80d78a1504549e1"),
    ("include/nvsdk_ngx_defs.h", "ea23f33497cd274860d1c25a97644fce807dcb0037c594547203343103fad03e"),
    ("include/nvsdk_ngx_defs_vk.h", "0a24d0861ace7d6b9362a67b7f08bea1b33ea123c5ce730b19133de8a891d031"),
    ("include/nvsdk_ngx.h", "dc38e7467cf415379c9d12ae1b6e4a494c453ed92720fb53e92aecb523e7b848"),
    ("include/nvsdk_ngx_helpers_cuda.h", "f2851a76107bdf4fbcb7b261f3c35573c312d8ecb4bc18fb02d9e2cec7705042"),
    ("include/nvsdk_ngx_helpers_d3d.h", "ec75224f36ed6580baaf250fa83620405ad81a1d98c2c436654a0c9cf6a6b8ba"),
    ("include/nvsdk_ngx_helpers_dlssd_cuda.h", "7e35cde214d905346a487b9a28c05d35e1c55b480e237fe3de69580a7e332730"),
    ("include/nvsdk_ngx_helpers_dlssd_d3d.h", "6df9d02c6f47feee3afc8d12340a9f554a7ddce4ddacb1b6a13a42b26c2c6293"),
    ("include/nvsdk_ngx_helpers_dlssd.h", "a25fdda925b479dfe869593aeba311ef08793da54e0391f2fe2fe756cbef5532"),
    ("include/nvsdk_ngx_helpers_dlssd_vk.h", "11df8fbda532a97fe867c8ef2b40058b49b7898183710f3468e55b1eedb8f40b"),
    ("include/nvsdk_ngx_helpers_dlssg_d3d.h", "6335ebcb95de39a361e87fe0257d6ce0723e5d8deaa15db3b35afacf2b3f4220"),
    ("include/nvsdk_ngx_helpers_dlssg.h", "6881d21419c40c78ec948b326902ece09fc112b0223144d51c4905f3fced7a1f"),
    ("include/nvsdk_ngx_helpers_dlssg_vk.h", "b371d90b9f4372b9a415e68a0afbdc2f31b7b552e219a9637d848749bc965316"),
    ("include/nvsdk_ngx_helpers.h", "5bcbadfe7478b802cf6d3aca4dc5ddd7d0889b99726e69c63f9e9bd555f44471"),
    ("include/nvsdk_ngx_helpers_vk.h", "c192bff72138f12f770db48e12c1d8f712dfc81ea0e8e055ebb27d8dd1b31623"),
    ("include/nvsdk_ngx_loader.h", "abdf5f66a692620538357d29fdf78e3e2a677ee8140b7a638962ecb766585e75"),
    ("include/nvsdk_ngx_params_dlssd.h", "d41e3ba8f1d72583b55273092c99bf426dc4a0d46de827229e25d5b1325a370e"),
    ("include/nvsdk_ngx_params_dlssg.h", "c24fa5aa68ffa2808c828b75cfa32843f1606f80a145f84055ac450d7e6c74ad"),
    ("include/nvsdk_ngx_params.h", "943bc8cc5cdae03b6303016fbad3183636f2335ae27a2d18776798c3b4efabbc"),
    ("include/nvsdk_ngx_standalone_common.h", "38413c3338b0aea0202c07da95d426bfee7080a42913823fa2d9dd585b087b53"),
    ("include/nvsdk_ngx_standalone_cuda.h", "b23436ed5040051ac8e81554c9af080f2463c941435bbc3b82222e5431a09499"),
    ("include/nvsdk_ngx_vk.h", "2d364ce7132881eb669e9498fd570d74cba563b1fbebc82c235f8e1ad2dd8b6d"),
]
# each platform's library (into lib/), DLSS's runtime and DLSS Frame Generation's (both into runtime/, the game's ngx)
PLATFORMS = {
    "linux": {
        "library": ("lib/Linux_x86_64/libnvsdk_ngx.a", "d3c50bb0d9624fc35e502a8e929b8fc1e2b505017ebdb84c0d5b627e11a235df"),
        "runtime": ("lib/Linux_x86_64/rel/libnvidia-ngx-dlss.so.310.9.16",
                    "30982c937afed85c96b8d313323de9a650a2eec6de1bb0f714b21750f4ce98f6"),
        "framegen": ("lib/Linux_x86_64/rel/libnvidia-ngx-dlssg.so.310.9.16",
                     "574839cae01c835de4d5845c36ca9cf75361f8160bd52397142c36a24e29e6f7"),
    },
    "windows": {
        # (the static NGX loader built against the static C runtime (/MT); llvm-mingw links it with gpu/vk/vk_ngx_msvc_rt.cpp)
        "library": ("lib/Windows_x86_64/x64/nvsdk_ngx_s.lib", "7b9a5239c1145af5f645ab472aa066f9ea79db36ac819562312aae69d65f3765"),
        "runtime": ("lib/Windows_x86_64/rel/nvngx_dlss.dll", "d00b5b0ab29c3ca101b70c3a5ecb3a01b7ef33766a3383a28a18623083e6123e"),
        "framegen": ("lib/Windows_x86_64/rel/nvngx_dlssg.dll",
                     "8d59d49b36410dd6b62fef2503a5871d347415bd85d943fa0848563a95dd50c7"),
    },
}
RUNTIMES = ("runtime", "framegen")   # (DLSS's runtime, then DLSS Frame Generation's)

NOTICE = """\
NVIDIA DLSS: this downloads NVIDIA's DLSS SDK (release v310.9.16) from github.com/NVIDIA/DLSS.
NVIDIA's files are NVIDIA's, not Hookshot's, and are under NVIDIA's own license, the NVIDIA RTX SDKs license:
  %s
(saved as LICENSE.txt beside the files). Using them means accepting it. Hookshot never includes or redistributes them:
they stay on this PC, in the SDK folder and the game folder's ngx folder. Never share a game folder built with them.""" % LICENSE_URL


def host_platform():
    """The platform a build here is for: Windows on Windows or with RX_TARGET=windows (a cross build), else Linux"""
    return "windows" if os.name == "nt" or os.environ.get("RX_TARGET", "").lower() == "windows" else "linux"


def destination(path):
    """Where a file of NVIDIA's repository goes in the SDK folder (a relative path)"""
    if path.startswith("lib/"):
        return ("runtime/" if "/rel/" in path else "lib/") + path.rsplit("/", 1)[1]
    return path


def files(platform, runtime=True):
    """[(path in NVIDIA's repository, SHA-256, path in the SDK folder)] for one platform; runtime=False: what a build reads only
    (not the runtimes)"""
    spec = PLATFORMS[platform]
    return [(p, s, destination(p)) for p, s in COMMON + [spec["library"]] + ([spec[k] for k in RUNTIMES] if runtime else [])]


def library(sdk, platform):
    """The library a build with this SDK folder links, for that platform"""
    return os.path.join(sdk, *destination(PLATFORMS[platform]["library"][0]).split("/"))


def runtime(sdk, platform, kind="runtime"):
    """A runtime in this SDK folder, for that platform: DLSS's ("runtime") or DLSS Frame Generation's ("framegen")"""
    return os.path.join(sdk, *destination(PLATFORMS[platform][kind][0]).split("/"))


def runtime_name(platform, kind="runtime"):
    return PLATFORMS[platform][kind][0].rsplit("/", 1)[1]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


TIMEOUT = 60   # (seconds without an answer, or between two reads, before a download gives up: a stalled one ends the build)


def fetch_one(path, digest, dst, opener=urllib.request.urlopen):
    """One file into dst, unless it is there already with that SHA-256. The download goes to dst.part and is moved in only
    when its SHA-256 is the expected one; otherwise it is deleted and ValueError says which"""
    if os.path.isfile(dst) and sha256(dst) == digest:
        return False
    os.makedirs(os.path.dirname(dst) or ".", exist_ok=True)
    tmp = dst + ".part"
    try:
        with opener(BASE + "/" + path, timeout=TIMEOUT) as r, open(tmp, "wb") as f:
            shutil.copyfileobj(r, f, 1 << 20)
        if sha256(tmp) != digest:
            raise ValueError("ngx-sdk: %s is not the file expected (SHA-256): not used" % path)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return True


def fetch(sdk, platform, opener=urllib.request.urlopen, say=print, runtime=True):
    """Every file of that platform into the SDK folder (runtime=False: not the runtimes, DLSS's and frame generation's)"""
    for path, digest, rel in files(platform, runtime):
        if fetch_one(path, digest, os.path.join(sdk, *rel.split("/")), opener):
            say("ngx-sdk: %s" % rel)


def install_runtime(sdk, platform, folder):
    """The SDK folder's DLSS runtime copied into folder (made when missing), where the game finds it, and DLSS Frame
    Generation's beside it when the SDK folder has it (one fetched before it was in the table has not: fetch() adds it).
    Returns DLSS's runtime there"""
    os.makedirs(folder, exist_ok=True)
    placed = []
    for kind in RUNTIMES:
        src = runtime(sdk, platform, kind)
        dst = os.path.join(folder, runtime_name(platform, kind))
        if kind != "runtime" and not os.path.isfile(src):
            continue
        if not (os.path.isfile(dst) and os.path.getsize(dst) == os.path.getsize(src) and sha256(dst) == sha256(src)):
            shutil.copyfile(src, dst + ".part")
            os.replace(dst + ".part", dst)
        placed.append(dst)
    return placed[0]


def accepted(flag, ask=input, interactive=None):
    """NVIDIA's license, accepted: the flag, or a typed yes when there is someone to ask"""
    if flag:
        return True
    if not (sys.stdin.isatty() if interactive is None else interactive):
        return False
    try:
        return ask("Do you accept NVIDIA's license? Type yes to go on: ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("sdk", nargs="?")
    p.add_argument("--platform", choices=sorted(PLATFORMS), default=host_platform())
    p.add_argument("--game", help="a game folder: the DLSS runtime into its ngx folder")
    p.add_argument("--runtime-dir", help="the DLSS runtime into this folder")
    p.add_argument("--accept-nvidia-license", action="store_true")
    p.add_argument("--build-only", action="store_true", help="not DLSS's runtime: only what the build reads")
    a = p.parse_args(argv)
    if a.build_only and (a.game or a.runtime_dir):
        p.error("--build-only fetches no runtime for --game or --runtime-dir")
    sdk = os.path.abspath(a.sdk or os.path.join(ROOT, "build-linux" if a.platform == "linux" else "work", "ngx-sdk"))
    print(NOTICE, flush=True)
    if not accepted(a.accept_nvidia_license):
        print("ngx-sdk: NVIDIA's license was not accepted (--accept-nvidia-license, or yes when asked): nothing downloaded",
              file=sys.stderr, flush=True)
        return 2
    try:
        fetch(sdk, a.platform, runtime=not a.build_only)
    except (OSError, ValueError) as e:
        print("ngx-sdk: %s" % e, file=sys.stderr, flush=True)
        return 1
    print("ngx-sdk: the SDK's headers and library in %s (NVIDIA's license: %s)" % (sdk, os.path.join(sdk, "LICENSE.txt")),
          flush=True)
    for folder in ([os.path.join(a.game, "ngx")] if a.game else []) + ([a.runtime_dir] if a.runtime_dir else []):
        print("ngx-sdk: the DLSS runtime in %s" % install_runtime(sdk, a.platform, folder), flush=True)
        print("ngx-sdk: DLSS Frame Generation's in %s" % os.path.join(folder, runtime_name(a.platform, "framegen")), flush=True)
    print("ngx-sdk: build with HOOKSHOT_NGX_SDK=%s" % sdk, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
