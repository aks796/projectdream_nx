/* unity.h -- loading and driving the Unity engine (unity.c), and feeding it
 * input (input.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef UNITY_H
#define UNITY_H

#include <stdint.h>
#include "so_util.h"

extern so_module *g_unity;   // libunity.so
extern so_module *g_il2cpp;  // libil2cpp.so

// Load libunity.so and libil2cpp.so (as Unity's libmain does), run their
// constructors and JNI_OnLoad, and find the UnityPlayer natives.
void unity_load(void);
// What UnityPlayer's constructor, surfaceCreated, onResume and the window
// focus do, in that order. The window must be free for EGL by now.
void unity_start(void);
// One UnityMain loop iteration; 0 when the engine wants to quit.
int unity_frame(void);
void unity_pause(int paused);      // HOME / sleep: nativePause / nativeResume
int unity_gfx_threading_mode(void); // 2 render thread, 3 graphics jobs, 4 native graphics jobs
void unity_surface_changed(void);  // the window's size changed (docked/handheld)
void unity_focus(int focused);
void unity_quit(void);

// --- input (input.c) ---------------------------------------------------------

// Touch: up to 10 fingers; ids are stable while a finger is down.
// action: 0 down, 1 up, 2 move (per pointer, batched into MotionEvents).
void input_init(void);
void input_touch(int id, int down, float x, float y);   // call per pointer per frame
void input_flush(void);                                  // send what changed this frame
void input_key(int android_keycode, int down);           // KEYCODE_BACK = 4
uint8_t unity_inject(void *event);                       // nativeInjectEvent

#endif
