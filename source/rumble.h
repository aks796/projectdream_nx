/* rumble.h -- HD rumble pulses (rumble.c)
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef RUMBLE_H
#define RUMBLE_H

#include <stdint.h>

void rumble_init(void);
// no rumble while > 0
extern int rumble_mute;
// a pulse of HD rumble on a player's controller (0, 1): ms long, its two
// bands' amplitudes (0..1) and frequencies, its shape
enum { SHAPE_FADE, SHAPE_THUD, SHAPE_CLICK, SHAPE_ROUGH };
void rumble_play(int player, float ms, float lo, float flo, float hi, float fhi, int shape);
// the same, starting delay_ms from now (a pattern of pulses)
void rumble_play_at(int player, float delay_ms, float ms, float lo, float flo, float hi, float fhi, int shape);
// every pulse of a player's playing or waiting, gone (one moment taking another's place)
void rumble_clear(int player);
// sdt_rumble.c: the game's moments (pd_shim.c's hooks in the game's code call these)
void rumble_on_damage(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_respawn(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_homing_hit(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_spin_dash(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_boost(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_boost_state(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_rail_boost(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_stumble(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_shield(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_switch(uint64_t self, uint64_t a1, uint64_t a2);
void rumble_on_goal(uint64_t self, uint64_t a1, uint64_t a2);

#endif
