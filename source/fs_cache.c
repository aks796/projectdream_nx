/* fs_cache.c -- a block cache under every file read on the SD card (-Wl,--wrap=fsFileRead and friends)
 *
 * Horizon has no file cache: each fsFileRead is a round trip to the file system service and the SD card. Unity reads
 * the game's bundles out of sdt.apk mostly in small pieces (a level load in Ryubing: 8938 reads in 10 s, 7380 of
 * them under 16 KB; and two small reads a frame while playing), which on the console made every load long. Here a
 * read under 128 KB is served from 64 KB blocks kept in memory (one read of the SD card per block missed); a bigger
 * one goes straight to the card. newlib's read/fread, pread_fake and the file mappings all end in fsFileRead, so
 * all of them go through this.
 *
 * Only files opened for reading alone are cached (fsFsOpenFile with FsOpenMode_Read): sdt.apk, the bundles, the
 * extracted libraries. Anything the game writes (saves, settings, logs) is read from the card as before. An open
 * file is known by its service object (the fsp-srv session and object id, used again after a close). Its blocks are
 * kept under the file's path when it is an .apk (it never changes while the game runs): they outlive the close, and
 * the next open of it finds them (round 12: Unity opens sdt.apk again and again, and dropping its blocks at each
 * close made a reward screen's load read 238 MB off the card for 67 MB asked -- 5 s with the screen white). Other
 * files' blocks go at their close, as before.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "log.h"

#define BLOCK_SHIFT 16                // 64 KB blocks
#define BLOCK (1u << BLOCK_SHIFT)
#define MAX_BLOCKS 1024               // 64 MB
#define BYPASS (128u << 10)           // reads this big go straight to the card
#define NBUCKETS 2048

Result __real_fsFileRead(FsFile *f, s64 off, void *buf, u64 size, u32 option, u64 *out);
Result __real_fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *out);
void __real_fsFileClose(FsFile *f);

typedef struct Block {
  u64 file;
  s64 index;
  u32 len;                  // valid bytes (less at the end of the file)
  u8 *data;
  struct Block *hnext;      // hash chain
  struct Block *prev, *next; // LRU list, most recent first
} Block;

static Mutex s_lock;
static Block *s_hash[NBUCKETS];
static Block *s_head, *s_tail;
static unsigned s_count;
static u64 s_hits, s_misses, s_direct, s_card_reads;

static u64 file_key(const FsFile *f) {
  return ((u64)f->s.session << 32) | f->s.object_id;
}

// the read-only files open now (open addressing; key 0 = empty, 1 = removed): their service object -> the id their
// blocks are kept under (the path's hash for an .apk, else the object itself)
#define NOPEN 2048
static struct {
  u64 key, fid;
  int keep;  // blocks kept after the close
} s_open[NOPEN];

static unsigned open_slot(u64 key) {
  return (unsigned)((key * 0x9E3779B97F4A7C15ull) >> 53) % NOPEN;
}

static int open_find(u64 key) {
  for (unsigned i = open_slot(key), n = 0; n < NOPEN; i = (i + 1) % NOPEN, n++) {
    if (s_open[i].key == key)
      return (int)i;
    if (s_open[i].key == 0)
      return -1;
  }
  return -1;
}

static void add_file(u64 key, u64 fid, int keep) {
  for (unsigned i = open_slot(key), n = 0; n < NOPEN; i = (i + 1) % NOPEN, n++)
    if (s_open[i].key <= 1 || s_open[i].key == key) {
      s_open[i].key = key, s_open[i].fid = fid, s_open[i].keep = keep;
      return;
    }
}

static u64 path_id(const char *p) {
  u64 h = 0xcbf29ce484222325ull; // FNV-1a, with the top bit set (never an object key)
  for (; *p; p++)
    h = (h ^ (u8)*p) * 0x100000001b3ull;
  return h | (1ull << 63);
}

static unsigned bucket(u64 file, s64 index) {
  const u64 h = (file * 0x9E3779B97F4A7C15ull) ^ ((u64)index * 0xC2B2AE3D27D4EB4Full);
  return (unsigned)(h >> 40) % NBUCKETS;
}

static void lru_unlink(Block *b) {
  if (b->prev) b->prev->next = b->next; else s_head = b->next;
  if (b->next) b->next->prev = b->prev; else s_tail = b->prev;
  b->prev = b->next = NULL;
}

static void lru_front(Block *b) {
  b->prev = NULL;
  b->next = s_head;
  if (s_head) s_head->prev = b;
  s_head = b;
  if (!s_tail) s_tail = b;
}

static Block *find(u64 file, s64 index) {
  for (Block *b = s_hash[bucket(file, index)]; b; b = b->hnext)
    if (b->file == file && b->index == index)
      return b;
  return NULL;
}

static void unhash(Block *b) {
  Block **pp = &s_hash[bucket(b->file, b->index)];
  while (*pp && *pp != b)
    pp = &(*pp)->hnext;
  if (*pp)
    *pp = b->hnext;
}

static void drop(Block *b) {
  unhash(b);
  lru_unlink(b);
  free(b->data);
  free(b);
  s_count--;
}

// every block of a file, or those of blocks [lo, hi] (lock held)
static void drop_file(u64 file, s64 lo, s64 hi) {
  for (Block *b = s_head; b;) {
    Block *n = b->next;
    if (b->file == file && b->index >= lo && b->index <= hi)
      drop(b);
    b = n;
  }
}

// a block read from the card, kept (its data is the caller's to copy from while the lock is held)
static Block *insert(u64 file, s64 index, u8 *data, u32 len) {
  Block *b = find(file, index);
  if (b) { // another thread read it meanwhile
    free(data);
    return b;
  }
  while (s_count >= MAX_BLOCKS && s_tail)
    drop(s_tail);
  b = calloc(1, sizeof *b);
  if (!b) {
    free(data);
    return NULL;
  }
  b->file = file, b->index = index, b->data = data, b->len = len;
  const unsigned k = bucket(file, index);
  b->hnext = s_hash[k];
  s_hash[k] = b;
  lru_front(b);
  s_count++;
  return b;
}

Result __wrap_fsFileRead(FsFile *f, s64 off, void *buf, u64 size, u32 option, u64 *out) {
  if (!f || size == 0 || size >= BYPASS || off < 0) {
    __atomic_add_fetch(&s_direct, 1, __ATOMIC_RELAXED);
    return __real_fsFileRead(f, off, buf, size, option, out);
  }
  mutexLock(&s_lock);
  const int slot = open_find(file_key(f));
  const u64 file = slot >= 0 ? s_open[slot].fid : 0;
  mutexUnlock(&s_lock);
  if (slot < 0) {
    __atomic_add_fetch(&s_direct, 1, __ATOMIC_RELAXED);
    return __real_fsFileRead(f, off, buf, size, option, out);
  }
  u8 *dst = buf;
  u64 done = 0;
  while (done < size) {
    const s64 pos = off + (s64)done;
    const s64 index = pos >> BLOCK_SHIFT;
    const u32 in_block = (u32)(pos & (BLOCK - 1));
    mutexLock(&s_lock);
    Block *b = find(file, index);
    if (b) {
      lru_unlink(b);
      lru_front(b);
      s_hits++;
    }
    if (!b) {
      mutexUnlock(&s_lock);
      u8 *data = malloc(BLOCK);
      if (!data) { // no memory for a block: the rest straight from the card
        u64 got = 0;
        const Result rc = __real_fsFileRead(f, pos, dst + done, size - done, option, &got);
        if (out)
          *out = done + (R_SUCCEEDED(rc) ? got : 0);
        return R_SUCCEEDED(rc) || done ? 0 : rc;
      }
      u64 got = 0;
      const Result rc = __real_fsFileRead(f, index << BLOCK_SHIFT, data, BLOCK, option, &got);
      __atomic_add_fetch(&s_card_reads, 1, __ATOMIC_RELAXED);
      if (R_FAILED(rc)) {
        free(data);
        if (out)
          *out = done;
        return done ? 0 : rc;
      }
      mutexLock(&s_lock);
      s_misses++;
      b = insert(file, index, data, (u32)got);
      if (!b) { // (no memory for the record: as read, once)
        mutexUnlock(&s_lock);
        const u32 n = got > in_block ? (u32)got - in_block : 0;
        const u32 take = (u32)((size - done) < n ? (size - done) : n);
        memcpy(dst + done, data + in_block, take);
        free(data);
        done += take;
        if (take == 0 || got < BLOCK)
          break;
        continue;
      }
    }
    const u32 n = b->len > in_block ? b->len - in_block : 0;
    const u32 take = (u32)((size - done) < n ? (size - done) : n);
    memcpy(dst + done, b->data + in_block, take);
    const int short_block = b->len < BLOCK;
    mutexUnlock(&s_lock);
    done += take;
    if (take == 0 || short_block)
      break; // the end of the file
  }
  if (out)
    *out = done;
  return 0;
}

Result __wrap_fsFsOpenFile(FsFileSystem *fs, const char *path, u32 mode, FsFile *out) {
  const Result rc = __real_fsFsOpenFile(fs, path, mode, out);
  if (R_SUCCEEDED(rc) && out && mode == FsOpenMode_Read) {
    const size_t n = path ? strlen(path) : 0;
    const int keep = n > 4 && !strcmp(path + n - 4, ".apk");
    mutexLock(&s_lock);
    add_file(file_key(out), keep ? path_id(path) : file_key(out), keep);
    mutexUnlock(&s_lock);
  }
  return rc;
}

void __wrap_fsFileClose(FsFile *f) {
  if (f && serviceIsActive(&f->s)) {
    const u64 key = file_key(f);
    mutexLock(&s_lock);
    const int slot = open_find(key);
    if (slot >= 0) {
      if (!s_open[slot].keep)
        drop_file(s_open[slot].fid, 0, INT64_MAX);
      s_open[slot].key = 1; // (removed)
    }
    mutexUnlock(&s_lock);
  }
  __real_fsFileClose(f);
}

// memory short (vkdk could not make a block, vkdkSetLowMemoryHook): every cached block given back
void fs_cache_drop_all(void) {
  mutexLock(&s_lock);
  const unsigned n = s_count;
  while (s_tail)
    drop(s_tail);
  mutexUnlock(&s_lock);
  log_printf("[mem] the SD read cache let go (%u blocks, %u MB) for the GPU's memory", n, (n * BLOCK) >> 20);
}

// for the [io] line: reads served from memory, blocks read from the card, reads sent straight to it
void fs_cache_stats(u64 *hits, u64 *card_reads, u64 *direct, unsigned *blocks) {
  mutexLock(&s_lock);
  *hits = s_hits;
  s_hits = 0;
  s_misses = 0;
  *blocks = s_count;
  mutexUnlock(&s_lock);
  *card_reads = __atomic_exchange_n(&s_card_reads, 0, __ATOMIC_RELAXED);
  *direct = __atomic_exchange_n(&s_direct, 0, __ATOMIC_RELAXED);
}
