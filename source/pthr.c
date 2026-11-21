/* pthr.c -- bionic<->newlib pthread wrappers for the game's libraries
 *
 * Ported from gm666q/lswtcs-vita (reimpl/pthr.c) via lbbg_nx and gd_geode_nx,
 * adapted for devkitA64/libnx. Every thread the game creates (and the main
 * thread) is recorded with its kernel handle and stack bounds: IL2CPP's
 * garbage collector asks for stack bounds, and stops threads through
 * gcstop.c, which needs the handles.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "pthr.h"
#include "net_shim.h"
#include "util.h"

#define BIONIC_PTHREAD_MUTEX_INITIALIZER            0
#define BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER  0x4000
#define BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER 0x8000

// ---------------------------------------------------------------------------
// fake RW TLS: AArch64 -fstack-protector loads the canary relative to
// TPIDR_EL0, which libnx leaves pointing at read-only/zero storage. Each game
// thread gets a small zeroed block installed in TPIDR_EL0 so those loads (and
// any other bionic TLS-relative reads) land on valid, writable memory. The
// block is intentionally leaked: it must stay live for the thread's lifetime.
// ---------------------------------------------------------------------------

void pthr_install_fake_tls(void) {
  uint8_t *tls = calloc(1, 0x200);
  armSetTlsRw(tls);
}

void pthr_ensure_fake_tls(void) {
  if (armGetTlsRw() == NULL)
    pthr_install_fake_tls();
}

// pthread calls return error numbers, and newlib's differ from bionic's for
// some (ETIMEDOUT is 116 vs 110: libc++'s condition_variable threw on every
// timed wait that simply timed out)
static inline int lx(int r) {
  return r ? net_errno_to_linux(r) : 0;
}

// ---------------------------------------------------------------------------
// lazy first-use initialization of game-owned sync objects: the hot path is
// one acquire-load of the magic word; only init and destroy take init_lock
// ---------------------------------------------------------------------------

#define PTHR_MUTEX_MAGIC 0x4D58544Du // "MTXM"
#define PTHR_COND_MAGIC  0x444E434Du // "CNDM"

static Mutex init_lock;

static int attr_static_init(pthread_attr_t_bionic *attr) {
  if (attr->magic != 0x42424242) {
    attr->magic = 0x42424242;
    attr->real_ptr = malloc(sizeof(pthread_attr_t));
    return pthread_attr_init(attr->real_ptr);
  }
  return 0;
}

static int mutex_static_init(pthread_mutex_t_bionic *mutex, const pthread_mutexattr_t *attr) {
  // pairs with the release store below so real_ptr is visible once magic is
  if (__atomic_load_n(&mutex->magic, __ATOMIC_ACQUIRE) == PTHR_MUTEX_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) == PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock); // another thread won the first-use race
    return 0;
  }

  int kind = PTHREAD_MUTEX_NORMAL;
  if (attr) {
    pthread_mutexattr_gettype((pthread_mutexattr_t *)attr, &kind);
  } else {
    // the kind word of a statically initialized bionic mutex (overlaps the
    // low half of real_ptr, which we haven't written yet)
    switch (*(int *)mutex) {
      case BIONIC_PTHREAD_RECURSIVE_MUTEX_INITIALIZER:  kind = PTHREAD_MUTEX_RECURSIVE;  break;
      case BIONIC_PTHREAD_ERRORCHECK_MUTEX_INITIALIZER: kind = PTHREAD_MUTEX_ERRORCHECK; break;
      default:                                          kind = PTHREAD_MUTEX_NORMAL;     break;
    }
  }

  pthread_mutex_t *real = malloc(sizeof(pthread_mutex_t));

  pthread_mutexattr_t ma;
  pthread_mutexattr_init(&ma);
  pthread_mutexattr_settype(&ma, kind);
  int ret = pthread_mutex_init(real, &ma);
  pthread_mutexattr_destroy(&ma);

  if (ret == 0) {
    mutex->real_ptr = real;
    __atomic_store_n(&mutex->magic, PTHR_MUTEX_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
  }
  mutexUnlock(&init_lock);
  return ret;
}

static int cond_static_init(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (__atomic_load_n(&cond->magic, __ATOMIC_ACQUIRE) == PTHR_COND_MAGIC)
    return 0;

  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) == PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }

  pthread_cond_t *real = malloc(sizeof(pthread_cond_t));
  int ret = pthread_cond_init(real, attr);

  if (ret == 0) {
    cond->real_ptr = real;
    __atomic_store_n(&cond->magic, PTHR_COND_MAGIC, __ATOMIC_RELEASE);
  } else {
    free(real);
  }
  mutexUnlock(&init_lock);
  return ret;
}

// ---------------------------------------------------------------------------
// scheduling
//
// Android's scheduler is preemptive: a thread that spins waiting for another
// is time-sliced, and the thread it waits for still runs. Unity relies on
// that (its AssetBundle decoder polls its input with no sleep at all; the
// main thread spins on UnityWebRequest.isDone and Sakasho calls). Horizon is
// not preemptive at the priorities games use: a runnable thread keeps its
// core until it blocks, and an equal-priority thread queued behind it on
// that core does not run. Here that showed as the title's UI loading taking
// a minute in bursts (WaitLoadUiResources), or a few seconds whenever file
// logging happened to make the busy threads block. The fix is the one the
// 32-bit ports' runtime proved on hardware (android32 dcr_sched.c):
//  - priority 59: the kernel rotates the priority-59 queue of cores 0-2
//    every 10 ms, migrating threads between cores as it goes (Mesosphere's
//    DPC manager -> KScheduler::RotateScheduledQueue). That is the kernel's
//    time slicing, and it exists at that priority only. The main thread and
//    every game thread run at 59; the port's own service threads (audio
//    0x20, watchdog 0x2a, offline server 0x2b, Looper threads one step up)
//    stay above them: short, and latency-bound;
//  - every application core in each thread's core mask (core 3, the system's,
//    too when the process may use it and config fourth_core is on: a
//    forwarder can allow it, the homebrew menu's title override does not),
//    new threads starting on them in turn, so the kernel can move a thread
//    that waits behind a busy one to a free core.
// No clock changes: users set those themselves.
// ---------------------------------------------------------------------------

#define GUEST_PRIO 59

static Mutex core_lock;
static int core_list[4];
static int core_count = 0;
static unsigned core_rr = 0;
static u64 core_mask;
static u64 s_allowed;  // the cores the process may use (svcGetInfo)
static int s_app_cores; // how many of them the game's threads use (main thread's included)

#define HORIZON_RESERVED_CORE 3

static void core_init_once(void) {
  if (core_count)
    return;
  u64 mask = 0;
  if (R_FAILED(svcGetInfo(&mask, InfoType_CoreMask, CUR_PROCESS_HANDLE, 0)) || mask == 0)
    mask = 0x7; // fallback: cores 0,1,2 (typical homebrew allotment)
  s_allowed = mask;

  const u64 app_mask = mask & ~(config.fourth_core ? 0ull : 1ull << HORIZON_RESERVED_CORE);
  if (app_mask)
    mask = app_mask;

  for (int c = 0; c < 4; c++)
    if (mask & (1ull << c))
      core_list[core_count++] = c;

  if (!core_count)
    core_list[core_count++] = 0;
  for (int i = 0; i < core_count; i++)
    core_mask |= 1ull << core_list[i];
  s_app_cores = core_count;
}

// the cores the game's threads use, the main thread's included (3, or 4 with the fourth core): Unity's job worker
// count when config job_workers is 0 (java_unity.c)
int pthr_app_cores(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int n = s_app_cores;
  mutexUnlock(&core_lock);
  return n;
}

// config main_core: the first application core for the main thread (UnityMain: the game's C# and the engine's
// frame) alone, every other thread on the rest. On the console the main thread was busy 98% of the frame with the
// other cores half idle; a job worker or the render thread started on its core waited until it blocked (it runs
// at libnx's priority 44, the game's threads at 59), and so did the main thread when it waited for that job.
// main_core 2 (config version 10): no thread starts on that core, but the game's threads keep it in their masks, so
// while the main thread waits (for the job workers, the GPU, the display) the kernel can run one of them there. They
// are below the main thread's priority: it takes its core back the moment it can run. (Thorhax's SonicDreamTeam-NX
// runs one job worker per core this way, with no core of the main thread's own.)
static int s_main_core = -1;

void pthr_dedicate_main_core(void) {
  s32 prio = -1, ideal = -1;
  u64 mask = 0;
  svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
  svcGetThreadCoreMask(&ideal, &mask, CUR_THREAD_HANDLE);
  mutexLock(&core_lock);
  core_init_once();
  if (config.main_core && core_count >= 3 && s_main_core < 0) {
    s_main_core = core_list[0];
    for (int i = 1; i < core_count; i++)
      core_list[i - 1] = core_list[i];
    core_count--;
    if (config.main_core != 2)
      core_mask &= ~(1ull << s_main_core);
  }
  mutexUnlock(&core_lock);
  Result rc = 0;
  if (s_main_core >= 0)
    rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, s_main_core, 1ull << s_main_core);
  s32 ideal2 = -1;
  u64 mask2 = 0;
  svcGetThreadCoreMask(&ideal2, &mask2, CUR_THREAD_HANDLE);
  log_printf("[sched] main thread: priority %d, core %d mask 0x%llx -> core %d mask 0x%llx%s; other threads on 0x%llx%s "
             "(cores allowed 0x%llx: %s)",
             (int)prio, (int)ideal, (unsigned long long)mask, (int)ideal2, (unsigned long long)mask2,
             R_FAILED(rc) ? " (refused)" : "", (unsigned long long)core_mask,
             s_main_core >= 0 && (core_mask >> s_main_core & 1) ? ", the main thread's core while it waits" : "",
             (unsigned long long)s_allowed,
             s_allowed & (1ull << HORIZON_RESERVED_CORE)
                 ? (config.fourth_core ? "the fourth core used" : "the fourth core left alone (fourth_core 0)")
                 : "three, as from the homebrew menu");
}

// the next application core in turn
static int next_core(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int core = core_list[core_rr++ % (unsigned)core_count];
  mutexUnlock(&core_lock);
  return core;
}

// a game thread (the caller): priority 59, any application core, starting on
// the next one in turn
static void sched_guest(void) {
  const int core = next_core();
  svcSetThreadPriority(CUR_THREAD_HANDLE, GUEST_PRIO);
  const Result rc = svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, core_mask);
  if (R_FAILED(rc)) {
    static int warned;
    if (!warned++)
      log_printf("[sched] core mask 0x%llx refused (0x%x): threads stay on one core",
                  (unsigned long long)core_mask, rc);
  }
}

void pthr_sched_main(void) {
  s32 prio_before = -1, prio_after = -1, ideal = -1;
  u64 mask_before = 0, mask_after = 0;
  svcGetThreadPriority(&prio_before, CUR_THREAD_HANDLE);
  svcGetThreadCoreMask(&ideal, &mask_before, CUR_THREAD_HANDLE);
  mutexLock(&core_lock);
  core_init_once();
  mutexUnlock(&core_lock);
  const Result rp = svcSetThreadPriority(CUR_THREAD_HANDLE, GUEST_PRIO);
  const Result rm = svcSetThreadCoreMask(CUR_THREAD_HANDLE, -3 /* ideal core unchanged */, core_mask);
  svcGetThreadPriority(&prio_after, CUR_THREAD_HANDLE);
  svcGetThreadCoreMask(&ideal, &mask_after, CUR_THREAD_HANDLE);
  log_printf("[sched] main thread: priority %d -> %d%s, cores 0x%llx -> 0x%llx%s (ideal %d)",
              (int)prio_before, (int)prio_after, R_FAILED(rp) ? " (set FAILED)" : "",
              (unsigned long long)mask_before, (unsigned long long)mask_after, R_FAILED(rm) ? " (set FAILED)" : "",
              (int)ideal);
}

