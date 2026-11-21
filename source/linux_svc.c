/* linux_svc.c -- Linux system calls the game's code makes itself
 *
 * Android code normally reaches the kernel through bionic, which the import
 * tables replace. libil2cpp.so's protector does not: its flattened code
 * makes 155 calls of its own (svc #0, the call number in x8, arguments in
 * x0-x5, -errno back in x0) for files, the clock and TCP sockets. On Horizon
 * svc #0 is a bad SVC (Ryujinx: "SVC 0x0000 is not implemented").
 *
 * The linker (so_util.c) branches each one to a stub and linux_svc_entry.s,
 * which keeps every register, and linux_svc() answers through the same
 * shims the libc imports use: a file or socket is the same thing to the game
 * whichever way it asks. Each call site is logged on its 1st, 10th, 100th...
 * call.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

#include "bionic_extra.h"
#include "libc_shim.h"
#include "log.h"
#include "net_shim.h"
#include "so_util.h"

// what linux_svc_entry.s saved: the registers at the svc
typedef struct {
  uint64_t x[31]; // x16 and x30 as the stub left them (x30: the return into it)
  uint64_t nzcv, fpsr, fpcr;
} SvcFrame;

// Linux arm64 call numbers
#define NR_FACCESSAT 48
#define NR_OPENAT 56
#define NR_CLOSE 57
#define NR_READ 63
#define NR_WRITE 64
#define NR_NEWFSTATAT 79
#define NR_GETTIMEOFDAY 169
#define NR_SOCKET 198
#define NR_BIND 200
#define NR_CONNECT 203
#define NR_RECVFROM 207
#define NR_SETSOCKOPT 208

#define L_AT_EMPTY_PATH 0x1000

static const char *call_name(long nr) {
  switch (nr) {
    case NR_FACCESSAT: return "faccessat";
    case NR_OPENAT: return "openat";
    case NR_CLOSE: return "close";
    case NR_READ: return "read";
    case NR_WRITE: return "write";
    case NR_NEWFSTATAT: return "newfstatat";
    case NR_GETTIMEOFDAY: return "gettimeofday";
    case NR_SOCKET: return "socket";
    case NR_BIND: return "bind";
    case NR_CONNECT: return "connect";
    case NR_RECVFROM: return "recvfrom";
    case NR_SETSOCKOPT: return "setsockopt";
    default: return NULL;
  }
}

// the call through the libc shims: their result, -1 with errno set on failure
static long call(long nr, const uint64_t *a) {
  switch (nr) {
    case NR_FACCESSAT:
      return faccessat_fake((int)a[0], (const char *)a[1], (int)a[2]);
    case NR_OPENAT:
      return openat_fake((int)a[0], (const char *)a[1], (int)a[2], (int)a[3]);
    case NR_CLOSE:
      return close_fake((int)a[0]);
    case NR_READ:
      return read_fake((int)a[0], (void *)a[1], (size_t)a[2]);
    case NR_WRITE:
      return write_fake((int)a[0], (const void *)a[1], (size_t)a[2]);
    case NR_NEWFSTATAT: {
      // fstatat(fd, "", st, AT_EMPTY_PATH) is fstat(fd)
      const char *name = (const char *)a[1];
      if (((int)a[3] & L_AT_EMPTY_PATH) && name && !name[0])
        return fstat_fake((int)a[0], (struct bionic_stat *)a[2]);
      return fstatat_fake((int)a[0], name, (struct bionic_stat *)a[2], (int)a[3]);
    }
    case NR_GETTIMEOFDAY:
      if (a[1])
        memset((void *)a[1], 0, 8); // struct timezone: UTC
      return a[0] ? gettimeofday((struct timeval *)a[0], NULL) : 0;
    case NR_SOCKET:
      return socket_fake((int)a[0], (int)a[1], (int)a[2]);
    case NR_BIND:
      return bind_fake((int)a[0], (const void *)a[1], (unsigned)a[2]);
    case NR_CONNECT:
      return connect_fake((int)a[0], (const void *)a[1], (unsigned)a[2]);
    case NR_RECVFROM:
      return recvfrom_fake((int)a[0], (void *)a[1], (size_t)a[2], (int)a[3], (void *)a[4],
                           (unsigned *)a[5]);
    case NR_SETSOCKOPT:
      return setsockopt_fake((int)a[0], (int)a[1], (int)a[2], (const void *)a[3],
                             (unsigned)a[4]);
    default:
      // futex, gettid, getrandom, ... or ENOSYS, logged once per number
      return syscall_fake(nr, a[0], a[1], a[2], a[3], a[4], a[5]);
  }
}

// a site's 1st, 10th, 100th, ... call, with what it was asked and what it
// got: one line for a call made once, a few for a loop
static void log_call(const SvcFrame *f, long nr, long r) {
  uintptr_t site;
  int index;
  // x30: the return from the stub's blr, its third instruction
  so_module *m = so_svc_site(f->x[30] - 12, &site, &index);
  if (!m)
    return;
  const uint32_t n = __atomic_add_fetch(&m->svc_hits[index], 1, __ATOMIC_RELAXED);
  uint32_t p = 1;
  while (p < n && p <= UINT32_MAX / 10)
    p *= 10;
  if (p != n)
    return;
  const char *path = NULL;
  if (nr == NR_FACCESSAT || nr == NR_OPENAT || nr == NR_NEWFSTATAT)
    path = f->x[1] ? (const char *)f->x[1] : "(null)";
  char num[24];
  const char *name = call_name(nr);
  if (!name) {
    snprintf(num, sizeof num, "syscall %ld", nr);
    name = num;
  }
  char times[24] = "";
  if (n > 1)
    snprintf(times, sizeof times, " (call %u)", n);
  log_printf("[svc] %s+0x%lx: %s(0x%lx, 0x%lx, 0x%lx)%s%s = %ld%s", m->soname,
             (unsigned long)(site - m->base), name, (unsigned long)f->x[0],
             (unsigned long)f->x[1], (unsigned long)f->x[2], path ? " " : "", path ? path : "", r,
             times);
}

// from linux_svc_entry.s: the value for x0
long linux_svc(SvcFrame *f) {
  const long nr = (long)f->x[8];
  const int saved = errno; // a system call leaves the C library's errno alone
  errno = 0;
  long r = call(nr, f->x);
  if (r < 0)
    r = -(long)(errno ? errno : 5); // EIO when a shim failed without saying why
  errno = saved;
  log_call(f, nr, r);
  return r;
}
