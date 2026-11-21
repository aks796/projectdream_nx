/* buildinfo.c -- the build stamp, recompiled on every make (Makefile) so the
 * whole .nro agrees on one value.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include "buildinfo.h"

const char *build_stamp(void) {
  return BUILD_STAMP;
}
