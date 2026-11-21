/* jni.c -- the JNIEnv / JavaVM function tables over a small object model
 * (see jni.h)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <switch.h>

#include "config.h"
#include "error.h"
#include "jni.h"
#include "log.h"

#define OBJ_MAGIC   0x4a424f4au // "JOBJ"
#define METH_MAGIC  0x4854454du // "METH"
#define FIELD_MAGIC 0x444c4946u // "FILD"

#define JF_PERMANENT 1

struct JFieldVal {
  JFieldVal *next;
  const char *name;   // interned (never freed)
  jvalue v;
};

typedef struct {
  uint32_t magic;
  JClass *cls;
  char *name;
  char *sig;
  int is_static;
  char type;          // 'L' for objects and arrays
  unsigned misses;
} JField;

static RMutex g_lock;

// --- small string-keyed hash map ----------------------------------------------------

typedef struct MapNode {
  struct MapNode *next;
  void *val;
  char key[];
} MapNode;

typedef struct {
  MapNode **b;
  unsigned cap;
} Map;

static uint32_t h32(const char *s) {
  uint32_t h = 2166136261u;
  for (; *s; s++)
    h = (h ^ (uint8_t)*s) * 16777619u;
  return h;
}

static void map_init(Map *m, unsigned cap) {
  m->cap = cap;
  m->b = calloc(cap, sizeof(MapNode *));
}

static void *map_get(Map *m, const char *k) {
  for (MapNode *n = m->b[h32(k) & (m->cap - 1)]; n; n = n->next)
    if (!strcmp(n->key, k))
      return n->val;
  return NULL;
}

static void map_put(Map *m, const char *k, void *v) {
  const unsigned i = h32(k) & (m->cap - 1);
  for (MapNode *n = m->b[i]; n; n = n->next)
    if (!strcmp(n->key, k)) {
      n->val = v;
      return;
    }
  const size_t l = strlen(k);
  MapNode *n = malloc(sizeof(*n) + l + 1);
  memcpy(n->key, k, l + 1);
  n->val = v;
  n->next = m->b[i];
  m->b[i] = n;
}

static Map g_classes;    // name -> JClass*
static Map g_handlers;   // method name -> HEntry list
static Map g_methods;    // "S|cls|name|sig" -> JMethod*
static Map g_fields;     // "S|cls|name|sig" -> JField*
static Map g_natives;    // "cls|name" -> fn
static Map g_names;      // interned field names

typedef struct HEntry {
  struct HEntry *next;
  char *cls;    // "*" = any
  char *sig;    // NULL = any
  JHandler h;
} HEntry;

static const char *intern(const char *s) {
  const char *v = map_get(&g_names, s);
  if (v)
    return v;
  char *d = strdup(s);
  map_put(&g_names, s, d);
  return d;
}

// --- objects ------------------------------------------------------------------------

static JClass *g_class_class;  // java/lang/Class
static JClass *g_string_class;

static int valid(const void *p) {
  const JObj *o = p;
  return o && ((uintptr_t)o & 7) == 0 && (uintptr_t)o > 0x10000 && o->magic == OBJ_MAGIC;
}

static JObj *obj_new(JClass *cls, int kind) {
  JObj *o = calloc(1, sizeof(JObj));
  o->magic = OBJ_MAGIC;
  o->kind = (uint16_t)kind;
  o->cls = cls;
  return o;
}

static void norm_name(const char *in, char *out, size_t n) {
  // "java.lang.String" and "Ljava/lang/String;" both mean java/lang/String
  if (in[0] == 'L' && in[strlen(in) - 1] == ';' && strchr(in, '/')) {
    snprintf(out, n, "%.*s", (int)strlen(in) - 2, in + 1);
  } else {
    snprintf(out, n, "%s", in);
  }
  for (char *p = out; *p; p++)
    if (*p == '.')
      *p = '/';
}

static JClass *class_get(const char *name0) {
  char name[256];
  norm_name(name0, name, sizeof name);
  rmutexLock(&g_lock);
  JClass *c = map_get(&g_classes, name);
  if (!c) {
    c = calloc(1, sizeof(JClass));
    c->obj.magic = OBJ_MAGIC;
    c->obj.kind = JK_CLASS;
    c->obj.flags = JF_PERMANENT;
    c->obj.cls = g_class_class; // NULL only while java/lang/Class itself is made
    c->name = strdup(name);
    map_put(&g_classes, name, c);
    if (strcmp(name, "java/lang/Object") != 0)
      c->super = class_get("java/lang/Object");
  }
  rmutexUnlock(&g_lock);
  return c;
}

JClass *jni_class(const char *name) {
  return class_get(name);
}

void jni_set_super(const char *name, const char *super) {
  jni_class(name)->super = jni_class(super);
}

void jni_add_iface(const char *name, const char *iface) {
  JClass *c = jni_class(name);
  if (c->nifaces < 8)
    c->ifaces[c->nifaces++] = jni_class(iface);
}

static int class_derives(JClass *c, JClass *target) {
  for (int depth = 0; c && depth < 32; c = c->super, depth++) {
    if (c == target)
      return 1;
    for (int i = 0; i < c->nifaces; i++)
      if (class_derives(c->ifaces[i], target))
        return 1;
  }
  return 0;
}

int jni_instance_of(JObj *o, const char *cls) {
  if (!valid(o))
    return 0;
  return class_derives(o->cls, jni_class(cls));
}

const char *jni_class_name(JObj *o) {
  return valid(o) && o->cls ? o->cls->name : "";
}

JObj *jni_new(const char *cls) {
  return obj_new(jni_class(cls), JK_OBJECT);
}

static JFieldVal **field_slot(JFieldVal **list, const char *name) {
  for (JFieldVal **f = list; *f; f = &(*f)->next)
    if (!strcmp((*f)->name, name))
      return f;
  return NULL;
}

void jni_set_field(JObj *o, const char *name, jvalue v) {
  if (!valid(o))
    return;
  rmutexLock(&g_lock);
  JFieldVal **s = field_slot(&o->fields, name);
  if (s) {
    (*s)->v = v;
  } else {
    JFieldVal *f = calloc(1, sizeof(*f));
    f->name = intern(name);
    f->v = v;
    f->next = o->fields;
    o->fields = f;
  }
  rmutexUnlock(&g_lock);
}

jvalue jni_get_field(JObj *o, const char *name, int *found) {
  jvalue v = jv_v();
  int ok = 0;
  if (valid(o)) {
    rmutexLock(&g_lock);
    JFieldVal **s = field_slot(&o->fields, name);
    if (s) {
      v = (*s)->v;
      ok = 1;
    }
    rmutexUnlock(&g_lock);
  }
  if (found)
    *found = ok;
  return v;
}

int32_t jni_field_i(JObj *o, const char *name) {
  return jni_get_field(o, name, NULL).i;
}

void jni_static_field(const char *cls, const char *name, jvalue v) {
  JClass *c = jni_class(cls);
  rmutexLock(&g_lock);
  JFieldVal **s = field_slot(&c->statics, name);
  if (s) {
    (*s)->v = v;
  } else {
    JFieldVal *f = calloc(1, sizeof(*f));
    f->name = intern(name);
    f->v = v;
    f->next = c->statics;
    c->statics = f;
  }
  rmutexUnlock(&g_lock);
}

jvalue jni_get_static(const char *cls, const char *name, int *found) {
  jvalue v = jv_v();
  int ok = 0;
  rmutexLock(&g_lock);
  for (JClass *c = jni_class(cls); c && !ok; c = c->super) {
    JFieldVal **s = field_slot(&c->statics, name);
    if (s) {
      v = (*s)->v;
      ok = 1;
    }
  }
  rmutexUnlock(&g_lock);
  if (found)
    *found = ok;
  return v;
}

// --- strings --------------------------------------------------------------------------

static JObj *str_from_utf8(const char *s, int n) {
  JObj *o = obj_new(g_string_class, JK_STRING);
  JStr *js = calloc(1, sizeof(JStr));
  js->utf8 = malloc((size_t)n + 1);
  memcpy(js->utf8, s, (size_t)n);
  js->utf8[n] = '\0';
  js->len8 = n;
  js->u16 = malloc(((size_t)n + 1) * 2);
  int k = 0;
  for (int i = 0; i < n;) {
    const uint8_t c = (uint8_t)s[i];
    uint32_t cp;
    if (c < 0x80) {
      cp = c;
      i++;
    } else if ((c & 0xe0) == 0xc0 && i + 1 < n) {
      cp = (uint32_t)(c & 0x1f) << 6 | (s[i + 1] & 0x3f);
      i += 2;
    } else if ((c & 0xf0) == 0xe0 && i + 2 < n) {
      cp = (uint32_t)(c & 0x0f) << 12 | (uint32_t)(s[i + 1] & 0x3f) << 6 | (s[i + 2] & 0x3f);
      i += 3;
    } else if ((c & 0xf8) == 0xf0 && i + 3 < n) {
      cp = (uint32_t)(c & 0x07) << 18 | (uint32_t)(s[i + 1] & 0x3f) << 12 |
           (uint32_t)(s[i + 2] & 0x3f) << 6 | (s[i + 3] & 0x3f);
      i += 4;
    } else {
      cp = 0xfffd;
      i++;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      js->u16[k++] = (uint16_t)(0xd800 | (cp >> 10));
      js->u16[k++] = (uint16_t)(0xdc00 | (cp & 0x3ff));
    } else {
      js->u16[k++] = (uint16_t)cp;
    }
  }
  js->u16[k] = 0;
  js->len16 = k;
  o->native = js;
  return o;
}

static JObj *str_from_utf16(const uint16_t *u, int n) {
  char *buf = malloc((size_t)n * 4 + 1);
  int k = 0;
  for (int i = 0; i < n; i++) {
    uint32_t cp = u[i];
    if (cp >= 0xd800 && cp < 0xdc00 && i + 1 < n && u[i + 1] >= 0xdc00 && u[i + 1] < 0xe000) {
      cp = 0x10000 + ((cp - 0xd800) << 10) + (u[i + 1] - 0xdc00);
      i++;
    }
    if (cp < 0x80) {
      buf[k++] = (char)cp;
    } else if (cp < 0x800) {
      buf[k++] = (char)(0xc0 | (cp >> 6));
      buf[k++] = (char)(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      buf[k++] = (char)(0xe0 | (cp >> 12));
      buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      buf[k++] = (char)(0x80 | (cp & 0x3f));
    } else {
      buf[k++] = (char)(0xf0 | (cp >> 18));
      buf[k++] = (char)(0x80 | ((cp >> 12) & 0x3f));
      buf[k++] = (char)(0x80 | ((cp >> 6) & 0x3f));
      buf[k++] = (char)(0x80 | (cp & 0x3f));
    }
  }
  JObj *o = str_from_utf8(buf, k);
  free(buf);
  return o;
}

JObj *jni_str(const char *utf8) {
  return utf8 ? str_from_utf8(utf8, (int)strlen(utf8)) : NULL;
}

JObj *jni_strf(const char *fmt, ...) {
  char buf[2048];
  va_list va;
  va_start(va, fmt);
  vsnprintf(buf, sizeof buf, fmt, va);
  va_end(va);
  return jni_str(buf);
}

int jni_is_string(JObj *o) {
  return valid(o) && o->kind == JK_STRING;
}

const char *jni_utf(JObj *s) {
  return jni_is_string(s) ? ((JStr *)s->native)->utf8 : NULL;
}

static JStr *jstr(JObj *s) {
  return jni_is_string(s) ? (JStr *)s->native : NULL;
}

// --- arrays ----------------------------------------------------------------------------

static size_t elem_size(char t) {
  switch (t) {
    case 'Z': case 'B': return 1;
    case 'C': case 'S': return 2;
    case 'I': case 'F': return 4;
    default: return 8; // J D L
  }
}

JObj *jni_array(char type, int len, const char *elem_cls) {
  char cname[300];
  if (type == 'L')
    snprintf(cname, sizeof cname, "[L%s;", elem_cls ? elem_cls : "java/lang/Object");
  else
    snprintf(cname, sizeof cname, "[%c", type);
  JObj *o = obj_new(jni_class(cname), JK_ARRAY);
  JArr *a = calloc(1, sizeof(JArr));
  a->type = type;
  a->len = len < 0 ? 0 : len;
  a->esize = elem_size(type);
  a->data = calloc((size_t)a->len + 1, a->esize);
  o->native = a;
  return o;
}

JArr *jni_arr(JObj *a) {
  return valid(a) && a->kind == JK_ARRAY ? (JArr *)a->native : NULL;
}

JObj *jni_byte_array(const void *data, int len) {
  JObj *o = jni_array('B', len, NULL);
  if (data && len > 0)
    memcpy(jni_arr(o)->data, data, (size_t)len);
  return o;
}

JObj *jni_string_array(const char *const *items, int n) {
  JObj *o = jni_array('L', n, "java/lang/String");
  JObj **d = jni_arr(o)->data;
  for (int i = 0; i < n; i++)
    d[i] = jni_str(items[i]);
  return o;
}

// --- boxing ------------------------------------------------------------------------------

static const char *box_class(char t) {
  switch (t) {
    case 'Z': return "java/lang/Boolean";
    case 'B': return "java/lang/Byte";
    case 'C': return "java/lang/Character";
    case 'S': return "java/lang/Short";
    case 'I': return "java/lang/Integer";
    case 'J': return "java/lang/Long";
    case 'F': return "java/lang/Float";
    case 'D': return "java/lang/Double";
    default: return "java/lang/Object";
  }
}

JObj *jni_box(char type, jvalue v) {
  JObj *o = obj_new(jni_class(box_class(type)), JK_BOXED);
  JBoxed *b = calloc(1, sizeof(JBoxed));
  b->type = type;
  b->v = v;
  o->native = b;
  return o;
}

int jni_unbox(JObj *o, jvalue *out, char *type) {
  if (!valid(o) || o->kind != JK_BOXED)
    return 0;
  const JBoxed *b = o->native;
  if (out)
    *out = b->v;
  if (type)
    *type = b->type;
  return 1;
}

// --- exceptions -------------------------------------------------------------------------

static __thread JObj *t_exc;

static JObj *throwable_new(JClass *cls, const char *msg) {
  JObj *o = obj_new(cls, JK_THROWABLE);
  o->native = strdup(msg ? msg : "");
  return o;
}

void jni_throw(const char *cls, const char *msg) {
  t_exc = throwable_new(jni_class(cls), msg);
  log_printf("[jni] throw %s: %s", cls, msg ? msg : "");
}

int jni_pending(void) {
  return t_exc != NULL;
}

JObj *jni_take_pending(void) {
  JObj *e = t_exc;
  t_exc = NULL;
  return e;
}

// log_jni lines for one method: its first 10 calls, then the 100th, 1000th,
// ... (what the engine calls every frame would fill the log)
static int log_call(unsigned n) {
  if (n <= 10)
    return 1;
  while (n % 10 == 0)
    n /= 10;
  return n == 1;
}

// --- methods ------------------------------------------------------------------------------

static void parse_sig(JMethod *m) {
  const char *p = m->sig;
  m->nargs = 0;
  m->ret = 'V';
  if (!p || *p != '(')
    return;
  p++;
  while (*p && *p != ')') {
    char t = *p;
    if (t == '[') {
      while (*p == '[')
        p++;
      if (*p == 'L') {
        while (*p && *p != ';')
          p++;
      }
      if (*p)
        p++;
      t = 'L';
    } else if (t == 'L') {
      while (*p && *p != ';')
        p++;
      if (*p)
        p++;
    } else {
      p++;
    }
    if (m->nargs < (int)sizeof(m->argt))
      m->argt[m->nargs++] = t;
  }
  if (*p == ')') {
    p++;
    m->ret = (*p == '[') ? 'L' : (*p ? *p : 'V');
  }
}

void jni_method(const char *cls, const char *name, const char *sig, JHandler h) {
  char cn[256];
  if (strcmp(cls, "*") != 0)
    norm_name(cls, cn, sizeof cn);
  else
    snprintf(cn, sizeof cn, "*");
  HEntry *e = calloc(1, sizeof(*e));
  e->cls = strdup(cn);
  e->sig = sig ? strdup(sig) : NULL;
  e->h = h;
  rmutexLock(&g_lock);
  e->next = map_get(&g_handlers, name);
  map_put(&g_handlers, name, e);
  rmutexUnlock(&g_lock);
}

// a handler's signature matches on the parameters, as Java resolves
// overloads: C#'s AndroidJavaObject asks with its own return type
// ("()Ljava/lang/Object;" for getClass)
static int sig_match(const char *want, const char *sig) {
  const char *p = strchr(want, ')');
  const size_t n = p ? (size_t)(p - want) + 1 : strlen(want);
  return !strncmp(want, sig, n);
}

// the best handler for name/sig on class c: the nearest class up the chain
// (a matching signature beats a signature-less entry on the same class), then
// the "*" fallbacks
static JHandler find_handler(JClass *c, const char *name, const char *sig) {
  HEntry *list = map_get(&g_handlers, name);
  if (!list)
    return NULL;
  for (JClass *k = c; k; k = k->super) {
    JHandler any = NULL;
    for (HEntry *e = list; e; e = e->next) {
      if (strcmp(e->cls, k->name) != 0)
        continue;
      if (e->sig && sig && sig_match(e->sig, sig))
        return e->h;
      if (!e->sig && !any)
        any = e->h;
    }
    if (any)
      return any;
    for (int i = 0; i < k->nifaces; i++) {
      JHandler h = find_handler(k->ifaces[i], name, sig);
      if (h)
        return h;
    }
  }
  JHandler any = NULL;
  for (HEntry *e = list; e; e = e->next) {
    if (strcmp(e->cls, "*") != 0)
      continue;
    if (e->sig && sig && sig_match(e->sig, sig))
      return e->h;
    if (!e->sig && !any)
      any = e->h;
  }
  return any;
}

static JMethod *method_get(JClass *c, const char *name, const char *sig, int is_static) {
  char key[600];
  snprintf(key, sizeof key, "%d|%s|%s|%s", is_static, c->name, name, sig ? sig : "");
  rmutexLock(&g_lock);
  JMethod *m = map_get(&g_methods, key);
  if (!m) {
    m = calloc(1, sizeof(JMethod));
    m->magic = METH_MAGIC;
    m->cls = c;
    m->name = strdup(name);
    m->sig = strdup(sig ? sig : "()V");
    m->is_static = is_static;
    parse_sig(m);
    m->h = find_handler(c, name, sig);
    char nk[600];
    snprintf(nk, sizeof nk, "%s|%s", c->name, name);
    m->native = map_get(&g_natives, nk);
    map_put(&g_methods, key, m);
    if (config.log_jni || (!m->h && !m->native))
      log_printf("[jni] %s %s%s.%s%s", m->h ? "method" : "UNHANDLED", is_static ? "static " : "",
                 c->name, name, m->sig);
  }
  rmutexUnlock(&g_lock);
  return m;
}

static int valid_method(const JMethod *m) {
  return m && ((uintptr_t)m & 7) == 0 && (uintptr_t)m > 0x10000 && m->magic == METH_MAGIC;
}

static JObj *refmethod_new(JMethod *m) {
  JObj *o = obj_new(jni_class(strcmp(m->name, "<init>") ? "java/lang/reflect/Method"
                                                        : "java/lang/reflect/Constructor"),
                    JK_REFMETHOD);
  o->native = m;
  return o;
}

// --- natives & proxies -----------------------------------------------------------------

static void *s_proxy_invoke;   // ReflectionHelper.nativeProxyInvoke
static void *s_bridge_invoke;  // bitter.jnibridge.JNIBridge.invoke
static Map g_jnames;           // method name -> its Java String (C# proxy calls)

void *jni_native(const char *cls, const char *name) {
  char cn[256], key[600];
  norm_name(cls, cn, sizeof cn);
  snprintf(key, sizeof key, "%s|%s", cn, name);
  rmutexLock(&g_lock);
  void *fn = map_get(&g_natives, key);
  rmutexUnlock(&g_lock);
  return fn;
}

int jni_is_proxy(JObj *o) {
  return valid(o) && o->kind == JK_PROXY;
}

// The engine's proxies go through JNIBridge.invoke(ptr, Class, Method, args):
// the engine matches the Class and the method id against the ones it took
// from each interface, so both must be the declaring interface's. The
// reflected Method is made once per method; a C# proxy gets the method's
// name, one String per name.
JObj *jni_proxy_call(JObj *proxy, const char *iface, const char *method, const char *sig,
                     JObj *args) {
  if (!jni_is_proxy(proxy))
    return NULL;
  const JProxy *p = proxy->native;
  JClass *ic = iface ? jni_class(iface) : p->iface ? p->iface : jni_class("java/lang/Object");
  const int nargs = jni_arr(args) ? jni_arr(args)->len : 0;
  if (p->bridge) {
    JMethod *jm = method_get(ic, method, sig ? sig : "()V", 0);
    const unsigned n = __atomic_add_fetch(&jm->calls, 1, __ATOMIC_RELAXED);
    if (config.log_jni && log_call(n))
      log_printf("[jni] -> engine proxy %s.%s%s (%d args, call %u)", ic->name, method, jm->sig,
                 nargs, n);
    if (!s_bridge_invoke)
      s_bridge_invoke = jni_native("bitter/jnibridge/JNIBridge", "invoke");
    if (!s_bridge_invoke) {
      log_printf("[jni] no JNIBridge.invoke registered: cannot call %s on a proxy", method);
      return NULL;
    }
    JObj *mobj = __atomic_load_n(&jm->refl, __ATOMIC_ACQUIRE);
    if (!mobj) {
      // two threads may both make one: the spare is never used again
      mobj = refmethod_new(jm);
      __atomic_store_n(&jm->refl, mobj, __ATOMIC_RELEASE);
    }
    static JClass *bridge_cls;
    if (!bridge_cls)
      bridge_cls = jni_class("bitter/jnibridge/JNIBridge");
    JObj *(*fn)(void *, void *, int64_t, JObj *, JObj *, JObj *) = s_bridge_invoke;
    return fn(jni_env(), bridge_cls, p->ptr, &ic->obj, mobj, args);
  }
  if (config.log_jni)
    log_printf("[jni] -> C# proxy %s.%s%s (%d args)", ic->name, method, sig ? sig : "", nargs);
  if (!s_proxy_invoke)
    s_proxy_invoke = jni_native("com/unity3d/player/ReflectionHelper", "nativeProxyInvoke");
  if (!s_proxy_invoke) {
    log_printf("[jni] no nativeProxyInvoke registered: cannot call %s on a proxy", method);
    return NULL;
  }
  rmutexLock(&g_lock);
  JObj *name = map_get(&g_jnames, method);
  if (!name) {
    name = jni_str(method);
    map_put(&g_jnames, method, name);
  }
  rmutexUnlock(&g_lock);
  JObj *(*fn)(void *, void *, int64_t, JObj *, JObj *) = s_proxy_invoke;
  return fn(jni_env(), jni_class("com/unity3d/player/ReflectionHelper"), p->ptr, name, args);
}

JObj *jni_proxy_invoke_as(JObj *proxy, const char *iface, const char *method, const char *sig,
                          JObj **args, int nargs) {
  if (!jni_is_proxy(proxy))
    return NULL;
  JObj *arr = jni_array('L', nargs, "java/lang/Object");
  for (int i = 0; i < nargs; i++)
    ((JObj **)jni_arr(arr)->data)[i] = args[i];
  return jni_proxy_call(proxy, iface, method, sig, arr);
}

JObj *jni_proxy_invoke(JObj *proxy, const char *method, const char *sig, JObj **args, int nargs) {
  return jni_proxy_invoke_as(proxy, NULL, method, sig, args, nargs);
}

// a Java call on a proxy object: forward to C# with boxed arguments
static jvalue proxy_call(JObj *thiz, JMethod *m, const jvalue *a) {
  JObj *args[24];
  for (int i = 0; i < m->nargs; i++)
    args[i] = m->argt[i] == 'L' ? a[i].l : jni_box(m->argt[i], a[i]);
  JObj *r = jni_proxy_invoke(thiz, m->name, m->sig, args, m->nargs);
  jvalue v = jv_v();
  if (m->ret == 'L') {
    v.l = r;
  } else if (m->ret != 'V' && r) {
    jni_unbox(r, &v, NULL);
  }
  return v;
}

// --- the call path --------------------------------------------------------------------------

static jvalue invoke(JObj *thiz, JMethod *m, jvalue *args) {
  if (!valid_method(m)) {
    log_printf("[jni] call through a bad method id %p", (void *)m);
    return jv_v();
  }
  const unsigned n = __atomic_add_fetch(&m->calls, 1, __ATOMIC_RELAXED);
  if (config.log_jni && log_call(n)) {
    if (n > 10)
      log_printf("[jni] call %s.%s%s (call %u)", m->cls->name, m->name, m->sig, n);
    else
      log_printf("[jni] call %s.%s%s", m->cls->name, m->name, m->sig);
  }
  if (m->h)
    return m->h(thiz, m, args);
  if (thiz && valid(thiz) && thiz->kind == JK_PROXY)
    return proxy_call(thiz, m, args);
  if (n <= 3) {
    char argsbuf[512];
    int o = 0;
    argsbuf[0] = '\0';
    for (int i = 0; i < m->nargs && o < (int)sizeof argsbuf - 80; i++) {
      if (m->argt[i] == 'L' && args[i].l && valid(args[i].l) && ((JObj *)args[i].l)->kind == JK_STRING)
        o += snprintf(argsbuf + o, sizeof argsbuf - o, " \"%.60s\"", jni_utf(args[i].l));
      else if (m->argt[i] == 'I' || m->argt[i] == 'Z')
        o += snprintf(argsbuf + o, sizeof argsbuf - o, " %d", args[i].i);
    }
    log_printf("[jni] unhandled call %s%s.%s%s ->%s default", m->is_static ? "static " : "",
               m->cls->name, m->name, m->sig, argsbuf);
  }
  return jv_v();
}

static void va_args(const JMethod *m, va_list va, jvalue *out) {
  for (int i = 0; i < m->nargs; i++) {
    out[i].j = 0;
    switch (m->argt[i]) {
      case 'Z': case 'B': case 'C': case 'S': case 'I':
        out[i].i = va_arg(va, int);
        break;
      case 'J':
        out[i].j = va_arg(va, int64_t);
        break;
      case 'F':
        out[i].f = (float)va_arg(va, double);
        break;
      case 'D':
        out[i].d = va_arg(va, double);
        break;
      default:
        out[i].l = va_arg(va, void *);
        break;
    }
  }
}

static void a_args(const JMethod *m, const jvalue *in, jvalue *out) {
  for (int i = 0; i < m->nargs; i++) {
    out[i].j = 0;
    if (!in)
      continue;
    switch (m->argt[i]) {
      case 'Z': out[i].i = in[i].z; break;
      case 'B': out[i].i = in[i].b; break;
      case 'C': out[i].i = in[i].c; break;
      case 'S': out[i].i = in[i].s; break;
      case 'I': out[i].i = in[i].i; break;
      case 'F': out[i].f = in[i].f; break;
      default: out[i] = in[i]; break;
    }
  }
}

// --- the JNIEnv functions -------------------------------------------------------------------------

typedef void *JEnv;

static int32_t GetVersion(JEnv *env) { (void)env; return 0x00010006; }

static JClass *DefineClass(JEnv *env, const char *name, void *loader, const void *buf, int32_t len) {
  (void)env; (void)loader; (void)buf; (void)len;
  return name ? jni_class(name) : NULL;
}

// classes the engine must believe absent (it probes them to pick a path)
static const char *const absent_classes[] = {
  "com/google/ar/core/ArCoreApk",
};

static JClass *FindClass(JEnv *env, const char *name) {
  (void)env;
  if (!name)
    return NULL;
  char cn[256];
  norm_name(name, cn, sizeof cn);
  for (unsigned i = 0; i < sizeof(absent_classes) / sizeof(*absent_classes); i++) {
    if (!strcmp(cn, absent_classes[i])) {
      t_exc = throwable_new(jni_class("java/lang/ClassNotFoundException"), cn);
      if (config.log_jni)
        log_printf("[jni] FindClass %s -> absent", cn);
      return NULL;
    }
  }
  if (config.log_jni)
    log_printf("[jni] FindClass %s", cn);
  return jni_class(cn);
}

static JMethod *FromReflectedMethod(JEnv *env, JObj *o) {
  (void)env;
  return valid(o) && o->kind == JK_REFMETHOD ? o->native : NULL;
}

static JField *FromReflectedField(JEnv *env, JObj *o) {
  (void)env;
  return valid(o) && o->kind == JK_REFFIELD ? o->native : NULL;
}

static JObj *ToReflectedMethod(JEnv *env, JClass *c, JMethod *m, uint8_t is_static) {
  (void)env; (void)c; (void)is_static;
  return valid_method(m) ? refmethod_new(m) : NULL;
}

static JClass *GetSuperclass(JEnv *env, JClass *c) {
  (void)env;
  return valid(c) && c->obj.kind == JK_CLASS ? c->super : NULL;
}

static uint8_t IsAssignableFrom(JEnv *env, JClass *a, JClass *b) {
  (void)env;
  if (!valid(a) || !valid(b))
    return 0;
  return class_derives(a, b) || !strcmp(b->name, "java/lang/Object");
}

static JObj *ToReflectedField(JEnv *env, JClass *c, JField *f, uint8_t is_static) {
  (void)env; (void)c; (void)is_static;
  if (!f || f->magic != FIELD_MAGIC)
    return NULL;
  JObj *o = obj_new(jni_class("java/lang/reflect/Field"), JK_REFFIELD);
  o->native = f;
  return o;
}

static int32_t Throw(JEnv *env, JObj *t) {
  (void)env;
  t_exc = t;
  return 0;
}

static int32_t ThrowNew(JEnv *env, JClass *c, const char *msg) {
  (void)env;
  t_exc = throwable_new(valid(c) ? c : jni_class("java/lang/RuntimeException"), msg);
  return 0;
}

static JObj *ExceptionOccurred(JEnv *env) { (void)env; return t_exc; }

static void ExceptionDescribe(JEnv *env) {
  (void)env;
  if (t_exc)
    log_printf("[jni] pending exception %s: %s", t_exc->cls->name,
               t_exc->kind == JK_THROWABLE ? (const char *)t_exc->native : "");
}

static void ExceptionClear(JEnv *env) { (void)env; t_exc = NULL; }

static void FatalError(JEnv *env, const char *msg) {
  (void)env;
  fatal_error("Java FatalError:\n%s", msg ? msg : "");
}

static int32_t PushLocalFrame(JEnv *env, int32_t cap) { (void)env; (void)cap; return 0; }
static JObj *PopLocalFrame(JEnv *env, JObj *res) { (void)env; return res; }
static JObj *NewGlobalRef(JEnv *env, JObj *o) { (void)env; return o; }
static void DeleteGlobalRef(JEnv *env, JObj *o) { (void)env; (void)o; }
static void DeleteLocalRef(JEnv *env, JObj *o) { (void)env; (void)o; }
static uint8_t IsSameObject(JEnv *env, JObj *a, JObj *b) {
  (void)env;
  if (a == b)
    return 1;
  // equal strings stand for one interned object to the code asking
  return 0;
}
static JObj *NewLocalRef(JEnv *env, JObj *o) { (void)env; return o; }
static int32_t EnsureLocalCapacity(JEnv *env, int32_t cap) { (void)env; (void)cap; return 0; }

static JObj *AllocObject(JEnv *env, JClass *c) {
  (void)env;
  return valid(c) ? obj_new(c, JK_OBJECT) : NULL;
}

static JObj *new_with(JClass *c, JMethod *m, jvalue *a) {
  if (!valid(c))
    return NULL;
  JObj *o;
  if (!strcmp(c->name, "java/lang/String"))
    o = str_from_utf8("", 0);
  else
    o = obj_new(c, JK_OBJECT);
  if (valid_method(m) && m->h) {
    // a constructor handler may hand back a different object (a String's)
    jvalue r = m->h(o, m, a);
    if (r.l && valid(r.l))
      o = r.l;
  } else if (valid_method(m) && config.log_jni) {
    log_printf("[jni] new %s%s (no constructor handler)", c->name, m->sig);
  }
  return o;
}

static JObj *NewObject(JEnv *env, JClass *c, JMethod *m, ...) {
  (void)env;
  jvalue a[24];
  va_list va;
  va_start(va, m);
  if (valid_method(m))
    va_args(m, va, a);
  va_end(va);
  return new_with(c, m, a);
}

static JObj *NewObjectV(JEnv *env, JClass *c, JMethod *m, va_list va) {
  (void)env;
  jvalue a[24];
  if (valid_method(m))
    va_args(m, va, a);
  return new_with(c, m, a);
}

static JObj *NewObjectA(JEnv *env, JClass *c, JMethod *m, const jvalue *in) {
  (void)env;
  jvalue a[24];
  if (valid_method(m))
    a_args(m, in, a);
  return new_with(c, m, a);
}

static JClass *GetObjectClass(JEnv *env, JObj *o) {
  (void)env;
  if (!valid(o))
    return NULL;
  return o->cls ? o->cls : jni_class("java/lang/Object");
}

static uint8_t IsInstanceOf(JEnv *env, JObj *o, JClass *c) {
  (void)env;
  if (!o)
    return 1;
  if (!valid(o) || !valid(c))
    return 0;
  return class_derives(o->cls, c) || !strcmp(c->name, "java/lang/Object");
}

static JMethod *GetMethodID(JEnv *env, JClass *c, const char *name, const char *sig) {
  (void)env;
  if (!valid(c) || !name)
    return NULL;
  return method_get(c, name, sig, 0);
}

static JMethod *GetStaticMethodID(JEnv *env, JClass *c, const char *name, const char *sig) {
  (void)env;
  if (!valid(c) || !name)
    return NULL;
  return method_get(c, name, sig, 1);
}

// Call<Type>Method[V|A], CallNonvirtual..., CallStatic...
#define CALLS(Name, Type, M)                                                                     \
  static Type Call##Name##Method(JEnv *env, JObj *o, JMethod *m, ...) {                          \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    va_list va;                                                                                  \
    va_start(va, m);                                                                             \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    va_end(va);                                                                                  \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type Call##Name##MethodV(JEnv *env, JObj *o, JMethod *m, va_list va) {                  \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type Call##Name##MethodA(JEnv *env, JObj *o, JMethod *m, const jvalue *in) {            \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    if (valid_method(m)) a_args(m, in, a);                                                       \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type CallNonvirtual##Name##Method(JEnv *env, JObj *o, JClass *c, JMethod *m, ...) {     \
    (void)env; (void)c;                                                                          \
    jvalue a[24];                                                                                \
    va_list va;                                                                                  \
    va_start(va, m);                                                                             \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    va_end(va);                                                                                  \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type CallNonvirtual##Name##MethodV(JEnv *env, JObj *o, JClass *c, JMethod *m,           \
                                            va_list va) {                                        \
    (void)env; (void)c;                                                                          \
    jvalue a[24];                                                                                \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type CallNonvirtual##Name##MethodA(JEnv *env, JObj *o, JClass *c, JMethod *m,           \
                                            const jvalue *in) {                                  \
    (void)env; (void)c;                                                                          \
    jvalue a[24];                                                                                \
    if (valid_method(m)) a_args(m, in, a);                                                       \
    return (Type)invoke(o, m, a).M;                                                              \
  }                                                                                              \
  static Type CallStatic##Name##Method(JEnv *env, JClass *c, JMethod *m, ...) {                  \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    va_list va;                                                                                  \
    va_start(va, m);                                                                             \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    va_end(va);                                                                                  \
    return (Type)invoke(&c->obj, m, a).M;                                                        \
  }                                                                                              \
  static Type CallStatic##Name##MethodV(JEnv *env, JClass *c, JMethod *m, va_list va) {          \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    if (valid_method(m)) va_args(m, va, a);                                                      \
    return (Type)invoke(&c->obj, m, a).M;                                                        \
  }                                                                                              \
  static Type CallStatic##Name##MethodA(JEnv *env, JClass *c, JMethod *m, const jvalue *in) {    \
    (void)env;                                                                                   \
    jvalue a[24];                                                                                \
    if (valid_method(m)) a_args(m, in, a);                                                       \
    return (Type)invoke(&c->obj, m, a).M;                                                        \
  }

CALLS(Object, JObj *, l)
CALLS(Boolean, uint8_t, z)
CALLS(Byte, int8_t, b)
CALLS(Char, uint16_t, c)
CALLS(Short, int16_t, s)
CALLS(Int, int32_t, i)
CALLS(Long, int64_t, j)
CALLS(Float, float, f)
CALLS(Double, double, d)

static void CallVoidMethod(JEnv *env, JObj *o, JMethod *m, ...) {
  (void)env;
  jvalue a[24];
  va_list va;
  va_start(va, m);
  if (valid_method(m)) va_args(m, va, a);
  va_end(va);
  invoke(o, m, a);
}
static void CallVoidMethodV(JEnv *env, JObj *o, JMethod *m, va_list va) {
  (void)env;
  jvalue a[24];
  if (valid_method(m)) va_args(m, va, a);
  invoke(o, m, a);
}
static void CallVoidMethodA(JEnv *env, JObj *o, JMethod *m, const jvalue *in) {
  (void)env;
  jvalue a[24];
  if (valid_method(m)) a_args(m, in, a);
  invoke(o, m, a);
}
static void CallNonvirtualVoidMethod(JEnv *env, JObj *o, JClass *c, JMethod *m, ...) {
  (void)env; (void)c;
  jvalue a[24];
  va_list va;
  va_start(va, m);
  if (valid_method(m)) va_args(m, va, a);
  va_end(va);
  invoke(o, m, a);
}
static void CallNonvirtualVoidMethodV(JEnv *env, JObj *o, JClass *c, JMethod *m, va_list va) {
  (void)env; (void)c;
  jvalue a[24];
  if (valid_method(m)) va_args(m, va, a);
  invoke(o, m, a);
}
static void CallNonvirtualVoidMethodA(JEnv *env, JObj *o, JClass *c, JMethod *m, const jvalue *in) {
  (void)env; (void)c;
  jvalue a[24];
  if (valid_method(m)) a_args(m, in, a);
  invoke(o, m, a);
}
static void CallStaticVoidMethod(JEnv *env, JClass *c, JMethod *m, ...) {
  (void)env;
  jvalue a[24];
  va_list va;
  va_start(va, m);
  if (valid_method(m)) va_args(m, va, a);
  va_end(va);
  invoke(&c->obj, m, a);
}
static void CallStaticVoidMethodV(JEnv *env, JClass *c, JMethod *m, va_list va) {
  (void)env;
  jvalue a[24];
  if (valid_method(m)) va_args(m, va, a);
  invoke(&c->obj, m, a);
}
static void CallStaticVoidMethodA(JEnv *env, JClass *c, JMethod *m, const jvalue *in) {
  (void)env;
  jvalue a[24];
  if (valid_method(m)) a_args(m, in, a);
  invoke(&c->obj, m, a);
}

// --- fields ------------------------------------------------------------------------------------

static JField *field_get(JClass *c, const char *name, const char *sig, int is_static) {
  char key[600];
  snprintf(key, sizeof key, "%d|%s|%s|%s", is_static, c->name, name, sig ? sig : "");
  rmutexLock(&g_lock);
  JField *f = map_get(&g_fields, key);
  if (!f) {
    f = calloc(1, sizeof(JField));
    f->magic = FIELD_MAGIC;
    f->cls = c;
    f->name = strdup(name);
    f->sig = strdup(sig ? sig : "I");
    f->is_static = is_static;
    f->type = (f->sig[0] == '[') ? 'L' : f->sig[0];
    map_put(&g_fields, key, f);
    if (config.log_jni)
      log_printf("[jni] field %s%s.%s %s", is_static ? "static " : "", c->name, name, f->sig);
  }
  rmutexUnlock(&g_lock);
  return f;
}

static JField *GetFieldID(JEnv *env, JClass *c, const char *name, const char *sig) {
  (void)env;
  if (!valid(c) || !name)
    return NULL;
  return field_get(c, name, sig, 0);
}

static JField *GetStaticFieldID(JEnv *env, JClass *c, const char *name, const char *sig) {
  (void)env;
  if (!valid(c) || !name)
    return NULL;
  return field_get(c, name, sig, 1);
}

static jvalue field_read(JObj *o, JField *f) {
  if (!f || f->magic != FIELD_MAGIC)
    return jv_v();
  int found = 0;
  jvalue v;
  if (f->is_static) {
    v = jni_get_static(f->cls->name, f->name, &found);
    if (!found && valid(o) && o->kind == JK_CLASS)
      v = jni_get_static(((JClass *)o)->name, f->name, &found);
  } else {
    v = jni_get_field(o, f->name, &found);
  }
  if (!found && f->misses++ < 2)
    log_printf("[jni] unset %sfield %s.%s %s read -> 0", f->is_static ? "static " : "",
               f->cls->name, f->name, f->sig);
  return v;
}

static void field_write(JObj *o, JField *f, jvalue v) {
  if (!f || f->magic != FIELD_MAGIC)
    return;
  if (f->is_static)
    jni_static_field(f->cls->name, f->name, v);
  else
    jni_set_field(o, f->name, v);
}

#define FIELDS(Name, Type, M)                                                              \
  static Type Get##Name##Field(JEnv *env, JObj *o, JField *f) {                            \
    (void)env;                                                                             \
    return (Type)field_read(o, f).M;                                                       \
  }                                                                                        \
  static void Set##Name##Field(JEnv *env, JObj *o, JField *f, Type val) {                  \
    (void)env;                                                                             \
    jvalue v;                                                                              \
    v.j = 0;                                                                               \
    v.M = val;                                                                             \
    field_write(o, f, v);                                                                  \
  }                                                                                        \
  static Type GetStatic##Name##Field(JEnv *env, JClass *c, JField *f) {                    \
    (void)env;                                                                             \
    return (Type)field_read(&c->obj, f).M;                                                 \
  }                                                                                        \
  static void SetStatic##Name##Field(JEnv *env, JClass *c, JField *f, Type val) {          \
    (void)env;                                                                             \
    jvalue v;                                                                              \
    v.j = 0;                                                                               \
    v.M = val;                                                                             \
    field_write(&c->obj, f, v);                                                            \
  }

FIELDS(Object, JObj *, l)
FIELDS(Boolean, uint8_t, z)
FIELDS(Byte, int8_t, b)
FIELDS(Char, uint16_t, c)
FIELDS(Short, int16_t, s)
FIELDS(Int, int32_t, i)
FIELDS(Long, int64_t, j)
FIELDS(Float, float, f)
FIELDS(Double, double, d)

// --- strings ----------------------------------------------------------------------------------

static JObj *NewString(JEnv *env, const uint16_t *u, int32_t n) {
  (void)env;
  return str_from_utf16(u ? u : (const uint16_t *)u"", u ? n : 0);
}

static int32_t GetStringLength(JEnv *env, JObj *s) {
  (void)env;
  JStr *js = jstr(s);
  return js ? js->len16 : 0;
}

static const uint16_t *GetStringChars(JEnv *env, JObj *s, uint8_t *copy) {
  (void)env;
  if (copy)
    *copy = 0;
  JStr *js = jstr(s);
  return js ? js->u16 : (const uint16_t *)u"";
}

static void ReleaseStringChars(JEnv *env, JObj *s, const uint16_t *c) { (void)env; (void)s; (void)c; }

static JObj *NewStringUTF(JEnv *env, const char *s) {
  (void)env;
  return s ? str_from_utf8(s, (int)strlen(s)) : NULL;
}

static int32_t GetStringUTFLength(JEnv *env, JObj *s) {
  (void)env;
  JStr *js = jstr(s);
  return js ? js->len8 : 0;
}

static const char *GetStringUTFChars(JEnv *env, JObj *s, uint8_t *copy) {
  (void)env;
  if (copy)
    *copy = 0;
  JStr *js = jstr(s);
  if (!js && s)
    log_printf("[jni] GetStringUTFChars on a %s", jni_class_name(s));
  return js ? js->utf8 : "";
}

static void ReleaseStringUTFChars(JEnv *env, JObj *s, const char *c) { (void)env; (void)s; (void)c; }

static void GetStringRegion(JEnv *env, JObj *s, int32_t start, int32_t len, uint16_t *buf) {
  (void)env;
  JStr *js = jstr(s);
  if (!js || start < 0 || len < 0 || start + len > js->len16) {
    t_exc = throwable_new(jni_class("java/lang/StringIndexOutOfBoundsException"), "");
    return;
  }
  memcpy(buf, js->u16 + start, (size_t)len * 2);
}

static void GetStringUTFRegion(JEnv *env, JObj *s, int32_t start, int32_t len, char *buf) {
  (void)env;
  JStr *js = jstr(s);
  if (!js || start < 0 || len < 0 || start + len > js->len16) {
    t_exc = throwable_new(jni_class("java/lang/StringIndexOutOfBoundsException"), "");
    return;
  }
  JObj *sub = str_from_utf16(js->u16 + start, len);
  memcpy(buf, ((JStr *)sub->native)->utf8, (size_t)((JStr *)sub->native)->len8 + 1);
}

static const uint16_t *GetStringCritical(JEnv *env, JObj *s, uint8_t *copy) {
  return GetStringChars(env, s, copy);
}

static void ReleaseStringCritical(JEnv *env, JObj *s, const uint16_t *c) { (void)env; (void)s; (void)c; }

// --- arrays --------------------------------------------------------------------------------------

static int32_t GetArrayLength(JEnv *env, JObj *a) {
  (void)env;
  JArr *ar = jni_arr(a);
  return ar ? ar->len : 0;
}

static JObj *NewObjectArray(JEnv *env, int32_t len, JClass *ec, JObj *init) {
  (void)env;
  JObj *o = jni_array('L', len, valid(ec) ? ec->name : "java/lang/Object");
  JObj **d = jni_arr(o)->data;
  for (int i = 0; i < len; i++)
    d[i] = init;
  return o;
}

static JObj *GetObjectArrayElement(JEnv *env, JObj *a, int32_t i) {
  (void)env;
  JArr *ar = jni_arr(a);
  if (!ar || i < 0 || i >= ar->len) {
    t_exc = throwable_new(jni_class("java/lang/ArrayIndexOutOfBoundsException"), "");
    return NULL;
  }
  return ((JObj **)ar->data)[i];
}

static void SetObjectArrayElement(JEnv *env, JObj *a, int32_t i, JObj *v) {
  (void)env;
  JArr *ar = jni_arr(a);
  if (!ar || i < 0 || i >= ar->len) {
    t_exc = throwable_new(jni_class("java/lang/ArrayIndexOutOfBoundsException"), "");
    return;
  }
  ((JObj **)ar->data)[i] = v;
}

#define ARRAYS(Name, Type, T)                                                                     \
  static JObj *New##Name##Array(JEnv *env, int32_t len) {                                         \
    (void)env;                                                                                    \
    return jni_array(T, len, NULL);                                                               \
  }                                                                                               \
  static Type *Get##Name##ArrayElements(JEnv *env, JObj *a, uint8_t *copy) {                      \
    (void)env;                                                                                    \
    if (copy) *copy = 0;                                                                          \
    JArr *ar = jni_arr(a);                                                                        \
    return ar ? (Type *)ar->data : NULL;                                                          \
  }                                                                                               \
  static void Release##Name##ArrayElements(JEnv *env, JObj *a, Type *e, int32_t mode) {           \
    (void)env; (void)a; (void)e; (void)mode;                                                      \
  }                                                                                               \
  static void Get##Name##ArrayRegion(JEnv *env, JObj *a, int32_t s, int32_t n, Type *buf) {       \
    (void)env;                                                                                    \
    JArr *ar = jni_arr(a);                                                                        \
    if (!ar || s < 0 || n < 0 || s + n > ar->len) {                                               \
      t_exc = throwable_new(jni_class("java/lang/ArrayIndexOutOfBoundsException"), "");           \
      return;                                                                                     \
    }                                                                                             \
    memcpy(buf, (Type *)ar->data + s, (size_t)n * sizeof(Type));                                  \
  }                                                                                               \
  static void Set##Name##ArrayRegion(JEnv *env, JObj *a, int32_t s, int32_t n, const Type *buf) { \
    (void)env;                                                                                    \
    JArr *ar = jni_arr(a);                                                                        \
    if (!ar || s < 0 || n < 0 || s + n > ar->len) {                                               \
      t_exc = throwable_new(jni_class("java/lang/ArrayIndexOutOfBoundsException"), "");           \
      return;                                                                                     \
    }                                                                                             \
    memcpy((Type *)ar->data + s, buf, (size_t)n * sizeof(Type));                                  \
  }

ARRAYS(Boolean, uint8_t, 'Z')
ARRAYS(Byte, int8_t, 'B')
ARRAYS(Char, uint16_t, 'C')
ARRAYS(Short, int16_t, 'S')
ARRAYS(Int, int32_t, 'I')
ARRAYS(Long, int64_t, 'J')
ARRAYS(Float, float, 'F')
ARRAYS(Double, double, 'D')

static void *GetPrimitiveArrayCritical(JEnv *env, JObj *a, uint8_t *copy) {
  (void)env;
  if (copy)
    *copy = 0;
  JArr *ar = jni_arr(a);
  return ar ? ar->data : NULL;
}

static void ReleasePrimitiveArrayCritical(JEnv *env, JObj *a, void *p, int32_t mode) {
  (void)env; (void)a; (void)p; (void)mode;
}

// --- natives ------------------------------------------------------------------------------------

typedef struct {
  const char *name;
  const char *sig;
  void *fn;
} JNINativeMethod;

static int32_t RegisterNatives(JEnv *env, JClass *c, const JNINativeMethod *ms, int32_t n) {
  (void)env;
  if (!valid(c))
    return -1;
  for (int i = 0; i < n; i++) {
    char key[600];
    snprintf(key, sizeof key, "%s|%s", c->name, ms[i].name);
    rmutexLock(&g_lock);
    map_put(&g_natives, key, ms[i].fn);
    rmutexUnlock(&g_lock);
    if (config.log_jni)
      log_printf("[jni] native %s.%s%s = %p", c->name, ms[i].name, ms[i].sig, ms[i].fn);
  }
  log_printf("[jni] %s registered %d native(s)", c->name, (int)n);
  return 0;
}

static int32_t UnregisterNatives(JEnv *env, JClass *c) { (void)env; (void)c; return 0; }
static int32_t MonitorEnter(JEnv *env, JObj *o) { (void)env; (void)o; return 0; }
static int32_t MonitorExit(JEnv *env, JObj *o) { (void)env; (void)o; return 0; }

static int32_t GetJavaVM(JEnv *env, void **vm) {
  (void)env;
  *vm = jni_vm();
  return 0;
}

static JObj *NewWeakGlobalRef(JEnv *env, JObj *o) { (void)env; return o; }
static void DeleteWeakGlobalRef(JEnv *env, JObj *o) { (void)env; (void)o; }
static uint8_t ExceptionCheck(JEnv *env) { (void)env; return t_exc != NULL; }

typedef struct {
  void *addr;
  int64_t cap;
} JBuf;

static JObj *NewDirectByteBuffer(JEnv *env, void *addr, int64_t cap) {
  (void)env;
  JObj *o = jni_new("java/nio/DirectByteBuffer");
  JBuf *b = calloc(1, sizeof(JBuf));
  b->addr = addr;
  b->cap = cap;
  o->native = b;
  return o;
}

// a java.nio.DirectByteBuffer over memory of the port's (java_sdt.c: FMOD's buffer)
JObj *jni_direct_buffer(void *addr, int64_t cap) {
  return NewDirectByteBuffer(NULL, addr, cap);
}

static void *GetDirectBufferAddress(JEnv *env, JObj *o) {
  (void)env;
  if (!jni_instance_of(o, "java/nio/DirectByteBuffer"))
    return NULL;
  return ((JBuf *)o->native)->addr;
}

static int64_t GetDirectBufferCapacity(JEnv *env, JObj *o) {
  (void)env;
  if (!jni_instance_of(o, "java/nio/DirectByteBuffer"))
    return -1;
  return ((JBuf *)o->native)->cap;
}

static int32_t GetObjectRefType(JEnv *env, JObj *o) { (void)env; return valid(o) ? 2 : 0; }
static void *GetModule(JEnv *env, JClass *c) { (void)env; (void)c; return NULL; }

// --- the tables -------------------------------------------------------------------------------------

#define ENV_SLOTS 240
static void *s_env_table[ENV_SLOTS];
static void *s_env_ptr = s_env_table;   // JNIEnv: a pointer to the table
static void *s_vm_table[8];
static void *s_vm_ptr = s_vm_table;     // JavaVM

static void unknown_slot(void) {
  log_printf("[jni] a JNIEnv function this port does not provide was called");
  fatal_error("A JNI function the port does not provide was called.\nSee debug.log.");
}

static int32_t vm_destroy(void *vm) { (void)vm; return 0; }
static int32_t vm_attach(void *vm, void **env, void *args) {
  (void)vm; (void)args;
  *env = jni_env();
  return 0;
}
static int32_t vm_detach(void *vm) { (void)vm; return 0; }
static int32_t vm_get_env(void *vm, void **env, int32_t version) {
  (void)vm; (void)version;
  *env = jni_env();
  return 0;
}

void *jni_env(void) { return &s_env_ptr; }
void *jni_vm(void) { return &s_vm_ptr; }

void jni_init(void) {
  rmutexInit(&g_lock);
  map_init(&g_classes, 1024);
  map_init(&g_handlers, 1024);
  map_init(&g_methods, 4096);
  map_init(&g_fields, 1024);
  map_init(&g_natives, 256);
  map_init(&g_names, 512);
  map_init(&g_jnames, 64);

  g_class_class = class_get("java/lang/Class");
  g_class_class->obj.cls = g_class_class;
  // classes made before Class existed point at it now
  jni_class("java/lang/Object")->obj.cls = g_class_class;
  g_string_class = jni_class("java/lang/String");

  for (int i = 0; i < ENV_SLOTS; i++)
    s_env_table[i] = (void *)unknown_slot;
  void **t = s_env_table;
  t[4] = GetVersion; t[5] = DefineClass; t[6] = FindClass; t[7] = FromReflectedMethod;
  t[8] = FromReflectedField; t[9] = ToReflectedMethod; t[10] = GetSuperclass;
  t[11] = IsAssignableFrom; t[12] = ToReflectedField; t[13] = Throw; t[14] = ThrowNew;
  t[15] = ExceptionOccurred; t[16] = ExceptionDescribe; t[17] = ExceptionClear;
  t[18] = FatalError; t[19] = PushLocalFrame; t[20] = PopLocalFrame; t[21] = NewGlobalRef;
  t[22] = DeleteGlobalRef; t[23] = DeleteLocalRef; t[24] = IsSameObject; t[25] = NewLocalRef;
  t[26] = EnsureLocalCapacity; t[27] = AllocObject; t[28] = NewObject; t[29] = NewObjectV;
  t[30] = NewObjectA; t[31] = GetObjectClass; t[32] = IsInstanceOf; t[33] = GetMethodID;
#define SET3(i, N) t[i] = Call##N##Method; t[i + 1] = Call##N##MethodV; t[i + 2] = Call##N##MethodA;
#define SETNV3(i, N) t[i] = CallNonvirtual##N##Method; t[i + 1] = CallNonvirtual##N##MethodV; \
                     t[i + 2] = CallNonvirtual##N##MethodA;
#define SETST3(i, N) t[i] = CallStatic##N##Method; t[i + 1] = CallStatic##N##MethodV; \
                     t[i + 2] = CallStatic##N##MethodA;
  SET3(34, Object) SET3(37, Boolean) SET3(40, Byte) SET3(43, Char) SET3(46, Short)
  SET3(49, Int) SET3(52, Long) SET3(55, Float) SET3(58, Double) SET3(61, Void)
  SETNV3(64, Object) SETNV3(67, Boolean) SETNV3(70, Byte) SETNV3(73, Char) SETNV3(76, Short)
  SETNV3(79, Int) SETNV3(82, Long) SETNV3(85, Float) SETNV3(88, Double) SETNV3(91, Void)
  t[94] = GetFieldID;
  t[95] = GetObjectField; t[96] = GetBooleanField; t[97] = GetByteField; t[98] = GetCharField;
  t[99] = GetShortField; t[100] = GetIntField; t[101] = GetLongField; t[102] = GetFloatField;
  t[103] = GetDoubleField;
  t[104] = SetObjectField; t[105] = SetBooleanField; t[106] = SetByteField; t[107] = SetCharField;
  t[108] = SetShortField; t[109] = SetIntField; t[110] = SetLongField; t[111] = SetFloatField;
  t[112] = SetDoubleField;
  t[113] = GetStaticMethodID;
  SETST3(114, Object) SETST3(117, Boolean) SETST3(120, Byte) SETST3(123, Char)
  SETST3(126, Short) SETST3(129, Int) SETST3(132, Long) SETST3(135, Float) SETST3(138, Double)
  SETST3(141, Void)
  t[144] = GetStaticFieldID;
  t[145] = GetStaticObjectField; t[146] = GetStaticBooleanField; t[147] = GetStaticByteField;
  t[148] = GetStaticCharField; t[149] = GetStaticShortField; t[150] = GetStaticIntField;
  t[151] = GetStaticLongField; t[152] = GetStaticFloatField; t[153] = GetStaticDoubleField;
  t[154] = SetStaticObjectField; t[155] = SetStaticBooleanField; t[156] = SetStaticByteField;
  t[157] = SetStaticCharField; t[158] = SetStaticShortField; t[159] = SetStaticIntField;
  t[160] = SetStaticLongField; t[161] = SetStaticFloatField; t[162] = SetStaticDoubleField;
  t[163] = NewString; t[164] = GetStringLength; t[165] = GetStringChars;
  t[166] = ReleaseStringChars; t[167] = NewStringUTF; t[168] = GetStringUTFLength;
  t[169] = GetStringUTFChars; t[170] = ReleaseStringUTFChars; t[171] = GetArrayLength;
  t[172] = NewObjectArray; t[173] = GetObjectArrayElement; t[174] = SetObjectArrayElement;
  t[175] = NewBooleanArray; t[176] = NewByteArray; t[177] = NewCharArray;
  t[178] = NewShortArray; t[179] = NewIntArray; t[180] = NewLongArray; t[181] = NewFloatArray;
  t[182] = NewDoubleArray;
  t[183] = GetBooleanArrayElements; t[184] = GetByteArrayElements; t[185] = GetCharArrayElements;
  t[186] = GetShortArrayElements; t[187] = GetIntArrayElements; t[188] = GetLongArrayElements;
  t[189] = GetFloatArrayElements; t[190] = GetDoubleArrayElements;
  t[191] = ReleaseBooleanArrayElements; t[192] = ReleaseByteArrayElements;
  t[193] = ReleaseCharArrayElements; t[194] = ReleaseShortArrayElements;
  t[195] = ReleaseIntArrayElements; t[196] = ReleaseLongArrayElements;
  t[197] = ReleaseFloatArrayElements; t[198] = ReleaseDoubleArrayElements;
  t[199] = GetBooleanArrayRegion; t[200] = GetByteArrayRegion; t[201] = GetCharArrayRegion;
  t[202] = GetShortArrayRegion; t[203] = GetIntArrayRegion; t[204] = GetLongArrayRegion;
  t[205] = GetFloatArrayRegion; t[206] = GetDoubleArrayRegion;
  t[207] = SetBooleanArrayRegion; t[208] = SetByteArrayRegion; t[209] = SetCharArrayRegion;
  t[210] = SetShortArrayRegion; t[211] = SetIntArrayRegion; t[212] = SetLongArrayRegion;
  t[213] = SetFloatArrayRegion; t[214] = SetDoubleArrayRegion;
  t[215] = RegisterNatives; t[216] = UnregisterNatives; t[217] = MonitorEnter;
  t[218] = MonitorExit; t[219] = GetJavaVM; t[220] = GetStringRegion;
  t[221] = GetStringUTFRegion; t[222] = GetPrimitiveArrayCritical;
  t[223] = ReleasePrimitiveArrayCritical; t[224] = GetStringCritical;
  t[225] = ReleaseStringCritical; t[226] = NewWeakGlobalRef; t[227] = DeleteWeakGlobalRef;
  t[228] = ExceptionCheck; t[229] = NewDirectByteBuffer; t[230] = GetDirectBufferAddress;
  t[231] = GetDirectBufferCapacity; t[232] = GetObjectRefType; t[233] = GetModule;
  // slots 0-3 are reserved (NULL on Android)
  t[0] = t[1] = t[2] = t[3] = NULL;

  s_vm_table[3] = vm_destroy;
  s_vm_table[4] = vm_attach;
  s_vm_table[5] = vm_detach;
  s_vm_table[6] = vm_get_env;
  s_vm_table[7] = vm_attach; // AttachCurrentThreadAsDaemon

  java_lang_init();
  java_android_init();
  java_unity_init();
  java_sdt_init();
}

// --- objects the handlers make (exported for java_*.c) ------------------------------------------

// A reflected method for ReflectionHelper.getMethodID & co.
JObj *jni_reflect_method(JClass *c, const char *name, const char *sig, int is_static) {
  return refmethod_new(method_get(c, name, sig, is_static));
}

JObj *jni_reflect_field(JClass *c, const char *name, const char *sig, int is_static) {
  JObj *o = obj_new(jni_class("java/lang/reflect/Field"), JK_REFFIELD);
  o->native = field_get(c, name, sig, is_static);
  return o;
}

JObj *jni_new_proxy(int64_t ptr, JClass *iface, int bridge) {
  JObj *o = obj_new(iface ? iface : jni_class("java/lang/reflect/Proxy"), JK_PROXY);
  JProxy *p = calloc(1, sizeof(JProxy));
  p->ptr = ptr;
  p->iface = iface;
  p->bridge = bridge;
  o->native = p;
  return o;
}

JObj *jni_new_throwable(const char *cls, const char *msg) {
  return throwable_new(jni_class(cls), msg);
}

const char *jni_throwable_msg(JObj *o) {
  return valid(o) && o->kind == JK_THROWABLE ? (const char *)o->native : NULL;
}

// the name/sig of a reflected method or field object
const char *jni_reflect_name(JObj *o) {
  if (!valid(o))
    return NULL;
  if (o->kind == JK_REFMETHOD)
    return ((JMethod *)o->native)->name;
  if (o->kind == JK_REFFIELD)
    return ((JField *)o->native)->name;
  return NULL;
}

const char *jni_reflect_sig(JObj *o) {
  if (!valid(o))
    return NULL;
  if (o->kind == JK_REFMETHOD)
    return ((JMethod *)o->native)->sig;
  if (o->kind == JK_REFFIELD)
    return ((JField *)o->native)->sig;
  return NULL;
}

int jni_valid(const void *o) {
  return valid(o);
}