// Every newlib pthread_create in the NRO comes here (-Wl,--wrap=pthread_create): libnx makes a pthread at priority
// 59 on the process's default core and only there (threadCreate(..., 0x3B, -2)), so FFmpeg's decoding threads
// (media_ndk.c) would all share one core and never move. Each new thread gets every application core in its mask
// and the next one in turn to start on, as the game's threads do (sched_guest); the game's own threads, which
// come through here too, set the same again in their trampoline.
int __real_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg);

typedef struct {
  void *(*fn)(void *);
  void *arg;
} WrapStart;

static void *wrap_start(void *p) {
  WrapStart w = *(WrapStart *)p;
  free(p);
  mutexLock(&core_lock);
  core_init_once();
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, next_core(), core_mask);
  return w.fn(w.arg);
}

int __wrap_pthread_create(pthread_t *t, const pthread_attr_t *attr, void *(*fn)(void *), void *arg) {
  WrapStart *w = malloc(sizeof *w);
  if (!w)
    return __real_pthread_create(t, attr, fn, arg);
  w->fn = fn, w->arg = arg;
  const int r = __real_pthread_create(t, attr, wrap_start, w);
  if (r)
    free(w);
  return r;
}

// a background core (not the last application core): for threads spawned
// outside our pthread trampoline (the OpenSL mixer and its helpers)
static void assign_core_bg(void) {
  mutexLock(&core_lock);
  core_init_once();
  const int n = (core_count > 1) ? core_count - 1 : core_count; // cores 0..n-1
  const int core = core_list[core_rr++ % (unsigned)n];
  mutexUnlock(&core_lock);
  svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, 1u << core);
}

