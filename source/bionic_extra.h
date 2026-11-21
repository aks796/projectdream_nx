/* bionic_extra.h -- libc for Geode, libc++_shared and mods (bionic_extra.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __BIONIC_EXTRA_H__
#define __BIONIC_EXTRA_H__

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <wchar.h>

struct stat;
struct bionic_stat;
struct bionic_statvfs;

// directory file descriptors
int is_dirfd_fake(int fd);
void dirfd_close_fake(int fd);
int dirfd_stat_fake(int fd, struct stat *st);
int openat_fake(int dirfd, const char *name, int flags, ...);
void *fdopendir_fake(int fd);
int closedir_fake(void *dirp);
// temporary files, kept in <port folder>/internal/tmp
int mkstemp_fake(char *tmpl);
int mkostemp_fake(char *tmpl, int flags);
int mkstemps_fake(char *tmpl, int slen);
int mkostemps_fake(char *tmpl, int slen, int flags);
FILE *tmpfile_fake(void);
char *tmpnam_fake(char *s);
char *tempnam_fake(const char *dir, const char *pfx);
// at start: the temp folder made and emptied, TMPDIR pointed at it
void temp_dir_reset(void);
int dirfd_of_fake(void *dirp);
int unlinkat_fake(int dirfd, const char *name, int flags);
int fstatat_fake(int dirfd, const char *name, struct bionic_stat *st, int flags);
int faccessat_fake(int dirfd, const char *name, int mode);
int mkdirat_fake(int dirfd, const char *name, unsigned mode);
int renameat_fake(int olddirfd, const char *oldname, int newdirfd, const char *newname);

// paths
int unlink_fake(const char *path);
int rmdir_fake(const char *path);
int truncate_fake(const char *path, long len);
int ftruncate_fake(int fd, long len);
int link_fake(const char *a, const char *b);
int symlink_fake(const char *a, const char *b);
long readlink_fake(const char *path, char *buf, size_t len);
long pathconf_fake(const char *path, int name);
int utimensat_fake(int dirfd, const char *path, const void *times, int flags);
int utime_fake(const char *path, const void *times);
int fchmod_fake(int fd, unsigned mode);
int fchmodat_fake(int dirfd, const char *path, unsigned mode, int flags);
int isatty_fake(int fd);
int fdatasync_fake(int fd);
int msync_fake(void *addr, size_t len, int flags);
char *getcwd_fake(char *buf, size_t size);
char *realpath_full_fake(const char *path, char *resolved);
char *mkdtemp_fake(char *tmpl);
int statvfs_fake(const char *path, struct bionic_statvfs *out);
long sendfile_fake(int out_fd, int in_fd, int64_t *offset, size_t count);
long pread_fake(int fd, void *buf, size_t count, int64_t off);
long pwrite_fake(int fd, const void *buf, size_t count, int64_t off);

// fortify
void *__memchr_chk_fake(const void *s, int c, size_t n, size_t buflen);
int __poll_chk_fake(void *fds, unsigned nfds, int timeout, size_t fdslen);
long __pread64_chk_fake(int fd, void *buf, size_t count, int64_t off, size_t buflen);
long __readlink_chk_fake(const char *path, char *buf, size_t len, size_t buflen);
int __FD_ISSET_chk_fake(int fd, const void *set, size_t setlen);
void *__cmsg_nxthdr_fake(void *msg, void *cmsg);

// stdio
extern FILE *bionic_stdin, *bionic_stdout, *bionic_stderr;
wint_t fputwc_fake(wchar_t c, FILE *f);

// logging
int android_log_vprint_fake(int prio, const char *tag, const char *fmt, va_list va);
int android_log_print_fake(int prio, const char *tag, const char *fmt, ...);
int android_log_buf_print_fake(int buf, int prio, const char *tag, const char *fmt, ...);
int android_log_write_fake2(int prio, const char *tag, const char *msg);

// process, randomness, time
void exit_now_fake(int code);
int raise_fake(int sig);
int register_atfork_fake(void *a, void *b, void *c, void *d);
int pthread_sigmask_fake(int how, const void *set, void *old);
int prctl_fake(int option, ...);
long ptrace_fake(int req, ...);
long process_vm_readv_fake(int pid, const void *l, unsigned long ln, const void *r,
                           unsigned long rn, unsigned long flags);
const char *getprogname_fake(void);
int getpagesize_fake(void);
void arc4random_buf_fake(void *buf, size_t n);
uint32_t arc4random_fake(void);
uint32_t arc4random_uniform_fake(uint32_t bound);
int getentropy_fake(void *buf, size_t n);
time_t timegm_fake(struct tm *tm);

// unsupported (callers fall back)
int epoll_create1_fake(int flags);
int epoll_ctl_fake(int epfd, int op, int fd, void *ev);
int epoll_wait_fake(int epfd, void *ev, int max, int timeout);
int eventfd_fake(unsigned initval, int flags);
int pipe2_fake(int fds[2], int flags);
int accept4_fake(int fd, void *addr, unsigned *len, int flags);
int mmsg_fake(int fd, void *v, unsigned n, int flags, ...);
long sendmsg_fake(int fd, const void *msg, int flags);
void *getservbyname_fake(const char *name, const char *proto);
void *getservbyport_fake(int port, const char *proto);
char *if_indextoname_fake(unsigned idx, char *buf);

// FILE*, stdio, fortify, process (second set)
void clearerr_fake(FILE *f);
int fgetc_fake(FILE *f);
int fgetpos_fake(FILE *f, fpos_t *pos);
int fsetpos_fake(FILE *f, const fpos_t *pos);
void rewind_fake(FILE *f);
wint_t fgetwc_fake(FILE *f);
wchar_t *fgetws_fake(wchar_t *s, int n, FILE *f);
int fputws_fake(const wchar_t *s, FILE *f);
int putc_unlocked_fake(int c, FILE *f);
int getc_unlocked_fake(FILE *f);
int feof_unlocked_fake(FILE *f);
int ferror_unlocked_fake(FILE *f);
void setbuffer_fake(FILE *f, char *buf, size_t size);
void setlinebuf_fake(FILE *f);
FILE *freopen_fake(const char *path, const char *mode, FILE *f);
int vfscanf_fake(FILE *f, const char *fmt, va_list va);
int fscanf_fake(FILE *f, const char *fmt, ...);
int vfwprintf_fake(FILE *f, const wchar_t *fmt, va_list va);
int fwprintf_fake(FILE *f, const wchar_t *fmt, ...);
int vfwscanf_fake(FILE *f, const wchar_t *fmt, va_list va);
int fwscanf_fake(FILE *f, const wchar_t *fmt, ...);
int puts_fake(const char *s);
int putchar_fake(int c);
int vprintf_fake(const char *fmt, va_list va);
int wprintf_fake(const wchar_t *fmt, ...);
int vwprintf_fake(const wchar_t *fmt, va_list va);
wint_t putwchar_fake(wchar_t c);
int getchar_fake(void);
wint_t getwchar_fake(void);
int scanf_fake(const char *fmt, ...);
int vscanf_fake(const char *fmt, va_list va);
void perror_fake(const char *s);
void psignal_fake(int sig, const char *s);
int __snprintf_chk_fake(char *buf, size_t maxlen, int flag, size_t slen, const char *fmt, ...);
int __sprintf_chk_fake(char *buf, int flag, size_t slen, const char *fmt, ...);
char *__stpcpy_chk_fake(char *dst, const char *src, size_t dstlen);
char *__stpncpy_chk_fake(char *dst, const char *src, size_t n, size_t dstlen);
char *__stpncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dl, size_t sl);
char *__strncpy_chk2_fake(char *dst, const char *src, size_t n, size_t dl, size_t sl);
void __assert_fake(const char *file, int line, const char *msg);
const char *__gnu_basename_fake(const char *path);
char *strerror_l_fake(int err, void *loc);
void *duplocale_fake(void *loc);
int system_fake(const char *cmd);
int execve_fake(const char *path, char *const argv[], char *const envp[]);
int execvp_fake(const char *file, char *const argv[]);
int wait_fake(int *status);
long times_fake(void *buf);
int at_quick_exit_fake(void (*fn)(void));

int fsync_fake(int fd);

#endif
