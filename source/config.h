/* config.h -- the port's settings (config.txt beside the .nro; config.c)
 *
 * From mkt_nx.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#ifndef __CONFIG_H__
#define __CONFIG_H__

#define CONFIG_NAME "config.txt"

// android.os.Build.VERSION.SDK_INT reported through the fake JNI (Project
// Dream's APK: min 26, target 33).
#define ANDROID_SDK_INT 33

// actual render/surface size (picked at runtime from docked state)
extern int screen_width;
extern int screen_height;

#define CONFIG_VERSION 13 // config.c: what an older file's settings become

typedef struct {
  int config_version;   // the file's format (CONFIG_VERSION)
  int screen_width;     // -1 = auto (1280x720 handheld, 1920x1080 docked)
  int screen_height;
  int vsync;            // 1 = 60 fps cap (FIFO present), 0 = uncapped (vkdk_wsi)
  int gfx_mt;           // the engine's multithreaded rendering (java_unity.c)
  int gfx_jobs;         // Unity's graphics jobs: 0 off, 1 jobs (the older mode), 2 native jobs
  int main_core;        // 1: the first core for the main thread alone; 2: its own, and the game's other threads may
                        // use it too while the main thread waits (they run below its priority); 0: no own core
  int job_workers;      // Unity's job worker threads: 0 = one per core the game may use, -1 = Unity's own count
  int fourth_core;      // 1: the fourth core too when the process may use it (a forwarder that allows four cores)
  int srp_batcher;      // 1: the SRP Batcher as the game asks (Project Dream's loader forces it off)
  float lod_bias;       // Unity's LOD bias held at this (0 = the game's): lower = simpler/fewer far objects
  float shadow_distance;// the real-time shadows' distance held at this, metres (0 = the game's)
  int shadows;          // 0: no real-time shadows (URP's shadow pass and caster culling skipped)
  int dynamic_res;      // 1: the render scale lowered while the GPU makes frames late (drs.c); the menu's option wins
  float dynamic_res_min;// the lowest render scale dynamic resolution goes to (0.70 = 70%)
  int auto_30;          // 1: an even 30 when the game asks for 60 and the frames cannot make it (drs.c); 2: always
  float max_delta;      // Time.maximumDeltaTime held at this at most (0 = the game's): physics catch-up after a hitch
  float fixed_step;     // Time.fixedDeltaTime in place of the game's 1/60 s (0 = the game's; 0.02 = 50 Hz physics)
  int vsync_wait;       // 1: Unity's own vsync wait always (frames round down to 30 when late); 0: only when on time
  int fps_unlock;       // 1: the game's frame rate held at 60 when it sets a lower one (overrides its 30 fps setting)
  int pd_patches;       // Project Dream's game patches (gp_initialize; debug: 0 = off)
  int fast_atomics;     // 1: the write barrier's atomics stubs with a fast path (pd_shim.c), 0: the loader's own
  int audio_update_hz;  // FMOD's System::update this many times a second at most (unity_diag.c; 0 = every frame)
  int rumble;           // HD rumble for the game's haptics: strength in percent, 0 = off
  int video_threads;    // FFmpeg decoding threads for videos (0 = auto: 3 for HD, else 2)
  int video_fast;       // videos: FFmpeg's faster H.264 (no deblocking of non-reference frames)
  int shader_log;       // log each shader variant vkdk compiles on the console
  int log_all;          // every Android log line into debug.log
  int log_jni;          // every JNI method lookup and call into debug.log
  int log_fs;           // every file open/stat into debug.log
  int log_net;          // every connection into debug.log
  int log_diag;         // more start-up detail (pd_shim.c)
  int pd_hwcap;         // debug: AT_HWCAP for Project Dream's loader (0: patch every LSE/RCpc/SHA3/dot instruction)
  int gc_off;
  int fmod_off;         // debug: no sound output thread           // debug: no garbage collection (GC_DONT_GC)
  int sample_main;      // debug: the main thread's pc every N ms into the emulator's log (sampler.c)
  int profile;          // where the main thread's time goes (prof.c)
  char language[16];    // Java Locale: "en", "ja", "fr", ...
  char country[16];     // Java Locale country: "US", "JP", ...
  char capture_at[96];  // debug: seconds (from the first frame) at which to save capture_<s>.bmp
  char dump_threads_at[96]; // debug: seconds at which to log where every thread is
  char test_script[160];// debug: input script file, "" = none
  char brk_unity[160];  // debug: hex offsets in libunity.so to trap at (emulator bisecting)
  char boot_extra[160]; // debug: more boot.config lines, ';' between them ("job-worker-count=3;gfx-jobs-sync=1")
} Config;

extern Config config;

int read_config(const char *file);
int write_config(const char *file);

#endif