// pin the calling thread to a background core. For threads our vendored code
// spawns directly through newlib pthread_create (the SDL audio mixer and the
// OpenSL play-callback thread). Also raise their priority so the mixer never
// starves into an audout stall.
void pthr_pin_bg_core(void) {
  assign_core_bg();
  svcSetThreadPriority(CUR_THREAD_HANDLE, 0x20);
}

// vendored OpenSL (sync.c / ThreadPool.c) pins its helper threads with this
void pthr_pin_worker_core(void) {
  assign_core_bg();
}

// ---------------------------------------------------------------------------
// thread creation (installs the fake TLS before running game code)
//
// libnx has no pthread_detach (it returns ENOSYS, and libc++'s
// std::thread::detach throws on that), so the port detaches itself: every
// thread it creates has a record, a detached thread is marked, and finished
// detached threads are joined -- their stacks freed -- on the next create or
// detach.
// ---------------------------------------------------------------------------

typedef struct ThreadRec {
  struct ThreadRec *next;
  pthread_t thread;
  void *(*start)(void *);
  void *arg;
  int detached;      // under thr_lock
  int finished;      // atomic: the start routine returned
  Handle handle;     // set by the thread itself, before the start routine
  uintptr_t stack_lo, stack_hi;
  char name[16];     // pthread_setname_np / prctl(PR_SET_NAME)
  u64 cpu_last;      // its CPU ticks at the last report (pthr_cpu_report)
} ThreadRec;

