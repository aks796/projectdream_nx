/* libc_shim.c -- bionic-compatible libc wrappers for the game's libraries
 *
 * Every library is linked against bionic. Where the bionic and newlib ABIs
 * differ (struct layouts, flag values, missing functions) we provide
 * converting wrappers here; everything that matches is passed straight
 * through from imports.c.
 *
 * File calls also go through vfs.c: the Nabe asset cache under the files
 * dir is served from the APK's nabe.tar, never from the SD.
 *
 * Adapted from lbbg_nx (fgsfds / gm666q lineage) by way of gd_geode_nx.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license. See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <malloc.h>
#include <locale.h>
#include <wchar.h>
#include <wctype.h>
#include <time.h>
#include <sys/stat.h>
#include <switch.h>

#include "vfs.h"
#include "vmem.h"

// no <sys/uio.h> in devkitA64 newlib; bionic's iovec layout
struct iovec {
  void *iov_base;
  size_t iov_len;
};

#include "config.h"
#include "paths.h"
#include "save_writer.h"
#include "so_util.h"
#include "bionic_extra.h"
#include "log.h"
#include "libc_shim.h"
#include "pthr.h"
#include "net_shim.h"
#include "sandbox.h"

// ---------------------------------------------------------------------------
// fortify (_chk) wrappers: ignore the object-size argument
// ---------------------------------------------------------------------------

void *__memcpy_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memcpy(dst, src, n);
}

void *__memmove_chk_fake(void *dst, const void *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return memmove(dst, src, n);
}

void *__memset_chk_fake(void *dst, int c, size_t n, size_t dstlen) {
  (void)dstlen;
  return memset(dst, c, n);
}

char *__strrchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strrchr(s, c);
}

long __read_chk_fake(int fd, void *buf, size_t count, size_t buf_size) {
  (void)buf_size;
  return read_fake(fd, buf, count);
}

char *__strcat_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcat(dst, src);
}

char *__strchr_chk_fake(const char *s, int c, size_t slen) {
  (void)slen;
  return strchr(s, c);
}

char *__strcpy_chk_fake(char *dst, const char *src, size_t dstlen) {
  (void)dstlen;
  return strcpy(dst, src);
}

size_t __strlen_chk_fake(const char *s, size_t slen) {
  (void)slen;
  return strlen(s);
}

char *__strncat_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncat(dst, src, n);
}

char *__strncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) {
  (void)dstlen;
  return strncpy(dst, src, n);
}

int __vsnprintf_chk_fake(char *s, size_t maxlen, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsnprintf(s, maxlen, fmt, va);
}

int __vsprintf_chk_fake(char *s, int flag, size_t slen, const char *fmt, va_list va) {
  (void)flag; (void)slen;
  return vsprintf(s, fmt, va);
}

// fortified FD_SET (libfmod's select loop); fd_set is bit-per-fd on both ABIs
void __FD_SET_chk_fake(int fd, void *set, size_t setlen) {
  (void)setlen;
  if (fd >= 0 && fd < (int)(8 * sizeof(fd_set)))
    FD_SET(fd, (fd_set *)set);
}

// ---------------------------------------------------------------------------
// misc bionic functions
// ---------------------------------------------------------------------------

int __system_property_get_fake(const char *name, char *value) {
  (void)name;
  value[0] = '\0';
  return 0;
}

// 0 = "no hwcaps": the game's embedded OpenSSL takes its portable C paths
// instead of probing for ARMv8 crypto extensions.
unsigned long getauxval_fake(unsigned long type) {
  (void)type;
  return 0;
}

int gettid_fake(void) {
  u64 thread_id = 1;
  if (R_SUCCEEDED(svcGetThreadId(&thread_id, CUR_THREAD_HANDLE)) && thread_id)
    return (int)(thread_id & 0x7fffffff);
  return 1;
}

#define ARM64_SYS_FUTEX 98
#define ARM64_SYS_GETTID 178
#define ARM64_SYS_GETRANDOM 278

// ---------------------------------------------------------------------------
// raw syscalls. Rust code calls the kernel through syscall() rather than
// libc: getrandom for its random numbers (Globed's sign-in library drew a
// nonce from it, got ENOSYS, panicked and aborted the game), futex for its
// mutexes, condition variables and thread parking.
// ---------------------------------------------------------------------------

// futex: the kernel's address arbitration, which is a futex: a waiter sleeps
// on the word's address while it holds the value (svcWaitForAddress,
// WaitIfEqual; the kernel checks and sleeps atomically), a waker signals up to
// count waiters on it (svcSignalToAddress). There is no lock of the port's
// in it. (It used to be a mutex and condition variable per bucket of
// addresses: a thread woken in it owns the bucket's mutex again until it
// returns, and when the garbage collector paused it there (gcstop.c), every
// other thread's futex call on that bucket -- the main thread's included --
// waited for ever.) The count of threads woken is not known; 0 is returned,
// which no caller here reads.
static long futex_wait(const uint32_t *word, uint32_t val, const struct timespec *ts, int absolute,
                       int realtime) {
  s64 ns = -1;
  if (ts) {
    ns = (s64)ts->tv_sec * 1000000000ll + ts->tv_nsec;
    if (absolute) {
      struct timespec now;
      clock_gettime_fake(realtime ? 0 : 1, &now);
      ns -= (s64)now.tv_sec * 1000000000ll + now.tv_nsec;
    }
    if (ns < 0)
      ns = 0;
  }
  const Result rc = svcWaitForAddress((void *)word, ArbitrationType_WaitIfEqual, (s32)val, ns);
  if (R_SUCCEEDED(rc))
    return 0;
  if (rc == KERNELRESULT(InvalidState)) { // the word no longer holds val
    errno = 11; // EAGAIN
    return -1;
  }
  if (rc == KERNELRESULT(TimedOut)) {
    errno = 110; // ETIMEDOUT
    return -1;
  }
  return 0; // cancelled: a spurious wake, which futex users handle
}

static long futex_wake(const uint32_t *word, int count) {
  svcSignalToAddress((void *)word, SignalType_Signal, 0, count > 0 ? count : 1);
  return 0;
}

long syscall_fake(long number, ...) {
  va_list va;
  va_start(va, number);
  long a[6];
  for (int i = 0; i < 6; i++)
    a[i] = va_arg(va, long);
  va_end(va);
  switch (number) {
    case ARM64_SYS_GETTID:
      return gettid_fake();
    case 122: // sched_setaffinity
      return 0;
    case 123: // sched_getaffinity(pid, size, mask)
      if (a[2] && a[1] > 0) {
        memset((void *)a[2], 0, (size_t)a[1]);
        *(uint8_t *)a[2] = 0x7;
      }
      return 8;
    case ARM64_SYS_GETRANDOM: // (buf, len, flags): the console's CSPRNG never blocks
      if (a[1] > 0)
        randomGet((void *)a[0], (size_t)a[1]);
      return a[1];
    case ARM64_SYS_FUTEX: { // (uaddr, op, val, timeout, uaddr2, val3)
      const int op = (int)a[1] & 127;               // without FUTEX_PRIVATE_FLAG
      const int realtime = ((int)a[1] & 256) != 0;  // FUTEX_CLOCK_REALTIME
      switch (op) {
        case 0: // FUTEX_WAIT: relative timeout
          return futex_wait((const uint32_t *)a[0], (uint32_t)a[2], (const struct timespec *)a[3], 0, 0);
        case 9: // FUTEX_WAIT_BITSET: absolute timeout (the bitset is ignored)
          return futex_wait((const uint32_t *)a[0], (uint32_t)a[2], (const struct timespec *)a[3], 1,
                            realtime);
        case 1:  // FUTEX_WAKE
        case 10: // FUTEX_WAKE_BITSET
          return futex_wake((const uint32_t *)a[0], (int)a[2]);
      }
      break;
    }
  }
  static uint64_t logged[8]; // once per number (below 512)
  if (number >= 0 && number < 512 && !(logged[number / 64] & (1ull << (number % 64)))) {
    logged[number / 64] |= 1ull << (number % 64);
    if (number == ARM64_SYS_FUTEX)
      log_printf("[syscall] futex op %ld not provided", a[1]);
    else
      log_printf("[syscall] %ld not provided (ENOSYS)", number);
  }
  errno = 38; // Linux ENOSYS (newlib's is 88)
  return -1;
}

void sincos_fake(double x, double *s, double *c) {
  *s = sin(x);
  *c = cos(x);
}

void sincosf_fake(float x, float *s, float *c) {
  *s = sinf(x);
  *c = cosf(x);
}

void android_set_abort_message_fake(const char *msg) {
  log_printf("[abort message] %s", msg ? msg : "(null)");
}

size_t __ctype_get_mb_cur_max_fake(void) {
  return 1;
}

int pthread_setname_np_fake(void *thread, const char *name) {
  pthr_set_name((pthread_t)thread, name);
  return 0;
}

// bionic sysconf constants
#define BIONIC_SC_PAGESIZE 39
#define BIONIC_SC_PAGE_SIZE 40
#define BIONIC_SC_NPROCESSORS_CONF 96
#define BIONIC_SC_NPROCESSORS_ONLN 97
#define BIONIC_SC_PHYS_PAGES 98

long sysconf_fake(int name) {
  switch (name) {
    case BIONIC_SC_PAGESIZE:
    case BIONIC_SC_PAGE_SIZE:
      return 0x1000;
    case BIONIC_SC_NPROCESSORS_CONF:
    case BIONIC_SC_NPROCESSORS_ONLN:
      return 3;
    case BIONIC_SC_PHYS_PAGES:
      return (3ll * 1024 * 1024 * 1024) / 0x1000;
    default:
      return -1;
  }
}

// High-resolution monotonic clocks use the 19.2 MHz system tick. Real-time
// IDs must use the console RTC: OpenSSL uses wall-clock time when validating
// certificate validity periods. Bionic's IDs differ from newlib's (Android
// CLOCK_REALTIME is 0; newlib CLOCK_REALTIME is 1).
#define FAKE_EPOCH_BASE 1700000000ull // ~2023-11, seconds

int64_t android_monotonic_ns(void) {
  return (int64_t)FAKE_EPOCH_BASE * 1000000000ll + (int64_t)armTicksToNs(armGetSystemTick());
}

int clock_gettime_fake(int clk_id, struct timespec *tp) {
  if (!tp)
    return -1;
  // Android CLOCK_REALTIME / REALTIME_COARSE / REALTIME_ALARM.
  if (clk_id == 0 || clk_id == 5 || clk_id == 8)
    return clock_gettime(CLOCK_REALTIME, tp);
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq(); // 19200000 on the Switch
  const u64 tick = armGetSystemTick();
  tp->tv_sec = (time_t)(FAKE_EPOCH_BASE + tick / freq);
  tp->tv_nsec = (long)(((tick % freq) * 1000000000ull) / freq);
  return 0;
}

// bionic (BSD-style) fpclassify values
#define BIONIC_FP_INFINITE  0x01
#define BIONIC_FP_NAN       0x02
#define BIONIC_FP_NORMAL    0x04
#define BIONIC_FP_SUBNORMAL 0x08
#define BIONIC_FP_ZERO      0x10

int __isnanf_fake(float f) {
  return isnan(f);
}

int __fpclassifyd_fake(double d) {
  switch (fpclassify(d)) {
    case FP_INFINITE:  return BIONIC_FP_INFINITE;
    case FP_NAN:       return BIONIC_FP_NAN;
    case FP_SUBNORMAL: return BIONIC_FP_SUBNORMAL;
    case FP_ZERO:      return BIONIC_FP_ZERO;
    default:           return BIONIC_FP_NORMAL;
  }
}

// bionic basename(3): never modifies its argument
char *basename_fake(const char *path) {
  if (!path || !*path)
    return (char *)".";
  const char *slash = strrchr(path, '/');
  return (char *)(slash ? slash + 1 : path);
}

void *memrchr_fake(const void *s, int c, size_t n) {
  const unsigned char *p = (const unsigned char *)s + n;
  while (n--) {
    if (*--p == (unsigned char)c)
      return (void *)p;
  }
  return NULL;
}

void __assert2_fake(const char *file, int line, const char *func, const char *expr) {
  log_printf("[assert] %s:%d %s: %s", file ? file : "?", line, func ? func : "?",
             expr ? expr : "?");
  abort_fake();
}

// where a call came from, as module+offset, for the abort/exit lines
static void log_caller(const char *what, uintptr_t lr) {
  so_dl_info info;
  if (so_dladdr((void *)lr, &info) && info.dli_fbase)
    log_printf("[%s] called from %s+0x%lx (%s)", what, info.dli_fname,
               (unsigned long)(lr - (uintptr_t)info.dli_fbase),
               info.dli_sname ? info.dli_sname : "?");
  else
    log_printf("[%s] called from %p", what, (void *)lr);
}

// the frame-pointer chain above the caller: where an abort came from (the
// modules keep frame pointers: the arm64 ABI)
static void log_backtrace(const char *what) {
  uintptr_t *fp = __builtin_frame_address(0);
  for (int i = 0; i < 24 && fp; i++) {
    const uintptr_t lr = fp[1];
    uintptr_t *next = (uintptr_t *)fp[0];
    if (!lr)
      break;
    log_caller(what, lr);
    if (next <= fp || (uintptr_t)next - (uintptr_t)fp > 0x100000)
      break; // the chain must climb the stack
    fp = next;
  }
}

void abort_fake(void) {
  log_caller("abort", (uintptr_t)__builtin_return_address(0));
  log_backtrace("abort trace");
  log_flush();
  abort();
}

void exit_fake(int code) {
  log_caller("exit", (uintptr_t)__builtin_return_address(0));
  log_printf("[exit] code %d", code);
  log_flush();
  exit(code);
}

int __android_log_write_fake(int prio, const char *tag, const char *msg) {
  (void)prio; (void)tag; (void)msg;
  return 0;
}

int setpriority_fake(int which, int who, int prio) {
  (void)which; (void)who; (void)prio;
  return 0;
}

// ---------------------------------------------------------------------------
// process/signal stubs: curl and the game's crash guards import these; none
// of it can work on Horizon, and all callers tolerate failure
// ---------------------------------------------------------------------------

int getpid_fake(void) { return 1000; }
int fork_fake(void) { errno = ENOSYS; return -1; }
int execl_fake(const char *path, const char *arg, ...) {
  (void)path; (void)arg;
  errno = ENOSYS;
  return -1;
}
int waitpid_fake(int pid, int *status, int options) {
  (void)pid; (void)options;
  if (status) *status = 0;
  errno = ECHILD;
  return -1;
}
int kill_fake(int pid, int sig) { (void)pid; (void)sig; return 0; }
unsigned alarm_fake(unsigned sec) { (void)sec; return 0; }
void *signal_fake(int sig, void *handler) { (void)sig; (void)handler; return NULL; }
int sigaction_fake(int sig, const void *act, void *oact) {
  (void)sig; (void)act; (void)oact;
  return 0;
}
int sigprocmask_fake(int how, const void *set, void *oset) {
  (void)how; (void)set; (void)oset;
  return 0;
}

// ---------------------------------------------------------------------------
// path remapping: every GD variant hardcodes a different Android package dir
// in a few places (song/save paths built before JNI's writable path is queried)
// ---------------------------------------------------------------------------

const char *fix_path(const char *path) {
  if (!path)
    return path;
  // OpenSSL builds look for a CA bundle at their build machine's OPENSSLDIR;
  // the offline server's CA is written to internal/cacert.pem
  static const char *ca_suffixes[] = {
    "/ssl/cert.pem",
    "/ssl/certs/ca-certificates.crt",
    "/etc/ssl/cert.pem",
    "/etc/ssl/certs/ca-certificates.crt",
  };
  const size_t path_len = strlen(path);
  for (unsigned i = 0; i < sizeof(ca_suffixes) / sizeof(*ca_suffixes); i++) {
    const size_t suffix_len = strlen(ca_suffixes[i]);
    if (path_len >= suffix_len &&
        strcmp(path + path_len - suffix_len, ca_suffixes[i]) == 0) {
      static char ca[300];
      snprintf(ca, sizeof ca, "%s/cacert.pem", path_internal());
      return ca;
    }
  }
  return path_from_android(path);
}

// ---------------------------------------------------------------------------
// /dev/urandom emulation: the embedded OpenSSL seeds its RNG from it (open/
// read/fstat/close on a raw fd). Served from Horizon's csrng.
// ---------------------------------------------------------------------------

#define URANDOM_FD_BASE 0x7f000000

int is_urandom_fd_fake(int fd) {
  return fd >= URANDOM_FD_BASE && fd < URANDOM_FD_BASE + 16;
}

static int is_urandom_path(const char *path) {
  return strcmp(path, "/dev/urandom") == 0 || strcmp(path, "/dev/random") == 0 ||
         strcmp(path, "/dev/srandom") == 0;
}

// ---------------------------------------------------------------------------
// open() flag translation (bionic/linux -> newlib)
// ---------------------------------------------------------------------------

#define LINUX_O_CREAT  0100
#define LINUX_O_EXCL   0200
#define LINUX_O_TRUNC  01000
#define LINUX_O_APPEND 02000
#define LINUX_O_NONBLOCK 04000
#define LINUX_O_DIRECTORY 0200000

static int convert_open_flags(int flags) {
  int out = flags & 3; // O_RDONLY/O_WRONLY/O_RDWR match
  if (flags & LINUX_O_CREAT)  out |= O_CREAT;
  if (flags & LINUX_O_EXCL)   out |= O_EXCL;
  if (flags & LINUX_O_TRUNC)  out |= O_TRUNC;
  if (flags & LINUX_O_APPEND) out |= O_APPEND;
  if (flags & LINUX_O_NONBLOCK) out |= O_NONBLOCK;
  return out;
}

// an open that can change the file: writing, creating, truncating
static int open_writes(int flags) {
  return (flags & 3) != 0 || (flags & (LINUX_O_CREAT | LINUX_O_TRUNC | LINUX_O_APPEND)) != 0;
}

// debug (log_fs): the game's saves traced call by call (which calls, what sizes): fds of paths with these names
static uint8_t s_traced[1024];
static u64 s_trace_t0[1024];

static int traced_path(const char *p) {
  return p && (strstr(p, "Save_ID") || strstr(p, "icloud_kvs"));
}

void fs_trace(int fd, const char *what, long a, long r) {
  if (fd >= 0 && fd < 1024 && s_traced[fd])
    log_printf("[save] fd %d %s %ld -> %ld (%.1f ms since open)", fd, what, a, r,
               armTicksToNs(armGetSystemTick() - s_trace_t0[fd]) / 1e6);
}

static void log_open(const char *what, const char *path, int fd) {
  if (config.log_fs)
    log_printf("[fs] %s %s -> %d%s", what, path, fd, fd < 0 ? strerror(errno) : "");
}

// open(2) with bionic flags on an already-mapped path; the tar's files are
// virtual fds unless they are being changed
static int open_mapped(const char *p, int flags, int mode) {
  const NabeFile *nf = NULL;
  const VfsKind k = vfs_classify(p, &nf);
  if (k == VFS_VFILE && !open_writes(flags)) {
    const int fd = vfd_open(nf);
    log_open("open(tar)", p, fd);
    return fd;
  }
  if (k == VFS_GONE && !(flags & LINUX_O_CREAT)) {
    errno = ENOENT;
    log_open("open(deleted)", p, -1);
    return -1;
  }
  if (open_writes(flags)) {
    if (!sandbox_check(p, "open for writing"))
      return -1;
    if (k == VFS_VFILE || k == VFS_GONE || k == VFS_REAL) {
      if (vfs_prepare_write(p, (flags & LINUX_O_TRUNC) || k == VFS_GONE) < 0)
        return -1;
    }
  }
  if (k == VFS_VDIR && (flags & LINUX_O_DIRECTORY)) {
    // a directory fd onto a tar directory: the SD's copy of it
    mkdir_fake(p, 0777);
  }
  const int sfd = sw_open(p, flags); // the game's saves: in memory, written to the card on a thread (save_writer.c)
  if (sfd != -2) {
    log_open("open(save)", p, sfd);
    return sfd;
  }
  const u64 t_open = armGetSystemTick();
  const int fd = open(p, convert_open_flags(flags), mode);
  if (fd < 0)
    errno = net_errno_to_linux(errno);
  else if (open_writes(flags))
    vfs_note_real(p);
  log_open("open", p, fd);
  if (fd >= 0 && fd < 1024) {
    s_traced[fd] = config.log_fs && traced_path(p);
    s_trace_t0[fd] = t_open;
    if (s_traced[fd])
      log_printf("[save] open %s flags 0x%x -> fd %d in %.1f ms", p, flags, fd,
                 armTicksToNs(armGetSystemTick() - t_open) / 1e6);
  }
  return fd;
}

// /sys/devices/system/cpu/{possible,present,online}: the CPUs as a list ("0-2"). Unity's cpufeatures reads
// "possible" with open() and counts one core without it (one core: no job worker threads). The console gives
// an application three cores.
static const char k_cpu_list[] = "0-2\n";

static int cpu_list_path(const char *path) {
  return path && (!strcmp(path, "/sys/devices/system/cpu/possible") ||
                  !strcmp(path, "/sys/devices/system/cpu/present") || !strcmp(path, "/sys/devices/system/cpu/online"));
}

// what is known not to be there, said at once: /sys (sys_file) and the
// memory map the game's libraries look at now and then
static int known_missing(const char *path) {
  if (path && (!strncmp(path, "/sys/", 5) || !strcmp(path, "/proc/self/maps"))) {
    errno = ENOENT;
    return 1;
  }
  return 0;
}

static int open_cpu_list(void) {
  char p[512];
  snprintf(p, sizeof p, "%s/cpu_list", path_internal());
  FILE *f = fopen(p, "w");
  if (f) {
    fputs(k_cpu_list, f);
    fclose(f);
  }
  return open(p, O_RDONLY);
}

int open_fake(const char *path, int flags, ...) {
  if (is_urandom_path(path))
    return URANDOM_FD_BASE;
  if (cpu_list_path(path))
    return open_cpu_list();
  if (known_missing(path))
    return -1;
  int mode = 0666;
  if (flags & LINUX_O_CREAT) {
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  return open_mapped(fix_path(path), flags, mode);
}

// bionic's fortified open with no variadic mode (read/existing files)
int open2_fake(const char *path, int flags) {
  if (is_urandom_path(path))
    return URANDOM_FD_BASE;
  if (cpu_list_path(path))
    return open_cpu_list();
  if (known_missing(path))
    return -1;
  return open_mapped(fix_path(path), flags, 0666);
}

void io_note(size_t asked, long got, u64 ticks); // bionic_extra.c
int fd_is_sd_file(int fd);

long read_fake(int fd, void *buf, size_t count) {
  if (is_urandom_fd_fake(fd)) {
    randomGet(buf, count); // kernel-entropy chacha, no service needed
    return (long)count;
  }
  if (vfd_is(fd))
    return vfd_read(fd, buf, count);
  if (sw_is_memfd(fd))
    return sw_read(fd, buf, count);
  long result;
  if (pipe_read_fake(fd, buf, count, &result) || net_mem_fd_read(fd, buf, count, &result))
    return result;
  const u64 t0 = armGetSystemTick();
  result = read(fd, buf, count);
  if (fd_is_sd_file(fd))
    io_note(count, result, armGetSystemTick() - t0);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  net_trace(fd, "read", (long)count, result);
  return result;
}

long write_fake(int fd, const void *buf, size_t count) {
  if (vfd_is(fd)) {
    errno = EBADF; // opened read-only
    return -1;
  }
  if (sw_is_memfd(fd))
    return sw_write(fd, buf, count);
  long result;
  if (pipe_write_fake(fd, buf, count, &result) || net_mem_fd_write(fd, buf, count, &result))
    return result;
  result = write(fd, buf, count);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  net_trace(fd, "write", (long)count, result);
  fs_trace(fd, "write", (long)count, result);
  return result;
}

int close_fake(int fd) {
  if (is_urandom_fd_fake(fd))
    return 0;
  if (vfd_is(fd))
    return vfd_close(fd);
  if (sw_is_memfd(fd))
    return sw_close(fd);
  if (is_dirfd_fake(fd)) {
    dirfd_close_fake(fd);
    return 0;
  }
  if (epoll_close_fake(fd) || pipe_close_fake(fd))
    return 0;
  net_forget_fd(fd);
  int result = close(fd);
  if (result < 0)
    errno = net_errno_to_linux(errno);
  fs_trace(fd, "close", 0, result);
  if (fd >= 0 && fd < 1024)
    s_traced[fd] = 0;
  return result;
}

// bionic SEEK_* match newlib's (0, 1, 2)
long lseek_fake(int fd, long off, int whence) {
  if (vfd_is(fd))
    return (long)vfd_lseek(fd, off, whence);
  if (sw_is_memfd(fd))
    return sw_lseek(fd, off, whence);
  if (is_urandom_fd_fake(fd))
    return 0;
  const long r = lseek(fd, off, whence);
  if (r < 0)
    errno = net_errno_to_linux(errno);
  fs_trace(fd, whence == 0 ? "lseek set" : whence == 1 ? "lseek cur" : "lseek end", off, r);
  return r;
}

int access_fake(const char *path, int mode) {
  (void)mode;
  const char *p = fix_path(path);
  switch (vfs_classify(p, NULL)) {
    case VFS_VFILE:
    case VFS_VDIR:
      return 0;
    case VFS_GONE:
      errno = ENOENT;
      return -1;
    default:
      break;
  }
  struct stat st;
  const int r = stat(p, &st);
  if (r < 0)
    errno = net_errno_to_linux(errno);
  return r;
}

int chmod_fake(const char *path, unsigned mode) {
  (void)path; (void)mode;
  return 0;
}

int mkdir_fake(const char *path, unsigned int mode) {
  if (path_android_ancestor(path)) {
    errno = EEXIST;
    return -1;
  }
  const char *p = fix_path(path);
  if (!sandbox_check(p, "mkdir"))
    return -1;
  struct stat st;
  if (stat(p, &st) == 0) {
    errno = EEXIST;
    return -1;
  }
  // Recursive: create each component, ignoring "exists".
  char tmp[1024];
  size_t n = strnlen(p, sizeof(tmp) - 1);
  memcpy(tmp, p, n);
  tmp[n] = '\0';
  for (char *s = tmp + 1; *s; s++) {
    if (*s == '/') {
      *s = '\0';
      mkdir(tmp, mode); // ignore EEXIST and intermediate failures
      *s = '/';
    }
  }
  const int r = mkdir(tmp, mode);
  if (r < 0)
    errno = net_errno_to_linux(errno);
  return r;
}

int remove_fake(const char *path) {
  const char *p = fix_path(path);
  sw_wait_path(p);
  if (!sandbox_check(p, "remove"))
    return -1;
  const VfsKind k = vfs_classify(p, NULL);
  if (k == VFS_GONE) {
    errno = ENOENT;
    return -1;
  }
  if (k == VFS_VFILE) {
    vfs_note_removed(p);
    return 0;
  }
  const int r = remove(p);
  if (r < 0) {
    errno = net_errno_to_linux(errno);
    return -1;
  }
  vfs_note_removed(p); // an SD copy of a tar file: hide the tar's too
  return r;
}

int rename_fake(const char *from, const char *to) {
  // both sides need remapping; fix_path uses rotating buffers so a single
  // call to each is safe before we hand them to rename()
  const char *f = fix_path(from);
  const char *t = fix_path(to);
  // both ends: moving a file in from elsewhere takes it away from there
  if (!sandbox_check(f, "rename from") || !sandbox_check(t, "rename to"))
    return -1;
  const int sr = sw_rename(f, t); // a save not on the card yet goes to its new name there
  if (sr != -2)
    return sr;
  sw_wait_path(f);
  const NabeFile *nf = NULL;
  const VfsKind kf = vfs_classify(f, &nf);
  if (kf == VFS_GONE) {
    errno = ENOENT;
    return -1;
  }
  if (kf == VFS_VFILE)
    return vfs_rename_virtual(nf, f, t);
  vfs_prepare_write(t, 1);
  // Horizon's filesystem RenameFile FAILS if the destination already exists,
  // whereas POSIX rename() atomically replaces it. Try the rename first: when
  // the destination does not exist the remove() would be a wasted FS call.
  if (rename(f, t) != 0) {
    // only when there is something to move: a rename whose source is missing
    // must not take the destination with it
    struct stat st;
    if (stat(f, &st) != 0) {
      errno = net_errno_to_linux(errno);
      return -1;
    }
    remove(t);
    if (rename(f, t) != 0) {
      errno = net_errno_to_linux(errno);
      return -1;
    }
  }
  vfs_note_real(t);
  vfs_note_removed(f);
  return 0;
}

int chdir_fake(const char *path) {
  return chdir(fix_path(path));
}

// ---------------------------------------------------------------------------
// struct stat conversion (bionic aarch64 layout)
// ---------------------------------------------------------------------------

struct bionic_timespec {
  int64_t tv_sec;
  int64_t tv_nsec;
};

struct bionic_stat {
  uint64_t st_dev;
  uint64_t st_ino;
  uint32_t st_mode;
  uint32_t st_nlink;
  uint32_t st_uid;
  uint32_t st_gid;
  uint64_t st_rdev;
  uint64_t __pad1;
  int64_t st_size;
  int32_t st_blksize;
  int32_t __pad2;
  int64_t st_blocks;
  struct bionic_timespec st_atim;
  struct bionic_timespec st_mtim;
  struct bionic_timespec st_ctim;
  uint32_t __unused4;
  uint32_t __unused5;
};

static void convert_stat(const struct stat *in, struct bionic_stat *out) {
  memset(out, 0, sizeof(*out));
  out->st_dev = in->st_dev;
  out->st_ino = in->st_ino;
  out->st_mode = in->st_mode;
  out->st_nlink = in->st_nlink;
  out->st_uid = in->st_uid;
  out->st_gid = in->st_gid;
  out->st_rdev = in->st_rdev;
  out->st_size = in->st_size;
  out->st_blksize = in->st_blksize;
  out->st_blocks = in->st_blocks;
  out->st_atim.tv_sec = in->st_atime;
  out->st_mtim.tv_sec = in->st_mtime;
  out->st_ctim.tv_sec = in->st_ctime;
}

// the stat of a tar member / directory: a fixed date (the tar's), sizes as
// the tar has them
#define TAR_MTIME 1790000000ll

static void virtual_stat(struct bionic_stat *st, int is_dir, uint64_t size, uint64_t ino) {
  memset(st, 0, sizeof(*st));
  st->st_mode = is_dir ? (S_IFDIR | 0777) : (S_IFREG | 0666);
  st->st_nlink = 1;
  st->st_size = (int64_t)size;
  st->st_blksize = 4096;
  st->st_blocks = (int64_t)((size + 511) / 512);
  st->st_ino = ino;
  st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = TAR_MTIME;
}

int stat_fake(const char *path, struct bionic_stat *st) {
  const char *p = fix_path(path);
  size_t pending = 0;
  if (sw_stat(p, &pending)) { // a save not on the card yet: its newest size
    virtual_stat(st, 0, pending, 0x5a5e);
    return 0;
  }
  const NabeFile *nf = NULL;
  switch (vfs_classify(p, &nf)) {
    case VFS_VFILE:
      virtual_stat(st, 0, nf->size, nf->off);
      return 0;
    case VFS_VDIR:
      virtual_stat(st, 1, 0, 1);
      return 0;
    case VFS_GONE:
      errno = ENOENT;
      return -1;
    default:
      break;
  }
  struct stat real;
  const int ret = stat(p, &real);
  if (ret == 0)
    convert_stat(&real, st);
  else
    errno = net_errno_to_linux(errno);
  return ret;
}

int fstat_fake(int fd, struct bionic_stat *st) {
  if (is_urandom_fd_fake(fd)) {
    // OpenSSL checks S_ISCHR on the random device
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0444;
    st->st_nlink = 1;
    return 0;
  }
  if (vfd_is(fd)) {
    virtual_stat(st, 0, vfd_size(fd), (uint64_t)fd);
    return 0;
  }
  if (sw_is_memfd(fd)) {
    virtual_stat(st, 0, (uint64_t)sw_size(fd), (uint64_t)fd);
    return 0;
  }
  struct stat real;
  const int ret = is_dirfd_fake(fd) ? dirfd_stat_fake(fd, &real) : fstat(fd, &real);
  if (ret == 0)
    convert_stat(&real, st);
  else
    errno = net_errno_to_linux(errno);
  return ret;
}

int lstat_fake(const char *path, struct bionic_stat *st) {
  return stat_fake(path, st);
}

// ---------------------------------------------------------------------------
// dirent conversion (bionic dirent64 layout)
// ---------------------------------------------------------------------------

struct bionic_dirent {
  uint64_t d_ino;
  int64_t d_off;
  uint16_t d_reclen;
  uint8_t d_type;
  char d_name[256];
};

// A directory listing is read whole when it is opened, and the directory
// closed again at once; readdir walks that snapshot. Horizon will not delete
// a directory while a handle to it is open, and a snapshot also cannot skip
// entries when files are deleted while it is read (libnx reads listings in
// batches by position). For a directory of the tar it is the union of the
// tar's children and whatever the SD holds there.
typedef struct {
  char *name;
  uint64_t ino;
  uint8_t type;
} SnapEntry;

typedef struct {
  uint32_t magic;
  int count, pos, cap;
  SnapEntry *entries;
} SnapDir;

#define SNAPDIR_MAGIC 0x50414e53 // "SNAP"
#define BIONIC_DT_DIR 4
#define BIONIC_DT_REG 8

static int snap_add(SnapDir *s, const char *name, uint64_t ino, uint8_t type) {
  if (s->count == s->cap) {
    const int cap = s->cap ? s->cap * 2 : 32;
    SnapEntry *n = realloc(s->entries, (size_t)cap * sizeof(*n));
    if (!n)
      return -1;
    s->entries = n;
    s->cap = cap;
  }
  SnapEntry *t = &s->entries[s->count];
  t->name = strdup(name);
  if (!t->name)
    return -1;
  t->ino = ino;
  t->type = type;
  s->count++;
  return 0;
}

static void snap_virtual_cb(const char *name, int is_dir, void *arg) {
  SnapDir *s = arg;
  for (int i = 0; i < s->count; i++)
    if (!strcmp(s->entries[i].name, name))
      return; // the SD has it too
  snap_add(s, name, 1, is_dir ? BIONIC_DT_DIR : BIONIC_DT_REG);
}

// path: already fixed (a device path); NULL with a Linux errno on failure
void *snapdir_open(const char *path) {
  const VfsKind k = vfs_classify(path, NULL);
  if (k == VFS_VFILE || k == VFS_GONE) {
    errno = k == VFS_GONE ? ENOENT : ENOTDIR;
    return NULL;
  }
  SnapDir *s = calloc(1, sizeof(*s));
  if (!s) {
    errno = 12; // ENOMEM
    return NULL;
  }
  DIR *d = opendir(path);
  if (!d && k != VFS_VDIR) {
    errno = net_errno_to_linux(errno);
    free(s);
    return NULL;
  }
  // (mkt_nx: MKT KV's mods folder hid Finder's ._ files)
  const int in_mods = 0;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      if (in_mods && (!strncmp(e->d_name, "._", 2) || !strcmp(e->d_name, ".DS_Store")))
        continue;
      if (k == VFS_VDIR) {
        // a file the game deleted from the tar but whose name is on the SD
        // shows as itself; one it deleted outright must not reappear
        char full[1024];
        snprintf(full, sizeof full, "%s/%s", path, e->d_name);
        if (vfs_classify(full, NULL) == VFS_GONE)
          continue;
      }
      if (snap_add(s, e->d_name, e->d_ino, e->d_type) < 0)
        break;
    }
    closedir(d);
  }
  if (k == VFS_VDIR)
    vfs_list_virtual(path, snap_virtual_cb, s);
  s->magic = SNAPDIR_MAGIC;
  return s;
}

int snapdir_close(void *dirp) {
  SnapDir *s = dirp;
  if (!s || s->magic != SNAPDIR_MAGIC) {
    errno = 9; // EBADF
    return -1;
  }
  for (int i = 0; i < s->count; i++)
    free(s->entries[i].name);
  free(s->entries);
  s->magic = 0;
  free(s);
  return 0;
}

void *opendir_fake(const char *path) {
  return snapdir_open(fix_path(path));
}

// (sdt_nx: Project Dream's loader rewinds its Darwin readdirs)
void rewinddir_fake(void *dirp) {
  SnapDir *s = dirp;
  if (s && s->magic == SNAPDIR_MAGIC)
    s->pos = 0;
}

void *readdir_fake(void *dirp) {
  static __thread struct bionic_dirent out; // per thread
  SnapDir *s = dirp;
  if (!s || s->magic != SNAPDIR_MAGIC || s->pos >= s->count)
    return NULL; // the end: errno untouched, as bionic leaves it
  const SnapEntry *e = &s->entries[s->pos++];
  memset(&out, 0, sizeof(out));
  out.d_ino = e->ino;
  out.d_off = s->pos;
  out.d_reclen = sizeof(out);
  out.d_type = e->type;
  snprintf(out.d_name, sizeof(out.d_name), "%s", e->name);
  return &out;
}

// ---------------------------------------------------------------------------
// mmap emulation: anonymous mappings (and read-only file windows) over malloc
// ---------------------------------------------------------------------------

#define LINUX_MAP_PRIVATE   0x02
#define LINUX_MAP_FIXED     0x10
#define LINUX_MAP_ANONYMOUS 0x20
#define LINUX_MAP_FAILED    ((void *)-1)

// Every mapping is remembered, so munmap frees only what mmap returned:
// a partial unmap (the head or tail of a mapping) is then left alone instead
// of handing free() a pointer into the middle of a block.
#define MAX_MAPS 4096
static struct { void *addr; size_t len; } s_maps[MAX_MAPS];
static Mutex s_maps_lock;
static size_t s_mapped_bytes;

static void map_note(void *p, size_t len) {
  mutexLock(&s_maps_lock);
  for (int i = 0; i < MAX_MAPS; i++) {
    if (!s_maps[i].addr) {
      s_maps[i].addr = p;
      s_maps[i].len = len;
      break;
    }
  }
  s_mapped_bytes += len;
  mutexUnlock(&s_maps_lock);
}

static void log_mem_call(const char *what, uintptr_t lr, void *addr, size_t len, int a, int b) {
  so_dl_info info;
  if (so_dladdr((void *)lr, &info) && info.dli_fbase) {
    const char *n = strrchr(info.dli_fname, '/');
    log_printf("[mem] %s %p +0x%zx (%d, 0x%x) from %s+0x%lx", what, addr, len, a, b,
               n ? n + 1 : info.dli_fname, (unsigned long)(lr - (uintptr_t)info.dli_fbase));
  } else {
    log_printf("[mem] %s %p +0x%zx (%d, 0x%x) from %p", what, addr, len, a, b, (void *)lr);
  }
}

int mprotect_fake(void *addr, size_t len, int prot) {
  if (config.log_fs)
    log_mem_call("mprotect", (uintptr_t)__builtin_return_address(0), addr, len, prot, 0);
  // committing part of a reservation (vmem.c); a decommit marks its pages free (the memory goes back to the heap only
  // when memory runs short: vmem_trim)
  if (vmem_owns(addr, len)) {
    if (prot && vmem_commit(addr, len) < 0) {
      errno = ENOMEM;
      return -1;
    }
    if (!prot)
      vmem_decommit(addr, len);
  }
  return 0;
}

long pread_fake(int fd, void *buf, size_t count, int64_t off); // bionic_extra.c
static u64 s_file_maps, s_file_map_bytes;

// file mappings so far (each read in whole when it is made)
void mmap_file_stats(u64 *maps, u64 *bytes) {
  *maps = __atomic_exchange_n(&s_file_maps, 0, __ATOMIC_RELAXED);
  *bytes = __atomic_exchange_n(&s_file_map_bytes, 0, __ATOMIC_RELAXED);
}

void *mmap_fake(void *addr, size_t len, int prot, int flags, int fd, long off) {
  if (config.log_fs)
    log_mem_call("mmap", (uintptr_t)__builtin_return_address(0), addr, len, prot, flags);
  if (len == 0) {
    errno = EINVAL;
    return LINUX_MAP_FAILED;
  }
  if ((flags & LINUX_MAP_FIXED) && addr) {
    // re-mapping inside a reservation: commit (fresh pages read as zero)
    if (vmem_owns(addr, len)) {
      if (prot && vmem_commit(addr, len) < 0) {
        errno = ENOMEM;
        return LINUX_MAP_FAILED;
      }
      vmem_zero(addr, len);
      return addr;
    }
    // re-mapping inside something we handed out: just give back the range
    // (zeroed for an anonymous one, as a fresh mapping would be). Only if it
    // lies inside one of our mappings: anything else is someone else's heap.
    int inside = 0;
    mutexLock(&s_maps_lock);
    for (int i = 0; i < MAX_MAPS && !inside; i++) {
      const uintptr_t m = (uintptr_t)s_maps[i].addr;
      inside = m && (uintptr_t)addr >= m && (uintptr_t)addr + len <= m + s_maps[i].len;
    }
    mutexUnlock(&s_maps_lock);
    if (!inside) {
      static unsigned s_outside;
      if (s_outside++ < 20)
        log_printf("[mmap] fixed mapping %p +0x%zx (prot %d) is not inside one of ours: left alone", addr, len, prot);
      return addr;
    }
    if (flags & LINUX_MAP_ANONYMOUS)
      memset(addr, 0, len);
    return addr;
  }
  // a big inaccessible reservation: address space only, committed later by
  // mprotect (Unity's memory manager reserves 512 MB at a time)
  if (prot == 0 && (flags & LINUX_MAP_ANONYMOUS) && len >= (16u << 20)) {
    void *r = vmem_reserve(len);
    if (r)
      return r;
  }
  void *p = memalign(0x1000, len);
  if (!p) {
    log_printf("[mmap] out of memory for %zu KB (%zu MB mapped)", len / 1024, s_mapped_bytes >> 20);
    errno = ENOMEM;
    return LINUX_MAP_FAILED;
  }
  if (len >= (64u << 20))
    log_printf("[mmap] %zu MB %s mapping", len >> 20, (flags & LINUX_MAP_ANONYMOUS) ? "anonymous" : "file");
  if (flags & LINUX_MAP_ANONYMOUS) {
    memset(p, 0, len);
    map_note(p, len);
    return p;
  }
  // file-backed: snapshot the window (private copy semantics)
  if (fd < 0) {
    free(p);
    errno = EBADF;
    return LINUX_MAP_FAILED;
  }
  // (positional: another thread may be reading the same descriptor; pread_fake counts it in the [io] lines)
  long got = vfd_is(fd) ? vfd_pread(fd, p, len, off) : pread_fake(fd, p, len, off);
  __atomic_add_fetch(&s_file_maps, 1, __ATOMIC_RELAXED);
  __atomic_add_fetch(&s_file_map_bytes, (u64)len, __ATOMIC_RELAXED);
  if (got < 0) {
    free(p);
    return LINUX_MAP_FAILED;
  }
  if ((size_t)got < len)
    memset((char *)p + got, 0, len - got);
  map_note(p, len);
  return p;
}

// MADV_DONTNEED on anonymous memory: Linux hands back zero pages next time,
// and allocators count on it (they "free" pages this way and expect zeroes)
int madvise_fake(void *addr, size_t len, int advice) {
  if (config.log_fs)
    log_mem_call("madvise", (uintptr_t)__builtin_return_address(0), addr, len, advice, 0);
  if (advice == 4 && addr && len && vmem_owns(addr, len)) {
    vmem_zero(addr, len);
    return 0;
  }
  if (advice == 4 && addr && len) {
    mutexLock(&s_maps_lock);
    for (int i = 0; i < MAX_MAPS; i++) {
      const uintptr_t a = (uintptr_t)s_maps[i].addr;
      if (a && (uintptr_t)addr >= a && (uintptr_t)addr + len <= a + s_maps[i].len) {
        memset(addr, 0, len);
        break;
      }
    }
    mutexUnlock(&s_maps_lock);
  }
  return 0;
}

int munmap_fake(void *addr, size_t len) {
  if (config.log_fs)
    log_mem_call("munmap", (uintptr_t)__builtin_return_address(0), addr, len, 0, 0);
  if (!addr || addr == LINUX_MAP_FAILED)
    return 0;
  if (vmem_owns(addr, 1)) {
    vmem_release(addr, len);
    return 0;
  }
  mutexLock(&s_maps_lock);
  for (int i = 0; i < MAX_MAPS; i++) {
    // only the whole mapping is freed: allocators reserve big and give back
    // the unaligned head and tail (Unity does), and the rest is still theirs
    if (s_maps[i].addr == addr && len >= s_maps[i].len) {
      s_mapped_bytes -= s_maps[i].len;
      s_maps[i].addr = NULL;
      mutexUnlock(&s_maps_lock);
      free(addr);
      return 0;
    }
  }
  mutexUnlock(&s_maps_lock);
  return 0; // part of a mapping: kept until the whole of it goes
}

// ---------------------------------------------------------------------------
// locale: ignore the locale argument and use the C locale versions
// ---------------------------------------------------------------------------

struct lconv *localeconv_fake(void) {
  return localeconv();
}

// Every locale is newlib's C locale: newlib's *_l functions dereference
// their locale argument, so it must be a real one
extern struct __locale_t __global_locale;

void *newlocale_fake(int mask, const char *locale, void *base) {
  (void)mask; (void)locale; (void)base;
  return &__global_locale;
}

void freelocale_fake(void *loc) {
  (void)loc;
}

void *uselocale_fake(void *loc) {
  (void)loc;
  return &__global_locale;
}

long double strtold_l_fake(const char *s, char **end, void *loc) {
  (void)loc;
  return strtold(s, end);
}

long long strtoll_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoll(s, end, base);
}

unsigned long long strtoull_l_fake(const char *s, char **end, int base, void *loc) {
  (void)loc;
  return strtoull(s, end, base);
}

size_t mbsnrtowcs_fake(wchar_t *dst, const char **src, size_t nms, size_t len, void *ps) {
  (void)ps;
  // ascii-ish naive conversion
  size_t i = 0;
  const char *s = *src;
  while (i < nms && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (unsigned char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

size_t wcsnrtombs_fake(char *dst, const wchar_t **src, size_t nwc, size_t len, void *ps) {
  (void)ps;
  size_t i = 0;
  const wchar_t *s = *src;
  while (i < nwc && s[i] && (!dst || i < len)) {
    if (dst) dst[i] = (char)s[i];
    i++;
  }
  if (dst && i < len) {
    dst[i] = 0;
    *src = NULL;
  }
  return i;
}

// ---------------------------------------------------------------------------
// memory
// ---------------------------------------------------------------------------

int posix_memalign_fake(void **out, size_t align, size_t size) {
  void *p = memalign(align, size);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

// ---------------------------------------------------------------------------
// filesystem odds and ends
// ---------------------------------------------------------------------------

char *realpath_fake(const char *path, char *resolved) {
  if (!resolved)
    resolved = malloc(0x1000);
  strcpy(resolved, fix_path(path));
  return resolved;
}

int strerror_r_fake(int err, char *buf, size_t len) {
  snprintf(buf, len, "%s", strerror(err));
  return 0;
}

// the GNU strerror_r (bionic's __gnu_strerror_r): returns the text
char *gnu_strerror_r_fake(int err, char *buf, size_t len) {
  if (buf && len)
    snprintf(buf, len, "%s", strerror(err));
  return buf;
}

long writev_fake(int fd, const struct iovec *iov, int iovcnt) {
  long total = 0;
  for (int i = 0; i < iovcnt; i++) {
    if (iov[i].iov_len == 0)
      continue;
    long w = write_fake(fd, iov[i].iov_base, iov[i].iov_len);
    if (w < 0)
      return total > 0 ? total : -1;
    total += w;
    if ((size_t)w < iov[i].iov_len)
      break;
  }
  return total;
}

// ---------------------------------------------------------------------------
// stdio over the fake bionic __sF (stdin/stdout/stderr): the game's libc++
// and its own logging write to &__sF[1]/&__sF[2]; these wrappers absorb
// accesses to those fake FILEs and forward the rest
// ---------------------------------------------------------------------------

uint8_t fake_sF[3][0x100]; // referenced by imports.c too

static int is_fake_file(const void *f) {
  const uint8_t *p = f;
  const uint8_t *base = (const uint8_t *)fake_sF;
  return p >= base && p < base + sizeof(fake_sF);
}

// /proc emulation: the embedded OpenSSL reads /proc/cpuinfo when getauxval
// yields nothing; serve a plausible file from memory.
// The console's three application cores (Cortex-A57). Also written as internal/proc/cpuinfo (paths.c): Android's
// cpufeatures (in libunity.so) open()s /proc/cpuinfo before it reads /sys/devices/system/cpu/possible, and counts
// one core when it cannot -- Unity then made one job worker.
const char fake_cpuinfo[] =
    "processor\t: 0\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n"
    "\n"
    "processor\t: 1\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n"
    "\n"
    "processor\t: 2\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: fp asimd evtstrm aes pmull sha1 sha2 crc32 cpuid\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd07\n"
    "CPU revision\t: 1\n"
    "\n";
static const char fake_meminfo[] =
    "MemTotal:        3276800 kB\n"
    "MemFree:         1048576 kB\n"
    "MemAvailable:    2097152 kB\n";

// /sys: Unity reads each core's capacity and top frequency every frame
// (cpu_capacity, cpufreq/cpuinfo_max_freq). Answered from memory: all cores
// alike at the Switch's 1.02 GHz; the rest of /sys is not there, said at
// once (on the SD card each miss is a round trip to the file system).
static FILE *sys_file(const char *path) {
  static const char capacity[] = "1024\n", freq[] = "1020000\n";
  const size_t n = strlen(path);
  if (cpu_list_path(path))
    return fmemopen((void *)k_cpu_list, sizeof k_cpu_list - 1, "r");
#define ENDS(s) (n >= sizeof(s) - 1 && !strcmp(path + n - (sizeof(s) - 1), s))
  if (!strncmp(path, "/sys/devices/system/cpu/cpu", 27)) {
    if (ENDS("/cpu_capacity"))
      return fmemopen((void *)capacity, sizeof capacity - 1, "r");
    if (ENDS("_max_freq") || ENDS("_min_freq") || ENDS("_cur_freq"))
      return fmemopen((void *)freq, sizeof freq - 1, "r");
  }
#undef ENDS
  errno = ENOENT;
  return NULL;
}

FILE *fopen_fake(const char *path, const char *mode) {
  if (path && !strncmp(path, "/sys/", 5))
    return sys_file(path);
  if (strcmp(path, "/proc/cpuinfo") == 0)
    return fmemopen((void *)fake_cpuinfo, sizeof(fake_cpuinfo) - 1, "r");
  if (strcmp(path, "/proc/meminfo") == 0)
    return fmemopen((void *)fake_meminfo, sizeof(fake_meminfo) - 1, "r");
  if (is_urandom_path(path))
    path = "/dev/urandom"; // no FILE-level RNG consumer known; fall through
  const char *p = fix_path(path);
  int saved = 0;
  FILE *sf = sw_fopen(p, mode, &saved); // the game's saves (Project Dream's key-value file): in memory first
  if (saved) {
    log_open("fopen(save)", p, sf ? 0 : -1);
    return sf;
  }
  const int writes = mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
  const NabeFile *nf = NULL;
  const VfsKind k = vfs_classify(p, &nf);
  if (!writes) {
    if (k == VFS_VFILE) {
      FILE *vf = vfs_fopen(nf);
      log_open("fopen(tar)", p, vf ? 0 : -1);
      return vf;
    }
    if (k == VFS_GONE) {
      errno = ENOENT;
      return NULL;
    }
  } else {
    if (!sandbox_check(p, "fopen for writing"))
      return NULL;
    // "w" truncates; "a" and "r+" keep what is there, so a tar file is copied out
    if (vfs_prepare_write(p, strchr(mode, 'w') != NULL || k == VFS_GONE) < 0)
      return NULL;
  }
  FILE *f = fopen(p, mode);
  if (!f)
    errno = net_errno_to_linux(errno);
  else if (writes)
    vfs_note_real(p);
  log_open("fopen", p, f ? 0 : -1);
  return f;
}

size_t fwrite_fake(const void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return n; // stdout/stderr sink
  return fwrite(ptr, size, n, f);
}

size_t fread_fake(void *ptr, size_t size, size_t n, FILE *f) {
  if (is_fake_file(f))
    return 0;
  const u64 t0 = armGetSystemTick();
  const size_t r = fread(ptr, size, n, f);
  io_note(size * n, (long)(r * size), armGetSystemTick() - t0);
  return r;
}

int fputc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return c;
  return fputc(c, f);
}

int fputs_fake(const char *s, FILE *f) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  return fputs(s, f);
}

int fflush_fake(FILE *f) {
  if (is_fake_file(f) || f == NULL)
    return 0;
  return fflush(f);
}

int fclose_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  sw_file_closed(f);
  vfs_file_closed(f);
  return fclose(f);
}

int ferror_fake(FILE *f) {
  if (is_fake_file(f))
    return 0;
  return ferror(f);
}

int feof_fake(FILE *f) {
  if (is_fake_file(f))
    return 1;
  return feof(f);
}

int fileno_fake(FILE *f) {
  if (is_fake_file(f))
    return ((const uint8_t *)f - &fake_sF[0][0]) / 0x100;
  const int vfd = vfs_file_fd(f);
  if (vfd >= 0)
    return vfd;
  const int sfd = sw_fileno(f);
  if (sfd >= 0)
    return sfd;
  return fileno(f);
}

long ftell_fake(FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ftell(f);
}

int fseek_fake(FILE *f, long off, int whence) {
  if (is_fake_file(f))
    return -1;
  return fseek(f, off, whence);
}

int fprintf_fake(FILE *f, const char *fmt, ...) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  va_list va;
  va_start(va, fmt);
  int ret = vfprintf(f, fmt, va);
  va_end(va);
  return ret;
}

int vfprintf_fake(FILE *f, const char *fmt, va_list va) {
  if (is_fake_file(f))
    return 0; // stdout/stderr sink
  return vfprintf(f, fmt, va);
}

int getc_fake(FILE *f) {
  if (is_fake_file(f))
    return -1; // EOF
  return getc(f);
}

int ungetc_fake(int c, FILE *f) {
  if (is_fake_file(f))
    return -1;
  return ungetc(c, f);
}

char *fgets_fake(char *s, int size, FILE *f) {
  if (is_fake_file(f))
    return NULL;
  return fgets(s, size, f);
}

void setbuf_fake(FILE *f, char *buf) {
  if (is_fake_file(f))
    return;
  setbuf(f, buf);
}

int setvbuf_fake(FILE *f, char *buf, int mode, size_t size) {
  if (is_fake_file(f))
    return 0;
  return setvbuf(f, buf, mode, size);
}

// wide-char stdio: the game's C++ runtime binds std::wcout/wcerr to the fake
// __sF slots; absorb those like the narrow functions above
wint_t getwc_fake(FILE *f) {
  if (is_fake_file(f))
    return WEOF;
  return getwc(f);
}

wint_t putwc_fake(wchar_t c, FILE *f) {
  if (is_fake_file(f))
    return (wint_t)c;
  return putwc(c, f);
}

wint_t ungetwc_fake(wint_t c, FILE *f) {
  if (is_fake_file(f))
    return WEOF;
  return ungetwc(c, f);
}

// ---------------------------------------------------------------------------
// pthread extras: rwlocks and semaphores via pointer indirection
// (bionic types are plain structs the game allocates; we stash a pointer
// to the real object in their first bytes, like the mutex fakes)
// ---------------------------------------------------------------------------

typedef struct {
  RwLock lock;
} FakeRwLock;

static FakeRwLock *get_rwlock(void **storage) {
  if (!*storage) {
    FakeRwLock *l = calloc(1, sizeof(*l));
    rwlockInit(&l->lock);
    *storage = l;
  }
  return *storage;
}

int pthread_rwlock_init_fake(void **rw, const void *attr) {
  (void)attr;
  get_rwlock(rw);
  return 0;
}

int pthread_rwlock_destroy_fake(void **rw) {
  if (rw && *rw) {
    free(*rw);
    *rw = NULL;
  }
  return 0;
}

int pthread_rwlock_rdlock_fake(void **rw) {
  rwlockReadLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_wrlock_fake(void **rw) {
  rwlockWriteLock(&get_rwlock(rw)->lock);
  return 0;
}

int pthread_rwlock_unlock_fake(void **rw) {
  FakeRwLock *l = get_rwlock(rw);
  // libnx needs to know which way it was locked
  if (rwlockIsWriteLockHeldByCurrentThread(&l->lock))
    rwlockWriteUnlock(&l->lock);
  else
    rwlockReadUnlock(&l->lock);
  return 0;
}

typedef struct {
  Semaphore sem;
} FakeSem;

int sem_init_fake(void **s, int pshared, unsigned int value) {
  (void)pshared;
  FakeSem *fs = calloc(1, sizeof(*fs));
  semaphoreInit(&fs->sem, value);
  *s = fs;
  return 0;
}

int sem_destroy_fake(void **s) {
  if (s && *s) {
    free(*s);
    *s = NULL;
  }
  return 0;
}

int sem_post_fake(void **s) {
  if (s && *s)
    semaphoreSignal(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_wait_fake(void **s) {
  if (s && *s)
    semaphoreWait(&((FakeSem *)*s)->sem);
  return 0;
}

int sem_trywait_fake(void **s) {
  if (s && *s && semaphoreTryWait(&((FakeSem *)*s)->sem))
    return 0;
  errno = EAGAIN;
  return -1;
}
