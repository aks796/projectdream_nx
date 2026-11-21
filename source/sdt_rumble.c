/* sdt_rumble.c -- the game's moments as HD rumble
 *
 * Sonic Dream Team was made for phones and Apple TV: its own vibration (Hardlight's VibrationManager) has two events,
 * DefaultSelection and LongPressSelection -- touch-screen taps -- and the Mac build the loader runs has even its
 * platform call compiled out. So the moments here are chosen as Sonic games on the Switch use rumble: being hit
 * (and a stumble, the shield), losing a life, striking an enemy, a spin dash let go, a boost,
 * switching characters, the goal. Nothing for running, jumping, rings or menus. Each a pulse or two of HD rumble
 * (rumble.c: two bands, a shape), the same moment not twice within its own spacing.
 *
 * Called from the game's code itself: pd_shim.c's hooks at these methods' first instruction (ga_hook.s), on the
 * game's main thread -- rumble_play only queues a pulse for the rumble thread.
 * On or off: the game's Settings menu, Vibration (Project Dream's option, pd_vibration_option); its strength:
 * config rumble (percent).
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <switch.h>

#include "config.h"
#include "log.h"
#include "pd_shim.h"
#include "rumble.h"

enum { EV_DAMAGE, EV_STUMBLE, EV_SHIELD, EV_RESPAWN, EV_HOMING, EV_SPIN_DASH, EV_BOOST, EV_SWITCH, EV_GOAL, EV_COUNT };
static const char *const k_names[EV_COUNT] = { "hit", "stumble", "shield hit", "a life lost", "an enemy struck",
                                               "spin dash", "boost", "character switch", "goal" };
static u64 s_last[EV_COUNT];
static unsigned s_logged[EV_COUNT];

// the moment may rumble now: Vibration on, and not the same moment within min_ms
static int moment(int ev, unsigned min_ms) {
  if (pd_vibration_option() == 0 || config.rumble <= 0)
    return 0;
  const u64 now = armGetSystemTick();
  if (s_last[ev] && now - s_last[ev] < armNsToTicks((u64)min_ms * 1000000ull))
    return 0;
  s_last[ev] = now;
  if (s_logged[ev]++ < 3)
    log_printf("[rumble] %s", k_names[ev]);
  return 1;
}

// The feel (round 19: "still weak, not official"). The Joy-Cons' actuators resonate near 160 Hz (the low band) and
// 320 Hz (the high band): round 18's 90-140 Hz lows hummed and barely moved them, however strong. Every pulse here sits
// on those two, and the moments are built as Nintendo's own are -- a crisp transient (the high band, 30-45 ms) where
// something strikes, a weighty body (the low band, decaying fast: THUD) where it lands, a rising surge (FADE) only for
// a launch; nothing long or wobbling. rumble.c keeps the two bands together at full at most.
#define LO 160.f
#define HI 320.f
// round 20: on resonance, the round 19 amplitudes were "too strong" -- all of them at 60%, the shapes kept (config
// rumble scales them again: 100 = these)
#define K 0.6f

// rings scattered (Character.TriggerCascade, the knockback): a crack and a heavy, quick-dying body
void rumble_on_damage(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (s_last[EV_RESPAWN] && armGetSystemTick() - s_last[EV_RESPAWN] < armNsToTicks(100000000ull))
    return; // (the life lost's rumble already)
  if (moment(EV_DAMAGE, 500)) {
    rumble_play(0, 40.f, K * 0.45f, LO, K * 0.55f, HI, SHAPE_CLICK);
    rumble_play_at(0, 25.f, 190.f, K * 0.92f, LO, K * 0.08f, HI, SHAPE_THUD);
  }
}

// a stumble on a hazard: one solid bump
void rumble_on_stumble(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_STUMBLE, 400))
    rumble_play(0, 130.f, K * 0.75f, LO, K * 0.15f, HI, SHAPE_THUD);
}

// the shield taking the hit: a bright knock, a lighter body
void rumble_on_shield(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_SHIELD, 400)) {
    rumble_play(0, 35.f, K * 0.30f, LO, K * 0.70f, HI, SHAPE_CLICK);
    rumble_play_at(0, 20.f, 120.f, K * 0.60f, LO, K * 0.10f, HI, SHAPE_THUD);
  }
}

// a fall, a defeat (TriggerRespawn, TriggerHardFailure): one heavy blow, then a short settling
void rumble_on_respawn(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_RESPAWN, 1500)) {
    // (a fall scatters the rings too, the same moment: the hit's pulses give way to this one)
    if (s_last[EV_DAMAGE] && armGetSystemTick() - s_last[EV_DAMAGE] < armNsToTicks(100000000ull))
      rumble_clear(0);
    rumble_play(0, 45.f, K * 0.40f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 30.f, 320.f, K * 1.00f, LO, K * 0.00f, HI, SHAPE_THUD);
    rumble_play_at(0, 360.f, 160.f, K * 0.45f, LO, K * 0.00f, HI, SHAPE_THUD);
  }
}

// an enemy or a breakable struck (PlayerDamageableObject.ReceiveHit): a crisp tap with a little weight
void rumble_on_homing_hit(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_HOMING, 90)) {
    rumble_play(0, 32.f, K * 0.35f, LO, K * 0.65f, HI, SHAPE_CLICK);
    rumble_play_at(0, 15.f, 70.f, K * 0.55f, LO, K * 0.10f, HI, SHAPE_THUD);
  }
}

// MovementGroundSpinDashCharge.OnLaunch: let go -- a kick, then the surge
void rumble_on_spin_dash(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_SPIN_DASH, 300)) {
    rumble_play(0, 35.f, K * 0.40f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 20.f, 240.f, K * 0.80f, LO, K * 0.12f, HI, SHAPE_FADE);
  }
}

// the boost (CharacterState_GroundBoostBase.DoOnEnter: the boost state entered, its stamina switched on): a kick and
// a surge -- the boost's sound, felt
void rumble_on_boost_state(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_BOOST, 300)) {
    rumble_play(0, 40.f, K * 0.40f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 20.f, 280.f, K * 0.85f, LO, K * 0.12f, HI, SHAPE_FADE);
  }
}

// a boost on a rail (CharacterState_RailMovementBoostBase.DoOnEnter): the same, lighter
void rumble_on_rail_boost(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_BOOST, 300)) {
    rumble_play(0, 35.f, K * 0.35f, LO, K * 0.55f, HI, SHAPE_CLICK);
    rumble_play_at(0, 20.f, 220.f, K * 0.70f, LO, K * 0.10f, HI, SHAPE_FADE);
  }
}

// a boost pad or ring (CharacterBoostTrigger.DoTriggerEnter): the boost's kick and a shorter surge
void rumble_on_boost(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_BOOST, 300)) {
    rumble_play(0, 35.f, K * 0.35f, LO, K * 0.65f, HI, SHAPE_CLICK);
    rumble_play_at(0, 20.f, 180.f, K * 0.75f, LO, K * 0.10f, HI, SHAPE_FADE);
  }
}

// a team member swapping in (Character.BeginSwapStart, CharacterManager.SwapCharacter): two quick taps, the second
// with weight
void rumble_on_switch(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_SWITCH, 500)) {
    rumble_play(0, 30.f, K * 0.25f, LO, K * 0.65f, HI, SHAPE_CLICK);
    rumble_play_at(0, 110.f, 40.f, K * 0.40f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 130.f, 110.f, K * 0.65f, LO, K * 0.10f, HI, SHAPE_THUD);
  }
}

// MissionReachGoalTracker.Action_ReachedGoal: three rising taps and a swell
void rumble_on_goal(uint64_t self, uint64_t a1, uint64_t a2) {
  (void)self, (void)a1, (void)a2;
  if (moment(EV_GOAL, 3000)) {
    rumble_play(0, 30.f, K * 0.20f, LO, K * 0.50f, HI, SHAPE_CLICK);
    rumble_play_at(0, 120.f, 35.f, K * 0.30f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 240.f, 40.f, K * 0.40f, LO, K * 0.60f, HI, SHAPE_CLICK);
    rumble_play_at(0, 300.f, 360.f, K * 0.80f, LO, K * 0.10f, HI, SHAPE_FADE);
  }
}