static Mutex thr_lock;
static ThreadRec *thr_list;

static void reap_locked(void) {
  for (ThreadRec **pp = &thr_list; *pp;) {
    ThreadRec *r = *pp;
    if (r->detached && __atomic_load_n(&r->finished, __ATOMIC_ACQUIRE)) {
      *pp = r->next;
      pthread_join(r->thread, NULL); // it has returned; this waits for the exit
      free(r);
    } else {
      pp = &r->next;
    }
  }
}

static ThreadRec *find_locked(pthread_t t) {
  for (ThreadRec *r = thr_list; r; r = r->next)
    if (r->thread == t)
      return r;
  return NULL;
}

static void stack_of_current(uintptr_t *lo, uintptr_t *hi) {
  uintptr_t sp;
  __asm__ volatile("mov %0, sp" : "=r"(sp));
  MemoryInfo mi;
  u32 pi;
  if (R_SUCCEEDED(svcQueryMemory(&mi, &pi, (u64)sp)) && mi.size) {
    *lo = (uintptr_t)mi.addr;
    *hi = (uintptr_t)(mi.addr + mi.size);
  } else {
    *lo = sp & ~(uintptr_t)0xfffff;
    *hi = *lo + 0x100000;
  }
}

static ThreadRec s_main_rec;

void pthr_register_main(void) {
  s_main_rec.thread = pthread_self();
  s_main_rec.handle = threadGetCurHandle();
  stack_of_current(&s_main_rec.stack_lo, &s_main_rec.stack_hi);
}

int pthr_thread_info(pthread_t t, Handle *h, uintptr_t *lo, uintptr_t *hi) {
  ThreadRec *r = NULL;
  if (t == s_main_rec.thread) {
    r = &s_main_rec;
  } else {
    mutexLock(&thr_lock);
    for (ThreadRec *q = thr_list; q; q = q->next)
      if (q->thread == t) {
        r = q;
        break;
      }
    mutexUnlock(&thr_lock);
  }
  if (!r || !r->handle || __atomic_load_n(&r->finished, __ATOMIC_ACQUIRE))
    return 0;
  if (h) *h = r->handle;
  if (lo) *lo = r->stack_lo;
  if (hi) *hi = r->stack_hi;
  return 1;
}

int pthr_threads(Handle *h, uintptr_t *lo, uintptr_t *hi, int max) {
  int n = 0;
  if (n < max && s_main_rec.handle) {
    h[n] = s_main_rec.handle;
    lo[n] = s_main_rec.stack_lo;
    hi[n] = s_main_rec.stack_hi;
    n++;
  }
  // a stuck thread may hold the list's lock: then only the main thread
  for (int tries = 0; tries < 100; tries++) {
    if (mutexTryLock(&thr_lock)) {
      for (ThreadRec *r = thr_list; r && n < max; r = r->next) {
        if (!r->handle || __atomic_load_n(&r->finished, __ATOMIC_ACQUIRE))
          continue;
        h[n] = r->handle;
        lo[n] = r->stack_lo;
        hi[n] = r->stack_hi;
        n++;
      }
      mutexUnlock(&thr_lock);
      break;
    }
    svcSleepThread(10000000ll);
  }
  return n;
}

