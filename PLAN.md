# sdt_nx — Sonic Dream Team (Project Dream) on the Switch

## What we have

- `../Sonic-Dream-Team-1.10.1-macOS-universal.zip`: the Apple Arcade macOS build. Unity **2022.3.54f1**, IL2CPP,
  URP. `Contents/Frameworks/GameAssembly.dylib` is a universal Mach-O (x86_64 + arm64, **not stripped**:
  symbols for every C# method). Native plug-ins: Burst (`lib_burst_generated.bundle`, not needed: Burst falls back
  to the IL2CPP code), Apple's GameController/GameKit wrappers, the developer's `HLMacCore` and `FPBundleWrapper`.
  Textures are desktop BC formats (the Tegra X1 samples them natively), shaders are Metal only.
- `../project_dream/Project_Dream.exe`: "Project Dream" (Beatz, from a GitLab repo that was taken down), a .NET 6
  WinForms tool that turns the Mac build into a Windows build (D3D11/Vulkan) or an **Android build (Vulkan only)**.
  Decompiled with ILSpy 9.1 (Docker) into `../analysis/decomp/proj` (sources + embedded resources).

## How Project Dream's Android build works (the part we reuse)

- `libunity.so` / `libmain.so` from Unity's own Android player 2022.3.54f1 (downloaded from Unity; here streamed
  out of the pkg into `../unity_android`, with `libunity.sym.so`). Two patches in
  `SerializedFile::{CompareAgainstNewType, ReadMetadata}` (`ldr w1,[sp,#0xd8]` -> `mov w1,#13`) make the
  StandaloneOSX asset bundles read as Android ones.
- `lib/arm64-v8a/libil2cpp.so` is **Project Dream's own loader** (prebuilt, not stripped, ~780 KB): it exports the
  241 `il2cpp_*` functions libunity calls, loads `assets/bin/Data/GameAssembly.arm64` (the thinned arm64 slice,
  37 code patches for touch/controller UI) as a Mach-O, binds its Darwin imports to bionic (`d_*`: libc, pthreads,
  errno/flag translation, variadic ABI), emulates objc/CoreFoundation/Foundation, GameController (`gc_*`, fed by
  `ControllerInput.publish` over JNI), GameKit, iCloud KVS (`hl_cloud_*`), NSUserDefaults (`kv_*`), C++ exceptions
  (libunwind), and applies its own game patches (`gp_*`: menus, graphics options, SMAA, shadows, touch skills) with
  `assets/gamepatches.bundle`. GC stop-the-world: Darwin `thread_suspend/get_state/resume` done with signals.
- Data: `globalgamemanagers` patched (Vulkan only, gpuSkinning off, androidBlitType 1, VideoDecodeAndroid shader),
  276 Shader objects in 81 files replaced from `Shaders.shader-android.bin` (LZMA, Vulkan SPIR-V subprograms),
  textures transcoded (we skip this: BC is native on the Tegra), Addressables catalog CRCs updated for the rewritten
  bundles, StreamingAssets copied to `assets/`.

## The Switch port

```
sdt_nx.nro (libnx)                          sdmc:/switch/sdt_nx/
 ├─ so_util (ELF linker) + bionic/libc shim  ├─ sdt_nx.nro
 ├─ JNI + Java emulation (UnityPlayer etc.)  ├─ sdt.apk   <- made by tools/prepare (stored zip, ~2.3 GB)
 ├─ NDK: ANativeWindow, AAsset, ALooper      ├─ config.txt
 ├─ audio (OpenSL ES / AAudio)               ├─ internal/  (extracted libs, saves, shader caches)
 ├─ vkdk = libvulkan.so (Vulkan on deko3d)   └─ debug.log
 └─ pd_shim: Switch versions of the loader's
    OS-specific parts
libunity.so (Unity Android 2022.3.54f1, -force-vulkan)
 └─ libil2cpp.so (Project Dream's loader) ── GameAssembly.arm64 (Mach-O, the Mac game's code)
```

### Pieces

1. **Host prep** (`tools/prepare/`): a .NET 8 console app (Docker `mcr.microsoft.com/dotnet/sdk:8.0`) built from the
   decompiled Project Dream sources it needs (AssetsTools.NET, ShaderRead, Addressables, VideoShader,
   PatchGlobalGameManagers, GameAssembly/libunity patches) + a Python packer that streams the Mac zip into
   `sdt.apk` (everything stored, 4 KB aligned), swapping in the ~85 converted files. No texture transcoding.
   The user supplies the Mac zip and Project_Dream.exe; nothing of the game or of Unity ships with the port.
2. **Loader** (`source/`): mkt_nx's generic core (Unity 2022.3 boot sequence, JNI/Java, bionic shim, so_util,
   apk/vfs, audio, pthreads, logging, crash, vsync) without MKT's game code. `/proc/self/maps` names `sdt.apk` (the
   loader finds the APK there), `/proc/self/cmdline` = `com.sonicdreamteam.android`, saves under
   `/data/data/com.sonicdreamteam.android/files/PCSaveData` -> `internal/data/...`.
