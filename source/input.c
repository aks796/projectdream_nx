/* input.c -- touch and keys into Unity as Android MotionEvents and KeyEvents
 *
 * Unity reads an event back through JNI during nativeInjectEvent (action,
 * pointers, coordinates, source, times). One MotionEvent and one KeyEvent
 * object are reused for every event sent; Unity 2022 keeps a touch for
 * later with MotionEvent.obtain(event), which gets a copy of its own
 * (recycled copies are reused).
 *
 * Per frame the caller reports every finger (input_touch) and then calls
 * input_flush, which turns the changes into the Android sequence: DOWN for
 * the first finger, POINTER_DOWN/POINTER_UP for the others, MOVE for motion,
 * UP when the last one lifts.
 *
 * The soft keyboard Unity opens for input fields is the console's (swkbd);
 * the text goes back through nativeSetInputString / nativeSoftInputClosed.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "config.h"
#include "jni.h"
#include "libc_shim.h"
#include "log.h"
#include "unity.h"

#define MAX_PTR 10

typedef struct {
  int down;
  float x, y;
} Ptr;

static Ptr s_cur[MAX_PTR], s_new[MAX_PTR];
static JObj *s_motion, *s_key, *s_device;
static int64_t s_down_time;

// a MotionEvent's content: pointer ids and positions in index order
typedef struct {
  int action;
  int count;
  int id[MAX_PTR];
  float x[MAX_PTR], y[MAX_PTR];
  int64_t time, down_time;
} MotionData;

// the event being sent (s_motion's); copies made by MotionEvent.obtain
// carry their own (JObj.native), and recycled ones are kept for reuse
static MotionData ev;
static JObj *s_free_motion[64];
static int s_nfree_motion;

static const MotionData *md(const JObj *o) {
  return o && o->native ? (const MotionData *)o->native : &ev;
}

static struct {
  int action, code, repeat;
  int64_t time, down_time;
} kev;

// SystemClock.uptimeMillis's clock, as Android's event times
static int64_t now_ms(void) {
  return android_monotonic_ns() / 1000000;
}

// --- MotionEvent ---------------------------------------------------------------------

static int idx_arg(const MotionData *e, const JMethod *m, const jvalue *a) {
  const int i = m->nargs >= 1 && m->argt[m->nargs - 1] == 'I' ? a[m->nargs - 1].i : 0;
  return i >= 0 && i < e->count ? i : 0;
}

JH(me_getAction) { JUNUSED; return jv_i(md(thiz)->action); }
JH(me_getActionMasked) { JUNUSED; return jv_i(md(thiz)->action & 0xff); }
JH(me_getActionIndex) { JUNUSED; return jv_i((md(thiz)->action >> 8) & 0xff); }
JH(me_getPointerCount) { JUNUSED; return jv_i(md(thiz)->count); }
JH(me_getPointerId) { JUNUSED; const MotionData *e = md(thiz); return jv_i(e->id[idx_arg(e, m, a)]); }
JH(me_findPointerIndex) {
  JUNUSED;
  const MotionData *e = md(thiz);
  for (int i = 0; i < e->count; i++)
    if (e->id[i] == a[0].i)
      return jv_i(i);
  return jv_i(-1);
}
JH(me_getX) { JUNUSED; const MotionData *e = md(thiz); return jv_f(e->x[idx_arg(e, m, a)]); }
JH(me_getY) { JUNUSED; const MotionData *e = md(thiz); return jv_f(e->y[idx_arg(e, m, a)]); }
JH(me_getAxisValue) {
  JUNUSED;
  // (axis) or (axis, pointerIndex): AXIS_X 0, AXIS_Y 1, PRESSURE 2, SIZE 3
  const MotionData *e = md(thiz);
  const int axis = a[0].i;
  const int i = m->nargs > 1 && a[1].i >= 0 && a[1].i < e->count ? a[1].i : 0;
  switch (axis) {
    case 0: return jv_f(e->x[i]);
    case 1: return jv_f(e->y[i]);
    case 2: return jv_f(1.0f);
    case 3: return jv_f(0.1f);
    default: return jv_f(0.0f);
  }
}
JH(me_getEventTime) { JUNUSED; return jv_j(md(thiz)->time); }
JH(me_getEventTimeNanos) { JUNUSED; return jv_j(md(thiz)->time * 1000000ll); }
JH(me_getDownTime) { JUNUSED; return jv_j(md(thiz)->down_time); }
// MotionEvent.obtain(MotionEvent): a copy of its content, kept by Unity
// to read later (Unity 2022 queues the copy)
JH(me_obtain) {
  JUNUSED;
  JObj *o = s_nfree_motion ? s_free_motion[--s_nfree_motion] : jni_new("android/view/MotionEvent");
  if (!o->native)
    o->native = malloc(sizeof(MotionData));
  memcpy(o->native, md(m->nargs ? a[0].l : NULL), sizeof(MotionData));
  return jv_l(o);
}
// recycle(): a copy goes back for the next obtain
JH(me_recycle) {
  JUNUSED;
  if (thiz && thiz != s_motion && thiz->native && s_nfree_motion < 64)
    s_free_motion[s_nfree_motion++] = thiz;
  return jv_v();
}
JH(me_getSource) { JUNUSED; return jv_i(0x1002); } // SOURCE_TOUCHSCREEN
JH(me_getDeviceId) { JUNUSED; return jv_i(1); }
JH(me_getToolType) { JUNUSED; return jv_i(1); }    // TOOL_TYPE_FINGER
JH(me_getPressure) { JUNUSED; return jv_f(1.0f); }
JH(me_getSize) { JUNUSED; return jv_f(0.1f); }
JH(me_getTouchMajor) { JUNUSED; return jv_f(20.0f); }
JH(me_zero_i) { JUNUSED; return jv_i(0); }
JH(me_zero_f) { JUNUSED; return jv_f(0.0f); }
JH(me_zero_j) { JUNUSED; return jv_j(0); }
JH(me_isFromSource) { JUNUSED; return jv_z((a[0].i & 0x1002) == a[0].i); }
JH(me_getDevice) { JUNUSED; return jv_l(s_device); }

// --- KeyEvent --------------------------------------------------------------------------

JH(ke_getAction) { JUNUSED; return jv_i(kev.action); }
JH(ke_getKeyCode) { JUNUSED; return jv_i(kev.code); }
JH(ke_getRepeatCount) { JUNUSED; return jv_i(kev.repeat); }
JH(ke_getEventTime) { JUNUSED; return jv_j(kev.time); }
JH(ke_getDownTime) { JUNUSED; return jv_j(kev.down_time); }
JH(ke_getSource) { JUNUSED; return jv_i(0x101); }  // SOURCE_KEYBOARD
JH(ke_getUnicodeChar) { JUNUSED; return jv_i(0); }
JH(ke_getCharacters) { JUNUSED; return jv_l(NULL); }
JH(ke_isFromSource) { JUNUSED; return jv_z((a[0].i & 0x101) == a[0].i); }

// --- InputDevice -------------------------------------------------------------------------

JH(dev_get) { JUNUSED; return jv_l(s_device); }
JH(dev_getName) { JUNUSED; return jv_l(jni_str("Switch touchscreen")); }
JH(dev_getSources) { JUNUSED; return jv_i(0x1002 | 0x101); }
JH(dev_getId) { JUNUSED; return jv_i(1); }
JH(dev_getIds) {
  JUNUSED;
  JObj *arr = jni_array('I', 1, NULL);
  ((int32_t *)jni_arr(arr)->data)[0] = 1;
  return jv_l(arr);
}
JH(dev_isVirtual) { JUNUSED; return jv_z(0); }
JH(dev_getMotionRange) { JUNUSED; return jv_l(NULL); }
JH(dev_getDescriptor) { JUNUSED; return jv_l(jni_str("nx-touch")); }
JH(dev_getKeyboardType) { JUNUSED; return jv_i(0); }

// --- sending ------------------------------------------------------------------------------

static void send_motion(int action) {
  ev.action = action;
  ev.time = now_ms();
  ev.down_time = s_down_time;
  unity_inject(s_motion);
}

static void build_pointers(const Ptr *set) {
  ev.count = 0;
  for (int i = 0; i < MAX_PTR; i++) {
    if (!set[i].down)
      continue;
    ev.id[ev.count] = i;
    ev.x[ev.count] = set[i].x;
    ev.y[ev.count] = set[i].y;
    ev.count++;
  }
}

static int index_of(int id) {
  for (int i = 0; i < ev.count; i++)
    if (ev.id[i] == id)
      return i;
  return 0;
}

// per pointer id: the caller's finger it is, -1 when free. Android gives a
// new finger the lowest id not in use (the first finger is always 0), and
// Unity drops every touch with an id above 0 while the game has multi-touch
// off; the console's finger ids keep counting up.
static int s_finger_of[MAX_PTR] = { [0 ... MAX_PTR - 1] = -1 };

void input_touch(int finger, int down, float x, float y) {
  if (!down)
    return; // callers report the fingers that are down
  int id = -1;
  for (int i = 0; i < MAX_PTR && id < 0; i++)
    if (s_finger_of[i] == finger)
      id = i;
  for (int i = 0; i < MAX_PTR && id < 0; i++)
    if (s_finger_of[i] < 0) {
      id = i;
      s_finger_of[i] = finger;
    }
  if (id < 0)
    return;
  s_new[id].down = 1;
  s_new[id].x = x;
  s_new[id].y = y;
}

void input_flush(void) {
  // lifts first (with the pointer still in the list), then new fingers, then moves
  for (int id = 0; id < MAX_PTR; id++) {
    if (s_cur[id].down && !s_new[id].down) {
      build_pointers(s_cur);
      int n = 0;
      for (int k = 0; k < MAX_PTR; k++)
        n += s_cur[k].down;
      send_motion(n == 1 ? 1 : (6 | index_of(id) << 8)); // UP / POINTER_UP
      s_cur[id].down = 0;
    }
    if (!s_new[id].down)
      s_finger_of[id] = -1; // lifted (or never came down): the id is free again
  }
  for (int id = 0; id < MAX_PTR; id++) {
    if (!s_cur[id].down && s_new[id].down) {
      int n = 0;
      for (int k = 0; k < MAX_PTR; k++)
        n += s_cur[k].down;
      s_cur[id] = s_new[id];
      build_pointers(s_cur);
      if (n == 0)
        s_down_time = now_ms();
      send_motion(n == 0 ? 0 : (5 | index_of(id) << 8)); // DOWN / POINTER_DOWN
    }
  }
  int moved = 0;
  for (int id = 0; id < MAX_PTR; id++) {
    if (s_cur[id].down && s_new[id].down &&
        (s_cur[id].x != s_new[id].x || s_cur[id].y != s_new[id].y)) {
      s_cur[id] = s_new[id];
      moved = 1;
    }
  }
  if (moved) {
    build_pointers(s_cur);
    send_motion(2); // MOVE
  }
  // the next frame starts from "nothing reported": callers report every frame
  memset(s_new, 0, sizeof s_new);
}

extern char __start__[];
void input_key(int code, int down) {
  if (code == 4 && down && config.log_diag) // (debug: who sent BACK)
    log_printf("[input] BACK from nro+0x%lx", (unsigned long)((uintptr_t)__builtin_return_address(0) - (uintptr_t)__start__));
  const int64_t t = now_ms();
  kev.code = code;
  kev.action = down ? 0 : 1; // ACTION_DOWN / ACTION_UP
  kev.repeat = 0;
  kev.time = t;
  if (down)
    kev.down_time = t;
  unity_inject(s_key);
}

// --- the soft keyboard -------------------------------------------------------------------

int test_script_keyboard(char *out, size_t n);

typedef struct {
  char initial[512], placeholder[128];
  int type, secure;
} KbdReq;

// on the frame after the engine asked: answered inside showSoftInput, the
// engine sets the keyboard visible again after and the field stays in
// editing until the next touch (Android answers from its UI thread, later)
static void keyboard_show(void *arg) {
  KbdReq *rq = arg;
  SwkbdConfig kbd;
  char out[512] = "";
  Result rc = 0;
  if (test_script_keyboard(out, sizeof out))
    goto answered;
  if (R_FAILED(swkbdCreate(&kbd, 0))) {
    free(rq);
    return;
  }
  swkbdConfigMakePresetDefault(&kbd);
  if (rq->initial[0])
    swkbdConfigSetInitialText(&kbd, rq->initial);
  if (rq->placeholder[0])
    swkbdConfigSetGuideText(&kbd, rq->placeholder);
  if (rq->secure)
    swkbdConfigMakePresetPassword(&kbd);
  rc = swkbdShow(&kbd, out, sizeof out);
  swkbdClose(&kbd);
answered:
  free(rq);
  void (*set_str)(void *, void *, void *) = jni_native("com/unity3d/player/UnityPlayer", "nativeSetInputString");
  void (*closed)(void *, void *) = jni_native("com/unity3d/player/UnityPlayer", "nativeSoftInputClosed");
  void (*canceled)(void *, void *) = jni_native("com/unity3d/player/UnityPlayer", "nativeSoftInputCanceled");
  if (R_SUCCEEDED(rc)) {
    if (set_str)
      set_str(jni_env(), jni_unity_player(), jni_str(out));
    if (closed)
      closed(jni_env(), jni_unity_player());
  } else if (canceled) {
    canceled(jni_env(), jni_unity_player());
  }
}

void unity_soft_keyboard(const char *initial, int type, int multiline, int secure,
                         const char *placeholder) {
  (void)multiline;
  log_printf("[input] keyboard requested (type %d)", type);
  KbdReq *rq = calloc(1, sizeof *rq);
  if (!rq)
    return;
  snprintf(rq->initial, sizeof rq->initial, "%s", initial ? initial : "");
  snprintf(rq->placeholder, sizeof rq->placeholder, "%s", placeholder ? placeholder : "");
  rq->type = type;
  rq->secure = secure;
  java_android_later(keyboard_show, rq);
}

void input_init(void) {
  jni_set_super("android/view/MotionEvent", "android/view/InputEvent");
  jni_set_super("android/view/KeyEvent", "android/view/InputEvent");
  s_motion = jni_new("android/view/MotionEvent");
  s_key = jni_new("android/view/KeyEvent");
  s_device = jni_new("android/view/InputDevice");

  const char *M = "android/view/MotionEvent";
  jni_method(M, "getAction", NULL, me_getAction);
  jni_method(M, "getActionMasked", NULL, me_getActionMasked);
  jni_method(M, "getActionIndex", NULL, me_getActionIndex);
  jni_method(M, "getPointerCount", NULL, me_getPointerCount);
  jni_method(M, "getPointerId", NULL, me_getPointerId);
  jni_method(M, "findPointerIndex", NULL, me_findPointerIndex);
  jni_method(M, "getX", NULL, me_getX);
  jni_method(M, "getY", NULL, me_getY);
  jni_method(M, "getRawX", NULL, me_getX);
  jni_method(M, "getRawY", NULL, me_getY);
  jni_method(M, "getAxisValue", NULL, me_getAxisValue);
  jni_method(M, "getEventTime", NULL, me_getEventTime);
  jni_method(M, "getEventTimeNanos", NULL, me_getEventTimeNanos);
  jni_method(M, "getDownTime", NULL, me_getDownTime);
  jni_method(M, "getSource", NULL, me_getSource);
  jni_method(M, "getDeviceId", NULL, me_getDeviceId);
  jni_method(M, "getToolType", NULL, me_getToolType);
  jni_method(M, "getPressure", NULL, me_getPressure);
  jni_method(M, "getSize", NULL, me_getSize);
  jni_method(M, "getTouchMajor", NULL, me_getTouchMajor);
  jni_method(M, "getTouchMinor", NULL, me_getTouchMajor);
  jni_method(M, "getToolMajor", NULL, me_getTouchMajor);
  jni_method(M, "getToolMinor", NULL, me_getTouchMajor);
  jni_method(M, "getOrientation", NULL, me_zero_f);
  jni_method(M, "getHistorySize", NULL, me_zero_i);
  jni_method(M, "getHistoricalX", NULL, me_getX);
  jni_method(M, "getHistoricalY", NULL, me_getY);
  jni_method(M, "getHistoricalEventTime", NULL, me_getEventTime);
  jni_method(M, "getMetaState", NULL, me_zero_i);
  jni_method(M, "getButtonState", NULL, me_zero_i);
  jni_method(M, "getFlags", NULL, me_zero_i);
  jni_method(M, "getEdgeFlags", NULL, me_zero_i);
  jni_method(M, "getDisplayId", NULL, me_zero_i);
  jni_method(M, "getXPrecision", NULL, me_zero_f);
  jni_method(M, "getYPrecision", NULL, me_zero_f);
  jni_method(M, "isFromSource", NULL, me_isFromSource);
  jni_method(M, "getDevice", NULL, me_getDevice);
  jni_method(M, "getClassification", NULL, me_zero_i);
  jni_method(M, "obtain", "(Landroid/view/MotionEvent;)Landroid/view/MotionEvent;", me_obtain);
  jni_method(M, "recycle", NULL, me_recycle);

  const char *K = "android/view/KeyEvent";
  jni_method(K, "getAction", NULL, ke_getAction);
  jni_method(K, "getKeyCode", NULL, ke_getKeyCode);
  jni_method(K, "getRepeatCount", NULL, ke_getRepeatCount);
  jni_method(K, "getEventTime", NULL, ke_getEventTime);
  jni_method(K, "getDownTime", NULL, ke_getDownTime);
  jni_method(K, "getSource", NULL, ke_getSource);
  jni_method(K, "getDeviceId", NULL, me_getDeviceId);
  jni_method(K, "getMetaState", NULL, me_zero_i);
  jni_method(K, "getFlags", NULL, me_zero_i);
  jni_method(K, "getScanCode", NULL, me_zero_i);
  jni_method(K, "getUnicodeChar", NULL, ke_getUnicodeChar);
  jni_method(K, "getCharacters", NULL, ke_getCharacters);
  jni_method(K, "isFromSource", NULL, ke_isFromSource);
  jni_method(K, "getDevice", NULL, me_getDevice);
  jni_method(K, "isLongPress", NULL, me_zero_i);
  jni_method(K, "getDisplayLabel", NULL, me_zero_i);

  const char *D = "android/view/InputDevice";
  jni_method(D, "getDevice", NULL, dev_get);
  jni_method(D, "getDeviceIds", NULL, dev_getIds);
  jni_method(D, "getName", NULL, dev_getName);
  jni_method(D, "getSources", NULL, dev_getSources);
  jni_method(D, "getId", NULL, dev_getId);
  jni_method(D, "isVirtual", NULL, dev_isVirtual);
  jni_method(D, "isExternal", NULL, dev_isVirtual);
  jni_method(D, "getMotionRange", NULL, dev_getMotionRange);
  jni_method(D, "getMotionRanges", NULL, (JHandler)me_zero_j);
  jni_method(D, "getDescriptor", NULL, dev_getDescriptor);
  jni_method(D, "getKeyboardType", NULL, dev_getKeyboardType);
  jni_method(D, "getVendorId", NULL, me_zero_i);
  jni_method(D, "getProductId", NULL, me_zero_i);
  jni_method(D, "hasKeys", NULL, (JHandler)me_zero_j);
  jni_method("android/hardware/input/InputManager", "getInputDeviceIds", NULL, dev_getIds);
  jni_method("android/hardware/input/InputManager", "getInputDevice", NULL, dev_get);
}
