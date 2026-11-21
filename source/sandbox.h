/* sandbox.h -- nothing outside the port's folder is ever changed (sandbox.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __SANDBOX_H__
#define __SANDBOX_H__

// after paths_init: 0 when the port's folder is not one of its own (the
// card's root, /switch, ...) -- then it must not run
int sandbox_init(void);
// path (as handed to the filesystem) lies strictly inside the port's folder
int sandbox_allows(const char *path);
// sandbox_allows, else a "[sandbox] refused <what> <path>" log line and
// errno EACCES; returns 1 when allowed
int sandbox_check(const char *path, const char *what);
// the port's folder, folded ("/switch/gd_geode_nx")
const char *sandbox_root(void);

#endif
