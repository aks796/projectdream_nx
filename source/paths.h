/* paths.h -- runtime file layout, resolved from the .nro's own location so the
 * game folder can be named anything (not tied to a fixed /switch/<name>/).
 *
 *   <base>/                 the .nro, sdt.apk, config.txt, debug.log
 *   <base>/internal/        lib/ (the APK's native libraries), apk.stamp, tmp/,
 *                           proc/ (/proc/self: status, cmdline, maps)
 *   <base>/data/            Android's /data/data/com.sonicdreamteam.android
 *                           (the saves: files/PCSaveData)
 *   <base>/ext/             Android's /storage/emulated/0/Android/data/<pkg>
 *
 * From mkt_nx.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef PATHS_H
#define PATHS_H

#define PORT_NAME "projectdream_nx"

// Resolve the base directory from argv[0] (else the cwd). Call once at startup.
void paths_init(const char *argv0);

const char *path_base(void);      // directory of the .nro
const char *path_config(void);    // "<base>/config.txt"
const char *path_internal(void);  // "<base>/internal"
const char *path_lib(void);       // "<base>/internal/lib"
const char *path_tmp(void);       // "<base>/internal/tmp"
const char *path_data(void);      // "<base>/data"        (/data/data/<pkg>)
const char *path_files(void);     // "<base>/data/files"  (Context.getFilesDir)
const char *path_cache(void);     // "<base>/data/cache"  (Context.getCacheDir)
const char *path_ext(void);       // "<base>/ext"         (getExternalFilesDir)
const char *path_obb(void);       // "<base>/internal/obb"  (getObbDir)

// The player's APK, once apk_setup() found it ("" before).
const char *path_apk(void);
void paths_set_apk(const char *path);

// Make every directory above exist.
void paths_make_dirs(void);
void paths_fix_bundle_marker(void); // the loader's bundle cache marker moved to this folder (a renamed folder)

// The game's package (Project Dream's): its Android paths, its process name.
void paths_set_package(const char *pkg);
const char *sdt_package(void);

// Rewrites an Android-absolute path of the game (its data dirs, its OBB
// dir, /sdcard) onto the SD layout. Returns the input when nothing matches,
// else one of a few rotating static buffers.
const char *path_from_android(const char *path);
// 1 for a directory above one of the game's Android dirs (/data, /data/data, ...)
int path_android_ancestor(const char *path);

#endif
