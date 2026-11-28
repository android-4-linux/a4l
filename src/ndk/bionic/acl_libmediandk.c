// SPDX-License-Identifier: Apache-2.0
// Provenance: signatures and error values from the NDK <media/NdkMediaCodec.h>,
// <media/NdkMediaFormat.h> and <media/NdkMediaError.h> stable API (AOSP,
// Apache-2.0). No GPL-3.0 sources.
//
// Staged as bionic libmediandk.so. An app's native library that DT_NEEDs it
// does not load at all without one, and takes with it whatever else the
// library does: Spotify's libfull-jni-spotify.so holds its sign-in and its
// playback, and needs AMediaCodec and AMediaFormat.
//
// AMediaFormat is whole: a list of typed values by key. AMediaCodec has no
// codec behind it yet. The create functions return NULL, which the NDK
// documents for a codec that cannot be made, and every call says so once on
// stderr, so a run shows what an app asks for. AOSP's own libmediandk is a
// device library (Binder, mediaserver) and cannot stand in here.
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#define EXPORT __attribute__((visibility("default")))

typedef int32_t media_status_t;
#define AMEDIA_OK 0
#define AMEDIA_ERROR_UNSUPPORTED (-10002)
#define AMEDIA_ERROR_INVALID_OBJECT (-10003)
#define AMEDIA_ERROR_INVALID_PARAMETER (-10004)

static void say(const char* fmt, ...) {
  char line[256];
  int n = snprintf(line, sizeof line, "acl.mediandk: ");
  va_list ap;
  va_start(ap, fmt);
  n += vsnprintf(line + n, sizeof line - (size_t)n - 1, fmt, ap);
  va_end(ap);
  if (n > (int)sizeof line - 2)
    n = (int)sizeof line - 2;
  line[n++] = '\n';
  (void)!write(2, line, (size_t)n);
}

// ── AMediaFormat ────────────────────────────────────────────────────────────
enum { kInt32, kInt64, kFloat, kString, kBuffer };

typedef struct Entry {
  struct Entry* next;
  char* key;
  int type;
  union {
    int32_t i32;
    int64_t i64;
    float f;
  } v;
  void* data;  // kString: NUL-terminated; kBuffer: size bytes
  size_t size;
} Entry;

typedef struct AMediaFormat {
  Entry* head;
  char* text;  // AMediaFormat_toString's result, owned here
} AMediaFormat;

static Entry* find(const AMediaFormat* f, const char* key) {
  if (f == NULL || key == NULL)
    return NULL;
  for (Entry* e = f->head; e != NULL; e = e->next)
    if (strcmp(e->key, key) == 0)
      return e;
  return NULL;
}

// The entry for key, made if absent, with any old value released.
static Entry* slot(AMediaFormat* f, const char* key, int type) {
  if (f == NULL || key == NULL)
    return NULL;
  Entry* e = find(f, key);
  if (e == NULL) {
    e = calloc(1, sizeof *e);
    if (e == NULL)
      return NULL;
    e->key = strdup(key);
    if (e->key == NULL) {
      free(e);
      return NULL;
    }
    e->next = f->head;
    f->head = e;
  }
  free(e->data);
  e->data = NULL;
  e->size = 0;
  e->type = type;
  return e;
}

EXPORT AMediaFormat* AMediaFormat_new(void) {
  return calloc(1, sizeof(AMediaFormat));
}

EXPORT media_status_t AMediaFormat_delete(AMediaFormat* f) {
  if (f == NULL)
    return AMEDIA_OK;
  for (Entry* e = f->head; e != NULL;) {
    Entry* next = e->next;
    free(e->key);
    free(e->data);
    free(e);
    e = next;
  }
  free(f->text);
  free(f);
  return AMEDIA_OK;
}

EXPORT void AMediaFormat_setInt32(AMediaFormat* f, const char* key, int32_t v) {
  Entry* e = slot(f, key, kInt32);
  if (e != NULL)
    e->v.i32 = v;
}

EXPORT void AMediaFormat_setInt64(AMediaFormat* f, const char* key, int64_t v) {
  Entry* e = slot(f, key, kInt64);
  if (e != NULL)
    e->v.i64 = v;
}

EXPORT void AMediaFormat_setFloat(AMediaFormat* f, const char* key, float v) {
  Entry* e = slot(f, key, kFloat);
  if (e != NULL)
    e->v.f = v;
}

EXPORT void AMediaFormat_setString(AMediaFormat* f,
                                   const char* key,
                                   const char* v) {
  if (v == NULL)
    return;
  char* copy = strdup(v);
  if (copy == NULL)
    return;
  Entry* e = slot(f, key, kString);
  if (e == NULL) {
    free(copy);
    return;
  }
  e->data = copy;
  e->size = strlen(copy);
}

