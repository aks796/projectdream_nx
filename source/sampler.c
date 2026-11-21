/* sampler.c -- debug: where the game's main thread is, every few ms, written
 * straight to the emulator's log (svcOutputDebugString: Ryubing writes it at
 * once), for crashes the emulator reports without an address (an instruction
 * abort under its hypervisor). config sample_main <ms>, 0 = off.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "pd_shim.h"
#include "so_util.h"

static Handle s_target;
static Thread s_thread;

static void name(char *out, size_t n, uintptr_t a) {
  uintptr_t base = 0;
  const char *m = so_name_at_unlocked(a, &base);
  if (m) {
    snprintf(out, n, "%s+%lx", m, (unsigned long)(a - base));
    return;
  }
  if (pd_shim_name(a, out, n))
    return;
  snprintf(out, n, "%lx", (unsigned long)a);
}

static void run(void *arg) {
  (void)arg;
  const u64 period = (u64)config.sample_main * 1000000ull;
  char line[300], pc[96], lr[96];
  unsigned k = 0;
  for (;;) {
    svcSleepThread(period);
    pd_shim_debug();
    if (R_FAILED(svcSetThreadActivity(s_target, ThreadActivity_Paused)))
      continue;
    ThreadContext ctx;
    const Result rc = svcGetThreadContext3(&ctx, s_target);
    svcSetThreadActivity(s_target, ThreadActivity_Runnable);
    if (R_FAILED(rc))
      continue;
    name(pc, sizeof pc, ctx.pc.x);
    name(lr, sizeof lr, ctx.lr);
    const int len = snprintf(line, sizeof line, "[sample %u] pc %s lr %s sp %lx", k++, pc, lr, (unsigned long)ctx.sp);
    svcOutputDebugString(line, len);
  }
}

void sampler_start(Handle main_thread) {
  if (config.sample_main <= 0)
    return;
  s_target = main_thread;
  if (R_SUCCEEDED(threadCreate(&s_thread, run, NULL, NULL, 0x4000, 0x20, -2)))
    threadStart(&s_thread);
  log_printf("[sample] the main thread every %d ms, into the emulator's log", config.sample_main);
}
