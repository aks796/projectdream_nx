/* opensles.c -- OpenSL ES for Wwise (and anything else that asks): an engine,
 * output mixes, and buffer-queue audio players, mixed by one thread into the
 * console's audio output (audio.c, audout at 48 kHz stereo).
 *
 * Wwise's Android sink makes one player fed through an Android simple buffer
 * queue: it enqueues a few buffers, sets PLAYING, and refills from the queue
 * callback. The mixer plays the head buffer of every playing player,
 * resampling to 48 kHz, and calls the callback each time a buffer finishes --
 * on the mixer thread, as OpenSL's own internal thread would.
 *
 * Interface pointers follow OpenSL's shape: an interface is a pointer to a
 * pointer to its function table, so each object keeps one "itf" field per
 * interface and the functions find the object from it.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <math.h>
#include <switch.h>

#include "audio.h"
#include "log.h"
#include "pthr.h"
#include "so_util.h"

typedef uint32_t SLuint32;
typedef int32_t SLint32;
typedef int16_t SLint16;
typedef uint32_t SLresult;
typedef uint32_t SLboolean;
typedef int16_t SLmillibel;
typedef const void *SLInterfaceID;

#define SL_RESULT_SUCCESS 0
#define SL_RESULT_PARAMETER_INVALID 2
#define SL_RESULT_MEMORY_FAILURE 3
#define SL_RESULT_BUFFER_INSUFFICIENT 7
#define SL_RESULT_CONTENT_UNSUPPORTED 9
#define SL_RESULT_FEATURE_UNSUPPORTED 12

#define SL_OBJECT_STATE_REALIZED 2
#define SL_PLAYSTATE_STOPPED 1
#define SL_PLAYSTATE_PAUSED 2
#define SL_PLAYSTATE_PLAYING 3

#define SL_DATALOCATOR_BUFFERQUEUE 6
#define SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE 0x800007BD
#define SL_DATAFORMAT_PCM 2
#define SL_ANDROID_DATAFORMAT_PCM_EX 4
#define SL_ANDROID_PCM_REPRESENTATION_FLOAT 3

// interface ids: their addresses are the ids
#define IID(n) static const char iid_##n = 0; const void *const SL_IID_##n = &iid_##n
IID(ENGINE); IID(OBJECT); IID(PLAY); IID(BUFFERQUEUE); IID(ANDROIDSIMPLEBUFFERQUEUE);
IID(VOLUME); IID(ANDROIDCONFIGURATION); IID(OUTPUTMIX); IID(SEEK); IID(PLAYBACKRATE);
IID(EFFECTSEND); IID(PREFETCHSTATUS); IID(METADATAEXTRACTION); IID(MUTESOLO); IID(NULL);
IID(ENVIRONMENTALREVERB); IID(PRESETREVERB); IID(EQUALIZER); IID(BASSBOOST); IID(VIRTUALIZER);
IID(ANDROIDEFFECT); IID(ANDROIDEFFECTSEND); IID(ANDROIDEFFECTCAPABILITIES); IID(RECORD);
IID(DYNAMICINTERFACEMANAGEMENT); IID(ENGINECAPABILITIES); IID(AUDIOIODEVICECAPABILITIES);
IID(DEVICEVOLUME); IID(THREADSYNC); IID(ANDROIDBUFFERQUEUESOURCE); IID(ANDROIDACOUSTICECHOCANCELLATION);
#undef IID

typedef void (*BqCallback)(void *bq, void *ctx);

#define MAX_QUEUE 16

typedef struct Player {
  const void *obj_itf, *play_itf, *bq_itf, *vol_itf, *cfg_itf, *seek_itf, *rate_itf;
  struct Player *next;
  // format
  int channels, rate, bits, is_float;
  // queue
  const uint8_t *q_buf[MAX_QUEUE];
  uint32_t q_size[MAX_QUEUE];
  int q_head, q_count;
  uint32_t played;      // buffers finished so far (the queue's index)
  double pos;           // read position in the head buffer, in input frames
  BqCallback cb;
  void *cb_ctx;
  uint32_t state;
  float gain;
  int muted;
  uint64_t frames_out;
} Player;

typedef struct {
  const void *obj_itf, *eng_itf;
} Engine;

typedef struct {
  const void *obj_itf, *mix_itf;
} OutputMix;

#define OBJ(T, field, self) ((T *)((char *)(self) - offsetof(T, field)))

static Mutex s_lock;
static Player *s_players;
static Thread s_thread;
static int s_running;

// --- the mixer ---------------------------------------------------------------------------

#define MIX_FRAMES 512
#define OUT_RATE 48000

static inline float sample_at(const Player *p, const uint8_t *buf, long frame, int ch) {
  const int c = ch < p->channels ? ch : p->channels - 1;
  const long k = frame * p->channels + c;
  if (p->is_float)
    return ((const float *)buf)[k];
  if (p->bits == 8)
    return ((int)buf[k] - 128) / 128.0f;
  if (p->bits == 32)
    return ((const int32_t *)buf)[k] / 2147483648.0f;
  return ((const int16_t *)buf)[k] / 32768.0f;
}

// mix one player into acc (stereo float); returns the callbacks to make
static int mix_player(Player *p, float *acc, int frames, BqCallback *cbs, void **ctxs, int maxcb) {
  int ncb = 0;
  const int bytes_per_frame = p->channels * (p->bits / 8);
  const double step = (double)p->rate / OUT_RATE;
  const float g = p->muted ? 0.f : p->gain;
  int i = 0;
  while (i < frames && p->q_count > 0) {
    const uint8_t *buf = p->q_buf[p->q_head];
    const long nfr = (long)(p->q_size[p->q_head] / (uint32_t)bytes_per_frame);
    while (i < frames && p->pos < (double)nfr) {
      const long f0 = (long)p->pos;
      const float t = (float)(p->pos - (double)f0);
      const long f1 = f0 + 1 < nfr ? f0 + 1 : f0;
      const float l = sample_at(p, buf, f0, 0) * (1.f - t) + sample_at(p, buf, f1, 0) * t;
      const float r = sample_at(p, buf, f0, 1) * (1.f - t) + sample_at(p, buf, f1, 1) * t;
      acc[i * 2] += l * g;
      acc[i * 2 + 1] += r * g;
      i++;
      p->pos += step;
    }
    if (p->pos >= (double)nfr) {
      p->pos -= (double)nfr;
      p->q_head = (p->q_head + 1) % MAX_QUEUE;
      p->q_count--;
      p->played++;
      if (p->cb && ncb < maxcb) {
        cbs[ncb] = p->cb;
        ctxs[ncb] = p->cb_ctx;
        ncb++;
      }
    }
  }
  p->frames_out += (uint64_t)i;
  return ncb;
}

static void mixer_thread(void *arg) {
  (void)arg;
  pthr_ensure_fake_tls();
  static float acc[MIX_FRAMES * 2];
  static int16_t out[MIX_FRAMES * 2];
  while (s_running) {
    memset(acc, 0, sizeof acc);
    struct { BqCallback cb; void *bq; void *ctx; } calls[32];
    int ncalls = 0;
    int active = 0;
    mutexLock(&s_lock);
    for (Player *p = s_players; p; p = p->next) {
      if (p->state != SL_PLAYSTATE_PLAYING)
        continue;
      active++;
      BqCallback cbs[8];
      void *ctxs[8];
      const int n = mix_player(p, acc, MIX_FRAMES, cbs, ctxs, 8);
      for (int k = 0; k < n && ncalls < 32; k++) {
        calls[ncalls].cb = cbs[k];
        calls[ncalls].bq = &p->bq_itf;
        calls[ncalls].ctx = ctxs[k];
        ncalls++;
      }
    }
    mutexUnlock(&s_lock);
    // the queue callbacks enqueue more: outside the lock
    for (int k = 0; k < ncalls; k++)
      calls[k].cb(calls[k].bq, calls[k].ctx);
    for (int i = 0; i < MIX_FRAMES * 2; i++) {
      float v = acc[i];
      if (v > 1.f) v = 1.f;
      if (v < -1.f) v = -1.f;
      out[i] = (int16_t)(v * 32767.f);
    }
    // blocks until the output takes it: this paces the mixer
    nx_audio_write(out, MIX_FRAMES * 2);
    (void)active;
  }
}

static void start_mixer(void) {
  if (s_running)
    return;
  if (!nx_audio_init(OUT_RATE, 2))
    return;
  s_running = 1;
  // a high priority, off the render core
  if (R_SUCCEEDED(threadCreate(&s_thread, mixer_thread, NULL, NULL, 0x10000, 0x20, 2)))
    threadStart(&s_thread);
  else
    s_running = 0;
  log_printf("[sles] mixer %s", s_running ? "running" : "failed to start");
}

// --- Object -----------------------------------------------------------------------------

static SLresult obj_Realize(void *self, SLboolean async) { (void)self; (void)async; return 0; }
static SLresult obj_Resume(void *self, SLboolean async) { (void)self; (void)async; return 0; }
static SLresult obj_GetState(void *self, SLuint32 *s) { (void)self; if (s) *s = SL_OBJECT_STATE_REALIZED; return 0; }
static SLresult obj_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return 0; }
static SLresult obj_Abort(void *self) { (void)self; return 0; }
static SLresult obj_SetPriority(void *self, SLint32 p, SLboolean pre) { (void)self; (void)p; (void)pre; return 0; }
static SLresult obj_GetPriority(void *self, SLint32 *p) { (void)self; if (p) *p = 0; return 0; }
static SLresult obj_SetLoss(void *self, SLint32 n, const SLInterfaceID *ids, SLboolean e) {
  (void)self; (void)n; (void)ids; (void)e;
  return 0;
}

// --- Player interfaces ---------------------------------------------------------------------

static SLresult play_SetPlayState(void *self, SLuint32 state) {
  Player *p = OBJ(Player, play_itf, self);
  mutexLock(&s_lock);
  if (state == SL_PLAYSTATE_STOPPED) {
    // stopping returns the queue to empty
    p->q_count = 0;
    p->q_head = 0;
    p->pos = 0;
  }
  p->state = state;
  mutexUnlock(&s_lock);
  if (state == SL_PLAYSTATE_PLAYING)
    start_mixer();
  return 0;
}
static SLresult play_GetPlayState(void *self, SLuint32 *s) {
  if (s) *s = OBJ(Player, play_itf, self)->state;
  return 0;
}
static SLresult play_GetDuration(void *self, SLuint32 *ms) { (void)self; if (ms) *ms = 0xffffffffu; return 0; }
static SLresult play_GetPosition(void *self, SLuint32 *ms) {
  Player *p = OBJ(Player, play_itf, self);
  if (ms) *ms = (SLuint32)(p->frames_out * 1000 / OUT_RATE);
  return 0;
}
static SLresult play_RegisterCallback(void *self, void *cb, void *ctx) { (void)self; (void)cb; (void)ctx; return 0; }
static SLresult play_SetMask(void *self, SLuint32 m) { (void)self; (void)m; return 0; }
static SLresult play_GetMask(void *self, SLuint32 *m) { (void)self; if (m) *m = 0; return 0; }
static SLresult play_SetU32(void *self, SLuint32 v) { (void)self; (void)v; return 0; }
static SLresult play_GetU32(void *self, SLuint32 *v) { (void)self; if (v) *v = 0; return 0; }
static SLresult play_Clear(void *self) { (void)self; return 0; }

static const void *const play_vt[] = {
  play_SetPlayState, play_GetPlayState, play_GetDuration, play_GetPosition, play_RegisterCallback,
  play_SetMask, play_GetMask, play_SetU32, play_Clear, play_GetU32, play_SetU32, play_GetU32,
};

static SLresult bq_Enqueue(void *self, const void *buf, SLuint32 size) {
  Player *p = OBJ(Player, bq_itf, self);
  mutexLock(&s_lock);
  if (p->q_count >= MAX_QUEUE) {
    mutexUnlock(&s_lock);
    return SL_RESULT_BUFFER_INSUFFICIENT;
  }
  const int slot = (p->q_head + p->q_count) % MAX_QUEUE;
  p->q_buf[slot] = buf;
  p->q_size[slot] = size;
  p->q_count++;
  mutexUnlock(&s_lock);
  return 0;
}
static SLresult bq_Clear(void *self) {
  Player *p = OBJ(Player, bq_itf, self);
  mutexLock(&s_lock);
  p->q_count = 0;
  p->q_head = 0;
  p->pos = 0;
  mutexUnlock(&s_lock);
  return 0;
}
static SLresult bq_GetState(void *self, SLuint32 *state) {
  Player *p = OBJ(Player, bq_itf, self);
  if (state) {
    state[0] = (SLuint32)p->q_count;
    state[1] = p->played;
  }
  return 0;
}
static SLresult bq_RegisterCallback(void *self, BqCallback cb, void *ctx) {
  Player *p = OBJ(Player, bq_itf, self);
  mutexLock(&s_lock);
  p->cb = cb;
  p->cb_ctx = ctx;
  mutexUnlock(&s_lock);
  return 0;
}
static const void *const bq_vt[] = { bq_Enqueue, bq_Clear, bq_GetState, bq_RegisterCallback };

static SLresult vol_SetVolumeLevel(void *self, SLmillibel mb) {
  Player *p = OBJ(Player, vol_itf, self);
  p->gain = mb <= -9600 ? 0.f : powf(10.f, mb / 2000.f);
  return 0;
}
static SLresult vol_GetVolumeLevel(void *self, SLmillibel *mb) {
  Player *p = OBJ(Player, vol_itf, self);
  if (mb) *mb = p->gain <= 0.f ? -9600 : (SLmillibel)(2000.f * log10f(p->gain));
  return 0;
}
static SLresult vol_GetMax(void *self, SLmillibel *mb) { (void)self; if (mb) *mb = 0; return 0; }
static SLresult vol_SetMute(void *self, SLboolean m) { OBJ(Player, vol_itf, self)->muted = m != 0; return 0; }
static SLresult vol_GetMute(void *self, SLboolean *m) { if (m) *m = OBJ(Player, vol_itf, self)->muted; return 0; }
static SLresult vol_Enable(void *self, SLboolean e) { (void)self; (void)e; return 0; }
static SLresult vol_IsEnabled(void *self, SLboolean *e) { (void)self; if (e) *e = 0; return 0; }
static SLresult vol_SetPos(void *self, SLint16 v) { (void)self; (void)v; return 0; }
static SLresult vol_GetPos(void *self, SLint16 *v) { (void)self; if (v) *v = 0; return 0; }
static const void *const vol_vt[] = { vol_SetVolumeLevel, vol_GetVolumeLevel, vol_GetMax, vol_SetMute,
                                      vol_GetMute, vol_Enable, vol_IsEnabled, vol_SetPos, vol_GetPos };

static SLresult cfg_Set(void *self, const char *key, const void *val, SLuint32 size) {
  (void)self; (void)val; (void)size;
  log_printf("[sles] SetConfiguration(%s)", key ? key : "");
  return 0;
}
static SLresult cfg_Get(void *self, const char *key, SLuint32 *size, void *val) {
  (void)self; (void)key; (void)size; (void)val;
  return SL_RESULT_PARAMETER_INVALID;
}
static SLresult cfg_Acquire(void *self, SLuint32 t, void **proxy) { (void)self; (void)t; if (proxy) *proxy = NULL; return SL_RESULT_FEATURE_UNSUPPORTED; }
static SLresult cfg_Release(void *self, SLuint32 t) { (void)self; (void)t; return 0; }
static const void *const cfg_vt[] = { cfg_Set, cfg_Get, cfg_Acquire, cfg_Release };

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void *out);
static void player_Destroy(void *self);
static const void *const player_obj_vt[] = {
  obj_Realize, obj_Resume, obj_GetState, player_GetInterface, obj_RegisterCallback, obj_Abort,
  player_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLoss,
};

static SLresult player_GetInterface(void *self, SLInterfaceID iid, void *out) {
  Player *p = OBJ(Player, obj_itf, self);
  void **o = out;
  if (iid == SL_IID_PLAY) *o = &p->play_itf;
  else if (iid == SL_IID_BUFFERQUEUE || iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE) *o = &p->bq_itf;
  else if (iid == SL_IID_VOLUME) *o = &p->vol_itf;
  else if (iid == SL_IID_ANDROIDCONFIGURATION) *o = &p->cfg_itf;
  else if (iid == SL_IID_OBJECT) *o = &p->obj_itf;
  else {
    *o = NULL;
    return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return 0;
}

static void player_Destroy(void *self) {
  Player *p = OBJ(Player, obj_itf, self);
  mutexLock(&s_lock);
  for (Player **pp = &s_players; *pp; pp = &(*pp)->next)
    if (*pp == p) {
      *pp = p->next;
      break;
    }
  mutexUnlock(&s_lock);
  free(p);
}

// --- OutputMix / Engine -------------------------------------------------------------------

static SLresult mix_GetInterface(void *self, SLInterfaceID iid, void *out) {
  OutputMix *m = OBJ(OutputMix, obj_itf, self);
  if (iid == SL_IID_OUTPUTMIX || iid == SL_IID_OBJECT) {
    *(void **)out = iid == SL_IID_OBJECT ? (void *)&m->obj_itf : (void *)&m->mix_itf;
    return 0;
  }
  *(void **)out = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void mix_Destroy(void *self) { free(OBJ(OutputMix, obj_itf, self)); }
static const void *const mix_obj_vt[] = {
  obj_Realize, obj_Resume, obj_GetState, mix_GetInterface, obj_RegisterCallback, obj_Abort,
  mix_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLoss,
};
static SLresult unsupported(void) { return SL_RESULT_FEATURE_UNSUPPORTED; }
static const void *const mix_vt[] = { unsupported, unsupported, unsupported, unsupported, unsupported };

typedef struct { SLuint32 type; SLuint32 n; } Locator;
typedef struct { void *locator; void *format; } DataSrc;

static SLresult eng_CreateAudioPlayer(void *self, void **pplayer, DataSrc *src, DataSrc *snk,
                                      SLuint32 n, const SLInterfaceID *ids, const SLboolean *req) {
  (void)self; (void)snk; (void)n; (void)ids; (void)req;
  if (!pplayer || !src || !src->locator || !src->format)
    return SL_RESULT_PARAMETER_INVALID;
  const Locator *loc = src->locator;
  if (loc->type != SL_DATALOCATOR_BUFFERQUEUE && loc->type != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE) {
    log_printf("[sles] audio player with data locator 0x%x: unsupported", loc->type);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  const SLuint32 *fmt = src->format;
  if (fmt[0] != SL_DATAFORMAT_PCM && fmt[0] != SL_ANDROID_DATAFORMAT_PCM_EX) {
    log_printf("[sles] audio player with data format %u: unsupported", fmt[0]);
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  Player *p = calloc(1, sizeof(Player));
  if (!p)
    return SL_RESULT_MEMORY_FAILURE;
  p->obj_itf = player_obj_vt;
  p->play_itf = play_vt;
  p->bq_itf = bq_vt;
  p->vol_itf = vol_vt;
  p->cfg_itf = cfg_vt;
  p->channels = fmt[1] ? (int)fmt[1] : 2;
  p->rate = (int)(fmt[2] / 1000); // milliHz
  if (p->rate <= 0)
    p->rate = OUT_RATE;
  p->bits = (int)fmt[3];
  p->is_float = fmt[0] == SL_ANDROID_DATAFORMAT_PCM_EX && fmt[7] == SL_ANDROID_PCM_REPRESENTATION_FLOAT;
  p->state = SL_PLAYSTATE_STOPPED;
  p->gain = 1.f;
  mutexLock(&s_lock);
  p->next = s_players;
  s_players = p;
  mutexUnlock(&s_lock);
  log_printf("[sles] audio player: %d ch, %d Hz, %d-bit%s, %u queue buffers", p->channels, p->rate,
             p->bits, p->is_float ? " float" : "", loc->n);
  *pplayer = &p->obj_itf;
  return 0;
}

static SLresult eng_CreateOutputMix(void *self, void **pmix, SLuint32 n, const SLInterfaceID *ids,
                                    const SLboolean *req) {
  (void)self; (void)n; (void)ids; (void)req;
  OutputMix *m = calloc(1, sizeof(OutputMix));
  m->obj_itf = mix_obj_vt;
  m->mix_itf = mix_vt;
  *pmix = &m->obj_itf;
  return 0;
}

static SLresult eng_QueryNum(void *self, SLuint32 *n) { (void)self; if (n) *n = 0; return 0; }
static SLresult eng_IsExt(void *self, const char *e, SLboolean *r) { (void)self; (void)e; if (r) *r = 0; return 0; }

static const void *const eng_vt[] = {
  unsupported,           // CreateLEDDevice
  unsupported,           // CreateVibraDevice
  eng_CreateAudioPlayer,
  unsupported,           // CreateAudioRecorder
  unsupported,           // CreateMidiPlayer
  unsupported,           // CreateListener
  unsupported,           // Create3DGroup
  eng_CreateOutputMix,
  unsupported,           // CreateMetadataExtractor
  unsupported,           // CreateExtensionObject
  eng_QueryNum,          // QueryNumSupportedInterfaces
  unsupported,           // QuerySupportedInterfaces
  eng_QueryNum,          // QueryNumSupportedExtensions
  unsupported,           // QuerySupportedExtension
  eng_IsExt,             // IsExtensionSupported
};

static SLresult engine_GetInterface(void *self, SLInterfaceID iid, void *out) {
  Engine *e = OBJ(Engine, obj_itf, self);
  if (iid == SL_IID_ENGINE) {
    *(void **)out = &e->eng_itf;
    return 0;
  }
  if (iid == SL_IID_OBJECT) {
    *(void **)out = &e->obj_itf;
    return 0;
  }
  *(void **)out = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}
static void engine_Destroy(void *self) { (void)self; }
static const void *const engine_obj_vt[] = {
  obj_Realize, obj_Resume, obj_GetState, engine_GetInterface, obj_RegisterCallback, obj_Abort,
  engine_Destroy, obj_SetPriority, obj_GetPriority, obj_SetLoss,
};

static Engine s_engine = { engine_obj_vt, eng_vt };

static SLresult slCreateEngine(void **pengine, SLuint32 nopts, const void *opts, SLuint32 n,
                               const SLInterfaceID *ids, const SLboolean *req) {
  (void)nopts; (void)opts; (void)n; (void)ids; (void)req;
  static int once;
  if (!once++) {
    mutexInit(&s_lock);
    log_printf("[sles] engine created");
  }
  *pengine = &s_engine.obj_itf;
  return 0;
}

static SLresult slQueryNumSupportedEngineInterfaces(SLuint32 *n) { if (n) *n = 0; return 0; }

DynLibFunction sles_functions[] = {
  { "slCreateEngine", (uintptr_t)&slCreateEngine },
  { "slQueryNumSupportedEngineInterfaces", (uintptr_t)&slQueryNumSupportedEngineInterfaces },
  { "SL_IID_ENGINE", (uintptr_t)&SL_IID_ENGINE },
  { "SL_IID_OBJECT", (uintptr_t)&SL_IID_OBJECT },
  { "SL_IID_PLAY", (uintptr_t)&SL_IID_PLAY },
  { "SL_IID_BUFFERQUEUE", (uintptr_t)&SL_IID_BUFFERQUEUE },
  { "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", (uintptr_t)&SL_IID_ANDROIDSIMPLEBUFFERQUEUE },
  { "SL_IID_VOLUME", (uintptr_t)&SL_IID_VOLUME },
  { "SL_IID_ANDROIDCONFIGURATION", (uintptr_t)&SL_IID_ANDROIDCONFIGURATION },
  { "SL_IID_OUTPUTMIX", (uintptr_t)&SL_IID_OUTPUTMIX },
  { "SL_IID_SEEK", (uintptr_t)&SL_IID_SEEK },
  { "SL_IID_PLAYBACKRATE", (uintptr_t)&SL_IID_PLAYBACKRATE },
  { "SL_IID_EFFECTSEND", (uintptr_t)&SL_IID_EFFECTSEND },
  { "SL_IID_PREFETCHSTATUS", (uintptr_t)&SL_IID_PREFETCHSTATUS },
  { "SL_IID_METADATAEXTRACTION", (uintptr_t)&SL_IID_METADATAEXTRACTION },
  { "SL_IID_MUTESOLO", (uintptr_t)&SL_IID_MUTESOLO },
  { "SL_IID_NULL", (uintptr_t)&SL_IID_NULL },
  { "SL_IID_ENVIRONMENTALREVERB", (uintptr_t)&SL_IID_ENVIRONMENTALREVERB },
  { "SL_IID_PRESETREVERB", (uintptr_t)&SL_IID_PRESETREVERB },
  { "SL_IID_EQUALIZER", (uintptr_t)&SL_IID_EQUALIZER },
  { "SL_IID_BASSBOOST", (uintptr_t)&SL_IID_BASSBOOST },
  { "SL_IID_VIRTUALIZER", (uintptr_t)&SL_IID_VIRTUALIZER },
  { "SL_IID_ANDROIDEFFECT", (uintptr_t)&SL_IID_ANDROIDEFFECT },
  { "SL_IID_ANDROIDEFFECTSEND", (uintptr_t)&SL_IID_ANDROIDEFFECTSEND },
  { "SL_IID_ANDROIDEFFECTCAPABILITIES", (uintptr_t)&SL_IID_ANDROIDEFFECTCAPABILITIES },
  { "SL_IID_RECORD", (uintptr_t)&SL_IID_RECORD },
  { "SL_IID_DYNAMICINTERFACEMANAGEMENT", (uintptr_t)&SL_IID_DYNAMICINTERFACEMANAGEMENT },
  { "SL_IID_ENGINECAPABILITIES", (uintptr_t)&SL_IID_ENGINECAPABILITIES },
  { "SL_IID_AUDIOIODEVICECAPABILITIES", (uintptr_t)&SL_IID_AUDIOIODEVICECAPABILITIES },
  { "SL_IID_DEVICEVOLUME", (uintptr_t)&SL_IID_DEVICEVOLUME },
  { "SL_IID_THREADSYNC", (uintptr_t)&SL_IID_THREADSYNC },
  { "SL_IID_ANDROIDBUFFERQUEUESOURCE", (uintptr_t)&SL_IID_ANDROIDBUFFERQUEUESOURCE },
  { "SL_IID_ANDROIDACOUSTICECHOCANCELLATION", (uintptr_t)&SL_IID_ANDROIDACOUSTICECHOCANCELLATION },
};

size_t sles_numfunctions = sizeof(sles_functions) / sizeof(*sles_functions);
