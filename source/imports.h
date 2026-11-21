/* imports.h -- .so import resolution
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __IMPORTS_H__
#define __IMPORTS_H__

#include <stdio.h>
#include <stdlib.h>
#include "so_util.h"

// the system libraries (libc, libm, liblog, libz, ...)
extern DynLibFunction dynlib_functions[];
extern size_t dynlib_numfunctions;
// bionic's libstdc++.so
extern DynLibFunction dynlib_stdcxx[];
extern size_t dynlib_numstdcxx;
// libm and pure libc functions passed straight to newlib (libc_table.c)
extern DynLibFunction libc_functions[];
extern const size_t libc_numfunctions;
// OpenGL ES / EGL (gl_table.c)
extern DynLibFunction gl_functions[];
extern const size_t gl_numfunctions;

void update_imports(void);

#endif
