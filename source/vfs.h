/* vfs.h -- virtual fds over byte ranges of the game file (vfs.c)
 *
 * The libc shim asks vfs_classify about every path (always a real file here)
 * and routes the virtual fds (VFD_BASE..) of AAsset_openFileDescriptor here.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef VFS_H
#define VFS_H

#include <stdint.h>
#include <stdio.h>
#include "nabe.h"

#define VFD_BASE 0x7b000000
#define VFD_MAX  2048

typedef enum {
  VFS_REAL,   // not ours (or the overlay's): use the real filesystem
  VFS_VFILE,  // a file of the tar
  VFS_VDIR,   // a directory of the tar (may also exist on the SD)
  VFS_GONE,   // a tar file the game deleted
} VfsKind;

// After nabe_init: scan the overlay, load the whiteouts.
void vfs_init(void);
int vfs_ready(void);

// path: an SD path as the game's file calls hand it over (Android paths
// already mapped). *f gets the tar member for VFS_VFILE.
VfsKind vfs_classify(const char *path, const NabeFile **f);

// --- virtual fds -----------------------------------------------------------
int vfd_open(const NabeFile *f);
int vfd_is(int fd);
long vfd_read(int fd, void *buf, size_t n);
long vfd_pread(int fd, void *buf, size_t n, int64_t off);
int64_t vfd_lseek(int fd, int64_t off, int whence); // newlib SEEK_* values
int vfd_close(int fd);
uint64_t vfd_size(int fd);
int vfd_dup(int fd);

// A newlib FILE* reading a tar member (fopencookie); fileno() through
// vfs_file_fd() gives its virtual fd.
FILE *vfs_fopen(const NabeFile *f);
int vfs_file_fd(FILE *fp);       // -1 when fp is not one of ours
void vfs_file_closed(FILE *fp);

// --- the game changing the tree ---------------------------------------------
// About to create or rewrite path (open for writing, fopen "w"/"a"/"+"):
// makes its directories exist on the SD and, unless truncate, copies a tar
// file's content out first. Returns 0, or -1 with errno set.
int vfs_prepare_write(const char *path, int truncate);
// path now exists on the SD (created / renamed into place)
void vfs_note_real(const char *path);
// path was removed: hides a tar file of that name from now on. Returns 1
// when the path was a tar file (so a missing real file is not an error).
int vfs_note_removed(const char *path);
// rename of a tar file: copies it to `to` and hides `from`. 0 / -1.
int vfs_rename_virtual(const NabeFile *f, const char *from, const char *to);

// Directory listing: calls fn for each tar child of path not deleted (the
// caller merges in the real directory's entries and drops duplicates).
int vfs_list_virtual(const char *path, void (*fn)(const char *name, int is_dir, void *arg),
                     void *arg);

// counters for the log
void vfs_stats(unsigned *opens, unsigned *reads_mb);

#endif