EXPORT void AMediaFormat_setBuffer(AMediaFormat* f,
                                   const char* key,
                                   const void* data,
                                   size_t size) {
  if (data == NULL && size != 0)
    return;
  void* copy = malloc(size != 0 ? size : 1);
  if (copy == NULL)
    return;
  if (size != 0)
    memcpy(copy, data, size);
  Entry* e = slot(f, key, kBuffer);
  if (e == NULL) {
    free(copy);
    return;
  }
  e->data = copy;
  e->size = size;
}

EXPORT bool AMediaFormat_getInt32(AMediaFormat* f,
                                  const char* key,
                                  int32_t* out) {
  const Entry* e = find(f, key);
  if (e == NULL || e->type != kInt32 || out == NULL)
    return false;
  *out = e->v.i32;
  return true;
}

EXPORT bool AMediaFormat_getInt64(AMediaFormat* f,
                                  const char* key,
                                  int64_t* out) {
  const Entry* e = find(f, key);
  if (e == NULL || e->type != kInt64 || out == NULL)
    return false;
  *out = e->v.i64;
  return true;
}

EXPORT bool AMediaFormat_getFloat(AMediaFormat* f,
                                  const char* key,
                                  float* out) {
  const Entry* e = find(f, key);
  if (e == NULL || e->type != kFloat || out == NULL)
    return false;
  *out = e->v.f;
  return true;
}

// The string stays the format's until the key is set again or the format is
// deleted, as the NDK has it.
EXPORT bool AMediaFormat_getString(AMediaFormat* f,
                                   const char* key,
                                   const char** out) {
  const Entry* e = find(f, key);
  if (e == NULL || e->type != kString || out == NULL)
    return false;
  *out = e->data;
  return true;
}

EXPORT bool AMediaFormat_getBuffer(AMediaFormat* f,
                                   const char* key,
                                   void** data,
                                   size_t* size) {
  const Entry* e = find(f, key);
  if (e == NULL || e->type != kBuffer || data == NULL || size == NULL)
    return false;
  *data = e->data;
  *size = e->size;
  return true;
}

EXPORT const char* AMediaFormat_toString(AMediaFormat* f) {
  if (f == NULL)
    return "";
  size_t cap = 64;
  for (const Entry* e = f->head; e != NULL; e = e->next)
    cap += strlen(e->key) + 48 + (e->type == kString ? e->size : 0);
  char* s = malloc(cap);
  if (s == NULL)
    return "";
  size_t n = 0;
  s[0] = '\0';
  for (const Entry* e = f->head; e != NULL; e = e->next) {
    const char* sep = n != 0 ? ", " : "";
    int w = 0;
    switch (e->type) {
      case kInt32:
        w = snprintf(s + n, cap - n, "%s%s: int32(%d)", sep, e->key, e->v.i32);
        break;
      case kInt64:
        w = snprintf(s + n, cap - n, "%s%s: int64(%lld)", sep, e->key,
                     (long long)e->v.i64);
        break;
      case kFloat:
        w = snprintf(s + n, cap - n, "%s%s: float(%f)", sep, e->key,
                     (double)e->v.f);
        break;
      case kString:
        w = snprintf(s + n, cap - n, "%s%s: string(%s)", sep, e->key,
                     (const char*)e->data);
        break;
      default:
        w = snprintf(s + n, cap - n, "%s%s: data(%zu bytes)", sep, e->key,
                     e->size);
        break;
    }
    if (w < 0 || (size_t)w >= cap - n)
      break;
    n += (size_t)w;
  }
  free(f->text);
  f->text = s;
  return s;
}

// ── AMEDIAFORMAT_KEY_*: the key names, as <media/NdkMediaFormat.h> declares
// them and with the values MediaFormat.java gives them. An app's library
// imports these as data, so one that is missing stops the library loading.
EXPORT const char* AMEDIAFORMAT_KEY_AAC_DRC_ATTENUATION_FACTOR =
    "aac-drc-cut-level";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_DRC_BOOST_FACTOR =
    "aac-drc-boost-level";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_DRC_HEAVY_COMPRESSION =
    "aac-drc-heavy-compression";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_DRC_TARGET_REFERENCE_LEVEL =
    "aac-target-ref-level";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_ENCODED_TARGET_LEVEL =
    "aac-encoded-target-level";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_MAX_OUTPUT_CHANNEL_COUNT =
    "aac-max-output-channel_count";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_PROFILE = "aac-profile";
