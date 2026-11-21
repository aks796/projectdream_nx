/* nabe.h -- a byte range of the game file (vfs.c's virtual fds)
 *
 * (The name is mkt_nx's: there it was a member of the APK's asset cache.)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef NABE_H
#define NABE_H

#include <stdint.h>

typedef struct {
  const char *path; // the entry's name, for logs (may be NULL)
  uint64_t off;     // data offset in the APK
  uint64_t size;
} NabeFile;

#endif
