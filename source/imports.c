/* imports.c -- the "system libraries" of the game's modules
 *
 * Every import of libunity, libil2cpp, Wwise, the Sakasho SDK and the rest
 * that is not another module's export is bound here (with gl_table.c,
 * libc_table.c, android_ndk.c and opensles.c). GL goes to the native
 * mesa/nouveau drivers, sockets through the bionic<->BSD converters,
 * threads/fs through the bionic shims (the files dir's asset cache through
 * vfs.c), and the rest to newlib.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <ctype.h>
#include <wchar.h>
#include <wctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <locale.h>
#include <errno.h>
#include <pthread.h>
#include <dirent.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <malloc.h>
#include <switch.h>

#include "config.h"
#include "paths.h"
#include "so_util.h"
#include "util.h"
#include "libc_shim.h"
#include "net_shim.h"
#include "pthr.h"
#include "imports.h"
#include "bionic_extra.h"
#include "log.h"

// crt/newlib-provided symbols we forward by address
extern void __stack_chk_fail(void);

// newlib's setjmp/longjmp are real functions; grab their addresses without
// tripping over the setjmp.h macros. sigsetjmp/siglongjmp bind to the same
// entry points: the extra savemask argument lands in a register setjmp
// ignores, and the buffer (bionic-sized, 256 B) is larger than newlib's.
extern int newlib_setjmp(void *env) asm("setjmp");
extern void newlib_longjmp(void *env, int val) asm("longjmp");

// ---------------------------------------------------------------------------
// small local shims
// ---------------------------------------------------------------------------


// bionic clock(): microseconds of process time (CLOCKS_PER_SEC = 1000000)
static long clock_fake(void) {
  static u64 freq = 0;
  if (!freq)
    freq = armGetSystemTickFreq();
  return (long)(armGetSystemTick() / (freq / 1000000ull));
}

static int getpwuid_r_fake(unsigned uid, void *pwd, char *buf, size_t buflen, void **result) {
  (void)uid; (void)pwd; (void)buf; (void)buflen;
  if (result)
    *result = NULL;
  return 0; // "not found"
}

// Static destructors of the loaded modules. newlib's __cxa_atexit refuses
// these registrations (-1), and OpenSSL treats a failed atexit() as a failed
// OPENSSL_init_crypto: Geode's hashing and TLS stopped working. The port
// leaves through __libnx_exit and never runs module destructors, so a
// registration only has to succeed.
static int cxa_atexit_fake(void (*fn)(void *), void *arg, void *dso) {
  (void)fn; (void)arg; (void)dso;
  return 0;
}

// C++ static-local init guards (libfmod imports these from libstdc++.so)
static Mutex cxa_guard_lock;

static int __cxa_guard_acquire_fake(uint64_t *guard) {
  mutexLock(&cxa_guard_lock);
  if (*(uint8_t *)guard) {
    mutexUnlock(&cxa_guard_lock);
    return 0; // already initialized
  }
  return 1; // caller runs the initializer, then calls release
}

static void __cxa_guard_release_fake(uint64_t *guard) {
  *(uint8_t *)guard = 1;
  mutexUnlock(&cxa_guard_lock);
}

static void __cxa_pure_virtual_fake(void) {
  abort();
}

static void operator_delete_fake(void *p) {
  free(p);
}

// the game's stack-guard cookie (both the global and the TLS slot land here)
static uintptr_t fake_stack_chk_guard = 0x4242424242424242ull;

// bionic's _ctype_ data symbol: 1 + 256 char-class table, indexed _ctype_[c+1]
// (index 0 covers EOF). Filled in update_imports() from the C-locale classes.
unsigned char bionic_ctype_table[1 + 256];

static void fill_ctype_table(void) {
  for (int c = 0; c < 256; c++) {
    unsigned char b = 0;
    if (isupper(c))  b |= 0x01; // _U
    if (islower(c))  b |= 0x02; // _L
    if (isdigit(c))  b |= 0x04; // _N
    if (isspace(c))  b |= 0x08; // _S
    if (ispunct(c))  b |= 0x10; // _P
    if (iscntrl(c))  b |= 0x20; // _C
    if (isxdigit(c)) b |= 0x40; // _X
    if (c == ' ')    b |= 0x80; // _B
    bionic_ctype_table[c + 1] = b;
  }
  bionic_ctype_table[0] = 0;
}

// --- shims for symbols newlib lacks (used by the newer SubZero 2.2.147 NDK) --

static int sched_yield_fake(void) {
  svcSleepThread(0);
  return 0;
}

static int __signbit_fake(double x) {
  return signbit(x) ? 1 : 0;
}

// thread-local destructor registration: we don't run TLS dtors, so drop it
static int __cxa_thread_atexit_impl_fake(void (*func)(void *), void *obj, void *dso) {
  (void)func; (void)obj; (void)dso;
  return 0;
}

// ---------------------------------------------------------------------------
// import table
// ---------------------------------------------------------------------------

DynLibFunction dynlib_functions[] = {
  // --- runtime / bionic ------------------------------------------------------
  { "__android_log_print", (uintptr_t)&android_log_print_fake },
  { "__android_log_write", (uintptr_t)&android_log_write_fake2 },
  { "__android_log_vprint", (uintptr_t)&android_log_vprint_fake },
  { "__android_log_buf_print", (uintptr_t)&android_log_buf_print_fake },
  { "__assert2", (uintptr_t)&__assert2_fake },
  { "__ctype_get_mb_cur_max", (uintptr_t)&__ctype_get_mb_cur_max_fake },
  { "__cxa_atexit", (uintptr_t)&cxa_atexit_fake },
  { "atexit", (uintptr_t)&cxa_atexit_fake }, // only module code calls this one
  { "__cxa_finalize", (uintptr_t)&ret0 },
  { "__errno", (uintptr_t)&__errno },
  { "__fpclassifyd", (uintptr_t)&__fpclassifyd_fake },
  { "__isnanf", (uintptr_t)&__isnanf_fake },
  { "__google_potentially_blocking_region_begin", (uintptr_t)&ret0 },
  { "__google_potentially_blocking_region_end", (uintptr_t)&ret0 },
  { "__sF", (uintptr_t)&fake_sF },
  { "__stack_chk_fail", (uintptr_t)&__stack_chk_fail },
  { "__stack_chk_guard", (uintptr_t)&fake_stack_chk_guard },
  { "__FD_SET_chk", (uintptr_t)&__FD_SET_chk_fake },
  { "_ctype_", (uintptr_t)&bionic_ctype_table[0] },
  { "getauxval", (uintptr_t)&getauxval_fake },
  { "gettid", (uintptr_t)&gettid_fake },
  { "syscall", (uintptr_t)&syscall_fake },
  { "sysconf", (uintptr_t)&sysconf_fake },
  { "syslog", (uintptr_t)&ret0 },
  { "android_set_abort_message", (uintptr_t)&android_set_abort_message_fake },
  { "setpriority", (uintptr_t)&setpriority_fake },
  { "basename", (uintptr_t)&basename_fake },
  { "dl_iterate_phdr", (uintptr_t)&so_dl_iterate_phdr },
  { "dlopen", (uintptr_t)&so_dlopen },
  { "android_dlopen_ext", (uintptr_t)&so_dlopen },
  { "dlsym", (uintptr_t)&so_dlsym },
  { "dlvsym", (uintptr_t)&so_dlsym },
  { "dlclose", (uintptr_t)&so_dlclose },
  { "dlerror", (uintptr_t)&so_dlerror },
  { "dladdr", (uintptr_t)&so_dladdr },

  // --- process / signals (stubs) --------------------------------------------
  { "getpid", (uintptr_t)&getpid_fake },
  { "fork", (uintptr_t)&fork_fake },
  { "execl", (uintptr_t)&execl_fake },
  { "waitpid", (uintptr_t)&waitpid_fake },
  { "kill", (uintptr_t)&kill_fake },
  { "alarm", (uintptr_t)&alarm_fake },
  { "signal", (uintptr_t)&signal_fake },
  { "sigaction", (uintptr_t)&sigaction_fake },
  { "sigprocmask", (uintptr_t)&sigprocmask_fake },
  { "sigemptyset", (uintptr_t)&ret0 },
  { "sigfillset", (uintptr_t)&ret0 },
  { "sigaddset", (uintptr_t)&ret0 },
  { "sigdelset", (uintptr_t)&ret0 },
  { "getuid", (uintptr_t)&ret0 },
  { "geteuid", (uintptr_t)&ret0 },
  { "getgid", (uintptr_t)&ret0 },
  { "getegid", (uintptr_t)&ret0 },
  { "setuid", (uintptr_t)&ret0 },
  { "setgid", (uintptr_t)&ret0 },
  { "umask", (uintptr_t)&ret0 },
  { "initgroups", (uintptr_t)&ret0 },
  { "getpwuid_r", (uintptr_t)&getpwuid_r_fake },
  { "tcgetattr", (uintptr_t)&retm1 },
  { "tcsetattr", (uintptr_t)&retm1 },

  // --- setjmp ----------------------------------------------------------------
  { "setjmp", (uintptr_t)&newlib_setjmp },
  { "longjmp", (uintptr_t)&newlib_longjmp },
  { "sigsetjmp", (uintptr_t)&newlib_setjmp },
  { "siglongjmp", (uintptr_t)&newlib_longjmp },

  // --- math ------------------------------------------------------------------
  { "acos", (uintptr_t)&acos },
  { "acosf", (uintptr_t)&acosf },
  { "asinf", (uintptr_t)&asinf },
  { "asinh", (uintptr_t)&asinh },
  { "atan2", (uintptr_t)&atan2 },
  { "atan2f", (uintptr_t)&atan2f },
  { "atanf", (uintptr_t)&atanf },
  { "cos", (uintptr_t)&cos },
  { "cosf", (uintptr_t)&cosf },
  { "exp", (uintptr_t)&exp },
  { "exp2f", (uintptr_t)&exp2f },
  { "expf", (uintptr_t)&expf },
  { "fmax", (uintptr_t)&fmax },
  { "fmin", (uintptr_t)&fmin },
  { "fmod", (uintptr_t)&fmod },
  { "fmodf", (uintptr_t)&fmodf },
  { "frexp", (uintptr_t)&frexp },
  { "ldexp", (uintptr_t)&ldexp },
  { "ldexpf", (uintptr_t)&ldexpf },
  { "log", (uintptr_t)&log },
  { "log10", (uintptr_t)&log10 },
  { "log10f", (uintptr_t)&log10f },
  { "logf", (uintptr_t)&logf },
  { "lround", (uintptr_t)&lround },
  { "modf", (uintptr_t)&modf },
  { "pow", (uintptr_t)&pow },
  { "powf", (uintptr_t)&powf },
  { "sin", (uintptr_t)&sin },
  { "sincos", (uintptr_t)&sincos_fake },
  { "sincosf", (uintptr_t)&sincosf_fake },
  { "sinf", (uintptr_t)&sinf },
  { "sqrt", (uintptr_t)&sqrt },
  { "sqrtf", (uintptr_t)&sqrtf },
  { "tanf", (uintptr_t)&tanf },
  { "tanh", (uintptr_t)&tanh },

  // --- memory / stdlib --------------------------------------------------------
  { "abort", (uintptr_t)&abort_fake },
  { "atof", (uintptr_t)&atof },
  { "atoi", (uintptr_t)&atoi },
  { "bsearch", (uintptr_t)&bsearch },
  { "calloc", (uintptr_t)&calloc },
  { "exit", (uintptr_t)&exit_fake },
  { "free", (uintptr_t)&free },
  { "getenv", (uintptr_t)&getenv },
  { "malloc", (uintptr_t)&malloc },
  { "memchr", (uintptr_t)&memchr },
  { "memcmp", (uintptr_t)&memcmp },
  { "memcpy", (uintptr_t)&memcpy },
  { "memmove", (uintptr_t)&memmove },
  { "memrchr", (uintptr_t)&memrchr_fake },
  { "memset", (uintptr_t)&memset },
  { "qsort", (uintptr_t)&qsort },
  { "rand", (uintptr_t)&rand },
  { "realloc", (uintptr_t)&realloc },
  { "srand", (uintptr_t)&srand },
  { "strtod", (uintptr_t)&strtod },
  { "strtof", (uintptr_t)&strtof },
  { "strtol", (uintptr_t)&strtol },
  { "strtold", (uintptr_t)&strtold },
  { "strtoll", (uintptr_t)&strtoll },
  { "strtoul", (uintptr_t)&strtoul },
  { "strtoull", (uintptr_t)&strtoull },

  // --- strings ----------------------------------------------------------------
  { "strcat", (uintptr_t)&strcat },
  { "strcasecmp", (uintptr_t)&strcasecmp },
  { "strchr", (uintptr_t)&strchr },
  { "strcmp", (uintptr_t)&strcmp },
  { "strcoll", (uintptr_t)&strcoll },
  { "strcpy", (uintptr_t)&strcpy },
  { "strcspn", (uintptr_t)&strcspn },
  { "strdup", (uintptr_t)&strdup },
  { "strerror", (uintptr_t)&strerror },
  { "strerror_r", (uintptr_t)&strerror_r_fake },
  { "__gnu_strerror_r", (uintptr_t)&gnu_strerror_r_fake },
  { "strftime", (uintptr_t)&strftime },
  { "strlen", (uintptr_t)&strlen },
  { "strncasecmp", (uintptr_t)&strncasecmp },
  { "strncat", (uintptr_t)&strncat },
  { "strncmp", (uintptr_t)&strncmp },
  { "strncpy", (uintptr_t)&strncpy },
  { "strpbrk", (uintptr_t)&strpbrk },
  { "strrchr", (uintptr_t)&strrchr },
  { "strspn", (uintptr_t)&strspn },
  { "strstr", (uintptr_t)&strstr },
  { "strtok", (uintptr_t)&strtok },
  { "strtok_r", (uintptr_t)&strtok_r },
  { "strxfrm", (uintptr_t)&strxfrm },

  // --- ctype / wide -------------------------------------------------------------
  { "isalnum", (uintptr_t)&isalnum },
  { "isalpha", (uintptr_t)&isalpha },
  { "iscntrl", (uintptr_t)&iscntrl },
  { "isgraph", (uintptr_t)&isgraph },
  { "islower", (uintptr_t)&islower },
  { "isprint", (uintptr_t)&isprint },
  { "ispunct", (uintptr_t)&ispunct },
  { "isspace", (uintptr_t)&isspace },
  { "isupper", (uintptr_t)&isupper },
  { "isxdigit", (uintptr_t)&isxdigit },
  { "tolower", (uintptr_t)&tolower },
  { "toupper", (uintptr_t)&toupper },
  { "towlower", (uintptr_t)&towlower },
  { "towupper", (uintptr_t)&towupper },
  { "iswctype", (uintptr_t)&iswctype },
  { "wctype", (uintptr_t)&wctype },
  { "btowc", (uintptr_t)&btowc },
  { "wctob", (uintptr_t)&wctob },
  { "mbrtowc", (uintptr_t)&mbrtowc },
  { "wcrtomb", (uintptr_t)&wcrtomb },
  { "wcscoll", (uintptr_t)&wcscoll },
  { "wcsftime", (uintptr_t)&wcsftime },
  { "wcslen", (uintptr_t)&wcslen },
  { "wcsxfrm", (uintptr_t)&wcsxfrm },
  { "wmemchr", (uintptr_t)&wmemchr },
  { "wmemcmp", (uintptr_t)&wmemcmp },
  { "wmemcpy", (uintptr_t)&wmemcpy },
  { "wmemmove", (uintptr_t)&wmemmove },
  { "wmemset", (uintptr_t)&wmemset },
  { "setlocale", (uintptr_t)&setlocale },
  { "localeconv", (uintptr_t)&localeconv_fake },

  // --- stdio ----------------------------------------------------------------
  { "fclose", (uintptr_t)&fclose_fake },
  { "fdopen", (uintptr_t)&fdopen },
  { "feof", (uintptr_t)&feof_fake },
  { "ferror", (uintptr_t)&ferror_fake },
  { "fflush", (uintptr_t)&fflush_fake },
  { "fgets", (uintptr_t)&fgets_fake },
  { "fileno", (uintptr_t)&fileno_fake },
  { "fopen", (uintptr_t)&fopen_fake },
  { "fprintf", (uintptr_t)&fprintf_fake },
  { "fputc", (uintptr_t)&fputc_fake },
  { "fputs", (uintptr_t)&fputs_fake },
  { "fread", (uintptr_t)&fread_fake },
  { "fseek", (uintptr_t)&fseek_fake },
  { "ftell", (uintptr_t)&ftell_fake },
  { "fwrite", (uintptr_t)&fwrite_fake },
  { "getc", (uintptr_t)&getc_fake },
  { "getwc", (uintptr_t)&getwc_fake },
  { "printf", (uintptr_t)&ret0 },
  { "putc", (uintptr_t)&fputc_fake },
  { "putwc", (uintptr_t)&putwc_fake },
  { "setbuf", (uintptr_t)&setbuf_fake },
  { "setvbuf", (uintptr_t)&setvbuf_fake },
  { "snprintf", (uintptr_t)&snprintf },
  { "sprintf", (uintptr_t)&sprintf },
  { "sscanf", (uintptr_t)&sscanf },
  { "swprintf", (uintptr_t)&swprintf },
  { "ungetc", (uintptr_t)&ungetc_fake },
  { "ungetwc", (uintptr_t)&ungetwc_fake },
  { "vfprintf", (uintptr_t)&vfprintf_fake },
  { "vsnprintf", (uintptr_t)&vsnprintf },
  { "vsprintf", (uintptr_t)&vsprintf },

  // --- fs / unix io -----------------------------------------------------------
  { "access", (uintptr_t)&access_fake },
  { "chmod", (uintptr_t)&chmod_fake },
  { "close", (uintptr_t)&close_fake },
  { "closedir", (uintptr_t)&closedir_fake },
  { "lseek", (uintptr_t)&lseek_fake },
  { "lseek64", (uintptr_t)&lseek_fake }, // off_t is 64-bit on arm64
  { "mkdir", (uintptr_t)&mkdir_fake },
  { "open", (uintptr_t)&open_fake },
  { "__open_2", (uintptr_t)&open2_fake },
  { "opendir", (uintptr_t)&opendir_fake },
  { "read", (uintptr_t)&read_fake },
  { "readdir", (uintptr_t)&readdir_fake },
  { "remove", (uintptr_t)&remove_fake },
  { "rename", (uintptr_t)&rename_fake },
  { "stat", (uintptr_t)&stat_fake },
  { "fstat", (uintptr_t)&fstat_fake },
  { "lstat", (uintptr_t)&lstat_fake },
  { "write", (uintptr_t)&write_fake },
  { "writev", (uintptr_t)&writev_fake },
  { "mmap", (uintptr_t)&mmap_fake },
  { "munmap", (uintptr_t)&munmap_fake },
  { "mprotect", (uintptr_t)&mprotect_fake },
  { "madvise", (uintptr_t)&madvise_fake },
  { "mlock", (uintptr_t)&ret0 },
  { "__read_chk", (uintptr_t)&__read_chk_fake },

  // --- time / sched -----------------------------------------------------------
  { "clock", (uintptr_t)&clock_fake },
  { "clock_gettime", (uintptr_t)&clock_gettime_fake },
  { "difftime", (uintptr_t)&difftime },
  { "gettimeofday", (uintptr_t)&gettimeofday },
  { "gmtime", (uintptr_t)&gmtime },
  { "gmtime_r", (uintptr_t)&gmtime_r },
  { "localtime", (uintptr_t)&localtime },
  { "nanosleep", (uintptr_t)&nanosleep },
  { "time", (uintptr_t)&time },
  { "usleep", (uintptr_t)&usleep },

  // --- sockets (bionic<->BSD conversion, net_shim.c) ---------------------------
  { "accept", (uintptr_t)&accept_fake },
  { "bind", (uintptr_t)&bind_fake },
  { "connect", (uintptr_t)&connect_fake },
  { "freeaddrinfo", (uintptr_t)&freeaddrinfo_fake },
  { "gai_strerror", (uintptr_t)&gai_strerror_fake },
  { "getaddrinfo", (uintptr_t)&getaddrinfo_fake },
  { "gethostbyname", (uintptr_t)&gethostbyname_fake },
  { "gethostname", (uintptr_t)&gethostname_fake },
  { "getnameinfo", (uintptr_t)&getnameinfo_fake },
  { "getpeername", (uintptr_t)&getpeername_fake },
  { "getsockname", (uintptr_t)&getsockname_fake },
  { "getsockopt", (uintptr_t)&getsockopt_fake },
  { "if_nametoindex", (uintptr_t)&if_nametoindex_fake },
  { "inet_ntop", (uintptr_t)&inet_ntop_fake },
  { "inet_pton", (uintptr_t)&inet_pton_fake },
  { "listen", (uintptr_t)&listen_fake },
  { "poll", (uintptr_t)&poll_fake },
  { "recv", (uintptr_t)&recv_fake },
  { "recvfrom", (uintptr_t)&recvfrom_fake },
  { "select", (uintptr_t)&select_fake },
  { "send", (uintptr_t)&send_fake },
  { "sendto", (uintptr_t)&sendto_fake },
  { "setsockopt", (uintptr_t)&setsockopt_fake },
  { "shutdown", (uintptr_t)&shutdown_fake },
  { "socket", (uintptr_t)&socket_fake },
  { "socketpair", (uintptr_t)&socketpair_fake },
  { "pipe", (uintptr_t)&pipe_fake },
  { "dup2", (uintptr_t)&dup2_fake },
  { "fcntl", (uintptr_t)&fcntl_fake },
  { "ioctl", (uintptr_t)&ioctl_fake },

  // --- pthread (bionic<->newlib wrappers in pthr.c) ----------------------------
  { "pthread_attr_destroy", (uintptr_t)&pthread_attr_destroy_soloader },
  { "pthread_attr_init", (uintptr_t)&pthread_attr_init_soloader },
  { "pthread_attr_setdetachstate", (uintptr_t)&pthread_attr_setdetachstate_soloader },
  { "pthread_attr_setstacksize", (uintptr_t)&pthread_attr_setstacksize_soloader },
  { "pthread_cond_broadcast", (uintptr_t)&pthread_cond_broadcast_soloader },
  { "pthread_cond_destroy", (uintptr_t)&pthread_cond_destroy_soloader },
  { "pthread_cond_init", (uintptr_t)&pthread_cond_init_soloader },
  { "pthread_cond_signal", (uintptr_t)&pthread_cond_signal_soloader },
  { "pthread_cond_timedwait", (uintptr_t)&pthread_cond_timedwait_soloader },
  { "pthread_cond_wait", (uintptr_t)&pthread_cond_wait_soloader },
  { "pthread_create", (uintptr_t)&pthread_create_soloader },
  { "pthread_detach", (uintptr_t)&pthread_detach_soloader },
  { "pthread_equal", (uintptr_t)&pthread_equal_soloader },
  { "pthread_exit", (uintptr_t)&pthread_exit },
  { "pthread_getschedparam", (uintptr_t)&pthread_getschedparam_soloader },
  { "pthread_getspecific", (uintptr_t)&pthread_getspecific_soloader },
  { "pthread_join", (uintptr_t)&pthread_join_soloader },
  { "pthread_key_create", (uintptr_t)&pthread_key_create_soloader },
  { "pthread_key_delete", (uintptr_t)&pthread_key_delete_soloader },
  { "pthread_mutex_destroy", (uintptr_t)&pthread_mutex_destroy_soloader },
  { "pthread_mutex_init", (uintptr_t)&pthread_mutex_init_soloader },
  { "pthread_mutex_lock", (uintptr_t)&pthread_mutex_lock_soloader },
  { "pthread_mutex_trylock", (uintptr_t)&pthread_mutex_trylock_soloader },
  { "pthread_mutex_unlock", (uintptr_t)&pthread_mutex_unlock_soloader },
  { "pthread_mutexattr_destroy", (uintptr_t)&pthread_mutexattr_destroy_soloader },
  { "pthread_mutexattr_init", (uintptr_t)&pthread_mutexattr_init_soloader },
  { "pthread_mutexattr_settype", (uintptr_t)&pthread_mutexattr_settype_soloader },
  { "pthread_once", (uintptr_t)&pthread_once_soloader },
  { "pthread_self", (uintptr_t)&pthread_self_soloader },
  { "pthread_setname_np", (uintptr_t)&pthread_setname_np_fake },
  { "pthread_setspecific", (uintptr_t)&pthread_setspecific_soloader },
  { "pthread_rwlock_init", (uintptr_t)&pthread_rwlock_init_fake },
  { "pthread_rwlock_destroy", (uintptr_t)&pthread_rwlock_destroy_fake },
  { "pthread_rwlock_rdlock", (uintptr_t)&pthread_rwlock_rdlock_fake },
  { "pthread_rwlock_wrlock", (uintptr_t)&pthread_rwlock_wrlock_fake },
  { "pthread_rwlock_unlock", (uintptr_t)&pthread_rwlock_unlock_fake },
  { "sem_destroy", (uintptr_t)&sem_destroy_fake },
  { "sem_init", (uintptr_t)&sem_init_fake },
  { "sem_post", (uintptr_t)&sem_post_fake },
  { "sem_trywait", (uintptr_t)&sem_trywait_fake },
  { "sem_wait", (uintptr_t)&sem_wait_fake },

  // --- newer-NDK libc imports ------------------------------------------------
  { "__cxa_thread_atexit_impl", (uintptr_t)&__cxa_thread_atexit_impl_fake },
  { "__signbit", (uintptr_t)&__signbit_fake },
  { "closelog", (uintptr_t)&ret0 },
  { "openlog", (uintptr_t)&ret0 },
  { "sched_yield", (uintptr_t)&sched_yield_fake },
  { "abs", (uintptr_t)&abs },
  { "asinhf", (uintptr_t)&asinhf },
  { "tanhf", (uintptr_t)&tanhf },
  { "isascii", (uintptr_t)&isascii },
  { "fseeko", (uintptr_t)&fseeko },
  { "ftello", (uintptr_t)&ftello },
  { "vasprintf", (uintptr_t)&vasprintf },
  { "vsscanf", (uintptr_t)&vsscanf },
  { "newlocale", (uintptr_t)&newlocale_fake },
  { "freelocale", (uintptr_t)&freelocale_fake },
  { "uselocale", (uintptr_t)&uselocale_fake },
  { "strtold_l", (uintptr_t)&strtold_l_fake },
  { "strtoll_l", (uintptr_t)&strtoll_l_fake },
  { "strtoull_l", (uintptr_t)&strtoull_l_fake },
  { "mbsnrtowcs", (uintptr_t)&mbsnrtowcs_fake },
  { "wcsnrtombs", (uintptr_t)&wcsnrtombs_fake },
  { "isdigit_l", (uintptr_t)&isdigit_l },
  { "islower_l", (uintptr_t)&islower_l },
  { "isupper_l", (uintptr_t)&isupper_l },
  { "isxdigit_l", (uintptr_t)&isxdigit_l },
  { "tolower_l", (uintptr_t)&tolower_l },
  { "toupper_l", (uintptr_t)&toupper_l },
  { "iswalpha_l", (uintptr_t)&iswalpha_l },
  { "iswblank_l", (uintptr_t)&iswblank_l },
  { "iswcntrl_l", (uintptr_t)&iswcntrl_l },
  { "iswdigit_l", (uintptr_t)&iswdigit_l },
  { "iswlower_l", (uintptr_t)&iswlower_l },
  { "iswprint_l", (uintptr_t)&iswprint_l },
  { "iswpunct_l", (uintptr_t)&iswpunct_l },
  { "iswspace_l", (uintptr_t)&iswspace_l },
  { "iswupper_l", (uintptr_t)&iswupper_l },
  { "iswxdigit_l", (uintptr_t)&iswxdigit_l },
  { "towlower_l", (uintptr_t)&towlower_l },
  { "towupper_l", (uintptr_t)&towupper_l },
  { "strcoll_l", (uintptr_t)&strcoll_l },
  { "strxfrm_l", (uintptr_t)&strxfrm_l },
  { "strftime_l", (uintptr_t)&strftime_l },
  { "wcscoll_l", (uintptr_t)&wcscoll_l },
  { "wcsxfrm_l", (uintptr_t)&wcsxfrm_l },
  { "mbrlen", (uintptr_t)&mbrlen },
  { "mbsrtowcs", (uintptr_t)&mbsrtowcs },
  { "mbtowc", (uintptr_t)&mbtowc },
  { "wcstod", (uintptr_t)&wcstod },
  { "wcstof", (uintptr_t)&wcstof },
  { "wcstol", (uintptr_t)&wcstol },
  { "wcstold", (uintptr_t)&wcstold },
  { "wcstoll", (uintptr_t)&wcstoll },
  { "wcstoul", (uintptr_t)&wcstoul },
  { "wcstoull", (uintptr_t)&wcstoull },

  // --- the rest of bionic (bionic_extra.c) ------------------------------------
  { "stdin", (uintptr_t)&bionic_stdin },
  { "stdout", (uintptr_t)&bionic_stdout },
  { "stderr", (uintptr_t)&bionic_stderr },
  { "__system_property_get", (uintptr_t)&__system_property_get_fake },

  // std::filesystem
  { "openat", (uintptr_t)&openat_fake },
  { "__openat_2", (uintptr_t)&openat_fake },
  { "fdopendir", (uintptr_t)&fdopendir_fake },
  { "dirfd", (uintptr_t)&dirfd_of_fake },
  { "unlinkat", (uintptr_t)&unlinkat_fake },
  { "fstatat", (uintptr_t)&fstatat_fake },
  { "fstatat64", (uintptr_t)&fstatat_fake },
  { "mkdirat", (uintptr_t)&mkdirat_fake },
  { "renameat", (uintptr_t)&renameat_fake },
  { "unlink", (uintptr_t)&unlink_fake },
  { "rmdir", (uintptr_t)&rmdir_fake },
  { "truncate", (uintptr_t)&truncate_fake },
  { "truncate64", (uintptr_t)&truncate_fake },
  { "ftruncate", (uintptr_t)&ftruncate_fake },
  { "ftruncate64", (uintptr_t)&ftruncate_fake },
  { "link", (uintptr_t)&link_fake },
  { "symlink", (uintptr_t)&symlink_fake },
  { "readlink", (uintptr_t)&readlink_fake },
  { "pathconf", (uintptr_t)&pathconf_fake },
  { "utimensat", (uintptr_t)&utimensat_fake },
  { "futimens", (uintptr_t)&ret0 },
  { "utime", (uintptr_t)&utime_fake },
  { "utimes", (uintptr_t)&ret0 },
  { "fchmod", (uintptr_t)&fchmod_fake },
  { "fchmodat", (uintptr_t)&fchmodat_fake },
  { "isatty", (uintptr_t)&isatty_fake },
  { "fsync", (uintptr_t)&fsync_fake },
  { "dup", (uintptr_t)&dup_fake },
  { "fdatasync", (uintptr_t)&fdatasync_fake },
  { "msync", (uintptr_t)&msync_fake },
  { "getcwd", (uintptr_t)&getcwd_fake },
  { "chdir", (uintptr_t)&chdir_fake },
  { "realpath", (uintptr_t)&realpath_full_fake },
  { "mkdtemp", (uintptr_t)&mkdtemp_fake },
  { "mkstemp", (uintptr_t)&mkstemp_fake },
  { "mkostemp", (uintptr_t)&mkostemp_fake },
  { "mkstemps", (uintptr_t)&mkstemps_fake },
  { "mkostemps", (uintptr_t)&mkostemps_fake },
  { "tmpfile", (uintptr_t)&tmpfile_fake },
  { "tmpnam", (uintptr_t)&tmpnam_fake },
  { "tempnam", (uintptr_t)&tempnam_fake },
  { "statvfs", (uintptr_t)&statvfs_fake },
  { "fstatvfs", (uintptr_t)&statvfs_fake }, // ignores its first argument
  { "sendfile", (uintptr_t)&sendfile_fake },
  { "sendfile64", (uintptr_t)&sendfile_fake },
  { "pread", (uintptr_t)&pread_fake },
  { "pread64", (uintptr_t)&pread_fake },
  { "pwrite", (uintptr_t)&pwrite_fake },
  { "pwrite64", (uintptr_t)&pwrite_fake },

  // memory
  { "posix_memalign", (uintptr_t)&posix_memalign_fake },
  { "memalign", (uintptr_t)&memalign },
  { "malloc_usable_size", (uintptr_t)&malloc_usable_size },

  // fortify
  { "__memcpy_chk", (uintptr_t)&__memcpy_chk_fake },
  { "__memmove_chk", (uintptr_t)&__memmove_chk_fake },
  { "__memset_chk", (uintptr_t)&__memset_chk_fake },
  { "__memchr_chk", (uintptr_t)&__memchr_chk_fake },
  { "__strcat_chk", (uintptr_t)&__strcat_chk_fake },
  { "__strchr_chk", (uintptr_t)&__strchr_chk_fake },
  { "__strrchr_chk", (uintptr_t)&__strrchr_chk_fake },
  { "__strcpy_chk", (uintptr_t)&__strcpy_chk_fake },
  { "__strlen_chk", (uintptr_t)&__strlen_chk_fake },
  { "__strncat_chk", (uintptr_t)&__strncat_chk_fake },
  { "__strncpy_chk", (uintptr_t)&__strncpy_chk_fake },
  { "__vsnprintf_chk", (uintptr_t)&__vsnprintf_chk_fake },
  { "__vsprintf_chk", (uintptr_t)&__vsprintf_chk_fake },
  { "__poll_chk", (uintptr_t)&__poll_chk_fake },
  { "__pread_chk", (uintptr_t)&__pread64_chk_fake },
  { "__pread64_chk", (uintptr_t)&__pread64_chk_fake },
  { "__readlink_chk", (uintptr_t)&__readlink_chk_fake },
  { "__FD_ISSET_chk", (uintptr_t)&__FD_ISSET_chk_fake },
  { "__cmsg_nxthdr", (uintptr_t)&__cmsg_nxthdr_fake },

  // stdio
  { "fputwc", (uintptr_t)&fputwc_fake },

  // process
  { "_Exit", (uintptr_t)&exit_now_fake },
  { "_exit", (uintptr_t)&exit_now_fake },
  { "raise", (uintptr_t)&raise_fake },
  { "__register_atfork", (uintptr_t)&register_atfork_fake },
  { "pthread_atfork", (uintptr_t)&register_atfork_fake },
  { "pthread_sigmask", (uintptr_t)&pthread_sigmask_fake },
  { "prctl", (uintptr_t)&prctl_fake },
  { "ptrace", (uintptr_t)&ptrace_fake },
  { "process_vm_readv", (uintptr_t)&process_vm_readv_fake },
  { "getprogname", (uintptr_t)&getprogname_fake },
  { "getpagesize", (uintptr_t)&getpagesize_fake },

  // randomness
  { "arc4random", (uintptr_t)&arc4random_fake },
  { "arc4random_buf", (uintptr_t)&arc4random_buf_fake },
  { "arc4random_uniform", (uintptr_t)&arc4random_uniform_fake },
  { "getentropy", (uintptr_t)&getentropy_fake },

  // time
  { "timegm", (uintptr_t)&timegm_fake },
  { "localtime_r", (uintptr_t)&localtime_r },
  { "mktime", (uintptr_t)&mktime },
  { "ctime", (uintptr_t)&ctime },
  { "ctime_r", (uintptr_t)&ctime_r },
  { "asctime", (uintptr_t)&asctime },
  { "strptime", (uintptr_t)&strptime },
  { "tzset", (uintptr_t)&tzset },

  // strings
  { "memmem", (uintptr_t)&memmem },
  { "strnlen", (uintptr_t)&strnlen },
  { "strndup", (uintptr_t)&strndup },
  { "strsep", (uintptr_t)&strsep },
  { "strlcpy", (uintptr_t)&strlcpy },
  { "strlcat", (uintptr_t)&strlcat },
  { "stpcpy", (uintptr_t)&stpcpy },
  { "strcasestr", (uintptr_t)&strcasestr },
  { "memccpy", (uintptr_t)&memccpy },
  { "wcscmp", (uintptr_t)&wcscmp },
  { "wcsncmp", (uintptr_t)&wcsncmp },
  { "wcschr", (uintptr_t)&wcschr },
  { "wcsrchr", (uintptr_t)&wcsrchr },
  { "wcscpy", (uintptr_t)&wcscpy },
  { "wcsncpy", (uintptr_t)&wcsncpy },
  { "wcstombs", (uintptr_t)&wcstombs },
  { "mbstowcs", (uintptr_t)&mbstowcs },
  { "wctomb", (uintptr_t)&wctomb },

  // no Horizon equivalent; curl and c-ares fall back without them
  { "epoll_create", (uintptr_t)&epoll_create1_fake },
  { "epoll_create1", (uintptr_t)&epoll_create1_fake },
  { "epoll_ctl", (uintptr_t)&epoll_ctl_fake },
  { "epoll_wait", (uintptr_t)&epoll_wait_fake },
  { "epoll_pwait", (uintptr_t)&epoll_wait_fake },
  { "eventfd", (uintptr_t)&eventfd_fake },
  { "pipe2", (uintptr_t)&pipe2_fake },
  { "accept4", (uintptr_t)&accept4_fake },
  { "recvmmsg", (uintptr_t)&mmsg_fake },
  { "sendmmsg", (uintptr_t)&mmsg_fake },
  { "sendmsg", (uintptr_t)&sendmsg_fake },
  { "recvmsg", (uintptr_t)&sendmsg_fake },
  { "getservbyname", (uintptr_t)&getservbyname_fake },
  { "getservbyport", (uintptr_t)&getservbyport_fake },
  { "if_indextoname", (uintptr_t)&if_indextoname_fake },

  // FILE*, stdio and process calls mods use (bionic_extra.c)
  { "clearerr", (uintptr_t)&clearerr_fake },
  { "clearerr_unlocked", (uintptr_t)&clearerr_fake },
  { "fgetc", (uintptr_t)&fgetc_fake },
  { "fgetpos", (uintptr_t)&fgetpos_fake },
  { "fsetpos", (uintptr_t)&fsetpos_fake },
  { "rewind", (uintptr_t)&rewind_fake },
  { "fgetwc", (uintptr_t)&fgetwc_fake },
  { "fgetws", (uintptr_t)&fgetws_fake },
  { "fputws", (uintptr_t)&fputws_fake },
  { "putc_unlocked", (uintptr_t)&putc_unlocked_fake },
  { "getc_unlocked", (uintptr_t)&getc_unlocked_fake },
  { "feof_unlocked", (uintptr_t)&feof_unlocked_fake },
  { "ferror_unlocked", (uintptr_t)&ferror_unlocked_fake },
  { "setbuffer", (uintptr_t)&setbuffer_fake },
  { "setlinebuf", (uintptr_t)&setlinebuf_fake },
  { "freopen", (uintptr_t)&freopen_fake },
  { "fscanf", (uintptr_t)&fscanf_fake },
  { "vfscanf", (uintptr_t)&vfscanf_fake },
  { "fwprintf", (uintptr_t)&fwprintf_fake },
  { "vfwprintf", (uintptr_t)&vfwprintf_fake },
  { "fwscanf", (uintptr_t)&fwscanf_fake },
  { "vfwscanf", (uintptr_t)&vfwscanf_fake },
  { "swscanf", (uintptr_t)&swscanf },
  { "vswscanf", (uintptr_t)&vswscanf },
  { "vswprintf", (uintptr_t)&vswprintf },
  { "puts", (uintptr_t)&puts_fake },
  { "putchar", (uintptr_t)&putchar_fake },
  { "putchar_unlocked", (uintptr_t)&putchar_fake },
  { "vprintf", (uintptr_t)&vprintf_fake },
  { "wprintf", (uintptr_t)&wprintf_fake },
  { "vwprintf", (uintptr_t)&vwprintf_fake },
  { "putwchar", (uintptr_t)&putwchar_fake },
  { "getchar", (uintptr_t)&getchar_fake },
  { "getchar_unlocked", (uintptr_t)&getchar_fake },
  { "getwchar", (uintptr_t)&getwchar_fake },
  { "scanf", (uintptr_t)&scanf_fake },
  { "vscanf", (uintptr_t)&vscanf_fake },
  { "wscanf", (uintptr_t)&scanf_fake },
  { "vwscanf", (uintptr_t)&vscanf_fake },
  { "perror", (uintptr_t)&perror_fake },
  { "psignal", (uintptr_t)&psignal_fake },
  { "__snprintf_chk", (uintptr_t)&__snprintf_chk_fake },
  { "__sprintf_chk", (uintptr_t)&__sprintf_chk_fake },
  { "__stpcpy_chk", (uintptr_t)&__stpcpy_chk_fake },
  { "__stpncpy_chk", (uintptr_t)&__stpncpy_chk_fake },
  { "__stpncpy_chk2", (uintptr_t)&__stpncpy_chk2_fake },
  { "__strncpy_chk2", (uintptr_t)&__strncpy_chk2_fake },
  { "__assert", (uintptr_t)&__assert_fake },
  { "__gnu_basename", (uintptr_t)&__gnu_basename_fake },
  { "strerror_l", (uintptr_t)&strerror_l_fake },
  { "duplocale", (uintptr_t)&duplocale_fake },
  { "system", (uintptr_t)&system_fake },
  { "execve", (uintptr_t)&execve_fake },
  { "execv", (uintptr_t)&execvp_fake },
  { "execvp", (uintptr_t)&execvp_fake },
  { "wait", (uintptr_t)&wait_fake },
  { "times", (uintptr_t)&times_fake },
  { "quick_exit", (uintptr_t)&exit_now_fake },
  { "at_quick_exit", (uintptr_t)&at_quick_exit_fake },
};

size_t dynlib_numfunctions = sizeof(dynlib_functions) / sizeof(*dynlib_functions);

// bionic's libstdc++.so: only modules that list it in DT_NEEDED (libms)
DynLibFunction dynlib_stdcxx[] = {
  { "__cxa_guard_acquire", (uintptr_t)&__cxa_guard_acquire_fake },
  { "__cxa_guard_release", (uintptr_t)&__cxa_guard_release_fake },
  { "__cxa_pure_virtual", (uintptr_t)&__cxa_pure_virtual_fake },
  { "_ZdlPv", (uintptr_t)&operator_delete_fake },
  { "_ZdaPv", (uintptr_t)&operator_delete_fake },
  { "_Znwm", (uintptr_t)&malloc },
  { "_Znam", (uintptr_t)&malloc },
};

size_t dynlib_numstdcxx = sizeof(dynlib_stdcxx) / sizeof(*dynlib_stdcxx);

void update_imports(void) {
  fill_ctype_table();
}