EXPORT const char* AMEDIAFORMAT_KEY_AAC_SBR_MODE = "aac-sbr-mode";
EXPORT const char* AMEDIAFORMAT_KEY_ALBUM = "album";
EXPORT const char* AMEDIAFORMAT_KEY_ALBUMART = "albumart";
EXPORT const char* AMEDIAFORMAT_KEY_ALBUMARTIST = "albumartist";
EXPORT const char* AMEDIAFORMAT_KEY_ARTIST = "artist";
EXPORT const char* AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_INFO =
    "audio-presentation-info";
EXPORT const char* AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PRESENTATION_ID =
    "audio-presentation-presentation-id";
EXPORT const char* AMEDIAFORMAT_KEY_AUDIO_PRESENTATION_PROGRAM_ID =
    "audio-presentation-program-id";
EXPORT const char* AMEDIAFORMAT_KEY_AUDIO_SESSION_ID = "audio-session-id";
EXPORT const char* AMEDIAFORMAT_KEY_AUTHOR = "author";
EXPORT const char* AMEDIAFORMAT_KEY_BITRATE_MODE = "bitrate-mode";
EXPORT const char* AMEDIAFORMAT_KEY_BITS_PER_SAMPLE = "bits-per-sample";
EXPORT const char* AMEDIAFORMAT_KEY_BIT_RATE = "bitrate";
EXPORT const char* AMEDIAFORMAT_KEY_CAPTURE_RATE = "capture-rate";
EXPORT const char* AMEDIAFORMAT_KEY_CDTRACKNUMBER = "cdtracknum";
EXPORT const char* AMEDIAFORMAT_KEY_CHANNEL_COUNT = "channel-count";
EXPORT const char* AMEDIAFORMAT_KEY_CHANNEL_MASK = "channel-mask";
EXPORT const char* AMEDIAFORMAT_KEY_COLOR_FORMAT = "color-format";
EXPORT const char* AMEDIAFORMAT_KEY_COLOR_RANGE = "color-range";
EXPORT const char* AMEDIAFORMAT_KEY_COLOR_STANDARD = "color-standard";
EXPORT const char* AMEDIAFORMAT_KEY_COLOR_TRANSFER = "color-transfer";
EXPORT const char* AMEDIAFORMAT_KEY_COMPILATION = "compilation";
EXPORT const char* AMEDIAFORMAT_KEY_COMPLEXITY = "complexity";
EXPORT const char* AMEDIAFORMAT_KEY_COMPOSER = "composer";
EXPORT const char* AMEDIAFORMAT_KEY_CREATE_INPUT_SURFACE_SUSPENDED =
    "create-input-buffers-suspended";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_DEFAULT_IV_SIZE =
    "crypto-default-iv-size";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_BYTE_BLOCK =
    "crypto-encrypted-byte-block";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_ENCRYPTED_SIZES =
    "crypto-encrypted-sizes";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_IV = "crypto-iv";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_KEY = "crypto-key";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_MODE = "crypto-mode";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_PLAIN_SIZES = "crypto-plain-sizes";
EXPORT const char* AMEDIAFORMAT_KEY_CRYPTO_SKIP_BYTE_BLOCK =
    "crypto-skip-byte-block";
EXPORT const char* AMEDIAFORMAT_KEY_CSD = "csd";
EXPORT const char* AMEDIAFORMAT_KEY_CSD_0 = "csd-0";
EXPORT const char* AMEDIAFORMAT_KEY_CSD_1 = "csd-1";
EXPORT const char* AMEDIAFORMAT_KEY_CSD_2 = "csd-2";
EXPORT const char* AMEDIAFORMAT_KEY_CSD_AVC = "csd-avc";
EXPORT const char* AMEDIAFORMAT_KEY_CSD_HEVC = "csd-hevc";
EXPORT const char* AMEDIAFORMAT_KEY_D263 = "d263";
EXPORT const char* AMEDIAFORMAT_KEY_DATE = "date";
EXPORT const char* AMEDIAFORMAT_KEY_DISCNUMBER = "discnum";
EXPORT const char* AMEDIAFORMAT_KEY_DISPLAY_CROP = "crop";
EXPORT const char* AMEDIAFORMAT_KEY_DISPLAY_HEIGHT = "display-height";
EXPORT const char* AMEDIAFORMAT_KEY_DISPLAY_WIDTH = "display-width";
EXPORT const char* AMEDIAFORMAT_KEY_DURATION = "durationUs";
EXPORT const char* AMEDIAFORMAT_KEY_ENCODER_DELAY = "encoder-delay";
EXPORT const char* AMEDIAFORMAT_KEY_ENCODER_PADDING = "encoder-padding";
EXPORT const char* AMEDIAFORMAT_KEY_ESDS = "esds";
EXPORT const char* AMEDIAFORMAT_KEY_EXIF_OFFSET = "exif-offset";
EXPORT const char* AMEDIAFORMAT_KEY_EXIF_SIZE = "exif-size";
EXPORT const char* AMEDIAFORMAT_KEY_FLAC_COMPRESSION_LEVEL =
    "flac-compression-level";
