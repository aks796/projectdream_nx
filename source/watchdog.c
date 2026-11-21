/* watchdog.c -- where every thread is when the main loop stops
 *
 * The main loop beats once a frame (watchdog_beat). When no beat comes for
 * a few seconds while the app is in focus, each thread the port knows (the
 * main thread, and every one made through the pthread wrapper) is paused for
 * a moment, and its PC, LR and frame-pointer chain go to debug.log as
 * module+offset ("[watchdog]" lines; "nro+0x..." is the port itself, for
 * addr2line on mkt_nx.elf). Once per stall. watchdog_stack_scan lists the
 * return addresses on the calling thread's stack (game_patch.c's
 * diagnostics; IL2CPP code here keeps no frame records).
 *
 * It runs on a libnx thread of its own, which the game's garbage collector
 * never stops. A thread that is already paused (by the collector) is read
 * and left paused.
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
#include "pthr.h"
#include "so_util.h"
#include "srv.h"

#define STALL_NS 5000000000ll
#define MAX_THREADS 96

static volatile u64 s_beats;
static volatile int s_paused; // HOME/sleep: no beats expected
static Thread s_thread;

void watchdog_beat(void) { s_beats++; }
void watchdog_pause(int paused) { s_paused = paused; }

static const char *where(uintptr_t v, char *buf, size_t n) {
  extern char _start[];
  uintptr_t base;
  const char *name = so_name_at_unlocked(v, &base);
  if (name)
    snprintf(buf, n, "%s+0x%lx", name, (unsigned long)(v - base));
  else if (v >= (uintptr_t)_start && v - (uintptr_t)_start < (64u << 20))
    snprintf(buf, n, "nro+0x%lx", (unsigned long)(v - (uintptr_t)_start));
  else
    snprintf(buf, n, "0x%lx", (unsigned long)v);
  return buf;
}

// " <- caller <- its caller ...": the frame records from fp, while they stay
// in [lo, hi) (the thread's stack)
static void chain(char *line, size_t cap, uintptr_t fp, uintptr_t lo, uintptr_t hi) {
  char a[96];
  int o = 0;
  line[0] = '\0';
  for (int i = 0; i < 16 && fp >= lo && fp < hi && hi - fp >= 16 && !(fp & 7); i++) {
    const uintptr_t next = ((const uintptr_t *)fp)[0], ret = ((const uintptr_t *)fp)[1];
    o += snprintf(line + o, cap - (size_t)o, " <- %s", where(ret, a, sizeof a));
    if (next <= fp || o > (int)cap - 100)
      break;
    fp = next;
  }
}

// The return addresses among a stack's words (copied from sp up): every
// word that points into a game library just after a BL or BLR. IL2CPP code
// here keeps no frame records, so this is how its callers show up; stale
// words from older calls can be among them.
static void scan_words(const char *tag, const uintptr_t *w, size_t n) {
  char line[1400], a[96];
  int o = 0, hits = 0;
  line[0] = '\0';
  for (size_t i = 0; i < n && hits < 14; i++) {
    const uintptr_t v = w[i];
    const so_module *m = so_module_at(v);
    if (!m || (v & 3) || v - 4 < m->base || v - m->base > m->text_size)
      continue;
    const uint32_t insn = *(const uint32_t *)(v - 4);
    if ((insn & 0xfc000000) != 0x94000000 && (insn & 0xfffffc1f) != 0xd63f0000)
      continue;
    o += snprintf(line + o, sizeof line - (size_t)o, " sp+0x%lx:%s", (unsigned long)(i * 8),
                  where(v, a, sizeof a));
    hits++;
  }
  log_printf("%s   stack:%s", tag, line[0] ? line : " nothing");
}

#define SCAN_WORDS 1024 // 8 KB of stack

static void dump(Handle h, uintptr_t lo, uintptr_t hi) {
  u64 tid = 0;
  svcGetThreadId(&tid, h);
  // already paused (a collection in progress): read it, leave it paused
  const int paused_here = R_SUCCEEDED(svcSetThreadActivity(h, ThreadActivity_Paused));
  ThreadContext ctx;
  const Result rc = svcGetThreadContext3(&ctx, h);
  // its stack, copied while it cannot change (logging waits until it runs
  // again: it may hold the log's lock)
  static uintptr_t words[SCAN_WORDS];
  size_t nw = 0;
  if (R_SUCCEEDED(rc) && hi > lo && ctx.sp >= lo && ctx.sp < hi) {
    nw = (hi - (ctx.sp & ~(uintptr_t)7)) / 8;
    if (nw > SCAN_WORDS)
      nw = SCAN_WORDS;
    memcpy(words, (const void *)(ctx.sp & ~(uintptr_t)7), nw * 8);
  }
  if (paused_here)
    svcSetThreadActivity(h, ThreadActivity_Runnable);
  if (R_FAILED(rc)) {
    log_printf("[watchdog] thread %llu: no context (0x%x)", (unsigned long long)tid, rc);
    return;
  }
  char a[96], b[96];
  log_printf("[watchdog] thread %llu%s: pc %s, lr %s", (unsigned long long)tid,
             paused_here ? "" : " (paused by someone else)", where(ctx.pc.x, a, sizeof a),
             where(ctx.lr, b, sizeof b));
  char line[900];
  chain(line, sizeof line, ctx.fp, lo, hi);
  if (line[0])
    log_printf("[watchdog]   %s", line);
  if (nw)
    scan_words("[watchdog]", words, nw);
}

void watchdog_stack_scan(const char *tag, uintptr_t sp) {
  const Thread *t = threadGetSelf();
  uintptr_t top = sp + SCAN_WORDS * 8;
  if (t && t->stack_sz && sp >= (uintptr_t)t->stack_mirror &&
      top > (uintptr_t)t->stack_mirror + t->stack_sz)
    top = (uintptr_t)t->stack_mirror + t->stack_sz;
  scan_words(tag, (const uintptr_t *)(sp & ~(uintptr_t)7), (top - (sp & ~(uintptr_t)7)) / 8);
}

// every thread the port knows, and the offline server's
static void dump_all(const char *why) {
  static Handle h[MAX_THREADS];
  static uintptr_t lo[MAX_THREADS], hi[MAX_THREADS];
  const int n = pthr_threads(h, lo, hi, MAX_THREADS);
  log_printf("[watchdog] %s: %d threads", why, n);
  for (int i = 0; i < n; i++)
    dump(h[i], lo[i], hi[i]);
  if (srv_thread_handle()) {
    log_printf("[watchdog] the offline server:");
    dump(srv_thread_handle(), 0, 0);
  }
}

static void watchdog_main(void *arg) {
  (void)arg;
  u64 last = s_beats, since = armGetSystemTick();
  int reported = 0;
  unsigned srv_last = 0, srv_quiet = 0;
  // dump_threads_at: seconds after start at which to log every thread
  int at[16], nat = 0, next_at = 0;
  for (const char *p = config.dump_threads_at; *p && nat < 16;) {
    char *e;
    const long v = strtol(p, &e, 10);
    if (e == p)
      break;
    at[nat++] = (int)v;
    p = *e == ',' ? e + 1 : e;
  }
  const u64 start = armGetSystemTick();
  unsigned ticks = 0;
  for (;;) {
    svcSleepThread(500000000ll);
    const u64 now = armGetSystemTick();
    if (!(ticks++ & 1)) { // (once a second: the clocks the console runs at, for the [clk] lines)
      void clocks_sample(void);
      clocks_sample();
    }
    if (next_at < nat && (s64)armTicksToNs(now - start) >= (s64)at[next_at] * 1000000000ll) {
      char why[64];
      snprintf(why, sizeof why, "every thread at %d s", at[next_at++]);
      dump_all(why);
    }
    // the offline server's loop goes round every 10 ms: 3 s without one is a stall
    if (srv_thread_handle()) {
      const unsigned r = srv_rounds();
      if (r != srv_last) {
        srv_last = r;
        srv_quiet = 0;
      } else if (++srv_quiet == 6) {
        log_printf("[watchdog] the offline server stopped at round %u:", r);
        dump(srv_thread_handle(), 0, 0);
      }
    }
    if (s_beats != last || s_paused) {
      last = s_beats;
      since = now;
      reported = 0;
      continue;
    }
    if (reported || (s64)armTicksToNs(now - since) < STALL_NS)
      continue;
    reported = 1;
    char why[96];
    snprintf(why, sizeof why, "no frame for %llu s (after %llu)",
             (unsigned long long)(armTicksToNs(now - since) / 1000000000ull), (unsigned long long)last);
    dump_all(why);
  }
}

void watchdog_init(void) {
  // above the game's threads, so a busy core cannot hide a stall
  if (R_SUCCEEDED(threadCreate(&s_thread, watchdog_main, NULL, NULL, 0x10000, 0x2a, 2))) // (off core 0: the main thread's)
    threadStart(&s_thread);
}
