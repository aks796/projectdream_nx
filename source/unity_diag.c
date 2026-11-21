/* unity_diag.c -- what libunity.so decides about rendering, counted for the [perf] lines (config log_diag)
 *
 * IsSRPBatcherCompatible(RenderNode const&, Shader const&, int subshader, int pass): asked of each render node a
 * frame; the nodes it refuses are drawn one by one through the classic per-draw path (the 800 draws a frame of the
 * first level's colour pass, in hardware profiles PrepareValueParameters / ShaderPropertySheet lookups).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdbool.h>
#include <math.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "so_util.h"

#define IS_SRP_BATCHER_COMPATIBLE 0x80a358 // this libunity.so's

static bool (*s_compat_orig)(const void *node, const void *shader, int subshader, int pass);
static u64 s_yes, s_no;

static bool compat_counted(const void *node, const void *shader, int subshader, int pass) {
  const bool r = s_compat_orig(node, shader, subshader, pass);
  __atomic_add_fetch(r ? &s_yes : &s_no, 1, __ATOMIC_RELAXED);
  return r;
}

#define GET_GRAPHICS_SETTINGS 0xe44778 // GetGraphicsSettingsPtr()
#define USE_SRP_BATCHER_FLAG 0x17d5020  // ScriptableBatchRenderer's decision (UpdateUseSRPBatcher)

static uintptr_t s_unity_base;

// the engine's SRP Batcher decision now (1 on), and GraphicsSettings.useScriptableRenderPipelineBatching (+350)
int unity_diag_srp_batcher(int *setting) {
  if (!s_unity_base)
    return -1;
  const uint8_t *gs = ((const uint8_t *(*)(void))(s_unity_base + GET_GRAPHICS_SETTINGS))();
  *setting = gs ? gs[350] : -1;
  return *(volatile uint8_t *)(s_unity_base + USE_SRP_BATCHER_FLAG);
}

// ---- GameObject.Find (FindActiveTransformWithPath): 2-6% of the main thread on the console in every profile window
// (each call walks the scene). Counted by name -- which names, how often, how long -- to see which can be kept
// between calls; [find] line in main.c.
#define FIND_ACTIVE_TRANSFORM 0x8be130
static void *(*s_find_orig)(const char *path);
static Mutex s_find_lock;
static struct {
  char name[48];
  unsigned n;
  u64 ticks;
  unsigned found;
} s_finds[24];
static unsigned s_nfinds, s_find_calls;
static u64 s_find_ticks;

// Project Dream's patches look for three menu buttons to hide, about twice a second all three at once, wherever the
// game is: in a level each search walks every object there and finds nothing (Ryubing: these were the only Find
// calls; the console: 2-6% of the main thread, in bursts of three). For these names: after a search that finds
// nothing the next 16, then 64 calls are answered "not found" without one; a scene change (unity_tune_frame) or
// a search that finds the button starts again from every call; and one of them a frame at most (the others wait).
static const char *const s_pd_finds[] = { "[Default] HLCrossPromo_LaunchButton", "button_whats_new",
                                          "button_reset_save" };
static struct {
  unsigned miss, skip;
} s_pd_state[3];
static volatile int s_scene_changed;
static u64 s_pd_last, s_pd_skipped;

// 1: answer "not found" without a search; *k: which of the three (-1: another name)
// Project Dream's settings screen opening (its gp_menu_open, hooked in pd_shim.c): the searches go every time again
void unity_find_menu_opened(void) {
  __atomic_store_n(&s_scene_changed, 1, __ATOMIC_RELEASE);
}

static int pd_find_skip(const char *path, int *k) {
  *k = -1;
  for (int i = 0; i < 3 && path; i++)
    if (!strcmp(path, s_pd_finds[i]))
      *k = i;
  if (*k < 0)
    return 0;
  if (__atomic_exchange_n(&s_scene_changed, 0, __ATOMIC_ACQ_REL))
    memset(s_pd_state, 0, sizeof s_pd_state);
  if (s_pd_state[*k].skip) {
    s_pd_state[*k].skip--;
    return 1;
  }
  const u64 now = armGetSystemTick();
  if (s_pd_last && armTicksToNs(now - s_pd_last) < 12000000ull) // (another one this frame: next time)
    return 1;
  s_pd_last = now;
  return 0;
}

static void *find_counted(const char *path) {
  int k;
  if (pd_find_skip(path, &k)) {
    __atomic_add_fetch(&s_pd_skipped, 1, __ATOMIC_RELAXED);
    return NULL;
  }
  const u64 t0 = armGetSystemTick();
  void *r = s_find_orig(path);
  const u64 dt = armGetSystemTick() - t0;
  if (k >= 0) {
    if (r) {
      s_pd_state[k].miss = 0;
    } else {
      // (each one 8-26 ms in a level on the console, rounds 10-11: 16 calls, ~8 s, after the first miss, then 64,
      // ~30 s; a settings screen opening -- where the buttons are -- starts them again: unity_find_menu_opened)
      s_pd_state[k].skip = s_pd_state[k].miss ? 64 : 16;
      s_pd_state[k].miss++;
    }
  }
  mutexLock(&s_find_lock);
  s_find_calls++;
  s_find_ticks += dt;
  unsigned i = 0;
  while (i < s_nfinds && strncmp(s_finds[i].name, path ? path : "", sizeof s_finds[i].name - 1))
    i++;
  if (i == s_nfinds && s_nfinds < 24) {
    snprintf(s_finds[i].name, sizeof s_finds[i].name, "%s", path ? path : "");
    s_finds[i].n = 0, s_finds[i].ticks = 0, s_finds[i].found = 0;
    s_nfinds++;
  }
  if (i < s_nfinds) {
    s_finds[i].n++;
    s_finds[i].ticks += dt;
    s_finds[i].found += r != NULL;
  }
  mutexUnlock(&s_find_lock);
  return r;
}

void unity_find_report(void) {
  mutexLock(&s_find_lock);
  if (s_find_calls) {
    char line[900];
    int o = snprintf(line, sizeof line, "[find] GameObject.Find: %u calls, %.1f ms (%llu of Project Dream's spared);",
                     s_find_calls, armTicksToNs(s_find_ticks) / 1e6,
                     (unsigned long long)__atomic_exchange_n(&s_pd_skipped, 0, __ATOMIC_RELAXED));
    for (int k = 0; k < 6; k++) { // the costliest names
      int best = -1;
      for (unsigned i = 0; i < s_nfinds; i++)
        if (s_finds[i].n && (best < 0 || s_finds[i].ticks > s_finds[best].ticks))
          best = (int)i;
      if (best < 0 || o > (int)sizeof line - 120)
        break;
      o += snprintf(line + o, sizeof line - (size_t)o, " \"%s\" %ux %.1f ms (found %u),", s_finds[best].name,
                    s_finds[best].n, armTicksToNs(s_finds[best].ticks) / 1e6, s_finds[best].found);
      s_finds[best].n = 0;
    }
    log_printf("%s", line);
  }
  s_nfinds = 0, s_find_calls = 0, s_find_ticks = 0;
  mutexUnlock(&s_find_lock);
}

// Unity's vSyncCount held at 1 when something sets 0: the swapchain presents at the display's refresh whatever it
// says (vkdk FIFO), and Project Dream's VSync option -- now the menu's Dynamic Resolution (pd_shim.c) -- would set 0
// (its key no longer found). Unity's own pacing (WaitVSync, unity_pacing) stays as it was.
#define QS_SET_VSYNC_COUNT 0x55f35c
static void (*s_vsync_orig)(int);
static void vsync_held(int n) {
  s_vsync_orig(n == 0 && config.vsync ? 1 : n);
}

// FMOD's System::update -- channels' audibility, virtual voices, 3D, callbacks -- from AudioManager::Update once a
// frame: 3.1% of the main thread at 1785 MHz in round 14 (UpdateAudibilities over every channel the game keeps). FMOD
// asks for it once per game tick; 40 times a second at most here (config audio_update_hz; 0 = every frame): at 60
// fps every other frame, at 30 fps every frame. The mixing runs on its own thread and is not touched; a sound
// started on a skipped frame starts at the next update, 16 ms later at most.
#define FMOD_SYSTEM_UPDATE 0x16fe4f8
static int (*s_fmod_update_orig)(void *);
static u64 s_fmod_last;
static int fmod_update_capped(void *system) {
  const u64 now = armGetSystemTick();
  if (config.audio_update_hz > 0 && s_fmod_last &&
      now - s_fmod_last < armNsToTicks(1000000000ull / (u64)config.audio_update_hz) - armNsToTicks(4000000ull))
    return 0; // FMOD_OK
  s_fmod_last = now;
  return s_fmod_update_orig(system);
}

void unity_diag_init(uintptr_t unity_base) {
  s_unity_base = unity_base;
  const uintptr_t f = unity_base + IS_SRP_BATCHER_COMPATIBLE;
  if (*(const uint32_t *)f != 0xa9bd5ffeu) { // stp x30, x23, [sp, #-48]!
    log_printf("[diag] libunity.so is not the expected build: no SRP Batcher counts");
    return;
  }
  s_compat_orig = (void *)hook_arm64_replace(f, (uintptr_t)&compat_counted);
  const uintptr_t vs = unity_base + QS_SET_VSYNC_COUNT;
  if (*(const uint32_t *)vs == 0xa9bf4ffeu) // stp x30, x19, [sp, #-16]!
    s_vsync_orig = (void *)hook_arm64_replace(vs, (uintptr_t)&vsync_held);
  const uintptr_t fu = unity_base + FMOD_SYSTEM_UPDATE;
  if (*(const uint32_t *)fu == 0xd10083ffu) // sub sp, sp, #0x20
  {
    s_fmod_update_orig = (int (*)(void *))hook_arm64_replace(fu, (uintptr_t)&fmod_update_capped);
    log_printf("[diag] FMOD's update %d times a second at most%s", config.audio_update_hz,
               config.audio_update_hz > 0 ? "" : " (0: every frame)");
  }
  const uintptr_t fa = unity_base + FIND_ACTIVE_TRANSFORM;
  if (*(const uint32_t *)fa == 0xa9bd5ffeu) { // stp x30, x23, [sp, #-48]!
    mutexInit(&s_find_lock);
    s_find_orig = (void *)hook_arm64_replace(fa, (uintptr_t)&find_counted);
  }
}

// render nodes the SRP Batcher took and refused since the last call
void unity_diag_take(u64 *batched, u64 *refused) {
  *batched = __atomic_exchange_n(&s_yes, 0, __ATOMIC_RELAXED);
  *refused = __atomic_exchange_n(&s_no, 0, __ATOMIC_RELAXED);
}

// ---- tuning: QualitySettings the port holds (config lod_bias, shadow_distance; 0 = the game's), applied again
// whenever the game's quality code sets its own (checked once a second, on the main thread)
#define QS_GET_LOD_BIAS 0x55f130
#define QS_SET_LOD_BIAS 0x55f154

#include "config.h"

#define APP_GET_TARGET_FRAME_RATE 0x54251c // Application.targetFrameRate's getter and setter (icalls)
#define APP_SET_TARGET_FRAME_RATE 0x542520

#define GS_CURRENT_RENDER_PIPELINE 0x5c077c // GraphicsSettings.currentRenderPipeline (icall): the managed asset
#define URP_SHADOW_DISTANCE 168               // UniversalRenderPipelineAsset.m_ShadowDistance (its getter/setter)
#define TIME_SET_MAX_DELTA 0x5b19e0           // Time.maximumDeltaTime's setter (icall)
#define TIME_GET_FIXED_STEP 0x5b1998          // Time.fixedDeltaTime's getter and setter (icalls)
#define TIME_SET_FIXED_STEP 0x5b19ac

static float s_last_set = -1;

// the active URP asset's shadow distance field, or NULL (no asset yet)
static float *urp_shadow_distance(void) {
  uintptr_t asset = ((uintptr_t (*)(void))(s_unity_base + GS_CURRENT_RENDER_PIPELINE))();
  return asset ? (float *)(asset + URP_SHADOW_DISTANCE) : NULL;
}

void unity_tune_frame(unsigned frame) {
  if (!s_unity_base)
    return;
  // The game's 30 fps caps (FramerateConfiguration.Apply from its performance profile; cutscenes): held at 60, so a
  // console that can go faster does. (The frame after the game sets it; WaitVSync's [pacing] line logs each change.)
  if (config.fps_unlock) {
    const int fps = ((int (*)(void))(s_unity_base + APP_GET_TARGET_FRAME_RATE))();
    if (fps > 0 && fps < 55) {
      static int logged;
      if (logged++ < 8)
        log_printf("[tune] the game set targetFrameRate %d -> 60 (fps_unlock)", fps);
      ((void (*)(int))(s_unity_base + APP_SET_TARGET_FRAME_RATE))(60);
    }
  }
  if (frame % 60 != 0)
    return;
  { // Unity's async uploads (textures and meshes of a load, sent to the GPU a slice a frame): 4 ms and a 32 MB buffer
    // at least (Unity's 2 ms / 16 MB: round 12, a reward screen's 5 s load had AsyncUploadManager on its stack)
    const int slice = ((int (*)(void))(s_unity_base + 0x55f3f0))();
    const int buf = ((int (*)(void))(s_unity_base + 0x55f42c))();
    if (slice < 4 || buf < 32) {
      static int s_logged_up;
      if (!s_logged_up++)
        log_printf("[tune] async uploads: %d ms a frame, %d MB buffer (the game's) -> %d ms, %d MB", slice, buf,
                   slice < 4 ? 4 : slice, buf < 32 ? 32 : buf);
      if (slice < 4)
        ((void (*)(int))(s_unity_base + 0x55f414))(4);
      if (buf < 32)
        ((void (*)(int))(s_unity_base + 0x55f450))(32);
    }
  }
  { // the active scene: a new one lets Project Dream's button searches go every time again (find_counted)
    static int s_scene = -1;
    int sc = 0;
    ((void (*)(int *))(s_unity_base + 0x5befc4))(&sc); // SceneManager.GetActiveScene (icall)
    if (sc != s_scene) {
      s_scene = sc;
      __atomic_store_n(&s_scene_changed, 1, __ATOMIC_RELEASE);
    }
  }
  static int logged_lod, logged_shadow;
  // The most game time one frame may cover (Time.maximumDeltaTime): the game's 0.333 s let a slow frame (a load, a
  // hitch) run up to 20 physics steps of 1/60 s at once, which made the next frame slow too. Held at max_delta.
  if (config.max_delta > 0) {
    const float now = ((float (*)(void))(s_unity_base + 0x5b19cc))();
    if (now > config.max_delta + 0.0005f) {
      static int logged_md;
      if (logged_md++ < 2)
        log_printf("[tune] maximumDeltaTime %.3f s (the game's) -> %.3f s", now, config.max_delta);
      ((void (*)(float))(s_unity_base + TIME_SET_MAX_DELTA))(config.max_delta);
    }
  }
  // The physics step (Time.fixedDeltaTime, the game's 1/60 s): config fixed_step, off by default. Physics and the
  // game's FixedUpdate code (characters, the camera's recentering) run once a step; at 0.02 (50 Hz) a 60 fps frame
  // has 0.83 steps instead of 1 (Thorhax's SonicDreamTeam-NX has the same knob, SDT_FIXED_STEP). It can change how
  // movement feels, hence a choice to try. Only the game's own 1/60 is replaced: another value it sets (a slow-motion
  // moment) stands until it goes back to 1/60.
  if (config.fixed_step >= 0.01f && config.fixed_step <= 0.05f) {
    const float now = ((float (*)(void))(s_unity_base + TIME_GET_FIXED_STEP))();
    if (fabsf(now - 1.f / 60.f) < 0.0002f) {
      static int logged_fs;
      if (logged_fs++ < 4)
        log_printf("[tune] fixedDeltaTime %.4f s (the game's) -> %.4f s (%.0f Hz physics)", now, config.fixed_step,
                   1.f / config.fixed_step);
      ((void (*)(float))(s_unity_base + TIME_SET_FIXED_STEP))(config.fixed_step);
    }
  }
  if (config.lod_bias > 0) {
    const float now = ((float (*)(void))(s_unity_base + QS_GET_LOD_BIAS))();
    if (now != config.lod_bias) {
      if (logged_lod++ < 4)
        log_printf("[tune] LOD bias %.2f (the game's) -> %.2f", now, config.lod_bias);
      ((void (*)(float))(s_unity_base + QS_SET_LOD_BIAS))(config.lod_bias);
    }
  }
  // Real-time shadows: URP reads its own asset's shadowDistance (QualitySettings' is not used). 0 there = no shadow
  // casters culled and no shadow pass at all; a shorter one = fewer casters drawn into the shadow map each frame.
  const float want = !config.shadows ? 0.0f : config.shadow_distance > 0 ? config.shadow_distance : -1.0f;
  float *const urp = urp_shadow_distance();
  if (urp) {
    static float s_game = -1;
    const float now = *urp;
    if (now != want && now != s_last_set)
      s_game = now; // (the game's own, as it sets it: kept to log and to give back)
    if (want >= 0 && now != want) {
      if (logged_shadow++ < 4)
        log_printf("[tune] real-time shadow distance %.1f m (the game's) -> %.1f m%s", now, want,
                   want == 0 ? " (shadows off)" : "");
      *urp = want;
      s_last_set = want;
    } else if (want < 0 && s_last_set >= 0 && s_game >= 0 && now == s_last_set) {
      *urp = s_game; // (the knob turned back: the game's own again)
      s_last_set = -1;
    }
    static int logged_game;
    if (!logged_game && s_game >= 0) {
      logged_game = 1;
      log_printf("[tune] the game's real-time shadow distance: %.1f m", s_game);
    }
  }
}

// ---- frame pacing: Unity's WaitVSync (AndroidVSync::WaitForLastPresentationAndGetTimestamp's wait). It waits for
// the vsync after the last presented frame; behind vkdk's three-image FIFO a frame reaches the screen a vsync or more
// after it is submitted, so a frame that took 22 ms of work still waited for the next tick: 33.3 ms, a hard 30 fps
// (and 50 ms, 20 fps, past that). The console's measurements: 28-34% of the main thread in this wait at 30 fps.
// config vsync_wait 0: while the main thread's frames take longer than a vsync, the wait is skipped and the
// swapchain's FIFO paces the frames alone (no rounding down to 30); when they are quick, Unity waits as before (the
// least latency at 60). A game asking for 30 fps (targetFrameRate 30 or vSyncCount 2) is always waited for.
#define WAIT_VSYNC 0x9dac58
#define VSYNC_COUNTER 0x18658a8   // the counter WaitVSync waits on (the Choreographer's frames)
#define APP_TARGET_FRAME_RATE 0x54251c
#define QS_GET_VSYNC_COUNT 0x55f338

static int64_t (*s_wait_vsync_orig)(int64_t target);
static u64 s_vs_last, s_vs_skipped, s_vs_waited;

// drs.c's even 30 (config auto_30): every frame two refreshes long while the game asks for 60 and cannot make it --
// Unity's wait goes on to the refresh after the one it waits for (its target is the last shown frame's refresh plus
// one), and a frame later than that is not waited for. Also the time spent waiting, for drs.c (the frames' own work
// is the frame less this wait).
static int s_even30, s_game60 = 1;
static uint64_t s_vsync_wait_ns;

void unity_set_even_30(int on) {
  s_even30 = on;
}

int unity_game_wants_60(void) {
  return s_game60;
}

// The even 30's wait, from main.c's loop before each frame (round 17: inside Unity's vsync wait it never happened --
// Unity calls that wait only when a frame is early, and a frame running late is the one to hold): the frame starts
// two refreshes after the last one started, on the vsync counter Unity's own wait uses (the port's Choreographer
// pump), through that wait. A frame later than that starts at once and the count goes on from it.
void unity_even30_wait(void) {
  static int64_t s_frame_vs;
  if (!s_unity_base || !s_game60 || !(s_even30 || config.auto_30 == 2)) {
    s_frame_vs = 0;
    return;
  }
  const int64_t counter = *(volatile int64_t *)(s_unity_base + VSYNC_COUNTER);
  if (!s_frame_vs || counter >= s_frame_vs + 2 || counter < s_frame_vs) {
    s_frame_vs = counter;
    return;
  }
  const u64 t0 = armGetSystemTick();
  int64_t (*wait)(int64_t) = s_wait_vsync_orig ? s_wait_vsync_orig : (int64_t (*)(int64_t))(s_unity_base + WAIT_VSYNC);
  s_frame_vs = wait(s_frame_vs + 2);
  s_vsync_wait_ns += armTicksToNs(armGetSystemTick() - t0);
}

uint64_t unity_vsync_wait_take(void) {
  const uint64_t n = s_vsync_wait_ns;
  s_vsync_wait_ns = 0;
  return n;
}

static int64_t wait_vsync_paced(int64_t target) {
  const u64 now = armGetSystemTick();
  const u64 interval = s_vs_last ? now - s_vs_last : 0;
  s_vs_last = now;
  const int fps = ((int (*)(void))(s_unity_base + APP_TARGET_FRAME_RATE))();
  const int vsc = ((int (*)(void))(s_unity_base + QS_GET_VSYNC_COUNT))();
  const int game_wants_60 = vsc <= 1 && (fps <= 0 || fps >= 55);
  s_game60 = game_wants_60;
  static int s_fps_seen = -999, s_vsc_seen = -999;
  if (fps != s_fps_seen || vsc != s_vsc_seen) { // (the game's frame rate request: its settings, menus, cutscenes)
    s_fps_seen = fps, s_vsc_seen = vsc;
    log_printf("[pacing] the game asks for targetFrameRate %d, vSyncCount %d (%s)", fps, vsc,
               game_wants_60 ? "60 fps" : vsc >= 2 ? "60 / vSyncCount fps" : "its own cap");
  }
  // behind: the last frame took over a vsync and a bit (17.5 ms)
  if (game_wants_60 && interval > armNsToTicks(17500000)) {
    s_vs_skipped++;
    return *(volatile int64_t *)(s_unity_base + VSYNC_COUNTER);
  }
  s_vs_waited++;
  const int64_t r = s_wait_vsync_orig(target);
  s_vs_last = armGetSystemTick(); // (the wait is not the next frame's work)
  s_vsync_wait_ns += armTicksToNs(s_vs_last - now);
  return r;
}

void unity_pacing_init(void) {
  if (!s_unity_base || config.vsync_wait)
    return;
  const uintptr_t f = s_unity_base + WAIT_VSYNC;
  if (*(const uint32_t *)f != 0xf81d0ffeu) { // str x30, [sp, #-48]!
    log_printf("[pacing] libunity.so is not the expected build: Unity's vsync wait left as it is");
    return;
  }
  s_wait_vsync_orig = (void *)hook_arm64_replace(f, (uintptr_t)&wait_vsync_paced);
  log_printf("[pacing] Unity's vsync wait skipped while frames run late (the swapchain paces them)");
}

// The engine's settings that decide each frame's work, logged once in play: the fixed (physics) step and the most a
// frame may catch up (several FixedUpdates a frame when frames are long), physics' transform syncing, the quality
// level. (Getters of Unity's own bindings: no state changed.)
void unity_log_settings(void) {
  if (!s_unity_base)
    return;
  const float fixed = ((float (*)(void))(s_unity_base + 0x5b1998))();
  const float maxdt = ((float (*)(void))(s_unity_base + 0x5b19cc))();
  const float scale = ((float (*)(void))(s_unity_base + 0x5b1a48))();
  const int sim = ((int (*)(void))(s_unity_base + 0x5f4f94))();
  const int autosync = ((int (*)(void))(s_unity_base + 0x5f58c0))() & 1;
  const int level = ((int (*)(void))(s_unity_base + 0x55fdac))();
  log_printf("[unity] fixedDeltaTime %.4f s (%.0f Hz), maximumDeltaTime %.3f, timeScale %.2f, physics simulation mode %d, "
             "autoSyncTransforms %d, quality level %d", fixed, fixed > 0 ? 1.0 / fixed : 0.0, maxdt, scale, sim, autosync,
             level);
}

void unity_pacing_take(u64 *skipped, u64 *waited) {
  *skipped = s_vs_skipped;
  *waited = s_vs_waited;
  s_vs_skipped = s_vs_waited = 0;
}
