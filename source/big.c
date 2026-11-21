/* big.c -- big allocations (256 KB and up) apart from the small ones, from the top of the heap down
 *
 * Hardware rounds 11-12: the GPU's memory could not get 8 MB with 719 MB of the heap free -- the largest free piece
 * was 4 MB. Everything came from newlib's one malloc: Unity's 16 MB granules (vmem.c), the GPU's blocks (deko3d's
 * aligned_alloc), the loaders' code memory, the game's mappings, and thousands of small allocations, which over a
 * session settled in the holes the big ones left until no big hole was left (a level with the pirate outfit's
 * textures could not load).
 *
 * Here every malloc-family call is wrapped (-Wl,--wrap=..., deko3d's and libnx's own calls too): under BIG_MIN it is
 * newlib's as before, from the bottom of the heap up; BIG_MIN and over comes from pages of 64 KB at the top of the
 * heap, taken downwards as needed. newlib is kept out of them by moving fake_heap_end (its sbrk checks it each time)
 * to the big pages' lowest one; when 64 MB or more at that edge are free, all but 32 MB of it goes back to newlib.
 * Small allocations can no longer break up the space the big ones need, and big ones are found first-fit from the
 * top in a page map of their own (freed neighbours merge by construction).
 *
 * A pointer is a big one when it is at or above the big pages' lowest address: newlib's never are (its break stays
 * below fake_heap_end), and a big allocation's pages are never given back while it lives.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <errno.h>
#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <switch.h>

#define PG (64u << 10)
#define BIG_MIN (256u << 10)
#define MAX_PAGES 65536u                       // 4 GB of 64 KB pages at most
#define GIVE_BACK (64u << 20)                  // free pages at the low edge before some go back to newlib
#define KEEP (32u << 20)                       // of which this much stays big

void *__real_malloc(size_t n);
void __real_free(void *p);
void *__real_calloc(size_t a, size_t b);
void *__real_realloc(void *p, size_t n);
void *__real_memalign(size_t align, size_t n);
size_t __real_malloc_usable_size(void *p);

extern char *fake_heap_end;
void __malloc_lock(struct _reent *r);
void __malloc_unlock(struct _reent *r);

static Mutex s_lock;
static uintptr_t s_top;             // the heap's end: page i is [s_top - (i+1)*PG, s_top - i*PG)
static volatile uintptr_t s_low;    // the lowest big page's start (= fake_heap_end); 0 before the first
static unsigned s_npages;           // pages the big region has now
static uint8_t s_used[MAX_PAGES];   // per page: in use
static uint32_t s_run[MAX_PAGES];   // at an allocation's lowest page (its highest index): its length in pages
static size_t s_used_pages, s_peak_pages;

static int is_big(const void *p) {
  const uintptr_t low = s_low;
  return low && (uintptr_t)p >= low && (uintptr_t)p < s_top;
}

// a run of n free pages whose lowest address is a multiple of align (lock held): its highest index, or -1
static long find_run(unsigned n, size_t align) {
  unsigned run = 0;
  for (unsigned i = 0; i < s_npages; i++) {
    if (s_used[i]) {
      run = 0;
      continue;
    }
    run++;
    if (run >= n) {
      const uintptr_t addr = s_top - (uintptr_t)(i + 1) * PG; // the run [i-n+1 .. i]: lowest page i
      if ((addr & (align - 1)) == 0)
        return (long)i;
    }
  }
  return -1;
}

// the region grown downwards by at least `more` pages (lock held): 0 when newlib's heap is in the way
static int grow(unsigned more, size_t align) {
  if (!s_top) {
    s_top = (uintptr_t)fake_heap_end;
    s_low = s_top;
  }
  // (newlib's malloc lock held: its sbrk checks fake_heap_end, then moves its break -- not between the two)
  __malloc_lock(_REENT);
  const uintptr_t brk = (uintptr_t)sbrk(0);
  int ok = 0;
  for (unsigned extra = more; brk != (uintptr_t)-1;) {
    const unsigned np = s_npages + extra;
    if (np > MAX_PAGES)
      break;
    const uintptr_t low = s_top - (uintptr_t)np * PG;
    if (low < brk + (1u << 20)) // (room left for newlib's top pad)
      break;
    if ((low & (align - 1)) == 0 || align <= PG) {
      s_npages = np;
      s_low = low;
      fake_heap_end = (char *)low;
      ok = 1;
      break;
    }
    extra++;
  }
  __malloc_unlock(_REENT);
  return ok;
}

static void *big_alloc(size_t size, size_t align) {
  if (align < PG)
    align = PG; // (pages are 64 KB aligned)
  const unsigned n = (unsigned)((size + PG - 1) / PG);
  mutexLock(&s_lock);
  long i = s_top ? find_run(n, align) : -1;
  if (i < 0) {
    // free pages at the low edge count towards the run made there
    unsigned tail = 0;
    while (tail < s_npages && !s_used[s_npages - 1 - tail])
      tail++;
    if (grow(n > tail ? n - tail : 0, align) || n <= tail)
      i = find_run(n, align);
  }
  void *p = NULL;
  if (i >= 0) {
    for (long k = i - (long)n + 1; k <= i; k++)
      s_used[k] = 1;
    s_run[i] = n;
    s_used_pages += n;
    if (s_used_pages > s_peak_pages)
      s_peak_pages = s_used_pages;
    p = (void *)(s_top - (uintptr_t)(i + 1) * PG);
  }
  mutexUnlock(&s_lock);
  return p;
}

static size_t big_size(const void *p) {
  const unsigned i = (unsigned)((s_top - (uintptr_t)p) / PG) - 1;
  return (size_t)s_run[i] * PG;
}

static void big_free(void *p) {
  mutexLock(&s_lock);
  const unsigned i = (unsigned)((s_top - (uintptr_t)p) / PG) - 1;
  const unsigned n = s_run[i];
  s_run[i] = 0;
  for (unsigned k = i - n + 1; k <= i; k++)
    s_used[k] = 0;
  s_used_pages -= n;
  // a big free stretch at the low edge: most of it back to newlib
  unsigned tail = 0;
  while (tail < s_npages && !s_used[s_npages - 1 - tail])
    tail++;
  if ((size_t)tail * PG >= GIVE_BACK) {
    const unsigned give = tail - KEEP / PG;
    s_npages -= give;
    s_low = s_top - (uintptr_t)s_npages * PG;
    fake_heap_end = (char *)s_low;
  }
  mutexUnlock(&s_lock);
}

// ---- the wrapped calls

void *__wrap_malloc(size_t n) {
  if (n >= BIG_MIN) {
    void *p = big_alloc(n, PG);
    if (p)
      return p;
  }
  return __real_malloc(n);
}

void __wrap_free(void *p) {
  if (!p)
    return;
  if (is_big(p))
    big_free(p);
  else
    __real_free(p);
}

void *__wrap_calloc(size_t a, size_t b) {
  const size_t n = a * b;
  if (b && n / b != a) {
    errno = ENOMEM;
    return NULL;
  }
  if (n >= BIG_MIN) {
    void *p = big_alloc(n, PG);
    if (p) {
      memset(p, 0, n);
      return p;
    }
  }
  return __real_calloc(a, b);
}

void *__wrap_memalign(size_t align, size_t n) {
  if (n >= BIG_MIN && align <= (16u << 20)) {
    void *p = big_alloc(n, align);
    if (p)
      return p;
  }
  return __real_memalign(align, n);
}

void *__wrap_aligned_alloc(size_t align, size_t n) {
  return __wrap_memalign(align, n);
}

int __wrap_posix_memalign(void **out, size_t align, size_t n) {
  void *p = __wrap_memalign(align < sizeof(void *) ? sizeof(void *) : align, n);
  if (!p)
    return ENOMEM;
  *out = p;
  return 0;
}

size_t __wrap_malloc_usable_size(void *p) {
  if (!p)
    return 0;
  return is_big(p) ? big_size(p) : __real_malloc_usable_size(p);
}

void *__wrap_realloc(void *p, size_t n) {
  if (!p)
    return __wrap_malloc(n);
  if (n == 0) {
    __wrap_free(p);
    return NULL;
  }
  const int was_big = is_big(p);
  if (!was_big && n < BIG_MIN)
    return __real_realloc(p, n);
  const size_t old = was_big ? big_size(p) : __real_malloc_usable_size(p);
  if (was_big && n <= old && n >= BIG_MIN)
    return p; // (fits, and still big)
  void *q = __wrap_malloc(n);
  if (!q)
    return NULL;
  memcpy(q, p, old < n ? old : n);
  __wrap_free(p);
  return q;
}

// for main.c's [mem] line: the big pages' region, in use, the most ever in use, the largest free run, the room
// between newlib's break and the region (the region can grow into it), the heap's end (bytes / address)
void big_stats(size_t *region, size_t *used, size_t *peak, size_t *largest_free, size_t *room, uintptr_t *top) {
  __malloc_lock(_REENT);
  const uintptr_t brk = (uintptr_t)sbrk(0);
  __malloc_unlock(_REENT);
  mutexLock(&s_lock);
  const uintptr_t low = s_top ? s_low : (uintptr_t)fake_heap_end;
  *room = brk != (uintptr_t)-1 && low > brk + (1u << 20) ? low - brk - (1u << 20) : 0;
  *top = s_top ? s_top : (uintptr_t)fake_heap_end;
  *region = (size_t)s_npages * PG;
  *used = s_used_pages * PG;
  *peak = s_peak_pages * PG;
  unsigned run = 0, best = 0;
  for (unsigned i = 0; i < s_npages; i++) {
    run = s_used[i] ? 0 : run + 1;
    if (run > best)
      best = run;
  }
  *largest_free = (size_t)best * PG;
  mutexUnlock(&s_lock);
}