EXPORT const char* AMEDIAFORMAT_KEY_FRAME_COUNT = "frame-count";
EXPORT const char* AMEDIAFORMAT_KEY_FRAME_RATE = "frame-rate";
EXPORT const char* AMEDIAFORMAT_KEY_GENRE = "genre";
EXPORT const char* AMEDIAFORMAT_KEY_GRID_COLUMNS = "grid-cols";
EXPORT const char* AMEDIAFORMAT_KEY_GRID_ROWS = "grid-rows";
EXPORT const char* AMEDIAFORMAT_KEY_HAPTIC_CHANNEL_COUNT =
    "haptic-channel-count";
EXPORT const char* AMEDIAFORMAT_KEY_HDR10_PLUS_INFO = "hdr10-plus-info";
EXPORT const char* AMEDIAFORMAT_KEY_HDR_STATIC_INFO = "hdr-static-info";
EXPORT const char* AMEDIAFORMAT_KEY_HEIGHT = "height";
EXPORT const char* AMEDIAFORMAT_KEY_ICC_PROFILE = "icc-profile";
EXPORT const char* AMEDIAFORMAT_KEY_INTRA_REFRESH_PERIOD =
    "intra-refresh-period";
EXPORT const char* AMEDIAFORMAT_KEY_IS_ADTS = "is-adts";
EXPORT const char* AMEDIAFORMAT_KEY_IS_AUTOSELECT = "is-autoselect";
EXPORT const char* AMEDIAFORMAT_KEY_IS_DEFAULT = "is-default";
EXPORT const char* AMEDIAFORMAT_KEY_IS_FORCED_SUBTITLE = "is-forced-subtitle";
EXPORT const char* AMEDIAFORMAT_KEY_IS_SYNC_FRAME = "is-sync-frame";
EXPORT const char* AMEDIAFORMAT_KEY_I_FRAME_INTERVAL = "i-frame-interval";
EXPORT const char* AMEDIAFORMAT_KEY_LANGUAGE = "language";
EXPORT const char* AMEDIAFORMAT_KEY_LAST_SAMPLE_INDEX_IN_CHUNK =
    "last-sample-index-in-chunk";
EXPORT const char* AMEDIAFORMAT_KEY_LATENCY = "latency";
EXPORT const char* AMEDIAFORMAT_KEY_LEVEL = "level";
EXPORT const char* AMEDIAFORMAT_KEY_LOCATION = "location";
EXPORT const char* AMEDIAFORMAT_KEY_LOOP = "loop";
EXPORT const char* AMEDIAFORMAT_KEY_LOW_LATENCY = "low-latency";
EXPORT const char* AMEDIAFORMAT_KEY_LYRICIST = "lyricist";
EXPORT const char* AMEDIAFORMAT_KEY_MANUFACTURER = "manufacturer";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_BIT_RATE = "max-bitrate";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_FPS_TO_ENCODER = "max-fps-to-encoder";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_HEIGHT = "max-height";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_INPUT_SIZE = "max-input-size";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_PTS_GAP_TO_ENCODER =
    "max-pts-gap-to-encoder";
EXPORT const char* AMEDIAFORMAT_KEY_MAX_WIDTH = "max-width";
EXPORT const char* AMEDIAFORMAT_KEY_MIME = "mime";
EXPORT const char* AMEDIAFORMAT_KEY_MPEG2_STREAM_HEADER = "mpeg2-stream-header";
EXPORT const char* AMEDIAFORMAT_KEY_MPEGH_COMPATIBLE_SETS =
    "mpegh-compatible-sets";
EXPORT const char* AMEDIAFORMAT_KEY_MPEGH_PROFILE_LEVEL_INDICATION =
    "mpegh-profile-level-indication";
EXPORT const char* AMEDIAFORMAT_KEY_MPEGH_REFERENCE_CHANNEL_LAYOUT =
    "mpegh-reference-channel-layout";
