/* drs.c -- dynamic resolution from the GPU's own time per frame, and an even 30 when 60 is out of reach
 *
 * The game's 3D picture is drawn at URP's render scale (UniversalRenderPipelineAsset.renderScale, a float at +96 of
 * the active pipeline asset) and stretched to the screen at the end. Project Dream's Resolution Scale slider sets it:
 * its option's value v (0..50) is a scale of (v + 50) / 100, 0.50 to 1.00 (round 15: read as v / 50 before, so 0.70
 * on the slider was taken for 0.40 and the port drew at that). Project Dream applies the slider once when it is changed
 * (its settings rebuild, a moment later) and, as it was, again every frame its value differs -- the port patches that
 * every-frame check off (pd_shim.c), so the two never take turns: the port keeps the asset at the slider's value
 * times its own factor (s_k), never above the slider.
 *
 * GPU time: vkdk writes a GPU timestamp before and after each submission's work (vkdkGpuBusyTotalNs), the GPU's busy
 * time per frame without its idle gaps -- what dynamic resolution needs, and what the CPU's waits for the GPU (round
 * 11-14's measure) could only hint at. Every half second, the frames' average GPU time G against a budget of 90% of
 * the frame (92%: 15.3 ms at 60 fps, 30.7 ms when the game or the even 30 below caps it):
 *  - G over the budget: the scale goes at once to what the budget needs (a frame's pixels go with the scale squared:
 *    scale * sqrt(budget / G)), in steps of 0.05, as low as config dynamic_res_min (absolute; 0.70 by default --
 *    round 17: lower looked grainy);
 *  - G under 70% of the budget for 2 s: one step (0.05) up, back to the slider's at most;
 *  - nothing for a second after a change (URP makes its targets again at each new size).
 * A CPU-bound frame has the GPU idle part of it: G stays low and the resolution stays up (round 13's waits-based
 * version lowered it for nothing then).
 * The game's own writes of a scale (its phone profile's 0.50) are put back at once, as Project Dream did every frame;
 * only a value the game keeps writing (a cutscene's) stands while it does.
 * On or off: the Graphics menu's Dynamic Resolution (pd_dynamic_res_option; config dynamic_res when it is not there,
 * and dynamic_res 2 -- held at the floor, a debug mode -- over it). Off: the slider's scale as it is.
 *
 * An even 30 (the menu's Frame Rate Auto -- pd_shim.c: 30 / 60 / Auto, held here; config auto_30 when the menu has
 * no such choice): with the game asking for 60 and the frames not making it -- under 45 fps for 4 s, the
 * resolution as low as it goes or the GPU not the reason -- a 60 Hz screen shows them unevenly, a frame one refresh
 * and the next two (round 15: "30 fps comfortably at stock clocks but it stutters" = 36-43 fps). Then every frame is
 * held to two refreshes (unity_diag.c's vsync wait): an even 30. Back to 60 when the frames' own work -- the frame
 * less its wait for the display -- fits 60 with room (under 15 ms, and the GPU's time at the lowest scale under 14 ms)
 * for 6 s. At least 6 s in each state.
 *
 * [drs] line with the [perf] ones: the scale on average and its range, the GPU's time per frame, the even 30's state.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "pd_shim.h"
#include "vkdk.h"

#define TARGET_FRAME_RATE 0x54251c          // Application.targetFrameRate's getter and setter (icalls)
#define SET_TARGET_FRAME_RATE 0x542520
#define GS_CURRENT_RENDER_PIPELINE 0x5c077c // GraphicsSettings.currentRenderPipeline (icall): the managed asset
#define URP_RENDER_SCALE 96                 // UniversalRenderPipelineAsset.m_RenderScale (its getter/setter)
#define STEP 0.05f
#define WINDOW_NS 500000000ull

void unity_set_even_30(int on);                // unity_diag.c: every frame two refreshes long
uint64_t unity_vsync_wait_take(void);          // unity_diag.c: ns in Unity's vsync wait since the last call
int unity_game_wants_60(void);                 // unity_diag.c: the game's own request is 60 fps

static uintptr_t s_unity;
static float s_k = 1.f;          // the port's factor on the slider's scale
static float s_written = -1.f;   // what the port last wrote to the asset
static uint64_t s_foreign_since;  // the game's own last write of a scale (neither the port's nor the slider's)
static unsigned s_foreign_writes;  // its writes within a second of each other
static int s_game_holds;           // it keeps writing it: its value stands

void drs_init(uintptr_t unity_base) {
  s_unity = unity_base;
}

static float *render_scale(void) {
  const uintptr_t asset = ((uintptr_t (*)(void))(s_unity + GS_CURRENT_RENDER_PIPELINE))();
  return asset ? (float *)(asset + URP_RENDER_SCALE) : NULL;
}

// this half second's sums
static uint64_t s_win_start, s_last_gpu, s_win_gpu, s_win_period;
static unsigned s_win_n, s_win_bad;
static unsigned s_good, s_hold;           // (in windows)
// the even 30
static int s_even30;
static uint64_t s_even30_since;
static unsigned s_slow_windows, s_fits_windows;
// the [drs] line's
static float s_sum_scale, s_lo = 9, s_hi = 0, s_slider = 1.f, s_last_g;
static unsigned s_frames, s_even30_frames;

static float quantize_down(float s) {
  return floorf(s / STEP + 0.001f) * STEP;
}

// once a frame, on the main thread, with the frame's length
void drs_frame(uint64_t period_ns) {
  if (!s_unity)
    return;
  float *const rs = render_scale();
  if (!rs)
    return;
  const int menu = pd_dynamic_res_option();
  const int mode = config.dynamic_res == 2 ? 2 : menu >= 0 ? menu : config.dynamic_res;
  const float sl = pd_render_scale_option();
  const float base = sl >= 0.5f && sl <= 1.f ? sl : 1.f;
  if (fabsf(base - s_slider) > 0.001f) {
    log_printf("[drs] Resolution Scale %.2f; dynamic resolution %s (down to %.2f)", base, mode ? "on" : "off",
               config.dynamic_res_min);
    s_slider = base;
  }
  const float now = *rs;
  const uint64_t tick = armGetSystemTick();
  // A value written by someone else: the slider (Project Dream's settings rebuild) or the game's own. The game sets
  // 0.50 of its own now and then (its phone profile's scale: round 17 found it held for minutes, the picture at half
  // resolution), and Project Dream used to put the slider's back every frame: the port does that now -- unless the
  // game keeps writing its value (3 times within a second: it means it, a cutscene's own scale), when it stands
  // until the game has written nothing for a second (no two writers taking turns: round 12's comic flicker).
  if (s_written < 0.f || fabsf(now - s_written) > 0.001f) {
    if (fabsf(now - base) >= 0.006f) {
      s_foreign_writes = armTicksToNs(tick - s_foreign_since) < 1000000000ull ? s_foreign_writes + 1 : 1;
      s_foreign_since = tick;
      if (s_foreign_writes == 3 && !s_game_holds) {
        s_game_holds = 1;
        log_printf("[drs] the game holds a render scale of its own: %.2f (Resolution Scale %.2f)", now, base);
      }
      static unsigned s_logged;
      if (s_logged++ < 12)
        log_printf("[drs] the game set a render scale of its own: %.2f (put back to the port's)", now);
    }
    s_written = -1.f;
  }
  if (s_game_holds && armTicksToNs(tick - s_foreign_since) > 1000000000ull) {
    s_game_holds = 0;
    log_printf("[drs] the game let go of its own render scale");
  }
  const float floor_abs = config.dynamic_res_min > 0.2f ? config.dynamic_res_min : 0.7f;
  const float lo = fminf(base, floor_abs); // (a slider under the floor: the slider's own)

  // ---- this half second's GPU time and frames
  const uint64_t gpu = vkdkGpuBusyTotalNs();
  if (s_last_gpu && gpu >= s_last_gpu)
    s_win_gpu += gpu - s_last_gpu;
  s_last_gpu = gpu;
  if (period_ns > 0 && period_ns < 250000000ull) {
    s_win_period += period_ns;
    s_win_n++;
  } else {
    s_win_bad = 1; // (a load, a pause)
  }
  if (!s_win_start)
    s_win_start = tick;
  // the menu's Frame Rate (pd_shim.c: Project Dream no longer sets it): 30, or 60 (Auto too), held over anything
  // else -- the game's own requests included, as Project Dream's every-few-seconds setting did
  const int choice = pd_fps_choice();
  int fps_req = ((int (*)(void))(s_unity + TARGET_FRAME_RATE))();
  if (choice >= 0) {
    const int want_fps = choice == 0 ? 30 : 60;
    if (fps_req != want_fps) {
      ((void (*)(int))(s_unity + SET_TARGET_FRAME_RATE))(want_fps);
      fps_req = want_fps;
    }
  }
  const int auto30 = choice >= 0 ? choice == 2 : config.auto_30;
  const int game60 = unity_game_wants_60();
  const float frame_ms = s_even30 && game60 ? 33.333f : 1000.f / (fps_req > 0 && fps_req < 55 ? (float)fps_req : 60.f);
  const float budget = frame_ms * 0.92f;

  if (armTicksToNs(tick - s_win_start) >= WINDOW_NS && s_win_n >= 4) {
    const float g = (float)s_win_gpu / 1e6f / (float)s_win_n;          // GPU ms a frame
    const float p = (float)s_win_period / 1e6f / (float)s_win_n;       // frame ms
    const float w = (float)unity_vsync_wait_take() / 1e6f / (float)s_win_n; // the display wait a frame
    s_last_g = g;
    const float scale = *rs > 0.f ? *rs : base;
    // ---- dynamic resolution
    if (s_win_bad) {
      s_good = 0;
    } else if (s_hold) {
      s_hold--;
    } else if (mode && !s_game_holds && g > budget) {
      const float want = fmaxf(lo, quantize_down(scale * sqrtf(budget / g)));
      if (want < scale - 0.001f) {
        s_k = want / base;
        s_hold = 2;
      }
      s_good = 0;
    } else if (mode && g < budget * 0.7f) {
      if (++s_good >= 4 && s_k < 1.f) {
        s_k = fminf(1.f, (quantize_down(base * s_k + 0.001f) + STEP) / base);
        s_good = 0;
        s_hold = 2;
      }
    } else {
      s_good = 0;
    }
    // ---- the even 30
    if (auto30 && game60 && !s_win_bad) {
      const float g_low = scale > 0.f ? g * (lo / scale) * (lo / scale) : g; // the GPU's time at the lowest scale
      const int res_spent = !mode || s_k * base <= lo + 0.001f || g <= budget;
      const uint64_t in_state = armTicksToNs(tick - s_even30_since);
      if (!s_even30) {
        s_slow_windows = p > 1000.f / 45.f && res_spent ? s_slow_windows + 1 : 0;
        if (s_slow_windows >= 8 && in_state > 6000000000ull) {
          s_even30 = 1, s_even30_since = tick, s_slow_windows = 0, s_fits_windows = 0;
          unity_set_even_30(1);
          log_printf("[drs] frames at %.1f ms (%.0f fps) with the game asking for 60: an even 30 (frames %.1f ms of "
                     "work, the GPU %.1f ms)", p, 1000.f / p, p - w, g);
        }
      } else {
        s_fits_windows = p - w < 15.f && g_low < 14.f ? s_fits_windows + 1 : 0;
        if (s_fits_windows >= 12 && in_state > 6000000000ull) {
          s_even30 = 0, s_even30_since = tick, s_fits_windows = 0, s_slow_windows = 0;
          unity_set_even_30(0);
          log_printf("[drs] frames' work %.1f ms, the GPU %.1f ms at the lowest scale: back to 60", p - w, g_low);
        }
      }
    } else if (s_even30 && (!auto30 || !game60)) { // (30 chosen, the game capping itself, or Auto off)
      s_even30 = 0, s_even30_since = tick;
      unity_set_even_30(0);
    }
    s_win_start = tick, s_win_gpu = 0, s_win_period = 0, s_win_n = 0, s_win_bad = 0;
  }
  if (mode == 2) // (debug: held at the lowest, to see what it does)
    s_k = lo / base;

  // ---- the asset's value
  float want = mode ? fmaxf(lo, base * s_k) : base;
  if (s_game_holds) {
    want = now; // (the game's own, while it keeps writing it)
  } else if (fabsf(now - want) > 0.0005f) {
    *rs = want;
    s_written = want;
  } else {
    s_written = want;
  }
  s_sum_scale += want;
  s_lo = fminf(s_lo, want);
  s_hi = fmaxf(s_hi, want);
  s_frames++;
  s_even30_frames += s_even30;
}

// for main.c's [perf] lines
void drs_report(void) {
  if (s_frames)
    log_printf("[drs] render scale %.0f%% on average (%.0f-%.0f%%; Resolution Scale %.0f%%), the GPU %.1f ms a frame%s",
               100.f * s_sum_scale / (float)s_frames, 100.f * s_lo, 100.f * s_hi, 100.f * s_slider, s_last_g,
               s_even30_frames ? (s_even30_frames == s_frames ? "; an even 30" : "; an even 30 part of the time")
                               : "");
  s_frames = 0, s_even30_frames = 0, s_sum_scale = 0, s_lo = 9, s_hi = 0;
}
