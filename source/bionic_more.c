/* bionic_more.c -- the libc/libz imports of Unity, IL2CPP, Wwise, the Sakasho
 * SDK, Firebase and Bugsnag that the shared shims did not have yet
 *
 * Threads: stack bounds for the garbage collector (pthread_getattr_np +
 * pthread_attr_getstack), pthread_kill through gcstop.c, timed semaphore and
 * mutex waits on either clock. The rest are small: honest failures for what
 * Horizon has no equivalent of (popen, inotify, affinity), and zlib, which
 * the port links anyway.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fnmatch.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <zlib.h>
#include <switch.h>

#include "config.h"
#include "libc_shim.h"
#include "log.h"
#include "pthr.h"
#include "so_util.h"

// --- logging ---------------------------------------------------------------------------

static int android_log_buf_write(int buf, int prio, const char *tag, const char *msg) {
  (void)buf;
  if (config.log_all && msg)
    log_printf("[%s %d] %s", tag ? tag : "", prio, msg);
  return 1;
}

static void android_log_assert(const char *cond, const char *tag, const char *fmt, ...) {
  char buf[512] = "";
  if (fmt) {
    va_list va;
    va_start(va, fmt);
    vsnprintf(buf, sizeof buf, fmt, va);
    va_end(va);
  }
  log_printf("[assert] %s %s: %s", tag ? tag : "", cond ? cond : "", buf);
  abort_fake();
}

// --- system properties ----------------------------------------------------------------------

static void *system_property_find(const char *name) { (void)name; return NULL; }
static int system_property_read(const void *pi, char *name, char *value) {
  (void)pi;
  if (name) name[0] = '\0';
  if (value) value[0] = '\0';
  return 0;
}

// --- threads ---------------------------------------------------------------------------------

// bionic's pthread_attr_t: {u32 flags; void *stack_base; size_t stack_size;
// size_t guard_size; i32 policy; i32 priority; ...}. pthr.c overlays the
// first 16 bytes; stack info from getattr_np goes in stack_size/guard_size,
// marked in the priority word.
typedef struct {
  void *real_ptr;
  int32_t magic;
  int32_t pad;
  size_t stack_size;
  size_t stack_base;   // (bionic's guard_size slot)
  int32_t policy;
  int32_t marker;
} BAttr;

#define STACK_MARKER 0x4b545453 // "STTK"

static int pthread_getattr_np_fake(pthread_t t, BAttr *attr) {
  if (!attr)
    return 22;
  uintptr_t lo, hi;
  if (t == pthread_self())
    pthr_current_stack(&lo, &hi);
  else if (!pthr_thread_info(t, NULL, &lo, &hi))
    pthr_current_stack(&lo, &hi);
  memset(attr, 0, sizeof(*attr));
  attr->stack_base = lo;
  attr->stack_size = hi - lo;
  attr->marker = STACK_MARKER;
  return 0;
}

static int pthread_attr_getstack_fake(const BAttr *attr, void **addr, size_t *size) {
  uintptr_t lo, hi;
  if (attr && attr->marker == STACK_MARKER) {
    lo = attr->stack_base;
    hi = lo + attr->stack_size;
  } else {
    pthr_current_stack(&lo, &hi);
  }
  if (addr) *addr = (void *)lo;
  if (size) *size = hi - lo;
  return 0;
}

static int pthread_attr_getstacksize_fake(const BAttr *attr, size_t *size) {
  if (size)
    *size = attr && attr->marker == STACK_MARKER ? attr->stack_size : 2 * 1024 * 1024;
  return 0;
}

static int pthread_attr_setstack_fake(BAttr *attr, void *addr, size_t size) {
  (void)attr; (void)addr; (void)size;
  return 0; // threads get stacks of their own here
}

static int pthread_attr_getdetachstate_fake(const BAttr *attr, int *state) {
  (void)attr;
  if (state) *state = 0; // PTHREAD_CREATE_JOINABLE
  return 0;
}

static int pthread_kill_fake(pthread_t t, int sig) {
  int handled;
  // (mkt_nx's gcstop.c took IL2CPP's GC signals here; this game's collector
  // is the Mac build's, which suspends threads with Mach calls: pd_shim.c)
  (void)handled;
  if (sig != 0 && config.log_all)
    log_printf("[thread] pthread_kill(%p, %d) ignored", (void *)t, sig);
  return 0;
}

static void pthread_cleanup_push_fake(void *c, void (*fn)(void *), void *arg) {
  (void)c; (void)fn; (void)arg;
}
static void pthread_cleanup_pop_fake(void *c, int execute) {
  (void)c; (void)execute;
}

static int pthread_setschedparam_fake(pthread_t t, int policy, const void *param) {
  (void)t; (void)policy; (void)param;
  return 0;
}
static int sched_prio_max(int policy) { (void)policy; return 99; }
static int sched_prio_min(int policy) { (void)policy; return 1; }

static int sched_getaffinity_fake(int pid, size_t size, void *mask) {
  (void)pid;
  if (mask && size)
    memset(mask, 0, size), ((uint8_t *)mask)[0] = 0x7;
  return 0;
}
static int sched_setaffinity_fake(int pid, size_t size, const void *mask) {
  (void)pid; (void)size; (void)mask;
  return 0;
}

// bionic's condattr is a long: the clock in it is only ever read back by the
// cond's owner, and the wait itself finds the clock from the deadline
static int condattr_init(long *a) { if (a) *a = 0; return 0; }
static int condattr_destroy(long *a) { (void)a; return 0; }
static int condattr_setclock(long *a, int clk) { if (a) *a = clk; return 0; }

static int sem_timedwait_fake(void **s, const struct timespec *abstime) {
  if (!s || !*s) {
    errno = EINVAL;
    return -1;
  }
  struct timespec dl, now;
  pthr_deadline_to_realtime(abstime, &dl);
  for (;;) {
    if (sem_trywait_fake(s) == 0)
      return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    const int64_t left = (int64_t)(dl.tv_sec - now.tv_sec) * 1000000000ll + (dl.tv_nsec - now.tv_nsec);
    if (left <= 0) {
      errno = 110; // ETIMEDOUT
      return -1;
    }
    svcSleepThread(left < 1000000 ? left : 1000000);
  }
}

static int sem_getvalue_fake(void **s, int *val) {
  if (!s || !*s || !val)
    return -1;
  // libnx's Semaphore keeps its count in a plain field (after the condvar and mutex)
  Semaphore *sem = *s;
  *val = (int)sem->count;
  return 0;
}

static int pthread_mutex_timedlock_fake(void *m, const struct timespec *abstime) {
  struct timespec dl, now;
  pthr_deadline_to_realtime(abstime, &dl);
  for (;;) {
    if (pthread_mutex_trylock_soloader(m) == 0)
      return 0;
    clock_gettime(CLOCK_REALTIME, &now);
    const int64_t left = (int64_t)(dl.tv_sec - now.tv_sec) * 1000000000ll + (dl.tv_nsec - now.tv_nsec);
    if (left <= 0)
      return 110;
    svcSleepThread(left < 500000 ? left : 500000);
  }
}

// --- signals --------------------------------------------------------------------------------

static int sigaltstack_fake(const void *ss, void *old) {
  (void)ss;
  if (old)
    memset(old, 0, 24);
  return 0;
}
static int sigsuspend_fake(const void *mask) {
  (void)mask;
  svcSleepThread(1000000);
  errno = 4; // EINTR
  return -1;
}

// --- files / system --------------------------------------------------------------------------

static int flock_fake(int fd, int op) { (void)fd; (void)op; return 0; }
static int inotify_init_fake(void) { errno = 38; return -1; }
static int inotify_add_watch_fake(int fd, const char *p, uint32_t m) { (void)fd; (void)p; (void)m; errno = 38; return -1; }
static int fchown_fake(int fd, int u, int g) { (void)fd; (void)u; (void)g; return 0; }
static unsigned umask_chk_fake(unsigned m) { (void)m; return 022; }
static int getpriority_fake(int which, int who) { (void)which; (void)who; return 0; }
static void *popen_fake(const char *cmd, const char *mode) { (void)cmd; (void)mode; errno = 38; return NULL; }
static int pclose_fake(void *f) { (void)f; return -1; }
static unsigned sleep_fake(unsigned s) { svcSleepThread((s64)s * 1000000000ll); return 0; }
static int clock_getres_fake(int clk, struct timespec *res) {
  (void)clk;
  if (res) {
    res->tv_sec = 0;
    res->tv_nsec = 1;
  }
  return 0;
}

static char *empty_env[] = { NULL };
static char **environ_fake = empty_env;

struct bionic_utsname {
  char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65];
};
static int uname_fake(struct bionic_utsname *u) {
  if (!u)
    return -1;
  memset(u, 0, sizeof(*u));
  strcpy(u->sysname, "Linux");
  strcpy(u->nodename, "localhost");
  strcpy(u->release, "4.9.0");
  strcpy(u->version, "#1 SMP PREEMPT");
  strcpy(u->machine, "aarch64");
  strcpy(u->domainname, "(none)");
  return 0;
}

// bionic struct statfs (aarch64)
struct bionic_statfs {
  uint64_t f_type, f_bsize, f_blocks, f_bfree, f_bavail, f_files, f_ffree;
  int32_t f_fsid[2];
  uint64_t f_namelen, f_frsize, f_flags, f_spare[4];
};
static int statfs_fake(const char *path, struct bionic_statfs *st) {
  (void)path;
  if (!st)
    return -1;
  memset(st, 0, sizeof(*st));
  s64 fr = 8ll << 30, tot = 64ll << 30;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs) {
    fsFsGetFreeSpace(fs, "/", &fr);
    fsFsGetTotalSpace(fs, "/", &tot);
  }
  st->f_type = 0x4d44; // MSDOS
  st->f_bsize = st->f_frsize = 32768;
  st->f_blocks = (uint64_t)tot / 32768;
  st->f_bfree = st->f_bavail = (uint64_t)fr / 32768;
  st->f_files = st->f_ffree = 1u << 20;
  st->f_namelen = 255;
  return 0;
}

struct bionic_rusage { int64_t utime[2], stime[2]; long rest[14]; };
static int getrusage_fake(int who, struct bionic_rusage *r) {
  (void)who;
  if (r) {
    memset(r, 0, sizeof(*r));
    const u64 us = armTicksToNs(armGetSystemTick()) / 1000;
    r->utime[0] = (int64_t)(us / 1000000);
    r->utime[1] = (int64_t)(us % 1000000);
  }
  return 0;
}

typedef struct { void *b; size_t n; } BIovec;
static ssize_t readv_fake(int fd, const BIovec *iov, int cnt) {
  ssize_t total = 0;
  for (int i = 0; i < cnt; i++) {
    const long r = read_fake(fd, iov[i].b, iov[i].n);
    if (r < 0)
      return total ? total : -1;
    total += r;
    if ((size_t)r < iov[i].n)
      break;
  }
  return total;
}

static char *dirname_fake(char *path) {
  static __thread char buf[1024];
  if (!path || !*path)
    return (char *)".";
  snprintf(buf, sizeof buf, "%s", path);
  char *s = strrchr(buf, '/');
  if (!s)
    return (char *)".";
  if (s == buf)
    s[1] = '\0';
  else
    *s = '\0';
  return buf;
}

// --- networking helpers ---------------------------------------------------------------------

static uint32_t inet_addr_fake(const char *s) {
  unsigned a, b, c, d;
  if (!s || sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
    return 0xffffffffu;
  return (uint32_t)(a | b << 8 | c << 16 | d << 24);
}
static int inet_aton_fake(const char *s, uint32_t *out) {
  const uint32_t a = inet_addr_fake(s);
  if (a == 0xffffffffu && (!s || strcmp(s, "255.255.255.255") != 0))
    return 0;
  if (out) *out = a;
  return 1;
}
static char *inet_ntoa_fake(uint32_t a) {
  static __thread char buf[16];
  snprintf(buf, sizeof buf, "%u.%u.%u.%u", a & 255, (a >> 8) & 255, (a >> 16) & 255, a >> 24);
  return buf;
}
static void *gethostbyaddr_fake(const void *a, int len, int type) { (void)a; (void)len; (void)type; return NULL; }
static __thread int t_h_errno;
static int *get_h_errno_fake(void) { return &t_h_errno; }

// bionic's struct passwd: Unity looks up the home directory there when
// $HOME is unset
struct bionic_passwd {
  char *pw_name, *pw_passwd;
  uint32_t pw_uid, pw_gid;
  char *pw_gecos, *pw_dir, *pw_shell;
};
static struct bionic_passwd *getpwuid_fake2(uint32_t uid) {
  static struct bionic_passwd pw;
  static char dir[300];
  snprintf(dir, sizeof dir, "%s", getenv("HOME") ? getenv("HOME") : "/");
  pw.pw_name = (char *)"u0_a123";
  pw.pw_passwd = (char *)"";
  pw.pw_uid = uid;
  pw.pw_gid = uid;
  pw.pw_gecos = (char *)"";
  pw.pw_dir = dir;
  pw.pw_shell = (char *)"/system/bin/sh";
  return &pw;
}

static long timezone_fake = 0;
static char *tzname_fake[2] = { (char *)"UTC", (char *)"UTC" };

DynLibFunction more_functions[] = {
  { "__android_log_buf_write", (uintptr_t)&android_log_buf_write },
  { "__android_log_assert", (uintptr_t)&android_log_assert },
  { "__system_property_find", (uintptr_t)&system_property_find },
  { "__system_property_read", (uintptr_t)&system_property_read },
  { "pthread_getattr_np", (uintptr_t)&pthread_getattr_np_fake },
  { "pthread_attr_getstack", (uintptr_t)&pthread_attr_getstack_fake },
  { "pthread_attr_getstacksize", (uintptr_t)&pthread_attr_getstacksize_fake },
  { "pthread_attr_setstack", (uintptr_t)&pthread_attr_setstack_fake },
  { "pthread_attr_getdetachstate", (uintptr_t)&pthread_attr_getdetachstate_fake },
  { "pthread_kill", (uintptr_t)&pthread_kill_fake },
  { "__pthread_cleanup_push", (uintptr_t)&pthread_cleanup_push_fake },
  { "__pthread_cleanup_pop", (uintptr_t)&pthread_cleanup_pop_fake },
  { "pthread_setschedparam", (uintptr_t)&pthread_setschedparam_fake },
  { "pthread_condattr_init", (uintptr_t)&condattr_init },
  { "pthread_condattr_destroy", (uintptr_t)&condattr_destroy },
  { "pthread_condattr_setclock", (uintptr_t)&condattr_setclock },
  { "pthread_mutex_timedlock", (uintptr_t)&pthread_mutex_timedlock_fake },
  { "sched_get_priority_max", (uintptr_t)&sched_prio_max },
  { "sched_get_priority_min", (uintptr_t)&sched_prio_min },
  { "sched_getaffinity", (uintptr_t)&sched_getaffinity_fake },
  { "sched_setaffinity", (uintptr_t)&sched_setaffinity_fake },
  { "sem_timedwait", (uintptr_t)&sem_timedwait_fake },
  { "sem_getvalue", (uintptr_t)&sem_getvalue_fake },
  { "sigaltstack", (uintptr_t)&sigaltstack_fake },
  { "sigsuspend", (uintptr_t)&sigsuspend_fake },
  { "flock", (uintptr_t)&flock_fake },
  { "fnmatch", (uintptr_t)&fnmatch },
  { "inotify_init", (uintptr_t)&inotify_init_fake },
  { "inotify_add_watch", (uintptr_t)&inotify_add_watch_fake },
  { "fchown", (uintptr_t)&fchown_fake },
  { "__umask_chk", (uintptr_t)&umask_chk_fake },
  { "getpriority", (uintptr_t)&getpriority_fake },
  { "popen", (uintptr_t)&popen_fake },
  { "pclose", (uintptr_t)&pclose_fake },
  { "sleep", (uintptr_t)&sleep_fake },
  { "clock_getres", (uintptr_t)&clock_getres_fake },
  { "environ", (uintptr_t)&environ_fake },
  { "uname", (uintptr_t)&uname_fake },
  { "statfs", (uintptr_t)&statfs_fake },
  { "statfs64", (uintptr_t)&statfs_fake },
  { "getrusage", (uintptr_t)&getrusage_fake },
  { "readv", (uintptr_t)&readv_fake },
  { "dirname", (uintptr_t)&dirname_fake },
  { "getline", (uintptr_t)&__getline },
  { "inet_addr", (uintptr_t)&inet_addr_fake },
  { "inet_aton", (uintptr_t)&inet_aton_fake },
  { "inet_ntoa", (uintptr_t)&inet_ntoa_fake },
  { "gethostbyaddr", (uintptr_t)&gethostbyaddr_fake },
  { "__get_h_errno", (uintptr_t)&get_h_errno_fake },
  { "timezone", (uintptr_t)&timezone_fake },
  { "getpwuid", (uintptr_t)&getpwuid_fake2 },
  { "tzname", (uintptr_t)&tzname_fake },
  // zlib: the port's own (same ABI and z_stream layout as Android's)
  { "inflate", (uintptr_t)&inflate },
  { "inflateEnd", (uintptr_t)&inflateEnd },
  { "inflateInit2_", (uintptr_t)&inflateInit2_ },
  { "inflateInit_", (uintptr_t)&inflateInit_ },
  { "inflateReset", (uintptr_t)&inflateReset },
  { "deflate", (uintptr_t)&deflate },
  { "deflateEnd", (uintptr_t)&deflateEnd },
  { "deflateInit2_", (uintptr_t)&deflateInit2_ },
  { "deflateInit_", (uintptr_t)&deflateInit_ },
  { "crc32", (uintptr_t)&crc32 },
  { "adler32", (uintptr_t)&adler32 },
  { "zError", (uintptr_t)&zError },
  { "compress", (uintptr_t)&compress },
  { "compress2", (uintptr_t)&compress2 },
  { "compressBound", (uintptr_t)&compressBound },
  { "uncompress", (uintptr_t)&uncompress },
};

size_t more_numfunctions = sizeof(more_functions) / sizeof(*more_functions);
