/* sandbox.c -- nothing outside the port's folder is ever changed
 *
 * Every file the game, Geode, libc++ and mods touch reaches the SD card
 * through the port's libc (imports.c: the modules have no other way to the
 * filesystem -- no syscalls, no libnx). The calls there that change the card
 * -- opening for writing, creating, truncating, renaming (both ends) and
 * deleting files and folders, temporary files -- pass their path through
 * sandbox_check() first, as do the port's own writes from the APK it unpacks.
 *
 * A path passes only when it lies strictly inside the folder the .nro runs
 * from (path_base(), /switch/gd_geode_nx): it is written out in full first
 * (a relative path against the current directory, which mods may change),
 * "." and ".." folded the way Horizon folds them, then compared without case
 * (FAT and exFAT ignore it) up to a '/' boundary, so /switch/gd_geode_nx2 or
 * /switch/gd_geode_nx/../x are outside. Another device than the SD card, a
 * backslash, or a path too long to check are refused outright. A refused
 * call fails with EACCES and leaves a "[sandbox] refused" line in debug.log.
 * Reading is not restricted.
 *
 * The folder itself must be one of its own: run from the card's root or
 * loose in /switch, "inside the port's folder" would be everything, so the
 * port refuses to start there (sandbox_init).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "log.h"
#include "paths.h"
#include "sandbox.h"

#define PATH_CAP 1024

static char s_root[PATH_CAP]; // the port's folder, folded ("" until init)
static size_t s_root_len;

// Fold path into out as an absolute SD card path ("/a/b"): 1, or 0 when it
// cannot be judged (another device, a backslash, too long).
static int fold(const char *path, char *out, size_t cap) {
  char full[PATH_CAP];
  if (!path || !path[0])
    return 0;
  // a device prefix: only the SD card's
  const char *colon = strchr(path, ':');
  const char *slash = strchr(path, '/');
  if (colon && (!slash || colon < slash)) {
    if (colon - path != 4 || strncasecmp(path, "sdmc", 4) != 0)
      return 0;
    path = colon + 1;
    // "sdmc:x" is relative to that device's own current directory, which
    // may not be getcwd()'s: not judged, refused
    if (path[0] != '/')
      return 0;
  }
  if (path[0] == '/') {
    if (snprintf(full, sizeof full, "%s", path) >= (int)sizeof full)
      return 0;
  } else {
    // relative: against the current directory, as the filesystem resolves it
    char cwd[PATH_CAP];
    if (!getcwd(cwd, sizeof cwd))
      return 0;
    const char *c = cwd;
    const char *cc = strchr(cwd, ':');
    if (cc) {
      if (cc - cwd != 4 || strncasecmp(cwd, "sdmc", 4) != 0)
        return 0;
      c = cc + 1;
    }
    if (snprintf(full, sizeof full, "%s/%s", c, path) >= (int)sizeof full)
      return 0;
  }
  if (strchr(full, '\\') || strchr(full, ':'))
    return 0;

  // "." and empty components dropped, ".." takes the last one off (never
  // above the root)
  size_t len = 0;
  out[0] = '\0';
  const char *p = full;
  while (*p) {
    while (*p == '/')
      p++;
    const char *end = p;
    while (*end && *end != '/')
      end++;
    const size_t n = (size_t)(end - p);
    if (n == 0 || (n == 1 && p[0] == '.')) {
      // nothing
    } else if (n == 2 && p[0] == '.' && p[1] == '.') {
      while (len > 0 && out[len - 1] != '/')
        len--;
      if (len > 0)
        len--; // the '/' before it
      out[len] = '\0';
    } else {
      if (len + 1 + n + 1 > cap)
        return 0;
      out[len++] = '/';
      memcpy(out + len, p, n);
      len += n;
      out[len] = '\0';
    }
    p = end;
  }
  if (len == 0) {
    if (cap < 2)
      return 0;
    out[0] = '/';
    out[1] = '\0';
  }
  return 1;
}

int sandbox_init(void) {
  char root[PATH_CAP];
  s_root[0] = '\0';
  s_root_len = 0;
  if (!fold(path_base(), root, sizeof root))
    return 0;
  // folders shared with everything else on the card are never "the port's"
  static const char *shared[] = {
    "/", "/switch", "/atmosphere", "/bootloader", "/config", "/Nintendo", "/emuMMC", "/emummc",
  };
  for (size_t i = 0; i < sizeof shared / sizeof *shared; i++)
    if (!strcasecmp(root, shared[i]))
      return 0;
  snprintf(s_root, sizeof s_root, "%s", root);
  s_root_len = strlen(s_root);
  return 1;
}

int sandbox_allows(const char *path) {
  char p[PATH_CAP];
  if (!s_root_len || !fold(path, p, sizeof p))
    return 0;
  return strncasecmp(p, s_root, s_root_len) == 0 && p[s_root_len] == '/' && p[s_root_len + 1];
}

int sandbox_check(const char *path, const char *what) {
  if (sandbox_allows(path))
    return 1;
  log_printf("[sandbox] refused %s %s", what, path ? path : "(null)");
  errno = EACCES;
  return 0;
}

const char *sandbox_root(void) {
  return s_root;
}
