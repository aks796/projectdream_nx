/* prof.h -- where the main thread's time goes in a race (prof.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef PROF_H
#define PROF_H

// on the main thread, once: the profiler (config profile)
void prof_init(void);
// the thread presenting the frames (egl_shim.c): the render thread, sampled too
void prof_render_thread(Handle h);

#endif