void pthr_set_name(pthread_t t, const char *name) {
  if (!name)
    return;
  ThreadRec *r = NULL;
  if (t == s_main_rec.thread) {
    r = &s_main_rec;
  } else {
    mutexLock(&thr_lock);
    r = find_locked(t);
    mutexUnlock(&thr_lock);
  }
  if (r)
    snprintf(r->name, sizeof r->name, "%s", name);
  if (config.log_diag)
    log_printf("[thread] named %s", name);
}

static u64 cpu_ticks(Handle h) {
  u64 v = 0;
  if (R_FAILED(svcGetInfo(&v, InfoType_ThreadTickCount, h, (u64)-1)) &&
      R_FAILED(svcGetInfo(&v, InfoType_ThreadTickCountDeprecated, h, (u64)-1)))
    return 0;
  return v;
}

typedef struct {
  const char *name;
  Handle h;
  unsigned pct;
} CpuUse;

static int by_use(const void *a, const void *b) {
  return (int)((const CpuUse *)b)->pct - (int)((const CpuUse *)a)->pct;
}

// the port's own threads (libnx's threadCreate, not pthreads) counted in the report too
static struct {
  Handle h;
  const char *name;
  u64 last;
} s_watch[8];
static int s_nwatch;

void pthr_cpu_watch(Handle h, const char *name) {
  mutexLock(&thr_lock);
  if (s_nwatch < 8)
    s_watch[s_nwatch++] = (typeof(s_watch[0])){ h, name, 0 };
  mutexUnlock(&thr_lock);
}

// the busiest threads since the last call: "name/handle pct%, ..." (of one core)
void pthr_cpu_report(char *out, size_t n, int top) {
  static u64 last_at;
  const u64 now = armGetSystemTick();
  const u64 span = last_at ? now - last_at : 0;
  last_at = now;
  CpuUse use[64];
  int nu = 0;
  out[0] = '\0';
  mutexLock(&thr_lock);
  for (ThreadRec *r = &s_main_rec; r; r = r == &s_main_rec ? thr_list : r->next) {
    if (!r->handle || __atomic_load_n(&r->finished, __ATOMIC_ACQUIRE))
      continue;
    const u64 t = cpu_ticks(r->handle);
    const u64 d = r->cpu_last && t > r->cpu_last ? t - r->cpu_last : 0;
    r->cpu_last = t;
    if (span && d && nu < 64)
      use[nu++] = (CpuUse){ r == &s_main_rec ? "main" : r->name[0] ? r->name : "?", r->handle,
                            (unsigned)(d * 100 / span) };
  }
  for (int i = 0; i < s_nwatch; i++) {
    const u64 t = cpu_ticks(s_watch[i].h);
    const u64 d = s_watch[i].last && t > s_watch[i].last ? t - s_watch[i].last : 0;
    s_watch[i].last = t;
    if (span && d && nu < 64)
      use[nu++] = (CpuUse){ s_watch[i].name, s_watch[i].h, (unsigned)(d * 100 / span) };
  }
  qsort(use, (size_t)nu, sizeof *use, by_use);
  size_t o = 0;
  for (int i = 0; i < nu && i < top && o < n; i++)
    o += (size_t)snprintf(out + o, n - o, "%s%s/%x %u%%", i ? ", " : "", use[i].name, use[i].h, use[i].pct);
  mutexUnlock(&thr_lock);
}

int pthr_snapshot(pthread_t *t, Handle *h, uintptr_t *lo, uintptr_t *hi, int max) {
  int n = 0;
  if (n < max && s_main_rec.handle) {
    t[n] = s_main_rec.thread;
    h[n] = s_main_rec.handle;
    lo[n] = s_main_rec.stack_lo;
    hi[n] = s_main_rec.stack_hi;
    n++;
  }
  mutexLock(&thr_lock);
  for (ThreadRec *r = thr_list; r && n < max; r = r->next) {
    if (!r->handle || __atomic_load_n(&r->finished, __ATOMIC_ACQUIRE))
      continue;
    t[n] = r->thread;
    h[n] = r->handle;
    lo[n] = r->stack_lo;
    hi[n] = r->stack_hi;
    n++;
  }
  mutexUnlock(&thr_lock);
  return n;
}

