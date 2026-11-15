"""What the shader caches are good for: the shader translator's sources, and the stamp made of them.

nx2/build.py compiles the stamp into the renderer (HOOKSHOT_SHADER_STAMP), where translator_stamp (gpu/vk/vk_shader.cpp) folds
in what the translator is told of the GPU and the environment, and the sizes and C++ library of the records. The program cache
(hookshot_programs.bin), the prepare record and the Vulkan pipeline cache files are dropped whole when it changes, and every
player translates every program again at the next start, so the stamp holds the sources that can change a translated program:

- gpu/vk/eden, gpu/vk/sirit and gpu/vk/compat, every file: Eden's recompiler, the SPIR-V emitter and what they are built with;
- vk_shader.cpp: what drives them (the environment they read, the passes, the program keys and the answers kept with them);
- vk_bank.cpp: the driver constant banks c0 and c2 the translation reads constants from;
- vk_state.h: NvnState, which those two read (its arrays and its defaults: the seeds are translated from a blank one).

Not vk_api.h. The translator reaches the host through MkwVkHost's gpu_read, gpu_guest, guest_base and guest_size, which carry the
game's memory, and depth_minus_w, whose answer translator_stamp holds itself: what they answer is the host's code (host/), which
the stamp never held, and a change to how they are called changes vk_shader.cpp or vk_bank.cpp. The NVN types NvnState is made of
are gpu/render/rx_nvn.h's, which vk_api.h includes but which the stamp never held either. The rest of vk_api.h (MKWVK_API_VERSION,
the menu's option ids, the controls, the prompts, MkwVkRenderer) the translator does not use, and the menu, input and audio rows
added there made every player translate every program again.

Files are stamped with Windows line ends as Unix ones, otherwise byte for byte, comments included. scripts/dev_packs.py prepares
a runtime again by the same files (stamped), and nx2/bake.py, nx2/bake_mimo.py and nx2/bake_ds/bake_all.py stamp their banks
with stamp().
"""
import hashlib
import os

DIRS = ("eden", "sirit", "compat")                       # (gpu/vk/<dir>/: every file below)
FILES = ("vk_shader.cpp", "vk_bank.cpp", "vk_state.h")   # (gpu/vk/<file>: after the folders' files, in this order)


def files(vk):
    """The stamped files below gpu/vk (vk), in the stamp's order: the folders' files sorted, then FILES as listed (appended,
    not sorted in: docs/BAKE.md's stamp trap)"""
    found = [os.path.join(d, f) for top in DIRS for d, _, fs in os.walk(os.path.join(vk, top)) for f in fs]
    return sorted(found) + [os.path.join(vk, n) for n in FILES]


def stamped(rel):
    """Whether a path relative to the checkout (gpu/vk/..., / separated) is one of the stamp's"""
    return rel.startswith(tuple("gpu/vk/%s/" % d for d in DIRS)) or rel in tuple("gpu/vk/" + f for f in FILES)


def stamp(vk):
    """HOOKSHOT_SHADER_STAMP's 16 hex digits for the translator below gpu/vk (vk)"""
    h = hashlib.sha1(usedforsecurity=False)
    for f in files(vk):
        with open(f, "rb") as src:
            h.update(os.path.relpath(f, vk).replace("\\", "/").encode() + src.read().replace(b"\r\n", b"\n"))
    return h.hexdigest()[:16]
