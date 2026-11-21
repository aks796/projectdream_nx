/* prof.c -- where the main thread's time goes, in races and menus, on hardware
 *
 * The first hardware runs: races at 25 ms a frame with the main thread (the
 * engine's frame: the game's C#, the engine, Mesa building the GL commands)
 * busy 85-90 % of the time, and the GPU idle (presenting takes 0.4 ms). This
 * says which code. In a race, and in the menus (the home menu ran ~45-50 fps
 * with the main thread busy and 4-5 draws), every 30 s for 3 s from 12 s into
 * each, a thread of its own pauses
 * the main thread every 2 ms for a moment (as the watchdog does), reads
 * where it is and lets it go. The window's samples go to debug.log
 * ([prof] lines): by library, then by 256-byte piece of code, the busiest
 * first, named as module+offset (libil2cpp.so offsets by the il2cpp dump:
 * tools/il2cpp_addr.py; nro+ offsets against mkt_nx.elf). A sample taken
 * while a collection has the thread paused is skipped. config profile 0
 * turns it off. The render thread (the engine's own, when it has one:
 * egl_shim.c names it at its first present) is sampled the same way, in turn
 * with the main thread, and reported after it.
 * Each sample also keeps its callers (the link register and the frame
 * records up the stack), for two more lists: the waits (the thread in a
 * system call: a lock, a condition, the GPU's fence, IPC) by who waits, and
 * the port's own time (Mesa, the GL calls) by the game's code that called
 * into it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "prof.h"
#include "pd_shim.h"
static int race_drive_racing(void) { return 0; } // (mkt_nx: in a race; one phase here)
#include "so_util.h"

#define WINDOW_NS 3000000000ull
#define EVERY_NS 30000000000ull
#define FIRST_NS 12000000000ull // into a race (or the menus) before the first window
#define STEP_NS 2000000ll
#define MAX_SAMPLES 2048
#define TOP 24
#define DEPTH 32 // callers kept: the link register, then the frame records'

static Handle s_main = INVALID_HANDLE;
static volatile Handle s_render = INVALID_HANDLE; // the engine's render thread (prof_render_thread)
static Thread s_thread;
// each thread's samples: 0 the main thread, 1 the render thread (s_t: the one at hand)
static uintptr_t s_pcs[2][MAX_SAMPLES], s_ups[2][MAX_SAMPLES][DEPTH];
static int s_ns[2];
static int s_t;
static const char *s_phase = "race"; // the window's: "race" or "menu"
#define s_pc s_pcs[s_t]
#define s_up s_ups[s_t]
#define s_n s_ns[s_t]
static uintptr_t s_stack_los[2], s_stack_his[2]; // each thread's stack (frame records are read only there)
#define s_stack_lo s_stack_los[s_t]
#define s_stack_hi s_stack_his[s_t]

void prof_render_thread(Handle h) {
  const Handle r = h == s_main ? INVALID_HANDLE : h; // (the main thread presenting: no render thread)
  if (r != s_render) {
    s_render = r;
    log_printf("[prof] %s", r != INVALID_HANDLE ? "the render thread is sampled too" : "no render thread");
  }
}

// the frame records from fp up: return addresses, while they stay on the stack and go up it
static void callers(const ThreadContext *ctx, uintptr_t *up) {
  memset(up, 0, sizeof(uintptr_t) * DEPTH);
  up[0] = ctx->lr;
  // its stack, asked each time (a thread made anew, as the render thread is, has another)
  MemoryInfo mi;
  u32 pi;
  if (R_FAILED(svcQueryMemory(&mi, &pi, ctx->sp)) || !(mi.perm & Perm_R))
    return;
  s_stack_lo = mi.addr, s_stack_hi = mi.addr + mi.size;
  uintptr_t fp = ctx->fp;
  for (int d = 1; d < DEPTH; d++) {
    // (fp - its end, not fp + 16: a register used for data, ~0 - 7, wraps fp + 16 round to 8)
    if (fp < ctx->sp || fp >= s_stack_hi || s_stack_hi - fp < 16 || (fp & 7))
      break;
    const uintptr_t next = ((const uintptr_t *)fp)[0], ret = ((const uintptr_t *)fp)[1];
    if (!ret)
      break;
    up[d] = ret;
    if (next <= fp)
      break;
    fp = next;
  }
}

static void sample(Handle h) {
  if (s_n >= MAX_SAMPLES)
    return;
  // already paused (a collection, the watchdog): left alone
  if (R_FAILED(svcSetThreadActivity(h, ThreadActivity_Paused)))
    return;
  ThreadContext ctx;
  for (int k = 0; k < 4; k++) { // the pause holds once it leaves its core
    if (R_SUCCEEDED(svcGetThreadContext3(&ctx, h))) {
      s_pc[s_n] = ctx.pc.x;
      callers(&ctx, s_up[s_n]); // (still paused: its stack holds still)
      s_n++;
      break;
    }
    svcSleepThread(20000);
  }
  svcSetThreadActivity(h, ThreadActivity_Runnable);
}

// libnx's system call stubs (one file of them): a sample there is a wait
static uintptr_t s_svc_lo, s_svc_hi;

static void svc_range(void) {
  const uintptr_t f[] = { (uintptr_t)svcSetHeapSize, (uintptr_t)svcSleepThread, (uintptr_t)svcWaitSynchronization,
                          (uintptr_t)svcArbitrateLock, (uintptr_t)svcWaitProcessWideKeyAtomic,
                          (uintptr_t)svcSendSyncRequest, (uintptr_t)svcWaitForAddress, (uintptr_t)svcReplyAndReceive };
  s_svc_lo = s_svc_hi = f[0];
  for (size_t i = 1; i < sizeof f / sizeof f[0]; i++) {
    s_svc_lo = f[i] < s_svc_lo ? f[i] : s_svc_lo;
    s_svc_hi = f[i] > s_svc_hi ? f[i] : s_svc_hi;
  }
  s_svc_hi += 0x20;
}

static int in_nro(uintptr_t v) {
  extern char _start[];
  return v >= (uintptr_t)_start && v - (uintptr_t)_start < (64u << 20);
}

// a code address as module+offset, and its module's name alone
static void name_of(uintptr_t v, char *buf, size_t n, char *mod, size_t mn) {
  extern char _start[];
  uintptr_t base;
  const char *name = so_name_at_unlocked(v, &base);
  if (name) {
    snprintf(buf, n, "%s+0x%lx", name, (unsigned long)(v - base));
    snprintf(mod, mn, "%s", name);
  } else if (v >= (uintptr_t)_start && v - (uintptr_t)_start < (64u << 20)) {
    snprintf(buf, n, "nro+0x%lx", (unsigned long)(v - (uintptr_t)_start));
    snprintf(mod, mn, "nro (the port, Mesa, libnx)");
  } else if (pd_shim_name(v, buf, n)) {
    // Project Dream's code memory: the Mac game's GameAssembly (its symbol = offset - 0x40000, the atomics' stubs
    // first) or the loader's own trampolines
    snprintf(mod, mn, "%s", !strncmp(buf, "GameAssembly", 12) ? "GameAssembly (the game's C#)" : "pdcode");
  } else {
    snprintf(buf, n, "0x%lx", (unsigned long)v);
    snprintf(mod, mn, "elsewhere");
  }
}

typedef struct {
  uintptr_t at;
  int n;
  char mod[48];
} Bucket;

static int by_count(const void *a, const void *b) {
  return ((const Bucket *)b)->n - ((const Bucket *)a)->n;
}

// waits (1: samples in a system call) by their callers, or the port's own
// time (0: in the nro, not waiting) by the first caller outside it (the
// game's code calling GL)
typedef struct {
  uintptr_t key[3];
  int n;
} Chain;

static int by_chain_count(const void *a, const void *b) {
  return ((const Chain *)b)->n - ((const Chain *)a)->n;
}

static void report_chains(int waits) {
  static Chain c[MAX_SAMPLES];
  int nc = 0, total = 0;
  for (int i = 0; i < s_n; i++) {
    const int wait = s_pc[i] >= s_svc_lo && s_pc[i] < s_svc_hi;
    uintptr_t key[3] = { 0, 0, 0 };
    if (waits) {
      if (!wait)
        continue;
      // the stub's caller (a lock, a condition), the first caller outside
      // the port (the game's code waiting) and its caller
      key[0] = s_up[i][0];
      int d = 1;
      while (d < DEPTH && s_up[i][d] && in_nro(s_up[i][d]))
        d++;
      if (d < DEPTH)
        key[1] = s_up[i][d], key[2] = d + 1 < DEPTH ? s_up[i][d + 1] : 0;
      if (!key[1]) // all in the port: the two above the stub's caller
        key[1] = s_up[i][1], key[2] = s_up[i][2];
    } else {
      if (wait || !in_nro(s_pc[i]))
        continue;
      int d = 0;
      while (d < DEPTH && s_up[i][d] && in_nro(s_up[i][d]))
        d++;
      if (d < DEPTH && s_up[i][d]) // the game's code that called in, and its caller
        key[0] = s_up[i][d], key[1] = d + 1 < DEPTH ? s_up[i][d + 1] : 0;
      key[2] = d ? s_up[i][d - 1] : s_pc[i]; // the port's function it called (its return address in it)
    }
    total++;
    int k = 0;
    while (k < nc && memcmp(c[k].key, key, sizeof key))
      k++;
    if (k == nc)
      c[nc++] = (Chain){ { key[0], key[1], key[2] }, 0 };
    c[k].n++;
  }
  if (total < 20)
    return;
  qsort(c, (size_t)nc, sizeof *c, by_chain_count);
  log_printf("[prof] %s: %.1f%% of the samples, the most common %s:", waits ? "waits (in a system call)" : "the port's code (Mesa, GL, libc) not waiting",
             100.0 * total / s_n, waits ? "callers (the stub's caller < the game's code waiting < its caller)" : "callers from the game (caller < its caller; into the port at)");
  if (waits && nc) { // the busiest wait's whole chain, from one of its samples
    for (int i = 0; i < s_n; i++) {
      if (!(s_pc[i] >= s_svc_lo && s_pc[i] < s_svc_hi) || s_up[i][0] != c[0].key[0])
        continue;
      char line[600];
      int o = snprintf(line, sizeof line, "[prof]   its callers:");
      for (int d = 0; d < DEPTH && s_up[i][d] && o < (int)sizeof line - 60; d++) {
        char w[96], m[48];
        name_of(s_up[i][d], w, sizeof w, m, sizeof m);
        o += snprintf(line + o, sizeof line - (size_t)o, " %s", w);
      }
      log_printf("%s", line);
      break;
    }
  }
  for (int k = 0; k < nc && k < 10; k++) {
    char a[96], b2[96], c2[96], m[48];
    name_of(c[k].key[0], a, sizeof a, m, sizeof m);
    name_of(c[k].key[1], b2, sizeof b2, m, sizeof m);
    name_of(c[k].key[2], c2, sizeof c2, m, sizeof m);
    log_printf("[prof]   %5.1f%%  %s < %s %s %s", 100.0 * c[k].n / s_n, c[k].key[0] ? a : "?", c[k].key[1] ? b2 : "?",
               waits ? "<" : "; at", c2);
  }
}

// Inclusive time: each call site on a sample's stack counts that sample once (the share of the thread's time spent
// in the calls made from there). The flat profile above spreads over hundreds of small functions; this shows which
// of the engine's and the game's steps (player loop phases, scripts, culling) hold the time.
#define NSITES 8192
static void report_inclusive(void) {
  static uintptr_t site[NSITES];
  static int cnt[NSITES];
  memset(site, 0, sizeof site);
  memset(cnt, 0, sizeof cnt);
  int used = 0, busy = 0;
  for (int i = 0; i < s_n; i++) {
    if (s_pc[i] >= s_svc_lo && s_pc[i] < s_svc_hi) // (waiting: counted apart, in the waits above)
      continue;
    busy++;
    for (int d = 0; d < DEPTH && s_up[i][d]; d++) {
      const uintptr_t a = s_up[i][d];
      int dup = 0;
      for (int e = 0; e < d; e++)
        if (s_up[i][e] == a)
          dup = 1;
      if (dup)
        continue;
      unsigned h = (unsigned)((a * 0x9E3779B97F4A7C15ull) >> 51) % NSITES;
      while (site[h] && site[h] != a)
        h = (h + 1) % NSITES;
      if (!site[h]) {
        if (used >= NSITES * 3 / 4)
          continue;
        site[h] = a;
        used++;
      }
      cnt[h]++;
    }
  }
  static Bucket top[48];
  int nt = 0;
  for (int k = 0; k < NSITES; k++) {
    if (!site[k] || in_nro(site[k]) || cnt[k] * 100 < busy * 2) // (the port's own frames and under 2% left out)
      continue;
    int j = nt < 48 ? nt++ : 47;
    if (j == 47 && top[47].n >= cnt[k])
      continue;
    top[j] = (Bucket){ site[k], cnt[k], "" };
    while (j > 0 && top[j].n > top[j - 1].n) {
      const Bucket t = top[j];
      top[j] = top[j - 1];
      top[j - 1] = t;
      j--;
    }
  }
  if (busy < 50)
    return;
  log_printf("[prof] inclusive: the call sites on the main thread's stacks while it works (%d samples, %.0f%% of "
             "them; the waits left out), by the share of its work under them:", busy, 100.0 * busy / s_n);
  for (int k = 0; k < nt; k++) {
    char where[96], mod[48];
    name_of(top[k].at, where, sizeof where, mod, sizeof mod);
    log_printf("[prof]   incl %5.1f%%  %s", 100.0 * top[k].n / busy, where);
  }
}

static void report(double secs) {
  static Bucket b[MAX_SAMPLES], m[32];
  int nb = 0, nm = 0;
  for (int i = 0; i < s_n; i++) {
    const uintptr_t at = s_pc[i] & ~(uintptr_t)0xff;
    int k = 0;
    while (k < nb && b[k].at != at)
      k++;
    if (k == nb) {
      char where[96];
      b[nb] = (Bucket){ at, 0, "" };
      name_of(at, where, sizeof where, b[nb].mod, sizeof b[nb].mod);
      nb++;
    }
    b[k].n++;
    int j = 0;
    while (j < nm && strcmp(m[j].mod, b[k].mod))
      j++;
    if (j == nm && nm < 32)
      m[nm++] = (Bucket){ 0, 0, "" }, snprintf(m[j].mod, sizeof m[j].mod, "%s", b[k].mod);
    if (j < nm)
      m[j].n++;
  }
  if (s_n < 50)
    return;
  qsort(m, (size_t)nm, sizeof *m, by_count);
  char line[600];
  int o = snprintf(line, sizeof line, "[prof] %s: %d samples of the %s thread in %.1f s:", s_phase, s_n,
                   s_t ? "render" : "main", secs);
  for (int j = 0; j < nm && o < (int)sizeof line - 60; j++)
    o += snprintf(line + o, sizeof line - (size_t)o, " %s %.0f%%,", m[j].mod, 100.0 * m[j].n / s_n);
  log_printf("%s", line);
  qsort(b, (size_t)nb, sizeof *b, by_count);
  for (int k = 0; k < nb && k < TOP; k++) {
    char where[96], mod[48];
    name_of(b[k].at, where, sizeof where, mod, sizeof mod);
    log_printf("[prof]   %5.1f%%  %s", 100.0 * b[k].n / s_n, where);
  }
  report_chains(1);
  report_chains(0);
  if (s_t == 0)
    report_inclusive();
}

static void prof_main(void *arg) {
  (void)arg;
  u64 next = 0;
  int phase = -1; // 1 racing, 0 not (menus, loading)
  for (;;) {
    svcSleepThread(250000000ll);
    const u64 now = armGetSystemTick();
    const int racing = race_drive_racing() ? 1 : 0;
    if (racing != phase) { // a new phase: its first window 12 s in
      phase = racing;
      next = now + armNsToTicks(FIRST_NS);
    }
    if (now < next)
      continue;
    next = now + armNsToTicks(EVERY_NS);
    s_phase = racing ? "race" : "menu";
    const Handle render = s_render;
    s_ns[0] = s_ns[1] = 0;
    const u64 t0 = armGetSystemTick();
    while (armTicksToNs(armGetSystemTick() - t0) < WINDOW_NS && (race_drive_racing() ? 1 : 0) == racing) {
      s_t = 0;
      sample(s_main);
      if (render != INVALID_HANDLE) {
        s_t = 1;
        sample(render);
      }
      svcSleepThread(STEP_NS);
    }
    const double secs = (double)armTicksToNs(armGetSystemTick() - t0) / 1e9;
    for (s_t = 0; s_t < 2; s_t++)
      report(secs);
  }
}

void prof_init(void) {
  extern char _start[];
  if (!config.profile)
    return;
  s_main = threadGetCurHandle();
  svc_range();
  // a high priority (above the game's 59) on the third core, out of the way
  if (R_FAILED(threadCreate(&s_thread, prof_main, NULL, NULL, 0x8000, 0x2a, 2)) || R_FAILED(threadStart(&s_thread))) {
    log_printf("[prof] no profiler thread");
    return;
  }
  log_printf("[prof] on: in races and menus, 3 s of samples every 30 s (prof_init at nro+0x%lx)",
             (unsigned long)((uintptr_t)prof_init - (uintptr_t)_start));
}
