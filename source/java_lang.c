/* java_lang.c -- java.lang and java.util as far as the game's code uses them
 * through JNI: Object, Class, String, the boxed primitives, Throwable,
 * reflection objects, System/Thread/Runtime, and the collections (ArrayList,
 * HashMap, their iterators and entries).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <switch.h>

#include "jni.h"
#include "libc_shim.h"
#include "log.h"
#include "paths.h"
#include "so_util.h"
#include "srv.h"

// --- java.lang.Object ----------------------------------------------------------------

static uint32_t str_hash(const char *s) {
  // String.hashCode over UTF-16 units; ASCII is the common case
  uint32_t h = 0;
  for (; *s; s++)
    h = 31 * h + (uint8_t)*s;
  return h;
}

JH(obj_toString) {
  JUNUSED;
  if (!thiz)
    return jv_l(jni_str("null"));
  if (jni_is_string(thiz))
    return jv_l(thiz);
  jvalue v;
  char t;
  if (jni_unbox(thiz, &v, &t)) {
    switch (t) {
      case 'Z': return jv_l(jni_str(v.i ? "true" : "false"));
      case 'J': return jv_l(jni_strf("%lld", (long long)v.j));
      case 'F': return jv_l(jni_strf("%g", (double)v.f));
      case 'D': return jv_l(jni_strf("%g", v.d));
      case 'C': return jv_l(jni_strf("%c", (char)v.i));
      default: return jv_l(jni_strf("%d", v.i));
    }
  }
  const char *msg = jni_throwable_msg(thiz);
  if (msg) {
    char cn[256];
    snprintf(cn, sizeof cn, "%s", jni_class_name(thiz));
    for (char *p = cn; *p; p++)
      if (*p == '/') *p = '.';
    return jv_l(msg[0] ? jni_strf("%s: %s", cn, msg) : jni_str(cn));
  }
  if (thiz->kind == JK_CLASS) {
    char cn[256];
    snprintf(cn, sizeof cn, "%s", ((JClass *)thiz)->name);
    for (char *p = cn; *p; p++)
      if (*p == '/') *p = '.';
    return jv_l(jni_strf("class %s", cn));
  }
  return jv_l(jni_strf("%s@%x", jni_class_name(thiz), (unsigned)((uintptr_t)thiz >> 4)));
}

JH(obj_hashCode) {
  JUNUSED;
  if (jni_is_string(thiz))
    return jv_i((int32_t)str_hash(jni_utf(thiz)));
  jvalue v;
  if (jni_unbox(thiz, &v, NULL))
    return jv_i((int32_t)(v.j ^ (v.j >> 32)));
  return jv_i((int32_t)((uintptr_t)thiz >> 4));
}

JH(obj_equals) {
  JUNUSED;
  JObj *o = a[0].l;
  if (thiz == o)
    return jv_z(1);
  if (jni_is_string(thiz) && jni_is_string(o))
    return jv_z(!strcmp(jni_utf(thiz), jni_utf(o)));
  jvalue x, y;
  char tx, ty;
  if (jni_unbox(thiz, &x, &tx) && jni_unbox(o, &y, &ty))
    return jv_z(tx == ty && x.j == y.j);
  return jv_z(0);
}

JH(obj_getClass) {
  JUNUSED;
  return jv_l(thiz ? thiz->cls : NULL);
}

JH(ret_void) { JUNUSED; return jv_v(); }

// --- java.lang.Class -------------------------------------------------------------------

static JClass *as_class(JObj *o) {
  return o && jni_valid(o) && o->kind == JK_CLASS ? (JClass *)o : NULL;
}

JH(class_getName) {
  JUNUSED;
  JClass *c = as_class(thiz);
  if (!c)
    return jv_l(NULL);
  char cn[256];
  snprintf(cn, sizeof cn, "%s", c->name);
  for (char *p = cn; *p; p++)
    if (*p == '/') *p = '.';
  return jv_l(jni_str(cn));
}

JH(class_getSimpleName) {
  JUNUSED;
  JClass *c = as_class(thiz);
  if (!c)
    return jv_l(NULL);
  const char *s = strrchr(c->name, '/');
  s = s ? s + 1 : c->name;
  const char *d = strrchr(s, '$');
  return jv_l(jni_str(d ? d + 1 : s));
}

JH(class_forName) {
  JUNUSED;
  const char *n = jni_utf(a[0].l);
  return jv_l(n ? jni_class(n) : NULL);
}

JH(class_isArray) {
  JUNUSED;
  JClass *c = as_class(thiz);
  return jv_z(c && c->name[0] == '[');
}

JH(class_getSuperclass) {
  JUNUSED;
  JClass *c = as_class(thiz);
  return jv_l(c ? c->super : NULL);
}

JH(class_isInstance) {
  JUNUSED;
  JClass *c = as_class(thiz);
  return jv_z(c && a[0].l && jni_instance_of(a[0].l, c->name));
}

JH(class_isAssignableFrom) {
  JUNUSED;
  JClass *c = as_class(thiz), *o = as_class(a[0].l);
  if (!c || !o)
    return jv_z(0);
  for (JClass *k = o; k; k = k->super)
    if (k == c)
      return jv_z(1);
  return jv_z(!strcmp(c->name, "java/lang/Object"));
}

JH(class_getClassLoader) {
  JUNUSED;
  static JObj *loader;
  if (!loader)
    loader = jni_new("dalvik/system/PathClassLoader");
  return jv_l(loader);
}

JH(loader_loadClass) {
  JUNUSED;
  const char *n = jni_utf(a[0].l);
  return jv_l(n ? jni_class(n) : NULL);
}

// --- java.lang.String --------------------------------------------------------------------

JH(str_init) {
  JUNUSED;
  // new String(byte[]) / (byte[], charset) / (char[]) / (String)
  if (m->nargs >= 1 && a[0].l) {
    JArr *ar = jni_arr(a[0].l);
    if (ar && ar->type == 'B') {
      int off = 0, len = ar->len;
      if (m->nargs >= 3 && m->argt[1] == 'I' && m->argt[2] == 'I') {
        off = a[1].i;
        len = a[2].i;
      }
      char *tmp = malloc((size_t)len + 1);
      memcpy(tmp, (char *)ar->data + off, (size_t)len);
      tmp[len] = '\0';
      JObj *s = jni_str(tmp);
      free(tmp);
      return jv_l(s);
    }
    if (ar && ar->type == 'C') {
      char *tmp = malloc((size_t)ar->len * 3 + 1);
      int k = 0;
      for (int i = 0; i < ar->len; i++) {
        const uint16_t c = ((uint16_t *)ar->data)[i];
        if (c < 0x80) tmp[k++] = (char)c;
        else if (c < 0x800) { tmp[k++] = (char)(0xc0 | (c >> 6)); tmp[k++] = (char)(0x80 | (c & 0x3f)); }
        else { tmp[k++] = (char)(0xe0 | (c >> 12)); tmp[k++] = (char)(0x80 | ((c >> 6) & 0x3f)); tmp[k++] = (char)(0x80 | (c & 0x3f)); }
      }
      tmp[k] = '\0';
      JObj *s = jni_str(tmp);
      free(tmp);
      return jv_l(s);
    }
    if (jni_is_string(a[0].l))
      return jv_l(jni_str(jni_utf(a[0].l)));
  }
  return jv_l(jni_str(""));
}

static const char *S(JObj *o) {
  const char *s = jni_utf(o);
  return s ? s : "";
}

JH(str_length) { JUNUSED; return jv_i((int32_t)strlen(S(thiz))); }
JH(str_isEmpty) { JUNUSED; return jv_z(S(thiz)[0] == '\0'); }
JH(str_getBytes) {
  JUNUSED;
  const char *s = S(thiz);
  return jv_l(jni_byte_array(s, (int)strlen(s)));
}
JH(str_equalsIgnoreCase) {
  JUNUSED;
  return jv_z(a[0].l && !strcasecmp(S(thiz), S(a[0].l)));
}
JH(str_startsWith) {
  JUNUSED;
  const char *p = S(a[0].l);
  return jv_z(!strncmp(S(thiz), p, strlen(p)));
}
JH(str_endsWith) {
  JUNUSED;
  const char *s = S(thiz), *p = S(a[0].l);
  const size_t n = strlen(s), k = strlen(p);
  return jv_z(n >= k && !strcmp(s + n - k, p));
}
JH(str_contains) { JUNUSED; return jv_z(strstr(S(thiz), S(a[0].l)) != NULL); }
JH(str_indexOf) {
  JUNUSED;
  const char *s = S(thiz);
  if (m->argt[0] == 'I') {
    const char *p = strchr(s, a[0].i);
    return jv_i(p ? (int32_t)(p - s) : -1);
  }
  const char *p = strstr(s, S(a[0].l));
  return jv_i(p ? (int32_t)(p - s) : -1);
}
JH(str_substring) {
  JUNUSED;
  const char *s = S(thiz);
  const int n = (int)strlen(s);
  int b = a[0].i, e = m->nargs > 1 ? a[1].i : n;
  if (b < 0) b = 0;
  if (e > n) e = n;
  if (e < b) e = b;
  char *t = malloc((size_t)(e - b) + 1);
  memcpy(t, s + b, (size_t)(e - b));
  t[e - b] = '\0';
  JObj *r = jni_str(t);
  free(t);
  return jv_l(r);
}
JH(str_toLowerCase) {
  JUNUSED;
  char *t = strdup(S(thiz));
  for (char *p = t; *p; p++) *p = (char)tolower((unsigned char)*p);
  JObj *r = jni_str(t);
  free(t);
  return jv_l(r);
}
JH(str_toUpperCase) {
  JUNUSED;
  char *t = strdup(S(thiz));
  for (char *p = t; *p; p++) *p = (char)toupper((unsigned char)*p);
  JObj *r = jni_str(t);
  free(t);
  return jv_l(r);
}
JH(str_trim) {
  JUNUSED;
  const char *s = S(thiz);
  while (*s && isspace((unsigned char)*s)) s++;
  size_t n = strlen(s);
  while (n && isspace((unsigned char)s[n - 1])) n--;
  char *t = strndup(s, n);
  JObj *r = jni_str(t);
  free(t);
  return jv_l(r);
}
JH(str_valueOf) {
  JUNUSED;
  switch (m->argt[0]) {
    case 'I': case 'S': case 'B': return jv_l(jni_strf("%d", a[0].i));
    case 'J': return jv_l(jni_strf("%lld", (long long)a[0].j));
    case 'Z': return jv_l(jni_str(a[0].i ? "true" : "false"));
    case 'F': return jv_l(jni_strf("%g", (double)a[0].f));
    case 'D': return jv_l(jni_strf("%g", a[0].d));
    case 'C': return jv_l(jni_strf("%c", (char)a[0].i));
    default: return obj_toString(a[0].l, m, a);
  }
}
JH(str_compareTo) { JUNUSED; return jv_i(strcmp(S(thiz), S(a[0].l))); }
JH(str_replace) {
  JUNUSED;
  const char *s = S(thiz);
  if (m->argt[0] == 'C') {
    char *t = strdup(s);
    for (char *p = t; *p; p++)
      if (*p == (char)a[0].i) *p = (char)a[1].i;
    JObj *r = jni_str(t);
    free(t);
    return jv_l(r);
  }
  const char *from = S(a[0].l), *to = S(a[1].l);
  const size_t fl = strlen(from), tl = strlen(to);
  if (!fl)
    return jv_l(thiz);
  size_t cap = strlen(s) * (tl > fl ? tl / fl + 2 : 1) + 1;
  char *t = malloc(cap), *o = t;
  for (const char *p = s; *p;) {
    if (!strncmp(p, from, fl)) {
      memcpy(o, to, tl);
      o += tl;
      p += fl;
    } else {
      *o++ = *p++;
    }
  }
  *o = '\0';
  JObj *r = jni_str(t);
  free(t);
  return jv_l(r);
}
JH(str_charAt) {
  JUNUSED;
  const char *s = S(thiz);
  return jv_i(a[0].i >= 0 && a[0].i < (int)strlen(s) ? (uint8_t)s[a[0].i] : 0);
}
JH(str_split) {
  JUNUSED;
  // a literal (regex-free) split: enough for the separators the game uses
  const char *s = S(thiz), *sep = S(a[0].l);
  const size_t sl = strlen(sep);
  JObj *list = jni_list_new();
  if (!sl) {
    jni_list_add(list, jni_str(s));
  } else {
    const char *p = s;
    for (;;) {
      const char *q = strstr(p, sep);
      if (!q) {
        jni_list_add(list, jni_str(p));
        break;
      }
      char *t = strndup(p, (size_t)(q - p));
      jni_list_add(list, jni_str(t));
      free(t);
      p = q + sl;
    }
  }
  const int n = jni_list_size(list);
  JObj *arr = jni_array('L', n, "java/lang/String");
  for (int i = 0; i < n; i++)
    ((JObj **)jni_arr(arr)->data)[i] = jni_list_get(list, i);
  return jv_l(arr);
}

// --- boxed primitives --------------------------------------------------------------------

JH(box_valueOf) {
  JUNUSED;
  // Integer.valueOf(int), Long.valueOf(long), ... or valueOf(String)
  const char *cn = jni_class_name(thiz->kind == JK_CLASS ? thiz : NULL);
  if (thiz->kind == JK_CLASS)
    cn = ((JClass *)thiz)->name;
  char t = 'I';
  if (strstr(cn, "Long")) t = 'J';
  else if (strstr(cn, "Float")) t = 'F';
  else if (strstr(cn, "Double")) t = 'D';
  else if (strstr(cn, "Boolean")) t = 'Z';
  else if (strstr(cn, "Short")) t = 'S';
  else if (strstr(cn, "Byte")) t = 'B';
  else if (strstr(cn, "Character")) t = 'C';
  jvalue v = a[0];
  if (m->argt[0] == 'L') {
    const char *s = S(a[0].l);
    v.j = 0;
    switch (t) {
      case 'J': v.j = strtoll(s, NULL, 10); break;
      case 'F': v.f = strtof(s, NULL); break;
      case 'D': v.d = strtod(s, NULL); break;
      case 'Z': v.i = !strcasecmp(s, "true"); break;
      default: v.i = (int32_t)strtol(s, NULL, 10); break;
    }
  }
  return jv_l(jni_box(t, v));
}

JH(box_parse) {
  JUNUSED;
  const char *s = S(a[0].l);
  switch (m->ret) {
    case 'J': return jv_j(strtoll(s, NULL, m->nargs > 1 ? a[1].i : 10));
    case 'F': return jv_f(strtof(s, NULL));
    case 'D': return jv_d(strtod(s, NULL));
    case 'Z': return jv_z(!strcasecmp(s, "true"));
    default: return jv_i((int32_t)strtol(s, NULL, m->nargs > 1 ? a[1].i : 10));
  }
}

JH(box_value) {
  JUNUSED;
  // intValue/longValue/floatValue/doubleValue/booleanValue/... with conversion
  jvalue v;
  char t;
  if (!jni_unbox(thiz, &v, &t))
    return jv_v();
  double d;
  switch (t) {
    case 'J': d = (double)v.j; break;
    case 'F': d = v.f; break;
    case 'D': d = v.d; break;
    default: d = v.i; break;
  }
  switch (m->ret) {
    case 'J': return jv_j(t == 'J' ? v.j : (int64_t)d);
    case 'F': return jv_f((float)d);
    case 'D': return jv_d(d);
    case 'Z': return jv_z(v.i != 0);
    default: return jv_i(t == 'J' ? (int32_t)v.j : (int32_t)d);
  }
}

// --- Throwable ----------------------------------------------------------------------------

JH(thr_init) {
  JUNUSED;
  // new Exception(String): the object made by NewObject keeps the message
  const char *msg = m->nargs >= 1 ? jni_utf(a[0].l) : NULL;
  return jv_l(jni_new_throwable(jni_class_name(thiz), msg ? msg : ""));
}
JH(thr_getMessage) {
  JUNUSED;
  const char *msg = jni_throwable_msg(thiz);
  return jv_l(msg ? jni_str(msg) : NULL);
}
JH(thr_getStackTrace) {
  JUNUSED;
  return jv_l(jni_array('L', 0, "java/lang/StackTraceElement"));
}
JH(log_getStackTraceString) {
  JUNUSED;
  const char *msg = jni_throwable_msg(a[0].l);
  return jv_l(jni_strf("%s: %s", jni_class_name(a[0].l), msg ? msg : ""));
}

// --- reflection objects ---------------------------------------------------------------------

JH(refl_getName) {
  JUNUSED;
  const char *n = jni_reflect_name(thiz);
  return jv_l(n ? jni_str(n) : NULL);
}

// --- System / Thread / Runtime ------------------------------------------------------------

JH(sys_currentTimeMillis) {
  JUNUSED;
  struct timeval tv;
  gettimeofday(&tv, NULL);
  return jv_j((int64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000);
}
// System.load(path) / System.loadLibrary(name): load the library and run its
// JNI_OnLoad, once, as Java does. Unity loads each P/Invoke plugin this way
// (ClassLoader.findLibrary, then System.load) before it binds to it, and the
// plugins keep the JavaVM their JNI_OnLoad is given (Nabe's initialize
// reads it back).
JH(sys_load) {
  JUNUSED;
  const char *arg = S(a[0].l);
  char path[600];
  if (!strcmp(m->name, "loadLibrary"))
    snprintf(path, sizeof path, "%s/lib%s.so", path_lib(), arg);
  else
    snprintf(path, sizeof path, "%s", arg);
  so_module *mod = so_dlopen(path, 0);
  if (!mod) {
    log_printf("[java] System.%s(%s) failed: %s", m->name, arg, so_dlerror());
    jni_throw("java/lang/UnsatisfiedLinkError", path);
    return jv_v();
  }
  if (mod == so_dlopen(NULL, 0) || __atomic_exchange_n(&mod->jni_loaded, 1, __ATOMIC_ACQ_REL))
    return jv_v(); // a system library, or JNI_OnLoad already ran
  int32_t (*onload)(void *vm, void *reserved) = (void *)so_sym(mod, "JNI_OnLoad");
  if (!onload)
    return jv_v();
  const int32_t v = onload(jni_vm(), NULL);
  log_printf("[java] System.%s: %s JNI_OnLoad -> 0x%x", m->name, mod->soname, v);
  if (v < 0)
    jni_throw("java/lang/UnsatisfiedLinkError", mod->soname);
  return jv_v();
}
JH(sys_nanoTime) {
  JUNUSED;
  return jv_j(android_monotonic_ns());
}
JH(sys_getProperty) {
  JUNUSED;
  const char *k = S(a[0].l);
  if (!strcmp(k, "http.agent"))
    return jv_l(jni_str("Dalvik/2.1.0 (Linux; U; Android 13; Switch Build/TQ3A)"));
  if (!strcmp(k, "line.separator"))
    return jv_l(jni_str("\n"));
  if (!strcmp(k, "os.arch"))
    return jv_l(jni_str("aarch64"));
  if (!strcmp(k, "java.vm.version"))
    return jv_l(jni_str("2.1.0"));
  if (!strcmp(k, "file.encoding"))
    return jv_l(jni_str("UTF-8"));
  return jv_l(m->nargs > 1 ? a[1].l : NULL);
}
JH(sys_identityHashCode) { JUNUSED; return jv_i((int32_t)((uintptr_t)a[0].l >> 4)); }
JH(sys_arraycopy) {
  JUNUSED;
  JArr *s = jni_arr(a[0].l), *d = jni_arr(a[2].l);
  if (s && d && s->esize == d->esize && a[1].i >= 0 && a[3].i >= 0 && a[4].i >= 0 &&
      a[1].i + a[4].i <= s->len && a[3].i + a[4].i <= d->len)
    memmove((char *)d->data + (size_t)a[3].i * d->esize, (char *)s->data + (size_t)a[1].i * s->esize,
            (size_t)a[4].i * s->esize);
  return jv_v();
}
JH(thread_currentThread) {
  JUNUSED;
  static JObj *t;
  if (!t)
    t = jni_new("java/lang/Thread");
  return jv_l(t);
}
JH(thread_getName) { JUNUSED; return jv_l(jni_str("main")); }
JH(thread_getId) { JUNUSED; return jv_j(1); }
JH(runtime_getRuntime) {
  JUNUSED;
  static JObj *r;
  if (!r)
    r = jni_new("java/lang/Runtime");
  return jv_l(r);
}
JH(runtime_maxMemory) { JUNUSED; return jv_j(512ll << 20); }
JH(runtime_totalMemory) { JUNUSED; return jv_j(256ll << 20); }
JH(runtime_freeMemory) { JUNUSED; return jv_j(128ll << 20); }
JH(runtime_availableProcessors) { JUNUSED; return jv_i(3); }

// --- javax.net.ssl: the system's trusted certificates ---------------------------------
// Unity's TLS (unitytls) builds its root store from
// TrustManagerFactory.getInstance(getDefaultAlgorithm()).init(null)
// .getTrustManagers()[0].getAcceptedIssuers(), each one's getEncoded(). The
// system here trusts one CA: the offline server's (srv.h).

JH(tmf_getDefaultAlgorithm) { JUNUSED; return jv_l(jni_str("PKIX")); }
JH(tmf_getInstance) {
  JUNUSED;
  static JObj *f;
  if (!f)
    f = jni_new("javax/net/ssl/TrustManagerFactory");
  return jv_l(f);
}
JH(tmf_getTrustManagers) {
  JUNUSED;
  JObj *arr = jni_array('L', 1, "javax/net/ssl/TrustManager");
  ((JObj **)jni_arr(arr)->data)[0] = jni_new("com/android/org/conscrypt/TrustManagerImpl");
  return jv_l(arr);
}
JH(tm_getAcceptedIssuers) {
  JUNUSED;
  size_t n = 0;
  const uint8_t *der = srv_ca_der(&n);
  JObj *arr = jni_array('L', der ? 1 : 0, "java/security/cert/X509Certificate");
  if (der) {
    JObj *cert = jni_new("com/android/org/conscrypt/OpenSSLX509Certificate");
    jni_set_field(cert, "encoded", jv_l(jni_byte_array(der, (int)n)));
    ((JObj **)jni_arr(arr)->data)[0] = cert;
  }
  return jv_l(arr);
}
JH(cert_getEncoded) { JUNUSED; return jni_get_field(thiz, "encoded", NULL); }

// --- collections ------------------------------------------------------------------------------
//
// An ArrayList's payload is a growable array of objects; a HashMap's is two
// parallel arrays (keys, values) searched linearly -- the game's maps are a
// handful of entries. Iterators and entries are small objects pointing back.

typedef struct {
  JObj **items;
  int n, cap;
} JList;

typedef struct {
  JObj **keys, **vals;
  int n, cap;
} JMap;

typedef struct {
  JObj *src;
  int kind;   // 0 list items, 1 map keys, 2 map values, 3 map entries
  int pos;
} JIter;

static int is_list(JObj *o) {
  return o && jni_valid(o) && (jni_instance_of(o, "java/util/ArrayList") ||
                               jni_instance_of(o, "java/util/HashSet") ||
                               jni_instance_of(o, "java/util/List")) && o->native;
}

static int is_map(JObj *o) {
  return o && jni_valid(o) && jni_instance_of(o, "java/util/HashMap") && o->native;
}

JObj *jni_list_new(void) {
  JObj *o = jni_new("java/util/ArrayList");
  o->native = calloc(1, sizeof(JList));
  return o;
}

void jni_list_add(JObj *list, JObj *item) {
  if (!is_list(list))
    return;
  JList *l = list->native;
  if (l->n == l->cap) {
    l->cap = l->cap ? l->cap * 2 : 8;
    l->items = realloc(l->items, (size_t)l->cap * sizeof(JObj *));
  }
  l->items[l->n++] = item;
}

int jni_list_size(JObj *list) {
  return is_list(list) ? ((JList *)list->native)->n : 0;
}

JObj *jni_list_get(JObj *list, int i) {
  if (!is_list(list))
    return NULL;
  JList *l = list->native;
  return i >= 0 && i < l->n ? l->items[i] : NULL;
}

JObj *jni_map_new(void) {
  JObj *o = jni_new("java/util/HashMap");
  o->native = calloc(1, sizeof(JMap));
  return o;
}

static int key_eq(JObj *x, JObj *y) {
  if (x == y)
    return 1;
  if (jni_is_string(x) && jni_is_string(y))
    return !strcmp(jni_utf(x), jni_utf(y));
  jvalue a, b;
  char ta, tb;
  if (jni_unbox(x, &a, &ta) && jni_unbox(y, &b, &tb))
    return ta == tb && a.j == b.j;
  return 0;
}

static int map_index(JMap *mp, JObj *k) {
  for (int i = 0; i < mp->n; i++)
    if (key_eq(mp->keys[i], k))
      return i;
  return -1;
}

void jni_map_put(JObj *map, JObj *key, JObj *value) {
  if (!is_map(map))
    return;
  JMap *mp = map->native;
  const int i = map_index(mp, key);
  if (i >= 0) {
    mp->vals[i] = value;
    return;
  }
  if (mp->n == mp->cap) {
    mp->cap = mp->cap ? mp->cap * 2 : 8;
    mp->keys = realloc(mp->keys, (size_t)mp->cap * sizeof(JObj *));
    mp->vals = realloc(mp->vals, (size_t)mp->cap * sizeof(JObj *));
  }
  mp->keys[mp->n] = key;
  mp->vals[mp->n] = value;
  mp->n++;
}

JObj *jni_map_get_s(JObj *map, const char *key) {
  if (!is_map(map))
    return NULL;
  JMap *mp = map->native;
  for (int i = 0; i < mp->n; i++)
    if (jni_is_string(mp->keys[i]) && !strcmp(jni_utf(mp->keys[i]), key))
      return mp->vals[i];
  return NULL;
}

int jni_map_size(JObj *map) {
  return is_map(map) ? ((JMap *)map->native)->n : 0;
}

JObj *jni_map_key_at(JObj *map, int i) {
  return is_map(map) && i >= 0 && i < ((JMap *)map->native)->n ? ((JMap *)map->native)->keys[i] : NULL;
}

JObj *jni_map_val_at(JObj *map, int i) {
  return is_map(map) && i >= 0 && i < ((JMap *)map->native)->n ? ((JMap *)map->native)->vals[i] : NULL;
}

void jni_map_remove_s(JObj *map, const char *key) {
  if (!is_map(map) || !key)
    return;
  JMap *mp = map->native;
  for (int i = 0; i < mp->n; i++) {
    if (jni_is_string(mp->keys[i]) && !strcmp(jni_utf(mp->keys[i]), key)) {
      memmove(mp->keys + i, mp->keys + i + 1, (size_t)(mp->n - i - 1) * sizeof(JObj *));
      memmove(mp->vals + i, mp->vals + i + 1, (size_t)(mp->n - i - 1) * sizeof(JObj *));
      mp->n--;
      return;
    }
  }
}

JH(coll_init) {
  JUNUSED;
  if (jni_instance_of(thiz, "java/util/HashMap"))
    thiz->native = calloc(1, sizeof(JMap));
  else
    thiz->native = calloc(1, sizeof(JList));
  return jv_v();
}

JH(coll_size) {
  JUNUSED;
  if (is_map(thiz))
    return jv_i(((JMap *)thiz->native)->n);
  return jv_i(jni_list_size(thiz));
}
JH(coll_isEmpty) {
  JUNUSED;
  if (is_map(thiz))
    return jv_z(((JMap *)thiz->native)->n == 0);
  return jv_z(jni_list_size(thiz) == 0);
}
JH(list_add) {
  JUNUSED;
  if (m->nargs == 2) { // add(int, Object): appended (order is rarely relied on)
    jni_list_add(thiz, a[1].l);
    return jv_v();
  }
  if (jni_instance_of(thiz, "java/util/HashSet")) {
    JList *l = thiz->native;
    for (int i = 0; l && i < l->n; i++)
      if (key_eq(l->items[i], a[0].l))
        return jv_z(0);
  }
  jni_list_add(thiz, a[0].l);
  return jv_z(1);
}
JH(list_get) { JUNUSED; return jv_l(jni_list_get(thiz, a[0].i)); }
JH(list_contains) {
  JUNUSED;
  if (is_map(thiz))
    return jv_z(map_index(thiz->native, a[0].l) >= 0);
  JList *l = is_list(thiz) ? thiz->native : NULL;
  for (int i = 0; l && i < l->n; i++)
    if (key_eq(l->items[i], a[0].l))
      return jv_z(1);
  return jv_z(0);
}
JH(coll_clear) {
  JUNUSED;
  if (is_map(thiz))
    ((JMap *)thiz->native)->n = 0;
  else if (is_list(thiz))
    ((JList *)thiz->native)->n = 0;
  return jv_v();
}
JH(coll_toArray) {
  JUNUSED;
  const int n = jni_list_size(thiz);
  JObj *arr = jni_array('L', n, "java/lang/Object");
  for (int i = 0; i < n; i++)
    ((JObj **)jni_arr(arr)->data)[i] = jni_list_get(thiz, i);
  return jv_l(arr);
}
JH(map_put) {
  JUNUSED;
  JObj *old = NULL;
  if (is_map(thiz)) {
    const int i = map_index(thiz->native, a[0].l);
    if (i >= 0)
      old = ((JMap *)thiz->native)->vals[i];
  }
  jni_map_put(thiz, a[0].l, a[1].l);
  return jv_l(old);
}
JH(map_get) {
  JUNUSED;
  if (!is_map(thiz))
    return jv_l(NULL);
  JMap *mp = thiz->native;
  const int i = map_index(mp, a[0].l);
  return jv_l(i >= 0 ? mp->vals[i] : (m->nargs > 1 ? a[1].l : NULL));
}
JH(map_remove) {
  JUNUSED;
  if (!is_map(thiz))
    return jv_l(NULL);
  JMap *mp = thiz->native;
  const int i = map_index(mp, a[0].l);
  if (i < 0)
    return jv_l(NULL);
  JObj *old = mp->vals[i];
  memmove(mp->keys + i, mp->keys + i + 1, (size_t)(mp->n - i - 1) * sizeof(JObj *));
  memmove(mp->vals + i, mp->vals + i + 1, (size_t)(mp->n - i - 1) * sizeof(JObj *));
  mp->n--;
  return jv_l(old);
}

static JObj *iter_new(JObj *src, int kind) {
  JObj *o = jni_new("java/util/Iterator");
  JIter *it = calloc(1, sizeof(JIter));
  it->src = src;
  it->kind = kind;
  o->native = it;
  return o;
}

// keySet()/values()/entrySet() are views: a list object remembering the map
static JObj *view_new(JObj *map, int kind) {
  JObj *o = jni_list_new();
  JMap *mp = is_map(map) ? map->native : NULL;
  for (int i = 0; mp && i < mp->n; i++) {
    if (kind == 1) {
      jni_list_add(o, mp->keys[i]);
    } else if (kind == 2) {
      jni_list_add(o, mp->vals[i]);
    } else {
      JObj *e = jni_new("java/util/Map$Entry");
      jni_set_field(e, "key", jv_l(mp->keys[i]));
      jni_set_field(e, "value", jv_l(mp->vals[i]));
      jni_list_add(o, e);
    }
  }
  return o;
}

JH(map_keySet) { JUNUSED; return jv_l(view_new(thiz, 1)); }
JH(map_values) { JUNUSED; return jv_l(view_new(thiz, 2)); }
JH(map_entrySet) { JUNUSED; return jv_l(view_new(thiz, 3)); }
JH(entry_getKey) { JUNUSED; return jni_get_field(thiz, "key", NULL); }
JH(entry_getValue) { JUNUSED; return jni_get_field(thiz, "value", NULL); }
JH(coll_iterator) { JUNUSED; return jv_l(iter_new(thiz, 0)); }
JH(iter_hasNext) {
  JUNUSED;
  JIter *it = thiz ? thiz->native : NULL;
  return jv_z(it && it->pos < jni_list_size(it->src));
}
JH(iter_next) {
  JUNUSED;
  JIter *it = thiz ? thiz->native : NULL;
  if (!it)
    return jv_l(NULL);
  return jv_l(jni_list_get(it->src, it->pos++));
}

void java_lang_init(void) {
  // the class tree the code checks with IsInstanceOf / getSuperclass
  const char *boxes[] = { "java/lang/Integer", "java/lang/Long", "java/lang/Float", "java/lang/Double",
                          "java/lang/Short", "java/lang/Byte" };
  for (unsigned i = 0; i < sizeof(boxes) / sizeof(*boxes); i++)
    jni_set_super(boxes[i], "java/lang/Number");
  jni_set_super("java/lang/Exception", "java/lang/Throwable");
  jni_set_super("java/lang/Error", "java/lang/Throwable");
  jni_set_super("java/lang/RuntimeException", "java/lang/Exception");
  jni_set_super("java/lang/IllegalStateException", "java/lang/RuntimeException");
  jni_set_super("java/lang/IllegalArgumentException", "java/lang/RuntimeException");
  jni_set_super("java/lang/NullPointerException", "java/lang/RuntimeException");
  jni_set_super("java/lang/UnsupportedOperationException", "java/lang/RuntimeException");
  jni_set_super("java/lang/ClassNotFoundException", "java/lang/Exception");
  jni_set_super("java/lang/NoSuchMethodError", "java/lang/Error");
  jni_set_super("java/lang/NoSuchFieldError", "java/lang/Error");
  jni_set_super("java/io/IOException", "java/lang/Exception");
  jni_set_super("java/io/FileNotFoundException", "java/io/IOException");
  jni_set_super("java/lang/reflect/Method", "java/lang/reflect/AccessibleObject");
  jni_set_super("java/lang/reflect/Constructor", "java/lang/reflect/AccessibleObject");
  jni_set_super("java/lang/reflect/Field", "java/lang/reflect/AccessibleObject");
  jni_set_super("java/util/ArrayList", "java/util/AbstractList");
  jni_add_iface("java/util/ArrayList", "java/util/List");
  jni_add_iface("java/util/List", "java/util/Collection");
  jni_add_iface("java/util/HashSet", "java/util/Set");
  jni_add_iface("java/util/Set", "java/util/Collection");
  jni_add_iface("java/util/HashMap", "java/util/Map");
  jni_set_super("dalvik/system/PathClassLoader", "java/lang/ClassLoader");
  jni_add_iface("java/lang/String", "java/lang/CharSequence");

  jni_method("*", "toString", "()Ljava/lang/String;", obj_toString);
  jni_method("*", "hashCode", "()I", obj_hashCode);
  jni_method("*", "equals", "(Ljava/lang/Object;)Z", obj_equals);
  jni_method("*", "getClass", "()Ljava/lang/Class;", obj_getClass);
  jni_method("*", "notify", "()V", ret_void);
  jni_method("*", "notifyAll", "()V", ret_void);
  jni_method("*", "<init>", "()V", ret_void);

  jni_method("java/lang/Class", "getName", NULL, class_getName);
  jni_method("java/lang/Class", "getCanonicalName", NULL, class_getName);
  jni_method("java/lang/Class", "getSimpleName", NULL, class_getSimpleName);
  jni_method("java/lang/Class", "forName", NULL, class_forName);
  jni_method("java/lang/Class", "isArray", NULL, class_isArray);
  jni_method("java/lang/Class", "getSuperclass", NULL, class_getSuperclass);
  jni_method("java/lang/Class", "isInstance", NULL, class_isInstance);
  jni_method("java/lang/Class", "isAssignableFrom", NULL, class_isAssignableFrom);
  jni_method("java/lang/Class", "getClassLoader", NULL, class_getClassLoader);
  jni_method("java/lang/ClassLoader", "loadClass", NULL, loader_loadClass);
  jni_method("java/lang/ClassLoader", "findClass", NULL, loader_loadClass);

  const char *str = "java/lang/String";
  jni_method(str, "<init>", NULL, str_init);
  jni_method(str, "length", NULL, str_length);
  jni_method(str, "isEmpty", NULL, str_isEmpty);
  jni_method(str, "getBytes", NULL, str_getBytes);
  jni_method(str, "equalsIgnoreCase", NULL, str_equalsIgnoreCase);
  jni_method(str, "startsWith", NULL, str_startsWith);
  jni_method(str, "endsWith", NULL, str_endsWith);
  jni_method(str, "contains", NULL, str_contains);
  jni_method(str, "indexOf", NULL, str_indexOf);
  jni_method(str, "substring", NULL, str_substring);
  jni_method(str, "toLowerCase", NULL, str_toLowerCase);
  jni_method(str, "toUpperCase", NULL, str_toUpperCase);
  jni_method(str, "trim", NULL, str_trim);
  jni_method(str, "valueOf", NULL, str_valueOf);
  jni_method(str, "compareTo", NULL, str_compareTo);
  jni_method(str, "replace", NULL, str_replace);
  jni_method(str, "charAt", NULL, str_charAt);
  jni_method(str, "split", NULL, str_split);

  for (unsigned i = 0; i < sizeof(boxes) / sizeof(*boxes); i++) {
    jni_method(boxes[i], "valueOf", NULL, box_valueOf);
  }
  jni_method("java/lang/Boolean", "valueOf", NULL, box_valueOf);
  jni_method("java/lang/Character", "valueOf", NULL, box_valueOf);
  jni_method("java/lang/Integer", "parseInt", NULL, box_parse);
  jni_method("java/lang/Long", "parseLong", NULL, box_parse);
  jni_method("java/lang/Float", "parseFloat", NULL, box_parse);
  jni_method("java/lang/Double", "parseDouble", NULL, box_parse);
  jni_method("java/lang/Boolean", "parseBoolean", NULL, box_parse);
  const char *values[] = { "intValue", "longValue", "floatValue", "doubleValue", "booleanValue",
                           "shortValue", "byteValue", "charValue" };
  for (unsigned i = 0; i < sizeof(values) / sizeof(*values); i++)
    jni_method("*", values[i], NULL, box_value);

  jni_method("java/lang/Throwable", "<init>", NULL, thr_init);
  jni_method("java/lang/Throwable", "getMessage", NULL, thr_getMessage);
  jni_method("java/lang/Throwable", "getLocalizedMessage", NULL, thr_getMessage);
  jni_method("java/lang/Throwable", "getStackTrace", NULL, thr_getStackTrace);
  jni_method("java/lang/Throwable", "printStackTrace", NULL, ret_void);
  jni_method("android/util/Log", "getStackTraceString", NULL, log_getStackTraceString);
  jni_method("java/lang/reflect/AccessibleObject", "getName", NULL, refl_getName);

  const char *TMF = "javax/net/ssl/TrustManagerFactory";
  jni_set_super("com/android/org/conscrypt/TrustManagerImpl", "javax/net/ssl/X509ExtendedTrustManager");
  jni_set_super("javax/net/ssl/X509ExtendedTrustManager", "javax/net/ssl/X509TrustManager");
  jni_set_super("javax/net/ssl/X509TrustManager", "javax/net/ssl/TrustManager");
  jni_set_super("com/android/org/conscrypt/OpenSSLX509Certificate", "java/security/cert/X509Certificate");
  jni_set_super("java/security/cert/X509Certificate", "java/security/cert/Certificate");
  jni_method(TMF, "getDefaultAlgorithm", NULL, tmf_getDefaultAlgorithm);
  jni_method(TMF, "getInstance", NULL, tmf_getInstance);
  jni_method(TMF, "init", NULL, ret_void);
  jni_method(TMF, "getTrustManagers", NULL, tmf_getTrustManagers);
  jni_method("javax/net/ssl/X509TrustManager", "getAcceptedIssuers", NULL, tm_getAcceptedIssuers);
  jni_method("java/security/cert/Certificate", "getEncoded", NULL, cert_getEncoded);

  jni_method("java/lang/System", "currentTimeMillis", NULL, sys_currentTimeMillis);
  jni_method("java/lang/System", "nanoTime", NULL, sys_nanoTime);
  jni_method("java/lang/System", "getProperty", NULL, sys_getProperty);
  jni_method("java/lang/System", "identityHashCode", NULL, sys_identityHashCode);
  jni_method("java/lang/System", "arraycopy", NULL, sys_arraycopy);
  jni_method("java/lang/System", "gc", NULL, ret_void);
  jni_method("java/lang/System", "loadLibrary", NULL, sys_load);
  jni_method("java/lang/System", "load", NULL, sys_load);
  jni_method("java/lang/Thread", "currentThread", NULL, thread_currentThread);
  jni_method("java/lang/Thread", "getName", NULL, thread_getName);
  jni_method("java/lang/Thread", "getId", NULL, thread_getId);
  jni_method("java/lang/Runtime", "getRuntime", NULL, runtime_getRuntime);
  jni_method("java/lang/Runtime", "maxMemory", NULL, runtime_maxMemory);
  jni_method("java/lang/Runtime", "totalMemory", NULL, runtime_totalMemory);
  jni_method("java/lang/Runtime", "freeMemory", NULL, runtime_freeMemory);
  jni_method("java/lang/Runtime", "availableProcessors", NULL, runtime_availableProcessors);
  jni_method("java/lang/Runtime", "gc", NULL, ret_void);

  const char *colls[] = { "java/util/ArrayList", "java/util/HashMap", "java/util/HashSet",
                          "java/util/LinkedList", "java/util/TreeMap", "java/util/LinkedHashMap" };
  jni_set_super("java/util/LinkedList", "java/util/ArrayList");
  jni_set_super("java/util/TreeMap", "java/util/HashMap");
  jni_set_super("java/util/LinkedHashMap", "java/util/HashMap");
  for (unsigned i = 0; i < sizeof(colls) / sizeof(*colls); i++)
    jni_method(colls[i], "<init>", NULL, coll_init);
  jni_method("java/util/Collection", "size", NULL, coll_size);
  jni_method("java/util/Collection", "isEmpty", NULL, coll_isEmpty);
  jni_method("java/util/Collection", "add", NULL, list_add);
  jni_method("java/util/Collection", "contains", NULL, list_contains);
  jni_method("java/util/Collection", "clear", NULL, coll_clear);
  jni_method("java/util/Collection", "toArray", NULL, coll_toArray);
  jni_method("java/util/Collection", "iterator", NULL, coll_iterator);
  jni_method("java/util/List", "get", NULL, list_get);
  jni_method("java/util/Map", "size", NULL, coll_size);
  jni_method("java/util/Map", "isEmpty", NULL, coll_isEmpty);
  jni_method("java/util/Map", "put", NULL, map_put);
  jni_method("java/util/Map", "get", NULL, map_get);
  jni_method("java/util/Map", "getOrDefault", NULL, map_get);
  jni_method("java/util/Map", "remove", NULL, map_remove);
  jni_method("java/util/Map", "containsKey", NULL, list_contains);
  jni_method("java/util/Map", "clear", NULL, coll_clear);
  jni_method("java/util/Map", "keySet", NULL, map_keySet);
  jni_method("java/util/Map", "values", NULL, map_values);
  jni_method("java/util/Map", "entrySet", NULL, map_entrySet);
  jni_method("java/util/Map$Entry", "getKey", NULL, entry_getKey);
  jni_method("java/util/Map$Entry", "getValue", NULL, entry_getValue);
  jni_method("java/util/Iterator", "hasNext", NULL, iter_hasNext);
  jni_method("java/util/Iterator", "next", NULL, iter_next);
}
