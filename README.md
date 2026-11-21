# projectdream_nx

**Sonic Dream Team on the Nintendo Switch.** A homebrew loader that runs the game's Android build, the one
[Project Dream](https://archive.org/details/project_dream) makes from the macOS version, natively on a Switch with
custom firmware. It draws with deko3d (the Switch's own graphics API) and has HD Rumble and the Switch's button icons.

It contains **no game code and no game data**. You need your own copy of the game.

## What you need

- A Switch with Atmosphère.
- Your own copy of **Sonic Dream Team 1.10.1 for macOS**.
- **Project Dream** by Beatz, a Windows tool. Its GitLab page is gone. A mirror is on the Internet Archive:
  [archive.org/details/project_dream](https://archive.org/details/project_dream).
- About 2.5 GB free on the SD card.

## Install

1. On a PC, open Project Dream, point it at the macOS game and make the **Android** build. You get an `.apk`.
2. Make a folder `sdmc:/switch/projectdream_nx/` and copy into it:
   - `projectdream_nx.nro` (from the [release](../../releases)),
   - the `.apk` from step 1 (any name; `sdt.apk` is fine).
3. Start it with **title override**: hold **R** while starting any installed game, then pick
   *Sonic Dream Team* in the homebrew menu. The album (applet mode) does not have enough memory.

The first start extracts the game's libraries from the APK, which takes a little while. Shaders are compiled the
first time they are needed and kept on the SD card, so the first play of each area can stutter briefly.

Saves and settings stay in the same folder (`data/`, `ext/`, `config.txt`).

## Settings

The game's own Settings menu (Project Dream's) has the Switch-specific options:

| Option | What it does |
|---|---|
| **Resolution Scale** | How sharp the 3D picture is, 0.50-1.00. |
| **Frame Rate** | **30**, **60**, or **Auto**: 60, and an even 30 whenever 60 is out of reach. A 60 Hz screen only shows these two smoothly. |
| **Dynamic Resolution** | Lowers the resolution only while the GPU is the limit (measured on the GPU itself), never below 0.70 and never above Resolution Scale. |
| **Vibration** | HD Rumble for hits, a life lost, striking enemies, boosts, switching characters and the goal. |

`config.txt` next to the NRO has more, each explained in the file. For example, `rumble` sets the HD Rumble
strength in percent, and `fixed_step 0.02` runs physics at 50 Hz for a little more speed.

## Performance

Measured on one console, handheld, 720p:

- **Stock clocks** (CPU 1020 MHz, GPU 460 MHz): **30** in levels (Frame Rate Auto or 30), with an occasional
  dropped frame in the heaviest spots.
- **60 fps** needs an overclocked CPU (about 1785 MHz). At GPU 460 MHz, a Resolution Scale near 0.70 (or Dynamic
  Resolution) keeps the heavier areas near 60.
- The main thread is the limit: the game's own code. A forwarder that allows a **fourth CPU core** gives the engine
  one more worker thread, which the port uses automatically. (In sphaira: select the NRO, Options, Install
  Forwarder, CPU Cores 4.)

## Building

Needs devkitPro with devkitA64, libnx, deko3d and the `switch-ffmpeg`, `switch-dav1d`, `switch-bzip2` and
`switch-zlib` portlibs, plus CMake and Ninja for vkdk:

```sh
tools/build_vkdk.sh   # vkdk: Vulkan on deko3d (build_vkdk/)
make                  # projectdream_nx.nro
```

`PLAN.md` is the development log: how Project Dream's Android build works, what the port does at each step, and
every hardware round's findings.

## Credits

- **Sonic Dream Team** © SEGA, developed by HARDlight.
- **Project Dream** by Beatz: the Android build of the game and its loader, which this port runs.
- The Switch `.so` loader lineage: TheFloW (Andy Nguyen), fgsfds, GoatHonks.
- [deko3d](https://github.com/devkitPro/deko3d) by fincs, and its UAM shader compiler (based on Mesa).
- [SPIRV-Cross](https://github.com/KhronosGroup/SPIRV-Cross), the Vulkan headers, FFmpeg, libnx and devkitPro.
- Thorhax's [SonicDreamTeam-NX](https://github.com/Thorhax/SonicDreamTeam-NX) (MIT), whose measurements suggested the
  engine thread setup used here.

## Disclaimer

Not affiliated with or endorsed by SEGA, HARDlight, Nintendo, Unity or Apple. This repository contains none of the
game; use it only with a copy you own.

## License

MIT (`LICENSE`). Third-party parts keep their own licenses (`vkdk/uam/LICENSE`, `vkdk/spirv_cross/`, the Vulkan
headers). The button icons in `source/glyphs_data.c` are remade from the game's own icons.
