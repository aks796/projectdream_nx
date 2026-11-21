/* apkfile.h -- random access to the player's APK (apkfile.c)
 *
 * The APK is opened once with libnx's FsFile API, which reads at an offset
 * without a shared file position, so any thread can read any part of it at
 * any time and it costs one filesystem handle no matter how many virtual
 * files are open on it.
 *
 * Its zip central directory is parsed once into a hash table, which gives
 * every entry's data offset: a stored entry (data.unity3d, nabe.tar, the
 * _nabe_ chunks) is then just a byte range of the file.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef APKFILE_H
#define APKFILE_H

#include <stddef.h>
#include <stdint.h>

typedef struct {
  const char *name;     // entry name ("assets/bin/Data/data.unity3d")
  uint64_t data_off;    // where the (compressed) data starts in the APK
  uint64_t csize;       // compressed size
  uint64_t usize;       // uncompressed size
  uint16_t method;      // 0 stored, 8 deflated
  uint32_t crc;
  uint32_t mtime_dos;   // DOS date << 16 | DOS time
} ApkEntry;

// Open the APK and read its directory. 0 on success, else -1 with
// apkfile_error() telling why.
int apkfile_open(const char *path);
// Add a split APK (its native libraries: an app bundle's
// split_config.arm64_v8a.apk) to the open one; its entries are found as the
// APK's own (a name in both stays the APK's). Also another zip read in place
// (MKT KV's Nabe.zip). Its part number (1, 2), or -1.
int apkfile_add(const char *path);
// the part entry i is in (0 the APK, then each apkfile_add's), -1 none
int apkfile_part_at(int i);
void apkfile_close(void);
const char *apkfile_error(void);
const char *apkfile_path(void);
uint64_t apkfile_size(void);

// pread on the APK: bytes read, or -1.
long apkfile_read(uint64_t off, void *buf, size_t len);

int apkfile_count(void);
const ApkEntry *apkfile_entry_at(int i);
// entry i as the zip's directory gives it, without reading its local header:
// name, method and sizes (data_off is apkfile_entry_at's)
const ApkEntry *apkfile_entry_dir(int i);
const ApkEntry *apkfile_find(const char *name);

// An entry's whole uncompressed content in a malloc'd buffer (inflating a
// deflated one); NULL on failure. *size_out gets its length.
void *apkfile_load(const ApkEntry *e, size_t *size_out);

// Copy an entry to a file on the SD (inflating as needed), through a
// temporary name; progress(bytes_done) is called now and then if given.
int apkfile_extract(const ApkEntry *e, const char *dest, void (*progress)(uint64_t done, void *arg),
                    void *arg);

// reads since the last call, by size (<=4 KB, <=64 KB, <=1 MB, more), and their time
void apkfile_stats(unsigned counts[4], double *ms);

// the small reads' cache since the last call: served from it, read into it
void apkfile_cache_stats(unsigned *hits, unsigned *misses);

#endif
