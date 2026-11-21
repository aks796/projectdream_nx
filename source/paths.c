/* paths.c -- see paths.h.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/stat.h>

#include "config.h"
#include "apk.h"
#include "paths.h"

static char s_base[256], s_config[300], s_internal[300], s_lib[300], s_tmp[300];
static char s_data[420], s_files[440], s_cache[440], s_ext[420], s_obb[420], s_apk_alias[300];
static char s_apk[512];
static char s_pkg[96] = SDT_PACKAGE; // Project Dream's package

static void write_proc(void);

// strip a "device:" prefix and the trailing "/<file>" component
static void dir_of(const char *in, char *out, size_t n) {
  const char *colon = strchr(in, ':');
  snprintf(out, n, "%s", colon ? colon + 1 : in);
  char *slash = strrchr(out, '/');
  if (slash)
    *slash = '\0';
}

void paths_init(const char *argv0) {
  s_base[0] = '\0';
  if (argv0 && argv0[0])
    dir_of(argv0, s_base, sizeof s_base);
  if (!s_base[0]) {
    char cwd[256];
    if (getcwd(cwd, sizeof cwd)) {
      const char *colon = strchr(cwd, ':');
      snprintf(s_base, sizeof s_base, "%s", colon ? colon + 1 : cwd);
    }
  }
  // no argv[0] (an emulator starting the .nro directly) leaves the cwd, the
  // card root: use the default folder then
  if (!s_base[0] || !strcmp(s_base, "/"))
    snprintf(s_base, sizeof s_base, "/switch/" PORT_NAME);
  if (s_base[0] != '/') {
    char tmp[256];
    snprintf(tmp, sizeof tmp, "/%s", s_base);
    snprintf(s_base, sizeof s_base, "%s", tmp);
  }
  snprintf(s_config,   sizeof s_config,   "%s/%s", s_base, CONFIG_NAME);
  snprintf(s_internal, sizeof s_internal, "%s/internal", s_base);
  snprintf(s_lib,      sizeof s_lib,      "%s/internal/lib", s_base);
  snprintf(s_tmp,      sizeof s_tmp,      "%s/internal/tmp", s_base);
  snprintf(s_data,     sizeof s_data,     "%s/data", s_base);
  snprintf(s_files,    sizeof s_files,    "%s/data/files", s_base);
  snprintf(s_cache,    sizeof s_cache,    "%s/data/cache", s_base);
  snprintf(s_ext,      sizeof s_ext,      "%s/ext", s_base);
  snprintf(s_obb,      sizeof s_obb,      "%s/internal/obb", s_base);
  // Project Dream's loader finds the APK as Android names it: <app dir>/base.apk
  snprintf(s_apk_alias, sizeof s_apk_alias, "%s/base.apk", s_base);
}

const char *path_base(void)     { return s_base; }
const char *path_config(void)   { return s_config; }
const char *path_internal(void) { return s_internal; }
const char *path_lib(void)      { return s_lib; }
const char *path_tmp(void)      { return s_tmp; }
const char *path_data(void)     { return s_data; }
const char *path_files(void)    { return s_files; }
const char *path_cache(void)    { return s_cache; }
const char *path_ext(void)      { return s_ext; }
const char *path_obb(void)      { return s_obb; }
const char *path_apk(void)      { return s_apk; }

// Project Dream's bundle cache marker (data/files/bundles/.apk: the APK path it copied bundles from). A renamed
// folder (sdt_nx -> projectdream_nx, the first release) leaves the old folder's path in it; the loader then deletes
// the bundles folder (with the marker still in it: an error on the console it goes past, an exception in Ryubing).
// The marker is moved to this folder's path instead -- the bundles are read from the APK in place now anyway.
void paths_fix_bundle_marker(void) {
  char p[512];
  snprintf(p, sizeof p, "%s/bundles/.apk", s_files);
  FILE *f = fopen(p, "rb");
  if (!f)
    return;
  char old[300] = { 0 };
  const size_t n = fread(old, 1, sizeof old - 1, f);
  fclose(f);
  old[n] = 0;
  const size_t ol = strlen(old);
  if (!strcmp(old, s_apk_alias) || ol < 9 || strcmp(old + ol - 9, "/base.apk"))
    return; // (this folder's, or not the loader's kind)
  f = fopen(p, "wb");
  if (f) {
    fwrite(s_apk_alias, 1, strlen(s_apk_alias), f);
    fclose(f);
  }
}

void paths_set_apk(const char *path) {
  snprintf(s_apk, sizeof s_apk, "%s", path ? path : "");
}

void paths_make_dirs(void) {
  const char *dirs[] = {
    s_internal, s_lib, s_tmp, s_data, s_files, s_cache, s_ext, s_obb,
  };
  for (unsigned i = 0; i < sizeof(dirs) / sizeof(*dirs); i++)
    mkdir(dirs[i], 0777);
  char p[600];
  snprintf(p, sizeof p, "%s/shared_prefs", s_data);
  mkdir(p, 0777);
  write_proc();
}

// /proc (path_from_android maps it to internal/proc): the protector in
// libil2cpp stats and reads /proc/self/status, which every Android process
// has, for a debugger (TracerPid); a process's name is its package (the
// status line has its last 15 characters). Pid matches getpid_fake.
static void write_proc(void) {
  char p[340];

  snprintf(p, sizeof p, "%s/proc", s_internal);
  mkdir(p, 0777);
  snprintf(p, sizeof p, "%s/proc/self", s_internal);
  mkdir(p, 0777);
  snprintf(p, sizeof p, "%s/proc/self/status", s_internal);
  FILE *f = fopen(p, "w");
  if (f) {
    const size_t n = strlen(s_pkg);
    fprintf(f, "Name:\t%s\n", s_pkg + (n > 15 ? n - 15 : 0));
    fputs("Umask:\t0077\n"
          "State:\tS (sleeping)\n"
          "Tgid:\t1000\n"
          "Ngid:\t0\n"
          "Pid:\t1000\n"
          "PPid:\t600\n"
          "TracerPid:\t0\n"
          "Uid:\t10234\t10234\t10234\t10234\n"
          "Gid:\t10234\t10234\t10234\t10234\n"
          "FDSize:\t256\n"
          "Groups:\t3003 9997 20234 50234\n"
          "Threads:\t40\n"
          "SigQ:\t0/27367\n"
          "SigPnd:\t0000000000000000\n"
          "ShdPnd:\t0000000000000000\n"
          "SigBlk:\t0000000080001204\n"
          "SigIgn:\t0000000000001001\n"
          "SigCgt:\t0000006e400084f8\n"
          "CapInh:\t0000000000000000\n"
          "CapPrm:\t0000000000000000\n"
          "CapEff:\t0000000000000000\n"
          "CapBnd:\t0000000000000000\n"
          "CapAmb:\t0000000000000000\n"
          "NoNewPrivs:\t0\n"
          "Seccomp:\t2\n"
          "Cpus_allowed:\t7\n"
          "Cpus_allowed_list:\t0-2\n"
          "voluntary_ctxt_switches:\t120\n"
          "nonvoluntary_ctxt_switches:\t8\n",
          f);
    fclose(f);
  }
  // Project Dream's libil2cpp.so finds its APK here (apk_path: the line with
  // "/base.apk", which path_from_android turns into sdt.apk)
  snprintf(p, sizeof p, "%s/proc/self/maps", s_internal);
  if ((f = fopen(p, "w"))) {
    fprintf(f, "7f00000000-7f00100000 r--s 00000000 fe:2a 1234                       %s\n", s_apk_alias);
    fclose(f);
  }
  snprintf(p, sizeof p, "%s/proc/self/cmdline", s_internal);
  if ((f = fopen(p, "wb"))) {
    fwrite(s_pkg, 1, strlen(s_pkg) + 1, f);
    fclose(f);
  }
  extern const char fake_cpuinfo[]; // libc_shim.c
  snprintf(p, sizeof p, "%s/proc/cpuinfo", s_internal);
  if ((f = fopen(p, "w"))) {
    fputs(fake_cpuinfo, f);
    fclose(f);
  }
}

// prefix -> replacement directory (the prefixes: the package's, paths_set_package)
static const char *const android_root_fmt[][2] = {
  { "/data/data/%s",                         NULL /* data */ },
  { "/data/user/0/%s",                       NULL },
  { "/data/user_de/0/%s",                    NULL },
  { "/storage/emulated/0/Android/data/%s/files", "ext" },
  { "/sdcard/Android/data/%s/files",         "ext" },
  { "/storage/emulated/0/Android/data/%s",   "extroot" },
  { "/sdcard/Android/data/%s",               "extroot" },
  { "/storage/emulated/0/Android/obb/%s",    "obb" },
  { "/sdcard/Android/obb/%s",                "obb" },
  { "/proc",                                 "proc" },
};
#define NROOTS (sizeof(android_root_fmt) / sizeof(*android_root_fmt))
static char s_root_prefix[NROOTS][160];
static const char *android_roots[NROOTS][2];

