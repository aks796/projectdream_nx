/* java_sdt.c -- Java classes this port's game needs beyond mkt_nx's
 *
 * org.fmod.FMODAudioDevice: Unity's FMOD (Android) plays through a Java
 * AudioTrack when it does not use OpenSL ES or AAudio. start() runs FMOD's
 * loop (FMODAudioDevice.run): ask fmodGetInfo for the format (0 rate, 1 block
 * length in frames, 4 channels: 1 mono, else stereo), then while fmodGetInfo(3)
 * says it runs, fmodProcess fills a direct ByteBuffer with one block of 16-bit
 * PCM, which goes to the console's audio output (audio.c) instead of an
 * AudioTrack. That write blocks while the output's queue is full, which paces
 * FMOD as AudioTrack.write would.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include "audio.h"
#include "jni.h"
#include "log.h"
#include "pthr.h"

#define FMOD "org/fmod/FMODAudioDevice"

static Thread s_thread;
static volatile int s_running;
static JObj *s_device;

static void fmod_loop(void *arg) {
  (void)arg;
  pthr_ensure_fake_tls();
  int32_t (*info)(void *env, JObj *thiz, int32_t what) = jni_native(FMOD, "fmodGetInfo");
  int32_t (*process)(void *env, JObj *thiz, JObj *buf) = jni_native(FMOD, "fmodProcess");
  if (!info || !process) {
    log_printf("[fmod] FMODAudioDevice's natives are not registered: no sound");
    return;
  }
  void *env = jni_env();
  const int rate = info(env, s_device, 0), block = info(env, s_device, 1);
  const int channels = info(env, s_device, 4) == 1 ? 1 : 2;
  if (rate <= 0 || block <= 0) {
    log_printf("[fmod] FMOD gave no format (rate %d, block %d): no sound", rate, block);
    return;
  }
  const size_t bytes = (size_t)block * channels * 2;
  void *mem = memalign(0x1000, (bytes + 0xfff) & ~(size_t)0xfff);
  JObj *buf = jni_direct_buffer(mem, (int64_t)bytes);
  if (!mem || !buf || !nx_audio_init(rate, channels)) {
    log_printf("[fmod] no audio output: no sound");
    return;
  }
  log_printf("[fmod] FMOD's output: %d Hz, %d channel(s), %d frames a block", rate, channels, block);
  while (s_running) {
    if (info(env, s_device, 3) != 1) { // (not running yet / paused)
      svcSleepThread(5000000);
      continue;
    }
    process(env, s_device, buf);
    nx_audio_write(mem, block * channels);
  }
  free(mem);
}

static jvalue fmod_start(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)m;
  (void)a;
  if (s_running)
    return (jvalue){ 0 };
  if (getenv("SDT_NO_FMOD")) { // (debug: config fmod_off)
    log_printf("[fmod] off (debug)");
    return (jvalue){ 0 };
  }
  s_device = thiz;
  s_running = 1;
  // (high priority: FMOD's mixer is what keeps the sound from breaking up). On cores 1-2, not the default core 0:
  // that core is the game's main thread's alone (pthr.c), and this loop -- all of FMOD's mixing, at a priority above
  // the main thread's -- took its time out of the main thread's frames there (hardware round 9: the main thread
  // runnable ~95% of a frame, running ~80%).
  if (R_FAILED(threadCreate(&s_thread, fmod_loop, NULL, NULL, 0x20000, 0x2B, 1)) ||
      R_FAILED(svcSetThreadCoreMask(s_thread.handle, 1, 0x6)) || R_FAILED(threadStart(&s_thread))) {
    s_running = 0;
    log_printf("[fmod] its thread did not start: no sound");
  } else {
    pthr_cpu_watch(s_thread.handle, "FMOD mixer (port)");
  }
  return (jvalue){ 0 };
}

static jvalue fmod_stop(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)thiz;
  (void)m;
  (void)a;
  if (s_running) {
    s_running = 0;
    threadWaitForExit(&s_thread);
    threadClose(&s_thread);
  }
  return (jvalue){ 0 };
}

static jvalue fmod_running(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)thiz;
  (void)m;
  (void)a;
  return (jvalue){ .z = (uint8_t)(s_running != 0) };
}

static jvalue fmod_record(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)thiz;
  (void)m;
  (void)a;
  return (jvalue){ .i = -1 }; // (no microphone)
}

static jvalue fmod_void(JObj *thiz, JMethod *m, const jvalue *a) {
  (void)thiz;
  (void)m;
  (void)a;
  return (jvalue){ 0 };
}

void java_sdt_init(void) {
  jni_method(FMOD, "start", "()V", fmod_start);
  jni_method(FMOD, "stop", "()V", fmod_stop);
  jni_method(FMOD, "close", "()V", fmod_stop);
  jni_method(FMOD, "isRunning", "()Z", fmod_running);
  jni_method(FMOD, "startAudioRecord", "(III)I", fmod_record);
  jni_method(FMOD, "stopAudioRecord", "()V", fmod_void);
}
