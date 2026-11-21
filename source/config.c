/* config.c -- simple configuration parser
 *
 * Copyright (C) 2021 Andy Nguyen, fgsfds
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <switch.h>

#include "config.h"
#include "paths.h"

#define CONFIG_VARS \
  CONFIG_VAR_INT(config_version); \
  CONFIG_VAR_INT(screen_width); \
  CONFIG_VAR_INT(screen_height); \
  CONFIG_VAR_INT(vsync); \
  CONFIG_VAR_INT(gfx_mt); \
  CONFIG_VAR_INT(gfx_jobs); \
  CONFIG_VAR_INT(main_core); \
  CONFIG_VAR_INT(job_workers); \
  CONFIG_VAR_INT(fourth_core); \
  CONFIG_VAR_INT(fast_atomics); \
  CONFIG_VAR_INT(auto_30); \
  CONFIG_VAR_INT(audio_update_hz); \
  CONFIG_VAR_INT(srp_batcher); \
  CONFIG_VAR_FLOAT(lod_bias); \
  CONFIG_VAR_FLOAT(shadow_distance); \
  CONFIG_VAR_INT(shadows); \
  CONFIG_VAR_FLOAT(max_delta); \
  CONFIG_VAR_FLOAT(fixed_step); \
  CONFIG_VAR_INT(dynamic_res); \
  CONFIG_VAR_FLOAT(dynamic_res_min); \
  CONFIG_VAR_INT(vsync_wait); \
  CONFIG_VAR_INT(fps_unlock); \
  CONFIG_VAR_INT(pd_patches); \
  CONFIG_VAR_INT(rumble); \
  CONFIG_VAR_INT(video_threads); \
  CONFIG_VAR_INT(video_fast); \
  CONFIG_VAR_INT(shader_log); \
  CONFIG_VAR_INT(log_all); \
  CONFIG_VAR_INT(log_jni); \
  CONFIG_VAR_INT(log_fs); \
  CONFIG_VAR_INT(log_net); \
  CONFIG_VAR_INT(log_diag); \
  CONFIG_VAR_INT(profile); \
  CONFIG_VAR_INT(sample_main); \
  CONFIG_VAR_INT(gc_off); \
  CONFIG_VAR_INT(fmod_off); \
  CONFIG_VAR_INT(pd_hwcap); \
  CONFIG_VAR_STR(language); \
  CONFIG_VAR_STR(country); \
  CONFIG_VAR_STR(capture_at); \
  CONFIG_VAR_STR(dump_threads_at); \
  CONFIG_VAR_STR(test_script); \
  CONFIG_VAR_STR(brk_unity); \
  CONFIG_VAR_STR(boot_extra);

Config config;

// actual screen size that is in use right now
int screen_width = 1280;
int screen_height = 720;

static inline void parse_var(const char *name, const char *value) {
  #define CONFIG_VAR_INT(var) if (!strcmp(name, #var)) { config.var = atoi(value); return; }
  #define CONFIG_VAR_STR(var) if (!strcmp(name, #var)) { snprintf(config.var, sizeof(config.var), "%s", value); return; }
  #define CONFIG_VAR_FLOAT(var) if (!strcmp(name, #var)) { config.var = strtof(value, NULL); return; }
  CONFIG_VARS
  #undef CONFIG_VAR_INT
  #undef CONFIG_VAR_STR
  #undef CONFIG_VAR_FLOAT
}

// The console's language as a Java Locale (language, country).
static void console_locale(char *lang, size_t ll, char *country, size_t cl) {
  u64 lcode = 0;
  SetLanguage sl = SetLanguage_ENUS;
  if (R_SUCCEEDED(setInitialize())) {
    if (R_SUCCEEDED(setGetSystemLanguage(&lcode)))
      setMakeLanguage(lcode, &sl);
    setExit();
  }
  const char *l = "en", *c = "US";
  switch (sl) {
    case SetLanguage_ENGB:  l = "en"; c = "GB"; break;
    case SetLanguage_FR:    l = "fr"; c = "FR"; break;
    case SetLanguage_FRCA:  l = "fr"; c = "CA"; break;
    case SetLanguage_DE:    l = "de"; c = "DE"; break;
    case SetLanguage_IT:    l = "it"; c = "IT"; break;
    case SetLanguage_ES:    l = "es"; c = "ES"; break;
    case SetLanguage_ES419: l = "es"; c = "MX"; break;
    case SetLanguage_NL:    l = "nl"; c = "NL"; break;
    case SetLanguage_PT:    l = "pt"; c = "PT"; break;
    case SetLanguage_PTBR:  l = "pt"; c = "BR"; break;
    case SetLanguage_RU:    l = "ru"; c = "RU"; break;
    case SetLanguage_JA:    l = "ja"; c = "JP"; break;
    case SetLanguage_KO:    l = "ko"; c = "KR"; break;
    case SetLanguage_ZHCN:
    case SetLanguage_ZHHANS:l = "zh"; c = "CN"; break;
    case SetLanguage_ZHTW:
    case SetLanguage_ZHHANT:l = "zh"; c = "TW"; break;
    default: break;
  }
  snprintf(lang, ll, "%s", l);
  snprintf(country, cl, "%s", c);
}

static void set_defaults(Config *c) {
  memset(c, 0, sizeof(*c));
  c->config_version = CONFIG_VERSION;
  c->screen_width = -1;
  c->screen_height = -1;
  c->vsync = 1;
  c->gfx_mt = 1;
  c->gfx_jobs = 1;
  c->main_core = 2;
  c->job_workers = 0;
  c->fourth_core = 1;
  c->srp_batcher = 1;
  c->lod_bias = 0.6f;
  c->shadows = 1;
  c->shadow_distance = 25.0f; // (the game's 50 m: half as far, the shadows near the player kept)
  c->max_delta = 0.1f;
  c->dynamic_res = 1;
  c->dynamic_res_min = 0.70f;
  c->auto_30 = 1;
  c->pd_patches = 1;
  c->fast_atomics = 1;
  c->audio_update_hz = 40;
  c->rumble = 100;
  c->video_fast = 0;
  c->log_diag = 1;
  c->profile = 0;
  console_locale(c->language, sizeof(c->language), c->country, sizeof(c->country));
}

int read_config(const char *file) {
  char line[1024] = { 0 };

  set_defaults(&config);

  FILE *f = fopen(file, "r");
  if (f == NULL)
    return -1;
  config.config_version = 0; // a file from before versions: the oldest

  do {
    char *name = NULL, *value = NULL, *tmp = NULL;
    if (fgets(line, sizeof(line), f) != NULL) {
      name = line;
      while (*name && isspace((int)*name)) ++name;
      if (name[0] == '#') continue; // skip comments
      for (tmp = name; *tmp && !isspace((int)*tmp); ++tmp);
      if (*tmp != 0) {
        *tmp = 0;
        for (value = tmp + 1; *value && isspace((int)*value); ++value);
        for (tmp = value + strlen(value) - 1; tmp >= value && isspace((int)*tmp); --tmp) *tmp = 0;
        parse_var(name, value);
      }
    }
  } while (!feof(f));

  fclose(f);

  // version 2 (hardware round 3, frames at 25-40 ms): the profiler on, to see where the main thread's time goes
  if (config.config_version < 2)
    config.profile = 1;
  // version 3 (round 4, the main thread the limit with the other cores half idle): native graphics jobs (the
  // render loop's work on the job workers) and a core of its own for the main thread
  if (config.config_version < 3) {
    config.gfx_jobs = 2;
    config.main_core = 1;
  }
  // version 4 (round 5: 800 single draws a frame in a level's colour pass, the SRP Batcher off by Project Dream's
  // loader): the SRP Batcher as the game asks
  if (config.config_version < 4) {
    config.srp_batcher = 1;
    config.lod_bias = 0.6f; // (the first level: 14% fewer draws at 0.5, the picture all but the same)
  }
  // version 6: fps_unlock off again (version 5 turned it on): the game's 30 fps cap is its own setting, the player's
  // choice -- holding 60 over it only took that choice away
  if (config.config_version < 6)
    config.fps_unlock = 0;
  // version 7 (round 8: physics at 1/60 s with up to 0.333 s of catch-up a frame): catch-up held at 0.1 s; shadows
  // a knob (the URP asset's distance; the old shadow_distance set QualitySettings', which URP does not read)
  if (config.config_version < 7) {
    config.max_delta = 0.1f;
    config.shadows = 1;
    if (config.shadow_distance <= 0)
      config.shadow_distance = 25.0f;
  }
  // version 8 (round 10: world 2 GPU-bound at 460 MHz even at 50% render scale): dynamic resolution on
  if (config.config_version < 8) {
    config.dynamic_res = 1;
    config.dynamic_res_min = 0.5f;
  }
  // version 9 (the player: 50% too soft): dynamic resolution down to 75% at most, unless set to something else
  if (config.config_version < 9 && fabsf(config.dynamic_res_min - 0.5f) < 0.001f)
    config.dynamic_res_min = 0.75f;
  // version 10 (round 12, the main thread still the limit; another Switch port of the game, Thorhax's
  // SonicDreamTeam-NX, measured these faster there): graphics jobs in the older mode instead of native, one job
  // worker per core with the main thread's core shared while it waits, and the fourth core when it is allowed
  if (config.config_version < 10) {
    if (config.gfx_jobs == 2)
      config.gfx_jobs = 1;
    if (config.main_core == 1)
      config.main_core = 2;
    config.job_workers = 0;
    config.fourth_core = 1;
  }
  // version 11 (round 15: 0.70 needed at 460 MHz for 60, dynamic resolution stopped at 0.75; frames at 36-43 fps at
  // stock clocks shown unevenly): dynamic resolution down to 0.65, the even 30 on
  if (config.config_version < 11) {
    if (fabsf(config.dynamic_res_min - 0.75f) < 0.001f)
      config.dynamic_res_min = 0.65f;
    config.auto_30 = 1;
  }
  // version 13 (the first release): the sampling profiler off (it pauses the main thread 500 times a second for 3 s
  // of every 30); profile 1 turns it on again
  if (config.config_version < 13)
    config.profile = 0;
  // version 12 (round 17: "the drop looks grainy"): dynamic resolution down to 0.70 at most
  if (config.config_version < 12 && fabsf(config.dynamic_res_min - 0.65f) < 0.001f)
    config.dynamic_res_min = 0.70f;
  config.config_version = CONFIG_VERSION;
  return 0;
}

int write_config(const char *file) {
  FILE *f = fopen(file, "w");
  if (f == NULL)
    return -1;

  fprintf(f,
    "# " PORT_NAME " configuration\n"
    "\n"
    "# (this file's format; leave it)\n"
    "config_version %d\n"
    "\n"
    "# Render size. -1 = auto: 720p handheld, 1080p docked.\n"
    "screen_width %d\n"
    "screen_height %d\n"
    "\n"
    "# 1 = held to the display's 60 Hz, 0 = uncapped.\n"
    "vsync %d\n"
    "\n"
    "# 1 = the engine renders on a thread of its own (Unity's multithreaded\n"
    "# rendering), 0 = on the game's main thread.\n"
    "gfx_mt %d\n"
    "\n"
    "# Unity's graphics jobs: the render loop's work (culling, batching, draw\n"
    "# setup) on the job worker threads. 0 off, 1 jobs (the older mode),\n"
    "# 2 native jobs (before version 10).\n"
    "gfx_jobs %d\n"
    "\n"
    "# The game's main thread: 1 = a core of its own, the other threads\n"
    "# (render, jobs, loading) on the others; 2 = its own core, which the\n"
    "# other threads may use too while the main thread waits for them (they\n"
    "# never take it from the main thread); 0 = no core of its own.\n"
    "main_core %d\n"
    "\n"
    "# Unity's job worker threads: 0 = one per core the game may use (3, or 4\n"
    "# with the fourth core), -1 = Unity's own count (2), or a number.\n"
    "job_workers %d\n"
    "\n"
    "# 1: use the fourth core too when allowed. The homebrew menu (title\n"
    "# override) gives three; a forwarder can allow four (sphaira: Install\n"
    "# Forwarder, CPU Cores 4).\n"
    "fourth_core %d\n"
    "\n"
    "# 1: Unity's SRP Batcher on, as the game asks (Project Dream's loader\n"
    "# turns it off): the many small objects of a level drawn far cheaper.\n"
    "srp_batcher %d\n"
    "\n"
    "# Unity's LOD bias held at this (0 = the game's). A lower one draws far\n"
    "# objects simpler or not at all: less work for the CPU and the GPU\n"
    "# (1.0 is Unity's usual; try 0.4-0.7).\n"
    "lod_bias %.2f\n"
    "\n"
    "# Real-time shadows. shadows 0: none at all (a whole culling and drawing\n"
    "# pass less each frame). shadow_distance: how far they reach, in metres\n"
    "# (the game's: 50; 0 = the game's; shorter = fewer objects drawn into\n"
    "# the shadows each frame).\n"
    "shadows %d\n"
    "shadow_distance %.1f\n"
    "\n"
    "# Dynamic resolution: while the GPU's own time per frame is over the\n"
    "# frame's, the picture is drawn smaller (down to dynamic_res_min: 0.70\n"
    "# = 70%%), and back up to the Resolution Scale setting when it has room.\n"
    "# On or off: the game's Graphics Quality menu (Dynamic Resolution, in\n"
    "# VSync's place); this line only when it has none.\n"
    "dynamic_res %d\n"
    "dynamic_res_min %.2f\n"
    "\n"
    "# The game's Graphics menu has it now: Frame Rate 30 / 60 / Auto (Auto:\n"
    "# 60, and an even 30 while 60 is out of reach). This line only counts\n"
    "# when the menu does not: 1 = as Auto, 0 = off.\n"
    "auto_30 %d\n"
    "\n"
    "# The most game time one frame may cover, in seconds (0 = the game's\n"
    "# 0.333): after a hitch the physics catches up at most this much at\n"
    "# once, so one slow frame does not make the next one slow too.\n"
    "max_delta %.3f\n"
    "\n"
    "# The physics step in seconds, in place of the game's 1/60 (0 = the\n"
    "# game's). 0.02 = 50 Hz: physics and the game's fixed-step code run a\n"
    "# sixth less often (a little more speed); movement can feel different.\n"
    "fixed_step %.3f\n"
    "\n"
    "# 0: when a frame takes longer than 1/60 s, Unity does not wait for the\n"
    "# next screen refresh on top (no rounding down to 30 fps); 1: it does.\n"
    "vsync_wait %d\n"
    "\n"
    "# 1: the game's frame rate held at 60 even when it asks for 30 (its own\n"
    "# 30 fps setting no longer works then); 0: the game's choice.\n"
    "fps_unlock %d\n"
    "\n"
    "# HD rumble for the game's haptics: strength in percent, 0 = off.\n"
    "rumble %d\n"
    "\n"
    "# Videos (the intro movie, menu clips): decoding threads (0 = auto),\n"
    "# 1 = FFmpeg's faster H.264 (skips deblocking where it shows least).\n"
    "video_threads %d\n"
    "video_fast %d\n"
    "\n"
    "# Project Dream's game patches (its menus, graphics options, touch UI).\n"
    "# debug: 0 = off.\n"
    "pd_patches %d\n"
    "\n"
    "# 1: the garbage collector's write barrier skips its atomic operation\n"
    "# when there is nothing to change (2-3%% of the main thread's time);\n"
    "# 0: Project Dream's loader's code as it is.\n"
    "fast_atomics %d\n"
    "\n"
    "# The audio engine's update (sound positions, voices) this many times a\n"
    "# second at most: 40 = every other frame at 60 fps; 0 = every frame.\n"
    "audio_update_hz %d\n"
    "\n"
    "# The game's language as a Java locale (default: the console's).\n"
    "language %s\n"
    "country %s\n"
    "\n"
    "# debug.log detail (1 = on): each shader variant compiled on the console,\n"
    "# every Android log line, JNI call, file access, connection; start-up\n"
    "# detail; where the main thread's time goes.\n"
    "shader_log %d\n"
    "log_all %d\n"
    "log_jni %d\n"
    "log_fs %d\n"
    "log_net %d\n"
    "log_diag %d\n"
    "profile %d\n"
    "\n"
    "# debug: seconds at which to log every thread's place (\"30 60\"), an\n"
    "# input script, libunity.so offsets to trap at\n"
    "dump_threads_at %s\n"
    "test_script %s\n"
    "brk_unity %s\n"
    "boot_extra %s\n"
    "sample_main %d\n"
    "capture_at %s\n"
    "gc_off %d\n"
    "pd_hwcap %d\n"
    "fmod_off %d\n",
    config.config_version, config.screen_width, config.screen_height, config.vsync, config.gfx_mt, config.gfx_jobs, config.main_core, config.job_workers, config.fourth_core, config.srp_batcher, (double)config.lod_bias, config.shadows, (double)config.shadow_distance, config.dynamic_res, (double)config.dynamic_res_min, config.auto_30, (double)config.max_delta, (double)config.fixed_step, config.vsync_wait, config.fps_unlock, config.rumble, config.video_threads, config.video_fast, config.pd_patches, config.fast_atomics, config.audio_update_hz,
    config.language, config.country, config.shader_log, config.log_all, config.log_jni, config.log_fs,
    config.log_net, config.log_diag, config.profile, config.dump_threads_at, config.test_script, config.brk_unity, config.boot_extra, config.sample_main, config.capture_at, config.gc_off, config.pd_hwcap, config.fmod_off);
  fclose(f);
  return 0;
}
