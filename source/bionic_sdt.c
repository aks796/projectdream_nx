/* bionic_sdt.c -- bionic functions Project Dream's loader imports that
 * mkt_nx's games never did
 *
 *   __libc_current_sigrtmin  its GC signal's number (no signals here: the
 *                            stops are pd_shim.c's)
 *   rewinddir                over libc_shim.c's directory snapshots
 *   sysinfo                  the console's memory, for Darwin's hw.memsize
 *   nftw                     a depth-first walk (it clears an old extraction
 *                            folder with it)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "libc_shim.h"
#include "log.h"
#include "so_util.h"

static int libc_current_sigrtmin(void) { return 34; }

// bionic's LP64 struct sysinfo (112 bytes: its _f[] padding is empty on LP64;
// writing more than that overwrote the caller's frame, d_sysctl's saved lr)
struct bionic_sysinfo {
  long uptime;
  unsigned long loads[3];
  unsigned long totalram, freeram, sharedram, bufferram, totalswap, freeswap;
  unsigned short procs, pad;
  unsigned long totalhigh, freehigh;
  unsigned int mem_unit;
};
_Static_assert(sizeof(struct bionic_sysinfo) == 112, "bionic struct sysinfo");

static int sysinfo_fake(struct bionic_sysinfo *info) {
  if (!info) {
    errno = EFAULT;
    return -1;
  }
  memset(info, 0, sizeof *info);
  u64 total = 0, used = 0;
  svcGetInfo(&total, InfoType_TotalMemorySize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&used, InfoType_UsedMemorySize, CUR_PROCESS_HANDLE, 0);
  info->uptime = (long)(armTicksToNs(armGetSystemTick()) / 1000000000ull);
  info->totalram = (unsigned long)total;
  info->freeram = total > used ? (unsigned long)(total - used) : 0;
  info->procs = 1;
  info->mem_unit = 1;
  return 0;
}

// nftw(dir, fn, fds, flags): FTW_PHYS 1, FTW_MOUNT 2, FTW_CHDIR 4, FTW_DEPTH 8;
// fn(path, stat, type, ftw): FTW_F 0, FTW_D 1, FTW_DNR 2, FTW_NS 3, FTW_DP 5
typedef int (*NftwFn)(const char *path, const void *st, int type, void *ftw);
struct bionic_ftw {
  int base, level;
};

static int walk(const char *path, NftwFn fn, int flags, int level, int base) {
  uint8_t st[128] = { 0 }; // (a bionic struct stat: the callers here read none of it)
  void *d = opendir_fake(path);
  struct bionic_ftw ftw = { base, level };
  if (!d)
    return fn(path, st, 0, &ftw); // a file
  int r = 0;
  if (!(flags & 8))
    r = fn(path, st, 1, &ftw);
  struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } *e;
  while (!r && (e = readdir_fake(d)) != NULL) {
    if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
      continue;
    char *child = malloc(strlen(path) + strlen(e->name) + 2);
    if (!child)
      break;
    sprintf(child, "%s/%s", path, e->name);
    r = walk(child, fn, flags, level + 1, (int)strlen(path) + 1);
    free(child);
  }
  closedir_fake(d);
  if (!r && (flags & 8))
    r = fn(path, st, 5, &ftw);
  return r;
}

static int nftw_fake(const char *dir, NftwFn fn, int fds, int flags) {
  (void)fds;
  if (!dir || !fn) {
    errno = EINVAL;
    return -1;
  }
  const char *slash = strrchr(dir, '/');
  return walk(dir, fn, flags, 0, slash ? (int)(slash - dir) + 1 : 0);
}

DynLibFunction sdt_functions[] = {
  { "__libc_current_sigrtmin", (uintptr_t)&libc_current_sigrtmin },
  { "__libc_current_sigrtmax", (uintptr_t)&libc_current_sigrtmin },
  { "rewinddir", (uintptr_t)&rewinddir_fake },
  { "sysinfo", (uintptr_t)&sysinfo_fake },
  { "nftw", (uintptr_t)&nftw_fake },
  { "nftw64", (uintptr_t)&nftw_fake },
};
size_t sdt_numfunctions = sizeof(sdt_functions) / sizeof(*sdt_functions);