EXPORT const char* AMEDIAFORMAT_KEY_MPEG_USER_DATA = "mpeg-user-data";
EXPORT const char* AMEDIAFORMAT_KEY_OPERATING_RATE = "operating-rate";
EXPORT const char* AMEDIAFORMAT_KEY_PCM_BIG_ENDIAN = "pcm-big-endian";
EXPORT const char* AMEDIAFORMAT_KEY_PCM_ENCODING = "pcm-encoding";
EXPORT const char* AMEDIAFORMAT_KEY_PICTURE_TYPE = "picture-type";
EXPORT const char* AMEDIAFORMAT_KEY_PRIORITY = "priority";
EXPORT const char* AMEDIAFORMAT_KEY_PROFILE = "profile";
EXPORT const char* AMEDIAFORMAT_KEY_PSSH = "pssh";
EXPORT const char* AMEDIAFORMAT_KEY_PUSH_BLANK_BUFFERS_ON_STOP =
    "push-blank-buffers-on-shutdown";
EXPORT const char* AMEDIAFORMAT_KEY_REPEAT_PREVIOUS_FRAME_AFTER =
    "repeat-previous-frame-after";
EXPORT const char* AMEDIAFORMAT_KEY_ROTATION = "rotation-degrees";
EXPORT const char* AMEDIAFORMAT_KEY_SAMPLE_FILE_OFFSET = "sample-file-offset";
EXPORT const char* AMEDIAFORMAT_KEY_SAMPLE_RATE = "sample-rate";
EXPORT const char* AMEDIAFORMAT_KEY_SAMPLE_TIME_BEFORE_APPEND =
    "sample-time-before-append";
EXPORT const char* AMEDIAFORMAT_KEY_SAR_HEIGHT = "sar-height";
EXPORT const char* AMEDIAFORMAT_KEY_SAR_WIDTH = "sar-width";
EXPORT const char* AMEDIAFORMAT_KEY_SEI = "sei";
EXPORT const char* AMEDIAFORMAT_KEY_SLICE_HEIGHT = "slice-height";
EXPORT const char* AMEDIAFORMAT_KEY_SLOW_MOTION_MARKERS = "slow-motion-markers";
EXPORT const char* AMEDIAFORMAT_KEY_STRIDE = "stride";
EXPORT const char* AMEDIAFORMAT_KEY_TARGET_TIME = "target-time";
EXPORT const char* AMEDIAFORMAT_KEY_TEMPORAL_LAYERING = "ts-schema";
EXPORT const char* AMEDIAFORMAT_KEY_TEMPORAL_LAYER_COUNT =
    "temporal-layer-count";
EXPORT const char* AMEDIAFORMAT_KEY_TEMPORAL_LAYER_ID = "temporal-layer-id";
EXPORT const char* AMEDIAFORMAT_KEY_TEXT_FORMAT_DATA = "text-format-data";
EXPORT const char* AMEDIAFORMAT_KEY_THUMBNAIL_CSD_AV1C = "thumbnail-csd-av1c";
EXPORT const char* AMEDIAFORMAT_KEY_THUMBNAIL_CSD_HEVC = "thumbnail-csd-hevc";
EXPORT const char* AMEDIAFORMAT_KEY_THUMBNAIL_HEIGHT = "thumbnail-height";
EXPORT const char* AMEDIAFORMAT_KEY_THUMBNAIL_TIME = "thumbnail-time";
EXPORT const char* AMEDIAFORMAT_KEY_THUMBNAIL_WIDTH = "thumbnail-width";
EXPORT const char* AMEDIAFORMAT_KEY_TILE_HEIGHT = "tile-height";
EXPORT const char* AMEDIAFORMAT_KEY_TILE_WIDTH = "tile-width";
EXPORT const char* AMEDIAFORMAT_KEY_TIME_US = "timeUs";
EXPORT const char* AMEDIAFORMAT_KEY_TITLE = "title";
EXPORT const char* AMEDIAFORMAT_KEY_TRACK_ID = "track-id";
EXPORT const char* AMEDIAFORMAT_KEY_TRACK_INDEX = "track-index";
EXPORT const char* AMEDIAFORMAT_KEY_VALID_SAMPLES = "valid-samples";
EXPORT const char* AMEDIAFORMAT_KEY_VIDEO_ENCODING_STATISTICS_LEVEL =
    "video-encoding-statistics-level";
EXPORT const char* AMEDIAFORMAT_KEY_VIDEO_QP_AVERAGE = "video-qp-average";
EXPORT const char* AMEDIAFORMAT_KEY_WIDTH = "width";
EXPORT const char* AMEDIAFORMAT_KEY_XMP_OFFSET = "xmp-offset";
EXPORT const char* AMEDIAFORMAT_KEY_XMP_SIZE = "xmp-size";
EXPORT const char* AMEDIAFORMAT_KEY_YEAR = "year";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_B_MAX = "video-qp-b-max";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_B_MIN = "video-qp-b-min";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_I_MAX = "video-qp-i-max";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_I_MIN = "video-qp-i-min";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_MAX = "video-qp-max";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_MIN = "video-qp-min";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_P_MAX = "video-qp-p-max";
EXPORT const char* AMEDIAFORMAT_VIDEO_QP_P_MIN = "video-qp-p-min";