void pthr_current_stack(uintptr_t *lo, uintptr_t *hi) {
  stack_of_current(lo, hi);
}

static void *thread_trampoline(void *p) {
  ThreadRec *r = p;
  pthr_install_fake_tls();
  stack_of_current(&r->stack_lo, &r->stack_hi);
  __atomic_store_n(&r->handle, threadGetCurHandle(), __ATOMIC_RELEASE);
  sched_guest();
  void *ret = r->start(r->arg);
  __atomic_store_n(&r->finished, 1, __ATOMIC_RELEASE);
  return ret;
}

int pthread_create_soloader(pthread_t *thread, const pthread_attr_t_bionic *attr,
                            void *(*start)(void *), void *param) {
  ThreadRec *r = calloc(1, sizeof(*r));
  r->start = start;
  r->arg = param;

  pthread_attr_t a;
  pthread_attr_init(&a);
  // FMOD's mixer and the game's loaders are fine with 512k on Android, but
  // recursion depth is cheap to buy here; keep 2 MB unless the game asks big
  pthread_attr_setstacksize(&a, 2 * 1024 * 1024);
  if (attr) {
    attr_static_init((pthread_attr_t_bionic *)attr);
    size_t want = 0;
    if (attr->real_ptr && pthread_attr_getstacksize(attr->real_ptr, &want) == 0 &&
        want > 2 * 1024 * 1024)
      pthread_attr_setstacksize(&a, want);
    int ds = PTHREAD_CREATE_JOINABLE;
    if (attr->real_ptr && pthread_attr_getdetachstate(attr->real_ptr, &ds) == 0 &&
        ds == PTHREAD_CREATE_DETACHED)
      r->detached = 1;
  }

  mutexLock(&thr_lock);
  reap_locked();
  int ret = pthread_create(&r->thread, &a, thread_trampoline, r);
  if (ret == 0) {
    r->next = thr_list;
    thr_list = r;
    *thread = r->thread;
  }
  mutexUnlock(&thr_lock);
  pthread_attr_destroy(&a);
  if (ret != 0)
    free(r);
  return lx(ret);
}

int pthread_join_soloader(pthread_t thread, void **value_ptr) {
  const int ret = pthread_join(thread, value_ptr);
  if (ret == 0) {
    mutexLock(&thr_lock);
    for (ThreadRec **pp = &thr_list; *pp; pp = &(*pp)->next) {
      if ((*pp)->thread == thread) {
        ThreadRec *r = *pp;
        *pp = r->next;
        free(r);
        break;
      }
    }
    mutexUnlock(&thr_lock);
  }
  return lx(ret);
}

int pthread_detach_soloader(pthread_t thread) {
  mutexLock(&thr_lock);
  ThreadRec *r = find_locked(thread);
  if (r)
    r->detached = 1;
  reap_locked();
  mutexUnlock(&thr_lock);
  return 0; // threads the port did not create (the main thread): nothing to do
}

pthread_t pthread_self_soloader(void) { return pthread_self(); }

int pthread_equal_soloader(pthread_t t1, pthread_t t2) {
  if (t1 == t2) return 1;
  if (!t1 || !t2) return 0;
  return pthread_equal(t1, t2);
}

int pthread_getschedparam_soloader(pthread_t thread, int *policy, struct sched_param *param) {
  // newlib on devkitA64 doesn't expose pthread_getschedparam; the game only
  // reads these to echo them back, so reporting a default schedule is fine
  (void)thread;
  if (policy) *policy = 0; // SCHED_OTHER
  if (param) param->sched_priority = 0;
  return 0;
}

// bionic pthread_once_t is an int, 0 when fresh. 1 = running, 2 = done; a
// thread arriving while another runs the routine waits for it to finish
// (Geode's static initialisers and emutls rely on that).
int pthread_once_soloader(volatile int *once_control, void (*init_routine)(void)) {
  if (!once_control || !init_routine)
    return -1;
  if (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) == 2)
    return 0;
  int expected = 0;
  if (__atomic_compare_exchange_n(once_control, &expected, 1, 0, __ATOMIC_ACQ_REL,
                                  __ATOMIC_ACQUIRE)) {
    (*init_routine)();
    __atomic_store_n(once_control, 2, __ATOMIC_RELEASE);
    return 0;
  }
  while (__atomic_load_n(once_control, __ATOMIC_ACQUIRE) != 2)
    svcSleepThread(100000); // 0.1 ms
  return 0;
}

