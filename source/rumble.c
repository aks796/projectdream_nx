/* rumble.c -- HD rumble pulses (see rumble.h)
 *
 * The pulse mixer of mkt_nx's race rumble: a pulse has two bands (low ~100-160
 * Hz, high ~200-330 Hz: HD rumble's own), a length and a shape; a thread of its
 * own mixes the pulses playing and sends the controller's value 200 times a
 * second while any plays (sending blocks: never on the game's thread),
 * nothing when none does. Sonic Dream Team's haptics (Project Dream's
 * Haptics.play kinds, java_sdt.c) are its pulses here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <math.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "rumble.h"

#define TICK_NS 5000000ll // 200 Hz
#define MAX_PULSES 12

int rumble_mute;

typedef struct {
  u64 t0;           // its start (ticks); 0 = none
  float ms;         // how long
  float lo, flo;    // low band: amplitude (0..1) at its peak, frequency
  float hi, fhi;    // high band
  int shape;
} Pulse;

static Pulse s_pulses[2][MAX_PULSES];
static Mutex s_mutex;
static CondVar s_cv;
static Thread s_thread;
static int s_on;

// --- the controllers ----------------------------------------------------------------------------

typedef struct {
  HidNpadIdType id;
  u32 style; // the style the handles are for (0: none yet)
  int n;
  HidVibrationDeviceHandle h[2];
  int sent_zero;
} Dev;

// player 1: the console's Joy-Cons (handheld) and No. 1; player 2: No. 2
static Dev s_dev[2][2] = {
  { { .id = HidNpadIdType_Handheld }, { .id = HidNpadIdType_No1 } },
  { { .id = HidNpadIdType_No2 }, { .id = (HidNpadIdType)-1 } },
};

static u32 style_now(HidNpadIdType id, int *n) {
  const u32 set = hidGetNpadStyleSet(id);
  static const u32 order[] = { HidNpadStyleTag_NpadFullKey, HidNpadStyleTag_NpadHandheld, HidNpadStyleTag_NpadJoyDual,
                               HidNpadStyleTag_NpadJoyLeft, HidNpadStyleTag_NpadJoyRight };
  for (size_t i = 0; i < sizeof order / sizeof *order; i++)
    if (set & order[i]) {
      *n = order[i] == HidNpadStyleTag_NpadJoyLeft || order[i] == HidNpadStyleTag_NpadJoyRight ? 1 : 2;
      return order[i];
    }
  *n = 0;
  return 0;
}

static void send(Dev *d, const HidVibrationValue *v, int zero) {
  if ((int)d->id < 0)
    return;
  int n;
  const u32 style = style_now(d->id, &n);
  if (!style) {
    d->style = 0;
    return;
  }
  if (style != d->style) {
    if (R_FAILED(hidInitializeVibrationDevices(d->h, n, d->id, (HidNpadStyleTag)style))) {
      d->style = 0;
      return;
    }
    d->style = style, d->n = n, d->sent_zero = 0;
  }
  if (zero && d->sent_zero)
    return;
  HidVibrationValue vs[2] = { *v, *v };
  hidSendVibrationValues(d->h, vs, d->n);
  d->sent_zero = zero;
}

// --- the pulses ----------------------------------------------------------------------------------

// a pulse's strength (0..1) t ms in
static float envelope(const Pulse *p, float t) {
  const float x = t / p->ms; // 0..1
  switch (p->shape) {
  case SHAPE_CLICK: // full, then gone (a click: short and sharp)
    return x < 0.6f ? 1.f : (1.f - x) / 0.4f;
  case SHAPE_THUD: // hits hard, falls away fast
    return (1.f - x) * (1.f - x);
  case SHAPE_ROUGH: // shaking, dying away
    return (1.f - x) * (0.7f + 0.3f * sinf(t * 0.18f));
  default: { // a surge: up in 20 ms, then down
    const float a = 20.f / p->ms;
    return x < a ? x / a : 1.f - (x - a) / (1.f - a);
  }
  }
}

// a player's controller value now (amplitudes summed over the pulses
// playing, each band's frequency the strongest's); 0: none playing
static int mix(int pl, u64 now, HidVibrationValue *v) {
  float lo = 0, hi = 0, blo = 0, bhi = 0, flo = 160, fhi = 320;
  int any = 0;
  for (int i = 0; i < MAX_PULSES; i++) {
    Pulse *p = &s_pulses[pl][i];
    if (!p->t0)
      continue;
    if (now < p->t0) { // (a pattern's later pulse: not yet, but the thread stays up for it)
      any = 1;
      continue;
    }
    const float t = (float)armTicksToNs(now - p->t0) / 1e6f;
    if (t >= p->ms) {
      p->t0 = 0;
      continue;
    }
    any = 1;
    const float e = envelope(p, t);
    const float l = p->lo * e, h = p->hi * e;
    lo += l, hi += h;
    if (l > blo)
      blo = l, flo = p->flo;
    if (h > bhi)
      bhi = h, fhi = p->fhi;
  }
  const float k = (float)config.rumble / 100.f;
  lo *= k, hi *= k;
  if (lo + hi > 1.f) { // (the two bands together at most full)
    const float s = 1.f / (lo + hi);
    lo *= s, hi *= s;
  }
  v->amp_low = lo, v->freq_low = flo, v->amp_high = hi, v->freq_high = fhi;
  return any;
}

static void rumble_thread(void *arg) {
  (void)arg;
  int was[2] = { 0, 0 };
  for (;;) {
    mutexLock(&s_mutex);
    const u64 now = armGetSystemTick();
    HidVibrationValue v[2];
    int any[2];
    for (int p = 0; p < 2; p++)
      any[p] = mix(p, now, &v[p]);
    if (!any[0] && !any[1] && !was[0] && !was[1]) { // (nothing playing: asleep until a pulse)
      condvarWait(&s_cv, &s_mutex);
      mutexUnlock(&s_mutex);
      continue;
    }
    mutexUnlock(&s_mutex);
    for (int p = 0; p < 2; p++) {
      if (!any[p] && !was[p])
        continue;
      if (!any[p])
        v[p] = (HidVibrationValue){ 0, 160, 0, 320 };
      for (int d = 0; d < 2; d++)
        send(&s_dev[p][d], &v[p], !any[p]);
      was[p] = any[p];
    }
    svcSleepThread(TICK_NS);
  }
}

static void pulse(int pl, float delay_ms, float ms, float lo, float flo, float hi, float fhi, int shape);

void rumble_play(int player, float ms, float lo, float flo, float hi, float fhi, int shape) {
  pulse(player, 0.f, ms, lo, flo, hi, fhi, shape);
}

void rumble_play_at(int player, float delay_ms, float ms, float lo, float flo, float hi, float fhi, int shape) {
  pulse(player, delay_ms, ms, lo, flo, hi, fhi, shape);
}

static void pulse(int pl, float delay_ms, float ms, float lo, float flo, float hi, float fhi, int shape) {
  if (!s_on || pl < 0 || pl > 1 || config.rumble <= 0 || rumble_mute > 0)
    return;
  mutexLock(&s_mutex);
  const u64 now = armGetSystemTick();
  const u64 start = now + armNsToTicks((u64)(delay_ms > 0.f ? delay_ms * 1e6f : 0.f));
  int slot = 0;
  float least = 1e9f;
  for (int i = 0; i < MAX_PULSES; i++) {
    const Pulse *p = &s_pulses[pl][i];
    if (!p->t0) {
      slot = i;
      break;
    }
    const float left = p->t0 > now ? 1e8f : p->ms - (float)armTicksToNs(now - p->t0) / 1e6f; // (the nearest its end)
    if (left < least)
      least = left, slot = i;
  }
  s_pulses[pl][slot] = (Pulse){ start, ms, lo, flo, hi, fhi, shape };
  condvarWakeOne(&s_cv);
  mutexUnlock(&s_mutex);
}

void rumble_clear(int player) {
  if (!s_on || player < 0 || player > 1)
    return;
  mutexLock(&s_mutex);
  for (int i = 0; i < MAX_PULSES; i++)
    s_pulses[player][i].t0 = 0;
  condvarWakeOne(&s_cv);
  mutexUnlock(&s_mutex);
}

// --- set-up ---------------------------------------------------------------------------------------

void rumble_init(void) {
  mutexInit(&s_mutex);
  condvarInit(&s_cv);
  hidPermitVibration(true);
  // (cores 1-2: core 0 is the game's main thread's alone)
  if (R_FAILED(threadCreate(&s_thread, rumble_thread, NULL, NULL, 0x4000, 0x2B, 2)) ||
      R_FAILED(svcSetThreadCoreMask(s_thread.handle, 2, 0x6)) || R_FAILED(threadStart(&s_thread))) {
    log_printf("[rumble] off: its thread did not start");
    return;
  }
  s_on = 1;
  log_printf("[rumble] HD rumble on, strength %d%%", config.rumble);
}