// ── AMediaCodec: no codec yet ───────────────────────────────────────────────
typedef struct AMediaCodec AMediaCodec;
typedef struct AMediaCrypto AMediaCrypto;
typedef struct ANativeWindow ANativeWindow;
typedef struct {
  int32_t offset;
  int32_t size;
  int64_t presentationTimeUs;
  uint32_t flags;
} AMediaCodecBufferInfo;

EXPORT AMediaCodec* AMediaCodec_createDecoderByType(const char* mime) {
  say("AMediaCodec_createDecoderByType(%s): no codec, NULL",
      mime != NULL ? mime : "(null)");
  return NULL;
}

EXPORT AMediaCodec* AMediaCodec_createEncoderByType(const char* mime) {
  say("AMediaCodec_createEncoderByType(%s): no codec, NULL",
      mime != NULL ? mime : "(null)");
  return NULL;
}

EXPORT AMediaCodec* AMediaCodec_createCodecByName(const char* name) {
  say("AMediaCodec_createCodecByName(%s): no codec, NULL",
      name != NULL ? name : "(null)");
  return NULL;
}

// No AMediaCodec is ever made, so each of these is reached with NULL or not
// at all.
#define NO_CODEC(name)    \
  say(name ": no codec"); \
  return AMEDIA_ERROR_INVALID_OBJECT

EXPORT media_status_t AMediaCodec_delete(AMediaCodec* c) {
  (void)c;
  return AMEDIA_OK;
}

EXPORT media_status_t AMediaCodec_configure(AMediaCodec* c,
                                            const AMediaFormat* format,
                                            ANativeWindow* surface,
                                            AMediaCrypto* crypto,
                                            uint32_t flags) {
  (void)c, (void)format, (void)surface, (void)crypto, (void)flags;
  NO_CODEC("AMediaCodec_configure");
}

EXPORT media_status_t AMediaCodec_start(AMediaCodec* c) {
  (void)c;
  NO_CODEC("AMediaCodec_start");
}

EXPORT media_status_t AMediaCodec_stop(AMediaCodec* c) {
  (void)c;
  NO_CODEC("AMediaCodec_stop");
}

EXPORT media_status_t AMediaCodec_flush(AMediaCodec* c) {
  (void)c;
  NO_CODEC("AMediaCodec_flush");
}

EXPORT uint8_t* AMediaCodec_getInputBuffer(AMediaCodec* c,
                                           size_t idx,
                                           size_t* out_size) {
  (void)c, (void)idx;
  if (out_size != NULL)
    *out_size = 0;
  say("AMediaCodec_getInputBuffer: no codec");
  return NULL;
}

EXPORT uint8_t* AMediaCodec_getOutputBuffer(AMediaCodec* c,
                                            size_t idx,
                                            size_t* out_size) {
  (void)c, (void)idx;
  if (out_size != NULL)
    *out_size = 0;
  say("AMediaCodec_getOutputBuffer: no codec");
  return NULL;
}

EXPORT ssize_t AMediaCodec_dequeueInputBuffer(AMediaCodec* c,
                                              int64_t timeoutUs) {
  (void)c, (void)timeoutUs;
  NO_CODEC("AMediaCodec_dequeueInputBuffer");
}

EXPORT media_status_t AMediaCodec_queueInputBuffer(AMediaCodec* c,
                                                   size_t idx,
                                                   off_t offset,
                                                   size_t size,
                                                   uint64_t time,
                                                   uint32_t flags) {
  (void)c, (void)idx, (void)offset, (void)size, (void)time, (void)flags;
  NO_CODEC("AMediaCodec_queueInputBuffer");
}

EXPORT ssize_t AMediaCodec_dequeueOutputBuffer(AMediaCodec* c,
                                               AMediaCodecBufferInfo* info,
                                               int64_t timeoutUs) {
  (void)c, (void)info, (void)timeoutUs;
  NO_CODEC("AMediaCodec_dequeueOutputBuffer");
}

EXPORT AMediaFormat* AMediaCodec_getOutputFormat(AMediaCodec* c) {
  (void)c;
  say("AMediaCodec_getOutputFormat: no codec");
  return NULL;
}

EXPORT media_status_t AMediaCodec_releaseOutputBuffer(AMediaCodec* c,
                                                      size_t idx,
                                                      bool render) {
  (void)c, (void)idx, (void)render;
  NO_CODEC("AMediaCodec_releaseOutputBuffer");
}

