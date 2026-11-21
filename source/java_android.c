/* java_android.c -- the Android framework classes the engine and the game
 * call: the activity as a Context, its paths, PackageManager and the package
 * info, Resources/Configuration/DisplayMetrics, Display and WindowManager,
 * Build, Locale, java.io.File, StatFs, Environment, SharedPreferences, system
 * services (connectivity, power, audio, vibrator, ...), Intent/Bundle,
 * Settings, and Looper/Handler/Message/HandlerThread/Choreographer: a
 * HandlerThread is a real thread running a message queue and its
 * Choreographer's once-per-frame callbacks (what posts to the main looper
 * still runs at once).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <dirent.h>
#include <errno.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <switch.h>

#include "config.h"
#include "jni.h"
#include "libc_shim.h"
#include "log.h"
#include "paths.h"
#include "pthr.h"

#define ACTIVITY_CLASS "com/google/firebase/MessagingUnityPlayerActivity"

static JObj *s_activity, *s_app, *s_resources, *s_config, *s_metrics, *s_display, *s_wm;
static JObj *s_pm, *s_appinfo, *s_pkginfo, *s_assets, *s_resolver, *s_intent, *s_window;
static JObj *s_decor, *s_looper, *s_locale, *s_meta;
static volatile int s_layout_pending; // the surface view needs laying out (java_android_frame)

JObj *jni_activity(void) { return s_activity; }

static const char *S(JObj *o) {
  const char *s = jni_utf(o);
  return s ? s : "";
}

// --- java.io.File -------------------------------------------------------------------
// A File remembers its path in a field ("path"). Paths handed to the game
// are SD paths already; Android-absolute ones the game builds itself are
// remapped by the libc shim when they are used.

static JObj *file_new(const char *path) {
  JObj *f = jni_new("java/io/File");
  char p[768];
  snprintf(p, sizeof p, "%s", path ? path : "");
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/')
    p[--n] = '\0';
  jni_set_field(f, "path", jv_l(jni_str(p)));
  return f;
}

static const char *file_path(JObj *f) {
  if (jni_is_string(f))
    return jni_utf(f);
  return S(jni_get_field(f, "path", NULL).l);
}

JH(file_init) {
  JUNUSED;
  // File(String) / File(File, String) / File(String, String)
  char p[768];
  if (m->nargs == 1) {
    snprintf(p, sizeof p, "%s", file_path(a[0].l));
  } else {
    const char *dir = file_path(a[0].l), *leaf = S(a[1].l);
    if (leaf[0] == '/' || !dir[0])
      snprintf(p, sizeof p, "%s", leaf);
    else
      snprintf(p, sizeof p, "%s/%s", dir, leaf);
  }
  size_t n = strlen(p);
  while (n > 1 && p[n - 1] == '/')
    p[--n] = '\0';
  jni_set_field(thiz, "path", jv_l(jni_str(p)));
  return jv_v();
}

JH(file_getPath) { JUNUSED; return jv_l(jni_str(file_path(thiz))); }
JH(file_getAbsoluteFile) { JUNUSED; return jv_l(file_new(file_path(thiz))); }
JH(file_getName) {
  JUNUSED;
  const char *p = file_path(thiz);
  const char *s = strrchr(p, '/');
  return jv_l(jni_str(s ? s + 1 : p));
}
JH(file_getParent) {
  JUNUSED;
  char p[768];
  snprintf(p, sizeof p, "%s", file_path(thiz));
  char *s = strrchr(p, '/');
  if (!s)
    return jv_l(NULL);
  if (s == p)
    s[1] = '\0';
  else
    *s = '\0';
  return jv_l(m->ret == 'L' && strstr(m->sig, "File;") ? file_new(p) : jni_str(p));
}

struct bstat { uint64_t dev, ino; uint32_t mode, nlink, uid, gid; uint64_t rdev, pad; int64_t size;
               int32_t blksize, pad2; int64_t blocks; int64_t at, atn, mt, mtn, ct, ctn; uint32_t u4, u5; };

static int fstat_path(const char *p, struct bstat *st) {
  return stat_fake(p, (struct bionic_stat *)st);
}

JH(file_exists) {
  JUNUSED;
  struct bstat st;
  return jv_z(fstat_path(file_path(thiz), &st) == 0);
}
JH(file_isDirectory) {
  JUNUSED;
  struct bstat st;
  return jv_z(fstat_path(file_path(thiz), &st) == 0 && S_ISDIR(st.mode));
}
JH(file_isFile) {
  JUNUSED;
  struct bstat st;
  return jv_z(fstat_path(file_path(thiz), &st) == 0 && S_ISREG(st.mode));
}
JH(file_length) {
  JUNUSED;
  struct bstat st;
  return jv_j(fstat_path(file_path(thiz), &st) == 0 ? st.size : 0);
}
JH(file_lastModified) {
  JUNUSED;
  struct bstat st;
  return jv_j(fstat_path(file_path(thiz), &st) == 0 ? st.mt * 1000 : 0);
}
JH(file_mkdirs) {
  JUNUSED;
  const int r = mkdir_fake(file_path(thiz), 0777);
  return jv_z(r == 0);
}
JH(file_delete) {
  JUNUSED;
  const char *p = file_path(thiz);
  struct bstat st;
  if (fstat_path(p, &st) != 0)
    return jv_z(0);
  extern int rmdir_fake(const char *);
  return jv_z(S_ISDIR(st.mode) ? rmdir_fake(p) == 0 : remove_fake(p) == 0);
}
JH(file_renameTo) {
  JUNUSED;
  return jv_z(rename_fake(file_path(thiz), file_path(a[0].l)) == 0);
}
JH(file_createNewFile) {
  JUNUSED;
  struct bstat st;
  if (fstat_path(file_path(thiz), &st) == 0)
    return jv_z(0);
  FILE *f = fopen_fake(file_path(thiz), "wb");
  if (f)
    fclose_fake(f);
  return jv_z(f != NULL);
}
JH(ret_true) { JUNUSED; return jv_z(1); }
JH(ret_false) { JUNUSED; return jv_z(0); }
JH(ret_null) { JUNUSED; return jv_l(NULL); }
JH(ret_zero) { JUNUSED; return jv_j(0); }
JH(ret_void) { JUNUSED; return jv_v(); }
JH(ret_self) { JUNUSED; return jv_l(thiz); }

typedef struct {
  JObj *list;
  const char *dir;
  int files;
} ListCtx;

JH(file_list) {
  JUNUSED;
  // list() -> String[], listFiles() -> File[]
  const char *dir = file_path(thiz);
  void *d = opendir_fake(dir);
  if (!d)
    return jv_l(NULL);
  JObj *list = jni_list_new();
  struct { uint64_t ino; int64_t off; uint16_t reclen; uint8_t type; char name[256]; } *e;
  while ((e = readdir_fake(d)) != NULL) {
    if (!strcmp(e->name, ".") || !strcmp(e->name, ".."))
      continue;
    if (m->ret == 'L' && strstr(m->sig, "File;")) {
      char p[1024];
      snprintf(p, sizeof p, "%s/%s", dir, e->name);
      jni_list_add(list, file_new(p));
    } else {
      jni_list_add(list, jni_str(e->name));
    }
  }
  extern int closedir_fake(void *);
  closedir_fake(d);
  const int n = jni_list_size(list);
  JObj *arr = jni_array('L', n, strstr(m->sig, "File;") ? "java/io/File" : "java/lang/String");
  for (int i = 0; i < n; i++)
    ((JObj **)jni_arr(arr)->data)[i] = jni_list_get(list, i);
  return jv_l(arr);
}

static int64_t free_bytes(void) {
  s64 fr = 8ll << 30;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs)
    fsFsGetFreeSpace(fs, "/", &fr);
  return fr;
}

static int64_t total_bytes(void) {
  s64 t = 64ll << 30;
  FsFileSystem *fs = fsdevGetDeviceFileSystem("sdmc");
  if (fs)
    fsFsGetTotalSpace(fs, "/", &t);
  return t;
}

JH(file_getFreeSpace) { JUNUSED; return jv_j(free_bytes()); }
JH(file_getTotalSpace) { JUNUSED; return jv_j(total_bytes()); }

JH(file_toURI) {
  JUNUSED;
  JObj *u = jni_new("java/net/URI");
  jni_set_field(u, "str", jv_l(jni_strf("file://%s", file_path(thiz))));
  return jv_l(u);
}

// StatFs(path): sizes of the SD card
JH(statfs_init) { JUNUSED; return jv_v(); }
JH(statfs_availableBytes) { JUNUSED; return jv_j(free_bytes()); }
JH(statfs_totalBytes) { JUNUSED; return jv_j(total_bytes()); }
JH(statfs_blockSize) { JUNUSED; return m->ret == 'J' ? jv_j(32768) : jv_i(32768); }
JH(statfs_availBlocks) {
  JUNUSED;
  return m->ret == 'J' ? jv_j(free_bytes() / 32768) : jv_i((int32_t)(free_bytes() / 32768));
}
JH(statfs_blockCount) {
  JUNUSED;
  return m->ret == 'J' ? jv_j(total_bytes() / 32768) : jv_i((int32_t)(total_bytes() / 32768));
}

// --- Context / Activity ------------------------------------------------------------------

JH(ctx_getPackageName) { JUNUSED; return jv_l(jni_str(sdt_package())); }
JH(ctx_getPackageCodePath) { JUNUSED; return jv_l(jni_str(path_apk())); }
JH(ctx_getFilesDir) { JUNUSED; return jv_l(file_new(path_files())); }
JH(ctx_getCacheDir) { JUNUSED; return jv_l(file_new(path_cache())); }
JH(ctx_getDataDir) { JUNUSED; return jv_l(file_new(path_data())); }
JH(ctx_getNoBackupFilesDir) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/no_backup", path_data());
  mkdir(p, 0777);
  return jv_l(file_new(p));
}
JH(ctx_getCodeCacheDir) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/code_cache", path_data());
  mkdir(p, 0777);
  return jv_l(file_new(p));
}
JH(ctx_getExternalFilesDir) {
  JUNUSED;
  const char *type = m->nargs ? jni_utf(a[0].l) : NULL;
  char p[600];
  if (type && type[0]) {
    snprintf(p, sizeof p, "%s/%s", path_ext(), type);
    mkdir(p, 0777);
  } else {
    snprintf(p, sizeof p, "%s", path_ext());
  }
  return jv_l(file_new(p));
}
JH(ctx_getExternalFilesDirs) {
  JUNUSED;
  JObj *arr = jni_array('L', 1, "java/io/File");
  ((JObj **)jni_arr(arr)->data)[0] = ctx_getExternalFilesDir(thiz, m, a).l;
  return jv_l(arr);
}
JH(ctx_getExternalCacheDir) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/cache", path_ext());
  mkdir(p, 0777);
  return jv_l(file_new(p));
}
JH(ctx_getObbDir) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s", path_obb());
  mkdir(p, 0777);
  return jv_l(file_new(p));
}
JH(ctx_getObbDirs) {
  JUNUSED;
  JObj *arr = jni_array('L', 1, "java/io/File");
  ((JObj **)jni_arr(arr)->data)[0] = ctx_getObbDir(thiz, m, a).l;
  return jv_l(arr);
}
JH(ctx_getDir) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/app_%s", path_data(), S(a[0].l));
  mkdir(p, 0777);
  return jv_l(file_new(p));
}
JH(ctx_getDatabasePath) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/databases", path_data());
  mkdir(p, 0777);
  snprintf(p, sizeof p, "%s/databases/%s", path_data(), S(a[0].l));
  return jv_l(file_new(p));
}
JH(ctx_getApplicationContext) { JUNUSED; return jv_l(s_activity); }
JH(ctx_getApplication) { JUNUSED; return jv_l(s_app); }
JH(ctx_getResources) { JUNUSED; return jv_l(s_resources); }
JH(ctx_getAssets) { JUNUSED; return jv_l(s_assets); }
JH(ctx_getPackageManager) { JUNUSED; return jv_l(s_pm); }
JH(ctx_getContentResolver) { JUNUSED; return jv_l(s_resolver); }
JH(ctx_getApplicationInfo) { JUNUSED; return jv_l(s_appinfo); }
JH(ctx_getMainLooper) { JUNUSED; return jv_l(s_looper); }
JH(ctx_getIntent) { JUNUSED; return jv_l(s_intent); }
JH(ctx_getWindow) { JUNUSED; return jv_l(s_window); }
JH(ctx_getWindowManager) { JUNUSED; return jv_l(s_wm); }
JH(ctx_checkPermission) { JUNUSED; return jv_i(0); } // PERMISSION_GRANTED
JH(ctx_getRequestedOrientation) { JUNUSED; return jv_i(jni_field_i(s_activity, "requestedOrientation")); }
JH(ctx_setRequestedOrientation) {
  JUNUSED;
  jni_set_field(s_activity, "requestedOrientation", jv_i(a[0].i));
  log_printf("[android] setRequestedOrientation(%d)", a[0].i);
  return jv_v();
}

// Runnables: posted work runs at once on the calling thread. A Runnable from
// C# (AndroidJavaRunnable) is a proxy; one of ours has a "run" handler.
static void run_runnable(JObj *r) {
  if (!r)
    return;
  if (jni_is_proxy(r)) {
    jni_proxy_invoke(r, "run", "()V", NULL, 0);
    return;
  }
  log_printf("[android] a %s posted as a Runnable is not run", jni_class_name(r));
}

JH(ctx_runOnUiThread) { JUNUSED; run_runnable(a[0].l); return jv_v(); }
JH(handler_post) { JUNUSED; run_runnable(a[0].l); return jv_z(1); }

JH(ctx_getSystemService) {
  JUNUSED;
  const char *n = S(a[0].l);
  static JObj *services[32];
  static const char *names[32][2] = {
    { "window", "android/view/WindowManager" },
    { "audio", "android/media/AudioManager" },
    { "connectivity", "android/net/ConnectivityManager" },
    { "power", "android/os/PowerManager" },
    { "vibrator", "android/os/Vibrator" },
    { "vibrator_manager", "android/os/VibratorManager" },
    { "input", "android/hardware/input/InputManager" },
    { "sensor", "android/hardware/SensorManager" },
    { "display", "android/hardware/display/DisplayManager" },
    { "activity", "android/app/ActivityManager" },
    { "phone", "android/telephony/TelephonyManager" },
    { "wifi", "android/net/wifi/WifiManager" },
    { "uimode", "android/app/UiModeManager" },
    { "storage", "android/os/storage/StorageManager" },
    { "batterymanager", "android/os/BatteryManager" },
    { "clipboard", "android/content/ClipboardManager" },
    { "input_method", "android/view/inputmethod/InputMethodManager" },
    { "notification", "android/app/NotificationManager" },
    { "alarm", "android/app/AlarmManager" },
    { "game", "android/app/GameManager" },
    { "keyguard", "android/app/KeyguardManager" },
    { "location", "android/location/LocationManager" },
    { "media_router", "android/media/MediaRouter" },
    { "jobscheduler", "android/app/job/JobScheduler" },
    { "usagestats", "android/app/usage/UsageStatsManager" },
    { NULL, NULL },
  };
  for (int i = 0; names[i][0]; i++) {
    if (!strcmp(n, names[i][0])) {
      if (!strcmp(n, "window"))
        return jv_l(s_wm);
      if (!services[i])
        services[i] = jni_new(names[i][1]);
      return jv_l(services[i]);
    }
  }
  log_printf("[android] getSystemService(%s) -> null", n);
  return jv_l(NULL);
}

// --- SharedPreferences ----------------------------------------------------------------------
// One store per name, kept as a HashMap of String -> boxed value / String and
// written to data/shared_prefs/<name>.kv on apply()/commit() (one line per
// key: type, key, value; '\' escapes).

typedef struct Prefs {
  struct Prefs *next;
  char name[160];
  JObj *obj;      // the SharedPreferences
  JObj *map;      // key -> value
} Prefs;

static Prefs *s_prefs;
static Mutex s_prefs_lock;

static void esc_write(FILE *f, const char *s) {
  for (; *s; s++) {
    if (*s == '\\' || *s == '\t' || *s == '\n')
      fputc('\\', f), fputc(*s == '\n' ? 'n' : *s == '\t' ? 't' : '\\', f);
    else
      fputc(*s, f);
  }
}

static char *unesc(char *s) {
  char *o = s;
  for (char *p = s; *p; p++) {
    if (*p == '\\' && p[1]) {
      p++;
      *o++ = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p;
    } else {
      *o++ = *p;
    }
  }
  *o = '\0';
  return s;
}

static void prefs_path(const Prefs *p, char *out, size_t n) {
  snprintf(out, n, "%s/shared_prefs/%s.kv", path_data(), p->name);
}

static void prefs_save(Prefs *p) {
  char path[700], tmp[710];
  prefs_path(p, path, sizeof path);
  snprintf(tmp, sizeof tmp, "%s.tmp", path);
  FILE *f = fopen(tmp, "w");
  if (!f)
    return;
  const int n = jni_map_size(p->map);
  for (int i = 0; i < n; i++) {
    const char *k = jni_utf(jni_map_key_at(p->map, i));
    JObj *v = jni_map_val_at(p->map, i);
    if (!k)
      continue;
    jvalue bv;
    char t;
    if (jni_is_string(v)) {
      fputs("s\t", f);
      esc_write(f, k);
      fputc('\t', f);
      esc_write(f, jni_utf(v));
    } else if (jni_unbox(v, &bv, &t)) {
      fprintf(f, "%c\t", t);
      esc_write(f, k);
      if (t == 'J') fprintf(f, "\t%lld", (long long)bv.j);
      else if (t == 'F') fprintf(f, "\t%.9g", (double)bv.f);
      else if (t == 'D') fprintf(f, "\t%.17g", bv.d);
      else fprintf(f, "\t%d", bv.i);
    } else {
      continue;
    }
    fputc('\n', f);
  }
  fclose(f);
  remove(path);
  rename(tmp, path);
}

static void prefs_load(Prefs *p) {
  char path[700];
  prefs_path(p, path, sizeof path);
  FILE *f = fopen(path, "r");
  if (!f)
    return;
  char line[16384];
  while (fgets(line, sizeof line, f)) {
    size_t l = strlen(line);
    while (l && (line[l - 1] == '\n' || line[l - 1] == '\r'))
      line[--l] = '\0';
    char *t1 = strchr(line, '\t');
    if (!t1)
      continue;
    *t1 = '\0';
    char *key = t1 + 1, *t2 = strchr(key, '\t');
    if (!t2)
      continue;
    *t2 = '\0';
    char *val = t2 + 1;
    unesc(key);
    unesc(val);
    const char t = line[0];
    JObj *v;
    jvalue bv;
    bv.j = 0;
    switch (t) {
      case 's': v = jni_str(val); break;
      case 'J': bv.j = strtoll(val, NULL, 10); v = jni_box('J', bv); break;
      case 'F': bv.f = strtof(val, NULL); v = jni_box('F', bv); break;
      case 'D': bv.d = strtod(val, NULL); v = jni_box('D', bv); break;
      default: bv.i = (int32_t)strtol(val, NULL, 10); v = jni_box(t, bv); break;
    }
    jni_map_put(p->map, jni_str(key), v);
  }
  fclose(f);
}

static Prefs *prefs_get(const char *name) {
  mutexLock(&s_prefs_lock);
  for (Prefs *p = s_prefs; p; p = p->next)
    if (!strcmp(p->name, name)) {
      mutexUnlock(&s_prefs_lock);
      return p;
    }
  Prefs *p = calloc(1, sizeof(*p));
  snprintf(p->name, sizeof p->name, "%s", name);
  p->obj = jni_new("android/app/SharedPreferencesImpl");
  p->obj->native = p;
  p->map = jni_map_new();
  prefs_load(p);
  p->next = s_prefs;
  s_prefs = p;
  mutexUnlock(&s_prefs_lock);
  return p;
}

static Prefs *prefs_of(JObj *o) {
  // the store, or the editor's store
  if (!o)
    return NULL;
  if (jni_instance_of(o, "android/content/SharedPreferences$Editor"))
    o = jni_get_field(o, "prefs", NULL).l;
  return o && jni_instance_of(o, "android/content/SharedPreferences") ? o->native : NULL;
}

JH(ctx_getSharedPreferences) {
  JUNUSED;
  char defprefs[128];
  snprintf(defprefs, sizeof defprefs, "%s_preferences", sdt_package());
  const char *n = m->nargs ? S(a[0].l) : defprefs;
  return jv_l(prefs_get(n[0] ? n : "default")->obj);
}
JH(ctx_getPreferences) {
  JUNUSED;
  return jv_l(prefs_get("MessagingUnityPlayerActivity")->obj);
}
JH(pm_getDefaultSharedPreferences) {
  JUNUSED;
  char defprefs[128];
  snprintf(defprefs, sizeof defprefs, "%s_preferences", sdt_package());
  return jv_l(prefs_get(defprefs)->obj);
}

JH(prefs_get_value) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  JObj *v = p ? jni_map_get_s(p->map, S(a[0].l)) : NULL;
  if (!v) {
    if (m->ret == 'L')
      return jv_l(m->nargs > 1 ? a[1].l : NULL);
    return m->nargs > 1 ? a[1] : jv_v();
  }
  if (m->ret == 'L')
    return jv_l(v);
  jvalue bv;
  char t;
  if (!jni_unbox(v, &bv, &t))
    return m->nargs > 1 ? a[1] : jv_v();
  // a value stored as another type is converted, as Android would throw;
  // returning the default is closer to what the game expects
  if ((m->ret == 'J') != (t == 'J') || (m->ret == 'F') != (t == 'F'))
    return m->nargs > 1 ? a[1] : jv_v();
  return bv;
}
JH(prefs_contains) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  return jv_z(p && jni_map_get_s(p->map, S(a[0].l)) != NULL);
}
JH(prefs_getAll) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  return jv_l(p ? p->map : jni_map_new());
}
JH(prefs_edit) {
  JUNUSED;
  JObj *e = jni_new("android/app/SharedPreferencesImpl$EditorImpl");
  jni_set_field(e, "prefs", jv_l(thiz));
  return jv_l(e);
}
JH(editor_put) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  if (!p)
    return jv_l(thiz);
  JObj *v;
  switch (m->argt[1]) {
    case 'L': v = a[1].l; break;
    default: v = jni_box(m->argt[1], a[1]); break;
  }
  mutexLock(&s_prefs_lock);
  jni_map_put(p->map, jni_str(S(a[0].l)), v);
  mutexUnlock(&s_prefs_lock);
  return jv_l(thiz);
}
JH(editor_remove) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  if (p) {
    mutexLock(&s_prefs_lock);
    jni_map_remove_s(p->map, S(a[0].l));
    mutexUnlock(&s_prefs_lock);
  }
  return jv_l(thiz);
}
JH(editor_clear) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  if (p) {
    mutexLock(&s_prefs_lock);
    while (jni_map_size(p->map) > 0)
      jni_map_remove_s(p->map, jni_utf(jni_map_key_at(p->map, 0)));
    mutexUnlock(&s_prefs_lock);
  }
  return jv_l(thiz);
}
JH(editor_commit) {
  JUNUSED;
  Prefs *p = prefs_of(thiz);
  if (p) {
    mutexLock(&s_prefs_lock);
    prefs_save(p);
    mutexUnlock(&s_prefs_lock);
  }
  return m->ret == 'Z' ? jv_z(1) : jv_v();
}

// --- Resources / Configuration / DisplayMetrics / Display ----------------------------------

static void fill_metrics(JObj *dm) {
  const int dpi = screen_height >= 1080 ? 320 : 240;
  jni_set_field(dm, "widthPixels", jv_i(screen_width));
  jni_set_field(dm, "heightPixels", jv_i(screen_height));
  jni_set_field(dm, "densityDpi", jv_i(dpi));
  jni_set_field(dm, "density", jv_f(dpi / 160.0f));
  jni_set_field(dm, "scaledDensity", jv_f(dpi / 160.0f));
  jni_set_field(dm, "xdpi", jv_f(237.0f));
  jni_set_field(dm, "ydpi", jv_f(237.0f));
}

void java_android_screen_changed(void) {
  fill_metrics(s_metrics);
  s_layout_pending = 1; // the surface view is laid out again
  const int dpi = jni_field_i(s_metrics, "densityDpi");
  jni_set_field(s_config, "screenWidthDp", jv_i(screen_width * 160 / dpi));
  jni_set_field(s_config, "screenHeightDp", jv_i(screen_height * 160 / dpi));
  jni_set_field(s_config, "smallestScreenWidthDp", jv_i(screen_height * 160 / dpi));
  jni_set_field(s_config, "densityDpi", jv_i(dpi));
}

JH(res_getConfiguration) { JUNUSED; return jv_l(s_config); }
JH(res_getDisplayMetrics) { JUNUSED; return jv_l(s_metrics); }
#define SURFACE_VIEW_ID 0x7f0b0001 // R.id.unitySurfaceView (see "the surface view")

JH(res_getIdentifier) {
  JUNUSED;
  const int id = !strcmp(S(a[0].l), "unitySurfaceView") ? SURFACE_VIEW_ID : 0;
  log_printf("[android] Resources.getIdentifier(%s, %s) -> 0x%x", S(a[0].l), m->nargs > 1 ? S(a[1].l) : "",
             id);
  return jv_i(id);
}
JH(res_getString) { JUNUSED; return jv_l(jni_str("")); }
JH(res_getBoolean) { JUNUSED; return jv_z(0); }
JH(res_getInteger) { JUNUSED; return jv_i(0); }

JH(wm_getDefaultDisplay) { JUNUSED; return jv_l(s_display); }
JH(ctx_getDisplay) { JUNUSED; return jv_l(s_display); }
JH(disp_getMetrics) {
  JUNUSED;
  if (a[0].l)
    fill_metrics(a[0].l);
  return jv_v();
}
JH(disp_getRotation) { JUNUSED; return jv_i(0); }
JH(disp_getRefreshRate) { JUNUSED; return jv_f(60.0f); }
JH(disp_getWidth) { JUNUSED; return jv_i(screen_width); }
JH(disp_getHeight) { JUNUSED; return jv_i(screen_height); }
JH(disp_getDisplayId) { JUNUSED; return jv_i(0); }
JH(disp_getSize) {
  JUNUSED;
  if (a[0].l) {
    jni_set_field(a[0].l, "x", jv_i(screen_width));
    jni_set_field(a[0].l, "y", jv_i(screen_height));
  }
  return jv_v();
}
static JObj *s_mode;
JH(disp_getMode) { JUNUSED; return jv_l(s_mode); }
JH(disp_getSupportedModes) {
  JUNUSED;
  JObj *arr = jni_array('L', 1, "android/view/Display$Mode");
  ((JObj **)jni_arr(arr)->data)[0] = s_mode;
  return jv_l(arr);
}
JH(mode_getPhysicalWidth) { JUNUSED; return jv_i(screen_width); }
JH(mode_getPhysicalHeight) { JUNUSED; return jv_i(screen_height); }
JH(mode_getRefreshRate) { JUNUSED; return jv_f(60.0f); }
JH(mode_getModeId) { JUNUSED; return jv_i(1); }

// --- PackageManager ----------------------------------------------------------------------

JH(pm_getPackageInfo) { JUNUSED; return jv_l(s_pkginfo); }
JH(pm_getApplicationInfo) { JUNUSED; return jv_l(s_appinfo); }
JH(pm_getInstallerPackageName) { JUNUSED; return jv_l(jni_str("com.android.vending")); }
JH(pm_hasSystemFeature) {
  JUNUSED;
  const char *f = S(a[0].l);
  static const char *const yes[] = {
    "android.hardware.touchscreen", "android.hardware.touchscreen.multitouch",
    "android.hardware.touchscreen.multitouch.distinct", "android.hardware.wifi",
    "android.hardware.screen.landscape", "android.hardware.opengles.aep",
    "android.hardware.gamepad", "android.hardware.audio.output",
    "android.hardware.sensor.accelerometer", "android.hardware.sensor.gyroscope",
  };
  for (unsigned i = 0; i < sizeof(yes) / sizeof(*yes); i++)
    if (!strcmp(f, yes[i]))
      return jv_z(1);
  return jv_z(0);
}
JH(pm_getActivityInfo) {
  JUNUSED;
  JObj *ai = jni_new("android/content/pm/ActivityInfo");
  jni_set_field(ai, "screenOrientation", jv_i(12));
  jni_set_field(ai, "configChanges", jv_i(0x40003fff));
  jni_set_field(ai, "metaData", jv_l(s_meta));
  jni_set_field(ai, "name", jv_l(jni_str("com.google.firebase.MessagingUnityPlayerActivity")));
  jni_set_field(ai, "packageName", jv_l(jni_str(sdt_package())));
  jni_set_field(ai, "applicationInfo", jv_l(s_appinfo));
  return jv_l(ai);
}
JH(pm_queryList) { JUNUSED; return jv_l(jni_list_new()); }

// --- Bundle (meta-data, intent extras) -------------------------------------------------------

JH(bundle_get) {
  JUNUSED;
  JObj *v = jni_map_get_s(thiz ? jni_get_field(thiz, "map", NULL).l : NULL, S(a[0].l));
  if (m->ret == 'L') {
    if (v && strstr(m->sig, ")Ljava/lang/String;") && !jni_is_string(v))
      return jv_l(NULL);
    return jv_l(v ? v : (m->nargs > 1 ? a[1].l : NULL));
  }
  jvalue bv;
  if (v && jni_unbox(v, &bv, NULL))
    return bv;
  return m->nargs > 1 ? a[1] : jv_v();
}
JH(bundle_containsKey) {
  JUNUSED;
  return jv_z(jni_map_get_s(thiz ? jni_get_field(thiz, "map", NULL).l : NULL, S(a[0].l)) != NULL);
}
JH(bundle_init) {
  JUNUSED;
  jni_set_field(thiz, "map", jv_l(jni_map_new()));
  return jv_v();
}
JH(bundle_put) {
  JUNUSED;
  JObj *map = jni_get_field(thiz, "map", NULL).l;
  if (!map) {
    map = jni_map_new();
    jni_set_field(thiz, "map", jv_l(map));
  }
  jni_map_put(map, jni_str(S(a[0].l)), m->argt[1] == 'L' ? a[1].l : jni_box(m->argt[1], a[1]));
  return jv_v();
}

// --- Intent ---------------------------------------------------------------------------------

JH(intent_getAction) { JUNUSED; return jv_l(jni_str("android.intent.action.MAIN")); }
JH(intent_getExtra) {
  JUNUSED;
  // getStringExtra(name) -> null, getBooleanExtra(name, def) -> def, ...
  if (m->ret == 'L')
    return jv_l(NULL);
  return m->nargs > 1 ? a[1] : jv_v();
}

// --- Window / View --------------------------------------------------------------------------

JH(win_getDecorView) { JUNUSED; return jv_l(s_decor); }
JH(view_getWidth) { JUNUSED; return jv_i(screen_width); }
JH(view_getHeight) { JUNUSED; return jv_i(screen_height); }
static JObj *window_insets(void) {
  static JObj *ins;
  if (!ins)
    ins = jni_new("android/view/WindowInsets");
  return ins;
}
JH(view_getRootWindowInsets) { JUNUSED; return jv_l(window_insets()); }

// --- the surface view ------------------------------------------------------------------------
// Unity finds the SurfaceView it draws into by its resource id
// ("unitySurfaceView"), and learns the size it draws at (Screen.width and
// height, Screen.currentResolution) from that view's layout: an
// OnLayoutChangeListener, plus an OnApplyWindowInsetsListener for the safe
// area. With no view, both stayed 0x0, and the game sized everything from
// that (a 0-wide screenshot texture every frame). Android lays a view out on
// its UI thread after the listeners go on; here the main loop does it before
// the next frame (java_android_frame), and again when the screen changes.

static JObj *s_surface_view, *s_layout_view, *s_layout_listener, *s_insets_view, *s_insets_listener;

JH(view_findViewById) { JUNUSED; return jv_l(a[0].i == SURFACE_VIEW_ID ? s_surface_view : NULL); }
JH(view_addOnLayoutChangeListener) {
  JUNUSED;
  s_layout_view = thiz;
  s_layout_listener = a[0].l;
  s_layout_pending = 1;
  return jv_v();
}
JH(view_removeOnLayoutChangeListener) {
  JUNUSED;
  if (a[0].l == s_layout_listener)
    s_layout_listener = NULL;
  return jv_v();
}
JH(view_setOnApplyWindowInsetsListener) {
  JUNUSED;
  s_insets_view = thiz;
  s_insets_listener = a[0].l;
  s_layout_pending = 1;
  return jv_v();
}
JH(view_requestLayout) { JUNUSED; s_layout_pending = 1; return jv_v(); }
JH(view_getDisplay) { JUNUSED; return jv_l(s_display); }
JH(view_onApplyWindowInsets) { JUNUSED; return jv_l(a[0].l); }

// --- work for the next frame -------------------------------------------------------------
// What Android delivers later on its UI thread (a permission request's
// result) runs from the main loop before the next frame.

typedef struct {
  void (*fn)(void *arg);
  void *arg;
} Later;

static Later s_later[32];
static int s_nlater;
static Mutex s_later_lock;

void java_android_later(void (*fn)(void *arg), void *arg) {
  mutexLock(&s_later_lock);
  if (s_nlater < 32)
    s_later[s_nlater++] = (Later){ fn, arg };
  else
    log_printf("[android] too much work for the next frame: dropped");
  mutexUnlock(&s_later_lock);
}

static void run_later(void) {
  Later now[32];
  mutexLock(&s_later_lock);
  const int n = s_nlater;
  memcpy(now, s_later, sizeof(Later) * (size_t)n);
  s_nlater = 0;
  mutexUnlock(&s_later_lock);
  for (int i = 0; i < n; i++)
    now[i].fn(now[i].arg);
}

void java_android_frame(void) {
  run_later();
  if (!s_layout_pending)
    return;
  s_layout_pending = 0;
  if (s_insets_listener) {
    JObj *args[2] = { s_insets_view, window_insets() };
    jni_proxy_invoke_as(s_insets_listener, "android/view/View$OnApplyWindowInsetsListener", "onApplyWindowInsets",
                        "(Landroid/view/View;Landroid/view/WindowInsets;)Landroid/view/WindowInsets;", args, 2);
  }
  if (s_layout_listener) {
    static int old_w, old_h;
    const int v[8] = { 0, 0, screen_width, screen_height, 0, 0, old_w, old_h };
    JObj *args[9] = { s_layout_view };
    for (int i = 0; i < 8; i++)
      args[1 + i] = jni_box('I', jv_i(v[i]));
    log_printf("[android] layout: the surface view is %dx%d", screen_width, screen_height);
    jni_proxy_invoke_as(s_layout_listener, "android/view/View$OnLayoutChangeListener", "onLayoutChange",
                        "(Landroid/view/View;IIIIIIII)V", args, 9);
    old_w = screen_width;
    old_h = screen_height;
  }
}
JH(insets_zero) { JUNUSED; return jv_i(0); }
JH(win_getAttributes) {
  JUNUSED;
  static JObj *lp;
  if (!lp)
    lp = jni_new("android/view/WindowManager$LayoutParams");
  return jv_l(lp);
}

// --- Build / Locale / TimeZone -------------------------------------------------------------

JH(locale_getDefault) { JUNUSED; return jv_l(s_locale); }
JH(locale_getLanguage) { JUNUSED; return jv_l(jni_get_field(thiz, "language", NULL).l); }
JH(locale_getCountry) { JUNUSED; return jv_l(jni_get_field(thiz, "country", NULL).l); }
JH(locale_toString) {
  JUNUSED;
  return jv_l(jni_strf("%s_%s", S(jni_get_field(thiz, "language", NULL).l),
                       S(jni_get_field(thiz, "country", NULL).l)));
}
JH(locale_toLanguageTag) {
  JUNUSED;
  return jv_l(jni_strf("%s-%s", S(jni_get_field(thiz, "language", NULL).l),
                       S(jni_get_field(thiz, "country", NULL).l)));
}
JH(locale_getDisplayLanguage) {
  JUNUSED;
  static const char *names[][2] = {
    { "en", "English" }, { "ja", "日本語" }, { "fr", "français" }, { "de", "Deutsch" },
    { "es", "español" }, { "it", "italiano" }, { "nl", "Nederlands" }, { "pt", "português" },
    { "ru", "русский" }, { "ko", "한국어" }, { "zh", "中文" },
  };
  const char *l = S(jni_get_field(thiz, "language", NULL).l);
  for (unsigned i = 0; i < sizeof(names) / sizeof(*names); i++)
    if (!strcmp(l, names[i][0]))
      return jv_l(jni_str(names[i][1]));
  return jv_l(jni_str(l));
}
JH(locale_init) {
  JUNUSED;
  jni_set_field(thiz, "language", jv_l(jni_str(m->nargs > 0 ? S(a[0].l) : "")));
  jni_set_field(thiz, "country", jv_l(jni_str(m->nargs > 1 ? S(a[1].l) : "")));
  return jv_v();
}

static JObj *s_tz;
JH(tz_getDefault) { JUNUSED; return jv_l(s_tz); }
JH(tz_getID) { JUNUSED; return jv_l(jni_str("UTC")); }
JH(tz_getOffset) { JUNUSED; return jv_i(0); }

// --- system services ------------------------------------------------------------------------

static JObj *s_netinfo, *s_network, *s_netcaps;
JH(cm_getActiveNetworkInfo) { JUNUSED; return jv_l(s_netinfo); }
JH(cm_getActiveNetwork) { JUNUSED; return jv_l(s_network); }
JH(cm_getNetworkCapabilities) { JUNUSED; return jv_l(s_netcaps); }
JH(ni_getType) { JUNUSED; return jv_i(1); } // TYPE_WIFI
JH(ni_getTypeName) { JUNUSED; return jv_l(jni_str("WIFI")); }
JH(caps_has) {
  JUNUSED;
  // hasTransport(TRANSPORT_WIFI=1) / hasCapability(INTERNET=12, VALIDATED=16, ...)
  if (!strcmp(m->name, "hasTransport"))
    return jv_z(a[0].i == 1);
  return jv_z(1);
}

JH(audio_getProperty) {
  JUNUSED;
  const char *k = S(a[0].l);
  if (strstr(k, "SAMPLE_RATE"))
    return jv_l(jni_str("48000"));
  if (strstr(k, "FRAMES_PER_BUFFER"))
    return jv_l(jni_str("256"));
  return jv_l(NULL);
}
JH(audio_getStreamVolume) { JUNUSED; return jv_i(10); }
JH(audio_getStreamMaxVolume) { JUNUSED; return jv_i(15); }
JH(audio_requestAudioFocus) { JUNUSED; return jv_i(1); } // AUDIOFOCUS_REQUEST_GRANTED
JH(audio_getDevices) { JUNUSED; return jv_l(jni_array('L', 0, "android/media/AudioDeviceInfo")); }
JH(audio_getRingerMode) { JUNUSED; return jv_i(2); }

JH(pow_newWakeLock) { JUNUSED; return jv_l(jni_new("android/os/PowerManager$WakeLock")); }

JH(am_getMemoryClass) { JUNUSED; return jv_i(512); }
JH(am_getMemoryInfo) {
  JUNUSED;
  if (a[0].l) {
    jni_set_field(a[0].l, "totalMem", jv_j(3ll << 30));
    jni_set_field(a[0].l, "availMem", jv_j(2ll << 30));
    jni_set_field(a[0].l, "threshold", jv_j(256ll << 20));
    jni_set_field(a[0].l, "lowMemory", jv_z(0));
  }
  return jv_v();
}

JH(tel_getString) { JUNUSED; return jv_l(jni_str("")); }
JH(tel_getInt) { JUNUSED; return jv_i(0); }

// As on Android 8 and later (one per app and device): random, made on first
// use and kept in internal/android_id.txt
const char *java_android_id(void) {
  static char id[17];
  if (id[0])
    return id;
  char path[600];
  snprintf(path, sizeof path, "%s/android_id.txt", path_internal());
  FILE *f = fopen(path, "r");
  if (f) {
    if (!fgets(id, sizeof id, f) || strlen(id) != 16)
      id[0] = '\0';
    fclose(f);
  }
  if (!id[0]) {
    u64 v;
    randomGet(&v, sizeof v);
    snprintf(id, sizeof id, "%016llx", (unsigned long long)v);
    if ((f = fopen(path, "w"))) {
      fprintf(f, "%s\n", id);
      fclose(f);
    }
  }
  return id;
}

JH(settings_getString) {
  JUNUSED;
  const char *k = S(a[1].l);
  if (!strcmp(k, "android_id"))
    return jv_l(jni_str(java_android_id()));
  return jv_l(NULL);
}
JH(settings_getInt) {
  JUNUSED;
  const char *k = S(a[1].l);
  if (!strcmp(k, "accelerometer_rotation"))
    return jv_i(0);
  return m->nargs > 2 ? a[2] : jv_i(0);
}

JH(clock_elapsedRealtime) { JUNUSED; return jv_j(android_monotonic_ns() / 1000000); }
JH(clock_elapsedRealtimeNanos) { JUNUSED; return jv_j(android_monotonic_ns()); }
JH(proc_myPid) { JUNUSED; return jv_i(1000); }
JH(proc_myUid) { JUNUSED; return jv_i(10123); }

JH(env_getExternalStorageDirectory) { JUNUSED; return jv_l(file_new(path_ext())); }
JH(env_getExternalStorageState) { JUNUSED; return jv_l(jni_str("mounted")); }
JH(env_getDataDirectory) { JUNUSED; return jv_l(file_new(path_data())); }

JH(uuid_randomUUID) {
  JUNUSED;
  uint8_t r[16];
  randomGet(r, sizeof r);
  r[6] = (r[6] & 0x0f) | 0x40;
  r[8] = (r[8] & 0x3f) | 0x80;
  JObj *u = jni_new("java/util/UUID");
  jni_set_field(u, "str", jv_l(jni_strf(
      "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x", r[0], r[1], r[2],
      r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15])));
  return jv_l(u);
}
JH(field_str) { JUNUSED; return jni_get_field(thiz, "str", NULL); }

// --- Looper / Handler / Message / HandlerThread / Choreographer ----------------------
// A HandlerThread is a real thread (made like the game's own, through the
// pthread wrapper) running its Looper: a message queue in time order, and the
// frame callbacks of that thread's Choreographer, run once per display frame:
// every 1/60 s on System.nanoTime's clock, which the frame times handed to
// doFrame are on too (Android's vsync timestamps).
//
// Unity paces its frames with one, UnityChoreographer: its constructor
// starts the thread, sends it message 0 and waits until handleMessage(0)
// there has fetched Choreographer.getInstance(); message 1 posts its frame
// callback, 2 removes it (pause), 3 drops the Choreographer (shutdown). Each
// doFrame counts a frame and posts itself again, and the engine's main loop
// waits for that count before every frame.
//
// The main looper has no thread of its own (the engine's main thread is
// ours): what is posted or sent to it runs at once on the caller, as
// runOnUiThread does. A Choreographer asked for on a thread that runs no
// looper gets an internal "Choreographer" looper thread for its callbacks.

#define FRAME_HZ 60
#define HANDLER_CALLBACK "android/os/Handler$Callback"
#define FRAME_CALLBACK "android/view/Choreographer$FrameCallback"

typedef struct QMsg {
  struct QMsg *next;
  int64_t when;          // uptime ns; 0: the front of the queue
  JObj *handler;         // the target
  JObj *msg;             // a sent Message, NULL for a posted Runnable
  JObj *run;             // the posted Runnable
  JObj *token;           // its token (postDelayed/postAtTime)
} QMsg;

typedef struct FrameCb {
  struct FrameCb *next;
  int64_t due;           // the frame (start, ns) it runs in, or a later one
  JObj *cb;              // a Choreographer$FrameCallback
} FrameCb;

typedef struct JLooper {
  JObj *obj;             // its android.os.Looper
  JObj *thread;          // its HandlerThread; NULL: the main looper
  char name[64];
  int prio;              // the HandlerThread's Android priority
  int tid;
  Mutex m;
  CondVar cv;            // the queue or the callbacks changed, it quit, or it exited
  QMsg *queue;           // by time, first come first among equal times
  FrameCb *frames;       // in posting order
  JObj *choreo;          // its Choreographer, made by the first getInstance()
  JObj *frame_box;       // doFrame's argument (a Long) and the Object[] holding
  JObj *frame_args;      //   it: made once, reused every frame
  int64_t frame_time;    // the frame last run (getFrameTimeNanos)
  unsigned nframes;
  int started, quitting, exited, raised;
  int64_t quit_at;       // quitSafely(): what is due by then still runs; -1: nothing
} JLooper;

static JLooper s_main_looper;
static __thread JLooper *t_looper;   // the looper this thread runs (NULL: none)
static JLooper *s_choreo_looper;     // runs the Choreographer of threads without one
static Mutex s_choreo_lock;
static int64_t s_frame_origin;       // the start of frame 0
static int s_next_tid = 2000;

static int64_t uptime_ns(void) {
  return android_monotonic_ns();
}

// frame k starts at s_frame_origin + k/60 s
static int64_t frame_start(int64_t k) {
  return s_frame_origin + k * 1000000000ll / FRAME_HZ;
}

static int64_t frame_index(int64_t t) {
  return t > s_frame_origin ? (t - s_frame_origin) * FRAME_HZ / 1000000000ll : 0;
}

// the start of the first frame after t
static int64_t frame_after(int64_t t) {
  int64_t k = frame_index(t) + 1;
  while (frame_start(k) <= t)
    k++;
  return frame_start(k);
}

static JLooper *looper_new(const char *name, JObj *thread) {
  JLooper *L = calloc(1, sizeof(*L));
  snprintf(L->name, sizeof L->name, "%s", name && name[0] ? name : "HandlerThread");
  mutexInit(&L->m);
  condvarInit(&L->cv);
  L->obj = jni_new("android/os/Looper");
  L->obj->native = L;
  L->thread = thread;
  thread->native = L;
  L->tid = __atomic_add_fetch(&s_next_tid, 1, __ATOMIC_RELAXED);
  return L;
}

static JLooper *as_looper(JObj *o) {
  return o && jni_instance_of(o, "android/os/Looper") ? o->native : NULL;
}

static JLooper *as_thread(JObj *o) {
  return o && jni_instance_of(o, "android/os/HandlerThread") ? o->native : NULL;
}

static JLooper *as_choreo(JObj *o) {
  return o && jni_instance_of(o, "android/view/Choreographer") ? o->native : NULL;
}

static JLooper *handler_looper(JObj *h) {
  JLooper *L = as_looper(jni_get_field(h, "mLooper", NULL).l);
  return L ? L : &s_main_looper;
}

// one step above the looper thread's own priority, so a busy core does not
// hold a frame back (Unity raises its choreographer thread the same way, but
// setpriority is a no-op here). Only from the looper's own thread.
static void looper_raise(JLooper *L) {
  if (L != t_looper || L->raised)
    return;
  L->raised = 1;
  s32 p = 0;
  if (R_SUCCEEDED(svcGetThreadPriority(&p, CUR_THREAD_HANDLE)) && p > 0x1c)
    svcSetThreadPriority(CUR_THREAD_HANDLE, (u32)p - 1);
}

static void looper_exception(JLooper *L, const char *what) {
  JObj *e = jni_take_pending();
  if (e)
    log_printf("[android] \"%s\": %s left a %s pending (%s): cleared", L->name, what,
               jni_class_name(e), jni_throwable_msg(e) ? jni_throwable_msg(e) : "");
}

// Handler.dispatchMessage: the Message's (or the post's) Runnable, else the
// Handler's Callback -- a proxy, the engine's or C#'s. A Handler without one
// would run its own handleMessage, which no Java code here overrides.
static void deliver(JObj *h, JObj *msg, JObj *run) {
  if (!run && msg)
    run = jni_get_field(msg, "callback", NULL).l;
  if (run) {
    run_runnable(run);
    return;
  }
  JObj *cb = h ? jni_get_field(h, "mCallback", NULL).l : NULL;
  if (config.log_jni)
    log_printf("[android] \"%s\": message %d to a %s", t_looper ? t_looper->name : "main",
               jni_field_i(msg, "what"), cb ? jni_class_name(cb) : "Handler without a callback");
  if (jni_is_proxy(cb)) {
    JObj *arg = msg;
    jni_proxy_invoke_as(cb, HANDLER_CALLBACK, "handleMessage", "(Landroid/os/Message;)Z", &arg, 1);
    return;
  }
  static int dropped;
  if (dropped++ < 5)
    log_printf("[android] message %d for a Handler without a callback (%s) is dropped",
               jni_field_i(msg, "what"), cb ? jni_class_name(cb) : "none");
}

// queue a Message or a Runnable for Handler h at time when (uptime ns; 0:
// the front). The main looper runs it now. 0 when h's looper has quit.
static int enqueue(JObj *h, JObj *msg, JObj *run, JObj *token, int64_t when) {
  JLooper *L = handler_looper(h);
  if (!L->thread) {
    deliver(h, msg, run);
    return 1;
  }
  QMsg *q = calloc(1, sizeof(*q));
  q->when = when;
  q->handler = h;
  q->msg = msg;
  q->run = run;
  q->token = token;
  mutexLock(&L->m);
  if (L->quitting) {
    mutexUnlock(&L->m);
    free(q);
    log_printf("[android] \"%s\" has quit: a message for it is dropped", L->name);
    return 0;
  }
  QMsg **pp = &L->queue;
  while (when != 0 && *pp && (*pp)->when <= when)
    pp = &(*pp)->next;
  q->next = *pp;
  *pp = q;
  condvarWakeAll(&L->cv);
  mutexUnlock(&L->m);
  return 1;
}

// the entries of h's queue that match: what (when has_what), the Runnable
// (non-NULL), the object (msg.obj or the Runnable's token; non-NULL). They
// are removed when remove is set. Returns how many matched.
static int queue_scan(JObj *h, int has_what, int what, JObj *run, JObj *obj, int remove) {
  JLooper *L = handler_looper(h);
  if (!L->thread)
    return 0;
  int n = 0;
  QMsg *gone = NULL;
  mutexLock(&L->m);
  for (QMsg **pp = &L->queue; *pp;) {
    QMsg *q = *pp;
    const int qwhat = q->msg ? jni_field_i(q->msg, "what") : 0;
    JObj *qobj = q->msg ? jni_get_field(q->msg, "obj", NULL).l : q->token;
    JObj *qrun = q->run ? q->run : jni_get_field(q->msg, "callback", NULL).l;
    if (q->handler == h && (!has_what || qwhat == what) && (!run || qrun == run) &&
        (!obj || qobj == obj)) {
      n++;
      if (remove) {
        *pp = q->next;
        q->next = gone;
        gone = q;
        continue;
      }
    }
    pp = &q->next;
  }
  mutexUnlock(&L->m);
  while (gone) {
    QMsg *q = gone;
    gone = q->next;
    free(q);
  }
  return n;
}

// the frame callbacks due by now, each called once with this frame's start;
// what they post goes to the next frame. Called and returns with L->m held.
static void run_frame(JLooper *L, int64_t now) {
  FrameCb *due = NULL, **tail = &due;
  for (FrameCb **pp = &L->frames; *pp;) {
    FrameCb *f = *pp;
    if (f->due <= now) {
      *pp = f->next;
      f->next = NULL;
      *tail = f;
      tail = &f->next;
    } else {
      pp = &f->next;
    }
  }
  const int64_t t = frame_start(frame_index(now));
  L->frame_time = t;
  const unsigned n = ++L->nframes;
  mutexUnlock(&L->m);
  if (n == 1)
    log_printf("[android] \"%s\": first frame callback", L->name);
  if (!L->frame_args) {
    L->frame_box = jni_box('J', jv_j(t));
    L->frame_args = jni_array('L', 1, "java/lang/Object");
    ((JObj **)jni_arr(L->frame_args)->data)[0] = L->frame_box;
  }
  while (due) {
    FrameCb *f = due;
    due = f->next;
    if (jni_is_proxy(f->cb)) {
      ((JBoxed *)L->frame_box->native)->v.j = t;
      jni_proxy_call(f->cb, FRAME_CALLBACK, "doFrame", "(J)V", L->frame_args);
      looper_exception(L, "doFrame");
    } else {
      static int warned;
      if (!warned++)
        log_printf("[android] a %s posted as a frame callback is not called", jni_class_name(f->cb));
    }
    free(f);
  }
  mutexLock(&L->m);
}

// the HandlerThread: messages as they fall due, frame callbacks at each frame
static void *looper_main(void *arg) {
  JLooper *L = arg;
  t_looper = L;
  if (L->prio <= -4) // THREAD_PRIORITY_DISPLAY or more urgent
    looper_raise(L);
  log_printf("[android] HandlerThread \"%s\" running", L->name);
  mutexLock(&L->m);
  for (;;) {
    const int64_t now = uptime_ns();
    QMsg *q = L->queue;
    if (L->quitting && (!q || q->when > L->quit_at))
      break;
    if (q && q->when <= now) {
      L->queue = q->next;
      mutexUnlock(&L->m);
      deliver(q->handler, q->msg, q->run);
      looper_exception(L, "a message");
      free(q);
      mutexLock(&L->m);
      continue;
    }
    int64_t next = INT64_MAX;
    if (!L->quitting)
      for (FrameCb *f = L->frames; f; f = f->next)
        if (f->due < next)
          next = f->due;
    if (next <= now) {
      run_frame(L, now);
      continue;
    }
    if (q && q->when < next)
      next = q->when;
    if (next == INT64_MAX)
      condvarWait(&L->cv, &L->m);
    else
      condvarWaitTimeout(&L->cv, &L->m, (u64)(next - now));
  }
  // what is left is dropped, as by Looper.quit()
  QMsg *q = L->queue;
  FrameCb *f = L->frames;
  L->queue = NULL;
  L->frames = NULL;
  L->exited = 1;
  condvarWakeAll(&L->cv);
  mutexUnlock(&L->m);
  while (q) {
    QMsg *qn = q->next;
    free(q);
    q = qn;
  }
  while (f) {
    FrameCb *fn = f->next;
    free(f);
    f = fn;
  }
  log_printf("[android] HandlerThread \"%s\" quit (%u frames)", L->name, L->nframes);
  t_looper = NULL;
  return NULL;
}

static void looper_start(JLooper *L) {
  mutexLock(&L->m);
  const int first = !L->started;
  L->started = 1;
  mutexUnlock(&L->m);
  if (!first)
    return;
  pthread_t pt;
  const int r = pthread_create_soloader(&pt, NULL, looper_main, L);
  if (r != 0) {
    log_printf("[android] HandlerThread \"%s\" could not start (error %d)", L->name, r);
    mutexLock(&L->m);
    L->exited = 1;
    condvarWakeAll(&L->cv);
    mutexUnlock(&L->m);
    return;
  }
  pthread_detach_soloader(pt);
}

static void looper_quit(JLooper *L, int safely) {
  if (!L->thread) {
    log_printf("[android] the main looper is asked to quit: ignored");
    return;
  }
  mutexLock(&L->m);
  if (!L->quitting) {
    L->quitting = 1;
    L->quit_at = safely ? uptime_ns() : -1;
  }
  condvarWakeAll(&L->cv);
  mutexUnlock(&L->m);
}

// Looper

JH(looper_myLooper) { JUNUSED; return jv_l(t_looper ? t_looper->obj : s_looper); }
JH(looper_getMainLooper) { JUNUSED; return jv_l(s_looper); }
JH(looper_quit_h) {
  JUNUSED;
  JLooper *L = as_looper(thiz);
  if (L)
    looper_quit(L, !strcmp(m->name, "quitSafely"));
  return jv_v();
}
JH(looper_isCurrentThread) {
  JUNUSED;
  return jv_z(as_looper(thiz) == (t_looper ? t_looper : &s_main_looper));
}

// Message: fields what, arg1, arg2, obj, target (the Handler), callback (a
// Runnable), data (a Bundle), read with GetIntField & co. by name

static JObj *msg_new(JObj *target) {
  JObj *msg = jni_new("android/os/Message");
  jni_set_field(msg, "what", jv_i(0));
  jni_set_field(msg, "arg1", jv_i(0));
  jni_set_field(msg, "arg2", jv_i(0));
  jni_set_field(msg, "obj", jv_l(NULL));
  jni_set_field(msg, "target", jv_l(target));
  return msg;
}

// what, arg1, arg2 from the int arguments from index first on, in order; obj
// from an object one
static void msg_fill(JObj *msg, const JMethod *m, const jvalue *a, int first) {
  static const char *const ints[] = { "what", "arg1", "arg2" };
  int n = 0;
  for (int i = first; i < m->nargs; i++) {
    if (m->argt[i] == 'I' && n < 3)
      jni_set_field(msg, ints[n++], jv_i(a[i].i));
    else if (m->argt[i] == 'L')
      jni_set_field(msg, "obj", jv_l(a[i].l));
  }
}

JH(msg_obtain) {
  JUNUSED;
  // Message.obtain(), obtain(Message), obtain(Handler[, what[, obj] |
  // what, arg1, arg2[, obj]]), obtain(Handler, Runnable)
  if (!strncmp(m->sig, "(Landroid/os/Message;", 21)) {
    static const char *const copy[] = { "what", "arg1", "arg2", "obj", "callback", "data" };
    JObj *src = a[0].l;
    JObj *msg = msg_new(jni_get_field(src, "target", NULL).l);
    for (unsigned i = 0; i < sizeof(copy) / sizeof(*copy); i++) {
      int found;
      const jvalue v = jni_get_field(src, copy[i], &found);
      if (found)
        jni_set_field(msg, copy[i], v);
    }
    return jv_l(msg);
  }
  const int has_h = !strncmp(m->sig, "(Landroid/os/Handler;", 21);
  JObj *msg = msg_new(has_h ? a[0].l : NULL);
  if (has_h && strstr(m->sig, "Ljava/lang/Runnable;"))
    jni_set_field(msg, "callback", jv_l(a[1].l));
  else
    msg_fill(msg, m, a, has_h);
  return jv_l(msg);
}
JH(msg_sendToTarget) {
  JUNUSED;
  JObj *h = jni_get_field(thiz, "target", NULL).l;
  if (h)
    enqueue(h, thiz, NULL, NULL, uptime_ns());
  else
    log_printf("[android] Message.sendToTarget() without a target");
  return jv_v();
}
JH(msg_getTarget) { JUNUSED; return jv_l(jni_get_field(thiz, "target", NULL).l); }
JH(msg_setTarget) { JUNUSED; jni_set_field(thiz, "target", jv_l(a[0].l)); return jv_v(); }
JH(msg_getCallback) { JUNUSED; return jv_l(jni_get_field(thiz, "callback", NULL).l); }
JH(msg_setData) { JUNUSED; jni_set_field(thiz, "data", jv_l(a[0].l)); return jv_v(); }
JH(msg_getData) {
  JUNUSED;
  // getData() makes the Bundle, peekData() does not
  JObj *b = jni_get_field(thiz, "data", NULL).l;
  if (!b && !strcmp(m->name, "getData")) {
    b = jni_new("android/os/Bundle");
    jni_set_field(b, "map", jv_l(jni_map_new()));
    jni_set_field(thiz, "data", jv_l(b));
  }
  return jv_l(b);
}

// Handler: fields mLooper and mCallback

JH(handler_init) {
  JUNUSED;
  // Handler([Looper][, Callback][, boolean async]); no Looper: this thread's
  JObj *looper = NULL, *cb = NULL;
  for (int i = 0; i < m->nargs; i++) {
    JObj *o = m->argt[i] == 'L' ? a[i].l : NULL;
    if (as_looper(o))
      looper = o;
    else if (o)
      cb = o;
  }
  jni_set_field(thiz, "mLooper", jv_l(looper ? looper : t_looper ? t_looper->obj : s_looper));
  jni_set_field(thiz, "mCallback", jv_l(cb));
  return jv_v();
}
JH(handler_getLooper) { JUNUSED; return jv_l(handler_looper(thiz)->obj); }

// when a send.../post... call asks for: ...Delayed(.., long ms) from now,
// ...AtTime(.., long uptimeMillis), ...AtFrontOfQueue first, else now
static int64_t when_of(const JMethod *m, const jvalue *a) {
  if (strstr(m->name, "AtFrontOfQueue"))
    return 0;
  const int last = m->nargs - 1;
  if (last > 0 && m->argt[last] == 'J') {
    const int64_t ms = a[last].j > 0 ? a[last].j : 0;
    if (strstr(m->name, "AtTime"))
      return ms * 1000000ll;
    if (strstr(m->name, "Delayed"))
      return uptime_ns() + ms * 1000000ll;
  }
  return uptime_ns();
}

JH(handler_obtainMessage) {
  JUNUSED;
  // obtainMessage([what[, obj] | what, arg1, arg2[, obj]])
  JObj *msg = msg_new(thiz);
  msg_fill(msg, m, a, 0);
  return jv_l(msg);
}
JH(handler_sendMessage) {
  JUNUSED;
  // sendMessage(msg), ...Delayed(msg, ms), ...AtTime(msg, uptimeMs), ...AtFrontOfQueue(msg)
  JObj *msg = a[0].l;
  if (!msg)
    return jv_z(0);
  jni_set_field(msg, "target", jv_l(thiz));
  return jv_z(enqueue(thiz, msg, NULL, NULL, when_of(m, a)));
}
JH(handler_sendEmptyMessage) {
  JUNUSED;
  // sendEmptyMessage(what), ...Delayed(what, ms), ...AtTime(what, uptimeMs)
  JObj *msg = msg_new(thiz);
  jni_set_field(msg, "what", jv_i(a[0].i));
  return jv_z(enqueue(thiz, msg, NULL, NULL, when_of(m, a)));
}
JH(handler_postRunnable) {
  JUNUSED;
  // post(r), postDelayed(r[, token], ms), postAtTime(r[, token], uptimeMs),
  // postAtFrontOfQueue(r)
  if (!a[0].l)
    return jv_z(0);
  JObj *token = m->nargs > 1 && m->argt[1] == 'L' ? a[1].l : NULL;
  return jv_z(enqueue(thiz, NULL, a[0].l, token, when_of(m, a)));
}
JH(handler_scan) {
  JUNUSED;
  // removeMessages(what[, obj]), removeCallbacks(r[, token]),
  // removeCallbacksAndMessages(token, null: all), hasMessages(what[, obj]),
  // hasCallbacks(r)
  const int has = !strncmp(m->name, "has", 3);
  const int by_what = m->nargs > 0 && m->argt[0] == 'I';
  JObj *run = NULL, *obj = NULL;
  if (by_what) {
    obj = m->nargs > 1 ? a[1].l : NULL;
  } else if (!strcmp(m->name, "removeCallbacksAndMessages")) {
    obj = m->nargs > 0 ? a[0].l : NULL;
  } else {
    run = m->nargs > 0 ? a[0].l : NULL;
    obj = m->nargs > 1 ? a[1].l : NULL;
    if (!run)
      return has ? jv_z(0) : jv_v();
  }
  const int n = queue_scan(thiz, by_what, by_what ? a[0].i : 0, run, obj, !has);
  return has ? jv_z(n > 0) : jv_v();
}
JH(handler_dispatchMessage) { JUNUSED; deliver(thiz, a[0].l, NULL); return jv_v(); }

// HandlerThread, and the java.lang.Thread methods the engine calls on one
// (it looks them up on java/lang/Thread; other Threads are not run here)

JH(ht_init) {
  JUNUSED;
  // HandlerThread(String name[, int priority])
  JLooper *L = looper_new(m->nargs > 0 ? S(a[0].l) : "", thiz);
  L->prio = m->nargs > 1 ? a[1].i : 0;
  return jv_v();
}
JH(ht_getLooper) {
  JUNUSED;
  // Android waits for the thread to run first; this Looper exists already
  JLooper *L = as_thread(thiz);
  return jv_l(L ? L->obj : NULL);
}
JH(ht_quit) {
  JUNUSED;
  JLooper *L = as_thread(thiz);
  if (L)
    looper_quit(L, !strcmp(m->name, "quitSafely"));
  return jv_z(L != NULL);
}
JH(ht_getThreadId) { JUNUSED; JLooper *L = as_thread(thiz); return jv_i(L ? L->tid : -1); }
JH(ht_getName) { JUNUSED; JLooper *L = as_thread(thiz); return jv_l(jni_str(L ? L->name : "main")); }

JH(thread_start) {
  JUNUSED;
  JLooper *L = as_thread(thiz);
  if (L)
    looper_start(L);
  else
    log_printf("[android] Thread.start() on a %s: not run (only HandlerThreads run here)",
               jni_class_name(thiz));
  return jv_v();
}
JH(thread_join) {
  JUNUSED;
  // join([long millis]): until a HandlerThread has quit
  JLooper *L = as_thread(thiz);
  if (!L || L == t_looper)
    return jv_v();
  const int64_t ms = m->nargs > 0 && m->argt[0] == 'J' ? a[0].j : 0;
  const int64_t end = uptime_ns() + ms * 1000000ll;
  mutexLock(&L->m);
  while (L->started && !L->exited) {
    const int64_t now = uptime_ns();
    if (ms <= 0)
      condvarWait(&L->cv, &L->m);
    else if (now < end)
      condvarWaitTimeout(&L->cv, &L->m, (u64)(end - now));
    else
      break;
  }
  mutexUnlock(&L->m);
  return jv_v();
}
JH(thread_isAlive) {
  JUNUSED;
  JLooper *L = as_thread(thiz);
  if (!L)
    return jv_z(1); // Thread.currentThread()
  mutexLock(&L->m);
  const int alive = L->started && !L->exited;
  mutexUnlock(&L->m);
  return jv_z(alive);
}

// Choreographer: one per looper; native = that JLooper

JH(choreo_getInstance) {
  JUNUSED;
  JLooper *L = t_looper;
  if (!L) {
    mutexLock(&s_choreo_lock);
    if (!s_choreo_looper) {
      s_choreo_looper = looper_new("Choreographer", jni_new("android/os/HandlerThread"));
      s_choreo_looper->prio = -4; // THREAD_PRIORITY_DISPLAY
      looper_start(s_choreo_looper);
      log_printf("[android] Choreographer.getInstance() on a thread without a looper: its "
                 "frame callbacks run on an internal \"Choreographer\" thread");
    }
    L = s_choreo_looper;
    mutexUnlock(&s_choreo_lock);
  }
  mutexLock(&L->m);
  const int made = !L->choreo;
  if (made) {
    L->choreo = jni_new("android/view/Choreographer");
    L->choreo->native = L;
  }
  JObj *c = L->choreo;
  mutexUnlock(&L->m);
  if (made) {
    looper_raise(L);
    log_printf("[android] Choreographer on \"%s\": frame callbacks every 1/%d s", L->name, FRAME_HZ);
  }
  return jv_l(c);
}
JH(choreo_postFrameCallback) {
  JUNUSED;
  // postFrameCallback(cb), postFrameCallbackDelayed(cb, delayMillis): in the
  // first frame after now (and the delay)
  JLooper *L = as_choreo(thiz);
  if (!L || !a[0].l)
    return jv_v();
  int64_t at = uptime_ns();
  if (m->nargs > 1 && a[1].j > 0)
    at += a[1].j * 1000000ll;
  FrameCb *f = calloc(1, sizeof(*f));
  f->cb = a[0].l;
  f->due = frame_after(at);
  mutexLock(&L->m);
  if (L->quitting) {
    mutexUnlock(&L->m);
    free(f);
    return jv_v();
  }
  FrameCb **pp = &L->frames;
  while (*pp)
    pp = &(*pp)->next;
  *pp = f;
  condvarWakeAll(&L->cv);
  mutexUnlock(&L->m);
  return jv_v();
}
JH(choreo_removeFrameCallback) {
  JUNUSED;
  JLooper *L = as_choreo(thiz);
  if (!L)
    return jv_v();
  FrameCb *gone = NULL;
  mutexLock(&L->m);
  for (FrameCb **pp = &L->frames; *pp;) {
    FrameCb *f = *pp;
    if (f->cb == a[0].l) {
      *pp = f->next;
      f->next = gone;
      gone = f;
    } else {
      pp = &f->next;
    }
  }
  mutexUnlock(&L->m);
  while (gone) {
    FrameCb *f = gone;
    gone = f->next;
    free(f);
  }
  return jv_v();
}
JH(choreo_getFrameTime) {
  JUNUSED;
  // getFrameTimeNanos(), getFrameTime() (ms): the frame being run, or the
  // current one outside a callback
  JLooper *L = as_choreo(thiz);
  const int64_t t = L && L->frame_time ? L->frame_time : frame_start(frame_index(uptime_ns()));
  return jv_j(strcmp(m->name, "getFrameTime") ? t : t / 1000000);
}

// --- ClassLoader.findLibrary: the extracted native libraries ------------------------

JH(loader_findLibrary) {
  JUNUSED;
  char p[600];
  snprintf(p, sizeof p, "%s/lib%s.so", path_lib(), S(a[0].l));
  struct bstat st;
  return jv_l(fstat_path(p, &st) == 0 ? jni_str(p) : NULL);
}

// --- AlertDialog: shown by the engine for fatal errors -- into the log ----------------

JH(dlg_init) { JUNUSED; return jv_v(); }
JH(dlg_setTitle) {
  JUNUSED;
  jni_set_field(thiz, "title", jv_l(a[0].l));
  return jv_l(thiz);
}
JH(dlg_setMessage) {
  JUNUSED;
  jni_set_field(thiz, "message", jv_l(a[0].l));
  return jv_l(thiz);
}
JH(dlg_create) {
  JUNUSED;
  log_printf("[android] dialog: \"%s\" -- %s", S(jni_get_field(thiz, "title", NULL).l),
             S(jni_get_field(thiz, "message", NULL).l));
  return jv_l(thiz);
}

// --- android.net.Uri ----------------------------------------------------------------------
// Unity's PlayerPrefs store each key Uri.encode()d ("Screenmanager%20
// Resolution%20Width"). Unanswered, every key came back null and all of
// them landed on one empty key, Unity's saved screen size among them.

// Uri.encode(s[, allow]): UTF-8 bytes outside A-Z a-z 0-9 _-!.~'()* (and
// allow) as %XX
JH(uri_encode) {
  JUNUSED;
  const char *in = jni_utf(a[0].l);
  if (!in)
    return jv_l(NULL);
  const char *allow = m->nargs > 1 ? jni_utf(a[1].l) : NULL;
  char *out = malloc(strlen(in) * 3 + 1);
  size_t o = 0;
  for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
    if ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') ||
        strchr("_-!.~'()*", *p) || (allow && strchr(allow, *p)))
      out[o++] = (char)*p;
    else
      o += (size_t)sprintf(out + o, "%%%02X", *p);
  }
  out[o] = 0;
  JObj *r = jni_str(out);
  free(out);
  return jv_l(r);
}

// Uri.decode(s): %XX back to bytes ('+' stays a '+', as on Android)
JH(uri_decode) {
  JUNUSED;
  const char *in = jni_utf(a[0].l);
  if (!in)
    return jv_l(NULL);
  char *out = malloc(strlen(in) + 1);
  size_t o = 0;
  for (const char *p = in; *p; p++) {
    unsigned v;
    if (*p == '%' && p[1] && p[2] && sscanf(p + 1, "%2x", &v) == 1) {
      out[o++] = (char)v;
      p += 2;
    } else {
      out[o++] = *p;
    }
  }
  out[o] = 0;
  JObj *r = jni_str(out);
  free(out);
  return jv_l(r);
}

// --- registration ------------------------------------------------------------------------------

static JObj *obj_of(const char *cls) {
  JObj *o = jni_new(cls);
  return o;
}

// constants the engine reads (unset ones read as 0)
static void put_constants(void) {
  static const struct { const char *name; int v; } orient[] = {
    { "UNSPECIFIED", -1 }, { "LANDSCAPE", 0 }, { "PORTRAIT", 1 }, { "USER", 2 }, { "BEHIND", 3 },
    { "SENSOR", 4 }, { "NOSENSOR", 5 }, { "SENSOR_LANDSCAPE", 6 }, { "SENSOR_PORTRAIT", 7 },
    { "REVERSE_LANDSCAPE", 8 }, { "REVERSE_PORTRAIT", 9 }, { "FULL_SENSOR", 10 },
    { "USER_LANDSCAPE", 11 }, { "USER_PORTRAIT", 12 }, { "FULL_USER", 13 }, { "LOCKED", 14 },
  };
  for (size_t i = 0; i < sizeof orient / sizeof *orient; i++) {
    char n[64];
    snprintf(n, sizeof n, "SCREEN_ORIENTATION_%s", orient[i].name);
    jni_static_field("android/content/pm/ActivityInfo", n, jv_i(orient[i].v));
  }
  const char *V = "android/view/View";
  jni_static_field(V, "SYSTEM_UI_FLAG_VISIBLE", jv_i(0));
  jni_static_field(V, "SYSTEM_UI_FLAG_LOW_PROFILE", jv_i(0x1));
  jni_static_field(V, "SYSTEM_UI_FLAG_HIDE_NAVIGATION", jv_i(0x2));
  jni_static_field(V, "SYSTEM_UI_FLAG_FULLSCREEN", jv_i(0x4));
  jni_static_field(V, "SYSTEM_UI_FLAG_LAYOUT_STABLE", jv_i(0x100));
  jni_static_field(V, "SYSTEM_UI_FLAG_LAYOUT_HIDE_NAVIGATION", jv_i(0x200));
  jni_static_field(V, "SYSTEM_UI_FLAG_LAYOUT_FULLSCREEN", jv_i(0x400));
  jni_static_field(V, "SYSTEM_UI_FLAG_IMMERSIVE", jv_i(0x800));
  jni_static_field(V, "SYSTEM_UI_FLAG_IMMERSIVE_STICKY", jv_i(0x1000));
  const char *LP = "android/view/WindowManager$LayoutParams";
  jni_static_field(LP, "FLAG_KEEP_SCREEN_ON", jv_i(0x80));
  jni_static_field(LP, "FLAG_FULLSCREEN", jv_i(0x400));
  const char *AM = "android/media/AudioManager";
  jni_static_field(AM, "STREAM_MUSIC", jv_i(3));
  jni_static_field(AM, "GET_DEVICES_OUTPUTS", jv_i(2));
  jni_static_field(AM, "PROPERTY_OUTPUT_SAMPLE_RATE", jv_l(jni_str("android.media.property.OUTPUT_SAMPLE_RATE")));
  jni_static_field(AM, "PROPERTY_OUTPUT_FRAMES_PER_BUFFER",
                   jv_l(jni_str("android.media.property.OUTPUT_FRAMES_PER_BUFFER")));
  jni_static_field("android/media/MediaRouter", "ROUTE_TYPE_LIVE_VIDEO", jv_i(2));
  jni_static_field("android/content/Context", "MEDIA_ROUTER_SERVICE", jv_l(jni_str("media_router")));
}

static void put_build(void) {
  const char *b = "android/os/Build";
  jni_static_field(b, "MODEL", jv_l(jni_str("Nintendo Switch")));
  jni_static_field(b, "MANUFACTURER", jv_l(jni_str("Nintendo")));
  jni_static_field(b, "BRAND", jv_l(jni_str("Nintendo")));
  jni_static_field(b, "DEVICE", jv_l(jni_str("nx")));
  jni_static_field(b, "PRODUCT", jv_l(jni_str("nx")));
  jni_static_field(b, "HARDWARE", jv_l(jni_str("nx")));
  jni_static_field(b, "BOARD", jv_l(jni_str("nx")));
  jni_static_field(b, "ID", jv_l(jni_str("TQ3A.230901.001")));
  jni_static_field(b, "DISPLAY", jv_l(jni_str("TQ3A.230901.001")));
  jni_static_field(b, "HOST", jv_l(jni_str("localhost")));
  jni_static_field(b, "TAGS", jv_l(jni_str("release-keys")));
  jni_static_field(b, "TYPE", jv_l(jni_str("user")));
  jni_static_field(b, "USER", jv_l(jni_str("nx")));
  jni_static_field(b, "SERIAL", jv_l(jni_str("unknown")));
  jni_static_field(b, "BOOTLOADER", jv_l(jni_str("unknown")));
  jni_static_field(b, "RADIO", jv_l(jni_str("unknown")));
  jni_static_field(b, "FINGERPRINT", jv_l(jni_str("Nintendo/nx/nx:13/TQ3A.230901.001/1:user/release-keys")));
  jni_static_field(b, "CPU_ABI", jv_l(jni_str("arm64-v8a")));
  jni_static_field(b, "CPU_ABI2", jv_l(jni_str("")));
  jni_static_field(b, "SOC_MANUFACTURER", jv_l(jni_str("NVIDIA")));
  jni_static_field(b, "SOC_MODEL", jv_l(jni_str("Tegra X1")));
  static const char *abis[] = { "arm64-v8a" };
  jni_static_field(b, "SUPPORTED_ABIS", jv_l(jni_string_array(abis, 1)));
  jni_static_field(b, "SUPPORTED_64_BIT_ABIS", jv_l(jni_string_array(abis, 1)));
  jni_static_field(b, "SUPPORTED_32_BIT_ABIS", jv_l(jni_string_array(abis, 0)));
  jni_static_field(b, "TIME", jv_j(1693526400000ll));
  const char *v = "android/os/Build$VERSION";
  jni_static_field(v, "SDK_INT", jv_i(ANDROID_SDK_INT));
  jni_static_field(v, "RELEASE", jv_l(jni_str("13")));
  jni_static_field(v, "INCREMENTAL", jv_l(jni_str("1")));
  jni_static_field(v, "CODENAME", jv_l(jni_str("REL")));
  jni_static_field(v, "SDK", jv_l(jni_str("33")));
  jni_static_field(v, "SECURITY_PATCH", jv_l(jni_str("2023-09-01")));
  jni_static_field(v, "BASE_OS", jv_l(jni_str("")));
  jni_static_field(v, "PREVIEW_SDK_INT", jv_i(0));
}

void java_android_init(void) {
  mutexInit(&s_prefs_lock);
  mutexInit(&s_later_lock);

  // the class tree (Context hierarchy, the views, services)
  jni_set_super(ACTIVITY_CLASS, "com/unity3d/player/UnityPlayerActivity");
  jni_set_super("com/unity3d/player/UnityPlayerActivity", "android/app/Activity");
  jni_set_super("android/app/Activity", "android/view/ContextThemeWrapper");
  jni_set_super("android/view/ContextThemeWrapper", "android/content/ContextWrapper");
  jni_set_super("android/app/Application", "android/content/ContextWrapper");
  jni_set_super("android/content/ContextWrapper", "android/content/Context");
  jni_set_super("android/app/SharedPreferencesImpl", "android/content/SharedPreferences");
  jni_set_super("android/app/SharedPreferencesImpl$EditorImpl", "android/content/SharedPreferences$Editor");
  jni_set_super("android/view/WindowManagerImpl", "android/view/WindowManager");
  jni_set_super("com/android/internal/policy/PhoneWindow", "android/view/Window");
  jni_set_super("com/android/internal/policy/DecorView", "android/view/View");
  jni_set_super("android/app/ApplicationPackageManager", "android/content/pm/PackageManager");
  jni_set_super("android/os/SystemVibrator", "android/os/Vibrator");

  s_activity = obj_of(ACTIVITY_CLASS);
  s_app = obj_of("android/app/Application");
  s_resources = obj_of("android/content/res/Resources");
  s_config = obj_of("android/content/res/Configuration");
  s_metrics = obj_of("android/util/DisplayMetrics");
  s_display = obj_of("android/view/Display");
  s_wm = obj_of("android/view/WindowManagerImpl");
  s_pm = obj_of("android/app/ApplicationPackageManager");
  s_appinfo = obj_of("android/content/pm/ApplicationInfo");
  s_pkginfo = obj_of("android/content/pm/PackageInfo");
  s_assets = obj_of("android/content/res/AssetManager");
  s_resolver = obj_of("android/content/ContentResolver");
  s_intent = obj_of("android/content/Intent");
  s_window = obj_of("com/android/internal/policy/PhoneWindow");
  s_decor = obj_of("com/android/internal/policy/DecorView");
  s_looper = obj_of("android/os/Looper");
  s_locale = obj_of("java/util/Locale");
  s_meta = obj_of("android/os/Bundle");
  s_mode = obj_of("android/view/Display$Mode");
  s_tz = obj_of("java/util/TimeZone");
  s_netinfo = obj_of("android/net/NetworkInfo");
  s_network = obj_of("android/net/Network");
  s_netcaps = obj_of("android/net/NetworkCapabilities");
  jni_set_field(s_activity, "requestedOrientation", jv_i(12));

  // the main looper (no thread of its own), and frame 0 of every Choreographer
  jni_set_super("android/os/HandlerThread", "java/lang/Thread");
  mutexInit(&s_choreo_lock);
  mutexInit(&s_main_looper.m);
  condvarInit(&s_main_looper.cv);
  snprintf(s_main_looper.name, sizeof s_main_looper.name, "main");
  s_main_looper.obj = s_looper;
  s_looper->native = &s_main_looper;
  s_frame_origin = uptime_ns();

  jni_set_field(s_locale, "language", jv_l(jni_str(config.language)));
  jni_set_field(s_locale, "country", jv_l(jni_str(config.country)));

  // meta-data from the APK's manifest that Unity reads
  jni_set_field(s_meta, "map", jv_l(jni_map_new()));
  {
    JObj *mm = jni_get_field(s_meta, "map", NULL).l;
    jvalue t = jv_z(1), two = jv_i(2), aspect = jv_f(2.5f);
    jni_map_put(mm, jni_str("unityplayer.SkipPermissionsDialog"), jni_box('Z', t));
    jni_map_put(mm, jni_str("unity.splash-mode"), jni_box('I', two));
    jni_map_put(mm, jni_str("unity.splash-enable"), jni_box('Z', t));
    jni_map_put(mm, jni_str("unity.launch-fullscreen"), jni_box('Z', t));
    jni_map_put(mm, jni_str("unity.render-outside-safearea"), jni_box('Z', t));
    jni_map_put(mm, jni_str("unity.auto-report-fully-drawn"), jni_box('Z', t));
    jni_map_put(mm, jni_str("unityplayer.UnityActivity"), jni_box('Z', t));
    jni_map_put(mm, jni_str("notch_support"), jni_box('Z', t));
    jni_map_put(mm, jni_str("notch.config"), jni_str("portrait|landscape"));
    jni_map_put(mm, jni_str("android.max_aspect"), jni_box('F', aspect));
  }

  // ApplicationInfo / PackageInfo
  jni_set_field(s_appinfo, "packageName", jv_l(jni_str(sdt_package())));
  jni_set_field(s_appinfo, "sourceDir", jv_l(jni_str(path_apk())));
  jni_set_field(s_appinfo, "publicSourceDir", jv_l(jni_str(path_apk())));
  jni_set_field(s_appinfo, "nativeLibraryDir", jv_l(jni_str(path_lib())));
  jni_set_field(s_appinfo, "dataDir", jv_l(jni_str(path_data())));
  jni_set_field(s_appinfo, "targetSdkVersion", jv_i(33));
  jni_set_field(s_appinfo, "minSdkVersion", jv_i(26));
  jni_set_field(s_appinfo, "flags", jv_i(0x00c8be44)); // HAS_CODE|ALLOW_BACKUP|... (no DEBUGGABLE)
  jni_set_field(s_appinfo, "metaData", jv_l(s_meta));
  jni_set_field(s_appinfo, "splitSourceDirs", jv_l(NULL));
  jni_set_field(s_appinfo, "uid", jv_i(10123));
  jni_set_field(s_pkginfo, "packageName", jv_l(jni_str(sdt_package())));
  jni_set_field(s_pkginfo, "versionName", jv_l(jni_str("1.10.1")));
  jni_set_field(s_pkginfo, "versionCode", jv_i(1));
  jni_set_field(s_pkginfo, "firstInstallTime", jv_j(1700000000000ll));
  jni_set_field(s_pkginfo, "lastUpdateTime", jv_j(1700000000000ll));
  jni_set_field(s_pkginfo, "applicationInfo", jv_l(s_appinfo));

  // Configuration: landscape, touch, no keyboard
  jni_set_field(s_config, "orientation", jv_i(2));      // ORIENTATION_LANDSCAPE
  jni_set_field(s_config, "touchscreen", jv_i(3));      // TOUCHSCREEN_FINGER
  jni_set_field(s_config, "keyboard", jv_i(1));         // KEYBOARD_NOKEYS
  jni_set_field(s_config, "keyboardHidden", jv_i(2));
  jni_set_field(s_config, "hardKeyboardHidden", jv_i(2));
  jni_set_field(s_config, "navigation", jv_i(1));
  jni_set_field(s_config, "uiMode", jv_i(0x11));        // NORMAL | NIGHT_NO
  jni_set_field(s_config, "screenLayout", jv_i(0x13));  // LARGE, LONG_NO
  jni_set_field(s_config, "fontScale", jv_f(1.0f));
  jni_set_field(s_config, "locale", jv_l(s_locale));
  jni_set_field(s_config, "mcc", jv_i(0));
  jni_set_field(s_config, "mnc", jv_i(0));
  java_android_screen_changed();

  jni_set_field(s_netinfo, "connected", jv_z(1));

  put_build();
  put_constants();
  jni_method("android/net/Uri", "encode", NULL, uri_encode);
  jni_method("android/net/Uri", "decode", NULL, uri_decode);
  static const char *const services[][2] = {
    { "WINDOW_SERVICE", "window" }, { "AUDIO_SERVICE", "audio" },
    { "CONNECTIVITY_SERVICE", "connectivity" }, { "POWER_SERVICE", "power" },
    { "VIBRATOR_SERVICE", "vibrator" }, { "INPUT_SERVICE", "input" },
    { "SENSOR_SERVICE", "sensor" }, { "DISPLAY_SERVICE", "display" },
    { "ACTIVITY_SERVICE", "activity" }, { "TELEPHONY_SERVICE", "phone" },
    { "WIFI_SERVICE", "wifi" }, { "UI_MODE_SERVICE", "uimode" },
    { "STORAGE_SERVICE", "storage" }, { "BATTERY_SERVICE", "batterymanager" },
    { "CLIPBOARD_SERVICE", "clipboard" }, { "INPUT_METHOD_SERVICE", "input_method" },
    { "NOTIFICATION_SERVICE", "notification" }, { "ALARM_SERVICE", "alarm" },
    { "LOCATION_SERVICE", "location" }, { "KEYGUARD_SERVICE", "keyguard" },
    { "VIBRATOR_MANAGER_SERVICE", "vibrator_manager" }, { "GAME_SERVICE", "game" },
  };
  for (unsigned i = 0; i < sizeof(services) / sizeof(*services); i++)
    jni_static_field("android/content/Context", services[i][0], jv_l(jni_str(services[i][1])));
  jni_static_field("android/content/Context", "MODE_PRIVATE", jv_i(0));
  jni_static_field("android/os/Environment", "MEDIA_MOUNTED", jv_l(jni_str("mounted")));
  jni_static_field("android/os/Environment", "DIRECTORY_DOCUMENTS", jv_l(jni_str("Documents")));
  jni_static_field("android/os/Environment", "DIRECTORY_PICTURES", jv_l(jni_str("Pictures")));
  jni_static_field("android/os/Build$VERSION_CODES", "Q", jv_i(29));
  jni_static_field("android/os/Build$VERSION_CODES", "R", jv_i(30));

  // --- File / StatFs / Environment
  const char *F = "java/io/File";
  jni_method(F, "<init>", NULL, file_init);
  jni_method(F, "getPath", NULL, file_getPath);
  jni_method(F, "getAbsolutePath", NULL, file_getPath);
  jni_method(F, "getCanonicalPath", NULL, file_getPath);
  jni_method(F, "toString", NULL, file_getPath);
  jni_method(F, "getAbsoluteFile", NULL, file_getAbsoluteFile);
  jni_method(F, "getCanonicalFile", NULL, file_getAbsoluteFile);
  jni_method(F, "getName", NULL, file_getName);
  jni_method(F, "getParent", NULL, file_getParent);
  jni_method(F, "getParentFile", NULL, file_getParent);
  jni_method(F, "exists", NULL, file_exists);
  jni_method(F, "isDirectory", NULL, file_isDirectory);
  jni_method(F, "isFile", NULL, file_isFile);
  jni_method(F, "length", NULL, file_length);
  jni_method(F, "lastModified", NULL, file_lastModified);
  jni_method(F, "mkdir", NULL, file_mkdirs);
  jni_method(F, "mkdirs", NULL, file_mkdirs);
  jni_method(F, "delete", NULL, file_delete);
  jni_method(F, "renameTo", NULL, file_renameTo);
  jni_method(F, "createNewFile", NULL, file_createNewFile);
  jni_method(F, "list", NULL, file_list);
  jni_method(F, "listFiles", NULL, file_list);
  jni_method(F, "canRead", NULL, ret_true);
  jni_method(F, "canWrite", NULL, ret_true);
  jni_method(F, "canExecute", NULL, ret_false);
  jni_method(F, "isHidden", NULL, ret_false);
  jni_method(F, "isAbsolute", NULL, ret_true);
  jni_method(F, "setReadable", NULL, ret_true);
  jni_method(F, "setWritable", NULL, ret_true);
  jni_method(F, "setExecutable", NULL, ret_true);
  jni_method(F, "setLastModified", NULL, ret_true);
  jni_method(F, "getFreeSpace", NULL, file_getFreeSpace);
  jni_method(F, "getUsableSpace", NULL, file_getFreeSpace);
  jni_method(F, "getTotalSpace", NULL, file_getTotalSpace);
  jni_method(F, "toURI", NULL, file_toURI);
  jni_method("java/net/URI", "toString", NULL, field_str);
  jni_method("android/os/StatFs", "<init>", NULL, statfs_init);
  jni_method("android/os/StatFs", "getAvailableBytes", NULL, statfs_availableBytes);
  jni_method("android/os/StatFs", "getFreeBytes", NULL, statfs_availableBytes);
  jni_method("android/os/StatFs", "getTotalBytes", NULL, statfs_totalBytes);
  jni_method("android/os/StatFs", "getBlockSize", NULL, statfs_blockSize);
  jni_method("android/os/StatFs", "getBlockSizeLong", NULL, statfs_blockSize);
  jni_method("android/os/StatFs", "getAvailableBlocks", NULL, statfs_availBlocks);
  jni_method("android/os/StatFs", "getAvailableBlocksLong", NULL, statfs_availBlocks);
  jni_method("android/os/StatFs", "getFreeBlocksLong", NULL, statfs_availBlocks);
  jni_method("android/os/StatFs", "getBlockCount", NULL, statfs_blockCount);
  jni_method("android/os/StatFs", "getBlockCountLong", NULL, statfs_blockCount);
  const char *E = "android/os/Environment";
  jni_method(E, "getExternalStorageDirectory", NULL, env_getExternalStorageDirectory);
  jni_method(E, "getExternalStoragePublicDirectory", NULL, env_getExternalStorageDirectory);
  jni_method(E, "getExternalStorageState", NULL, env_getExternalStorageState);
  jni_method(E, "getDataDirectory", NULL, env_getDataDirectory);
  jni_method(E, "isExternalStorageEmulated", NULL, ret_true);
  jni_method(E, "isExternalStorageRemovable", NULL, ret_false);
  jni_method(E, "isExternalStorageLegacy", NULL, ret_false);
  jni_method(E, "isExternalStorageManager", NULL, ret_false);

  // --- Context / Activity
  const char *C = "android/content/Context";
  jni_method(C, "getPackageName", NULL, ctx_getPackageName);
  jni_method(C, "getOpPackageName", NULL, ctx_getPackageName);
  jni_method(C, "getPackageCodePath", NULL, ctx_getPackageCodePath);
  jni_method(C, "getPackageResourcePath", NULL, ctx_getPackageCodePath);
  jni_method(C, "getFilesDir", NULL, ctx_getFilesDir);
  jni_method(C, "getCacheDir", NULL, ctx_getCacheDir);
  jni_method(C, "getDataDir", NULL, ctx_getDataDir);
  jni_method(C, "getNoBackupFilesDir", NULL, ctx_getNoBackupFilesDir);
  jni_method(C, "getCodeCacheDir", NULL, ctx_getCodeCacheDir);
  jni_method(C, "getExternalFilesDir", NULL, ctx_getExternalFilesDir);
  jni_method(C, "getExternalFilesDirs", NULL, ctx_getExternalFilesDirs);
  jni_method(C, "getExternalCacheDir", NULL, ctx_getExternalCacheDir);
  jni_method(C, "getObbDir", NULL, ctx_getObbDir);
  jni_method(C, "getObbDirs", NULL, ctx_getObbDirs);
  jni_method(C, "getDir", NULL, ctx_getDir);
  jni_method(C, "getDatabasePath", NULL, ctx_getDatabasePath);
  jni_method(C, "getApplicationContext", NULL, ctx_getApplicationContext);
  jni_method(C, "getBaseContext", NULL, ctx_getApplicationContext);
  jni_method(C, "getApplication", NULL, ctx_getApplication);
  jni_method(C, "getResources", NULL, ctx_getResources);
  jni_method(C, "getAssets", NULL, ctx_getAssets);
  jni_method(C, "getPackageManager", NULL, ctx_getPackageManager);
  jni_method(C, "getContentResolver", NULL, ctx_getContentResolver);
  jni_method(C, "getApplicationInfo", NULL, ctx_getApplicationInfo);
  jni_method(C, "getMainLooper", NULL, ctx_getMainLooper);
  jni_method(C, "getSystemService", NULL, ctx_getSystemService);
  jni_method(C, "getSharedPreferences", NULL, ctx_getSharedPreferences);
  jni_method(C, "getPreferences", NULL, ctx_getPreferences);
  jni_method(C, "checkSelfPermission", NULL, ctx_checkPermission);
  jni_method(C, "checkPermission", NULL, ctx_checkPermission);
  jni_method(C, "checkCallingOrSelfPermission", NULL, ctx_checkPermission);
  jni_method(C, "getDisplay", NULL, ctx_getDisplay);
  jni_method(C, "startActivity", NULL, ret_void);
  jni_method(C, "sendBroadcast", NULL, ret_void);
  jni_method(C, "registerReceiver", NULL, ret_null);
  jni_method(C, "unregisterReceiver", NULL, ret_void);
  jni_method(C, "startService", NULL, ret_null);
  jni_method(C, "bindService", NULL, ret_false);
  const char *A = "android/app/Activity";
  jni_method(A, "getIntent", NULL, ctx_getIntent);
  jni_method(A, "getWindow", NULL, ctx_getWindow);
  jni_method(A, "getWindowManager", NULL, ctx_getWindowManager);
  jni_method(A, "runOnUiThread", NULL, ctx_runOnUiThread);
  jni_method(A, "isFinishing", NULL, ret_false);
  jni_method(A, "isDestroyed", NULL, ret_false);
  jni_method(A, "isChangingConfigurations", NULL, ret_false);
  jni_method(A, "hasWindowFocus", NULL, ret_true);
  jni_method(A, "isInMultiWindowMode", NULL, ret_false);
  jni_method(A, "isInPictureInPictureMode", NULL, ret_false);
  jni_method(A, "getRequestedOrientation", NULL, ctx_getRequestedOrientation);
  jni_method(A, "setRequestedOrientation", NULL, ctx_setRequestedOrientation);
  jni_method(A, "getTaskId", NULL, (JHandler)ret_zero);
  jni_method(A, "requestPermissions", NULL, ret_void);
  jni_method(A, "shouldShowRequestPermissionRationale", NULL, ret_false);
  jni_method(A, "moveTaskToBack", NULL, ret_true);
  jni_method(A, "reportFullyDrawn", NULL, ret_void);
  jni_method(A, "getCallingActivity", NULL, ret_null);
  jni_method(A, "getComponentName", NULL, ret_null);
  jni_method("android/preference/PreferenceManager", "getDefaultSharedPreferences", NULL,
             pm_getDefaultSharedPreferences);
  jni_method("java/lang/ClassLoader", "findLibrary", NULL, loader_findLibrary);
  const char *AD = "android/app/AlertDialog$Builder";
  jni_method(AD, "<init>", NULL, dlg_init);
  jni_method(AD, "setTitle", NULL, dlg_setTitle);
  jni_method(AD, "setMessage", NULL, dlg_setMessage);
  jni_method(AD, "create", NULL, dlg_create);
  jni_method(AD, "show", NULL, dlg_create);
  jni_method(AD, "setPositiveButton", NULL, ret_self);
  jni_method(AD, "setNegativeButton", NULL, ret_self);
  jni_method(AD, "setNeutralButton", NULL, ret_self);
  jni_method(AD, "setCancelable", NULL, ret_self);
  jni_method(AD, "setOnCancelListener", NULL, ret_self);
  jni_method(AD, "setOnKeyListener", NULL, ret_self);
  jni_method(AD, "setView", NULL, ret_self);
  jni_method(AD, "setItems", NULL, ret_self);

  // --- SharedPreferences
  const char *SP = "android/content/SharedPreferences";
  jni_method(SP, "getString", NULL, prefs_get_value);
  jni_method(SP, "getInt", NULL, prefs_get_value);
  jni_method(SP, "getLong", NULL, prefs_get_value);
  jni_method(SP, "getFloat", NULL, prefs_get_value);
  jni_method(SP, "getBoolean", NULL, prefs_get_value);
  jni_method(SP, "getStringSet", NULL, prefs_get_value);
  jni_method(SP, "contains", NULL, prefs_contains);
  jni_method(SP, "getAll", NULL, prefs_getAll);
  jni_method(SP, "edit", NULL, prefs_edit);
  jni_method(SP, "registerOnSharedPreferenceChangeListener", NULL, ret_void);
  jni_method(SP, "unregisterOnSharedPreferenceChangeListener", NULL, ret_void);
  const char *ED = "android/content/SharedPreferences$Editor";
  jni_method(ED, "putString", NULL, editor_put);
  jni_method(ED, "putInt", NULL, editor_put);
  jni_method(ED, "putLong", NULL, editor_put);
  jni_method(ED, "putFloat", NULL, editor_put);
  jni_method(ED, "putBoolean", NULL, editor_put);
  jni_method(ED, "putStringSet", NULL, editor_put);
  jni_method(ED, "remove", NULL, editor_remove);
  jni_method(ED, "clear", NULL, editor_clear);
  jni_method(ED, "apply", NULL, editor_commit);
  jni_method(ED, "commit", NULL, editor_commit);

  // --- Resources / display
  jni_method("android/content/res/Resources", "getConfiguration", NULL, res_getConfiguration);
  jni_method("android/content/res/Resources", "getDisplayMetrics", NULL, res_getDisplayMetrics);
  jni_method("android/content/res/Resources", "getIdentifier", NULL, res_getIdentifier);
  jni_method("android/content/res/Resources", "getString", NULL, res_getString);
  jni_method("android/content/res/Resources", "getBoolean", NULL, res_getBoolean);
  jni_method("android/content/res/Resources", "getInteger", NULL, res_getInteger);
  jni_method("android/content/res/Resources", "getSystem", NULL, ctx_getResources);
  jni_method("android/view/WindowManager", "getDefaultDisplay", NULL, wm_getDefaultDisplay);
  const char *D = "android/view/Display";
  jni_method(D, "getMetrics", NULL, disp_getMetrics);
  jni_method(D, "getRealMetrics", NULL, disp_getMetrics);
  jni_method(D, "getRotation", NULL, disp_getRotation);
  jni_method(D, "getOrientation", NULL, disp_getRotation);
  jni_method(D, "getRefreshRate", NULL, disp_getRefreshRate);
  jni_method(D, "getWidth", NULL, disp_getWidth);
  jni_method(D, "getHeight", NULL, disp_getHeight);
  jni_method(D, "getDisplayId", NULL, disp_getDisplayId);
  jni_method(D, "getSize", NULL, disp_getSize);
  jni_method(D, "getRealSize", NULL, disp_getSize);
  jni_method(D, "getMode", NULL, disp_getMode);
  jni_method(D, "getSupportedModes", NULL, disp_getSupportedModes);
  jni_method(D, "getCutout", NULL, ret_null);
  jni_method(D, "getHdrCapabilities", NULL, ret_null);
  jni_method(D, "isHdr", NULL, ret_false);
  jni_method(D, "getName", NULL, ret_null);
  jni_method("android/view/Display$Mode", "getPhysicalWidth", NULL, mode_getPhysicalWidth);
  jni_method("android/view/Display$Mode", "getPhysicalHeight", NULL, mode_getPhysicalHeight);
  jni_method("android/view/Display$Mode", "getRefreshRate", NULL, mode_getRefreshRate);
  jni_method("android/view/Display$Mode", "getModeId", NULL, mode_getModeId);
  jni_method("android/hardware/display/DisplayManager", "getDisplay", NULL, wm_getDefaultDisplay);
  jni_method("android/hardware/display/DisplayManager", "registerDisplayListener", NULL, ret_void);

  // --- PackageManager
  const char *PM = "android/content/pm/PackageManager";
  jni_method(PM, "getPackageInfo", NULL, pm_getPackageInfo);
  jni_method(PM, "getApplicationInfo", NULL, pm_getApplicationInfo);
  jni_method(PM, "getInstallerPackageName", NULL, pm_getInstallerPackageName);
  jni_method(PM, "hasSystemFeature", NULL, pm_hasSystemFeature);
  jni_method(PM, "getActivityInfo", NULL, pm_getActivityInfo);
  jni_method(PM, "queryIntentActivities", NULL, pm_queryList);
  jni_method(PM, "queryIntentServices", NULL, pm_queryList);
  jni_method(PM, "getInstalledPackages", NULL, pm_queryList);
  jni_method(PM, "resolveActivity", NULL, ret_null);
  jni_method(PM, "getLaunchIntentForPackage", NULL, ret_null);
  jni_method(PM, "checkPermission", NULL, ctx_checkPermission);

  // --- Bundle / Intent
  const char *B = "android/os/Bundle";
  jni_method(B, "<init>", NULL, bundle_init);
  jni_method(B, "getString", NULL, bundle_get);
  jni_method(B, "getInt", NULL, bundle_get);
  jni_method(B, "getBoolean", NULL, bundle_get);
  jni_method(B, "getFloat", NULL, bundle_get);
  jni_method(B, "getLong", NULL, bundle_get);
  jni_method(B, "get", NULL, bundle_get);
  jni_method(B, "containsKey", NULL, bundle_containsKey);
  jni_method(B, "putString", NULL, bundle_put);
  jni_method(B, "putInt", NULL, bundle_put);
  jni_method(B, "putBoolean", NULL, bundle_put);
  jni_method(B, "putLong", NULL, bundle_put);
  jni_method(B, "putFloat", NULL, bundle_put);
  const char *I = "android/content/Intent";
  jni_method(I, "getAction", NULL, intent_getAction);
  jni_method(I, "getData", NULL, ret_null);
  jni_method(I, "getDataString", NULL, ret_null);
  jni_method(I, "getExtras", NULL, ret_null);
  jni_method(I, "getStringExtra", NULL, intent_getExtra);
  jni_method(I, "getBooleanExtra", NULL, intent_getExtra);
  jni_method(I, "getIntExtra", NULL, intent_getExtra);
  jni_method(I, "getLongExtra", NULL, intent_getExtra);
  jni_method(I, "hasExtra", NULL, ret_false);
  jni_method(I, "getFlags", NULL, (JHandler)ret_zero);

  // --- Window / View
  jni_method("android/view/Window", "getDecorView", NULL, win_getDecorView);
  jni_method("android/view/Window", "getAttributes", NULL, win_getAttributes);
  jni_method("android/view/Window", "setFlags", NULL, ret_void);
  jni_method("android/view/Window", "addFlags", NULL, ret_void);
  jni_method("android/view/Window", "clearFlags", NULL, ret_void);
  jni_method("android/view/Window", "setAttributes", NULL, ret_void);
  jni_method("android/view/Window", "getInsetsController", NULL, ret_null);
  jni_method("android/view/View", "getWidth", NULL, view_getWidth);
  jni_method("android/view/View", "getHeight", NULL, view_getHeight);
  jni_method("android/view/View", "getRootWindowInsets", NULL, view_getRootWindowInsets);
  jni_method("android/view/View", "getRootView", NULL, ret_self);
  jni_set_super("android/view/SurfaceView", "android/view/View");
  s_surface_view = obj_of("android/view/SurfaceView");
  jni_method("android/view/View", "findViewById", NULL, view_findViewById);
  jni_method("android/view/Window", "findViewById", NULL, view_findViewById);
  jni_method("android/app/Activity", "findViewById", NULL, view_findViewById);
  jni_method("android/view/View", "addOnLayoutChangeListener", NULL, view_addOnLayoutChangeListener);
  jni_method("android/view/View", "removeOnLayoutChangeListener", NULL, view_removeOnLayoutChangeListener);
  jni_method("android/view/View", "setOnApplyWindowInsetsListener", NULL, view_setOnApplyWindowInsetsListener);
  jni_method("android/view/View", "requestLayout", NULL, view_requestLayout);
  jni_method("android/view/View", "requestApplyInsets", NULL, view_requestLayout);
  jni_method("android/view/View", "getDisplay", NULL, view_getDisplay);
  jni_method("android/view/View", "onApplyWindowInsets", NULL, view_onApplyWindowInsets);
  jni_method("android/view/View", "getLeft", NULL, (JHandler)ret_zero);
  jni_method("android/view/View", "getTop", NULL, (JHandler)ret_zero);
  jni_method("android/view/View", "setSystemUiVisibility", NULL, ret_void);
  jni_method("android/view/View", "getSystemUiVisibility", NULL, (JHandler)ret_zero);
  jni_method("android/view/View", "post", NULL, handler_post);
  jni_method("android/view/View", "postDelayed", NULL, handler_post);
  jni_method("android/view/View", "requestFocus", NULL, ret_true);
  jni_method("android/view/WindowInsets", "getSystemWindowInsetLeft", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getSystemWindowInsetTop", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getSystemWindowInsetRight", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getSystemWindowInsetBottom", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getStableInsetTop", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getStableInsetBottom", NULL, insets_zero);
  jni_method("android/view/WindowInsets", "getDisplayCutout", NULL, ret_null);

  // --- Locale / TimeZone / UUID
  jni_method("java/util/Locale", "getDefault", NULL, locale_getDefault);
  jni_method("java/util/Locale", "<init>", NULL, locale_init);
  jni_method("java/util/Locale", "getLanguage", NULL, locale_getLanguage);
  jni_method("java/util/Locale", "getCountry", NULL, locale_getCountry);
  jni_method("java/util/Locale", "toString", NULL, locale_toString);
  jni_method("java/util/Locale", "toLanguageTag", NULL, locale_toLanguageTag);
  jni_method("java/util/Locale", "getDisplayLanguage", NULL, locale_getDisplayLanguage);
  jni_method("java/util/Locale", "getDisplayName", NULL, locale_getDisplayLanguage);
  jni_method("java/util/Locale", "getISO3Language", NULL, locale_getLanguage);
  jni_method("java/util/Locale", "getISO3Country", NULL, locale_getCountry);
  jni_method("java/util/Locale", "getScript", NULL, (JHandler)ret_null);
  jni_method("java/util/Locale", "getVariant", NULL, (JHandler)ret_null);
  jni_method("java/util/TimeZone", "getDefault", NULL, tz_getDefault);
  jni_method("java/util/TimeZone", "getID", NULL, tz_getID);
  jni_method("java/util/TimeZone", "getRawOffset", NULL, tz_getOffset);
  jni_method("java/util/TimeZone", "getOffset", NULL, tz_getOffset);
  jni_method("java/util/TimeZone", "inDaylightTime", NULL, ret_false);
  jni_method("java/util/UUID", "randomUUID", NULL, uuid_randomUUID);
  jni_method("java/util/UUID", "toString", NULL, field_str);

  // --- services
  const char *CM = "android/net/ConnectivityManager";
  jni_method(CM, "getActiveNetworkInfo", NULL, cm_getActiveNetworkInfo);
  jni_method(CM, "getNetworkInfo", NULL, cm_getActiveNetworkInfo);
  jni_method(CM, "getActiveNetwork", NULL, cm_getActiveNetwork);
  jni_method(CM, "getNetworkCapabilities", NULL, cm_getNetworkCapabilities);
  jni_method(CM, "registerDefaultNetworkCallback", NULL, ret_void);
  jni_method(CM, "registerNetworkCallback", NULL, ret_void);
  jni_method(CM, "unregisterNetworkCallback", NULL, ret_void);
  jni_method(CM, "isActiveNetworkMetered", NULL, ret_false);
  jni_method("android/net/NetworkInfo", "isConnected", NULL, ret_true);
  jni_method("android/net/NetworkInfo", "isConnectedOrConnecting", NULL, ret_true);
  jni_method("android/net/NetworkInfo", "isAvailable", NULL, ret_true);
  jni_method("android/net/NetworkInfo", "getType", NULL, ni_getType);
  jni_method("android/net/NetworkInfo", "getTypeName", NULL, ni_getTypeName);
  jni_method("android/net/NetworkCapabilities", "hasTransport", NULL, caps_has);
  jni_method("android/net/NetworkCapabilities", "hasCapability", NULL, caps_has);
  const char *AM = "android/media/AudioManager";
  jni_method(AM, "getProperty", NULL, audio_getProperty);
  jni_method(AM, "getStreamVolume", NULL, audio_getStreamVolume);
  jni_method(AM, "getStreamMaxVolume", NULL, audio_getStreamMaxVolume);
  jni_method(AM, "requestAudioFocus", NULL, audio_requestAudioFocus);
  jni_method(AM, "abandonAudioFocus", NULL, audio_requestAudioFocus);
  jni_method(AM, "abandonAudioFocusRequest", NULL, audio_requestAudioFocus);
  jni_method(AM, "getDevices", NULL, audio_getDevices);
  jni_method(AM, "getRingerMode", NULL, audio_getRingerMode);
  jni_method(AM, "isMusicActive", NULL, ret_false);
  jni_method(AM, "isWiredHeadsetOn", NULL, ret_false);
  jni_method(AM, "isBluetoothA2dpOn", NULL, ret_false);
  jni_method(AM, "isSpeakerphoneOn", NULL, ret_true);
  jni_method(AM, "getMode", NULL, (JHandler)ret_zero);
  jni_method("android/os/PowerManager", "isPowerSaveMode", NULL, ret_false);
  jni_method("android/os/PowerManager", "isInteractive", NULL, ret_true);
  jni_method("android/os/PowerManager", "isScreenOn", NULL, ret_true);
  jni_method("android/os/PowerManager", "isSustainedPerformanceModeSupported", NULL, ret_false);
  jni_method("android/os/PowerManager", "getCurrentThermalStatus", NULL, (JHandler)ret_zero);
  jni_method("android/os/PowerManager", "newWakeLock", NULL, pow_newWakeLock);
  jni_method("android/os/PowerManager$WakeLock", "acquire", NULL, ret_void);
  jni_method("android/os/PowerManager$WakeLock", "release", NULL, ret_void);
  jni_method("android/os/Vibrator", "hasVibrator", NULL, ret_false);
  jni_method("android/os/Vibrator", "vibrate", NULL, ret_void);
  jni_method("android/os/Vibrator", "cancel", NULL, ret_void);
  jni_method("android/app/ActivityManager", "getMemoryClass", NULL, am_getMemoryClass);
  jni_method("android/app/ActivityManager", "getLargeMemoryClass", NULL, am_getMemoryClass);
  jni_method("android/app/ActivityManager", "getMemoryInfo", NULL, am_getMemoryInfo);
  jni_method("android/app/ActivityManager", "isLowRamDevice", NULL, ret_false);
  jni_method("android/app/ActivityManager", "getRunningAppProcesses", NULL, pm_queryList);
  jni_method("android/telephony/TelephonyManager", "getNetworkOperatorName", NULL, tel_getString);
  jni_method("android/telephony/TelephonyManager", "getSimCountryIso", NULL, tel_getString);
  jni_method("android/telephony/TelephonyManager", "getNetworkCountryIso", NULL, tel_getString);
  jni_method("android/telephony/TelephonyManager", "getNetworkOperator", NULL, tel_getString);
  jni_method("android/telephony/TelephonyManager", "getSimOperator", NULL, tel_getString);
  jni_method("android/telephony/TelephonyManager", "getPhoneType", NULL, tel_getInt);
  jni_method("android/telephony/TelephonyManager", "getNetworkType", NULL, tel_getInt);
  jni_method("android/hardware/SensorManager", "getDefaultSensor", NULL, ret_null);
  jni_method("android/hardware/SensorManager", "getSensorList", NULL, pm_queryList);
  jni_method("android/hardware/input/InputManager", "registerInputDeviceListener", NULL, ret_void);

  const char *ST = "android/provider/Settings$Secure";
  jni_method(ST, "getString", NULL, settings_getString);
  jni_method(ST, "getInt", NULL, settings_getInt);
  jni_method("android/provider/Settings$System", "getInt", NULL, settings_getInt);
  jni_method("android/provider/Settings$System", "getString", NULL, settings_getString);
  jni_method("android/provider/Settings$Global", "getInt", NULL, settings_getInt);
  jni_method("android/provider/Settings$Global", "getString", NULL, settings_getString);
  jni_method("android/os/SystemClock", "elapsedRealtime", NULL, clock_elapsedRealtime);
  jni_method("android/os/SystemClock", "uptimeMillis", NULL, clock_elapsedRealtime);
  jni_method("android/os/SystemClock", "elapsedRealtimeNanos", NULL, clock_elapsedRealtimeNanos);
  jni_method("android/os/Process", "myPid", NULL, proc_myPid);
  jni_method("android/os/Process", "myTid", NULL, proc_myPid);
  jni_method("android/os/Process", "myUid", NULL, proc_myUid);
  jni_method("android/os/Process", "setThreadPriority", NULL, ret_void);

  // --- Looper / Handler / Message / HandlerThread / Choreographer
  const char *LP = "android/os/Looper";
  jni_method(LP, "getMainLooper", NULL, looper_getMainLooper);
  jni_method(LP, "myLooper", NULL, looper_myLooper);
  jni_method(LP, "prepare", NULL, ret_void);
  jni_method(LP, "quit", NULL, looper_quit_h);
  jni_method(LP, "quitSafely", NULL, looper_quit_h);
  jni_method(LP, "isCurrentThread", NULL, looper_isCurrentThread);
  const char *H = "android/os/Handler";
  jni_method(H, "<init>", NULL, handler_init);
  jni_method(H, "getLooper", NULL, handler_getLooper);
  jni_method(H, "obtainMessage", NULL, handler_obtainMessage);
  static const char *const sends[] = { "sendMessage", "sendMessageDelayed", "sendMessageAtTime",
                                       "sendMessageAtFrontOfQueue" };
  static const char *const empties[] = { "sendEmptyMessage", "sendEmptyMessageDelayed",
                                         "sendEmptyMessageAtTime" };
  static const char *const posts[] = { "post", "postDelayed", "postAtTime", "postAtFrontOfQueue" };
  static const char *const scans[] = { "removeMessages", "removeCallbacks",
                                       "removeCallbacksAndMessages", "hasMessages", "hasCallbacks" };
  for (unsigned i = 0; i < sizeof(sends) / sizeof(*sends); i++)
    jni_method(H, sends[i], NULL, handler_sendMessage);
  for (unsigned i = 0; i < sizeof(empties) / sizeof(*empties); i++)
    jni_method(H, empties[i], NULL, handler_sendEmptyMessage);
  for (unsigned i = 0; i < sizeof(posts) / sizeof(*posts); i++)
    jni_method(H, posts[i], NULL, handler_postRunnable);
  for (unsigned i = 0; i < sizeof(scans) / sizeof(*scans); i++)
    jni_method(H, scans[i], NULL, handler_scan);
  jni_method(H, "dispatchMessage", NULL, handler_dispatchMessage);
  jni_method(H, "handleMessage", NULL, ret_void);
  const char *MS = "android/os/Message";
  jni_method(MS, "obtain", NULL, msg_obtain);
  jni_method(MS, "sendToTarget", NULL, msg_sendToTarget);
  jni_method(MS, "getTarget", NULL, msg_getTarget);
  jni_method(MS, "setTarget", NULL, msg_setTarget);
  jni_method(MS, "getCallback", NULL, msg_getCallback);
  jni_method(MS, "getData", NULL, msg_getData);
  jni_method(MS, "peekData", NULL, msg_getData);
  jni_method(MS, "setData", NULL, msg_setData);
  jni_method(MS, "getWhen", NULL, (JHandler)ret_zero);
  jni_method(MS, "recycle", NULL, ret_void);
  jni_method(MS, "setAsynchronous", NULL, ret_void);
  jni_method(MS, "isAsynchronous", NULL, ret_false);
  const char *HT = "android/os/HandlerThread";
  jni_method(HT, "<init>", NULL, ht_init);
  jni_method(HT, "getLooper", NULL, ht_getLooper);
  jni_method(HT, "quit", NULL, ht_quit);
  jni_method(HT, "quitSafely", NULL, ht_quit);
  jni_method(HT, "getThreadId", NULL, ht_getThreadId);
  jni_method(HT, "getName", NULL, ht_getName);
  jni_method("java/lang/Thread", "start", NULL, thread_start);
  jni_method("java/lang/Thread", "join", NULL, thread_join);
  jni_method("java/lang/Thread", "isAlive", NULL, thread_isAlive);
  const char *CH = "android/view/Choreographer";
  jni_method(CH, "getInstance", NULL, choreo_getInstance);
  jni_method(CH, "postFrameCallback", NULL, choreo_postFrameCallback);
  jni_method(CH, "postFrameCallbackDelayed", NULL, choreo_postFrameCallback);
  jni_method(CH, "removeFrameCallback", NULL, choreo_removeFrameCallback);
  jni_method(CH, "getFrameTimeNanos", NULL, choreo_getFrameTime);
  jni_method(CH, "getFrameTime", NULL, choreo_getFrameTime);
}
