/* media_ndk.c -- libmediandk.so for Unity's VideoPlayer: AMediaExtractor, AMediaCodec, AMediaFormat and
 * AMediaDataSource over FFmpeg (libavformat demuxes, libavcodec decodes on the CPU)
 *
 * Unity's Android player decodes video with the NDK's MediaCodec. Under Vulkan it wants AImageReader hardware
 * buffers, unless AndroidVulkanVideo::IsSupported() says no (media_ndk_init makes it say no): then it takes
 * "raw" output buffers -- YUV 4:2:0 planes, color-format 19 -- uploads them as textures and converts them with
 * its own Hidden/VideoDecode shader (Project Dream's shader pack has its Vulkan programs). So the decoder here
 * hands out I420 frames, and PCM 16 for audio (Unity mixes a video's sound itself).
 *
 * What Sonic Dream Team plays this way: the intro movie (1920x1080 H.264 + AAC, 24 fps), a looping clip in the
 * menus (850x850 H.264 + AAC, 60 fps) and the Apple Arcade splash (which the game's patches skip).
 *
 * Calls come from Unity's decode thread (extractor and codec) and its main thread (formats); each object has
 * its own lock.
 *
 * This software may be modified and distributed under the terms
 * of the MIT license.  See the LICENSE file for details.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>

#include "apkfile.h"
#include "config.h"
#include "jni.h"
#include "log.h"
#include "paths.h"
#include "so_util.h"
#include "unity.h"

#define AMEDIA_OK 0
#define AMEDIA_ERROR_UNKNOWN (-10000)
#define AMEDIA_ERROR_MALFORMED (-10001)
#define AMEDIA_ERROR_UNSUPPORTED (-10002)
#define AMEDIA_ERROR_INVALID_OBJECT (-10003)
#define AMEDIA_ERROR_INVALID_PARAMETER (-10004)

#define INFO_TRY_AGAIN_LATER (-1)
#define INFO_OUTPUT_FORMAT_CHANGED (-2)
#define FLAG_KEY_FRAME 1
#define FLAG_END_OF_STREAM 4

#define COLOR_FormatYUV420Planar 19

long pread_fake(int fd, void *buf, size_t count, int64_t off); // bionic_extra.c

static int s_log; // [video] detail (config log_diag)

// --- AMediaFormat ------------------------------------------------------------------------

enum { V_I32, V_I64, V_F, V_S };

typedef struct {
  char key[40];
  int type;
  int64_t i;
  double f;
  char *s;
} FmtEntry;

typedef struct AMediaFormat {
  Mutex lock;
  int n;
  FmtEntry e[32];
  AVCodecParameters *par; // the track's codec (extradata) for AMediaCodec_configure
} AMediaFormat;

static AMediaFormat *fmt_new(void) {
  AMediaFormat *f = calloc(1, sizeof *f);
  if (f)
    mutexInit(&f->lock);
  return f;
}

static FmtEntry *fmt_slot(AMediaFormat *f, const char *key, int add) {
  for (int i = 0; i < f->n; i++)
    if (!strcmp(f->e[i].key, key))
      return &f->e[i];
  if (!add || f->n >= (int)(sizeof f->e / sizeof *f->e))
    return NULL;
  FmtEntry *e = &f->e[f->n++];
  memset(e, 0, sizeof *e);
  snprintf(e->key, sizeof e->key, "%s", key);
  return e;
}

static void fmt_set_i32(AMediaFormat *f, const char *key, int32_t v) {
  FmtEntry *e = fmt_slot(f, key, 1);
  if (e) e->type = V_I32, e->i = v;
}
static void fmt_set_i64(AMediaFormat *f, const char *key, int64_t v) {
  FmtEntry *e = fmt_slot(f, key, 1);
  if (e) e->type = V_I64, e->i = v;
}
static void fmt_set_f(AMediaFormat *f, const char *key, double v) {
  FmtEntry *e = fmt_slot(f, key, 1);
  if (e) e->type = V_F, e->f = v;
}
static void fmt_set_s(AMediaFormat *f, const char *key, const char *v) {
  FmtEntry *e = fmt_slot(f, key, 1);
  if (e) {
    free(e->s);
    e->type = V_S, e->s = strdup(v);
  }
}

static void AMediaFormat_delete(AMediaFormat *f) {
  if (!f)
    return;
  for (int i = 0; i < f->n; i++)
    free(f->e[i].s);
  avcodec_parameters_free(&f->par);
  free(f);
}

static AMediaFormat *AMediaFormat_new(void) { return fmt_new(); }

// Android answers only for the stored type; Unity asks frame-rate as either, so numbers convert
static bool AMediaFormat_getInt32(AMediaFormat *f, const char *key, int32_t *out) {
  if (!f || !key)
    return false;
  mutexLock(&f->lock);
  FmtEntry *e = fmt_slot(f, key, 0);
  bool ok = e && e->type != V_S;
  if (ok && out)
    *out = e->type == V_F ? (int32_t)e->f : (int32_t)e->i;
  mutexUnlock(&f->lock);
  if (!ok && s_log > 1)
    log_printf("[video] format %p has no int %s", (void *)f, key);
  return ok;
}

static bool AMediaFormat_getInt64(AMediaFormat *f, const char *key, int64_t *out) {
  if (!f || !key)
    return false;
  mutexLock(&f->lock);
  FmtEntry *e = fmt_slot(f, key, 0);
  bool ok = e && e->type != V_S;
  if (ok && out)
    *out = e->type == V_F ? (int64_t)e->f : e->i;
  mutexUnlock(&f->lock);
  return ok;
}

static bool AMediaFormat_getFloat(AMediaFormat *f, const char *key, float *out) {
  if (!f || !key)
    return false;
  mutexLock(&f->lock);
  FmtEntry *e = fmt_slot(f, key, 0);
  bool ok = e && e->type != V_S;
  if (ok && out)
    *out = e->type == V_F ? (float)e->f : (float)e->i;
  mutexUnlock(&f->lock);
  return ok;
}

static bool AMediaFormat_getDouble(AMediaFormat *f, const char *key, double *out) {
  float v;
  if (!AMediaFormat_getFloat(f, key, &v))
    return false;
  if (out)
    *out = v;
  return true;
}

static bool AMediaFormat_getString(AMediaFormat *f, const char *key, const char **out) {
  if (!f || !key)
    return false;
  mutexLock(&f->lock);
  FmtEntry *e = fmt_slot(f, key, 0);
  bool ok = e && e->type == V_S;
  if (ok && out)
    *out = e->s; // valid until the format is deleted (as on Android)
  mutexUnlock(&f->lock);
  return ok;
}

static void AMediaFormat_setInt32(AMediaFormat *f, const char *key, int32_t v) {
  if (!f || !key)
    return;
  mutexLock(&f->lock);
  fmt_set_i32(f, key, v);
  mutexUnlock(&f->lock);
}
static void AMediaFormat_setInt64(AMediaFormat *f, const char *key, int64_t v) {
  if (!f || !key)
    return;
  mutexLock(&f->lock);
  fmt_set_i64(f, key, v);
  mutexUnlock(&f->lock);
}
static void AMediaFormat_setFloat(AMediaFormat *f, const char *key, float v) {
  if (!f || !key)
    return;
  mutexLock(&f->lock);
  fmt_set_f(f, key, v);
  mutexUnlock(&f->lock);
}
static void AMediaFormat_setString(AMediaFormat *f, const char *key, const char *v) {
  if (!f || !key || !v)
    return;
  mutexLock(&f->lock);
  fmt_set_s(f, key, v);
  mutexUnlock(&f->lock);
}

static const char *AMediaFormat_toString(AMediaFormat *f) {
  static __thread char buf[512];
  int k = 0;
  buf[0] = 0;
  for (int i = 0; f && i < f->n && k < (int)sizeof buf - 64; i++) {
    const FmtEntry *e = &f->e[i];
    if (e->type == V_S)
      k += snprintf(buf + k, sizeof buf - k, "%s=%s ", e->key, e->s);
    else if (e->type == V_F)
      k += snprintf(buf + k, sizeof buf - k, "%s=%g ", e->key, e->f);
    else
      k += snprintf(buf + k, sizeof buf - k, "%s=%lld ", e->key, (long long)e->i);
  }
  return buf;
}

// --- AMediaDataSource -----------------------------------------------------------------------

typedef long (*DsReadAt)(void *ud, int64_t offset, void *buf, size_t size);
typedef long (*DsGetSize)(void *ud);
typedef void (*DsClose)(void *ud);

typedef struct AMediaDataSource {
  void *ud;
  DsReadAt read_at;
  DsGetSize get_size;
  DsClose close;
} AMediaDataSource;

static AMediaDataSource *AMediaDataSource_new(void) { return calloc(1, sizeof(AMediaDataSource)); }
static void AMediaDataSource_delete(AMediaDataSource *d) { free(d); }
static void AMediaDataSource_setUserdata(AMediaDataSource *d, void *ud) { if (d) d->ud = ud; }
static void AMediaDataSource_setReadAt(AMediaDataSource *d, DsReadAt f) { if (d) d->read_at = f; }
static void AMediaDataSource_setGetSize(AMediaDataSource *d, DsGetSize f) { if (d) d->get_size = f; }
static void AMediaDataSource_setClose(AMediaDataSource *d, DsClose f) { if (d) d->close = f; }

// --- AMediaExtractor ----------------------------------------------------------------------

#define MAX_TRACKS 8

typedef struct AMediaExtractor {
  Mutex lock;
  AVFormatContext *fmt;
  AVIOContext *io;
  // the source: a data source (Unity's VFS), a descriptor range, or a path
  AMediaDataSource *ds;
  int fd;
  int64_t base, len, pos;
  FILE *file;
  int selected[MAX_TRACKS];
  AVPacket *pkt;
  int have; // pkt holds the current sample
  int eos;
  unsigned samples;
} AMediaExtractor;

static int io_read(void *opaque, uint8_t *buf, int n) {
  AMediaExtractor *x = opaque;
  long got;
  if (x->ds) {
    got = x->ds->read_at(x->ds->ud, x->pos, buf, (size_t)n);
  } else if (x->file) {
    fseeko(x->file, x->pos, SEEK_SET);
    got = (long)fread(buf, 1, (size_t)n, x->file);
  } else {
    if (x->len >= 0 && x->pos + n > x->len)
      n = (int)(x->len - x->pos);
    got = n > 0 ? pread_fake(x->fd, buf, (size_t)n, x->base + x->pos) : 0;
  }
  if (got <= 0)
    return AVERROR_EOF;
  x->pos += got;
  return (int)got;
}

static int64_t io_size(AMediaExtractor *x) {
  if (x->ds)
    return x->ds->get_size ? x->ds->get_size(x->ds->ud) : -1;
  if (x->file) {
    const off_t at = ftello(x->file);
    fseeko(x->file, 0, SEEK_END);
    const int64_t n = ftello(x->file);
    fseeko(x->file, at, SEEK_SET);
    return n;
  }
  return x->len;
}

static int64_t io_seek(void *opaque, int64_t off, int whence) {
  AMediaExtractor *x = opaque;
  if (whence & AVSEEK_SIZE)
    return io_size(x);
  switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: x->pos = off; break;
    case SEEK_CUR: x->pos += off; break;
    case SEEK_END: {
      const int64_t n = io_size(x);
      if (n < 0)
        return -1;
      x->pos = n + off;
      break;
    }
    default: return -1;
  }
  return x->pos;
}

static AMediaExtractor *AMediaExtractor_new(void) {
  AMediaExtractor *x = calloc(1, sizeof *x);
  if (!x)
    return NULL;
  mutexInit(&x->lock);
  x->fd = -1;
  x->len = -1;
  x->pkt = av_packet_alloc();
  return x;
}

static void extractor_close(AMediaExtractor *x) {
  if (x->fmt)
    avformat_close_input(&x->fmt);
  if (x->io) {
    av_freep(&x->io->buffer);
    avio_context_free(&x->io);
  }
  if (x->file) {
    fclose(x->file);
    x->file = NULL;
  }
  av_packet_unref(x->pkt);
  x->have = 0;
}

static void AMediaExtractor_delete(AMediaExtractor *x) {
  if (!x)
    return;
  extractor_close(x);
  av_packet_free(&x->pkt);
  free(x);
}

static int extractor_open(AMediaExtractor *x, const char *what) {
  const int bufsize = 256 * 1024;
  uint8_t *buf = av_malloc(bufsize);
  x->io = buf ? avio_alloc_context(buf, bufsize, 0, x, io_read, NULL, io_seek) : NULL;
  if (!x->io) {
    av_free(buf);
    return AMEDIA_ERROR_UNKNOWN;
  }
  x->fmt = avformat_alloc_context();
  x->fmt->pb = x->io;
  x->fmt->flags |= AVFMT_FLAG_CUSTOM_IO;
  int r = avformat_open_input(&x->fmt, NULL, NULL, NULL);
  if (r >= 0)
    r = avformat_find_stream_info(x->fmt, NULL);
  if (r < 0) {
    char e[128];
    av_strerror(r, e, sizeof e);
    log_printf("[video] %s: not a stream FFmpeg reads (%s)", what, e);
    extractor_close(x);
    return AMEDIA_ERROR_UNSUPPORTED;
  }
  log_printf("[video] open %s: %s, %u tracks, %.1f s", what, x->fmt->iformat->name, x->fmt->nb_streams,
             x->fmt->duration > 0 ? x->fmt->duration / 1e6 : 0.0);
  return AMEDIA_OK;
}

static int AMediaExtractor_setDataSourceCustom(AMediaExtractor *x, AMediaDataSource *ds) {
  if (!x || !ds || !ds->read_at)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&x->lock);
  extractor_close(x);
  x->ds = ds;
  x->pos = 0;
  const int r = extractor_open(x, "data source");
  mutexUnlock(&x->lock);
  return r;
}

// The APK entry holding byte off of the APK (NULL: none)
static const char *apk_entry_at(int64_t off) {
  for (int i = 0, n = apkfile_count(); i < n; i++) {
    const ApkEntry *e = apkfile_entry_at(i);
    if (e && (int64_t)e->data_off <= off && off < (int64_t)(e->data_off + e->csize))
      return e->name;
  }
  return NULL;
}

static int AMediaExtractor_setDataSourceFd(AMediaExtractor *x, int fd, int64_t offset, int64_t length) {
  if (!x)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  // Resources/ videos: the game's splash, Apple Arcade's logo in every screen shape. Not on a Switch: refused,
  // and the game goes on as when a video fails (Project Dream's loader finishes it, gp_video_error)
  const char *entry = apk_entry_at(offset);
  if (entry && strstr(entry, "Data/resources.resource")) {
    log_printf("[video] %s: the Apple Arcade splash, skipped", entry);
    return AMEDIA_ERROR_UNSUPPORTED;
  }
  mutexLock(&x->lock);
  extractor_close(x);
  x->ds = NULL;
  x->fd = fd, x->base = offset, x->len = length, x->pos = 0;
  char what[64];
  snprintf(what, sizeof what, "fd %d +0x%llx", fd, (unsigned long long)offset);
  const int r = extractor_open(x, what);
  mutexUnlock(&x->lock);
  return r;
}


static int AMediaExtractor_setDataSource(AMediaExtractor *x, const char *location) {
  if (!x || !location)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&x->lock);
  extractor_close(x);
  x->ds = NULL;
  const char *real = path_from_android(location);
  x->file = fopen(real ? real : location, "rb");
  int r = AMEDIA_ERROR_UNSUPPORTED;
  if (x->file) {
    x->pos = 0;
    r = extractor_open(x, location);
  } else {
    log_printf("[video] open %s: no such file", location);
  }
  mutexUnlock(&x->lock);
  return r;
}

static size_t AMediaExtractor_getTrackCount(AMediaExtractor *x) {
  return x && x->fmt ? x->fmt->nb_streams : 0;
}

static const char *mime_of(const AVCodecParameters *p) {
  switch (p->codec_id) {
    case AV_CODEC_ID_H264: return "video/avc";
    case AV_CODEC_ID_HEVC: return "video/hevc";
    case AV_CODEC_ID_VP8: return "video/x-vnd.on2.vp8";
    case AV_CODEC_ID_VP9: return "video/x-vnd.on2.vp9";
    case AV_CODEC_ID_AV1: return "video/av01";
    case AV_CODEC_ID_MPEG4: return "video/mp4v-es";
    case AV_CODEC_ID_AAC: return "audio/mp4a-latm";
    case AV_CODEC_ID_VORBIS: return "audio/vorbis";
    case AV_CODEC_ID_OPUS: return "audio/opus";
    case AV_CODEC_ID_MP3: return "audio/mpeg";
    default: return p->codec_type == AVMEDIA_TYPE_VIDEO ? "video/unknown" : "audio/unknown";
  }
}

// Android's MediaFormat color constants (COLOR_STANDARD_*, COLOR_RANGE_*)
static int color_standard_of(const AVCodecParameters *p) {
  switch (p->color_space) {
    case AVCOL_SPC_BT709: return 1;
    case AVCOL_SPC_BT470BG: return 2;
    case AVCOL_SPC_SMPTE170M: return 4;
    case AVCOL_SPC_BT2020_NCL:
    case AVCOL_SPC_BT2020_CL: return 6;
    default: return p->height >= 720 ? 1 : 4;
  }
}

static AMediaFormat *AMediaExtractor_getTrackFormat(AMediaExtractor *x, size_t i) {
  if (!x || !x->fmt || i >= x->fmt->nb_streams)
    return NULL;
  const AVStream *st = x->fmt->streams[i];
  const AVCodecParameters *p = st->codecpar;
  AMediaFormat *f = fmt_new();
  if (!f)
    return NULL;
  f->par = avcodec_parameters_alloc();
  avcodec_parameters_copy(f->par, p);
  fmt_set_s(f, "mime", mime_of(p));
  int64_t dur = st->duration > 0 ? av_rescale_q(st->duration, st->time_base, (AVRational){ 1, 1000000 })
                                 : (x->fmt->duration > 0 ? x->fmt->duration : 0);
  fmt_set_i64(f, "durationUs", dur);
  fmt_set_i32(f, "track-id", (int32_t)i + 1);
  if (p->codec_type == AVMEDIA_TYPE_VIDEO) {
    fmt_set_i32(f, "width", p->width);
    fmt_set_i32(f, "height", p->height);
    const AVRational fr = st->avg_frame_rate.num ? st->avg_frame_rate : st->r_frame_rate;
    if (fr.num && fr.den)
      fmt_set_f(f, "frame-rate", av_q2d(fr));
    fmt_set_i32(f, "max-input-size", 4 << 20);
    fmt_set_i32(f, "color-format", COLOR_FormatYUV420Planar);
    fmt_set_i32(f, "color-standard", color_standard_of(p));
    fmt_set_i32(f, "color-range", p->color_range == AVCOL_RANGE_JPEG ? 1 : 2);
    if (p->sample_aspect_ratio.num > 0 && p->sample_aspect_ratio.den > 0) {
      fmt_set_i32(f, "sar-width", p->sample_aspect_ratio.num);
      fmt_set_i32(f, "sar-height", p->sample_aspect_ratio.den);
    }
    fmt_set_i32(f, "rotation-degrees", 0);
  } else if (p->codec_type == AVMEDIA_TYPE_AUDIO) {
    fmt_set_i32(f, "channel-count", p->ch_layout.nb_channels);
    fmt_set_i32(f, "sample-rate", p->sample_rate);
    fmt_set_i32(f, "max-input-size", 64 << 10);
    fmt_set_i32(f, "pcm-encoding", 2); // AudioFormat.ENCODING_PCM_16BIT
    AVDictionaryEntry *lang = av_dict_get(st->metadata, "language", NULL, 0);
    fmt_set_s(f, "language", lang ? lang->value : "und");
    if (p->initial_padding > 0)
      fmt_set_i32(f, "encoder-delay", p->initial_padding);
  }
  if (s_log)
    log_printf("[video] track %zu: %s", i, AMediaFormat_toString(f));
  return f;
}

static int AMediaExtractor_selectTrack(AMediaExtractor *x, size_t i) {
  if (!x || !x->fmt || i >= x->fmt->nb_streams || i >= MAX_TRACKS)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&x->lock);
  x->selected[i] = 1;
  x->have = 0;
  av_packet_unref(x->pkt);
  mutexUnlock(&x->lock);
  return AMEDIA_OK;
}

static int AMediaExtractor_unselectTrack(AMediaExtractor *x, size_t i) {
  if (!x || i >= MAX_TRACKS)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&x->lock);
  x->selected[i] = 0;
  mutexUnlock(&x->lock);
  return AMEDIA_OK;
}

// the next sample of a selected track into x->pkt (lock held); 0 at the end
static int extractor_fill(AMediaExtractor *x) {
  if (x->have)
    return 1;
  if (!x->fmt || x->eos)
    return 0;
  for (;;) {
    av_packet_unref(x->pkt);
    const int r = av_read_frame(x->fmt, x->pkt);
    if (r < 0) {
      x->eos = 1;
      return 0;
    }
    const int s = x->pkt->stream_index;
    if (s >= 0 && s < MAX_TRACKS && x->selected[s]) {
      x->have = 1;
      x->samples++;
      return 1;
    }
  }
}

static long AMediaExtractor_readSampleData(AMediaExtractor *x, uint8_t *buf, size_t cap) {
  if (!x)
    return -1;
  mutexLock(&x->lock);
  long r = -1;
  if (extractor_fill(x)) {
    r = x->pkt->size;
    if ((size_t)r > cap) {
      log_printf("[video] sample of %ld bytes, buffer %zu", r, cap);
      r = -1;
    } else {
      memcpy(buf, x->pkt->data, (size_t)r);
    }
  }
  mutexUnlock(&x->lock);
  return r;
}

static int AMediaExtractor_getSampleTrackIndex(AMediaExtractor *x) {
  if (!x)
    return -1;
  mutexLock(&x->lock);
  const int r = extractor_fill(x) ? x->pkt->stream_index : -1;
  mutexUnlock(&x->lock);
  return r;
}

static int64_t pkt_time_us(AMediaExtractor *x) {
  const AVStream *st = x->fmt->streams[x->pkt->stream_index];
  int64_t t = x->pkt->pts != AV_NOPTS_VALUE ? x->pkt->pts : x->pkt->dts;
  if (t == AV_NOPTS_VALUE)
    return 0;
  if (st->start_time != AV_NOPTS_VALUE)
    t -= st->start_time;
  return av_rescale_q(t, st->time_base, (AVRational){ 1, 1000000 });
}

static int64_t AMediaExtractor_getSampleTime(AMediaExtractor *x) {
  if (!x)
    return -1;
  mutexLock(&x->lock);
  const int64_t r = extractor_fill(x) ? pkt_time_us(x) : -1;
  mutexUnlock(&x->lock);
  return r;
}

static uint32_t AMediaExtractor_getSampleFlags(AMediaExtractor *x) {
  if (!x)
    return 0;
  mutexLock(&x->lock);
  const uint32_t r = extractor_fill(x) && (x->pkt->flags & AV_PKT_FLAG_KEY) ? 1 : 0; // SAMPLE_FLAG_SYNC
  mutexUnlock(&x->lock);
  return r;
}

static int64_t AMediaExtractor_getSampleSize(AMediaExtractor *x) {
  if (!x)
    return -1;
  mutexLock(&x->lock);
  const int64_t r = extractor_fill(x) ? x->pkt->size : -1;
  mutexUnlock(&x->lock);
  return r;
}

static int64_t AMediaExtractor_getCachedDuration(AMediaExtractor *x) { (void)x; return -1; }

static bool AMediaExtractor_advance(AMediaExtractor *x) {
  if (!x)
    return false;
  mutexLock(&x->lock);
  extractor_fill(x);
  x->have = 0;
  av_packet_unref(x->pkt);
  const bool more = extractor_fill(x);
  mutexUnlock(&x->lock);
  return more;
}

// mode: 0 previous sync, 1 next sync, 2 closest sync
static int AMediaExtractor_seekTo(AMediaExtractor *x, int64_t us, int mode) {
  if (!x || !x->fmt)
    return AMEDIA_ERROR_INVALID_OBJECT;
  mutexLock(&x->lock);
  int64_t ts = us;
  if (x->fmt->start_time != AV_NOPTS_VALUE)
    ts += x->fmt->start_time;
  int r = av_seek_frame(x->fmt, -1, ts, mode == 1 ? 0 : AVSEEK_FLAG_BACKWARD);
  if (r < 0)
    r = av_seek_frame(x->fmt, -1, ts, AVSEEK_FLAG_ANY | AVSEEK_FLAG_BACKWARD);
  x->have = 0;
  x->eos = 0;
  av_packet_unref(x->pkt);
  mutexUnlock(&x->lock);
  if (s_log)
    log_printf("[video] seek to %.3f s (mode %d): %d", us / 1e6, mode, r);
  return r < 0 ? AMEDIA_ERROR_UNKNOWN : AMEDIA_OK;
}

static AMediaFormat *AMediaExtractor_getFileFormat(AMediaExtractor *x) {
  AMediaFormat *f = fmt_new();
  if (f && x && x->fmt) {
    fmt_set_s(f, "mime", "video/mp4");
    fmt_set_i64(f, "durationUs", x->fmt->duration > 0 ? x->fmt->duration : 0);
  }
  return f;
}

// --- AMediaCodec --------------------------------------------------------------------------

typedef struct {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
} AMediaCodecBufferInfo;

#define N_IN 2
#define N_OUT 8
#define IN_CAP (4 << 20)

typedef struct {
  uint8_t *data;
  size_t cap, size;
  int64_t pts;
  uint32_t flags;
  int busy;  // holds a decoded frame (dequeued or waiting)
  int given; // dequeued by the caller, not yet released
} OutSlot;

typedef struct AMediaCodec {
  Mutex lock;
  int audio;
  char mime[32];
  const AVCodec *codec;
  AVCodecContext *ctx;
  AVPacket *pkt;
  AVFrame *frame;
  uint8_t *in[N_IN];
  int in_given[N_IN];
  // a packet the decoder would not take yet (its frames are waiting for output slots)
  AVPacket *pending;
  int pending_eos;
  OutSlot out[N_OUT];
  int out_order[N_OUT]; // decoded slots, oldest first
  int nready;
  int started, eos_sent, eos_out;
  AMediaFormat *outfmt;
  int out_w, out_h, out_stride, out_slice;
  int fmt_changed; // the next dequeue says OUTPUT_FORMAT_CHANGED
  struct SwrContext *swr;
  struct SwsContext *sws;
  int swr_rate, swr_ch;
  // stats
  unsigned frames;
  u64 decode_ns;
} AMediaCodec;

static AMediaCodec *AMediaCodec_createDecoderByType(const char *mime) {
  if (!mime)
    return NULL;
  enum AVCodecID id = AV_CODEC_ID_NONE;
  if (!strcmp(mime, "video/avc")) id = AV_CODEC_ID_H264;
  else if (!strcmp(mime, "video/hevc")) id = AV_CODEC_ID_HEVC;
  else if (!strcmp(mime, "video/x-vnd.on2.vp8")) id = AV_CODEC_ID_VP8;
  else if (!strcmp(mime, "video/x-vnd.on2.vp9")) id = AV_CODEC_ID_VP9;
  else if (!strcmp(mime, "video/av01")) id = AV_CODEC_ID_AV1;
  else if (!strcmp(mime, "video/mp4v-es")) id = AV_CODEC_ID_MPEG4;
  else if (!strcmp(mime, "audio/mp4a-latm")) id = AV_CODEC_ID_AAC;
  else if (!strcmp(mime, "audio/vorbis")) id = AV_CODEC_ID_VORBIS;
  else if (!strcmp(mime, "audio/opus")) id = AV_CODEC_ID_OPUS;
  else if (!strcmp(mime, "audio/mpeg")) id = AV_CODEC_ID_MP3;
  const AVCodec *c = id == AV_CODEC_ID_AV1 ? avcodec_find_decoder_by_name("libdav1d") : NULL;
  if (!c)
    c = avcodec_find_decoder(id);
  if (!c) {
    log_printf("[video] no decoder for %s", mime);
    return NULL;
  }
  AMediaCodec *m = calloc(1, sizeof *m);
  if (!m)
    return NULL;
  mutexInit(&m->lock);
  m->codec = c;
  m->audio = !strncmp(mime, "audio/", 6);
  snprintf(m->mime, sizeof m->mime, "%s", mime);
  m->pkt = av_packet_alloc();
  m->pending = av_packet_alloc();
  m->frame = av_frame_alloc();
  for (int i = 0; i < N_IN; i++)
    m->in[i] = av_malloc(IN_CAP + AV_INPUT_BUFFER_PADDING_SIZE);
  log_printf("[video] decoder %s for %s", c->name, mime);
  return m;
}

static AMediaCodec *AMediaCodec_createCodecByName(const char *name) {
  (void)name;
  return NULL;
}

static int AMediaCodec_configure(AMediaCodec *m, const AMediaFormat *fmt, void *surface, void *crypto,
                                 uint32_t flags) {
  (void)crypto;
  if (!m || !fmt || (flags & 1)) // CONFIGURE_FLAG_ENCODE
    return AMEDIA_ERROR_INVALID_PARAMETER;
  if (surface) {
    log_printf("[video] configure with a surface: not here (raw buffers only)");
    return AMEDIA_ERROR_UNSUPPORTED;
  }
  mutexLock(&m->lock);
  avcodec_free_context(&m->ctx);
  m->ctx = avcodec_alloc_context3(m->codec);
  if (fmt->par)
    avcodec_parameters_to_context(m->ctx, fmt->par);
  m->ctx->pkt_timebase = (AVRational){ 1, 1000000 };
  if (!m->audio) {
    // frame threads on the console's three application cores; the game runs on them too
    const int big = m->ctx->width * m->ctx->height >= 1280 * 720;
    m->ctx->thread_count = config.video_threads > 0 ? config.video_threads : (big ? 3 : 2);
    m->ctx->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
    if (config.video_fast) {
      m->ctx->flags2 |= AV_CODEC_FLAG2_FAST;
      m->ctx->skip_loop_filter = AVDISCARD_NONREF;
    }
  } else {
    m->ctx->request_sample_fmt = AV_SAMPLE_FMT_S16;
  }
  AVDictionary *opt = NULL;
  const int r = avcodec_open2(m->ctx, m->codec, &opt);
  av_dict_free(&opt);
  mutexUnlock(&m->lock);
  if (r < 0) {
    log_printf("[video] %s: open failed (%d)", m->mime, r);
    return AMEDIA_ERROR_UNKNOWN;
  }
  if (!m->audio)
    log_printf("[video] %s %dx%d, %d threads%s", m->codec->name, m->ctx->width, m->ctx->height,
               m->ctx->thread_count, config.video_fast ? ", fast" : "");
  else
    log_printf("[video] %s %d Hz, %d ch", m->codec->name, m->ctx->sample_rate, m->ctx->ch_layout.nb_channels);
  return AMEDIA_OK;
}

static int AMediaCodec_start(AMediaCodec *m) {
  if (!m || !m->ctx)
    return AMEDIA_ERROR_INVALID_OBJECT;
  m->started = 1;
  return AMEDIA_OK;
}

static void codec_reset(AMediaCodec *m) {
  if (m->ctx)
    avcodec_flush_buffers(m->ctx);
  av_packet_unref(m->pending);
  m->pending_eos = 0;
  for (int i = 0; i < N_OUT; i++)
    m->out[i].busy = m->out[i].given = 0;
  for (int i = 0; i < N_IN; i++)
    m->in_given[i] = 0;
  m->nready = 0;
  m->eos_sent = m->eos_out = 0;
}

static int AMediaCodec_flush(AMediaCodec *m) {
  if (!m)
    return AMEDIA_ERROR_INVALID_OBJECT;
  mutexLock(&m->lock);
  codec_reset(m);
  mutexUnlock(&m->lock);
  return AMEDIA_OK;
}

static int AMediaCodec_stop(AMediaCodec *m) {
  if (!m)
    return AMEDIA_ERROR_INVALID_OBJECT;
  mutexLock(&m->lock);
  codec_reset(m);
  m->started = 0;
  mutexUnlock(&m->lock);
  return AMEDIA_OK;
}

static int AMediaCodec_delete(AMediaCodec *m) {
  if (!m)
    return AMEDIA_OK;
  if (m->frames)
    log_printf("[video] %s done: %u frames, %.2f ms each to decode", m->codec->name, m->frames,
               m->decode_ns / 1e6 / m->frames);
  avcodec_free_context(&m->ctx);
  av_packet_free(&m->pkt);
  av_packet_free(&m->pending);
  av_frame_free(&m->frame);
  for (int i = 0; i < N_IN; i++)
    av_free(m->in[i]);
  for (int i = 0; i < N_OUT; i++)
    free(m->out[i].data);
  AMediaFormat_delete(m->outfmt);
  swr_free(&m->swr);
  sws_freeContext(m->sws);
  free(m);
  return AMEDIA_OK;
}

static long AMediaCodec_dequeueInputBuffer(AMediaCodec *m, int64_t timeout_us) {
  (void)timeout_us;
  if (!m || !m->started)
    return INFO_TRY_AGAIN_LATER;
  mutexLock(&m->lock);
  long r = INFO_TRY_AGAIN_LATER;
  if (!m->pending->size && !m->pending_eos && !m->eos_sent)
    for (int i = 0; i < N_IN; i++)
      if (!m->in_given[i]) {
        m->in_given[i] = 1;
        r = i;
        break;
      }
  mutexUnlock(&m->lock);
  return r;
}

static uint8_t *AMediaCodec_getInputBuffer(AMediaCodec *m, size_t i, size_t *size) {
  if (!m || i >= N_IN)
    return NULL;
  if (size)
    *size = IN_CAP;
  return m->in[i];
}

static int free_slot(AMediaCodec *m) {
  for (int i = 0; i < N_OUT; i++)
    if (!m->out[i].busy)
      return i;
  return -1;
}

static void set_out_format(AMediaCodec *m, int w, int h) {
  AMediaFormat_delete(m->outfmt);
  m->outfmt = fmt_new();
  if (!m->audio) {
    m->out_w = w, m->out_h = h;
    m->out_stride = FFALIGN(w, 32);
    m->out_slice = FFALIGN(h, 2);
    fmt_set_s(m->outfmt, "mime", "video/raw");
    fmt_set_i32(m->outfmt, "width", w);
    fmt_set_i32(m->outfmt, "height", h);
    fmt_set_i32(m->outfmt, "stride", m->out_stride);
    fmt_set_i32(m->outfmt, "slice-height", m->out_slice);
    fmt_set_i32(m->outfmt, "color-format", COLOR_FormatYUV420Planar);
    fmt_set_i32(m->outfmt, "crop-left", 0);
    fmt_set_i32(m->outfmt, "crop-top", 0);
    fmt_set_i32(m->outfmt, "crop-right", w - 1);
    fmt_set_i32(m->outfmt, "crop-bottom", h - 1);
    fmt_set_i32(m->outfmt, "color-standard", m->ctx->colorspace == AVCOL_SPC_BT709 || h >= 720 ? 1 : 4);
    fmt_set_i32(m->outfmt, "color-range", m->ctx->color_range == AVCOL_RANGE_JPEG ? 1 : 2);
  } else {
    fmt_set_s(m->outfmt, "mime", "audio/raw");
    fmt_set_i32(m->outfmt, "sample-rate", m->ctx->sample_rate);
    fmt_set_i32(m->outfmt, "channel-count", m->ctx->ch_layout.nb_channels);
    fmt_set_i32(m->outfmt, "pcm-encoding", 2);
  }
  m->fmt_changed = 1;
  if (s_log)
    log_printf("[video] output format: %s", AMediaFormat_toString(m->outfmt));
}

static int slot_reserve(OutSlot *s, size_t n) {
  if (s->cap >= n)
    return 1;
  free(s->data);
  s->data = malloc(n);
  s->cap = s->data ? n : 0;
  return s->data != NULL;
}

// m->frame into output slot k: I420 (stride out_stride, slice out_slice) or interleaved PCM 16
static int frame_to_slot(AMediaCodec *m, int k) {
  OutSlot *s = &m->out[k];
  AVFrame *f = m->frame;
  const int64_t pts = f->best_effort_timestamp != AV_NOPTS_VALUE ? f->best_effort_timestamp : f->pts;
  s->pts = pts == AV_NOPTS_VALUE ? 0 : pts; // (pkt_timebase is microseconds)
  s->flags = 0;
  if (!m->audio) {
    const int w = m->out_w, h = m->out_h, ys = m->out_stride, sl = m->out_slice;
    const size_t ysize = (size_t)ys * sl, csize = (size_t)(ys / 2) * (sl / 2);
    if (!slot_reserve(s, ysize + 2 * csize))
      return 0;
    uint8_t *dst[3] = { s->data, s->data + ysize, s->data + ysize + csize };
    const int dls[3] = { ys, ys / 2, ys / 2 };
    if (f->format == AV_PIX_FMT_YUV420P || f->format == AV_PIX_FMT_YUVJ420P) {
      av_image_copy_plane(dst[0], dls[0], f->data[0], f->linesize[0], w, h);
      av_image_copy_plane(dst[1], dls[1], f->data[1], f->linesize[1], (w + 1) / 2, (h + 1) / 2);
      av_image_copy_plane(dst[2], dls[2], f->data[2], f->linesize[2], (w + 1) / 2, (h + 1) / 2);
    } else {
      m->sws = sws_getCachedContext(m->sws, f->width, f->height, f->format, w, h, AV_PIX_FMT_YUV420P,
                                    SWS_FAST_BILINEAR, NULL, NULL, NULL);
      if (!m->sws)
        return 0;
      sws_scale(m->sws, (const uint8_t *const *)f->data, f->linesize, 0, f->height, dst, dls);
    }
    s->size = ysize + 2 * csize;
  } else {
    const int ch = f->ch_layout.nb_channels, rate = f->sample_rate;
    if (!m->swr || m->swr_rate != rate || m->swr_ch != ch) {
      swr_free(&m->swr);
      AVChannelLayout out_layout;
      av_channel_layout_default(&out_layout, ch);
      swr_alloc_set_opts2(&m->swr, &out_layout, AV_SAMPLE_FMT_S16, rate, &f->ch_layout, f->format, rate, 0, NULL);
      if (!m->swr || swr_init(m->swr) < 0)
        return 0;
      m->swr_rate = rate, m->swr_ch = ch;
    }
    const size_t n = (size_t)f->nb_samples * ch * 2;
    if (!slot_reserve(s, n + 4096))
      return 0;
    uint8_t *out[1] = { s->data };
    const int got = swr_convert(m->swr, out, f->nb_samples + 256, (const uint8_t **)f->extended_data, f->nb_samples);
    s->size = got > 0 ? (size_t)got * ch * 2 : 0;
  }
  return 1;
}

// decoded frames into free slots, then the pending packet into the decoder (lock held)
static void codec_pump(AMediaCodec *m) {
  for (int round = 0; round < 4; round++) {
    // frames out while there is room for them
    for (;;) {
      if (m->fmt_changed)
        return; // the caller sees the new format before more frames
      const int k = free_slot(m);
      if (k < 0)
        break;
      const u64 t0 = armGetSystemTick();
      const int r = avcodec_receive_frame(m->ctx, m->frame);
      if (r == AVERROR_EOF) {
        m->eos_out = 1;
        break;
      }
      if (r < 0)
        break;
      m->decode_ns += armTicksToNs(armGetSystemTick() - t0);
      const int w = m->audio ? 0 : m->frame->width, h = m->audio ? 0 : m->frame->height;
      if (!m->outfmt || (!m->audio && (w != m->out_w || h != m->out_h))) {
        set_out_format(m, w, h);
        if (!frame_to_slot(m, k)) {
          av_frame_unref(m->frame);
          continue;
        }
        m->out[k].busy = 1;
        m->out_order[m->nready++] = k;
        av_frame_unref(m->frame);
        return;
      }
      if (frame_to_slot(m, k)) {
        m->out[k].busy = 1;
        m->out_order[m->nready++] = k;
        m->frames++;
      }
      av_frame_unref(m->frame);
    }
    // the waiting packet in
    if (m->pending->size) {
      const u64 t0 = armGetSystemTick();
      const int r = avcodec_send_packet(m->ctx, m->pending);
      m->decode_ns += armTicksToNs(armGetSystemTick() - t0);
      if (r == AVERROR(EAGAIN))
        continue; // its frames must come out first (next round, or the next dequeue)
      if (r < 0 && s_log)
        log_printf("[video] %s: packet refused (%d)", m->codec->name, r);
      av_packet_unref(m->pending);
    } else if (m->pending_eos) {
      avcodec_send_packet(m->ctx, NULL);
      m->pending_eos = 0;
      m->eos_sent = 1;
    } else {
      return;
    }
  }
}

static int AMediaCodec_queueInputBuffer(AMediaCodec *m, size_t i, int64_t offset, size_t size, uint64_t time_us,
                                        uint32_t flags) {
  if (!m || i >= N_IN || !m->ctx)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&m->lock);
  m->in_given[i] = 0;
  if (size > 0 && offset >= 0 && offset + size <= IN_CAP && !(flags & 2)) { // (2: codec config, in extradata)
    av_packet_unref(m->pending);
    if (av_new_packet(m->pending, (int)size) == 0) {
      memcpy(m->pending->data, m->in[i] + offset, size);
      m->pending->pts = (int64_t)time_us;
      m->pending->dts = AV_NOPTS_VALUE;
    }
  }
  if (flags & FLAG_END_OF_STREAM)
    m->pending_eos = 1;
  codec_pump(m);
  mutexUnlock(&m->lock);
  return AMEDIA_OK;
}

static long AMediaCodec_dequeueOutputBuffer(AMediaCodec *m, AMediaCodecBufferInfo *info, int64_t timeout_us) {
  (void)timeout_us;
  if (!m || !m->started)
    return INFO_TRY_AGAIN_LATER;
  mutexLock(&m->lock);
  long r = INFO_TRY_AGAIN_LATER;
  if (m->fmt_changed) {
    m->fmt_changed = 0;
    r = INFO_OUTPUT_FORMAT_CHANGED;
  } else {
    codec_pump(m);
    if (m->fmt_changed) {
      m->fmt_changed = 0;
      r = INFO_OUTPUT_FORMAT_CHANGED;
    } else if (m->nready > 0) {
      const int k = m->out_order[0];
      memmove(m->out_order, m->out_order + 1, sizeof(int) * (size_t)--m->nready);
      OutSlot *s = &m->out[k];
      s->given = 1;
      if (info) {
        info->offset = 0;
        info->size = (int32_t)s->size;
        info->presentationTimeUs = s->pts;
        info->flags = (m->eos_out && m->nready == 0) ? FLAG_END_OF_STREAM : 0;
      }
      r = k;
    } else if (m->eos_out) {
      // the end: an empty buffer with the flag
      const int k = free_slot(m);
      if (k >= 0) {
        OutSlot *s = &m->out[k];
        s->busy = s->given = 1;
        s->size = 0;
        if (info) {
          info->offset = 0;
          info->size = 0;
          info->presentationTimeUs = 0;
          info->flags = FLAG_END_OF_STREAM;
        }
        m->eos_out = 0;
        r = k;
      }
    }
  }
  mutexUnlock(&m->lock);
  return r;
}

static uint8_t *AMediaCodec_getOutputBuffer(AMediaCodec *m, size_t i, size_t *size) {
  if (!m || i >= N_OUT)
    return NULL;
  if (size)
    *size = m->out[i].cap;
  return m->out[i].data;
}

static AMediaFormat *AMediaCodec_getOutputFormat(AMediaCodec *m) {
  AMediaFormat *f = fmt_new();
  if (!m || !f)
    return f;
  mutexLock(&m->lock);
  if (m->outfmt) {
    f->n = m->outfmt->n;
    memcpy(f->e, m->outfmt->e, sizeof f->e);
    for (int i = 0; i < f->n; i++)
      if (f->e[i].s)
        f->e[i].s = strdup(f->e[i].s);
  } else if (m->ctx && !m->audio) {
    fmt_set_s(f, "mime", "video/raw");
    fmt_set_i32(f, "width", m->ctx->width);
    fmt_set_i32(f, "height", m->ctx->height);
    fmt_set_i32(f, "stride", FFALIGN(m->ctx->width, 32));
    fmt_set_i32(f, "slice-height", FFALIGN(m->ctx->height, 2));
    fmt_set_i32(f, "color-format", COLOR_FormatYUV420Planar);
  } else if (m->ctx) {
    fmt_set_s(f, "mime", "audio/raw");
    fmt_set_i32(f, "sample-rate", m->ctx->sample_rate);
    fmt_set_i32(f, "channel-count", m->ctx->ch_layout.nb_channels);
    fmt_set_i32(f, "pcm-encoding", 2);
  }
  mutexUnlock(&m->lock);
  return f;
}

static int AMediaCodec_releaseOutputBuffer(AMediaCodec *m, size_t i, bool render) {
  (void)render;
  if (!m || i >= N_OUT)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  mutexLock(&m->lock);
  m->out[i].busy = m->out[i].given = 0;
  mutexUnlock(&m->lock);
  return AMEDIA_OK;
}

static int AMediaCodec_releaseOutputBufferAtTime(AMediaCodec *m, size_t i, int64_t ns) {
  (void)ns;
  return AMediaCodec_releaseOutputBuffer(m, i, true);
}

static int AMediaCodec_setOutputSurface(AMediaCodec *m, void *surface) {
  (void)m, (void)surface;
  return AMEDIA_ERROR_UNSUPPORTED;
}

static int media_unsupported(void) { return AMEDIA_ERROR_UNSUPPORTED; }
static int media_ok(void) { return AMEDIA_OK; }

// --- the library --------------------------------------------------------------------------

static const char *const k_mime = "mime", *const k_width = "width", *const k_height = "height",
                         *const k_duration = "durationUs", *const k_frame_rate = "frame-rate",
                         *const k_rotation = "rotation-degrees", *const k_color = "color-format",
                         *const k_stride = "stride", *const k_slice = "slice-height",
                         *const k_channels = "channel-count", *const k_rate = "sample-rate",
                         *const k_lang = "language", *const k_delay = "encoder-delay",
                         *const k_crange = "color-range", *const k_cstd = "color-standard",
                         *const k_max_input = "max-input-size", *const k_pcm = "pcm-encoding",
                         *const k_track_id = "track-id", *const k_bitrate = "bitrate",
                         *const k_ctransfer = "color-transfer", *const k_padding = "encoder-padding",
                         *const k_crop_l = "crop-left", *const k_crop_t = "crop-top",
                         *const k_crop_r = "crop-right", *const k_crop_b = "crop-bottom",
                         *const k_is_adts = "is-adts", *const k_aac_profile = "aac-profile",
                         *const k_csd0 = "csd-0", *const k_csd1 = "csd-1";

#define F(name) { #name, (uintptr_t)&name }
#define K(name, var) { "AMEDIAFORMAT_KEY_" #name, (uintptr_t)&var }

DynLibFunction media_functions[] = {
  F(AMediaFormat_new), F(AMediaFormat_delete), F(AMediaFormat_getInt32), F(AMediaFormat_getInt64),
  F(AMediaFormat_getFloat), F(AMediaFormat_getDouble), F(AMediaFormat_getString), F(AMediaFormat_setInt32),
  F(AMediaFormat_setInt64), F(AMediaFormat_setFloat), F(AMediaFormat_setString), F(AMediaFormat_toString),

  F(AMediaDataSource_new), F(AMediaDataSource_delete), F(AMediaDataSource_setUserdata),
  F(AMediaDataSource_setReadAt), F(AMediaDataSource_setGetSize), F(AMediaDataSource_setClose),

  F(AMediaExtractor_new), F(AMediaExtractor_delete), F(AMediaExtractor_setDataSource),
  F(AMediaExtractor_setDataSourceFd), F(AMediaExtractor_setDataSourceCustom), F(AMediaExtractor_getTrackCount),
  F(AMediaExtractor_getTrackFormat), F(AMediaExtractor_selectTrack), F(AMediaExtractor_unselectTrack),
  F(AMediaExtractor_readSampleData), F(AMediaExtractor_getSampleTrackIndex), F(AMediaExtractor_getSampleTime),
  F(AMediaExtractor_getSampleFlags), F(AMediaExtractor_getSampleSize), F(AMediaExtractor_getCachedDuration),
  F(AMediaExtractor_advance), F(AMediaExtractor_seekTo), F(AMediaExtractor_getFileFormat),

  F(AMediaCodec_createDecoderByType), F(AMediaCodec_createCodecByName), F(AMediaCodec_configure),
  F(AMediaCodec_start), F(AMediaCodec_stop), F(AMediaCodec_flush), F(AMediaCodec_delete),
  F(AMediaCodec_dequeueInputBuffer), F(AMediaCodec_getInputBuffer), F(AMediaCodec_queueInputBuffer),
  F(AMediaCodec_dequeueOutputBuffer), F(AMediaCodec_getOutputBuffer), F(AMediaCodec_getOutputFormat),
  F(AMediaCodec_releaseOutputBuffer), F(AMediaCodec_releaseOutputBufferAtTime), F(AMediaCodec_setOutputSurface),

  // the hardware-buffer path (AndroidVulkanVideo), which media_ndk_init turns off
  { "AImageReader_newWithUsage", (uintptr_t)&media_unsupported },
  { "AImageReader_delete", (uintptr_t)&media_ok },
  { "AImageReader_getWindow", (uintptr_t)&media_unsupported },
  { "AImageReader_acquireLatestImage", (uintptr_t)&media_unsupported },
  { "AImageReader_setImageListener", (uintptr_t)&media_unsupported },
  { "AImageReader_setBufferRemovedListener", (uintptr_t)&media_unsupported },
  { "AImage_delete", (uintptr_t)&media_ok },
  { "AImage_deleteAsync", (uintptr_t)&media_ok },
  { "AImage_getHardwareBuffer", (uintptr_t)&media_unsupported },
  { "AImage_getTimestamp", (uintptr_t)&media_unsupported },
  { "AImage_getWidth", (uintptr_t)&media_unsupported },
  { "AHardwareBuffer_describe", (uintptr_t)&media_ok },
  { "AHardwareBuffer_release", (uintptr_t)&media_ok },

  K(MIME, k_mime), K(WIDTH, k_width), K(HEIGHT, k_height), K(DURATION, k_duration),
  K(FRAME_RATE, k_frame_rate), K(ROTATION, k_rotation), K(COLOR_FORMAT, k_color), K(STRIDE, k_stride),
  K(SLICE_HEIGHT, k_slice), K(CHANNEL_COUNT, k_channels), K(SAMPLE_RATE, k_rate), K(LANGUAGE, k_lang),
  K(ENCODER_DELAY, k_delay), K(COLOR_RANGE, k_crange), K(COLOR_STANDARD, k_cstd),
  K(MAX_INPUT_SIZE, k_max_input), K(PCM_ENCODING, k_pcm), K(TRACK_ID, k_track_id), K(BIT_RATE, k_bitrate),
  K(COLOR_TRANSFER, k_ctransfer), K(ENCODER_PADDING, k_padding), K(CROP_LEFT, k_crop_l),
  K(CROP_TOP, k_crop_t), K(CROP_RIGHT, k_crop_r), K(CROP_BOTTOM, k_crop_b), K(IS_ADTS, k_is_adts),
  K(AAC_PROFILE, k_aac_profile), K(CSD_0, k_csd0), K(CSD_1, k_csd1),
};

size_t media_numfunctions = sizeof(media_functions) / sizeof(*media_functions);

// The Java constants Unity's video code reads (android.media.MediaFormat, MediaCodecInfo.CodecCapabilities)
void media_ndk_java_init(void) {
  s_log = config.log_diag;
  const char *mf = "android/media/MediaFormat", *cc = "android/media/MediaCodecInfo$CodecCapabilities";
  jni_static_field(mf, "COLOR_RANGE_FULL", jv_i(1));
  jni_static_field(mf, "COLOR_RANGE_LIMITED", jv_i(2));
  jni_static_field(mf, "COLOR_STANDARD_BT709", jv_i(1));
  jni_static_field(mf, "COLOR_STANDARD_BT601_PAL", jv_i(2));
  jni_static_field(mf, "COLOR_STANDARD_BT601_NTSC", jv_i(4));
  jni_static_field(mf, "COLOR_STANDARD_BT2020", jv_i(6));
  jni_static_field(mf, "COLOR_TRANSFER_SDR_VIDEO", jv_i(3));
  jni_static_field(cc, "COLOR_FormatYUV420Planar", jv_i(19));
  jni_static_field(cc, "COLOR_FormatYUV420PackedPlanar", jv_i(20));
  jni_static_field(cc, "COLOR_FormatYUV420SemiPlanar", jv_i(21));
  jni_static_field(cc, "COLOR_FormatYUV420PackedSemiPlanar", jv_i(39));
  jni_static_field(cc, "COLOR_QCOM_FormatYUV420SemiPlanar", jv_i(0x7fa30c00));
  jni_static_field(cc, "COLOR_TI_FormatYUV420PackedSemiPlanar", jv_i(0x7f000100));
  jni_static_field(cc, "COLOR_FormatYUV420Flexible", jv_i(0x7f420888));
  jni_static_field(cc, "COLOR_FormatSurface", jv_i(0x7f000789));
}

// libunity.so: AndroidVulkanVideo::IsSupported() -> false, so Unity decodes into raw buffers (above) instead of
// AImageReader hardware buffers, which vkdk cannot import
void media_ndk_unity_patch(uintptr_t unity_base) {
  const uintptr_t is_supported = unity_base + 0x9d5be0;
  if (*(const uint32_t *)is_supported != 0xd10603ffu) { // sub sp, sp, #0x180
    log_printf("[video] libunity.so is not the expected build: video stays off");
    return;
  }
  static const uint32_t ret_false[2] = { 0x52800000u, 0xd65f03c0u }; // mov w0, #0; ret
  so_write_code((void *)is_supported, ret_false, sizeof ret_false);
}
