/* jni.h -- the Java side of Android, emulated (jni.c and java_*.c)
 *
 * The game's code reaches Java through JNI: Unity's engine for the
 * platform (context paths, display, input events), the game's C# through
 * Unity's AndroidJavaObject (which goes through com.unity3d.player.
 * ReflectionHelper and JNI), and the SDKs' natives. There is no Java here:
 * jni.c implements the JNIEnv/JavaVM function tables over a small object
 * model, and Java methods are C handlers registered by class and name
 * (java_android.c, java_unity.c, java_game.c, java_npf.c). A method nobody
 * handles is logged once ("[jni] unhandled ...") and returns 0/null.
 *
 * Objects are never freed (local and global references are no-ops); the
 * engine and the game use JNI lightly enough for that. What runs every frame
 * (the Choreographer's doFrame) reuses its objects instead.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef MKT_JNI_H
#define MKT_JNI_H

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

typedef union {
  uint8_t z;
  int8_t b;
  uint16_t c;
  int16_t s;
  int32_t i;
  int64_t j;
  float f;
  double d;
  void *l;
} jvalue;

typedef struct JClass JClass;
typedef struct JObj JObj;
typedef struct JMethod JMethod;

enum {
  JK_OBJECT = 1,
  JK_CLASS,
  JK_STRING,
  JK_ARRAY,
  JK_PROXY,      // a java.lang.reflect.Proxy made by ReflectionHelper (C#'s AndroidJavaProxy)
  JK_REFMETHOD,  // java.lang.reflect.Method / Constructor (payload: JMethod*)
  JK_REFFIELD,   // java.lang.reflect.Field (payload: JField*)
  JK_BOXED,      // Integer, Long, Float, Double, Boolean, ... (payload: JBoxed)
  JK_THROWABLE,  // payload: the message (char*)
};

typedef struct JFieldVal JFieldVal;

struct JObj {
  uint32_t magic;
  uint16_t kind;
  uint16_t flags;
  JClass *cls;        // the runtime class (getClass())
  void *native;       // per-kind payload, or a handler's own state
  JFieldVal *fields;  // instance field values set so far
};

struct JClass {
  JObj obj;           // a Class object itself (class java/lang/Class)
  char *name;         // "android/content/Context"
  JClass *super;
  JClass *ifaces[8];
  int nifaces;
  JFieldVal *statics; // static field values
};

typedef struct {
  char type;          // 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D'
  jvalue v;
} JBoxed;

typedef struct {
  char *utf8;         // modified UTF-8 (NUL-terminated)
  uint16_t *u16;      // UTF-16 (NUL-terminated)
  int len16;
  int len8;
} JStr;

typedef struct {
  char type;          // element type: 'Z' 'B' 'C' 'S' 'I' 'J' 'F' 'D', 'L' for objects
  int len;
  size_t esize;
  void *data;
} JArr;

typedef struct {
  int64_t ptr;        // Unity's proxy handle (the invoke native's first argument)
  JClass *iface;
  int bridge;         // 0: ReflectionHelper (C#'s AndroidJavaProxy), 1: JNIBridge (engine)
} JProxy;

// A Java method's implementation. thiz: the receiver (the Class object for
// a static method, the new object for a constructor); args: the arguments in
// declaration order (args[i].l for objects). The return value's member that
// matches the method's return type is read.
typedef jvalue (*JHandler)(JObj *thiz, JMethod *m, const jvalue *args);

struct JMethod {
  uint32_t magic;
  JClass *cls;
  char *name;
  char *sig;          // "(Ljava/lang/String;I)V"
  int is_static;
  JHandler h;         // NULL: unhandled
  void *native;       // a registered native (RegisterNatives), if it is one
  char ret;           // return type: 'V' 'Z' ... 'L' ('[' counts as 'L')
  int nargs;
  char argt[24];      // argument types ('L' for objects and arrays)
  unsigned calls;
  JObj *refl;         // its java.lang.reflect.Method, made on the first proxy call
};

// --- the VM ---------------------------------------------------------------------

void jni_init(void);
void *jni_env(void);   // JNIEnv* (one table, shared by every thread)
void *jni_vm(void);    // JavaVM*

// --- classes --------------------------------------------------------------------

JClass *jni_class(const char *name);                  // interned; '.' or '/' both work
void jni_set_super(const char *name, const char *super);
void jni_add_iface(const char *name, const char *iface);
int jni_instance_of(JObj *o, const char *cls);        // o's class is cls or derives from it
const char *jni_class_name(JObj *o);                  // "" for NULL

// --- registering Java methods and fields ------------------------------------------

// sig NULL matches any signature. cls "*" matches any class (a fallback).
// A handler on a class also serves its subclasses. Constructors are "<init>".
void jni_method(const char *cls, const char *name, const char *sig, JHandler h);
void jni_static_field(const char *cls, const char *name, jvalue v);
jvalue jni_get_static(const char *cls, const char *name, int *found);

// --- objects -----------------------------------------------------------------------

JObj *jni_new(const char *cls);                       // a plain object of class cls
void jni_set_field(JObj *o, const char *name, jvalue v);
jvalue jni_get_field(JObj *o, const char *name, int *found);
// the same field on o, as an int/long/float/object (0/NULL when unset)
int32_t jni_field_i(JObj *o, const char *name);

JObj *jni_str(const char *utf8);                      // NULL in -> NULL out
JObj *jni_strf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
const char *jni_utf(JObj *s);                         // NULL for NULL / non-strings
int jni_is_string(JObj *o);

JObj *jni_array(char type, int len, const char *elem_cls); // zeroed
JArr *jni_arr(JObj *a);                               // NULL for non-arrays
JObj *jni_byte_array(const void *data, int len);
JObj *jni_string_array(const char *const *items, int n);

JObj *jni_box(char type, jvalue v);                   // Integer.valueOf(...) etc.
int jni_unbox(JObj *o, jvalue *out, char *type);      // 1 when o was a boxed primitive

// java.util collections, as handlers expect them (java_android.c)
JObj *jni_list_new(void);                             // java/util/ArrayList
void jni_list_add(JObj *list, JObj *item);
int jni_list_size(JObj *list);
JObj *jni_list_get(JObj *list, int i);
JObj *jni_map_new(void);                              // java/util/HashMap
void jni_map_put(JObj *map, JObj *key, JObj *value);
JObj *jni_map_get_s(JObj *map, const char *key);
int jni_map_size(JObj *map);
JObj *jni_map_key_at(JObj *map, int i);
JObj *jni_map_val_at(JObj *map, int i);
void jni_map_remove_s(JObj *map, const char *key);

// --- exceptions --------------------------------------------------------------------

void jni_throw(const char *cls, const char *msg);     // pending on this thread
int jni_pending(void);
JObj *jni_take_pending(void);                         // this thread's pending one, cleared (NULL: none)

// --- calling the other way: game natives ------------------------------------------

// A native the game registered with RegisterNatives (or exported as
// Java_<class>_<name>), NULL when none.
void *jni_native(const char *cls, const char *name);
// a java.nio.DirectByteBuffer over addr (the caller keeps the memory)
JObj *jni_direct_buffer(void *addr, int64_t cap);
// Call a proxy's method: a C# AndroidJavaProxy through Unity's
// nativeProxyInvoke, an engine proxy through JNIBridge.invoke. sig: the
// Java signature of the method ("()V"), for the engine's reflection.
JObj *jni_proxy_invoke(JObj *proxy, const char *method, const char *sig, JObj **args, int nargs);
// The same, as declared by interface iface ("android/view/Choreographer$
// FrameCallback"): an engine proxy made for several interfaces only answers
// a call tagged with the interface that declares the method. NULL: the
// proxy's first interface.
JObj *jni_proxy_invoke_as(JObj *proxy, const char *iface, const char *method, const char *sig,
                          JObj **args, int nargs);
// ... with the arguments already in an Object[] (args): for calls made every
// frame, which then allocate nothing (objects are never freed)
JObj *jni_proxy_call(JObj *proxy, const char *iface, const char *method, const char *sig, JObj *args);
int jni_is_proxy(JObj *o);

// --- for the java_*.c handlers (jni.c) -------------------------------------------------
int jni_valid(const void *o);
JObj *jni_reflect_method(JClass *c, const char *name, const char *sig, int is_static);
JObj *jni_reflect_field(JClass *c, const char *name, const char *sig, int is_static);
const char *jni_reflect_name(JObj *o);
const char *jni_reflect_sig(JObj *o);
JObj *jni_new_proxy(int64_t ptr, JClass *iface, int bridge);
JObj *jni_new_throwable(const char *cls, const char *msg);
const char *jni_throwable_msg(JObj *o);

// --- helpers for handlers -------------------------------------------------------------

static inline jvalue jv_i(int32_t i) { jvalue v; v.j = 0; v.i = i; return v; }
static inline jvalue jv_j(int64_t j) { jvalue v; v.j = j; return v; }
static inline jvalue jv_z(int z) { jvalue v; v.j = 0; v.z = z ? 1 : 0; return v; }
static inline jvalue jv_f(float f) { jvalue v; v.j = 0; v.f = f; return v; }
static inline jvalue jv_d(double d) { jvalue v; v.d = d; return v; }
static inline jvalue jv_l(void *l) { jvalue v; v.j = 0; v.l = l; return v; }
static inline jvalue jv_v(void) { jvalue v; v.j = 0; return v; }

// declare and register in one go (java_*.c)
#define JH(fn) static jvalue fn(JObj *thiz, JMethod *m, const jvalue *a)
#define JUNUSED (void)thiz; (void)m; (void)a

// the per-area registration functions (each registers its classes)
void java_lang_init(void);
void java_android_init(void);
void java_android_frame(void);   // the main loop, before each frame (java_android.c)
// fn(arg) from the main loop before the next frame (java_android.c)
void java_android_later(void (*fn)(void *arg), void *arg);
void java_unity_init(void);
// UnityPlayer.UnitySendMessage(obj, method, msg): queued by Unity for that
// C# object's method, on its main thread (java_unity.c)
void java_unity_send(const char *obj, const char *method, const char *msg);
// Settings.Secure ANDROID_ID: 16 hex digits, stable per console (java_android.c)
const char *java_android_id(void);
void java_sdt_init(void); // java_sdt.c

// the objects the engine is driven with (java_unity.c)
JObj *jni_activity(void);       // the UnityPlayerActivity (also the Context)
JObj *jni_unity_player(void);   // the UnityPlayer
JObj *jni_surface(void);        // the android.view.Surface of our window

#endif