// ---------------------------------------------------------------------------
// thread-specific data. libnx has ~27 TLS slots, and every module carrying
// its own emutls runtime (Geode, each mod using thread_local, OpenSSL, ...)
// takes a key, so keys live in a per-thread table behind ONE libnx key whose
// destructor runs ours at thread exit.
// ---------------------------------------------------------------------------

#define MAX_KEYS 1024
#define DESTRUCTOR_ITERATIONS 4

static void (*key_dtors[MAX_KEYS])(void *);
static uint8_t key_used[MAX_KEYS];
static Mutex key_lock;
static pthread_key_t master_key;
static int master_ready;

// this thread's table, as master_key holds it: Project Dream's emulated TLS asks for it on every thread_local access
// of the Mac game's code (__emutls_get_address -> pthread_getspecific), so it is kept in native TLS too
static __thread void **t_keys;

static void run_key_dtors(void *table) {
  void **t = table;
  for (int pass = 0; pass < DESTRUCTOR_ITERATIONS; pass++) {
    int ran = 0;
    for (int k = 0; k < MAX_KEYS; k++) {
      void *v = t[k];
      if (!v || !key_used[k] || !key_dtors[k])
        continue;
      t[k] = NULL;
      key_dtors[k](v);
      ran = 1;
    }
    if (!ran)
      break;
  }
  if (t_keys == t)
    t_keys = NULL;
  free(t);
}

static void **key_table(int create) {
  if (!__atomic_load_n(&master_ready, __ATOMIC_ACQUIRE)) {
    mutexLock(&key_lock);
    if (!master_ready) {
      pthread_key_create(&master_key, run_key_dtors);
      __atomic_store_n(&master_ready, 1, __ATOMIC_RELEASE);
    }
    mutexUnlock(&key_lock);
  }
  void **t = pthread_getspecific(master_key);
  if (!t && create) {
    t = calloc(MAX_KEYS, sizeof(void *));
    pthread_setspecific(master_key, t);
  }
  t_keys = t;
  return t;
}

int pthread_key_create_soloader(int *key, void (*dtor)(void *)) {
  mutexLock(&key_lock);
  for (int k = 0; k < MAX_KEYS; k++) {
    if (!key_used[k]) {
      key_used[k] = 1;
      key_dtors[k] = dtor;
      mutexUnlock(&key_lock);
      *key = k;
      return 0;
    }
  }
  mutexUnlock(&key_lock);
  return 11; // EAGAIN
}

int pthread_key_delete_soloader(int key) {
  if (key < 0 || key >= MAX_KEYS)
    return 22; // EINVAL
  mutexLock(&key_lock);
  key_used[key] = 0;
  key_dtors[key] = NULL;
  mutexUnlock(&key_lock);
  return 0;
}

void *pthread_getspecific_soloader(int key) {
  if ((unsigned)key >= MAX_KEYS)
    return NULL;
  void **t = t_keys;
  if (__builtin_expect(t != NULL, 1))
    return t[key];
  t = key_table(0);
  return t ? t[key] : NULL;
}

int pthread_setspecific_soloader(int key, const void *value) {
  if (key < 0 || key >= MAX_KEYS)
    return 22;
  void **t = t_keys ? t_keys : key_table(1);
  if (!t)
    return 12; // ENOMEM
  t[key] = (void *)value;
  return 0;
}

// ---------------------------------------------------------------------------
// mutex / cond / attr
// ---------------------------------------------------------------------------

int pthread_mutexattr_init_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_init(attr); }
int pthread_mutexattr_settype_soloader(pthread_mutexattr_t *attr, int type) { return pthread_mutexattr_settype(attr, type); }
int pthread_mutexattr_destroy_soloader(pthread_mutexattr_t *attr) { return pthread_mutexattr_destroy(attr); }

int pthread_mutex_init_soloader(pthread_mutex_t_bionic *uid, const pthread_mutexattr_t *attr) {
  if (!uid) return EINVAL;
  return mutex_static_init(uid, attr);
}

int pthread_mutex_destroy_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&mutex->magic, __ATOMIC_RELAXED) != PTHR_MUTEX_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&mutex->magic, 0, __ATOMIC_RELEASE);
  pthread_mutex_t *real = mutex->real_ptr;
  mutex->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_mutex_destroy(real);
  free(real);
  return ret;
}

int pthread_mutex_lock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return lx(pthread_mutex_lock(mutex->real_ptr));
}

