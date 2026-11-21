/* json.h -- a small JSON reader and writer (json.c)
 *
 * The NPF SDK's bridge speaks JSON both ways (UnityBridge.execute in,
 * NativeBridgeCallback out), and the offline server's recorded answers are
 * JSON. Parsing makes a tree; writing appends to a growing buffer.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef MKT_JSON_H
#define MKT_JSON_H

#include <stddef.h>
#include <stdint.h>

typedef enum { JSON_NULL, JSON_FALSE, JSON_TRUE, JSON_NUM, JSON_STR, JSON_ARR, JSON_OBJ } JsonType;

typedef struct Json {
  JsonType type;
  double num;           // JSON_NUM
  char *str;            // JSON_STR: the decoded text (UTF-8); JSON_NUM: as written
  int n;                // JSON_ARR / JSON_OBJ: number of items
  struct Json **items;
  char **keys;          // JSON_OBJ: the key of each item
} Json;

Json *json_parse(const char *text);      // NULL when it is not JSON
void json_free(Json *j);

Json *json_get(const Json *obj, const char *key);  // NULL when absent or obj is no object
Json *json_at(const Json *arr, int i);             // NULL when out of range
const char *json_str(const Json *j, const char *def);  // def unless a string
int64_t json_int(const Json *j, int64_t def);         // def unless a number
int json_bool(const Json *j, int def);                // def unless true/false

// a growing text buffer
typedef struct {
  char *s;
  size_t len, cap;
} JsonBuf;

void jb_printf(JsonBuf *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void jb_str(JsonBuf *b, const char *s);   // a quoted, escaped string; NULL writes null
void jb_json(JsonBuf *b, const Json *j);  // a parsed value written back
void jb_free(JsonBuf *b);

#endif