// The rest of <media/NdkMediaCodec.h>. A library that imports one of these and
// finds it missing does not load: Spotify 9's liborbit-jni-spotify.so imports
// AMediaCodec_releaseOutputBufferAtTime beside the calls above.
EXPORT media_status_t
AMediaCodec_releaseOutputBufferAtTime(AMediaCodec* c,
                                      size_t idx,
                                      int64_t timestampNs) {
  (void)c, (void)idx, (void)timestampNs;
  NO_CODEC("AMediaCodec_releaseOutputBufferAtTime");
}

EXPORT media_status_t AMediaCodec_setOutputSurface(AMediaCodec* c,
                                                   ANativeWindow* surface) {
  (void)c, (void)surface;
  NO_CODEC("AMediaCodec_setOutputSurface");
}

EXPORT media_status_t AMediaCodec_setParameters(AMediaCodec* c,
                                                const AMediaFormat* params) {
  (void)c, (void)params;
  NO_CODEC("AMediaCodec_setParameters");
}

EXPORT media_status_t AMediaCodec_createInputSurface(AMediaCodec* c,
                                                     ANativeWindow** surface) {
  (void)c;
  if (surface != NULL)
    *surface = NULL;
  NO_CODEC("AMediaCodec_createInputSurface");
}

EXPORT media_status_t
AMediaCodec_createPersistentInputSurface(ANativeWindow** surface) {
  if (surface != NULL)
    *surface = NULL;
  NO_CODEC("AMediaCodec_createPersistentInputSurface");
}

EXPORT media_status_t AMediaCodec_setInputSurface(AMediaCodec* c,
                                                  ANativeWindow* surface) {
  (void)c, (void)surface;
  NO_CODEC("AMediaCodec_setInputSurface");
}

EXPORT media_status_t AMediaCodec_signalEndOfInputStream(AMediaCodec* c) {
  (void)c;
  NO_CODEC("AMediaCodec_signalEndOfInputStream");
}

EXPORT AMediaFormat* AMediaCodec_getInputFormat(AMediaCodec* c) {
  (void)c;
  say("AMediaCodec_getInputFormat: no codec");
  return NULL;
}

EXPORT AMediaFormat* AMediaCodec_getBufferFormat(AMediaCodec* c, size_t idx) {
  (void)c, (void)idx;
  say("AMediaCodec_getBufferFormat: no codec");
  return NULL;
}

EXPORT media_status_t AMediaCodec_getName(AMediaCodec* c, char** out_name) {
  (void)c;
  if (out_name != NULL)
    *out_name = NULL;
  NO_CODEC("AMediaCodec_getName");
}

// What AMediaCodec_getName gave. It gives none, and free(NULL) is a no-op.
EXPORT void AMediaCodec_releaseName(AMediaCodec* c, char* name) {
  (void)c;
  free(name);
}

// By value: four function pointers.
typedef struct {
  void* onAsyncInputAvailable;
  void* onAsyncOutputAvailable;
  void* onAsyncFormatChanged;
  void* onAsyncError;
} AMediaCodecOnAsyncNotifyCallback;

EXPORT media_status_t
AMediaCodec_setAsyncNotifyCallback(AMediaCodec* c,
                                   AMediaCodecOnAsyncNotifyCallback callback,
                                   void* userdata) {
  (void)c, (void)callback, (void)userdata;
  NO_CODEC("AMediaCodec_setAsyncNotifyCallback");
}

EXPORT media_status_t AMediaCodec_setOnFrameRenderedCallback(AMediaCodec* c,
                                                             void* callback,
                                                             void* userdata) {
  (void)c, (void)callback, (void)userdata;
  NO_CODEC("AMediaCodec_setOnFrameRenderedCallback");
}

EXPORT media_status_t AMediaCodec_releaseCrypto(AMediaCodec* c) {
  (void)c;
  NO_CODEC("AMediaCodec_releaseCrypto");
}

// MediaCodec.CodecException's action codes: 1 is transient, 2 recoverable.
EXPORT bool AMediaCodecActionCode_isRecoverable(int32_t actionCode) {
  return actionCode == 2;
}

EXPORT bool AMediaCodecActionCode_isTransient(int32_t actionCode) {
  return actionCode == 1;
}

// ── AMediaCodecCryptoInfo ───────────────────────────────────────────────────
// Whole, as AMediaFormat is: what an encrypted sample's subsamples are, kept
// and given back. Nothing decrypts with it: there is no codec to queue to.
typedef struct {
  int32_t encryptBlocks;
  int32_t skipBlocks;
} cryptoinfo_pattern_t;