int pthread_mutex_trylock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex) return EINVAL;
  mutex_static_init(mutex, NULL);
  return lx(pthread_mutex_trylock(mutex->real_ptr));
}

int pthread_mutex_unlock_soloader(pthread_mutex_t_bionic *mutex) {
  if (!mutex || !mutex->real_ptr) return EINVAL;
  return lx(pthread_mutex_unlock(mutex->real_ptr));
}

int pthread_cond_init_soloader(pthread_cond_t_bionic *cond, const pthread_condattr_t *attr) {
  if (!cond) return EINVAL;
  (void)attr; // bionic's condattr is not newlib's: its clock is handled at the wait
  return cond_static_init(cond, NULL);
}

int pthread_cond_destroy_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return 0;
  mutexLock(&init_lock);
  if (__atomic_load_n(&cond->magic, __ATOMIC_RELAXED) != PTHR_COND_MAGIC) {
    mutexUnlock(&init_lock);
    return 0;
  }
  __atomic_store_n(&cond->magic, 0, __ATOMIC_RELEASE);
  pthread_cond_t *real = cond->real_ptr;
  cond->real_ptr = NULL;
  mutexUnlock(&init_lock);
  int ret = pthread_cond_destroy(real);
  free(real);
  return ret;
}

int pthread_cond_signal_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_signal(cond->real_ptr);
}

int pthread_cond_broadcast_soloader(pthread_cond_t_bionic *cond) {
  if (!cond) return EINVAL;
  cond_static_init(cond, NULL);
  return pthread_cond_broadcast(cond->real_ptr);
}

int pthread_cond_wait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  return lx(pthread_cond_wait(cond->real_ptr, mutex->real_ptr));
}

// A deadline on either of the game's clocks (bionic's CLOCK_REALTIME, or
// CLOCK_MONOTONIC from a condattr_setclock) as one on newlib's real-time
// clock, which libnx's timed waits use. Which clock it was is told by which
// "now" it is nearer to: the two are years apart.
int clock_gettime_fake(int clk_id, struct timespec *tp);
void pthr_deadline_to_realtime(const struct timespec *in, struct timespec *out) {
  struct timespec mono, real, nreal;
  clock_gettime_fake(1, &mono);
  clock_gettime_fake(0, &real);
  clock_gettime(CLOCK_REALTIME, &nreal);
  const double t = (double)in->tv_sec + in->tv_nsec / 1e9;
  const double m = (double)mono.tv_sec + mono.tv_nsec / 1e9;
  const double r = (double)real.tv_sec + real.tv_nsec / 1e9;
  const double rel = (fabs(t - m) < fabs(t - r)) ? t - m : t - r;
  double abs_ns = (double)nreal.tv_sec * 1e9 + nreal.tv_nsec + (rel > 0 ? rel * 1e9 : 0);
  out->tv_sec = (time_t)(abs_ns / 1e9);
  out->tv_nsec = (long)(abs_ns - (double)out->tv_sec * 1e9);
  if (out->tv_nsec < 0) out->tv_nsec = 0;
  if (out->tv_nsec >= 1000000000) out->tv_nsec = 999999999;
}

int pthread_cond_timedwait_soloader(pthread_cond_t_bionic *cond, pthread_mutex_t_bionic *mutex,
                                    struct timespec *abstime) {
  if (!cond || !mutex) return EINVAL;
  cond_static_init(cond, NULL);
  mutex_static_init(mutex, NULL);
  struct timespec dl;
  pthr_deadline_to_realtime(abstime, &dl);
  return lx(pthread_cond_timedwait(cond->real_ptr, mutex->real_ptr, &dl));
}

int pthread_attr_init_soloader(pthread_attr_t_bionic *attr) {
  if (!attr) return EINVAL;
  return attr_static_init(attr);
}

int pthread_attr_destroy_soloader(pthread_attr_t_bionic *attr) {
  if (!attr || attr->magic != 0x42424242) return 0;
  int ret = pthread_attr_destroy(attr->real_ptr);
  free(attr->real_ptr);
  attr->magic = 0;
  return ret;
}

int pthread_attr_setdetachstate_soloader(pthread_attr_t_bionic *attr, int state) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setdetachstate(attr->real_ptr, state);
}

int pthread_attr_setstacksize_soloader(pthread_attr_t_bionic *attr, size_t stacksize) {
  if (!attr) return -1;
  attr_static_init(attr);
  return pthread_attr_setstacksize(attr->real_ptr, stacksize);
}
