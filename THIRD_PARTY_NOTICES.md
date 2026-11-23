# Third-party notices

The code written for this port is copyright (c) 2026 aks796:
- **`vkdk/`** (outside `vkdk/uam/`, `vkdk/spirv_cross/` and `vkdk/vulkan-headers/`): GPL-3.0 ([LICENSE](LICENSE)).
- **`source/`** (the files that say so in their header) **and `tools/`:** MIT ([source/LICENSE](source/LICENSE)).

The parts below come from other projects and keep their own licences.

## The game

Sonic Dream Team (SEGA, developed by HARDlight) and Project Dream (Beatz) are not part of this repository. The player
makes the game's Android build from their own copy with Project Dream. Sonic the Hedgehog and related characters are
trademarks of SEGA.

The button icons in `source/glyphs_data.c` (L, R, + and -) are the game's own LB, RB, Menu and View icons, redrawn by
`tools/switch_glyphs.py`. The NRO icon (`projectdream_nx.jpg`) is made from the game's logo. Both are SEGA's art.

## The builder (`tools/builder/`)

`projectdream_builder` loads the player's own `Project_Dream.exe` and calls Project Dream's conversion code in it (the
shader conversion, the Addressables checksums, the GameAssembly and libunity patches, with Project Dream's own copy of
AssetsTools.NET from the exe); none of that code is in this repository or in the builder. Unity's Android player is
downloaded from Unity by the player's builder, from the address in their `Project_Dream.exe`. Its release builds are
self-contained: they carry the .NET runtime (Microsoft, MIT) and System.IO.Hashing (Microsoft, MIT).

## Loader (`source/`)

| Code | From | Licence |
|---|---|---|
| The ELF loader, its JNI/libc layers and the files that say so in their header (`so_util.c`, `util.c`, `error.c`, `config.c` ...) | so-loader by Andy Nguyen (TheFloW) and fgsfds, through GoatHonks' fork | MIT: `source/LICENSE` |
| `pthr.c`, `pthr.h` | gm666q's [lswtcs-vita](https://github.com/gm666q/lswtcs-vita) (`reimpl/pthr.c`), through earlier Switch ports | MIT |
| `saltynx.c` | StevensND's [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) SaltyNX template (`extras/saltynx-template`), ported to C | GPL-3.0 |

## Renderer (`vkdk/`)

vkdk, the Vulkan subset the game draws with, implemented on deko3d, is this port's author's own (GPL-3.0). It builds
these with it:

| Code | From | Licence |
|---|---|---|
| `vkdk/uam/` | [uam](https://github.com/devkitPro/uam) 1.1.0 by devkitPro (fincs); its `mesa-imported/` part from Mesa | zlib for uam's own files, MIT for `mesa-imported/` (`vkdk/uam/LICENSE`) |
| The changes marked `SwitchWakerHD patch N` in `vkdk/uam/` (listed in `vkdk/uam/PATCHES.md`) | The SwitchWakerHD port | MPL-2.0 (for those changes) |
| `vkdk/spirv_cross/` | [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross) by Arm Limited and the Khronos Group | Apache-2.0 or MIT |
| `vkdk/vulkan-headers/` | The Khronos Group's [Vulkan headers](https://github.com/KhronosGroup/Vulkan-Headers) | Apache-2.0 |

The MPL-2.0 covers the changes those patches make. The files they touch stay under the MPL-2.0 for those changes, and
their source is `vkdk/uam/`.

## Libraries linked into the NRO

Not in this repository; they come from devkitPro's packages.

| Library | Licence |
|---|---|
| [libnx](https://github.com/switchbrew/libnx) | ISC |
| [deko3d](https://github.com/devkitPro/deko3d) | zlib |
| FFmpeg (`switch-ffmpeg`, built with `--enable-gpl`) | GPL |
| dav1d (`switch-dav1d`) | BSD-2-Clause |
| bzip2 (`switch-bzip2`) | bzip2 licence |
| zlib (`switch-zlib`) | zlib |
| newlib and libstdc++ (devkitA64) | their own (BSD-style; GPL with the GCC Runtime Library Exception) |

## Why the NRO is GPL-3.0

`vkdk/` and `source/saltynx.c` are GPL-3.0, and devkitPro's FFmpeg is a GPL build, so `projectdream_nx.nro` as a
whole is distributed under the GPL-3.0. The MIT-licensed files can still be reused under the MIT licence.
