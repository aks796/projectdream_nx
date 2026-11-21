/* vfs.c -- virtual fds over byte ranges of the game file (see vfs.h)
 *
 * From mkt_nx, where it also served a whole asset tree out of the APK; here
 * only AAsset_openFileDescriptor's file descriptors are virtual: a window of
 * sdt.apk read with apkfile_read. Every path is a real file.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <switch.h>

#include "apkfile.h"
#include "log.h"
#include "vfs.h"

static unsigned s_opens;
static uint64_t s_read_bytes;

void vfs_init(void) {}

int vfs_ready(void) { return 1; }

void vfs_stats(unsigned *opens, unsigned *reads_mb) {
  *opens = s_opens;
  *reads_mb = (unsigned)(s_read_bytes >> 20);
  s_opens = 0;
  s_read_bytes = 0;
}

VfsKind vfs_classify(const char *path, const NabeFile **f) {
  (void)path;
  (void)f;
  return VFS_REAL;
}

// --- virtual fds -------------------------------------------------------------

typedef struct {
  int used;
  int refs;          // dup'd fds share the position (like POSIX)
  uint64_t base, size, pos;
} Vfd;

static Vfd s_vfd[VFD_MAX];
static Mutex s_vfd_lock;

static Vfd *vfd_get(int fd) {
  if (fd < VFD_BASE || fd >= VFD_BASE + VFD_MAX)
    return NULL;
  Vfd *v = &s_vfd[fd - VFD_BASE];
  return v->used ? v : NULL;
}

int vfd_is(int fd) {
  return vfd_get(fd) != NULL;
}

int vfd_open(const NabeFile *f) {
  mutexLock(&s_vfd_lock);
  for (int i = 0; i < VFD_MAX; i++) {
    if (!s_vfd[i].used) {
      s_vfd[i].used = 1;
      s_vfd[i].refs = 1;
      s_vfd[i].base = f->off;
      s_vfd[i].size = f->size;
      s_vfd[i].pos = 0;
      mutexUnlock(&s_vfd_lock);
      s_opens++;
      return VFD_BASE + i;
    }
  }
  mutexUnlock(&s_vfd_lock);
  log_printf("[vfs] out of virtual fds");
  errno = EMFILE;
  return -1;
}

long vfd_pread(int fd, void *buf, size_t n, int64_t off) {
  Vfd *v = vfd_get(fd);
  if (!v) {
    errno = EBADF;
    return -1;
  }
  if (off < 0) {
    errno = EINVAL;
    return -1;
  }
  if ((uint64_t)off >= v->size || !n)
    return 0;
  if (n > v->size - (uint64_t)off)
    n = (size_t)(v->size - (uint64_t)off);
  const long got = apkfile_read(v->base + (uint64_t)off, buf, n);
  if (got < 0) {
    errno = EIO;
    return -1;
  }
  s_read_bytes += (uint64_t)got;
  return got;
}

long vfd_read(int fd, void *buf, size_t n) {
  Vfd *v = vfd_get(fd);
  if (!v) {
    errno = EBADF;
    return -1;
  }
  const long got = vfd_pread(fd, buf, n, (int64_t)v->pos);
  if (got > 0)
    v->pos += (uint64_t)got;
  return got;
}

int64_t vfd_lseek(int fd, int64_t off, int whence) {
  Vfd *v = vfd_get(fd);
  if (!v) {
    errno = EBADF;
    return -1;
  }
  int64_t np;
  switch (whence) {
    case SEEK_SET: np = off; break;
    case SEEK_CUR: np = (int64_t)v->pos + off; break;
    case SEEK_END: np = (int64_t)v->size + off; break;
    default: errno = EINVAL; return -1;
  }
  if (np < 0) {
    errno = EINVAL;
    return -1;
  }
  v->pos = (uint64_t)np;
  return np;
}

uint64_t vfd_size(int fd) {
  Vfd *v = vfd_get(fd);
  return v ? v->size : 0;
}

int vfd_close(int fd) {
  mutexLock(&s_vfd_lock);
  Vfd *v = vfd_get(fd);
  if (!v) {
    mutexUnlock(&s_vfd_lock);
    errno = EBADF;
    return -1;
  }
  v->used = 0;
  mutexUnlock(&s_vfd_lock);
  return 0;
}

int vfd_dup(int fd) {
  Vfd *v = vfd_get(fd);
  if (!v) {
    errno = EBADF;
    return -1;
  }
  NabeFile f = { NULL, v->base, v->size };
  const int nfd = vfd_open(&f);
  if (nfd >= 0)
    s_vfd[nfd - VFD_BASE].pos = v->pos;
  return nfd;
}

// --- FILE* over a virtual fd --------------------------------------------------

#define MAX_VFILES 256
static struct { FILE *fp; int fd; } s_vfiles[MAX_VFILES];

static ssize_t ck_read(void *c, char *buf, size_t n) {
  return vfd_read((int)(intptr_t)c, buf, n);
}

static int ck_seek(void *c, off_t *off, int whence) {
  const int64_t r = vfd_lseek((int)(intptr_t)c, *off, whence);
  if (r < 0)
    return -1;
  *off = r;
  return 0;
}

static int ck_close(void *c) {
  return vfd_close((int)(intptr_t)c);
}

FILE *vfs_fopen(const NabeFile *f) {
  const int fd = vfd_open(f);
  if (fd < 0)
    return NULL;
  cookie_io_functions_t io = { ck_read, NULL, ck_seek, ck_close };
  FILE *fp = fopencookie((void *)(intptr_t)fd, "r", io);
  if (!fp) {
    vfd_close(fd);
    return NULL;
  }
  setvbuf(fp, NULL, _IOFBF, 64 * 1024);
  mutexLock(&s_vfd_lock);
  for (int i = 0; i < MAX_VFILES; i++) {
    if (!s_vfiles[i].fp) {
      s_vfiles[i].fp = fp;
      s_vfiles[i].fd = fd;
      break;
    }
  }
  mutexUnlock(&s_vfd_lock);
  return fp;
}

int vfs_file_fd(FILE *fp) {
  int fd = -1;
  mutexLock(&s_vfd_lock);
  for (int i = 0; i < MAX_VFILES; i++)
    if (s_vfiles[i].fp == fp) {
      fd = s_vfiles[i].fd;
      break;
    }
  mutexUnlock(&s_vfd_lock);
  return fd;
}

void vfs_file_closed(FILE *fp) {
  mutexLock(&s_vfd_lock);
  for (int i = 0; i < MAX_VFILES; i++)
    if (s_vfiles[i].fp == fp)
      s_vfiles[i].fp = NULL;
  mutexUnlock(&s_vfd_lock);
}

// --- the tree: none (every path is a real file) --------------------------------

int vfs_prepare_write(const char *path, int truncate) {
  (void)path;
  (void)truncate;
  return 0;
}

void vfs_note_real(const char *path) { (void)path; }

int vfs_note_removed(const char *path) {
  (void)path;
  return 0;
}

int vfs_rename_virtual(const NabeFile *f, const char *from, const char *to) {
  (void)f;
  (void)from;
  (void)to;
  errno = ENOENT;
  return -1;
}

int vfs_list_virtual(const char *path, void (*fn)(const char *, int, void *), void *arg) {
  (void)path;
  (void)fn;
  (void)arg;
  return -1;
}
