/* pd_shim.h -- Project Dream's loader (libil2cpp.so) on the Switch (pd_shim.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef PD_SHIM_H
#define PD_SHIM_H

#include <switch.h>

// Before libil2cpp.so is loaded: its imports (mmap, mprotect, munmap) and the
// hooks put in as it is mapped (code memory, GC thread stops, import traps).
void pd_shim_init(void);
// After it is loaded: the Java methods its natives call, Haptics.register.
void pd_shim_java_init(void);
// Each frame: the console's controller as GameActivity's ControllerInput
// publishes it (held buttons, sticks in libnx's -32767..32767).
void pd_shim_pad(u64 held, int lx, int ly, int rx, int ry, int connected);
// GC thread stops so far.
void pd_shim_gc_stats(unsigned *stops);
// the collector's stops of the world since the last call: how many, their total and the longest
void pd_shim_gc_pauses(unsigned *n, double *total_ms, double *max_ms);
// code pages made writable again (unmapped and mapped: gp_initialize's patches)
unsigned pd_shim_remaps(void);
// debug: "GameAssembly+off" for an address in the loader's code memory (1), else 0
int pd_shim_name(uintptr_t a, char *out, size_t n);
void pd_shim_debug(void);
int pd_list_open(void); // Project Dream's list screen (Achievements) is open
void pd_list_move(int step, int sound); // its selection up (-1) / down (+1), with its tick or not
float pd_render_scale_option(void); // Project Dream's Resolution Scale slider as a scale (1.00 at its most; -1: none)
int pd_dynamic_res_option(void); // its Graphics menu's Dynamic Resolution: 0/1 (-1: not there)
int pd_fps_choice(void);
int pd_vibration_option(void); // Settings' Vibration: 0 off, 1 on (-1: not there) // its Frame Rate: 0 = 30, 1 = 60, 2 = Auto (-1: Project Dream's own slider)
// Unity's il2cpp API pointers in [lo, hi) to the game's functions (after the game's image is loaded): how many
int pd_direct_api(uintptr_t lo, uintptr_t hi);

#endif
