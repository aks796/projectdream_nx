/* android_ndk.c -- the libandroid / libmediandk functions the game's libraries
 * import directly:
 *
 *   ANativeWindow_*   the console's default window (libnx NWindow); Unity
 *                     makes its own EGL surface on it
 *   ALooper_*         a per-thread wait/wake (Unity's threads park in it)
 *   ASensor*          no sensors (yet)
 *   AAssetManager_*   the APK's assets/ read straight out of the zip
 *                     (Wwise reads its banks this way)
 *   (AMedia*, AImage*: media_ndk.c, FFmpeg)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <switch.h>

#include "apkfile.h"
#include "config.h"
#include "jni.h"
#include "log.h"
#include "so_util.h"
#include "vfs.h"

// --- ANativeWindow ----------------------------------------------------------------------

typedef struct ANativeWindow ANativeWindow;

static int s_win_w = 1280, s_win_h = 720;

static void win_geometry(int w, int h) {
  NWindow *nw = nwindowGetDefault();
  nwindowSetDimensions(nw, (u32)w, (u32)h);
  nwindowSetCrop(nw, 0, 0, (u32)w, (u32)h);
  s_win_w = w;
  s_win_h = h;
}

void android_window_init(int w, int h) {
  win_geometry(w, h);
}

// Destroying an EGL surface releases the window's buffers, and libnx then
// forgets its size (0x0): the next surface would be made at the default
// 1280x720 under the panel-sized crop, which the compositor refuses at the
// first present (QueueBuffer BadValue, mesa's fatal 2349-0004). Unity
// destroys and remakes its surface while it starts, so the size and crop
// go back on before each new one.
void android_window_prepare(void) {
  if (!nwindowGetDefault()->slots_configured)
    win_geometry(s_win_w, s_win_h);
}

static ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface) {
  (void)env; (void)surface;
  return (ANativeWindow *)nwindowGetDefault();
}

static void *ANativeWindow_toSurface(void *env, ANativeWindow *w) {
  (void)env; (void)w;
  return jni_surface();
}

static void ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
static void ANativeWindow_release(ANativeWindow *w) { (void)w; }
static int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return s_win_w; }
static int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return s_win_h; }
static int32_t ANativeWindow_getFormat(ANativeWindow *w) { (void)w; return 1; } // RGBA_8888

// The window is the panel's size, fixed: a smaller swapchain is never
// presented by the console (the first swap would block forever). Android
// devices without hardware scaling behave the same -- the call succeeds and
// the size stays -- and Unity then scales its own back buffer.
static int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *w, int32_t width, int32_t height,
                                                int32_t format) {
  (void)w; (void)format;
  if (width > 0 && height > 0 && (width != s_win_w || height != s_win_h))
    log_printf("[ndk] setBuffersGeometry %dx%d ignored (the window stays %dx%d)", width, height,
               s_win_w, s_win_h);
  return 0;
}

static int32_t ANativeWindow_setFrameRate(ANativeWindow *w, float rate, int8_t compat) {
  (void)w; (void)rate; (void)compat;
  return 0;
}

// --- ALooper ---------------------------------------------------------------------------

typedef struct {
  Mutex m;
  CondVar cv;
  int woken;
} Looper;

static __thread Looper *t_looper;

static Looper *ALooper_prepare(int opts) {
  (void)opts;
  if (!t_looper) {
    t_looper = calloc(1, sizeof(Looper));
    mutexInit(&t_looper->m);
    condvarInit(&t_looper->cv);
  }
  return t_looper;
}

static Looper *ALooper_forThread(void) { return t_looper; }
static void ALooper_acquire(Looper *l) { (void)l; }
static void ALooper_release(Looper *l) { (void)l; }

#define ALOOPER_POLL_WAKE    (-1)
#define ALOOPER_POLL_TIMEOUT (-3)

static int ALooper_pollOnce(int timeout_ms, int *fd, int *events, void **data) {
  if (fd) *fd = -1;
  if (events) *events = 0;
  if (data) *data = NULL;
  Looper *l = t_looper;
  if (!l) {
    if (timeout_ms > 0)
      svcSleepThread((s64)timeout_ms * 1000000ll);
    return ALOOPER_POLL_TIMEOUT;
  }
  mutexLock(&l->m);
  if (!l->woken && timeout_ms != 0) {
    if (timeout_ms < 0)
      condvarWait(&l->cv, &l->m);
    else
      condvarWaitTimeout(&l->cv, &l->m, (u64)timeout_ms * 1000000ull);
  }
  const int woke = l->woken;
  l->woken = 0;
  mutexUnlock(&l->m);
  return woke ? ALOOPER_POLL_WAKE : ALOOPER_POLL_TIMEOUT;
}

static void ALooper_wake(Looper *l) {
  if (!l)
    return;
  mutexLock(&l->m);
  l->woken = 1;
  condvarWakeAll(&l->cv);
  mutexUnlock(&l->m);
}

static int ALooper_addFd(Looper *l, int fd, int ident, int events, void *cb, void *data) {
  (void)l; (void)fd; (void)ident; (void)events; (void)cb; (void)data;
  return 1;
}

static int ALooper_removeFd(Looper *l, int fd) { (void)l; (void)fd; return 1; }

// --- ASensor: none ----------------------------------------------------------------------

static int s_sensor_manager;
static void *ASensorManager_getInstance(void) { return &s_sensor_manager; }
static void *ASensorManager_getInstanceForPackage(const char *p) { (void)p; return &s_sensor_manager; }
static const void *ASensorManager_getDefaultSensor(void *m, int type) { (void)m; (void)type; return NULL; }
static int ASensorManager_getSensorList(void *m, const void ***list) {
  (void)m;
  static const void *empty[1];
  if (list)
    *list = empty;
  return 0;
}
static int s_event_queue;
static void *ASensorManager_createEventQueue(void *m, void *looper, int ident, void *cb, void *data) {
  (void)m; (void)looper; (void)ident; (void)cb; (void)data;
  return &s_event_queue;
}
static int ASensorManager_destroyEventQueue(void *m, void *q) { (void)m; (void)q; return 0; }
static int ASensorEventQueue_enableSensor(void *q, const void *s) { (void)q; (void)s; return -1; }
static int ASensorEventQueue_disableSensor(void *q, const void *s) { (void)q; (void)s; return 0; }
static int ASensorEventQueue_setEventRate(void *q, const void *s, int us) { (void)q; (void)s; (void)us; return 0; }
static int ASensorEventQueue_hasEvents(void *q) { (void)q; return 0; }
static long ASensorEventQueue_getEvents(void *q, void *ev, size_t n) { (void)q; (void)ev; (void)n; return 0; }
static const char *ASensor_getName(const void *s) { (void)s; return ""; }
static const char *ASensor_getVendor(const void *s) { (void)s; return ""; }
static int ASensor_getType(const void *s) { (void)s; return 0; }
static float ASensor_getResolution(const void *s) { (void)s; return 0.f; }
static int ASensor_getMinDelay(const void *s) { (void)s; return 0; }

// --- AAssetManager over the APK -----------------------------------------------------------

typedef struct {
  const ApkEntry *e;
  uint8_t *buf;      // the whole content (deflated entries; getBuffer)
  uint64_t pos;
} Asset;

typedef struct {
  char prefix[512];  // "assets/<dir>/"
  int idx;
  char name[256];
} AssetDir;

static int s_asset_manager;

static void *AAssetManager_fromJava(void *env, void *obj) {
  (void)env; (void)obj;
  return &s_asset_manager;
}

static const ApkEntry *asset_entry(const char *name) {
  char full[600];
  while (name[0] == '/')
    name++;
  snprintf(full, sizeof full, "assets/%s", name);
  return apkfile_find(full);
}

static Asset *AAssetManager_open(void *mgr, const char *name, int mode) {
  (void)mgr; (void)mode;
  const ApkEntry *e = asset_entry(name);
  if (config.log_fs)
    log_printf("[ndk] AAsset open %s -> %s", name, e ? "ok" : "missing");
  if (!e)
    return NULL;
  Asset *a = calloc(1, sizeof(Asset));
  a->e = e;
  if (e->method != 0) {
    size_t n = 0;
    a->buf = apkfile_load(e, &n);
    if (!a->buf) {
      free(a);
      return NULL;
    }
  }
  return a;
}

static int AAsset_read(Asset *a, void *buf, size_t n) {
  if (!a)
    return -1;
  if (a->pos >= a->e->usize)
    return 0;
  if (n > a->e->usize - a->pos)
    n = (size_t)(a->e->usize - a->pos);
  if (a->buf) {
    memcpy(buf, a->buf + a->pos, n);
  } else {
    const long got = apkfile_read(a->e->data_off + a->pos, buf, n);
    if (got < 0)
      return -1;
    n = (size_t)got;
  }
  a->pos += n;
  return (int)n;
}

static int64_t AAsset_seek64(Asset *a, int64_t off, int whence) {
  if (!a)
    return -1;
  int64_t np;
  switch (whence) {
    case 0: np = off; break;
    case 1: np = (int64_t)a->pos + off; break;
    case 2: np = (int64_t)a->e->usize + off; break;
    default: return -1;
  }
  if (np < 0)
    return -1;
  a->pos = (uint64_t)np;
  return np;
}

static int64_t AAsset_getLength64(Asset *a) { return a ? (int64_t)a->e->usize : 0; }
static int64_t AAsset_getRemainingLength64(Asset *a) {
  return a && a->pos < a->e->usize ? (int64_t)(a->e->usize - a->pos) : 0;
}

static const void *AAsset_getBuffer(Asset *a) {
  if (!a)
    return NULL;
  if (!a->buf) {
    size_t n = 0;
    a->buf = apkfile_load(a->e, &n);
  }
  return a->buf;
}

// a stored asset can be handed out as a file: a virtual fd on its bytes
static int AAsset_openFileDescriptor64(Asset *a, int64_t *start, int64_t *len) {
  if (!a || a->e->method != 0)
    return -1;
  NabeFile f = { a->e->name, a->e->data_off, a->e->usize };
  const int fd = vfd_open(&f);
  if (fd < 0)
    return -1;
  if (start) *start = 0;
  if (len) *len = (int64_t)a->e->usize;
  return fd;
}

static int AAsset_isAllocated(Asset *a) { return a && a->buf != NULL; }

static void AAsset_close(Asset *a) {
  if (!a)
    return;
  free(a->buf);
  free(a);
}

static AssetDir *AAssetManager_openDir(void *mgr, const char *dir) {
  (void)mgr;
  AssetDir *d = calloc(1, sizeof(AssetDir));
  while (dir[0] == '/')
    dir++;
  if (dir[0])
    snprintf(d->prefix, sizeof d->prefix, "assets/%s/", dir);
  else
    snprintf(d->prefix, sizeof d->prefix, "assets/");
  return d;
}

static const char *AAssetDir_getNextFileName(AssetDir *d) {
  if (!d)
    return NULL;
  const size_t pl = strlen(d->prefix);
  while (d->idx < apkfile_count()) {
    const ApkEntry *e = apkfile_entry_at(d->idx++);
    if (!e || strncmp(e->name, d->prefix, pl) != 0)
      continue;
    const char *rest = e->name + pl;
    if (!rest[0] || strchr(rest, '/'))
      continue; // files directly in this directory only
    snprintf(d->name, sizeof d->name, "%s", rest);
    return d->name;
  }
  return NULL;
}

static void AAssetDir_rewind(AssetDir *d) { if (d) d->idx = 0; }
static void AAssetDir_close(AssetDir *d) { free(d); }

DynLibFunction ndk_functions[] = {
  { "ANativeWindow_fromSurface", (uintptr_t)&ANativeWindow_fromSurface },
  { "ANativeWindow_toSurface", (uintptr_t)&ANativeWindow_toSurface },
  { "ANativeWindow_acquire", (uintptr_t)&ANativeWindow_acquire },
  { "ANativeWindow_release", (uintptr_t)&ANativeWindow_release },
  { "ANativeWindow_getWidth", (uintptr_t)&ANativeWindow_getWidth },
  { "ANativeWindow_getHeight", (uintptr_t)&ANativeWindow_getHeight },
  { "ANativeWindow_getFormat", (uintptr_t)&ANativeWindow_getFormat },
  { "ANativeWindow_setBuffersGeometry", (uintptr_t)&ANativeWindow_setBuffersGeometry },
  { "ANativeWindow_setFrameRate", (uintptr_t)&ANativeWindow_setFrameRate },

  { "ALooper_prepare", (uintptr_t)&ALooper_prepare },
  { "ALooper_forThread", (uintptr_t)&ALooper_forThread },
  { "ALooper_acquire", (uintptr_t)&ALooper_acquire },
  { "ALooper_release", (uintptr_t)&ALooper_release },
  { "ALooper_pollOnce", (uintptr_t)&ALooper_pollOnce },
  { "ALooper_pollAll", (uintptr_t)&ALooper_pollOnce },
  { "ALooper_wake", (uintptr_t)&ALooper_wake },
  { "ALooper_addFd", (uintptr_t)&ALooper_addFd },
  { "ALooper_removeFd", (uintptr_t)&ALooper_removeFd },

  { "ASensorManager_getInstance", (uintptr_t)&ASensorManager_getInstance },
  { "ASensorManager_getInstanceForPackage", (uintptr_t)&ASensorManager_getInstanceForPackage },
  { "ASensorManager_getDefaultSensor", (uintptr_t)&ASensorManager_getDefaultSensor },
  { "ASensorManager_getSensorList", (uintptr_t)&ASensorManager_getSensorList },
  { "ASensorManager_createEventQueue", (uintptr_t)&ASensorManager_createEventQueue },
  { "ASensorManager_destroyEventQueue", (uintptr_t)&ASensorManager_destroyEventQueue },
  { "ASensorEventQueue_enableSensor", (uintptr_t)&ASensorEventQueue_enableSensor },
  { "ASensorEventQueue_disableSensor", (uintptr_t)&ASensorEventQueue_disableSensor },
  { "ASensorEventQueue_setEventRate", (uintptr_t)&ASensorEventQueue_setEventRate },
  { "ASensorEventQueue_hasEvents", (uintptr_t)&ASensorEventQueue_hasEvents },
  { "ASensorEventQueue_getEvents", (uintptr_t)&ASensorEventQueue_getEvents },
  { "ASensor_getName", (uintptr_t)&ASensor_getName },
  { "ASensor_getVendor", (uintptr_t)&ASensor_getVendor },
  { "ASensor_getType", (uintptr_t)&ASensor_getType },
  { "ASensor_getResolution", (uintptr_t)&ASensor_getResolution },
  { "ASensor_getMinDelay", (uintptr_t)&ASensor_getMinDelay },

  { "AAssetManager_fromJava", (uintptr_t)&AAssetManager_fromJava },
  { "AAssetManager_open", (uintptr_t)&AAssetManager_open },
  { "AAssetManager_openDir", (uintptr_t)&AAssetManager_openDir },
  { "AAsset_read", (uintptr_t)&AAsset_read },
  { "AAsset_seek", (uintptr_t)&AAsset_seek64 },
  { "AAsset_seek64", (uintptr_t)&AAsset_seek64 },
  { "AAsset_getLength", (uintptr_t)&AAsset_getLength64 },
  { "AAsset_getLength64", (uintptr_t)&AAsset_getLength64 },
  { "AAsset_getRemainingLength", (uintptr_t)&AAsset_getRemainingLength64 },
  { "AAsset_getRemainingLength64", (uintptr_t)&AAsset_getRemainingLength64 },
  { "AAsset_getBuffer", (uintptr_t)&AAsset_getBuffer },
  { "AAsset_openFileDescriptor", (uintptr_t)&AAsset_openFileDescriptor64 },
  { "AAsset_openFileDescriptor64", (uintptr_t)&AAsset_openFileDescriptor64 },
  { "AAsset_isAllocated", (uintptr_t)&AAsset_isAllocated },
  { "AAsset_close", (uintptr_t)&AAsset_close },
  { "AAssetDir_getNextFileName", (uintptr_t)&AAssetDir_getNextFileName },
  { "AAssetDir_rewind", (uintptr_t)&AAssetDir_rewind },
  { "AAssetDir_close", (uintptr_t)&AAssetDir_close },
};

size_t ndk_numfunctions = sizeof(ndk_functions) / sizeof(*ndk_functions);
