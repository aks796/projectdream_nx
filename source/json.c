/* json.c -- a small JSON reader and writer (see json.h)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json.h"

// --- reading ----------------------------------------------------------------------------

typedef struct {
  const char *p;
  int depth;
} Parser;

static void skip_ws(Parser *ps) {
  while (*ps->p == ' ' || *ps->p == '\t' || *ps->p == '\n' || *ps->p == '\r')
    ps->p++;
}

static Json *node(JsonType t) {
  Json *j = calloc(1, sizeof(*j));
  j->type = t;
  return j;
}

static void push(Json *j, Json *item, char *key) {
  if ((j->n & (j->n - 1)) == 0) { // grow at 0, 1, 2, 4, 8, ...
    const int cap = j->n ? j->n * 2 : 1;
    j->items = realloc(j->items, (size_t)cap * sizeof(*j->items));
    if (j->type == JSON_OBJ)
      j->keys = realloc(j->keys, (size_t)cap * sizeof(*j->keys));
  }
  j->items[j->n] = item;
  if (j->type == JSON_OBJ)
    j->keys[j->n] = key;
  j->n++;
}

static int hex4(const char *p, unsigned *out) {
  unsigned v = 0;
  for (int i = 0; i < 4; i++) {
    const char c = p[i];
    v <<= 4;
    if (c >= '0' && c <= '9') v |= (unsigned)(c - '0');
    else if (c >= 'a' && c <= 'f') v |= (unsigned)(c - 'a' + 10);
    else if (c >= 'A' && c <= 'F') v |= (unsigned)(c - 'A' + 10);
    else return 0;
  }
  *out = v;
  return 1;
}

static void put_utf8(char **o, unsigned cp) {
  char *d = *o;
  if (cp < 0x80) {
    *d++ = (char)cp;
  } else if (cp < 0x800) {
    *d++ = (char)(0xc0 | (cp >> 6));
    *d++ = (char)(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    *d++ = (char)(0xe0 | (cp >> 12));
    *d++ = (char)(0x80 | ((cp >> 6) & 0x3f));
    *d++ = (char)(0x80 | (cp & 0x3f));
  } else {
    *d++ = (char)(0xf0 | (cp >> 18));
    *d++ = (char)(0x80 | ((cp >> 12) & 0x3f));
    *d++ = (char)(0x80 | ((cp >> 6) & 0x3f));
    *d++ = (char)(0x80 | (cp & 0x3f));
  }
  *o = d;
}

// a string at ps->p (on the opening quote), decoded; NULL when malformed
static char *parse_string(Parser *ps) {
  const char *p = ps->p + 1;
  size_t n = 0;
  while (p[n] && p[n] != '"')
    n += p[n] == '\\' && p[n + 1] ? 2 : 1;
  if (p[n] != '"')
    return NULL;
  char *out = malloc(n + 1), *o = out; // decoding never grows the text
  const char *end = p + n;
  while (p < end) {
    if (*p != '\\') {
      *o++ = *p++;
      continue;
    }
    p++;
    switch (*p) {
      case 'b': *o++ = '\b'; p++; break;
      case 'f': *o++ = '\f'; p++; break;
      case 'n': *o++ = '\n'; p++; break;
      case 'r': *o++ = '\r'; p++; break;
      case 't': *o++ = '\t'; p++; break;
      case 'u': {
        unsigned cp, lo;
        if (end - p < 5 || !hex4(p + 1, &cp)) {
          free(out);
          return NULL;
        }
        p += 5;
        if (cp >= 0xd800 && cp < 0xdc00 && end - p >= 6 && p[0] == '\\' && p[1] == 'u' &&
            hex4(p + 2, &lo) && lo >= 0xdc00 && lo < 0xe000) {
          cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          p += 6;
        }
        put_utf8(&o, cp);
        break;
      }
      default: *o++ = *p++; break; // \" \\ \/
    }
  }
  *o = '\0';
  ps->p = end + 1;
  return out;
}

static Json *parse_value(Parser *ps) {
  skip_ws(ps);
  const char c = *ps->p;
  if (c == '{' || c == '[') {
    if (++ps->depth > 64)
      return NULL;
    Json *j = node(c == '{' ? JSON_OBJ : JSON_ARR);
    const char close = c == '{' ? '}' : ']';
    ps->p++;
    skip_ws(ps);
    if (*ps->p == close) {
      ps->p++;
      ps->depth--;
      return j;
    }
    for (;;) {
      char *key = NULL;
      if (j->type == JSON_OBJ) {
        skip_ws(ps);
        if (*ps->p != '"' || !(key = parse_string(ps)))
          goto bad;
        skip_ws(ps);
        if (*ps->p++ != ':') {
          free(key);
          goto bad;
        }
      }
      Json *v = parse_value(ps);
      if (!v) {
        free(key);
        goto bad;
      }
      push(j, v, key);
      skip_ws(ps);
      if (*ps->p == ',') {
        ps->p++;
        continue;
      }
      if (*ps->p++ == close)
        break;
      goto bad;
    }
    ps->depth--;
    return j;
  bad:
    json_free(j);
    return NULL;
  }
  if (c == '"') {
    char *s = parse_string(ps);
    if (!s)
      return NULL;
    Json *j = node(JSON_STR);
    j->str = s;
    return j;
  }
  if (!strncmp(ps->p, "true", 4)) {
    ps->p += 4;
    return node(JSON_TRUE);
  }
  if (!strncmp(ps->p, "false", 5)) {
    ps->p += 5;
    return node(JSON_FALSE);
  }
  if (!strncmp(ps->p, "null", 4)) {
    ps->p += 4;
    return node(JSON_NULL);
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    char *end;
    const double v = strtod(ps->p, &end);
    if (end == ps->p)
      return NULL;
    Json *j = node(JSON_NUM);
    j->num = v;
    j->str = strndup(ps->p, (size_t)(end - ps->p));
    ps->p = end;
    return j;
  }
  return NULL;
}

Json *json_parse(const char *text) {
  if (!text)
    return NULL;
  Parser ps = { text, 0 };
  Json *j = parse_value(&ps);
  skip_ws(&ps);
  if (j && *ps.p) {
    json_free(j);
    return NULL;
  }
  return j;
}

void json_free(Json *j) {
  if (!j)
    return;
  for (int i = 0; i < j->n; i++) {
    json_free(j->items[i]);
    if (j->keys)
      free(j->keys[i]);
  }
  free(j->items);
  free(j->keys);
  free(j->str);
  free(j);
}

Json *json_get(const Json *obj, const char *key) {
  if (!obj || obj->type != JSON_OBJ)
    return NULL;
  for (int i = 0; i < obj->n; i++)
    if (!strcmp(obj->keys[i], key))
      return obj->items[i];
  return NULL;
}

Json *json_at(const Json *arr, int i) {
  return arr && arr->type == JSON_ARR && i >= 0 && i < arr->n ? arr->items[i] : NULL;
}

const char *json_str(const Json *j, const char *def) {
  return j && j->type == JSON_STR ? j->str : def;
}

int64_t json_int(const Json *j, int64_t def) {
  return j && j->type == JSON_NUM ? (int64_t)strtoll(j->str, NULL, 10) : def;
}

int json_bool(const Json *j, int def) {
  return j && j->type == JSON_TRUE ? 1 : j && j->type == JSON_FALSE ? 0 : def;
}

// --- writing ----------------------------------------------------------------------------

static void jb_reserve(JsonBuf *b, size_t more) {
  if (b->len + more + 1 <= b->cap)
    return;
  size_t cap = b->cap ? b->cap : 256;
  while (b->len + more + 1 > cap)
    cap *= 2;
  b->s = realloc(b->s, cap);
  b->cap = cap;
}

void jb_printf(JsonBuf *b, const char *fmt, ...) {
  va_list va;
  va_start(va, fmt);
  const int n = vsnprintf(NULL, 0, fmt, va);
  va_end(va);
  if (n <= 0)
    return;
  jb_reserve(b, (size_t)n);
  va_start(va, fmt);
  vsnprintf(b->s + b->len, (size_t)n + 1, fmt, va);
  va_end(va);
  b->len += (size_t)n;
}

void jb_str(JsonBuf *b, const char *s) {
  if (!s) {
    jb_printf(b, "null");
    return;
  }
  jb_reserve(b, strlen(s) * 6 + 2);
  char *o = b->s + b->len;
  *o++ = '"';
  for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
    if (*p == '"' || *p == '\\') {
      *o++ = '\\';
      *o++ = (char)*p;
    } else if (*p < 0x20) {
      o += sprintf(o, "\\u%04x", *p);
    } else {
      *o++ = (char)*p;
    }
  }
  *o++ = '"';
  *o = '\0';
  b->len = (size_t)(o - b->s);
}

void jb_json(JsonBuf *b, const Json *j) {
  if (!j) {
    jb_printf(b, "null");
    return;
  }
  switch (j->type) {
    case JSON_NULL: jb_printf(b, "null"); break;
    case JSON_FALSE: jb_printf(b, "false"); break;
    case JSON_TRUE: jb_printf(b, "true"); break;
    case JSON_NUM: jb_printf(b, "%s", j->str); break;
    case JSON_STR: jb_str(b, j->str); break;
    case JSON_ARR:
    case JSON_OBJ:
      jb_printf(b, j->type == JSON_ARR ? "[" : "{");
      for (int i = 0; i < j->n; i++) {
        if (i)
          jb_printf(b, ",");
        if (j->type == JSON_OBJ) {
          jb_str(b, j->keys[i]);
          jb_printf(b, ":");
        }
        jb_json(b, j->items[i]);
      }
      jb_printf(b, j->type == JSON_ARR ? "]" : "}");
      break;
  }
}

void jb_free(JsonBuf *b) {
  free(b->s);
  b->s = NULL;
  b->len = b->cap = 0;
}
