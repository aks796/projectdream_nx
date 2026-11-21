/* apk.c -- set the game up from sdt.apk (see apk.h)
 *
 * sdt.apk is what tools/prepare makes from the player's own macOS Sonic Dream
 * Team and Project Dream: laid out as Project Dream's Android APK, every entry
 * stored. At launch this finds it next to the .nro (any name ending in .apk;
 * the biggest wins), checks it is one, and extracts lib/arm64-v8a/*.so
 * (libunity.so, Project Dream's libil2cpp.so) to internal/lib/ once.
 * Everything else Unity and Project Dream's loader read straight from the
 * file.
 *
 * internal/apk.stamp remembers which file the libraries came from (name and
 * size), so nothing is extracted again until it changes.
 *
 * Parts from mkt_nx's apk.c (finding the file, the stamp, the extraction).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <switch.h>

#include "apk.h"
#include "apkfile.h"
#include "log.h"
#include "paths.h"
#include "setup_ui.h"

#define LIB_DIR "lib/arm64-v8a/"

// Unity's Java-side launcher: never loaded here
static const char *const skip_libs[] = { "libmain.so" };

static const char *const NOTE = "Setting up Sonic Dream Team";

static int ends_with(const char *s, const char *suffix) {
  const size_t n = strlen(s), m = strlen(suffix);
  return n >= m && !strcasecmp(s + n - m, suffix);
}

// --- finding the file ---------------------------------------------------------------

#define MAX_APKS 16

typedef struct {
  char path[512];
  char name[200];
  uint64_t size;
} Cand;

static int cand_cmp(const void *a, const void *b) {
  const uint64_t x = ((const Cand *)a)->size, y = ((const Cand *)b)->size;
  return x < y ? 1 : x > y ? -1 : 0;
}

static int list_apks(Cand *out) {
  int n = 0;
  DIR *d = opendir(path_base());
  if (!d)
    return 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL && n < MAX_APKS) {
    if (e->d_name[0] == '.' || !ends_with(e->d_name, ".apk"))
      continue;
    snprintf(out[n].path, sizeof out[n].path, "%s/%s", path_base(), e->d_name);
    snprintf(out[n].name, sizeof out[n].name, "%s", e->d_name);
    struct stat st;
    out[n].size = stat(out[n].path, &st) == 0 ? (uint64_t)st.st_size : 0;
    n++;
  }
  closedir(d);
  qsort(out, n, sizeof(*out), cand_cmp);
  return n;
}

// --- extraction ---------------------------------------------------------------------

typedef struct {
  uint64_t done_before, total;
  const char *label;
} Prog;

static void extract_progress(uint64_t done, void *arg) {
  Prog *p = arg;
  const int pm = p->total ? (int)((p->done_before + done) * 1000 / p->total) : 0;
  setup_ui_progress(NOTE, p->label, pm);
}

static int is_wanted_lib(const char *name) {
  if (strncmp(name, LIB_DIR, strlen(LIB_DIR)) != 0 || !ends_with(name, ".so"))
    return 0;
  const char *base = name + strlen(LIB_DIR);
  if (strchr(base, '/'))
    return 0;
  for (unsigned i = 0; i < sizeof(skip_libs) / sizeof(*skip_libs); i++)
    if (!strcmp(base, skip_libs[i]))
      return 0;
  return 1;
}

static void stamp_path(char *out, size_t n) {
  snprintf(out, n, "%s/apk.stamp", path_internal());
}

static void stamp_text(char *out, size_t n) {
  const char *slash = strrchr(apkfile_path(), '/');
  snprintf(out, n, "%s %llu", slash ? slash + 1 : apkfile_path(), (unsigned long long)apkfile_size());
}

// the libraries are in place, and from this file
static int libs_current(void) {
  char p[600], want[400], got[400] = "";
  stamp_path(p, sizeof p);
  FILE *f = fopen(p, "r");
  if (!f)
    return 0;
  if (!fgets(got, sizeof got, f))
    got[0] = '\0';
  fclose(f);
  got[strcspn(got, "\r\n")] = '\0';
  stamp_text(want, sizeof want);
  if (strcmp(got, want) != 0)
    return 0;
  for (int i = 0; i < apkfile_count(); i++) {
    const ApkEntry *e = apkfile_entry_at(i);
    if (!e || !is_wanted_lib(e->name))
      continue;
    char lp[600];
    snprintf(lp, sizeof lp, "%s/%s", path_lib(), e->name + strlen(LIB_DIR));
    struct stat st;
    if (stat(lp, &st) != 0 || (uint64_t)st.st_size != e->usize)
      return 0;
  }
  return 1;
}

static int extract_libs(void) {
  uint64_t total = 0;
  for (int i = 0; i < apkfile_count(); i++) {
    const ApkEntry *e = apkfile_entry_at(i);
    if (e && is_wanted_lib(e->name))
      total += e->usize;
  }
  Prog pr = { 0, total, "" };
  char label[160];
  for (int i = 0; i < apkfile_count(); i++) {
    const ApkEntry *e = apkfile_entry_at(i);
    if (!e || !is_wanted_lib(e->name))
      continue;
    const char *base = e->name + strlen(LIB_DIR);
    snprintf(label, sizeof label, "Extracting %s", base);
    pr.label = label;
    char dest[600];
    snprintf(dest, sizeof dest, "%s/%s", path_lib(), base);
    log_printf("[apk] extracting %s (%llu KB)", base, (unsigned long long)(e->usize >> 10));
    if (apkfile_extract(e, dest, extract_progress, &pr) < 0)
      return -1;
    pr.done_before += e->usize;
  }
  char p[600], text[400];
  stamp_path(p, sizeof p);
  stamp_text(text, sizeof text);
  FILE *f = fopen(p, "w");
  if (f) {
    fprintf(f, "%s\n", text);
    fclose(f);
  }
  return 0;
}

// --- set-up -------------------------------------------------------------------------------

static const char *const s_needed[] = {
  LIB_DIR "libunity.so", LIB_DIR "libil2cpp.so", "assets/bin/Data/GameAssembly.arm64",
  "assets/bin/Data/globalgamemanagers", "assets/bin/Data/Managed/Metadata/global-metadata.dat",
  "assets/aa/catalog.json",
};

int apk_setup(GameInfo *info) {
  memset(info, 0, sizeof *info);
  Cand c[MAX_APKS];
  const int n = list_apks(c);
  if (n == 0) {
    char msg[700];
    snprintf(msg, sizeof msg,
             "No game file in %s/.\n\n"
             "Make the Android build of your own macOS Sonic\n"
             "Dream Team 1.10.1 with Project Dream on a PC, then\n"
             "copy its .apk into this folder.",
             path_base());
    setup_ui_message("sdt.apk is missing", msg);
    return -1;
  }
  int ok = -1;
  for (int i = 0; i < n && ok < 0; i++) {
    if (apkfile_open(c[i].path) != 0) {
      log_printf("[apk] %s: %s", c[i].name, apkfile_error());
      continue;
    }
    ok = 0;
    for (unsigned k = 0; k < sizeof s_needed / sizeof *s_needed; k++)
      if (!apkfile_find(s_needed[k])) {
        log_printf("[apk] %s has no %s", c[i].name, s_needed[k]);
        ok = -1;
        break;
      }
    if (ok < 0)
      apkfile_close();
  }
  if (ok < 0) {
    char msg[700];
    snprintf(msg, sizeof msg,
             "The .apk in %s/ is not the game's Android\n"
             "build. Make it with Project Dream from the macOS\n"
             "game 1.10.1 (debug.log says what was missing).",
             path_base());
    setup_ui_message("Not a Sonic Dream Team game file", msg);
    return -1;
  }
  paths_set_apk(apkfile_path());
  snprintf(info->package, sizeof info->package, "%s", SDT_PACKAGE);
  info->version_code = 1;
  snprintf(info->version_name, sizeof info->version_name, "1.10.1");
  if (!libs_current()) {
    if (extract_libs() < 0) {
      setup_ui_message("Setup failed", "The game's libraries could not be extracted\nfrom sdt.apk (is the card full?).");
      return -1;
    }
    info->extracted = 1;
  }
  return 0;
}
