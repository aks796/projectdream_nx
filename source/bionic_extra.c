/* bionic_extra.c -- the rest of the libc surface the game's libraries need
 * (libc_shim.c, net_shim.c and pthr.c hold the core).
 *
 * Mostly std::filesystem's POSIX calls (directory file descriptors for
 * remove_all, statvfs, sendfile for copy_file), fortify wrappers, time
 * helpers, and honest failures for what Horizon has no equivalent of
 * (eventfd, ptrace): curl falls back without them. epoll and pipes are
 * emulated on sockets in net_shim.c.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include "pthr.h"
#include <wchar.h>
#include <sys/stat.h>
#include <sys/iosupport.h>
#include <switch.h>

#include "apk.h"
#include "bionic_extra.h"
#include "so_util.h"
#include "libc_shim.h"
#include "net_shim.h"
#include "log.h"
#include "config.h"
#include "paths.h"
#include "sandbox.h"
#include "vfs.h"
#include "save_writer.h"

// bionic (Linux arm64) values
#define L_AT_FDCWD      (-100)
#define L_AT_REMOVEDIR  0x200
#define L_O_DIRECTORY   040000
#define L_ENOSYS        38
#define L_ENOTDIR       20
#define L_EINVAL        22

static int fail(int linux_errno) {
  errno = linux_errno;
  return -1;
}

// newlib errno -> Linux errno for filesystem results
static int fs_result(int r) {
  if (r < 0)
    errno = net_errno_to_linux(errno);
  return r;
}

// a delete that failed for another reason than "not there": into debug.log
// with the FS result libnx folded into EIO
static int fs_delete_result(int r, const char *what, const char *path) {
  if (r < 0 && errno != ENOENT) {
    const Result rc = fsdevGetLastResult();
    log_printf("[fs] %s %s failed: errno %d, result 0x%x (%04u-%04u)", what, path, errno, rc,
               2000 + R_MODULE(rc), R_DESCRIPTION(rc));
  }
  return fs_result(r);
}

// ---------------------------------------------------------------------------
// directory file descriptors (openat/fdopendir/unlinkat), for libc++'s
// remove_all and directory iteration. Horizon cannot open a directory as a
// file, so a directory fd is a slot remembering its path.
// ---------------------------------------------------------------------------

#define DIRFD_BASE 0x7e000000
#define MAX_DIRFDS 256

static struct {
  char *path;
  void *dir; // set once fdopendir() took the fd over (a snapshot listing)
} dirfds[MAX_DIRFDS];
static Mutex dirfd_lock;

int is_dirfd_fake(int fd) {
  return fd >= DIRFD_BASE && fd < DIRFD_BASE + MAX_DIRFDS && dirfds[fd - DIRFD_BASE].path;
}

static int dirfd_alloc(const char *path) {
  mutexLock(&dirfd_lock);
  for (int i = 0; i < MAX_DIRFDS; i++) {
    if (!dirfds[i].path) {
      dirfds[i].path = strdup(path);
      dirfds[i].dir = NULL;
      mutexUnlock(&dirfd_lock);
      return DIRFD_BASE + i;
    }
  }
  mutexUnlock(&dirfd_lock);
  return fail(24); // EMFILE
}

static const char *dirfd_path(int fd) {
  return is_dirfd_fake(fd) ? dirfds[fd - DIRFD_BASE].path : NULL;
}

void dirfd_close_fake(int fd) {
  if (!is_dirfd_fake(fd))
    return;
  mutexLock(&dirfd_lock);
  free(dirfds[fd - DIRFD_BASE].path);
  dirfds[fd - DIRFD_BASE].path = NULL;
  dirfds[fd - DIRFD_BASE].dir = NULL;
  mutexUnlock(&dirfd_lock);
}

int dirfd_stat_fake(int fd, struct stat *st) {
  const char *p = dirfd_path(fd);
  return p ? stat(p, st) : fail(9); // EBADF
}

// join (dirfd, name) into out; 0 or -1 (EBADF)
static int at_path(int dirfd, const char *name, char *out, size_t outlen) {
  if (!name)
    return fail(14); // EFAULT
  if (name[0] == '/' || dirfd == L_AT_FDCWD) {
    snprintf(out, outlen, "%s", fix_path(name));
    return 0;
  }
  const char *base = dirfd_path(dirfd);
  if (!base)
    return fail(9);
  snprintf(out, outlen, "%s/%s", base, name);
  return 0;
}

int openat_fake(int dirfd, const char *name, int flags, ...) {
  int mode = 0666;
  if (flags & 0100) { // O_CREAT
    va_list va;
    va_start(va, flags);
    mode = va_arg(va, int);
    va_end(va);
  }
  char path[1024];
  if (at_path(dirfd, name, path, sizeof(path)) < 0)
    return -1;
  struct bionic_stat_lite { uint64_t dev, ino; uint32_t mode; uint32_t pad[25]; } bst;
  struct stat st;
  int exists = stat_fake(path, (struct bionic_stat *)&bst) == 0;
  st.st_mode = exists ? bst.mode : 0;
  if (flags & L_O_DIRECTORY) {
    if (!exists)
      return fail(2);
    if (!S_ISDIR(st.st_mode))
      return fail(L_ENOTDIR);
    return dirfd_alloc(path);
  }
  if (exists && S_ISDIR(st.st_mode) && (flags & 3) == 0)
    return dirfd_alloc(path); // O_RDONLY on a directory, as Linux allows
  return open_fake(path, flags & ~(L_O_DIRECTORY | 0100000 | 02000000), mode);
}

void *fdopendir_fake(int fd) {
  const char *p = dirfd_path(fd);
  if (!p) {
    errno = 9;
    return NULL;
  }
  void *d = snapdir_open(p);
  if (!d)
    return NULL;
  mutexLock(&dirfd_lock);
  dirfds[fd - DIRFD_BASE].dir = d;
  mutexUnlock(&dirfd_lock);
  return d;
}

int closedir_fake(void *dirp) {
  mutexLock(&dirfd_lock);
  for (int i = 0; i < MAX_DIRFDS; i++) {
    if (dirfds[i].path && dirfds[i].dir == dirp) {
      free(dirfds[i].path);
      dirfds[i].path = NULL;
      dirfds[i].dir = NULL;
      break;
    }
  }
  mutexUnlock(&dirfd_lock);
  return snapdir_close(dirp);
}

int dirfd_of_fake(void *dirp) {
  mutexLock(&dirfd_lock);
  for (int i = 0; i < MAX_DIRFDS; i++) {
    if (dirfds[i].path && dirfds[i].dir == dirp) {
      mutexUnlock(&dirfd_lock);
      return DIRFD_BASE + i;
    }
  }
  mutexUnlock(&dirfd_lock);
  return fail(L_ENOSYS);
}

int unlinkat_fake(int dirfd, const char *name, int flags) {
  char path[1024];
  if (at_path(dirfd, name, path, sizeof(path)) < 0)
    return -1;
  if (!sandbox_check(path, (flags & L_AT_REMOVEDIR) ? "rmdir" : "unlink"))
    return -1;
  return (flags & L_AT_REMOVEDIR) ? fs_delete_result(rmdir(path), "rmdir", path)
                                  : unlink_fake(path);
}

int fstatat_fake(int dirfd, const char *name, struct bionic_stat *st, int flags) {
  (void)flags;
  char path[1024];
  if (at_path(dirfd, name, path, sizeof(path)) < 0)
    return -1;
  return stat_fake(path, st);
}

// the kernel's faccessat: no flags argument (bionic's flags are faccessat2's)
int faccessat_fake(int dirfd, const char *name, int mode) {
  char path[1024];
  if (at_path(dirfd, name, path, sizeof(path)) < 0)
    return -1;
  return access_fake(path, mode);
}

int mkdirat_fake(int dirfd, const char *name, unsigned mode) {
  char path[1024];
  if (at_path(dirfd, name, path, sizeof(path)) < 0)
    return -1;
  if (path_android_ancestor(path)) {
    errno = 17; // EEXIST
    return -1;
  }
  if (!sandbox_check(path, "mkdir"))
    return -1;
  return fs_result(mkdir(path, mode));
}

int renameat_fake(int olddirfd, const char *oldname, int newdirfd, const char *newname) {
  char from[1024], to[1024];
  if (at_path(olddirfd, oldname, from, sizeof(from)) < 0 ||
      at_path(newdirfd, newname, to, sizeof(to)) < 0)
    return -1;
  return fs_result(rename_fake(from, to));
}

// ---------------------------------------------------------------------------
// plain path calls
// ---------------------------------------------------------------------------

int unlink_fake(const char *path) {
  const char *p = fix_path(path);
  sw_wait_path(p);
  if (!sandbox_check(p, "unlink"))
    return -1;
  const VfsKind k = vfs_classify(p, NULL);
  if (k == VFS_GONE)
    return fail(2);
  if (k == VFS_VFILE) {
    vfs_note_removed(p);
    return 0;
  }
  const int r = fs_delete_result(unlink(p), "unlink", path);
  if (r == 0)
    vfs_note_removed(p);
  return r;
}
int rmdir_fake(const char *path) {
  const char *p = fix_path(path);
  if (!sandbox_check(p, "rmdir"))
    return -1;
  return fs_delete_result(rmdir(p), "rmdir", path);
}
int truncate_fake(const char *path, long len) {
  const char *p = fix_path(path);
  if (!sandbox_check(p, "truncate"))
    return -1;
  return fs_result(truncate(p, len));
}
void fs_trace(int fd, const char *what, long a, long r); // libc_shim.c
static FsFile *fs_file_of(int fd);

int ftruncate_fake(int fd, long len) {
  fs_trace(fd, "ftruncate", len, 0);
  if (sw_is_memfd(fd))
    return sw_ftruncate(fd, len) == 0 ? 0 : fail(L_EINVAL);
  if (vfd_is(fd))
    return fail(9); // read-only
  return fs_result(ftruncate(fd, len));
}
int link_fake(const char *a, const char *b) { (void)a; (void)b; return fail(1); } // EPERM
// No symlinks on the card.
int symlink_fake(const char *a, const char *b) {
  (void)a;
  (void)b;
  return fail(1);
}
long readlink_fake(const char *path, char *buf, size_t len) {
  (void)path; (void)buf; (void)len;
  return fail(L_EINVAL); // nothing is a symlink
}
long pathconf_fake(const char *path, int name) {
  (void)path;
  return name == 3 ? 255 : (name == 4 ? 1024 : -1); // _PC_NAME_MAX, _PC_PATH_MAX
}
int utimensat_fake(int dirfd, const char *path, const void *times, int flags) {
  (void)dirfd; (void)path; (void)times; (void)flags;
  return 0;
}
int utime_fake(const char *path, const void *times) { (void)path; (void)times; return 0; }
int fchmod_fake(int fd, unsigned mode) { (void)fd; (void)mode; return 0; }
int fchmodat_fake(int dirfd, const char *path, unsigned mode, int flags) {
  (void)dirfd; (void)path; (void)mode; (void)flags;
  return 0;
}
int isatty_fake(int fd) { (void)fd; errno = 25; return 0; } // ENOTTY
int fdatasync_fake(int fd) {
  if (vfd_is(fd) || sw_is_memfd(fd))
    return 0;
  FsFile *ff = fs_file_of(fd);
  if (ff)
    return R_SUCCEEDED(fsFileFlush(ff)) ? 0 : fail(5);
  return fs_result(fsync(fd));
}
int fsync_fake(int fd) {
  fs_trace(fd, "fsync", 0, 0);
  if (vfd_is(fd) || sw_is_memfd(fd))
    return 0;
  // libnx's sdmc device has no fsync (ENOSYS): a file on the card is flushed through its FsFile. (Project Dream's
  // key-value store fsyncs before its rename and gave up on the error: the cloud copy of the save was never stored.)
  FsFile *f = fs_file_of(fd);
  if (f)
    return R_SUCCEEDED(fsFileFlush(f)) ? 0 : fail(5);
  return fs_result(fsync(fd));
}
int msync_fake(void *addr, size_t len, int flags) { (void)addr; (void)len; (void)flags; return 0; }

char *getcwd_fake(char *buf, size_t size) {
  char tmp[1024];
  if (!getcwd(tmp, sizeof(tmp)))
    return NULL;
  // drop the "sdmc:" device prefix: every other path the game sees is
  // '/'-rooted on the default device
  const char *colon = strchr(tmp, ':');
  const char *p = colon ? colon + 1 : tmp;
  if (!buf) {
    size = size ? size : strlen(p) + 1;
    buf = malloc(size);
  }
  if (strlen(p) + 1 > size) {
    errno = 34; // ERANGE
    return NULL;
  }
  strcpy(buf, p);
  return buf;
}

// realpath: normalize "." / ".." / "//" lexically, then require existence
char *realpath_full_fake(const char *path, char *resolved) {
  if (!path) {
    errno = L_EINVAL;
    return NULL;
  }
  char in[1024], out[1024];
  if (path[0] == '/') {
    snprintf(in, sizeof(in), "%s", fix_path(path));
  } else {
    char cwd[512];
    if (!getcwd_fake(cwd, sizeof(cwd)))
      snprintf(cwd, sizeof(cwd), "/");
    snprintf(in, sizeof(in), "%s/%s", cwd, path);
  }
  size_t n = 0;
  out[0] = '\0';
  for (char *save = NULL, *tok = strtok_r(in, "/", &save); tok; tok = strtok_r(NULL, "/", &save)) {
    if (!strcmp(tok, "."))
      continue;
    if (!strcmp(tok, "..")) {
      char *slash = strrchr(out, '/');
      n = slash ? (size_t)(slash - out) : 0;
      out[n] = '\0';
      continue;
    }
    n += snprintf(out + n, sizeof(out) - n, "/%s", tok);
    if (n >= sizeof(out)) {
      errno = 36; // ENAMETOOLONG
      return NULL;
    }
  }
  if (!n)
    snprintf(out, sizeof(out), "/");
  struct stat st;
  if (stat(out, &st) < 0) {
    errno = 2;
    return NULL;
  }
  if (!resolved)
    resolved = malloc(strlen(out) + 1);
  strcpy(resolved, out);
  return resolved;
}

char *mkdtemp_fake(char *tmpl) {
  const size_t len = strlen(tmpl);
  if (len < 6 || strcmp(tmpl + len - 6, "XXXXXX") != 0) {
    errno = L_EINVAL;
    return NULL;
  }
  static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  for (int attempt = 0; attempt < 100; attempt++) {
    uint8_t r[6];
    randomGet(r, sizeof(r));
    for (int i = 0; i < 6; i++)
      tmpl[len - 6 + i] = chars[r[i] % (sizeof(chars) - 1)];
    const char *p = fix_path(tmpl);
    if (!sandbox_check(p, "mkdtemp"))
      return NULL;
    if (mkdir(p, 0700) == 0)
      return tmpl;
  }
  errno = 17;
  return NULL;
}

// bionic struct statvfs (LP64)
struct bionic_statvfs {
  unsigned long f_bsize, f_frsize;
  uint64_t f_blocks, f_bfree, f_bavail, f_files, f_ffree, f_favail;
  unsigned long f_fsid, f_flag, f_namemax;
  uint32_t spare[6];
};

int statvfs_fake(const char *path, struct bionic_statvfs *out) {
  (void)path;
  memset(out, 0, sizeof(*out));
  s64 total = 0, free_bytes = 0;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs) {
    fsFsGetTotalSpace(fs, "/", &total);
    fsFsGetFreeSpace(fs, "/", &free_bytes);
  }
  out->f_bsize = out->f_frsize = 32768;
  out->f_blocks = (uint64_t)total / 32768;
  out->f_bfree = out->f_bavail = (uint64_t)free_bytes / 32768;
  out->f_files = out->f_ffree = out->f_favail = 1u << 20;
  out->f_namemax = 255;
  return 0;
}

// sendfile: std::filesystem::copy_file's fast path
long sendfile_fake(int out_fd, int in_fd, int64_t *offset, size_t count) {
  static __thread uint8_t *buf;
  if (!buf)
    buf = malloc(64 * 1024);
  off_t saved = -1;
  if (offset) {
    saved = lseek_fake(in_fd, 0, SEEK_CUR);
    lseek_fake(in_fd, *offset, SEEK_SET);
  }
  long total = 0;
  while ((size_t)total < count) {
    const size_t want = count - total < 64 * 1024 ? count - total : 64 * 1024;
    const ssize_t got = read_fake(in_fd, buf, want);
    if (got <= 0)
      break;
    const ssize_t put = write_fake(out_fd, buf, got);
    if (put < 0) {
      total = total ? total : -1;
      break;
    }
    total += put;
    if (put < got)
      break;
  }
  if (offset) {
    if (total > 0)
      *offset += total;
    lseek_fake(in_fd, saved, SEEK_SET);
  }
  return total;
}

// pread/pwrite read and write at an offset without touching the descriptor's
// position, and several threads may use one descriptor at once (Unity's
// loading threads and Project Dream's loader all read sdt.apk). mkt_nx's
// version seeked, read and seeked back: two threads on one descriptor read
// each other's offsets (wrong bytes, no error). A file on the card is read
// with its FsFile at the offset (libnx's fsdev keeps it first in the
// descriptor's file struct); anything else under a lock.
typedef struct {
  FsFile fd; // (libnx fs_dev.c's fsdev_file_t starts so)
  int flags;
  s64 offset;
} FsdevFile;

static FsFile *fs_file_of(int fd) {
  __handle *h = __get_handle(fd);
  if (!h || !h->fileStruct || h->device < 0 || h->device >= STD_MAX || !devoptab_list[h->device] ||
      !devoptab_list[h->device]->name || strcmp(devoptab_list[h->device]->name, "sdmc") != 0)
    return NULL;
  return &((FsdevFile *)h->fileStruct)->fd;
}

// a file on the SD card (not a socket, pipe or virtual file)
int fd_is_sd_file(int fd) {
  return fs_file_of(fd) != NULL;
}

static Mutex s_pio_lock;

// File reads the game makes ([io] lines with the [perf] ones): how many, how big, how long. On the console each
// read is a round trip to the file system service and the SD card, so their sizes say why loading takes long.
static u64 s_io_calls, s_io_small, s_io_bytes, s_io_ticks;

void io_note(size_t asked, long got, u64 ticks) {
  __atomic_add_fetch(&s_io_calls, 1, __ATOMIC_RELAXED);
  if (asked < 16384)
    __atomic_add_fetch(&s_io_small, 1, __ATOMIC_RELAXED);
  if (got > 0)
    __atomic_add_fetch(&s_io_bytes, (u64)got, __ATOMIC_RELAXED);
  __atomic_add_fetch(&s_io_ticks, ticks, __ATOMIC_RELAXED);
}

void io_take_stats(u64 *calls, u64 *small, u64 *bytes, u64 *ns) {
  *calls = __atomic_exchange_n(&s_io_calls, 0, __ATOMIC_RELAXED);
  *small = __atomic_exchange_n(&s_io_small, 0, __ATOMIC_RELAXED);
  *bytes = __atomic_exchange_n(&s_io_bytes, 0, __ATOMIC_RELAXED);
  *ns = armTicksToNs(__atomic_exchange_n(&s_io_ticks, 0, __ATOMIC_RELAXED));
}

static long pread_real(int fd, void *buf, size_t count, int64_t off);

long pread_fake(int fd, void *buf, size_t count, int64_t off) {
  if (vfd_is(fd))
    return vfd_pread(fd, buf, count, off);
  if (sw_is_memfd(fd)) {
    const long at = sw_lseek(fd, 0, 1);
    sw_lseek(fd, (long)off, 0);
    const long r = sw_read(fd, buf, count);
    sw_lseek(fd, at, 0);
    return r;
  }
  const u64 t0 = armGetSystemTick();
  const long r = pread_real(fd, buf, count, off);
  io_note(count, r, armGetSystemTick() - t0);
  return r;
}

static long pread_real(int fd, void *buf, size_t count, int64_t off) {
  if (off < 0) {
    errno = L_EINVAL;
    return -1;
  }
  FsFile *f = fs_file_of(fd);
  if (f) {
    u64 got = 0;
    const Result rc = fsFileRead(f, off, buf, count, FsReadOption_None, &got);
    if (R_FAILED(rc)) {
      errno = 5; // EIO
      return -1;
    }
    return (long)got;
  }
  mutexLock(&s_pio_lock);
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  long r = -1;
  if (lseek(fd, off, SEEK_SET) >= 0)
    r = read(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  mutexUnlock(&s_pio_lock);
  return fs_result(r);
}

long pwrite_fake(int fd, const void *buf, size_t count, int64_t off) {
  if (off < 0) {
    errno = L_EINVAL;
    return -1;
  }
  FsFile *f = fs_file_of(fd);
  if (f) {
    s64 size = 0;
    if (R_SUCCEEDED(fsFileGetSize(f, &size)) && off + (s64)count > size && R_FAILED(fsFileSetSize(f, off + (s64)count))) {
      errno = 28; // ENOSPC
      return -1;
    }
    if (R_FAILED(fsFileWrite(f, off, buf, count, FsWriteOption_None))) {
      errno = 5; // EIO
      return -1;
    }
    return (long)count;
  }
  mutexLock(&s_pio_lock);
  const off_t saved = lseek(fd, 0, SEEK_CUR);
  long r = -1;
  if (lseek(fd, off, SEEK_SET) >= 0)
    r = write(fd, buf, count);
  lseek(fd, saved, SEEK_SET);
  mutexUnlock(&s_pio_lock);
  return fs_result(r);
}

// ---------------------------------------------------------------------------
// fortify (_chk) variants: the checks are the compiler's; just do the call
// ---------------------------------------------------------------------------

void *__memchr_chk_fake(const void *s, int c, size_t n, size_t buflen) {
  (void)buflen;
  return memchr(s, c, n);
}
int __poll_chk_fake(void *fds, unsigned nfds, int timeout, size_t fdslen) {
  (void)fdslen;
  return poll_fake(fds, nfds, timeout);
}
long __pread64_chk_fake(int fd, void *buf, size_t count, int64_t off, size_t buflen) {
  (void)buflen;
  return pread_fake(fd, buf, count, off);
}
long __readlink_chk_fake(const char *path, char *buf, size_t len, size_t buflen) {
  (void)buflen;
  return readlink_fake(path, buf, len);
}
int __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen) {
  (void)setlen;
  return (((const unsigned long *)set)[fd / 64] >> (fd % 64)) & 1;
}
void *__cmsg_nxthdr_fake(void *msg, void *cmsg) {
  (void)msg; (void)cmsg;
  return NULL;
}

// ---------------------------------------------------------------------------
// stdio extras
// ---------------------------------------------------------------------------

extern uint8_t fake_sF[3][0x100];
FILE *bionic_stdin = (FILE *)fake_sF[0];
FILE *bionic_stdout = (FILE *)fake_sF[1];
FILE *bionic_stderr = (FILE *)fake_sF[2];

wint_t fputwc_fake(wchar_t c, FILE *f) {
  if ((uint8_t *)f >= &fake_sF[0][0] && (uint8_t *)f < &fake_sF[0][0] + sizeof(fake_sF))
    return (wint_t)c;
  return fputwc(c, f);
}

// ---------------------------------------------------------------------------
// logging
// ---------------------------------------------------------------------------

int android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va) {
  if (prio < 5 && !config.log_all) // VERBOSE..INFO: Geode keeps its own log file
    return 0;
  char line[1024];
  vsnprintf(line, sizeof(line), fmt, va);
  log_printf("[%s] %s", tag ? tag : "?", line);
  return 0;
}

int android_log_print_fake(int prio, const char *tag, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = android_log_vprint_fake(prio, tag, fmt, va);
  va_end(va);
  return r;
}

int android_log_buf_print_fake(int buf, int prio, const char *tag, const char *fmt, ...) {
  (void)buf;
  va_list va;
  va_start(va, fmt);
  int r = android_log_vprint_fake(prio, tag, fmt, va);
  va_end(va);
  return r;
}

int android_log_write_fake2(int prio, const char *tag, const char *msg) {
  if (prio >= 5 || config.log_all)
    log_printf("[%s] %s", tag ? tag : "?", msg ? msg : "");
  return 0;
}

// ---------------------------------------------------------------------------
// process, randomness, time
// ---------------------------------------------------------------------------

void exit_now_fake(int code) {
  log_printf("[_exit] code %d", code);
  log_flush();
  _Exit(code);
}
int raise_fake(int sig) {
  log_printf("[libc] raise(%d)", sig);
  if (sig == 6) // SIGABRT
    abort_fake();
  return 0;
}
int register_atfork_fake(void *a, void *b, void *c, void *d) {
  (void)a; (void)b; (void)c; (void)d;
  return 0;
}
int pthread_sigmask_fake(int how, const void *set, void *old) {
  (void)how; (void)set;
  if (old)
    memset(old, 0, 8);
  return 0;
}
int prctl_fake(int option, ...) {
  if (option == 15) { // PR_SET_NAME: the calling thread's
    va_list ap;
    va_start(ap, option);
    const char *name = va_arg(ap, const char *);
    va_end(ap);
    pthr_set_name(pthread_self(), name);
  }
  return 0;
}
long ptrace_fake(int req, ...) { (void)req; return fail(1); }
long process_vm_readv_fake(int pid, const void *l, unsigned long ln, const void *r,
                           unsigned long rn, unsigned long flags) {
  (void)pid; (void)l; (void)ln; (void)r; (void)rn; (void)flags;
  return fail(L_ENOSYS);
}
const char *getprogname_fake(void) { return "geometrydash"; }
int getpagesize_fake(void) { return 0x1000; }

void arc4random_buf_fake(void *buf, size_t n) { randomGet(buf, n); }
uint32_t arc4random_fake(void) {
  uint32_t v;
  randomGet(&v, sizeof(v));
  return v;
}
uint32_t arc4random_uniform_fake(uint32_t bound) {
  if (bound < 2)
    return 0;
  const uint32_t min = -bound % bound;
  uint32_t r;
  do
    r = arc4random_fake();
  while (r < min);
  return r % bound;
}
int getentropy_fake(void *buf, size_t n) {
  if (n > 256)
    return fail(5); // EIO
  randomGet(buf, n);
  return 0;
}

time_t timegm_fake(struct tm *tm) {
  // days from civil (Howard Hinnant), no time zone involved
  int y = tm->tm_year + 1900;
  const int m = tm->tm_mon + 1;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm->tm_mday - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + (long)doe - 719468;
  return (time_t)days * 86400 + tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec;
}

// ---------------------------------------------------------------------------
// things Horizon has no equivalent of: callers have fallbacks
// ---------------------------------------------------------------------------

// epoll and pipe2: net_shim.c (on sockets)
int eventfd_fake(unsigned initval, int flags) { (void)initval; (void)flags; return fail(L_ENOSYS); }
int accept4_fake(int fd, void *addr, unsigned *len, int flags) {
  (void)flags;
  return accept_fake(fd, addr, len);
}
int mmsg_fake(int fd, void *v, unsigned n, int flags, ...) {
  (void)fd; (void)v; (void)n; (void)flags;
  return fail(L_ENOSYS);
}
long sendmsg_fake(int fd, const void *msg, int flags) {
  (void)fd; (void)msg; (void)flags;
  return fail(L_ENOSYS);
}
void *getservbyname_fake(const char *name, const char *proto) {
  (void)name; (void)proto;
  return NULL;
}
void *getservbyport_fake(int port, const char *proto) {
  (void)port; (void)proto;
  return NULL;
}
char *if_indextoname_fake(unsigned idx, char *buf) {
  if (idx != 1) {
    errno = 6; // ENXIO
    return NULL;
  }
  strcpy(buf, "wlan0");
  return buf;
}

// ---------------------------------------------------------------------------
// FILE* functions mods use beyond the game's set: bionic's stdin/stdout/
// stderr are the fake __sF slots (libc_shim.c), which newlib must never see
// ---------------------------------------------------------------------------

static int is_fake(FILE *f) {
  return (uint8_t *)f >= &fake_sF[0][0] && (uint8_t *)f < &fake_sF[0][0] + sizeof(fake_sF);
}

void clearerr_fake(FILE *f) { if (!is_fake(f)) clearerr(f); }
int fgetc_fake(FILE *f) { return is_fake(f) ? EOF : fgetc(f); }
int fgetpos_fake(FILE *f, fpos_t *pos) { return is_fake(f) ? -1 : fgetpos(f, pos); }
int fsetpos_fake(FILE *f, const fpos_t *pos) { return is_fake(f) ? -1 : fsetpos(f, pos); }
void rewind_fake(FILE *f) { if (!is_fake(f)) rewind(f); }
wint_t fgetwc_fake(FILE *f) { return is_fake(f) ? WEOF : fgetwc(f); }
wchar_t *fgetws_fake(wchar_t *s, int n, FILE *f) { return is_fake(f) ? NULL : fgetws(s, n, f); }
int fputws_fake(const wchar_t *s, FILE *f) { return is_fake(f) ? 0 : fputws(s, f); }
int putc_unlocked_fake(int c, FILE *f) { return is_fake(f) ? c : putc_unlocked(c, f); }
int getc_unlocked_fake(FILE *f) { return is_fake(f) ? EOF : getc_unlocked(f); }
int feof_unlocked_fake(FILE *f) { return is_fake(f) ? 1 : feof(f); }
int ferror_unlocked_fake(FILE *f) { return is_fake(f) ? 0 : ferror(f); }
void setbuffer_fake(FILE *f, char *buf, size_t size) { if (!is_fake(f)) setvbuf(f, buf, buf ? _IOFBF : _IONBF, size); }
void setlinebuf_fake(FILE *f) { if (!is_fake(f)) setvbuf(f, NULL, _IOLBF, 0); }
FILE *freopen_fake(const char *path, const char *mode, FILE *f) {
  if (is_fake(f))
    return f; // redirecting stdout/stderr to a file: keep the sink
  const char *p = path ? fix_path(path) : NULL;
  if (p && mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+')) &&
      !sandbox_check(p, "freopen for writing"))
    return NULL;
  return freopen(p, mode, f);
}

// ---------------------------------------------------------------------------
// temporary files: in <port folder>/internal/tmp, never the card's /tmp
// (newlib's default). Emptied at every start (tmpfile()'s files cannot be
// deleted while open on Horizon).
// ---------------------------------------------------------------------------

static void temp_fill(char *x, size_t n) {
  static const char chars[] = "abcdefghijklmnopqrstuvwxyz0123456789";
  uint8_t r[32];
  randomGet(r, n < sizeof(r) ? n : sizeof(r));
  for (size_t i = 0; i < n; i++)
    x[i] = chars[r[i % sizeof(r)] % (sizeof(chars) - 1)];
}

// mkstemp and co.: the XXXXXX before a suffix of slen characters, a new
// file opened read-write (sandboxed by open_fake)
static int temp_open(char *tmpl, int slen, int flags) {
  const size_t len = strlen(tmpl);
  if (slen < 0 || len < 6 + (size_t)slen || memcmp(tmpl + len - slen - 6, "XXXXXX", 6) != 0) {
    errno = L_EINVAL;
    return -1;
  }
  for (int attempt = 0; attempt < 100; attempt++) {
    temp_fill(tmpl + len - slen - 6, 6);
    // O_RDWR | O_CREAT | O_EXCL, plus the caller's (O_APPEND, O_CLOEXEC)
    const int fd = open_fake(tmpl, 2 | 0100 | 0200 | (flags & ~3), 0600);
    if (fd >= 0 || errno != 17) // EEXIST: another name
      return fd;
  }
  return -1;
}

int mkstemp_fake(char *tmpl) { return temp_open(tmpl, 0, 0); }
int mkostemp_fake(char *tmpl, int flags) { return temp_open(tmpl, 0, flags); }
int mkstemps_fake(char *tmpl, int slen) { return temp_open(tmpl, slen, 0); }
int mkostemps_fake(char *tmpl, int slen, int flags) { return temp_open(tmpl, slen, flags); }

FILE *tmpfile_fake(void) {
  char name[600];
  snprintf(name, sizeof(name), "%s/tmpXXXXXX", path_tmp());
  const int fd = temp_open(name, 0, 0);
  if (fd < 0)
    return NULL;
  FILE *f = fdopen(fd, "w+b");
  if (!f)
    close(fd);
  return f;
}

// names only (nothing created): in the port's temp folder whatever was asked
char *tmpnam_fake(char *s) {
  static char buf[600];
  snprintf(buf, sizeof(buf), "%s/tmpXXXXXX", path_tmp());
  temp_fill(buf + strlen(buf) - 6, 6);
  if (s) {
    strcpy(s, buf); // bionic's L_tmpnam is 4096
    return s;
  }
  return buf;
}

char *tempnam_fake(const char *dir, const char *pfx) {
  (void)dir;
  char buf[600];
  snprintf(buf, sizeof(buf), "%s/%.5sXXXXXX", path_tmp(), pfx ? pfx : "tmp");
  temp_fill(buf + strlen(buf) - 6, 6);
  return strdup(buf);
}

void temp_dir_reset(void) {
  mkdir(path_internal(), 0777);
  mkdir(path_tmp(), 0777);
  DIR *d = opendir(path_tmp());
  if (d) {
    struct dirent *e;
    char p[700];
    while ((e = readdir(d)) != NULL) {
      if (e->d_type == DT_DIR)
        continue;
      snprintf(p, sizeof(p), "%s/%s", path_tmp(), e->d_name);
      if (sandbox_allows(p))
        unlink(p);
    }
    closedir(d);
  }
  setenv("TMPDIR", path_tmp(), 1); // libc++'s temp_directory_path, and others
}
int vfscanf_fake(FILE *f, const char *fmt, va_list va) { return is_fake(f) ? EOF : vfscanf(f, fmt, va); }
int fscanf_fake(FILE *f, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = vfscanf_fake(f, fmt, va);
  va_end(va);
  return r;
}
int vfwprintf_fake(FILE *f, const wchar_t *fmt, va_list va) { return is_fake(f) ? 0 : vfwprintf(f, fmt, va); }
int fwprintf_fake(FILE *f, const wchar_t *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = vfwprintf_fake(f, fmt, va);
  va_end(va);
  return r;
}
int vfwscanf_fake(FILE *f, const wchar_t *fmt, va_list va) { return is_fake(f) ? EOF : vfwscanf(f, fmt, va); }
int fwscanf_fake(FILE *f, const wchar_t *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  int r = vfwscanf_fake(f, fmt, va);
  va_end(va);
  return r;
}

// stdout/stdin themselves: output is dropped (logcat-style printing is what
// __android_log is for), input is always at end of file
int puts_fake(const char *s) { (void)s; return 1; }
int putchar_fake(int c) { return c; }
int vprintf_fake(const char *fmt, va_list va) { (void)fmt; (void)va; return 0; }
int wprintf_fake(const wchar_t *fmt, ...) { (void)fmt; return 0; }
int vwprintf_fake(const wchar_t *fmt, va_list va) { (void)fmt; (void)va; return 0; }
wint_t putwchar_fake(wchar_t c) { return (wint_t)c; }
int getchar_fake(void) { return EOF; }
wint_t getwchar_fake(void) { return WEOF; }
int scanf_fake(const char *fmt, ...) { (void)fmt; return EOF; }
int vscanf_fake(const char *fmt, va_list va) { (void)fmt; (void)va; return EOF; }
void perror_fake(const char *s) { log_printf("[perror] %s: %s", s ? s : "", strerror(errno)); }
void psignal_fake(int sig, const char *s) { (void)sig; (void)s; }

// ---------------------------------------------------------------------------
// more fortify, process stubs, locale
// ---------------------------------------------------------------------------

int __snprintf_chk_fake(char *buf, size_t maxlen, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va;
  va_start(va, fmt);
  int r = vsnprintf(buf, maxlen, fmt, va);
  va_end(va);
  return r;
}
int __sprintf_chk_fake(char *buf, int flag, size_t slen, const char *fmt, ...) {
  (void)flag; (void)slen;
  va_list va;
  va_start(va, fmt);
  int r = vsprintf(buf, fmt, va);
  va_end(va);
  return r;
}
char *__stpcpy_chk_fake(char *dst, const char *src, size_t dstlen) { (void)dstlen; return stpcpy(dst, src); }
char *__stpncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen) { (void)dstlen; return stpncpy(dst, src, n); }
char *__stpncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl;
  return stpncpy(dst, src, n);
}
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dl, size_t sl) {
  (void)dl; (void)sl;
  return strncpy(dst, src, n);
}
void __assert_fake(const char *file, int line, const char *msg) {
  log_printf("[assert] %s:%d: %s", file ? file : "?", line, msg ? msg : "?");
  abort_fake();
}
const char *__gnu_basename_fake(const char *path) {
  const char *s = strrchr(path, '/');
  return s ? s + 1 : path;
}
char *strerror_l_fake(int err, void *loc) { (void)loc; return strerror(err); }
void *duplocale_fake(void *loc) { (void)loc; return newlocale_fake(0, "C", NULL); }

int system_fake(const char *cmd) { (void)cmd; return -1; }
int execve_fake(const char *path, char *const argv[], char *const envp[]) {
  (void)path; (void)argv; (void)envp;
  return fail(L_ENOSYS);
}
int execvp_fake(const char *file, char *const argv[]) {
  (void)file; (void)argv;
  return fail(L_ENOSYS);
}
int wait_fake(int *status) { (void)status; return fail(10); } // ECHILD
long times_fake(void *buf) {
  if (buf)
    memset(buf, 0, 32);
  return (long)(armGetSystemTick() / (armGetSystemTickFreq() / 100));
}
int at_quick_exit_fake(void (*fn)(void)) { (void)fn; return 0; }
