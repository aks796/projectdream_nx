/* vmem.c -- reserved address space that is committed piece by piece
 *
 * Unity's memory manager (2022.3) reserves big ranges of address space with
 * mmap(PROT_NONE) -- 512 MB at a time, trimmed to a 256 MB-aligned window --
 * and commits what it uses with mprotect(PROT_READ|PROT_WRITE). Handing it
 * committed heap for every reservation runs the console out of memory within
 * a second of start-up.
 *
 * So a reservation is address space only: a libnx virtmem reservation in the
 * code (ASLR) region, which is hundreds of GB -- the stack region, where
 * svcMapMemory maps, is under 2 GB and Unity reserves more than that.
 * Committing maps heap pages there the way the linker maps module data:
 * svcMapProcessCodeMemory, then read-write, in 16 MB granules (few memory
 * blocks for the kernel to track: Unity commits a megabyte at a time). Releasing the ends of
 * a reservation (Unity's trim) gives that address space back; releasing a
 * committed granule unmaps and frees it. Decommitting (PROT_NONE again) keeps
 * the memory: Unity commits it again soon after, and the peak is what counts -- until memory runs short (round
 * 11: the GPU's memory, from the same heap, could not be had late in a session): each granule counts its committed
 * 4 KB pages (Unity decommits with madvise(DONTNEED) then mprotect(PROT_NONE): the contents are gone), and
 * vmem_trim gives back the granules with none (vmem_commit, the GPU's allocations and main.c call it when the heap
 * runs low).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "log.h"
#include "so_util.h"
#include "vmem.h"

#define GRAN (16u << 20)
#define MAX_RES 64

#define PAGES (GRAN >> 12)      // 4 KB pages a granule
typedef struct {
  uint64_t bits[PAGES / 64];     // committed pages
  unsigned n;                    // how many
} Pages;

typedef struct {
  uintptr_t lo, hi;              // reserved range (GRAN aligned)
  VirtmemReservation *rv;
  void **backing;                // per granule: the heap block mapped there
  Pages *pages;                  // per granule: its committed pages
  int ngran;
} Res;

static Res s_res[MAX_RES];
static Mutex s_lock;
static size_t s_committed;
static int s_ok = 1;

void vmem_init(void) {
  u64 st = 0, ss = 0, as = 0, al = 0;
  svcGetInfo(&st, InfoType_StackRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&ss, InfoType_StackRegionSize, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&as, InfoType_AliasRegionAddress, CUR_PROCESS_HANDLE, 0);
  svcGetInfo(&al, InfoType_AliasRegionSize, CUR_PROCESS_HANDLE, 0);
  log_printf("[vmem] stack region 0x%llx +%llu MB, alias region 0x%llx +%llu MB",
             (unsigned long long)st, (unsigned long long)(ss >> 20), (unsigned long long)as,
             (unsigned long long)(al >> 20));
}

static Res *find(uintptr_t a) {
  for (int i = 0; i < MAX_RES; i++)
    if (s_res[i].hi && a >= s_res[i].lo && a < s_res[i].hi)
      return &s_res[i];
  return NULL;
}

void *vmem_reserve(size_t len) {
  if (!s_ok || len < GRAN)
    return NULL;
  const size_t size = (len + GRAN - 1) & ~(size_t)(GRAN - 1);
  mutexLock(&s_lock);
  int slot = -1;
  for (int i = 0; i < MAX_RES && slot < 0; i++)
    if (!s_res[i].hi)
      slot = i;
  if (slot < 0) {
    mutexUnlock(&s_lock);
    return NULL;
  }
  virtmemLock();
  // room for aligning the start to a granule
  void *raw = virtmemFindCodeMemory(size + GRAN, 0x10000);
  uintptr_t lo = raw ? ((uintptr_t)raw + GRAN - 1) & ~(uintptr_t)(GRAN - 1) : 0;
  VirtmemReservation *rv = lo ? virtmemAddReservation((void *)lo, size) : NULL;
  virtmemUnlock();
  if (!rv) {
    mutexUnlock(&s_lock);
    log_printf("[vmem] no address space for %zu MB", size >> 20);
    return NULL;
  }
  Res *r = &s_res[slot];
  r->lo = lo;
  r->hi = lo + size;
  r->rv = rv;
  r->ngran = (int)(size / GRAN);
  r->backing = calloc((size_t)r->ngran, sizeof(void *));
  r->pages = calloc((size_t)r->ngran, sizeof(Pages));
  mutexUnlock(&s_lock);
  log_printf("[vmem] reserved %zu MB at %p", size >> 20, (void *)lo);
  return (void *)lo;
}

int vmem_owns(const void *addr, size_t len) {
  mutexLock(&s_lock);
  Res *r = find((uintptr_t)addr);
  const int ok = r && (uintptr_t)addr + len <= r->hi;
  mutexUnlock(&s_lock);
  return ok;
}

// the pages of [a, e) in r marked committed (on) or not; a page partly inside counts as committed for "on" only
static void mark(Res *r, uintptr_t a, uintptr_t e, int on) {
  uintptr_t p0 = on ? a & ~(uintptr_t)0xfff : (a + 0xfff) & ~(uintptr_t)0xfff;
  const uintptr_t p1 = on ? (e + 0xfff) & ~(uintptr_t)0xfff : e & ~(uintptr_t)0xfff;
  for (uintptr_t p = p0; p < p1 && p < r->hi; p += 0x1000) {
    const uintptr_t off = p - r->lo;
    Pages *pg = &r->pages[off / GRAN];
    const unsigned i = (unsigned)((off % GRAN) >> 12);
    const uint64_t bit = 1ull << (i & 63);
    if (on && !(pg->bits[i >> 6] & bit)) {
      pg->bits[i >> 6] |= bit;
      pg->n++;
    } else if (!on && (pg->bits[i >> 6] & bit)) {
      pg->bits[i >> 6] &= ~bit;
      pg->n--;
    }
  }
}

static size_t trim_locked(void);

int vmem_commit(void *addr, size_t len) {
  if (!len)
    return 0;
  mutexLock(&s_lock);
  Res *r = find((uintptr_t)addr);
  if (!r) {
    mutexUnlock(&s_lock);
    return -1;
  }
  const uintptr_t a = (uintptr_t)addr, e = a + len;
  int rc = 0;
  for (uintptr_t g = (a - r->lo) / GRAN; g < (e - r->lo + GRAN - 1) / GRAN && (int)g < r->ngran; g++) {
    if (r->backing[g])
      continue;
    void *mem = memalign(0x1000, GRAN);
    if (!mem && trim_locked()) // (the granules nothing uses any more, given back first)
      mem = memalign(0x1000, GRAN);
    if (!mem) {
      log_printf("[vmem] out of memory committing %p (%zu MB committed)", addr, s_committed >> 20);
      rc = -1;
      break;
    }
    memset(mem, 0, GRAN);
    const uintptr_t dst = r->lo + g * GRAN;
    armDCacheFlush(mem, GRAN);
    Result res = svcMapProcessCodeMemory(so_own_process(), dst, (u64)mem, GRAN);
    if (R_SUCCEEDED(res))
      res = svcSetProcessMemoryPermission(so_own_process(), dst, GRAN, Perm_Rw);
    if (R_FAILED(res)) {
      log_printf("[vmem] mapping %p failed: 0x%x", (void *)dst, res);
      free(mem);
      rc = -1;
      break;
    }
    r->backing[g] = mem;
    s_committed += GRAN;
  }
  if (rc == 0)
    mark(r, a, e, 1);
  mutexUnlock(&s_lock);
  return rc;
}

// mprotect(PROT_NONE) on committed pages: Unity's decommit (after madvise(DONTNEED): their contents are gone)
void vmem_decommit(void *addr, size_t len) {
  mutexLock(&s_lock);
  Res *r = find((uintptr_t)addr);
  if (r)
    mark(r, (uintptr_t)addr, (uintptr_t)addr + len, 0);
  mutexUnlock(&s_lock);
}

void vmem_zero(void *addr, size_t len) {
  mutexLock(&s_lock);
  Res *r = find((uintptr_t)addr);
  if (r) {
    const uintptr_t a = (uintptr_t)addr;
    uintptr_t e = a + len;
    if (e > r->hi)
      e = r->hi;
    for (uintptr_t p = a; p < e;) {
      const uintptr_t g = (p - r->lo) / GRAN;
      const uintptr_t gend = r->lo + (g + 1) * GRAN;
      const uintptr_t n = (e < gend ? e : gend) - p;
      if (r->backing[g])
        memset((void *)p, 0, n);
      p += n;
    }
  }
  mutexUnlock(&s_lock);
}

static void unmap_granule(Res *r, int g) {
  if (!r->backing[g])
    return;
  const uintptr_t dst = r->lo + (uintptr_t)g * GRAN;
  svcUnmapProcessCodeMemory(so_own_process(), dst, (u64)r->backing[g], GRAN);
  free(r->backing[g]);
  r->backing[g] = NULL;
  memset(&r->pages[g], 0, sizeof(Pages));
  s_committed -= GRAN;
}

// the granules with no page committed given back to the heap (lock held): how many bytes
static size_t trim_locked(void) {
  size_t freed = 0;
  for (int i = 0; i < MAX_RES; i++) {
    Res *r = &s_res[i];
    if (!r->hi)
      continue;
    for (int g = 0; g < r->ngran; g++)
      if (r->backing[g] && r->pages[g].n == 0) {
        unmap_granule(r, g);
        freed += GRAN;
      }
  }
  return freed;
}

size_t vmem_trim(void) {
  mutexLock(&s_lock);
  const size_t freed = trim_locked();
  mutexUnlock(&s_lock);
  return freed;
}

void vmem_release(void *addr, size_t len) {
  mutexLock(&s_lock);
  Res *r = find((uintptr_t)addr);
  if (!r) {
    mutexUnlock(&s_lock);
    return;
  }
  uintptr_t a = (uintptr_t)addr, e = a + len;
  if (e > r->hi)
    e = r->hi;
  // whole granules inside the range are given back
  const uintptr_t ga = (a - r->lo + GRAN - 1) / GRAN, ge = (e - r->lo) / GRAN;
  for (uintptr_t g = ga; g < ge && (int)g < r->ngran; g++)
    unmap_granule(r, (int)g);
  // a released end shrinks the reservation (the address space goes back)
  uintptr_t nlo = r->lo, nhi = r->hi;
  if (a <= r->lo)
    nlo = r->lo + ge * GRAN > r->lo ? r->lo + ge * GRAN : r->lo;
  if (e >= r->hi)
    nhi = r->lo + ga * GRAN;
  if (nlo >= nhi) {
    virtmemLock();
    virtmemRemoveReservation(r->rv);
    virtmemUnlock();
    free(r->backing);
    free(r->pages);
    memset(r, 0, sizeof(*r));
  } else if (nlo != r->lo || nhi != r->hi) {
    const int off = (int)((nlo - r->lo) / GRAN), n = (int)((nhi - nlo) / GRAN);
    void **nb = calloc((size_t)n, sizeof(void *));
    memcpy(nb, r->backing + off, (size_t)n * sizeof(void *));
    free(r->backing);
    Pages *np = calloc((size_t)n, sizeof(Pages));
    memcpy(np, r->pages + off, (size_t)n * sizeof(Pages));
    free(r->pages);
    r->pages = np;
    virtmemLock();
    virtmemRemoveReservation(r->rv);
    r->rv = virtmemAddReservation((void *)nlo, nhi - nlo);
    virtmemUnlock();
    r->backing = nb;
    r->lo = nlo;
    r->hi = nhi;
    r->ngran = n;
  }
  mutexUnlock(&s_lock);
}

void vmem_stats(size_t *reserved, size_t *committed) {
  size_t res = 0;
  mutexLock(&s_lock);
  for (int i = 0; i < MAX_RES; i++)
    if (s_res[i].hi)
      res += s_res[i].hi - s_res[i].lo;
  if (committed)
    *committed = s_committed;
  mutexUnlock(&s_lock);
  if (reserved)
    *reserved = res;
}
