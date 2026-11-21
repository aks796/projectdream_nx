/* apk.h -- set the game up from sdt.apk (apk.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __APK_H__
#define __APK_H__

// Project Dream's Android package: the loader's save path and process name
#define SDT_PACKAGE "com.sonicdreamteam.android"

typedef struct {
  char package[128];
  int version_code;
  char version_name[32];
  int extracted; // the libraries were extracted this launch
} GameInfo;

// Finds sdt.apk next to the .nro, opens it (apkfile.c) and extracts the native
// libraries when they are missing or the file changed. 0 = ready, -1 = not
// (the player has been told why).
int apk_setup(GameInfo *info);

#endif