typedef struct AMediaCodecCryptoInfo {
  int numsubsamples;
  uint8_t key[16];
  uint8_t iv[16];
  int mode;  // cryptoinfo_mode_t
  cryptoinfo_pattern_t pattern;
  size_t* clearbytes;      // numsubsamples of them
  size_t* encryptedbytes;  // numsubsamples of them
} AMediaCodecCryptoInfo;

EXPORT AMediaCodecCryptoInfo* AMediaCodecCryptoInfo_new(
    int numsubsamples,
    uint8_t key[16],
    uint8_t iv[16],
    int mode,
    size_t* clearbytes,
    size_t* encryptedbytes) {
  if (numsubsamples < 0 || numsubsamples > (1 << 20))
    return NULL;
  AMediaCodecCryptoInfo* info = calloc(1, sizeof *info);
  if (info == NULL)
    return NULL;
  const size_t n = (size_t)numsubsamples;
  info->clearbytes = calloc(n > 0 ? n : 1, sizeof(size_t));
  info->encryptedbytes = calloc(n > 0 ? n : 1, sizeof(size_t));
  if (info->clearbytes == NULL || info->encryptedbytes == NULL) {
    free(info->clearbytes);
    free(info->encryptedbytes);
    free(info);
    return NULL;
  }
  info->numsubsamples = numsubsamples;
  info->mode = mode;
  if (key != NULL)
    memcpy(info->key, key, 16);
  if (iv != NULL)
    memcpy(info->iv, iv, 16);
  if (clearbytes != NULL)
    memcpy(info->clearbytes, clearbytes, n * sizeof(size_t));
  if (encryptedbytes != NULL)
    memcpy(info->encryptedbytes, encryptedbytes, n * sizeof(size_t));
  return info;
}

EXPORT media_status_t
AMediaCodecCryptoInfo_delete(AMediaCodecCryptoInfo* info) {
  if (info != NULL) {
    free(info->clearbytes);
    free(info->encryptedbytes);
    free(info);
  }
  return AMEDIA_OK;
}

EXPORT void AMediaCodecCryptoInfo_setPattern(AMediaCodecCryptoInfo* info,
                                             cryptoinfo_pattern_t* pattern) {
  if (info != NULL && pattern != NULL)
    info->pattern = *pattern;
}

EXPORT size_t
AMediaCodecCryptoInfo_getNumSubSamples(AMediaCodecCryptoInfo* info) {
  return info != NULL ? (size_t)info->numsubsamples : 0;
}

EXPORT media_status_t AMediaCodecCryptoInfo_getKey(AMediaCodecCryptoInfo* info,
                                                   uint8_t* dst) {
  if (info == NULL)
    return AMEDIA_ERROR_INVALID_OBJECT;
  if (dst == NULL)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  memcpy(dst, info->key, 16);
  return AMEDIA_OK;
}

EXPORT media_status_t AMediaCodecCryptoInfo_getIV(AMediaCodecCryptoInfo* info,
                                                  uint8_t* dst) {
  if (info == NULL)
    return AMEDIA_ERROR_INVALID_OBJECT;
  if (dst == NULL)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  memcpy(dst, info->iv, 16);
  return AMEDIA_OK;
}

EXPORT int AMediaCodecCryptoInfo_getMode(AMediaCodecCryptoInfo* info) {
  return info != NULL ? info->mode : AMEDIA_ERROR_INVALID_OBJECT;
}

EXPORT media_status_t
AMediaCodecCryptoInfo_getClearBytes(AMediaCodecCryptoInfo* info, size_t* dst) {
  if (info == NULL)
    return AMEDIA_ERROR_INVALID_OBJECT;
  if (dst == NULL)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  memcpy(dst, info->clearbytes, (size_t)info->numsubsamples * sizeof(size_t));
  return AMEDIA_OK;
}

EXPORT media_status_t
AMediaCodecCryptoInfo_getEncryptedBytes(AMediaCodecCryptoInfo* info,
                                        size_t* dst) {
  if (info == NULL)
    return AMEDIA_ERROR_INVALID_OBJECT;
  if (dst == NULL)
    return AMEDIA_ERROR_INVALID_PARAMETER;
  memcpy(dst, info->encryptedbytes,
         (size_t)info->numsubsamples * sizeof(size_t));
  return AMEDIA_OK;
}

EXPORT media_status_t
AMediaCodec_queueSecureInputBuffer(AMediaCodec* c,
                                   size_t idx,
                                   off_t offset,
                                   AMediaCodecCryptoInfo* info,
                                   uint64_t time,
                                   uint32_t flags) {
  (void)c, (void)idx, (void)offset, (void)info, (void)time, (void)flags;
  NO_CODEC("AMediaCodec_queueSecureInputBuffer");
}