3. **vkdk** (`vkdk/`, from Dekopon-32's copy: SPIR-V converted on the console with SPIRV-Cross, uam to DKSH,
   DKSH cache): add `VK_KHR_android_surface` (the ANativeWindow is the console's default window), every Vulkan 1.0
   entry point Unity loads (real where Unity uses it: indirect draws, fill/update buffer, resolve, subpasses,
   secondary command buffers; harmless elsewhere), the format/feature answers Unity's caps code wants.
4. **pd_shim** (`source/pd_shim.c`): hooks into the loader by its own symbol table (it is not stripped):
   - code memory: the Mach-O image, `code_alloc` and the hook trampolines come from code-capable memory
     (svcMapProcessCodeMemory, RW <-> RX with svcSetProcessMemoryPermission; RWX requests become RW while
     patching); Darwin `mmap` from the game stays ordinary memory;
   - GC: `d_thread_suspend/resume/get_state` with svcSetThreadActivity + svcGetThreadContext3 (as mkt_nx's gcstop);
   - controllers: HID -> `ControllerInput_publish` slots (Apple GCExtendedGamepad order), Haptics.play -> HD rumble.
5. **Frame**: 60 Hz target. Unity paced by the swapchain (FIFO, vsync); no clock pinning. Resolution:
   1280x720 handheld, docked 1920x1080 output with a lower render scale if needed (URP renderScale). Project Dream's
   own quality options (shadows, SMAA, post) default to the cheap side on the Switch.

### Phases

1. Prep tool -> `sdt.apk`; check it with Unity's own file layout.
2. Loader boots libunity -> `il2cpp_init` -> Project Dream's loader maps GameAssembly (Ryubing).
3. Vulkan: instance/device/swapchain -> first frame (Unity splash) -> menus -> a level.
4. Input, audio, saves, HOME/sleep.
5. Shader cache: dump the GLSL Unity's SPIR-V turns into, compile on the PC (uam), ship the prebuilt DKSH pack.
6. 60 fps work on hardware logs ([perf] lines: CPU threads, GPU time, draws).

## Risks

- vkdk is a subset made for BO2/Dekopon: Unity's Vulkan backend will hit missing pieces (found in Ryubing).
- Console shader compiles (~0.4 s each) until the prebuilt pack covers the variants.
- Video (VideoPlayer -> MediaCodec over Java): not on the Switch; first make it fail into Project Dream's
  `gp_video_error` path.
- CPU: the game's code ran on Apple cores; the A57s are much slower. Measure first.

## Where it stands (2026-11-18)

Ryubing (Docked, 1920x1080): boot -> sdt.apk -> libunity + Project Dream's loader -> GameAssembly mapped (119 MB
code memory) -> il2cpp -> gp patches (71) -> Vulkan on vkdk -> FMOD audio -> the title screen, ~59 fps with Unity's
render thread (`gfx_mt 1`), 26 draws a frame, stable for 90 s. Not on hardware yet. Build: `tools/build_vkdk.sh`,
`make`, `tools/package.sh` (SD_CARD/switch/sdt_nx; SD_CARD/debug is left alone).

Found on the way (each cost a crash):

- **sysinfo**: bionic's `struct sysinfo` is 112 bytes. A padded one overflowed `d_sysctl`'s stack (PC 0 / LR 0).
- **-mtp=soft**: vkdk's CMake flags must keep it (`tools/build_vkdk.sh`). Without it uam's `thread_local` read
  TPIDR_EL0, a small fake TLS block on the port's threads, and every shader compile wrote past it into the heap
  (Unity/il2cpp hash tables corrupted later, far from the cause).
- **Freed device memory**: Unity frees memory blocks while the GPU (or Ryubing's texture cache) still uses them.
  vkdk now holds a destroyed DkMemBlock for 6 presents (`RetireBlocks`).
- **pread/pwrite** must be positional (FsFile at an offset), not lseek + read: Unity's loading threads share fds.
- **Code memory** (hardware, round 1: crash at launch): svcMapProcessCodeMemory pages are AliasCode; RW makes them
  AliasCodeData, which can never change permission again (0xd401): the loader's "make the image executable" failed
  and the first call into GameAssembly faulted. Ryubing ignores permissions, so it never showed. Now RW -> R/RX =
  unmap + map (AliasCode again, same bytes) + the permission; R/RX -> RW = the permission. And the region table was
  128 entries for ~220 regions (the loader's hook trampolines are a page each): 4096 now.
- **Bundles copied out of the APK** (hardware, round 2: black screen after the green bar): the loader's bundle_path
  copied each AssetBundle to files/bundles before Unity opened it -- 57 bundles in 45 s on the SD card, hundreds of
  MB to go; Ryubing's copies had been made long before. Now the path goes to Unity as
  `jar:file://<sdt.apk>!/assets/...`, which its APK file system reads in place (sdt.apk is all stored entries).
  The plain `<app>/base.apk!/...` form is not one Unity opens ("Unable to open archive file").
- GC stops log `at ?0` on hardware too: for a thread blocked in a system call the kernel gives only the callee-saved
  registers and sp, which is what the collector needs there.
- **Window size**: libnx forgets the NWindow size when a swapchain is destroyed; Unity's second swapchain then got
  1280x720 caps while laying out at 1080p (zoomed into the top left). vkdk calls the loader's
  `android_window_prepare` before surface caps and swapchains, and sets the window size and crop to each swapchain.
- Ryubing only: its hypervisor maps guest memory RWX (permission changes are no-ops), and paused threads' contexts
  can come back as zeros (PC 0) -- the GC copes; hardware will show whether stops are slower.

Since then (Ryubing, both modes): the intro movie, the title, the comic cutscenes (hold A skips a part, as the
game's HoldToSkipInput intends), the first level (run, jump, rings, camera, tutorials), the settings pages, docked
<-> handheld switching -- all at 58-60 fps.

- **Video** (`source/media_ndk.c`): libmediandk.so over FFmpeg (devkitPro's 7.1: mov demuxer, h264/aac decoders).
  Unity's Android VideoPlayer under Vulkan wants AImageReader hardware buffers unless
  `AndroidVulkanVideo::IsSupported()` says no (patched to say no: libunity+0x9d5be0); then it takes raw YUV 4:2:0
  buffers (color-format 19) and converts them with its own Hidden/VideoDecode (in Project Dream's shader pack).
  Videos: the intro (1920x1080 H.264 24 fps + AAC, 96 s), the What's New clip (850x850 60 fps) and the Apple Arcade
  splash, refused (it is read from resources.resource) so the game skips it as before. FFmpeg's threads: newlib
  pthreads sit on one core (libnx: priority 59, core -2), so `pthread_create` is wrapped (pthr.c) to give every
  thread the application cores. config `video_threads`, `video_fast` (skips deblocking non-reference frames).
- **Bundles**: `path_from_android` handed out a shared ring of 4 static buffers; Unity's loading threads open
  bundles at once, so now and then one opened another's path ("Unable to open archive file" on a bundle that is
  there, RemoteProviderException, a cutscene waiting half a minute). Per-thread buffers now.
- **Cores**: cpufeatures in libunity.so open()s /proc/cpuinfo, then /sys/devices/system/cpu/possible; without them
  Unity counted one core and made one job worker. internal/proc/cpuinfo (3 x Cortex-A57) and "0-2": two workers.
- **Docked/handheld**: the operation mode change sets the window (1080p/720p), the size vkdk reports for the surface
  (`vkdkSetSurfaceSize`), the DisplayMetrics, then UnityPlayer's resize (nativeRecreateGfxState, the surface event).
  vkdk's present says OUT_OF_DATE while the swapchain is the old size (SUBOPTIMAL is ignored by Unity; out of date
  at the acquire left Unity a frame with no command buffer -- a crash). test_script `dock 0|1` simulates it.
- **Shaders**: vkdk reads a plain cache file as its prebuilt set; `tools/merge_dksh_cache.py` merges caches into
  `../sdt_out/shaders.pack` (181 variants from the Ryubing sessions: title, intro, comics, first level, menus),
  which `tools/package.sh` puts on the SD card. Built from the game's shaders: the player's, never in the zip.
- "Desired shader compiler platform 18 is not available": Hidden/Universal/HDRDebugView (globalgamemanagers.assets
  #60), URP's HDR debug view, never drawn. Left alone.
- Saves go through Project Dream's iCloud key-value emulation (data/files/PCSaveData/icloud_kvs.txt).

Hardware round 3 (2026-11-18, playable): 20-40 fps in play with UnityMain busy 88-99% (the render thread 15-28%, cores
not full), loads very long, Exit froze. Then:
- **SD reads** (`source/fs_cache.c`): Unity reads sdt.apk mostly in small pieces (a level load: ~9000 reads in 10 s,
  80% under 16 KB; two small reads a frame in play), and Horizon has no file cache (renut's model of the console:
  ~2.3 ms a call + ~70 MB/s). fsFileRead is wrapped: files opened read-only are read in 64 KB blocks kept in memory
  (48 MB LRU, no lock held over a card read); reads of 128 KB and more go straight to the card. Ryubing: 3.5-4x
  fewer card reads during loads, ~1.5x the bytes. `[io]` lines count it all.
- **Exit**: Unity's own shutdown (nativeDone) waited forever in FMOD's Sound::release. Now: the engine's pause (its
  saves), fflush, svcExitProcess (a 4 s timer if the pause hangs).
- **vkdk**: destroyed memory blocks go after the present, a few a frame (renut: block destroys before the present
  were 15-37 ms spikes); uam's dual-issue peephole tests canDualIssue before the commutation walk (same DKSH, much
  of a fragment shader's compile time saved: fnxf_xiii's measurement via renut).
- **Measuring**: `[gc]` lines (the collector's stops of the world: how many, total, longest); `[prof]` names
  GameAssembly samples; config version 2 turns `profile` on; `tools/prof_names.py <debug.log>` names [prof] lines.
- renut_nx_ultimate_base (ChanseyIsTheBest; permission to reuse with credit) ideas kept for when the profile
  shows their target: a sequence-word condvar on svcWaitForAddress (a woken waiter no longer handed the mutex
  before it has a core), sched_yield backoff, a futex rwlock, a small-block allocator, a free list of emptied
  deko3d blocks, per-function hot-code rewrites of GameAssembly/libunity (only for functions >= 2% of UnityMain).
- The v1.10.3 "APKVISION" APK: another loader (com.sdt.port, Android clang 18 with PGO/BOLT/LTO, its own perf
  report) running the iOS build's UnityFramework (Mach-O, iOS 13) on the same Unity 2022.3.54f1 Android player.
  The iOS code has no ARMv8.1+ atomics, but the Mac code has only ~550 (Project Dream patches them), so that is no
  speed difference; Project Dream already reports the platform as iPhone (icall_platform -> 8), so the game takes
  its iPhone performance profile here too.

Hardware round 4 (CPU 1785 MHz, GPU 460 MHz): still 20-45 ms a frame, UnityMain 97-99% busy, GC a few ms per
10 s, reads fine. The profile is flat (libunity ~60%: transform-change dispatch, shader parameters, culling, LOD,
light probes; GameAssembly ~30%: dictionaries, the game's data containers; the port ~5%). So the work moves off the
main thread instead:
- **Native graphics jobs** (config `gfx_jobs` 2, on by default from config version 3): the render loop's work on the
  job workers. libunity's CalculateGfxDeviceThreadingMode returns "a render thread" as soon as `force-gfx-mt` is
  set, so that line goes when jobs are on; then `gfx-enable-gfx-jobs` + `gfx-enable-native-gfx-jobs` give mode 4
  (logged: "[unity] graphics threading mode"). Ryubing, the first level: UnityMain 33-36% -> 28%, the workers
  9 -> 15% each, the render thread 10 -> 3% (+ a Vulkan submission thread); the picture the same.
- **The main thread's own core** (config `main_core` 1): UnityMain stays on core 0 (libnx's priority 44), every
  other thread (game, Unity, FFmpeg, the port's) on cores 1-2, so no worker the main thread waits for starts behind
  it. `boot_extra` adds boot.config lines for experiments.
- `[perf]` now has the GPU fence waits and acquire time a frame: a fence wait that grows means the GPU is the limit.

Hardware round 5 (75% resolution, 1785/460 MHz): graphics jobs on (mode 4): UnityMain 72-81% busy (was 98%), the
workers 30-43% each, frames 19-27 ms (37-52 fps) in play; heavy scenes (~1000 draws) still ~55 ms, the main thread
computing ~33 ms of it, acquires 3-6 ms (the GPU near its limit too at 460 MHz).
- **Draw census** (vkdk `vkdkDrawCensus`, test_script `census N`): the first level's frame is 798 single draws in
  the colour pass + 88-104 in a 512x512 shadow map. Hundreds of small props, each drawn alone.
- **SRP Batcher**: off because Project Dream's loader overrides the game's GraphicsSettings.
  useScriptableRenderPipelineBatching setter to always pass false (icall_set_srp_batching). config `srp_batcher` 1
  (default from config version 4) hooks that override to pass the game's value: in the level 916 render nodes a
  frame batched, 16 refused; the picture the same in Ryubing. `[srp]` lines count it (unity_diag.c).
- **LOD bias** (config `lod_bias`, 0.6 by default from version 4; `shadow_distance` too): held by unity_tune_frame
  through QualitySettings' setters. 0.5: the first level's draws 942 -> 809, the picture all but the same.

Next: hardware logs ([perf], [video] decode times, shader compiles past the pack); the rest of the game's levels.

Hardware round 6 (build 202611190041): frames came in steps of 16.7, 33.3 and 50 ms, never between. Unity's
WaitVSync (libunity+0x9dac58) waits until the vsync counter passes last-present + n, so behind the FIFO swapchain a
frame 2 ms late lost a whole vsync (28-34% of UnityMain in that wait). Saves: the game rewrote its 92 KB save + backup
and Project Dream's 91 KB cloud copy many times in a row, up to 19% of the main thread in a window (open/write/close
on the card); the cloud copy itself never stored on the console (fsync = ENOSYS, its writer gave up). 30 shader
compiles on the console (median 286 ms, max 485 ms, 7.1 s in all) = the "random stutters".
Build 202611190128 (5f43a53):
- **Pacing** (unity_diag.c `wait_vsync_paced`): when the game asks for 60 (vSyncCount <= 1, targetFrameRate 60 or
  unset) and the last frame took over 17.5 ms, Unity's vsync wait is skipped and the frame presents at once (the
  swapchain still holds it to a vsync). config `vsync_wait` 1 = Unity's own. `[pacing]` line.
- **Saves** (save_writer.c): files under ext/ named *Save_ID* and PCSaveData/ are memory files (open/read/write/
  lseek/fstat/rename/fopen via fopencookie); close queues the bytes to a writer thread (cores 1-2) that writes
  `<path>.nxw`, then renames over the file. stat/rename/remove of a pending path wait for it; Exit flushes all.
  `[save]` line. Ryubing: save + backup 92548 bytes each, icloud_kvs.txt 91691, reload fine, no .nxw left.
- **fsync/fdatasync** flush the SD file (fsFileFlush) instead of ENOSYS.
- **Shaders**: the console's dksh.cache (156 variants) merged into shaders.pack (338 variants now).

Hardware round 7 (build 202611190128, 1785/460 MHz): the card was ~100x slower to write than in round 6 (0.5 s per
start-up log line where round 6 took 3-10 ms; up to 3 s a save; cause on the console side unknown). Frames of 5-32 s:
the main thread in sw_rename, waiting for the save write under way and then removing the temp name on the card itself
(the watchdog's stack: kv_put < rename_fake < sw_rename < _remove_r). Exit: the engine's pause stuck behind the same
waits, then svcExitProcess alone, which the console showed as a crash. From ~520 s (a level load) the game itself
asked for 30 fps (its FramerateConfiguration.Apply -> Application.targetFrameRate) for the rest of the session.
Build 202611190215 (8c5fa02):
- **save_writer**: nothing on the game's threads waits for the card: a rename of a pending save retargets it (the
  writer removes the old name); a closed save waits 2 s on the queue (rewrites replace it: Ryubing 38 game saves ->
  13 card writes); the card's file is rewritten in place (no new file + rename; the game keeps its own backup); at
  once while in the background (HOME/sleep: it may be closed there) and at Exit. `[save]` = game saves vs card writes.
- **Exit**: libnx's exit-process path without the rest of libnx's exit (`close_like_a_game`: __nx_applet_exit_mode 1,
  appletExit until the exit function is libnx's, then it: ISelfController::Exit / ExitProcessAndReturn); a 5 s
  backstop svcExitProcess (Ryubing only stubs Exit: the backstop ends it there); leave_now runs once.
- **log**: its writer thread from launch (no write + sync per start-up line).
- **fps_unlock** (config v5 default 1, v6 default 0 again: the 30 cap was the game's own 30 fps setting, the player's choice): the game's targetFrameRate under 55 set back to 60 the next frame.
  `[pacing] the game asks for ...` logs each change of its request.
Build 202611190219 (4895bdd): fps_unlock off by default (config v6): the 30 cap was the game's own 30 fps setting.

Hardware round 8 (build 202611190215, 1227 and 1785 MHz): no more freezes (saves 17-240 ms on the save thread). In
play the main thread 75-83% busy, Job.Worker 0/1 20-30%, render/submit threads under 10%: one thread is the limit,
so a fourth core (bo2-360-nx's way: a forwarder with 4 cores, chosen threads pinned to core 3) would gain little. The
flat profile: libunity ~50%, the game's code ~35%, the port + Project Dream's loader ~10% (waits for jobs ~5%,
Project Dream's il2cpp wrappers + pthread_once ~2%, Boehm's LSE atomics stubs ~3.8% = GC_end_stubborn_change etc.
from the write barriers of the incremental GC, emulated TLS ~0.5%). fixedDeltaTime 1/60 (maximumDeltaTime 0.333):
at 40 fps physics and FixedUpdate run ~1.5 times a frame.
Build 202611190245 (5b3c58a): pd_direct_api (Unity's 188 pointers to the loader's il2cpp_* wrappers -> the game's
functions), pthread_getspecific from native TLS, prof's inclusive report (32 callers a sample; Ryubing is too fast to
show it: the main thread waits there), [unity] frame settings line. Next: the inclusive profile from the console to
pick the game's own work to cut (fixed step count, shadows, UI, scripts).
Build 202611190314 (ebb7598): cuts of the game's work from outside. shadows / shadow_distance on the URP asset itself
(GraphicsSettings.currentRenderPipeline -> UniversalRenderPipelineAsset m_ShadowDistance at +168; the game's 50 m,
25 m by default from config v7, 0 = no shadow pass or caster culling); max_delta 0.1 (Time.maximumDeltaTime, the
game's 0.333). lod_bias stays 0.6 (0.4-0.5 = more cut, more pop-in).

Hardware round 9 (build 202611190314; 1224 MHz with the game's own 30 cap, then 1785 MHz ~47-48 fps in Act 3). The
inclusive profile in play: the game's managed code 53-56% (its TimeManager.PerformUpdate 15-19%, URP's C# render
~9%, Cinemachine ~5%, HLInputModule ~4%, character FixedUpdate ~7%, ProceduralMesh spawning ~5%), Project Dream's
App.Update hook (gp_update) itself ~0.5% plus its button searches. The main thread runnable ~95% of a frame but
running ~80%: FMOD's mixer loop (java_sdt.c, priority 43, threadCreate on the default core 0) took core 0 from it.
UI reports: the mission's Complete screen would not continue, then the Language screen would not close (both not
reproduced in Ryubing with the player's save, ryubing/ now holds it; the old one in ryubing/save_backup_r9); the
Achievements list (Project Dream's gp_list, moved by Unity's UpArrow/DownArrow only) did not scroll; LB/RB icons.
Build 202611191555 (773e8d3):
- FMOD mixer, rumble, watchdog on cores 1-2 (pthr_cpu_watch puts the mixer in the [perf] cpu list).
- pd_direct_api only for wrappers that restore and jump (180 of 195): 15 null-check arguments, cut bools to a byte
  (il2cpp_gchandle_new[_weakref]: Android leaves upper bits undefined) or keep callbacks; round 8's build bypassed
  them (the likeliest cause of the stuck screens; unconfirmed).
- list_keys (main.c): while gp_list_open, D-pad/left stick -> Android D-pad keys (repeat 350 ms, then 160-60 ms).
- tools/switch_glyphs.sh -> ../sdt_out/glyphs (L, R, +, - from the game's LB, RB, Menu, View icons; RGBA32);
  package.sh copies them to glyphs/; pd_bundle_path loads them for controllericons/ (catalog m_Crc all 0).
  ZL/ZR are the game's trigger actions (inputs 8/10), L/R page (shared with D-pad left/right): not remapped.
- unity_diag: GameObject.Find counted ([find]); Project Dream's three searches back off 1/2/4/8 calls while not
  found, reset on an active-scene change, one a frame (Ryubing: 63 -> ~7 searches per 10 s).

Hardware round 10 (build 202611191555, 1784/460 MHz): the FMOD move worked (no core-0 theft); world 2 (Dream
Factory) GPU-bound: GPU 99%, fence waits 5-11 ms a frame, 37-55 fps. Census of a world 2 frame (Ryubing, docked):
~934 + ~124 draws in two 1920x1080 B10G11R11 passes, a 480x270 copy, UI, a 1024x32 LUT -- at full size although
the player's Resolution slider was at 50%: Project Dream's render_scale never reached URP (the asset's renderScale,
+96, stayed 1.00). The menu behind Achievements moved: round 9's Android D-pad keys reached the game's keyboard
navigation. Each of Project Dream's button searches ~8 ms in a level. 170 s with no frame at 262 s = the console
asleep (every thread's logging stopped).
Build 202611191705 (3aa39ee):
- drs.c: the slider (gp_options[0] value, +0x24) applied to URP's renderScale (the game's own lower value wins);
  dynamic resolution (config v8: dynamic_res 1, dynamic_res_min 0.5): 5% steps down while late and the CPU waits
  on GPU fences > 1.5 ms (vkdkFenceWaitTotalNs, new), up after 1.5 s of headroom; dynamic_res 2 = held at the
  floor (debug). Verified in Ryubing: 25% -> 480x270 passes. ScalableBufferManager does nothing here (cameras do
  not allow dynamic resolution).
- Achievements: pd_list_move -> gp_list_select(gp_list_selected +- 1); D-pad/stick hidden from the game while open.
- Find backoff up to 64 calls. vkdk ZCull stays off (Unity's depth copy writes depth from a shader: the case that
  broke it in Dekopon-32; untested here). In-menu Dynamic resolution toggle: Project Dream's options table is fixed
  (26 x 0x60), not done.
Build 202611191713 (960a619): Dynamic Resolution in Graphics Quality, in VSync's place (gp_options entry 3: key
dynamic_resolution, label, text, default on; VSync did nothing here -- the swapchain is FIFO); Unity's vSyncCount
held at 1 when set to 0 (Project Dream no longer finds "vsync" and would set 0). Correction: Resolution Scale's
value is out of its most (+0x20 = 50: 50 = 1.00, as the menu shows); the player's 50 was full resolution, Project
Dream applies it to URP itself (asset renderScale 1.00 = right), and round 10's build halved it by reading 50 as
50%. The world 2 census at full size was real. gp_options layout: key, label, text, page +0x18, default +0x1c, most
+0x20, value +0x24, choice labels from +0x28. Frame Rate value v -> targetFrameRate v + 30.
Then dynamic_res_min 0.75 by default (config v9; the player found 50% too soft -- that run was round 10's misread
slider at 50% the whole time; at 50% GPU waits were 0 and the main thread 92-97%: CPU-bound there).

Hardware round 11 (build 202611192339, 1785/460): a crash at 584 s -- 'Vulkan - Out of memory' (vkdk could not get
7.5 MB of heap for an image; Unity faulted on it after). Unity's committed memory (vmem) 816 -> 1312 MB: vmem kept
every granule Unity ever committed (decommits ignored). Ryubing's new [mem] line, a world 2 level: heap 2.55 of 3.24
GB in use = GPU 721 MB + Unity 1200 MB + ~630 MB the rest (the console's heap is ~100 MB smaller). Dynamic resolution
went down while CPU-bound (GPU waits ~0) and never up (it rose only with frames on time). Main thread 94-98% busy at
42-48 fps in play: the game's TimeManager 23%, its spline following (TransformSurfaceFollower ->
SegmentedSpline.FindNearestSurfaceLocationFromLocal, Tracker2D/CollisionResolver2D) ~10%, URP ~8%, Cinemachine ~6%,
HLInputModule ~4%; the port's own code ~3.5%; waits ~3%.
Build 202611200011 (6bcfc04): vkdk OOM recovery (drain, let retired blocks go, low-memory hook, retry; 4 MB+ blocks
in whole MB; vkdkDeviceMemoryBytes); vmem page tracking + vmem_trim (Unity's decommit = madvise DONTNEED + mprotect
NONE) every report / on failure (Ryubing: 240 MB back, heap -215 MB); [mem] line; drs on 1 s windows, 12.5% levels,
up when the GPU keeps up whatever the frame rate, 3 s between changes; Project Dream's searches 16/64 calls apart after
a miss, reset by gp_menu_open and scene changes. Next: the game's spline search (~10%) natively, if it is a pure
function of the spline's data.
Looked into (round 11 data), not done: the ~10% under TransformSurfaceFollower is not a search -- the profile's two
call sites in SegmentedSpline.FindNearestSurfaceLocationFromLocal are GetSplineLocationFromDistance (a linear walk
over the knots through ISplineRuntimeHandle interface calls) and SplineRuntimeComponent.GetSurfaceLocation. The
spline code's own samples add up to ~1.1% (GetSplineLocationFromDistance 0.8%, SplineLocation.get_SplineDistance
0.2%); the rest is Unity transform work it calls (Transform get/set, TransformChangeDispatch). IL2CPP's runtime
machinery (interface slow path, generic lookups, type checks) ~0.4%. A native rewrite would gain under 1%: not worth
the risk to movement on rails. The main thread's time is the game's own logic spread over many systems.

Hardware round 12 (build 202611200011): a crash starting a level with the pirate outfit -- out of memory for 8 MB
with 719 MB free in the heap but its largest free piece 4 MB (fragmentation: newlib's one malloc held Unity's
granules, the GPU's blocks, code memory and thousands of small allocations). Comics flickered (dynamic resolution
and the game's own cutscene scale, 0.5, took turns: URP remade its targets each frame). Reward screens: white for 5 s+
(the main thread in AssetBundleRequest.GetResult waiting for the load; [io]: 238 MB off the card for 67 MB asked --
fs_cache dropped sdt.apk's blocks at every close and Unity reopens it all the time); a black frame with the trophy
when confirming fast (not reproduced).
Build 202611200118 (75c0e74): big.c (256 KB+ from 64 KB pages at the top of the heap, newlib below, fake_heap_end
moved under newlib's malloc lock); drs hands off under the game's own lower scale and after 3 overwrites in 2 s;
fs_cache keeps an .apk's blocks by path across reopens, 64 MB; async uploads 4 ms / 32 MB. Ryubing world 2: big
2.2 GB, small < 200 MB, 729 MB still to be had in one piece; a level load 164 MB asked, 72 MB of blocks read.
Round 12 also: "couldn't overclock, stuck at stock clocks". The port sets no clocks (no apm, boost or pcv writes;
util.c cpu_boost has no callers) and no change since round 10 (when 1785 MHz ran) touches a clock service; the
round 12 log's levels ran ~30 fps with the main thread 97% busy, which fits stock. Build 202611200127: clocks.c reads
CPU/GPU/memory clocks (clkrst, read only) once a second on the watchdog's thread; each [perf] report gets a [clk]
line with the range the stretch ran at (Ryubing reads 0). Next log says whether the overclock tool's clocks applied.
Thorhax's SonicDreamTeam-NX (github.com/Thorhax/SonicDreamTeam-NX, MIT, battd_nx fork, the 1.10.3 APKVISION APK on
Mesa NVK) claims 57.5 fps in its heaviest level at CPU 1785 / GPU 768 from the homebrew menu and 60 from a 4-core
forwarder. Its CPU-side setup differs from ours in three ways: graphics jobs in the older mode (it measured native
slower), one job worker per core (3, or 4 when the process may use core 3; Unity's own is a core fewer) with no core
of the main thread's own, and the fourth core from a forwarder. (Its futex is the kernel's, as ours.) Build
202611200824 (config version 10) takes all three, each a switch: gfx_jobs 1 (2 = native), job_workers 0 (one per
core; -1 = Unity's 2), main_core 2 (main alone at priority 44 on core 0, the game's threads at 59 keep core 0 in their
masks so they run there while it waits; 1 = the old exclusive core), fourth_core 1 (core 3 in the game's threads'
masks and a 4th worker when svcGetInfo CoreMask has it). Ryubing: threading mode 3, Job.Worker 0-2, the first level
draws right; HW A/B is the real test (native vs older mode first).
Hardware round 13 (build 202611200824, the [clk] lines say the clocks applied: 1224/460, then 1785/768). The game's own
30 cap until ~190 s, then 60: at 1224/460 levels 40-48 fps (round 12's native jobs ~30 at the same clocks), at
1785/768 53-57 fps -- the main thread 95-99% busy, under 1 ms of waits, GPU fence waits ~0. The profile there: the
game's managed code ~57% (TimeManager 21%, TransformSurface* 8%, Tracker2D/CollisionResolver2D 5%, HLInputModule 4.8%
= the game's own per-handler virtual calls, ApplePluginControllerInputMonitor 2% = its input-binding updates,
Cinemachine ~7%), URP 8.7%, canvases 3%, FMOD update 2.7%, the loader's atomics stub for GC_end_stubborn_change (the
incremental GC's write barrier, relaxed LDSET into GC_dirty_pages) 2.85%, svcOutputDebugString 1-2% (the reports'
printing). Project Dream's options file had fps_counter 1, chromatic_aberration 1, shadows off.
Build 202611200956: fast_atomics (pd_shim.c stubs_fast_or: the 28 relaxed 64-bit LDSET stubs copied to the top of the stub area
with ldr + bic + cbz before the exclusive loop, the old stub's first word a branch to its copy; done in pd_mprotect
as the loader makes the stub pages executable, before any game code runs; Ryubing: 28 of 537, the copy disassembled
right, levels run); svcOutputDebugString from the log's thread; drs down only when GPU waits are over 2 ms and at
least half the lateness, two windows in a row.
Hardware round 14 (build 202611200956, no 4th core yet): stock 1020/460 levels 35-40 fps (60 asked), 1224/460 40-46,
1785/768 50-58.8; main thread 95-99%. Profile at 1785: the svc buckets (svcSendSyncRequest/svcExitProcess by name)
are waits (WaitVSync when on time, futex), not IPC; the port's own code not waiting ~3%, of it fputc 0.5% average and
1-1.3% in windows = Project Dream's kv_put (NSUbiquitousKeyValueStore -> PCSaveData/icloud_kvs.txt, 93 KB with the
save's JSON, rewritten a character at a time on each set, ~11 times in 10-20 s); HLInputModule.Process = 2 monitors
(Hardlight.ApplePluginControllerInputMonitor, Hardlight.InputMonitor: bindings, touch, swipe -- the game's own; probe
with il2cpp FindObjectsOfType in Ryubing); gp_update+0x4e4 = the game's App.Update; FMOD System::update 3.1%.
Achievements: holding the stick down cut the soundtrack until let go (gp_list_select ticks gp_achievement_sound on
each row, 16 a second at full tilt).
Build 202611201105: list ticks 4 a second at most (pd_list_move clears gp_list_open, read only for the tick, around the call);
Project Dream's fputc/putc = putc_unlocked (its files are each written by the function that opened them); save
memory files 64 KB buffered; FMOD::System::update (libunity+0x16fe4f8) at most audio_update_hz 40 a second.
Build 202611201115: config fixed_step (0 by default = the game's 1/60): Time.fixedDeltaTime set when the game's is 1/60
(another value it sets stands), e.g. 0.02 = 50 Hz physics and FixedUpdate; Ryubing: applied at start, held through
a level load, the level runs. For the player to try; it can change how movement and the camera feel.
Hardware round 15 (build 202611201115): 60 needs 1785 CPU, and at 460 GPU a Resolution Scale of 0.70; dynamic
resolution "not as consistent as 0.70"; stock clocks "30 comfortably but stutters". Log: 1785/460 GPU-bound (fence
waits 15 ms, 42-47 fps) with the scale at 88-100%; dragging the slider read 0.98..0.02 and -1 while the asset
lagged (0.95, 0.85, 0.76...) and "keeps setting its own render scale" every ~30 s. Cause: Project Dream's slider is
(v + 50) / 100 (0..50 = 0.50..1.00; gp_update+0x3728), not v / most -- 0.70 was read as 0.40 and drawn at that -- and
gp_update re-applies its value every frame it differs (gp_update+0x39cc b.le past set_renderScale), so the two took
turns. Stock 1020/460: 36-43 fps asked for 60 = frames shown unevenly (GC stops of 155-179 ms were in menus/loads).
Build 202611201225: pd_render_scale_option = (v + 50) / 100; pd_patch_rescale makes that b.le a b (the slider still applies
itself in its settings rebuild); vkdk timestamps (DkCounter_Timestamp before and after each submission's work, 64
slots, harvested in QueueSubmit: vkdkGpuBusyTotalNs; Ryubing stamps ~0); drs.c rewritten: G (GPU ms a frame) per
0.5 s against 90% of the frame, down at once to scale * sqrt(budget / G) in 0.05 steps, up 0.05 after 2 s under
70%, floor dynamic_res_min 0.65 (config v11), the game's own lower scale respected (3 minutes, then once only),
[drs] logs the game's own values; auto_30 (even 30: wait_vsync_paced waits for the port's own count + 2 when the
game asks for 60 and frames are under 45 fps for 4 s with resolution spent; back to 60 when work under 15 ms and the
GPU at the floor under 14 ms for 6 s; auto_30 2 = always, a debug).
Hardware round 16 (build 202611201225, stock 1020/460, the game's own 30 then 34, the log ends as 60 is chosen):
menus an even 30.0; levels 29.9 with ~12% of frames just over 33.3 ms (main thread 80-90%); vkdk's GPU time real
on the console: 20-26 ms a frame at 100% and 460 MHz (60 needs ~0.75). The player: Frame Rate slider 30-60 in steps
of 5? Project Dream's fps option is a slider 0..90 = value + 30 fps (gp_option_label prints it, gp_update sets
targetFrameRate every few seconds on a timer). Between 30 and 60 nothing shows evenly on 60 Hz, so:
Build 202611201258: Frame Rate = 30 / 60 / Auto (pd_shim.c fps_option: labels, most 2, default Auto; gp_option_label's fps
check NOPed at PD+0x8f270, gp_update's set_targetFrameRate cbz made a b at PD+0x7bd2c; an old value over 2 = Auto);
drs.c holds targetFrameRate at the choice every frame and Auto drives the even 30 (config auto_30 only without the
menu). Ryubing: labels '30' '60' 'Auto', fps=0 held 30, fps=2 held 60.
Hardware round 17 (build 202611201258, stock 1020/460, Auto): "the drop looks a bit grainy". The log: the scale at
exactly 50% for minutes, with Dynamic Resolution on and off -- the game's own 0.50 (its phone profile's scale, set
now and then, then 0.99) respected by drs.c's hands-off (3 minutes), where Project Dream had put the slider back every
frame. The even 30 "on" with frames at 37-46 fps: Unity calls WaitVSync only for an early frame (AndroidVSync: when
the counter is under its target), so the port's even-30 wait inside it never ran on the late frames. GPU at 100%:
20-24 ms a frame at 460 MHz. The player also asked for HD rumble with a menu toggle.
Build 202611201325: drs.c puts the slider's scale back after any game write, standing back only while the game writes 3+
times a second (until a second without); floor 0.70 (config v12), budget 92%; unity_even30_wait from main.c's loop
before each frame (two refreshes after the last frame's start, through Unity's WaitVSync); Ryubing auto_30 2: frames
33.3 ms. HD rumble: the game's VibrationManager is UI-only (VibrationEvent None/DefaultSelection/LongPressSelection,
VibrationType = iOS feedback kinds; its coroutine's platform call compiled out of the Mac build), so sdt_rumble.c
rumbles at six moments through hooks in the game's code (pd_shim.c ga_hooks_install, ga_hook.s stub copied into the
stub area, a branch at the method's first instruction; written at the stub area's mprotect while the image is still
RW): CharacterEvents.OnCharacterTakeDamage (0x73cacc), CharacterState_TriggerRespawn.DoOnEnter (0x75e5e8),
HomingAttackAirOneShot.ActionOnCharacterHit (0x729eec), MovementGroundSpinDashCharge.OnLaunch (0x724e3c),
CharacterBoostTrigger.DoTriggerEnter (0x5a21c8), MissionReachGoalTracker.Action_ReachedGoal (0x597dd4); each spaced,
logged the first 3 times; rumble_play_at for patterns; Settings' Vibration (Project Dream's "vibration") turns it off,
its text now says HD Rumble. Ryubing: a test hook on HLInputModule.Process ran 425 times with the game unharmed.
Hardware round 18 (build 202611201325): rumble felt only at the goal and after a respawn, and weak. The log: "a life
lost" x2 and "goal", nothing else -- OnCharacterTakeDamage, the homing attack's ActionOnCharacterHit, QueueBoost and the
boost trigger are not what play calls. Found with counting hooks in Ryubing (scripted play, script_rumble.txt: stick
forward + every button; 256 Character* methods, then the abilities' updates) and Character.TryTakeDamage's own calls:
the hit = Character.TriggerCascade / CharacterAbility_Knockback.Trigger / Character.TriggerKnockback, a stumble =
Stumble.TriggerFromHazard, the shield = Shield.TryTakeDamage, a loss = Character.TriggerHardFailure; the boost = its
ability's DoOnUpdate (GroundBoostBase / GroundBoostOneShot / RailBoostBase; first frame after 250 ms = start); the air
attack = Character.TriggerBurstAttackAir; the swap = Character.BeginSwapStart. (ActorAbility.Enter,
CharacterAbility.DoOnEnter, ActorState.DoOnEnter and Actor.DebugOnStateEnter are never called in play.)
Build 202611201447: 19 hooks; strengths as mktkv_nx's (0.55-0.85 low band, 200-600 ms; taps 0.3-0.55 at 40-80 ms); a hit at
the same moment as a life lost gives way (rumble_clear). Ryubing scripted play: air attack, boost, hit, a life lost.
Hardware round 19 (build 202611201447): no rumble for the boost but one at a jump's peak; "still not good or official".
The log: "air attack" + "boost" together (the boost abilities' DoOnUpdate and TriggerBurstAttackAir both ran on the
air attack, which uses stamina too). The boost: CharacterStamina.Activate's callers are the boost-like states'
DoOnEnter (generic, named _gshared: the earlier probes' patterns missed them) -- CharacterState_GroundBoostBase_2
(0xe58e70) and RailMovementBoostBase_2 (0xe5f5e4); the boost button is R (Ryubing script: boost on the R presses).
The feel: round 18's lows at 90-140 Hz are off the Joy-Con actuators' resonance (~160 Hz low, ~320 Hz high).
Build 202611201629: the air attack and ability-update hooks gone, the two boost states hooked; every pulse at 160/320 Hz, built
as a crisp high-band transient (CLICK 30-45 ms) plus a fast-decaying low-band body (THUD) or a launch surge (FADE);
no ROUGH. Ryubing scripted play: boost on R, hit, a life lost.
Hardware round 20: "too strong". Build 202611201639: every pattern's amplitudes at 60% (sdt_rumble.c K), shapes and timing kept.

Project Lucid (github.com/dev-camo/project-lucid): another route to the same game. Not used; worth reading if the
Mach-O loader route stalls.
