/* audio.c -- see audio.h. FMOD's mixed PCM straight to the console's audio
 * output service (audout); the FMOD mixer thread is the only writer, and
 * waiting for a buffer to come back paces it like a blocking Android
 * AudioTrack.
 *
 * Not SDL: its Switch backend (audren) waits in a loop for the buffer it just
 * queued to start playing, and once its thread had been held off long
 * enough for both buffers to run dry (a heavy load: Globed preloading its
 * icons) the voice stopped and that wait never ended -- the game was silent
 * from then on (hardware, and now and then Ryujinx). audout plays silence
 * when it runs dry and carries on when buffers come.
 *
 * audout plays 48 kHz stereo; FMOD mixes at what the game asks for (44.1 kHz
 * here): linear resampling, its position carried from one write to the next.
 * Opening, the first samples, and output that stops taking buffers (and
 * starts again) go to debug.log.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <malloc.h>
#include <string.h>
#include <switch.h>

#include "audio.h"
#include "log.h"

#define NBUF 4
#define BUF_FRAMES 1024 // 21 ms at 48 kHz: ~85 ms queued at most
#define OUT_RATE 48000

static AudioOutBuffer bufs[NBUF];
static int queued[NBUF];
static int fill = -1;       // the buffer being filled
static u32 fill_frames;
static int s_inited;
static int s_in_rate = 44100, s_in_ch = 2;
static double s_step;       // input frames per output frame
static double s_t = -1.0;   // next output frame's position, in input frames from this write's first
static int16_t s_prev[2];   // the last input frame of the previous write
static u64 s_written;

int nx_audio_init(int rate, int channels) {
  if (s_inited)
    return 1;
  s_in_rate = rate > 0 ? rate : 44100;
  s_in_ch = channels == 1 ? 1 : 2;
  s_step = (double)s_in_rate / OUT_RATE;

  Result rc = 0;
  for (int attempt = 0; attempt < 5; attempt++) {
    if (attempt)
      svcSleepThread(250000000ull);
    rc = audoutInitialize();
    if (R_SUCCEEDED(rc)) {
      rc = audoutStartAudioOut();
      if (R_SUCCEEDED(rc))
        break;
      audoutExit();
    }
    log_printf("[audio] audio output did not open (try %d): 0x%x", attempt + 1, rc);
  }
  if (R_FAILED(rc)) {
    log_printf("[audio] no audio this session: the output would not open");
    return 0;
  }
  const size_t size = (BUF_FRAMES * 2 * sizeof(int16_t) + 0xfff) & ~(size_t)0xfff;
  for (int i = 0; i < NBUF; i++) {
    memset(&bufs[i], 0, sizeof(bufs[i]));
    bufs[i].buffer = memalign(0x1000, size);
    if (!bufs[i].buffer) {
      log_printf("[audio] out of memory for the output buffers");
      audoutStopAudioOut();
      audoutExit();
      return 0;
    }
    memset(bufs[i].buffer, 0, size);
    bufs[i].buffer_size = size;
    queued[i] = 0;
  }
  s_inited = 1;
  log_printf("[audio] output open: %u Hz, %u ch (the game mixes %d Hz, %d ch)", audoutGetSampleRate(),
             audoutGetChannelCount(), s_in_rate, s_in_ch);
  return 1;
}

static void mark_released(const AudioOutBuffer *b) {
  for (int i = 0; i < NBUF; i++)
    if (b == &bufs[i])
      queued[i] = 0;
}

// a buffer to fill: a free one, else the next the output gives back
static int take_buffer(void) {
  u64 waited = 0;
  int quiet = 0;
  for (;;) {
    for (int i = 0; i < NBUF; i++)
      if (!queued[i])
        return i;
    AudioOutBuffer *rel = NULL;
    u32 count = 0;
    const Result rc = audoutWaitPlayFinish(&rel, &count, 200000000ull); // 200 ms
    if (R_SUCCEEDED(rc) && count && rel) {
      mark_released(rel);
      while (R_SUCCEEDED(audoutGetReleasedAudioOutBuffer(&rel, &count)) && count && rel)
        mark_released(rel);
      if (quiet)
        log_printf("[audio] output playing again after %llu ms", (unsigned long long)waited);
      waited = 0;
      quiet = 0;
    } else if ((waited += 200) >= 2000 && !quiet) {
      // (the game in the background: the output pauses, and so does this)
      quiet = 1;
      log_printf("[audio] output took no buffer for 2 s (0x%x)", rc);
    }
  }
}

static void put_frame(int16_t l, int16_t r) {
  if (fill < 0) {
    fill = take_buffer();
    fill_frames = 0;
  }
  int16_t *out = (int16_t *)bufs[fill].buffer + fill_frames * 2;
  out[0] = l;
  out[1] = r;
  // pacing check, once: 10 s of output should take 10 s
  static u64 frames, start;
  if (!frames++)
    start = armGetSystemTick();
  else if (frames == 10 * OUT_RATE)
    log_printf("[audio] 10 s of sound took %llu ms", (unsigned long long)(armTicksToNs(armGetSystemTick() - start) / 1000000));
  if (++fill_frames == BUF_FRAMES) {
    bufs[fill].data_size = BUF_FRAMES * 2 * sizeof(int16_t);
    bufs[fill].data_offset = 0;
    armDCacheFlush(bufs[fill].buffer, bufs[fill].data_size);
    if (R_SUCCEEDED(audoutAppendAudioOutBuffer(&bufs[fill])))
      queued[fill] = 1;
    fill = -1;
  }
}

void nx_audio_write(const void *pcm, int n_shorts) {
  if (!s_inited || !pcm || n_shorts <= 0)
    return;
  if (s_written == 0)
    log_printf("[audio] first samples");
  s_written += (u64)n_shorts * 2u;
  const int16_t *in = pcm;
  const int n = n_shorts / s_in_ch; // input frames
  if (n <= 0)
    return;
  // input frame i (-1: the last of the previous write)
#define L(i) ((i) < 0 ? s_prev[0] : in[(i) * s_in_ch])
#define R(i) ((i) < 0 ? s_prev[1] : in[(i) * s_in_ch + s_in_ch - 1])
  double t = s_t;
  while (t < (double)(n - 1)) {
    const int i = t < 0 ? -1 : (int)t;
    const double f = t - (double)i;
    const int l = (int)((double)L(i) + ((double)L(i + 1) - (double)L(i)) * f);
    const int r = (int)((double)R(i) + ((double)R(i + 1) - (double)R(i)) * f);
    put_frame((int16_t)l, (int16_t)r);
    t += s_step;
  }
  s_prev[0] = L(n - 1);
  s_prev[1] = R(n - 1);
#undef L
#undef R
  s_t = t - (double)n; // into the next write, from its first frame (>= -1)
}
