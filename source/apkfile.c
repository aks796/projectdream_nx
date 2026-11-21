/* apkfile.c -- random access to the player's APK (see apkfile.h)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <zlib.h>
#include <switch.h>

#include "apkfile.h"
#include "log.h"

typedef struct {
  ApkEntry e;
  uint64_t lho;      // local header offset
  int resolved;      // data_off known
  int part;          // which part of the APK it is in (0 the APK itself)
} Entry;

// The APK's parts: the APK itself, and a split APK with its native
// libraries if it came as two (an app bundle's base.apk and
// split_config.arm64_v8a.apk: apkfile_add). Their bytes are one range here,
// the split's after the base's, so an entry's offset says which file it is in.
#define MAX_PARTS 3 // (the third: an asset cache from another APK, apk.c)
static struct {
  FsFile file;
  uint64_t base, size;
} s_part[MAX_PARTS];
static int s_nparts;
static int s_open;
static uint64_t s_size; // all parts
static char s_path[512];
static char s_err[256];
static Entry *s_entries;
static int s_count;
static char *s_names_parts[MAX_PARTS]; // each part's entry names, NUL-separated
static int *s_hash;        // open addressing, -1 empty
static unsigned s_hash_cap;
static Mutex s_lock;

static void set_err(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void set_err(const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  vsnprintf(s_err, sizeof s_err, fmt, va);
  va_end(va);
  log_printf("[apk] %s", s_err);
}

const char *apkfile_error(void) { return s_err; }
const char *apkfile_path(void) { return s_path; }
uint64_t apkfile_size(void) { return s_size; }
int apkfile_count(void) { return s_count; }

// reads by size (up to 4 KB, 64 KB, 1 MB, more) and their time, for the log
static volatile unsigned s_nreads[4];
static volatile uint64_t s_read_ticks;

void apkfile_stats(unsigned counts[4], double *ms) {
  for (int i = 0; i < 4; i++) {
    counts[i] = s_nreads[i];
    s_nreads[i] = 0;
  }
  *ms = (double)armTicksToNs(s_read_ticks) / 1e6;
  s_read_ticks = 0;
}

// the APK's bytes, a request to the SD card each
static long read_direct(uint64_t off, void *buf, size_t len);

// Small reads through a cache of 256 KB blocks of the APK. The game reads its
// asset files a few KB at a time (a race's load: some 5,500 reads, 4,000 of
// them 4 KB or less), and each was a request to the card of its own; a block
// read once serves the reads near it. The APK never changes while it runs.
#define BLOCK (256u << 10)
#define NBLOCKS 64
#define SMALL (64u << 10)
static struct {
  uint64_t at;   // the block's offset (BLOCK-aligned), ~0: empty
  uint32_t len;  // its bytes (less at the APK's end)
  uint32_t used; // last use, for the least recently used
  uint8_t *data;
} s_blocks[NBLOCKS];
static uint32_t s_use;
static Mutex s_cache_lock;
static volatile unsigned s_hits, s_misses;

static long read_cached(uint64_t off, void *buf, size_t len) {
  size_t done = 0;
  mutexLock(&s_cache_lock);
  while (done < len) {
    const uint64_t at = (off + done) / BLOCK * BLOCK;
    int k = -1, victim = 0;
    for (int i = 0; i < NBLOCKS; i++) {
      if (s_blocks[i].data && s_blocks[i].at == at) {
        k = i;
        break;
      }
      if (!s_blocks[i].data || s_blocks[i].used < s_blocks[victim].used)
        victim = i;
    }
    if (k < 0) {
      k = victim;
      if (!s_blocks[k].data && !(s_blocks[k].data = malloc(BLOCK)))
        break;
      const long got = read_direct(at, s_blocks[k].data, s_size - at < BLOCK ? (size_t)(s_size - at) : BLOCK);
      if (got <= 0) {
        s_blocks[k].at = ~(uint64_t)0;
        break;
      }
      s_blocks[k].at = at;
      s_blocks[k].len = (uint32_t)got;
      s_misses++;
    } else {
      s_hits++;
    }
    s_blocks[k].used = ++s_use;
    const uint64_t in = off + done - at;
    if (in >= s_blocks[k].len)
      break;
    size_t n = s_blocks[k].len - (size_t)in;
    if (n > len - done)
      n = len - done;
    memcpy((uint8_t *)buf + done, s_blocks[k].data + in, n);
    done += n;
  }
  mutexUnlock(&s_cache_lock);
  return done || !len ? (long)done : -1;
}

void apkfile_cache_stats(unsigned *hits, unsigned *misses) {
  *hits = s_hits, *misses = s_misses;
  s_hits = s_misses = 0;
}

long apkfile_read(uint64_t off, void *buf, size_t len) {
  if (!s_open)
    return -1;
  if (off >= s_size)
    return 0;
  if (len > s_size - off)
    len = (size_t)(s_size - off);
  s_nreads[len <= 4096 ? 0 : len <= 65536 ? 1 : len <= (1u << 20) ? 2 : 3]++;
  return len < SMALL ? read_cached(off, buf, len) : read_direct(off, buf, len);
}

static long read_direct(uint64_t off, void *buf, size_t len) {
  const u64 t0 = armGetSystemTick();
  if (off >= s_size)
    return 0;
  if (len > s_size - off)
    len = (size_t)(s_size - off);
  u64 got = 0;
  size_t total = 0;
  // FS reads are capped per request on some firmwares; keep them moderate
  while (total < len) {
    int k = s_nparts - 1; // (the part the next byte is in; a read may cross into the next)
    while (k > 0 && off + total < s_part[k].base)
      k--;
    const uint64_t at = off + total - s_part[k].base, left = s_part[k].size - at;
    size_t want = len - total > (32u << 20) ? (32u << 20) : len - total;
    if (want > left)
      want = (size_t)left;
    Result rc = fsFileRead(&s_part[k].file, (s64)at, (u8 *)buf + total, want, FsReadOption_None, &got);
    if (R_FAILED(rc)) {
      log_printf("[apk] read of %zu bytes at 0x%llx failed: 0x%x", want,
                 (unsigned long long)(off + total), rc);
      return total ? (long)total : -1;
    }
    if (!got)
      break;
    total += got;
  }
  s_read_ticks += armGetSystemTick() - t0;
  return (long)total;
}

static inline uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) {
  return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static inline uint64_t rd64(const uint8_t *p) { return rd32(p) | (uint64_t)rd32(p + 4) << 32; }

static uint32_t name_hash(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++)
    h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static void hash_insert(int idx) {
  unsigned i = name_hash(s_entries[idx].e.name) & (s_hash_cap - 1);
  while (s_hash[i] >= 0)
    i = (i + 1) & (s_hash_cap - 1);
  s_hash[i] = idx;
}

// the entry of this name already read, if any (no local header read)
static const Entry *apkfile_find_quiet(const char *name) {
  if (!s_hash || !name)
    return NULL;
  unsigned i = name_hash(name) & (s_hash_cap - 1);
  while (s_hash[i] >= 0) {
    const Entry *e = &s_entries[s_hash[i]];
    if (!strcmp(e->e.name, name))
      return e;
    i = (i + 1) & (s_hash_cap - 1);
  }
  return NULL;
}

void apkfile_close(void) {
  // the block cache holds the closed file's bytes: none of them stay
  mutexLock(&s_cache_lock);
  for (int i = 0; i < NBLOCKS; i++)
    s_blocks[i].at = ~(uint64_t)0;
  mutexUnlock(&s_cache_lock);
  for (int i = 0; i < s_nparts; i++)
    fsFileClose(&s_part[i].file);
  s_nparts = 0;
  s_size = 0;
  s_open = 0;
  free(s_entries);
  for (int i = 0; i < MAX_PARTS; i++) {
    free(s_names_parts[i]);
    s_names_parts[i] = NULL;
  }
  free(s_hash);
  s_entries = NULL;
  s_hash = NULL;
  s_count = 0;
}

// open a part of the APK (its bytes after those already open)
static int open_part(const char *path) {
  if (s_nparts >= MAX_PARTS) {
    set_err("too many APK parts");
    return -1;
  }
  const char *fspath = path;
  const char *colon = strchr(path, ':');
  if (colon)
    fspath = colon + 1;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (!fs) {
    set_err("no sdmc filesystem");
    return -1;
  }
  char p[FS_MAX_PATH];
  snprintf(p, sizeof p, "%s", fspath);
  const int k = s_nparts;
  Result rc = fsFsOpenFile(fs, p, FsOpenMode_Read, &s_part[k].file);
  if (R_FAILED(rc)) {
    set_err("cannot open %s (0x%x)", path, rc);
    return -1;
  }
  s64 sz = 0;
  fsFileGetSize(&s_part[k].file, &sz);
  s_part[k].base = s_size;
  s_part[k].size = (uint64_t)sz;
  s_nparts++;
  s_size += (uint64_t)sz;
  s_open = 1;
  // a block cached at the old end holds only the bytes up to it: this part's
  // first ones read through it came back empty (Nabe.zip's first entries:
  // "bad local header", left out of its index). The cache starts over.
  mutexLock(&s_cache_lock);
  for (int i = 0; i < NBLOCKS; i++)
    s_blocks[i].at = ~(uint64_t)0;
  mutexUnlock(&s_cache_lock);
  return k;
}

// a part's zip directory into the entries (a name already there stays the
// earlier part's: the base APK's manifest, not the split's)
static int read_directory(int k, const char *path) {
  const uint64_t base = s_part[k].base, size = s_part[k].size;
  // end of central directory: in the last 64 KB + 22 bytes
  const size_t tail_len = size < 65557 ? (size_t)size : 65557;
  uint8_t *tail = malloc(tail_len);
  if (!tail || apkfile_read(base + size - tail_len, tail, tail_len) != (long)tail_len) {
    free(tail);
    set_err("cannot read the end of %s", path);
    return -1;
  }
  long eocd = -1;
  for (long i = (long)tail_len - 22; i >= 0; i--)
    if (rd32(tail + i) == 0x06054b50) { eocd = i; break; }
  if (eocd < 0) {
    free(tail);
    set_err("%s is not a zip/APK file", path);
    return -1;
  }
  uint64_t count = rd16(tail + eocd + 10);
  uint64_t cd_size = rd32(tail + eocd + 12);
  uint64_t cd_off = rd32(tail + eocd + 16);
  if ((count == 0xffff || cd_size == 0xffffffffu || cd_off == 0xffffffffu) && eocd >= 20 &&
      rd32(tail + eocd - 20) == 0x07064b50) {
    // zip64: the locator points at the zip64 end record
    const uint64_t z64 = rd64(tail + eocd - 20 + 8);
    uint8_t rec[56];
    if (apkfile_read(base + z64, rec, sizeof rec) == (long)sizeof rec && rd32(rec) == 0x06064b50) {
      count = rd64(rec + 32);
      cd_size = rd64(rec + 40);
      cd_off = rd64(rec + 48);
    }
  }
  free(tail);
  if (cd_off + cd_size > size || count == 0 || count > 1000000) {
    set_err("%s has a damaged zip directory", path);
    return -1;
  }

  uint8_t *cd = malloc(cd_size);
  if (!cd || apkfile_read(base + cd_off, cd, cd_size) != (long)cd_size) {
    free(cd);
    set_err("cannot read the zip directory of %s", path);
    return -1;
  }
  Entry *entries = realloc(s_entries, (s_count + count) * sizeof(Entry));
  char *names = malloc(cd_size); // names are shorter than their records
  if (!entries || !names) {
    free(cd);
    free(names);
    if (entries)
      s_entries = entries;
    set_err("out of memory reading %s", path);
    return -1;
  }
  s_entries = entries;
  memset(s_entries + s_count, 0, count * sizeof(Entry));
  size_t names_used = 0;
  const uint8_t *p8 = cd, *end = cd + cd_size;
  int n = s_count, added = 0;
  while (p8 + 46 <= end && rd32(p8) == 0x02014b50 && (uint64_t)(n - s_count) < count) {
    const uint16_t nlen = rd16(p8 + 28), xlen = rd16(p8 + 30), clen = rd16(p8 + 32);
    if (p8 + 46 + nlen + xlen + clen > end)
      break;
    Entry *e = &s_entries[n];
    e->e.method = rd16(p8 + 10);
    e->e.mtime_dos = (uint32_t)rd16(p8 + 14) << 16 | rd16(p8 + 12);
    e->e.crc = rd32(p8 + 16);
    e->e.csize = rd32(p8 + 20);
    e->e.usize = rd32(p8 + 24);
    e->lho = rd32(p8 + 42);
    // zip64 extra: the fields that were 0xffffffff, in order
    const uint8_t *x = p8 + 46 + nlen, *xend = x + xlen;
    while (x + 4 <= xend) {
      const uint16_t id = rd16(x), sz2 = rd16(x + 2);
      if (id == 1) {
        const uint8_t *q = x + 4;
        if (e->e.usize == 0xffffffffu && q + 8 <= x + 4 + sz2) { e->e.usize = rd64(q); q += 8; }
        if (e->e.csize == 0xffffffffu && q + 8 <= x + 4 + sz2) { e->e.csize = rd64(q); q += 8; }
        if (e->lho == 0xffffffffu && q + 8 <= x + 4 + sz2) { e->lho = rd64(q); q += 8; }
      }
      x += 4 + sz2;
    }
    e->lho += base;
    e->part = k;
    memcpy(names + names_used, p8 + 46, nlen);
    names[names_used + nlen] = '\0';
    e->e.name = names + names_used;
    names_used += nlen + 1;
    p8 += 46 + nlen + xlen + clen;
    if (k > 0 && apkfile_find_quiet(e->e.name)) { // (the base's)
      memset(e, 0, sizeof *e);
      continue;
    }
    n++;
    added++;
  }
  free(cd);
  s_count = n;
  s_names_parts[k] = names;
  s_hash_cap = 1;
  while (s_hash_cap < (unsigned)n * 2 + 16)
    s_hash_cap <<= 1;
  free(s_hash);
  s_hash = malloc(s_hash_cap * sizeof(int));
  memset(s_hash, 0xff, s_hash_cap * sizeof(int));
  for (int i = 0; i < n; i++)
    hash_insert(i);
  log_printf("[apk] %s: %d entries, %llu MB", path, added, (unsigned long long)(size >> 20));
  return 0;
}

int apkfile_open(const char *path) {
  apkfile_close();
  mutexInit(&s_lock);
  snprintf(s_path, sizeof s_path, "%s", path);
  const int k = open_part(path);
  return k < 0 ? -1 : read_directory(k, path);
}

int apkfile_add(const char *path) {
  if (!s_open) {
    set_err("no APK open to add %s to", path);
    return -1;
  }
  const int k = open_part(path);
  return k < 0 || read_directory(k, path) < 0 ? -1 : k;
}

int apkfile_part_at(int i) {
  return i >= 0 && i < s_count ? s_entries[i].part : -1;
}

// the local header's own name/extra lengths decide where the data starts
static int resolve(Entry *e) {
  if (e->resolved)
    return 0;
  uint8_t lh[30];
  if (apkfile_read(e->lho, lh, sizeof lh) != (long)sizeof lh || rd32(lh) != 0x04034b50) {
    log_printf("[apk] bad local header for %s", e->e.name);
    return -1;
  }
  e->e.data_off = e->lho + 30 + rd16(lh + 26) + rd16(lh + 28);
  e->resolved = 1;
  return 0;
}

const ApkEntry *apkfile_entry_dir(int i) {
  return i >= 0 && i < s_count ? &s_entries[i].e : NULL;
}

const ApkEntry *apkfile_entry_at(int i) {
  if (i < 0 || i >= s_count)
    return NULL;
  mutexLock(&s_lock);
  int ok = resolve(&s_entries[i]) == 0;
  mutexUnlock(&s_lock);
  return ok ? &s_entries[i].e : NULL;
}

const ApkEntry *apkfile_find(const char *name) {
  if (!s_hash || !name)
    return NULL;
  unsigned i = name_hash(name) & (s_hash_cap - 1);
  while (s_hash[i] >= 0) {
    Entry *e = &s_entries[s_hash[i]];
    if (!strcmp(e->e.name, name)) {
      mutexLock(&s_lock);
      int ok = resolve(e) == 0;
      mutexUnlock(&s_lock);
      return ok ? &e->e : NULL;
    }
    i = (i + 1) & (s_hash_cap - 1);
  }
  return NULL;
}

void *apkfile_load(const ApkEntry *e, size_t *size_out) {
  if (!e)
    return NULL;
  uint8_t *out = malloc(e->usize ? e->usize : 1);
  if (!out)
    return NULL;
  if (e->method == 0) {
    if (apkfile_read(e->data_off, out, e->usize) != (long)e->usize) {
      free(out);
      return NULL;
    }
  } else if (e->method == 8) {
    uint8_t *in = malloc(e->csize ? e->csize : 1);
    if (!in || apkfile_read(e->data_off, in, e->csize) != (long)e->csize) {
      free(in);
      free(out);
      return NULL;
    }
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -MAX_WBITS);
    zs.next_in = in;
    zs.avail_in = (uInt)e->csize;
    zs.next_out = out;
    zs.avail_out = (uInt)e->usize;
    const int r = inflate(&zs, Z_FINISH);
    inflateEnd(&zs);
    free(in);
    if (r != Z_STREAM_END || zs.total_out != e->usize) {
      log_printf("[apk] inflating %s failed (%d)", e->name, r);
      free(out);
      return NULL;
    }
  } else {
    log_printf("[apk] %s: compression method %u is not supported", e->name, e->method);
    free(out);
    return NULL;
  }
  if (size_out)
    *size_out = (size_t)e->usize;
  return out;
}

int apkfile_extract(const ApkEntry *e, const char *dest, void (*progress)(uint64_t, void *),
                    void *arg) {
  char tmp[600];
  snprintf(tmp, sizeof tmp, "%s.part", dest);
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    log_printf("[apk] cannot write %s (errno %d)", tmp, errno);
    return -1;
  }
  const size_t CH = 1 << 20;
  uint8_t *in = malloc(CH), *out = malloc(CH);
  int ok = in && out;
  uint64_t done = 0;
  if (ok && e->method == 0) {
    while (done < e->usize && ok) {
      const size_t n = e->usize - done > CH ? CH : (size_t)(e->usize - done);
      ok = apkfile_read(e->data_off + done, in, n) == (long)n && fwrite(in, 1, n, f) == n;
      done += n;
      if (progress)
        progress(done, arg);
    }
  } else if (ok && e->method == 8) {
    z_stream zs;
    memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, -MAX_WBITS);
    uint64_t read_off = 0;
    int r = Z_OK;
    while (ok && r != Z_STREAM_END) {
      if (zs.avail_in == 0 && read_off < e->csize) {
        const size_t n = e->csize - read_off > CH ? CH : (size_t)(e->csize - read_off);
        ok = apkfile_read(e->data_off + read_off, in, n) == (long)n;
        read_off += n;
        zs.next_in = in;
        zs.avail_in = (uInt)n;
      }
      zs.next_out = out;
      zs.avail_out = (uInt)CH;
      r = inflate(&zs, Z_NO_FLUSH);
      if (r != Z_OK && r != Z_STREAM_END && !(r == Z_BUF_ERROR && zs.avail_in == 0 && read_off < e->csize))
        ok = 0;
      const size_t have = CH - zs.avail_out;
      if (ok && have && fwrite(out, 1, have, f) != have)
        ok = 0;
      done += have;
      if (progress)
        progress(done, arg);
      if (ok && r != Z_STREAM_END && zs.avail_in == 0 && read_off >= e->csize && have == 0)
        ok = 0; // truncated stream
    }
    inflateEnd(&zs);
    ok = ok && done == e->usize;
  } else {
    ok = 0;
  }
  free(in);
  free(out);
  if (fclose(f) != 0)
    ok = 0;
  if (!ok) {
    log_printf("[apk] extracting %s failed", e->name);
    remove(tmp);
    return -1;
  }
  remove(dest);
  if (rename(tmp, dest) != 0) {
    log_printf("[apk] cannot rename %s (errno %d)", tmp, errno);
    remove(tmp);
    return -1;
  }
  return 0;
}
