"""The game's Windows manifest, as a .res the exe links in (nx2/build.py): UTF-8 as the process's code page.

host/ and gpu/vk keep paths as UTF-8 char strings and call Windows' A functions (GetModuleFileNameA, CreateFileA, fopen ...)
with them. Without a manifest those take and give the system's ANSI code page (1252 on a Western Windows), and a game folder
whose name it cannot spell (Japanese on a Western Windows, an emoji anywhere) came back as "?": the game found neither
image.bin nor its log's folder and closed with nothing on screen. activeCodePage UTF-8 (Windows 10 1903 and newer) makes
every A function and the C runtime's narrow file functions UTF-8 for the whole process, its DLLs too (hookshot_vk.dll,
SDL2, FFmpeg). host/hos_main.c says so in a message box when the Windows is too old for it and the folder's name needs it.

heapType SegmentHeap (Windows 10 2004 and newer; older ones ignore it): the process heap is the segment heap, not the NT heap.
The shader preparation's workers compile in the graphics driver, and AMD's compiler allocates and frees through the process heap
from all of them at once: on the NT heap they queued on its one lock, and more workers ran slower. gpu/vk/prepare_workers.h has
the measurements, and gives the first part of the preparation more workers when this heap is the one in use.

    python nx2/win_manifest.py [out.res]
"""
import os
import struct
import sys

MANIFEST = b"""<?xml version="1.0" encoding="UTF-8" standalone="yes"?>
<assembly xmlns="urn:schemas-microsoft-com:asm.v1" manifestVersion="1.0">
  <assemblyIdentity type="win32" name="Hookshot.Game" version="1.0.0.0"/>
  <application xmlns="urn:schemas-microsoft-com:asm.v3">
    <windowsSettings>
      <activeCodePage xmlns="http://schemas.microsoft.com/SMI/2019/WindowsSettings">UTF-8</activeCodePage>
      <heapType xmlns="http://schemas.microsoft.com/SMI/2020/WindowsSettings">SegmentHeap</heapType>
    </windowsSettings>
  </application>
  <trustInfo xmlns="urn:schemas-microsoft-com:asm.v3">
    <security><requestedPrivileges><requestedExecutionLevel level="asInvoker" uiAccess="false"/></requestedPrivileges></security>
  </trustInfo>
</assembly>
"""

RT_MANIFEST = 24
CREATEPROCESS_MANIFEST_RESOURCE_ID = 1   # (the one an exe's loader reads)


def res_entry(kind, name, data, flags=0x1030, language=0x0409):
    """one resource of a .res file (the format nx2/make_icon.py writes too): sizes, numeric type and name, flags, language"""
    header = struct.pack("<II", len(data), 32) + struct.pack("<HHHH", 0xFFFF, kind, 0xFFFF, name)
    header += struct.pack("<IHHII", 0, flags, language, 0, 0)
    return header + data + b"\0" * (-len(data) & 3)


def res_bytes(manifest=MANIFEST):
    return res_entry(0, 0, b"", 0, 0) + res_entry(RT_MANIFEST, CREATEPROCESS_MANIFEST_RESOURCE_ID, manifest)


def write(path):
    """hookshot_manifest.res at path, rewritten only when its bytes change (the link reads it each time)"""
    data = res_bytes()
    try:
        with open(path, "rb") as f:
            if f.read() == data:
                return path
    except OSError:
        pass
    with open(path, "wb") as f:
        f.write(data)
    return path


if __name__ == "__main__":
    print(write(sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.getcwd(), "hookshot_manifest.res")))
