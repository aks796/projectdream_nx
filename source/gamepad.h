/* gamepad.h -- playing with a controller (gamepad.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef GAMEPAD_H
#define GAMEPAD_H

#include <switch.h>

// the controller this frame (main.c: the console's, with scripted input)
typedef struct {
  u64 held, down;
  int lx, ly, rx, ry;   // sticks, -32767..32767 (y up)
} PadInput;

// the touchscreen's first finger this frame, window pixels (main.c, before
// gamepad_update)
void gamepad_touch(int down, float x, float y);
// one frame: fingers for the game (input.c) from the controller; w x h is
// the window; touchscreen_used hides the ring
void gamepad_update(const PadInput *pad, int w, int h, int touchscreen_used);
// the game's C# is up (main.c, once game_patch_il2cpp has run): the
// controller may ask it things from then on
void gamepad_il2cpp_up(void);
// the ring onto this button (a Selectable) once it is on the screen (a new
// screen's first choice, where the ring's own would be another)
void gamepad_focus_hint(void *selectable);
// the ring onto that button now (it is on the screen, pressable)
void gamepad_focus_now(void *selectable);
// the button the ring is on (NULL none)
void *gamepad_focus(void);
// the menus' buttons looked at every other frame for a while (a screen
// about to change)
void gamepad_scan_soon(int frames);
// the ring's button measured again (its list's rows moved under it)
void gamepad_remeasure(void);
// a race's steering from a controller: the stick's x as a wheel (a dead
// zone, a curve), the D-pad full lock; -1 .. 1
float gamepad_race_steer(const PadInput *pad, u64 held);
// the ring around the button the controller is on, for drawing (egl_shim.c):
// 1 when it is to be drawn, with its rectangle in window pixels
int gamepad_ring(float *x0, float *y0, float *x1, float *y1);
// the ring's shape: a rectangle, an arrow pointing right (the race's OK), a
// circle, a pill, a glow (no ring: the home screen's bottom row), or a
// rectangle with rounded corners just outside the button (MKT KV 5.2's track
// pictures: round their white border, over neither picture nor name)
enum { RING_RECT, RING_ARROW, RING_CIRCLE, RING_PILL, RING_GLOW, RING_ROUND };
int gamepad_ring_shape(void);
// A or a touch in the last half second (a press the player's, not the game's own)
int gamepad_user_pressed(void);
// RING_ARROW with its left side notched in (Custom Race's OK, Auto's point in it)
int gamepad_ring_notch(void);

#endif
