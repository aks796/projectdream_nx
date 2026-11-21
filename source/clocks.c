/* clocks.c -- the clocks the console runs at, read only, for the [clk] lines
 *
 * Never set here (no apm, no boost, no pcv writes): the player's overclock tool decides the clocks. Round 12 said
 * "stuck at stock clocks" and no log had them -- now the watchdog's thread reads them once a second (off the main
 * core; clkrst, read only, as sys-clk itself reads them) and each [perf] report says the range a stretch ran at.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <switch.h>

#include "log.h"

static int s_ok = -1;
static volatile u32 s_lo[3], s_hi[3], s_now[3];
static volatile unsigned s_samples;
static Mutex s_lock;

static void reset_locked(void) {
  for (int i = 0; i < 3; i++)
    s_lo[i] = ~0u, s_hi[i] = 0;
  s_samples = 0;
}

// once a second, on the watchdog's thread
void clocks_sample(void) {
  if (s_ok < 0) {
    s_ok = hosversionAtLeast(8, 0, 0) && R_SUCCEEDED(clkrstInitialize());
    mutexLock(&s_lock);
    reset_locked();
    mutexUnlock(&s_lock);
    if (!s_ok)
      log_printf("[clk] the clocks cannot be read (clkrst)");
  }
  if (!s_ok)
    return;
  static const PcvModuleId mods[3] = { PcvModuleId_CpuBus, PcvModuleId_GPU, PcvModuleId_EMC };
  u32 hz[3] = { 0, 0, 0 };
  for (int i = 0; i < 3; i++) {
    ClkrstSession sess;
    if (R_SUCCEEDED(clkrstOpenSession(&sess, mods[i], 3))) {
      clkrstGetClockRate(&sess, &hz[i]);
      clkrstCloseSession(&sess);
    }
  }
  mutexLock(&s_lock);
  for (int i = 0; i < 3; i++) {
    const u32 mhz = hz[i] / 1000000;
    s_now[i] = mhz;
    if (mhz < s_lo[i])
      s_lo[i] = mhz;
    if (mhz > s_hi[i])
      s_hi[i] = mhz;
  }
  s_samples++;
  mutexUnlock(&s_lock);
}

// with main.c's [perf] lines: the range since the last report
void clocks_report(void) {
  if (s_ok <= 0)
    return;
  mutexLock(&s_lock);
  if (s_samples) {
    char cpu[24], gpu[24], mem[24];
    const char *names[3] = { cpu, gpu, mem };
    for (int i = 0; i < 3; i++) {
      if (s_lo[i] == s_hi[i])
        snprintf((char *)names[i], 24, "%u", s_hi[i]);
      else
        snprintf((char *)names[i], 24, "%u-%u", s_lo[i], s_hi[i]);
    }
    log_printf("[clk] CPU %s MHz, GPU %s MHz, memory %s MHz (what the console ran at, %u readings)", cpu, gpu, mem,
               s_samples);
  }
  reset_locked();
  mutexUnlock(&s_lock);
}
