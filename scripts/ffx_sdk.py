"""AMD's FSR runtime for Vulkan (FSR SDK 2.4: FSR 4.1.2), for AMD FSR 4 in the Upscaler row (gpu/vk/vk_fsr4.cpp).

AMD's files are not part of Hookshot and are not committed: they are AMD's, signed by AMD, under AMD's FSR SDK license
(the SDK's docs/license.md, fetched beside them as LICENSE-AMD-FSR-SDK.md). This fetches them from AMD's FidelityFX SDK
repository at one pinned release, each file checked against its SHA-256, into <folder>:
  - amd_fidelityfx_loader_vk.dll (the loader the game calls) and amd_fidelityfx_upscaler_vk.dll (FSR 4 and FSR 3.1);
  - LICENSE-AMD-FSR-SDK.md.
gpu/vk/vk_fsr4.cpp looks for them in RX_VK_FSR_DIR, beside the executable, then in the game folder. Windows only: AMD
publishes the runtime for Windows alone (Linux takes a Vulkan FSR 4 library of its own: the readme).

Usage:  python -I scripts/ffx_sdk.py [--accept-amd-license] [--game <game folder>] [<folder>]
  <folder>: default work/ffx-sdk
  --accept-amd-license: AMD's license, accepted (without it: asked, and nothing is downloaded without a yes)
  --game: the runtime copied into <game folder> too (nx2/make_game.py --fetch-fsr-runtime does the same for the folder it
          builds)
"""
import argparse
import hashlib
import os
import shutil
import sys
import urllib.request

COMMIT = "7539ae8b6568ad29ed78a753404351daf3b5cca7"   # (tag v2.4.0: FSR 4.1.2, FSR 3.1.6)
BASE = "https://raw.githubusercontent.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/" + COMMIT
# AMD's license for these files, at the pinned commit (it grants installing, copying and redistributing them in binary form,
# with its notices kept and no reverse engineering). It asks for no acceptance step; one is asked all the same, as for
# NVIDIA's (scripts/ngx_sdk.py): one rule for both, and nobody gets AMD's files without having seen the terms
LICENSE_URL = ("https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/blob/" + COMMIT +
               "/Kits/FidelityFX/docs/license.md")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# (path in AMD's repository, its SHA-256, its name here)
FILES = [
    ("Kits/FidelityFX/signedbin/amd_fidelityfx_loader_vk.dll",
     "38a56a01fb3565c0757d9f5c1e4e69de4b65e5f2e5769b323b200dc1f142bab9", "amd_fidelityfx_loader_vk.dll"),
    ("Kits/FidelityFX/signedbin/amd_fidelityfx_upscaler_vk.dll",
     "4a7e895af6e0b615d40a84d59eb8c2b1054ad91cdd0c739567940270d51bdab7", "amd_fidelityfx_upscaler_vk.dll"),
    ("Kits/FidelityFX/docs/license.md",
     "32357259b965d6a8001e9e508ba60eda6034027c5923a7d226ffbaff813c2bfb", "LICENSE-AMD-FSR-SDK.md"),
]
RUNTIME = [f[2] for f in FILES]
DLLS = [f[2] for f in FILES if f[2].endswith(".dll")]

NOTICE = """\
AMD FSR 4: this downloads AMD's FSR runtime for Vulkan (FSR SDK v2.4.0: FSR 4.1.2) from
github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK. AMD's files are AMD's, not Hookshot's, signed by AMD and under AMD's
FSR SDK license (saved as LICENSE-AMD-FSR-SDK.md beside them):
  """ + LICENSE_URL

TIMEOUT = 60   # (seconds without an answer, or between two reads, before a download gives up)


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


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
            raise ValueError("ffx-sdk: %s is not the file expected (SHA-256): not used" % path)
        os.replace(tmp, dst)
    finally:
        if os.path.exists(tmp):
            os.remove(tmp)
    return True


def fetch(folder, opener=urllib.request.urlopen, say=print, tries=3):
    """Every file into folder (a download cut off is tried again, up to tries times)"""
    for path, digest, name in FILES:
        for attempt in range(tries):
            try:
                if fetch_one(path, digest, os.path.join(folder, name), opener):
                    say("ffx-sdk: %s" % name)
                break
            except OSError:
                if attempt + 1 == tries:
                    raise


def complete(folder):
    """Whether folder holds the runtime, each file the one expected"""
    return all(os.path.isfile(os.path.join(folder, n)) and sha256(os.path.join(folder, n)) == d for _, d, n in FILES)


def others(game_ffx):
    """AMD DLLs in game_ffx that are not the pinned ones (a newer SDK the player put there by hand, say)"""
    digests = {name: digest for _, digest, name in FILES}
    return [n for n in DLLS
            if os.path.isfile(os.path.join(game_ffx, n)) and sha256(os.path.join(game_ffx, n)) != digests[n]]


def install(folder, game_ffx, say=print):
    """The runtime copied from folder into game_ffx (made when missing): where the game finds it (the game folder). AMD's
    DLLs the player put there already, other than the pinned ones, are left alone, both of them (gpu/vk/vk_fsr4.cpp takes
    the newest FSR 4 the loader lists, so replacing them could be a downgrade), and say tells which. Returns game_ffx"""
    os.makedirs(game_ffx, exist_ok=True)
    kept = others(game_ffx)
    if kept:
        say("ffx-sdk: %s in %s %s not FSR SDK v2.4.0's: left as %s (delete %s to get v2.4.0's)" % (
            " and ".join(kept), game_ffx, "is" if len(kept) == 1 else "are", "it is" if len(kept) == 1 else "they are",
            "it" if len(kept) == 1 else "them"))
        return game_ffx
    for _, digest, name in FILES:
        dst = os.path.join(game_ffx, name)
        if not (os.path.isfile(dst) and sha256(dst) == digest):
            shutil.copyfile(os.path.join(folder, name), dst + ".part")
            os.replace(dst + ".part", dst)
    return game_ffx


def accepted(flag, ask=input, interactive=None):
    """AMD's license, accepted: the flag, or a typed yes when there is someone to ask (as scripts/ngx_sdk.py's)"""
    if flag:
        return True
    if not (sys.stdin.isatty() if interactive is None else interactive):
        return False
    try:
        return ask("Do you accept AMD's license? Type yes to go on: ").strip().lower() in ("y", "yes")
    except EOFError:
        return False


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    p.add_argument("folder", nargs="?")
    p.add_argument("--game", help="a game folder: the runtime into it")
    p.add_argument("--accept-amd-license", action="store_true")
    a = p.parse_args(argv)
    folder = os.path.abspath(a.folder or os.path.join(ROOT, "work", "ffx-sdk"))
    print(NOTICE, flush=True)
    if not accepted(a.accept_amd_license):
        print("ffx-sdk: AMD's license was not accepted (--accept-amd-license, or yes when asked): nothing downloaded",
              file=sys.stderr, flush=True)
        return 2
    try:
        fetch(folder)
    except (OSError, ValueError) as e:
        print("ffx-sdk: %s" % e, file=sys.stderr, flush=True)
        return 1
    print("ffx-sdk: AMD's FSR runtime in %s" % folder, flush=True)
    if a.game:
        print("ffx-sdk: copied into %s" % install(folder, a.game), flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
