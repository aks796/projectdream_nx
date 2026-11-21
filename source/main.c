/* main.c -- Sonic Dream Team (Project Dream's Android build) on the Switch
 *
 * Start-up:
 *   1. setup screen: find sdt.apk (tools/prepare made it from the player's own
 *      macOS game and Project Dream), extract its two native libraries
 *   2. the linker's "system": libc & co, Vulkan (vkdk on deko3d), the NDK
 *      (android_ndk.c), OpenSL ES (opensles.c); the Java world (jni.c,
 *      java_*.c)
 *   3. libunity.so (Unity's Android player 2022.3.54f1) and libil2cpp.so
 *      (Project Dream's loader, which maps the Mac game's arm64 code; its
 *      Switch parts in pd_shim.c), then what UnityPlayer does on Android:
 *      initJni, the surface, resume, focus
 *   4. the frame loop: the controller (as Project Dream's ControllerInput
 *      publishes it), touch, nativeRender; Unity owns the Vulkan swapchain and
 *      presents by itself
 *
 * Much of the loader is mkt_nx's (Mario Kart Tour on the Switch: the same
 * Unity 2022.3 Android player).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <errno.h>
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <switch.h>

#include "apk.h"
#include "apkfile.h"
#include "buildinfo.h"
#include "config.h"
#include "error.h"
#include "imports.h"
#include "jni.h"
#include "log.h"
#include "net_shim.h"
#include "paths.h"
#include "pd_shim.h"
#include "prof.h"
#include "pthr.h"
#include "rumble.h"
#include "saltynx.h"
#include "save_writer.h"
#include "sandbox.h"
#include "setup_ui.h"
#include "so_util.h"
#include "unity.h"
#include "vfs.h"
#include "vkdk.h"
#include "vmem.h"

// --- heap: all of it goes to newlib; the linker takes module images from it --

void __libnx_initheap(void) {
  void *addr;
  size_t size = 0;
  size_t mem_available = 0, mem_used = 0;

  if (envHasHeapOverride()) {
    addr = envGetHeapOverrideAddr();
    size = envGetHeapOverrideSize();
  } else {
    svcGetInfo(&mem_available, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
    svcGetInfo(&mem_used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
    if (mem_available > mem_used + 0x200000)
      size = (mem_available - mem_used - 0x200000) & ~0x1FFFFF;
    if (size == 0)
      size = 0x2000000 * 16;
    Result rc = svcSetHeapSize(&addr, size);
    if (R_FAILED(rc))
      diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_HeapAllocFailed));
  }

  extern char *fake_heap_start;
  extern char *fake_heap_end;
  fake_heap_start = (char *)addr;
  fake_heap_end = (char *)addr + size;
}

// --- linker tables ----------------------------------------------------------------

extern DynLibFunction ndk_functions[];
extern size_t ndk_numfunctions;
extern DynLibFunction sles_functions[];
extern size_t sles_numfunctions;
extern DynLibFunction more_functions[];
extern size_t more_numfunctions;
extern DynLibFunction sdt_functions[];
extern size_t sdt_numfunctions;
extern DynLibFunction vulkan_functions[];
extern const size_t vulkan_numfunctions;
extern DynLibFunction media_functions[];
extern size_t media_numfunctions;
void media_ndk_java_init(void);
void vulkan_table_init(void);

void watchdog_init(void);
void watchdog_beat(void);
void watchdog_pause(int paused);

static void setup_linker(void) {
  update_imports();
  vulkan_table_init();
  const DynLibFunction *tabs[] = { dynlib_functions, libc_functions, ndk_functions, sles_functions, more_functions,
                                   sdt_functions, vulkan_functions, media_functions };
  const size_t ns[] = { dynlib_numfunctions, libc_numfunctions, ndk_numfunctions, sles_numfunctions,
                        more_numfunctions, sdt_numfunctions, vulkan_numfunctions, media_numfunctions };
  const unsigned ntabs = sizeof(ns) / sizeof(*ns);
  size_t n = 0;
  for (unsigned i = 0; i < ntabs; i++)
    n += ns[i];
  DynLibFunction *all = malloc(n * sizeof(*all));
  size_t k = 0;
  for (unsigned i = 0; i < ntabs; i++) {
    memcpy(all + k, tabs[i], ns[i] * sizeof(*all));
    k += ns[i];
  }
  so_set_system_symbols(all, n);
  so_set_stdcxx_symbols(dynlib_stdcxx, dynlib_numstdcxx);
  so_add_search_dir(path_lib());
  // the engine is patched in place (debug traps); Project Dream's loader is
  // hooked (pd_shim.c)
  so_set_patchable("libunity.so");
  pd_shim_init();
}

// --- vkdk ---------------------------------------------------------------------------

static void vkdk_log(const char *line) {
  log_printf("[vkdk] %s", line);
}

void android_window_prepare(void); // android_ndk.c

// Each presented frame: Status Monitor's FPS and resolutions (SaltyNX, saltynx.c) -- the picture Unity draws and
// the screen it is shown on
static void on_present(uint32_t w, uint32_t h) {
  const int docked = appletGetOperationMode() == AppletOperationMode_Console;
  saltynx_present(w, h, docked ? 1920 : 1280, docked ? 1080 : 720);
}

// --- memory ---------------------------------------------------------------------------------
// Round 11: the GPU's memory (vkdk's blocks, from the same heap as everything) could not get a 7.5 MB piece late in
// a session and Unity crashed. [mem]: the heap in use and free, the largest free piece (probed), the GPU's memory,
// Unity's committed memory (vmem).
void fs_cache_drop_all(void);

static void mem_report(void) {
  const struct mallinfo mi = mallinfo();
  // Unity's decommitted granules back to the heap every report (Ryubing, a world 2 level: 240 MB over two minutes,
  // the heap in use 215 MB lower; the GPU's memory comes from the same heap)
  const size_t trimmed = vmem_trim();
  if (trimmed)
    log_printf("[mem] %zu MB Unity had given back returned to the heap", trimmed >> 20);
  size_t vcom = 0;
  vmem_stats(NULL, &vcom);
  void big_stats(size_t *region, size_t *used, size_t *peak, size_t *largest_free, size_t *room, uintptr_t *top);
  size_t b_region, b_used, b_peak, b_free, b_room;
  uintptr_t top;
  big_stats(&b_region, &b_used, &b_peak, &b_free, &b_room, &top);
  extern char *fake_heap_start;
  const size_t heap = (size_t)(top - (uintptr_t)fake_heap_start);
  const size_t biggest = b_free + b_room > b_free ? (b_room > b_free ? b_room : b_free) : b_free;
  log_printf("[mem] heap %zu MB: big pieces (256 KB and up) %zu MB at the top, %zu in use (the most %zu), the largest "
             "free run in it %zu; small ones %zu MB in use + %zu free inside; %zu MB between; the biggest piece to be "
             "had %zu MB; GPU memory %llu MB; Unity's committed %zu MB",
             heap >> 20, b_region >> 20, b_used >> 20, b_peak >> 20, b_free >> 20, (size_t)mi.uordblks >> 20,
             (size_t)mi.fordblks >> 20, b_room >> 20, biggest >> 20, (unsigned long long)(vkdkDeviceMemoryBytes() >> 20),
             vcom >> 20);
}

static void low_memory(void) {
  const size_t trimmed = vmem_trim();
  log_printf("[mem] the GPU's memory short: %zu MB of Unity's that it had given back returned to the heap",
             trimmed >> 20);
  fs_cache_drop_all();
  mem_report();
}

static void setup_vulkan(void) {
  char p[600];
  vkdkSetLowMemoryHook(low_memory);
  vkdkSetLogCallback(vkdk_log);
  vkdkSetWindowPrepare(android_window_prepare);
  vkdkSetPresentHook(on_present);
  snprintf(p, sizeof p, "%s/shaders", path_internal());
  mkdir(p, 0777);
  snprintf(p, sizeof p, "%s/shaders/dksh.cache", path_internal());
  vkdkSetShaderCachePath(p);
  snprintf(p, sizeof p, "%s/shaders.pack", path_base());
  vkdkSetPrebuiltShaderCachePath(p);
  // uam and SPIRV-Cross on vkdk's own 8 MB stack: started while the heap has room
  vkdkStartWorker();
}

// --- screen ---------------------------------------------------------------------------

void io_take_stats(u64 *calls, u64 *small, u64 *bytes, u64 *ns); // bionic_extra.c
void unity_diag_take(u64 *batched, u64 *refused);                  // unity_diag.c
void unity_tune_frame(unsigned frame);                             // unity_diag.c
void mmap_file_stats(u64 *maps, u64 *bytes);                     // libc_shim.c
void fs_cache_stats(u64 *hits, u64 *card_reads, u64 *direct, unsigned *blocks); // fs_cache.c

static int s_forced_mode = -1; // test_script's dock: 1 docked, 0 handheld

static void pick_screen_size(void) {
  const int docked = s_forced_mode >= 0 ? s_forced_mode : appletGetOperationMode() == AppletOperationMode_Console;
  if (config.screen_width > 0 && config.screen_height > 0 && config.screen_width <= 1920 &&
      config.screen_height <= 1080) {
    screen_width = config.screen_width;
    screen_height = config.screen_height;
  } else if (docked && config.docked_1080) {
    // (1.0.1: docked at 1280x720 by default -- the dock's GPU clock, 768 MHz, is what 720p at 60 needed in handheld;
    // 1080p is 2.25 times the pixels. The system scales the picture to the TV. config docked_1080 1: 1920x1080)
    screen_width = 1920;
    screen_height = 1080;
  } else {
    screen_width = 1280;
    screen_height = 720;
  }
}

void android_window_init(int w, int h);
void java_android_screen_changed(void);

// --- input ----------------------------------------------------------------------------

static PadState s_pad;

void input_touch(int id, int down, float x, float y);
void input_flush(void);
void input_init(void);
void test_script_init(void);
int test_script_poll(float *x, float *y, int *back);
int test_script_focus(void);
int test_script_dock(void);
void test_script_pad(u64 *held, int *lx, int *ly, int *rx, int *ry);
// Project Dream's list screens (Achievements): while one is open the D-pad and the left stick move its selection
// (pd_list_move), once at once and then repeated while held, faster the further the stick is pushed. The game gets
// no directions meanwhile (pd_shim_pad), so the menu behind the list stays where it is.
static void list_keys(u64 held, int ly) {
  static int s_held;  // the direction held now (1 up, -1 down)
  static u64 s_next;  // when a held direction repeats
  static u64 s_tick;  // when the list's tick last played
  int dir = 0;
  float speed = 1.f;
  const int open = pd_list_open();
  static int s_was_open = -1;
  if (open != s_was_open) { // (for a menu that will not close: where the list stood)
    if (s_was_open >= 0)
      log_printf("[ui] Project Dream's list screen %s", open ? "opened" : "closed");
    s_was_open = open;
  }
  if (open) {
    const float sy = (float)ly / 32767.f;
    if ((held & HidNpadButton_Up) || sy > 0.5f)
      dir = 1;
    else if ((held & HidNpadButton_Down) || sy < -0.5f)
      dir = -1;
    if (!(held & (HidNpadButton_Up | HidNpadButton_Down)))
      speed = sy > 0 ? sy : -sy;
  }
  const u64 now = armGetSystemTick();
  if (!dir) {
    s_held = 0;
    return;
  }
  int sound = 1;
  if (dir != s_held) { // a new push: at once, repeating after 350 ms
    s_held = dir;
    s_next = now + armNsToTicks(350000000ull);
  } else if (now < s_next) {
    return;
  } else { // held: every 160 ms, down to 60 ms with the stick all the way; its tick 4 times a second at most
    s_next = now + armNsToTicks((u64)(160000000.0f - 100000000.0f * (speed > 1.f ? 1.f : speed)));
    sound = now - s_tick >= armNsToTicks(250000000ull);
  }
  if (sound)
    s_tick = now;
  pd_list_move(dir > 0 ? -1 : 1, sound); // (up = the row before)
}

static void poll_input(void) {
  padUpdate(&s_pad);
  u64 held = padGetButtons(&s_pad);
  const HidAnalogStickState ls = padGetStickPos(&s_pad, 0), rs = padGetStickPos(&s_pad, 1);
  int lx = ls.x, ly = ls.y, rx = rs.x, ry = rs.y;
  test_script_pad(&held, &lx, &ly, &rx, &ry);
  pd_shim_pad(held, lx, ly, rx, ry, padIsConnected(&s_pad) || held);
  list_keys(held, ly);

  // the touchscreen reports in 1280x720 panel pixels; Unity's window may be 1080p
  float sx_, sy_;
  int sback;
  const int scripted = test_script_poll(&sx_, &sy_, &sback);
  if (scripted)
    input_touch(9, 1, sx_ * (float)screen_width / 1280.f, sy_ * (float)screen_height / 720.f);
  HidTouchScreenState ts = { 0 };
  if (hidGetTouchScreenStates(&ts, 1) && ts.count > 0) {
    const float sx = (float)screen_width / 1280.f, sy = (float)screen_height / 720.f;
    for (int i = 0; i < ts.count && i < 10; i++)
      input_touch(ts.touches[i].finger_id % 10, 1, ts.touches[i].x * sx, ts.touches[i].y * sy);
  }
  input_flush();
}

// --- HOME / sleep -----------------------------------------------------------------------

static AppletHookCookie s_hook;
static volatile int s_focus_changed;
static volatile int s_mode_changed;

static void applet_hook(AppletHookType type, void *param) {
  (void)param;
  if (type == AppletHookType_OnFocusState || type == AppletHookType_OnOperationMode)
    s_focus_changed = 1;
  if (type == AppletHookType_OnOperationMode)
    s_mode_changed = 1;
}

// --- leaving ------------------------------------------------------------------------------

// The way a game closes: the system is told (applet ISelfController::Exit for an application, ExitProcessAndReturn
// for a library applet) and ends the process itself, back to the HOME menu. libnx does this at exit when
// __nx_applet_exit_mode is 1; here it is done without the rest of libnx's exit (no services closed under the
// engine's threads, nothing run on the main thread's stack from another thread). svcExitProcess on its own, as
// before, looked to the console like a crash (hardware round 7).
static void exit_backstop(void *arg) {
  (void)arg;
  svcSleepThread(5000000000ll);
  svcExitProcess();
}

static void NX_NORETURN close_like_a_game(void) {
  extern u32 __nx_applet_exit_mode;
  const LoaderReturnFn before = envGetExitFuncPtr();
  __nx_applet_exit_mode = 1;
  // (the applet service's last reference: libnx sets the exit function to its own and keeps the service open)
  for (int i = 0; i < 16 && envGetExitFuncPtr() == before; i++)
    appletExit();
  const LoaderReturnFn exit_fn = envGetExitFuncPtr();
  // (a backstop: should the system not end the process in 5 s -- Ryubing only stubs the request -- it ends itself)
  static Thread backstop;
  if (R_SUCCEEDED(threadCreate(&backstop, exit_backstop, NULL, NULL, 0x1000, 0x2c, -2)))
    threadStart(&backstop);
  if (exit_fn != before && exit_fn)
    exit_fn(0); // tells the system, then waits to be ended
  svcExitProcess();
  for (;;) {
  }
}

static void NX_NORETURN leave_now(void) {
  // once: the engine's pause may come back just as its time out goes off
  static int s_leaving;
  if (__atomic_exchange_n(&s_leaving, 1, __ATOMIC_ACQ_REL))
    for (;;)
      svcSleepThread(1000000000ll);
  sw_flush_all(8000000000ull); // the game's last saves on the card
  fflush(NULL);
  log_printf("[main] out");
  log_flush();
  close_like_a_game();
}

static volatile int s_pause_back;

static void quit_timeout(void *arg) {
  (void)arg;
  svcSleepThread(4000000000ull);
  if (s_pause_back)
    return;
  log_printf("[main] the engine's pause did not come back in 4 s");
  leave_now();
}

// --- main ----------------------------------------------------------------------------------

int main(int argc, char **argv) {
  pthr_register_main();
  paths_init(argc > 0 ? argv[0] : NULL);
  if (!sandbox_init()) {
    char msg[512];
    snprintf(msg, sizeof msg,
             "The port is running from %s,\n"
             "which is not a folder of its own.\n\n"
             "Put " PORT_NAME ".nro in its own folder, like\n"
             "/switch/" PORT_NAME "/, together with sdt.apk.",
             path_base()[0] ? path_base() : "/");
    setup_ui_message("Move the port into its own folder", msg);
    setup_ui_close();
    extern void NX_NORETURN __libnx_exit(int rc);
    __libnx_exit(0);
  }
  log_init(path_base());
  log_printf(PORT_NAME " %s build %s", APP_VERSION_STRING, build_stamp());
  read_config(path_config());
  write_config(path_config());
  pthr_dedicate_main_core();
  paths_make_dirs();
  paths_fix_bundle_marker();
  void glyphs_install(void);
  glyphs_install(); // (the Switch's button icons, built in)

  const char *kerr = so_check_kernel();
  if (kerr)
    fatal_error("%s", kerr);

  extern char *fake_heap_start, *fake_heap_end;
  const unsigned heap_mb = (unsigned)((fake_heap_end - fake_heap_start) / (1024 * 1024));
  log_printf("[main] heap %u MB", heap_mb);
  if (heap_mb < 2000)
    fatal_error("Not enough memory (%u MB).\n\n"
                "Sonic Dream Team needs the console's full memory:\n"
                "start hbmenu by holding R while launching any\n"
                "installed game, then start this port from there.",
                heap_mb);

  GameInfo game;
  if (apk_setup(&game) != 0) {
    setup_ui_close();
    extern void NX_NORETURN __libnx_exit(int rc);
    __libnx_exit(0);
  }
  vfs_init();
  pick_screen_size();
  log_printf("[main] %s (%s %s), screen %dx%d", path_apk(), game.package, game.version_name, screen_width,
             screen_height);

  // the environment an Android app process has
  setenv("HOME", path_data(), 1);
  setenv("TMPDIR", path_tmp(), 1);
  setenv("ANDROID_DATA", "/data", 1);
  setenv("ANDROID_ROOT", "/system", 1);
  // debug: the Mac game's Boehm GC never collects (it reads GC_DONT_GC at its start)
  if (config.gc_off)
    setenv("GC_DONT_GC", "1", 1);
  if (config.fmod_off)
    setenv("SDT_NO_FMOD", "1", 1);
  vmem_init();
  net_init();
  setup_vulkan();
  setup_linker();
  pthr_install_fake_tls(); // before any game code: its stack-guard reads
  jni_init();
  input_init();
  rumble_init();
  java_android_screen_changed();

  padConfigureInput(1, HidNpadStyleSet_NpadStandard);
  padInitializeDefault(&s_pad);
  hidInitializeTouchScreen();

  setup_ui_progress("Starting Sonic Dream Team", "Loading the game's libraries", 0);
  unity_load();
  pd_shim_java_init();
  media_ndk_java_init();

  // the console's setup screen hands the window to Unity's Vulkan swapchain
  setup_ui_close();
  android_window_init(screen_width, screen_height);
  vkdkSetSurfaceSize(screen_width, screen_height);
  void sampler_start(Handle main_thread);
  sampler_start(threadGetCurHandle());
  unity_start();

  appletHook(&s_hook, applet_hook, NULL);
  saltynx_init();
  watchdog_init();
  prof_init();
  test_script_init();
  int focused = 1;
  unsigned frames = 0;
  u64 t_last = armGetSystemTick(), engine_ticks = 0, engine_max = 0, frame_at = 0;
  unsigned over20 = 0, over34 = 0, over50 = 0;
  while (appletMainLoop()) {
    const int scripted = test_script_focus();
    if (s_focus_changed || scripted >= 0) {
      s_focus_changed = 0;
      const int now = scripted >= 0 ? scripted : appletGetFocusState() == AppletFocusState_InFocus;
      if (now != focused) {
        focused = now;
        log_printf("[main] %s", focused ? "back in focus" : "lost focus (HOME/sleep)");
        watchdog_pause(!focused);
        unity_pause(!focused);
        sw_set_urgent(!focused); // in the background (HOME, sleep): saves to the card at once
      }
    }
    if (!focused) {
      svcSleepThread(50000000ull);
      continue;
    }
    // docked <-> handheld: the render size follows (1080p / 720p) -- the window, the size vkdk reports for the
    // surface, then Unity's surface-changed event (it remakes its swapchain at the new size, as on a phone)
    const int dock = test_script_dock();
    if (s_mode_changed || dock >= 0) {
      s_mode_changed = 0;
      if (dock >= 0)
        s_forced_mode = dock;
      const int ow = screen_width, oh = screen_height;
      pick_screen_size();
      if (screen_width != ow || screen_height != oh) {
        log_printf("[main] operation mode: %dx%d -> %dx%d", ow, oh, screen_width, screen_height);
        android_window_init(screen_width, screen_height);
        vkdkSetSurfaceSize(screen_width, screen_height);
        java_android_screen_changed(); // DisplayMetrics, Configuration, the surface view's layout
        unity_surface_changed();
      }
    }
    void unity_even30_wait(void);
    unity_even30_wait(); // (drs.c's even 30: this frame two refreshes after the last)
    poll_input();
    const u64 f0 = armGetSystemTick();
    if (frame_at) {
      void drs_frame(uint64_t period_ns);
      drs_frame(armTicksToNs(f0 - frame_at));
      const u64 period = armTicksToNs(f0 - frame_at) / 1000000;
      over20 += period > 20, over34 += period > 34, over50 += period > 50;
    }
    frame_at = f0;
    if (!unity_frame()) {
      log_printf("[main] the engine asked to quit");
      break;
    }
    const u64 fdt = armGetSystemTick() - f0;
    engine_ticks += fdt;
    if (fdt > engine_max)
      engine_max = fdt;
    frames++;
    unity_tune_frame(frames);
    watchdog_beat();
    // debug: captures of the screen at the seconds config capture_at lists
    if (config.capture_at[0]) {
      static u64 t0;
      static unsigned done;
      if (!t0)
        t0 = armGetSystemTick();
      const unsigned secs = (unsigned)(armTicksToNs(armGetSystemTick() - t0) / 1000000000ull);
      unsigned k = 0;
      for (const char *p = config.capture_at; *p && k < 32; k++) {
        char *end;
        const unsigned at = (unsigned)strtoul(p, &end, 10);
        if (end == p)
          break;
        if (!(done & (1u << k)) && secs >= at) {
          done |= 1u << k;
          char path[300];
          snprintf(path, sizeof path, "%s/capture_%u.bmp", path_base(), at);
          vkdkCaptureNext(path);
        }
        p = end;
        while (*p == ' ' || *p == ',')
          p++;
      }
    }
    if (frames == 600 || frames == 1800) { // (the game's image is in by now; a second try should it not be)
      const so_module *lu = so_find_loaded("libunity.so");
      if (lu)
        pd_direct_api(lu->base + lu->text_size, lu->base + lu->size);
      void unity_log_settings(void);
      unity_log_settings();

    }
    if (frames == 600) {
      log_boot_done();
      int unity_diag_srp_batcher(int *setting);
      int srp_setting = -1;
      const int srp = unity_diag_srp_batcher(&srp_setting);
      log_printf("[unity] graphics threading mode %d (2 a render thread, 3 graphics jobs, 4 native jobs); SRP Batcher "
                 "%d (GraphicsSettings %d)", unity_gfx_threading_mode(), srp, srp_setting);
    }
    if (frames % 600 == 0) {
      const u64 now = armGetSystemTick();
      const double secs = (double)armTicksToNs(now - t_last) / 1e9;
      size_t vres, vcom;
      vmem_stats(&vres, &vcom);
      char cpu[400];
      pthr_cpu_report(cpu, sizeof cpu, 6);
      VkdkStats vs;
      vkdkTakeStats(&vs);
      unsigned stops = 0;
      pd_shim_gc_stats(&stops);
      const double ms = 1e3 / 600.0 / 19200000.0;
      log_printf("[perf] frame %u: %.1f fps, %.1f ms (over 20 ms %u, 34 ms %u, 50 ms %u of 600); engine %.1f ms "
                 "(max %.0f); draws %.0f a frame, submits %.1f, shaders compiled %llu (%.0f ms), loaded %llu; GC "
                 "thread stops %u; %zu MB reserved, %zu MB committed; GPU fence waits %.1f ms, acquires %.1f ms a "
                 "frame; cpu %s",
                 frames, 600.0 / secs, secs * 1e3 / 600.0, over20, over34, over50, (double)engine_ticks * ms,
                 (double)engine_max * ms * 600.0, (double)vs.draws / 600.0, (double)vs.submits / 600.0,
                 (unsigned long long)vs.variants_compiled, (double)vs.variant_compile_ns / 1e6,
                 (unsigned long long)vs.variants_loaded, stops, vres >> 20, vcom >> 20, vs.fence_wait_ns / 6e8,
                 vs.acquire_ns / 6e8, cpu);
      u64 io_calls, io_small, io_bytes, io_ns;
      io_take_stats(&io_calls, &io_small, &io_bytes, &io_ns);
      u64 maps, map_bytes;
      mmap_file_stats(&maps, &map_bytes);
      u64 vs_skip, vs_wait;
      void unity_pacing_take(u64 *skipped, u64 *waited);
      unity_pacing_take(&vs_skip, &vs_wait);
      if (vs_skip)
        log_printf("[pacing] %llu frames went without Unity's vsync wait, %llu waited",
                   (unsigned long long)vs_skip, (unsigned long long)vs_wait);
      u64 srp_yes, srp_no;
      unity_diag_take(&srp_yes, &srp_no);
      if (srp_yes || srp_no)
        log_printf("[srp] SRP Batcher: %.0f render nodes a frame batched, %.0f refused (drawn one by one)",
                   srp_yes / 600.0, srp_no / 600.0);
      unsigned gc_n;
      double gc_total, gc_max;
      pd_shim_gc_pauses(&gc_n, &gc_total, &gc_max);
      if (gc_n)
        log_printf("[gc] %u stops of the world, %.0f ms in all, the longest %.0f ms", gc_n, gc_total, gc_max);
      void unity_find_report(void);
      unity_find_report();
      void drs_report(void);
      drs_report();
      mem_report();
      void clocks_report(void);
      clocks_report();
      u64 sv_n, sv_bytes, sv_written, sv_ns, sv_max;
      sw_stats(&sv_n, &sv_bytes, &sv_written, &sv_ns, &sv_max);
      if (sv_n || sv_written)
        log_printf("[save] the game saved %llu times (%.0f KB); %llu writes on the card by the save thread, %.0f ms "
                   "there, the slowest %.0f ms",
                   (unsigned long long)sv_n, sv_bytes / 1024.0, (unsigned long long)sv_written, sv_ns / 1e6,
                   sv_max / 1e6);
      u64 c_hits, c_card, c_direct;
      unsigned c_blocks;
      fs_cache_stats(&c_hits, &c_card, &c_direct, &c_blocks);
      if (io_calls || maps)
        log_printf("[io] %llu reads (%llu under 16 KB), %.1f MB, %.0f ms reading; %llu file mappings, %.1f MB; card "
                   "reads: %llu blocks + %llu direct, %llu from the cache (%u blocks kept)",
                   (unsigned long long)io_calls, (unsigned long long)io_small, io_bytes / 1048576.0, io_ns / 1e6,
                   (unsigned long long)maps, map_bytes / 1048576.0, (unsigned long long)c_card,
                   (unsigned long long)c_direct, (unsigned long long)c_hits, c_blocks);
      engine_ticks = engine_max = 0;
      over20 = over34 = over50 = 0;
      t_last = now;
    }
  }

  // The game's Exit: the engine's pause (OnApplicationPause: the game's and Unity's saves), every open file flushed,
  // then the process ends with all of its threads. Unity's own shutdown (nativeDone) is not run: it waited forever
  // in FMOD's Sound::release for a stream thread (the first hardware Exit froze there). A pause that does not come
  // back in 4 s is given up on.
  log_printf("[main] leaving: the engine's pause, then out");
  static Thread quit_timer;
  if (R_SUCCEEDED(threadCreate(&quit_timer, quit_timeout, NULL, NULL, 0x4000, 0x2c, -2)))
    threadStart(&quit_timer);
  unity_pause(1);
  s_pause_back = 1;
  leave_now();
  return 0;
}