static void build_roots(void) {
  for (unsigned i = 0; i < NROOTS; i++) {
    snprintf(s_root_prefix[i], sizeof s_root_prefix[i], android_root_fmt[i][0], s_pkg);
    android_roots[i][0] = s_root_prefix[i];
    android_roots[i][1] = android_root_fmt[i][1];
  }
}

const char *sdt_package(void) {
  return s_pkg;
}

void paths_set_package(const char *pkg) {
  snprintf(s_pkg, sizeof s_pkg, "%s", pkg && pkg[0] ? pkg : SDT_PACKAGE);
  build_roots();
  paths_make_dirs();
}

// An Android directory above the game's own (/data, /data/data, /storage/emulated/0,
// ...): it exists on a phone, so creating it is EEXIST (the game's
// Directory.CreateDirectory walks down from the root).
int path_android_ancestor(const char *path) {
  if (!path || path[0] != '/')
    return 0;
  if (!android_roots[0][0])
    build_roots();
  size_t n = strlen(path);
  while (n > 1 && path[n - 1] == '/')
    n--;
  for (unsigned i = 0; i < sizeof(android_roots) / sizeof(*android_roots); i++) {
    const char *r = android_roots[i][0];
    if (r && strlen(r) > n && !strncmp(r, path, n) && r[n] == '/')
      return 1;
  }
  return 0;
}

