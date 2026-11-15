zstd 1.5.7, from https://github.com/facebook/zstd (release v1.5.7, zstd-1.5.7.tar.gz, sha256
eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3), unmodified: lib/zstd.h, lib/zstd_errors.h,
lib/zdict.h, lib/common/ and lib/decompress/. BSD license (LICENSE; zstd is dual BSD or GPLv2, used here under BSD).

The decoder only, built with ZSTD_DISABLE_ASM (nx2/build.py). Used by gpu/vk/vk_imgui.cpp to read the game's own menu font
from the player's game files at run time (romfs Font2, with the dictionary in Pack/ZsDic.pack.zs), in memory, and by
host/prompts.c to read a mod's resource size table and the button prompts' own, to merge them.

upstream: https://github.com/facebook/zstd/releases/download/v1.5.7/zstd-1.5.7.tar.gz
upstream-sha256: eb33e51f49a15e023950cd7825ca74a4a2b43db8354825ac24fc1b7ee09e6fa3
upstream-root: zstd-1.5.7

tree-sha256: 456191855c64204ae12b4b13f680014d2917fb41ce51f1ec4cf95e09cc7dd7e7