// The result is the caller's until its fourth next call on the same thread. Per thread: Unity's loading threads
// open bundles at once, and with one shared ring a path could be overwritten under another thread before its
// open() ("Unable to open archive file" on a bundle that is there, now and then).
const char *path_from_android(const char *path) {
  static __thread char bufs[4][768];
  static __thread unsigned rr;
  if (!path || path[0] != '/')
    return path;
  // the APK under the name Project Dream's loader gives it
  if (s_apk[0] && !strcmp(path, s_apk_alias))
    return s_apk;
  if (!android_roots[0][0])
    build_roots();
  for (unsigned i = 0; i < sizeof(android_roots) / sizeof(*android_roots); i++) {
    const size_t n = strlen(android_roots[i][0]);
    if (strncmp(path, android_roots[i][0], n) != 0 || (path[n] && path[n] != '/'))
      continue;
    const char *kind = android_roots[i][1];
    char *out = bufs[rr++ & 3];
    if (!kind)
      snprintf(out, sizeof bufs[0], "%s%s", s_data, path + n);
    else if (!strcmp(kind, "ext"))
      snprintf(out, sizeof bufs[0], "%s%s", s_ext, path + n);
    else if (!strcmp(kind, "extroot"))
      snprintf(out, sizeof bufs[0], "%s/extroot%s", s_internal, path + n);
    else if (!strcmp(kind, "proc"))
      snprintf(out, sizeof bufs[0], "%s/proc%s", s_internal, path + n);
    else
      snprintf(out, sizeof bufs[0], "%s%s", s_obb, path + n);
    return out;
  }
  return path;
}
