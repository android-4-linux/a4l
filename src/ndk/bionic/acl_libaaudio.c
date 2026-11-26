// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. The functions, types and constants are those of
// the NDK's public aaudio/AAudio.h (Apache-2.0); the behavior follows that
// header's documentation. No AOSP implementation was read or copied. No
// GPL-3.0 sources.
//
// a4l's libaaudio.so: AAudio output over a4l's audio track. TARGET code,
// compiled for bionic. Android's libaaudio talks to AudioFlinger over Binder;
// there is neither here. Chromium plays a page's sound through AAudio (it
// opens libaaudio.so itself), so without this a WebView was silent.
//
// What there is:
// - Output streams, 16-bit and float PCM, with a data callback or with
//   AAudioStream_write. The PCM goes to the host through acl_audio_out_ops
//   (libandroid.so), as 16-bit.
// - A stream with a data callback has a thread of its own that asks the app for
//   a buffer and queues it. The queue blocks while the output is full, which
//   is what paces the callbacks.
//
// What there is not:
// - Input. Opening an input stream fails with AAUDIO_ERROR_UNAVAILABLE.
// - Device selection, sharing and performance modes, session ids,
//   spatialization, capture policy: stored and reported back, nothing more.
// - 24-bit and 32-bit integer PCM.
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <aaudio/AAudio.h>

#include "acl_audio_out.h"

#define ACL_EXPORT __attribute__((visibility("default")))

// In libandroid.so: the host's output calls, or NULL where there is no host
// audio (a build without it).
extern const struct acl_audio_out_ops* acl_audio_out_ops;

// What a stream is opened with. A builder is these and nothing else.
struct AclParams {
  int32_t sample_rate;
  int32_t channels;
  aaudio_format_t format;
  aaudio_direction_t direction;
  int32_t capacity_frames;
  int32_t frames_per_callback;
  int32_t device_id;
  aaudio_sharing_mode_t sharing;
  aaudio_performance_mode_t performance;
  aaudio_usage_t usage;
  aaudio_content_type_t content;
  aaudio_spatialization_behavior_t spatialization;
  bool spatialized;
  aaudio_input_preset_t input_preset;
  aaudio_allowed_capture_policy_t capture_policy;
  aaudio_session_id_t session;
  bool privacy_sensitive;
  aaudio_channel_mask_t channel_mask;
  AAudioStream_dataCallback data_cb;
  void* data_user;
  AAudioStream_errorCallback error_cb;
  void* error_user;
};

struct AAudioStreamBuilderStruct {
  struct AclParams p;
};

struct AAudioStreamStruct {
  struct AclParams p;
  void* out;  // the host's output
  int32_t burst_frames;
  int32_t buffer_frames;

  pthread_mutex_t lock;  // state, and the thread's start and end
  pthread_cond_t changed;
  aaudio_stream_state_t state;
  bool thread_running;  // the callback thread exists and has not been joined
  bool thread_stop;     // it is asked to end
  pthread_t thread;
  int64_t frames_written;  // guarded by lock

  void* cb_buf;    // one callback's worth, in the stream's format
  int16_t* pcm16;  // the same converted, where the format is float
  int32_t cb_frames;
};

enum {
  kDefaultRate = 48000,
  kMaxChannels = 8,
  kMinRate = 4000,
  kMaxRate = 192000,
  kMaxCallbackFrames = 1 << 16,
};

static int acl_bytes_per_sample(aaudio_format_t f) {
  return f == AAUDIO_FORMAT_PCM_FLOAT ? 4 : 2;
}

// float [-1,1] to 16-bit, clipped.
static void acl_float_to_i16(const float* in, int16_t* out, size_t n) {
  for (size_t i = 0; i < n; i++) {
    float v = in[i] * 32768.0f;
    if (!(v > -32768.0f))  // also NaN
      v = -32768.0f;
    else if (v > 32767.0f)
      v = 32767.0f;
    out[i] = (int16_t)v;
  }
}

static void acl_set_state(AAudioStream* s, aaudio_stream_state_t st) {
  s->state = st;
  pthread_cond_broadcast(&s->changed);
}

// Queues `frames` frames of the stream's format. Returns the frames taken.
static int acl_queue(AAudioStream* s, const void* data, int32_t frames) {
  const int16_t* pcm = (const int16_t*)data;
  if (s->p.format == AAUDIO_FORMAT_PCM_FLOAT) {
    acl_float_to_i16((const float*)data, s->pcm16,
                     (size_t)frames * (size_t)s->p.channels);
    pcm = s->pcm16;
  }
  const int taken = acl_audio_out_ops->write(s->out, pcm, frames);
  if (taken > 0) {
    pthread_mutex_lock(&s->lock);
    s->frames_written += taken;
    pthread_mutex_unlock(&s->lock);
  }
  return taken;
}

static void* acl_callback_thread(void* arg) {
  AAudioStream* s = (AAudioStream*)arg;
  for (;;) {
    pthread_mutex_lock(&s->lock);
    const bool stop = s->thread_stop;
    pthread_mutex_unlock(&s->lock);
    if (stop)
      break;
    const aaudio_data_callback_result_t r =
        s->p.data_cb(s, s->p.data_user, s->cb_buf, s->cb_frames);
    // A short count means the output was stopped under the write.
    if (acl_queue(s, s->cb_buf, s->cb_frames) < s->cb_frames)
      break;
    if (r != AAUDIO_CALLBACK_RESULT_CONTINUE) {
      // The app is done: the stream stops as if it had asked.
      pthread_mutex_lock(&s->lock);
      if (!s->thread_stop)
        acl_set_state(s, AAUDIO_STREAM_STATE_STOPPED);
      pthread_mutex_unlock(&s->lock);
      break;
    }
  }
  return NULL;
}

// Ends the callback thread and waits for it. Not from that thread itself.
static void acl_end_thread(AAudioStream* s) {
  pthread_mutex_lock(&s->lock);
  const bool running = s->thread_running;
  s->thread_stop = true;
  pthread_mutex_unlock(&s->lock);
  if (!running || pthread_equal(pthread_self(), s->thread))
    return;
  pthread_join(s->thread, NULL);
  pthread_mutex_lock(&s->lock);
  s->thread_running = false;
  pthread_mutex_unlock(&s->lock);
}

// ── Text
ACL_EXPORT const char* AAudio_convertResultToText(aaudio_result_t r) {
  switch (r) {
    case AAUDIO_OK:
      return "AAUDIO_OK";
    case AAUDIO_ERROR_DISCONNECTED:
      return "AAUDIO_ERROR_DISCONNECTED";
    case AAUDIO_ERROR_ILLEGAL_ARGUMENT:
      return "AAUDIO_ERROR_ILLEGAL_ARGUMENT";
    case AAUDIO_ERROR_INTERNAL:
      return "AAUDIO_ERROR_INTERNAL";
    case AAUDIO_ERROR_INVALID_STATE:
      return "AAUDIO_ERROR_INVALID_STATE";
    case AAUDIO_ERROR_INVALID_HANDLE:
      return "AAUDIO_ERROR_INVALID_HANDLE";
    case AAUDIO_ERROR_UNIMPLEMENTED:
      return "AAUDIO_ERROR_UNIMPLEMENTED";
    case AAUDIO_ERROR_UNAVAILABLE:
      return "AAUDIO_ERROR_UNAVAILABLE";
    case AAUDIO_ERROR_NO_FREE_HANDLES:
      return "AAUDIO_ERROR_NO_FREE_HANDLES";
    case AAUDIO_ERROR_NO_MEMORY:
      return "AAUDIO_ERROR_NO_MEMORY";
    case AAUDIO_ERROR_NULL:
      return "AAUDIO_ERROR_NULL";
    case AAUDIO_ERROR_TIMEOUT:
      return "AAUDIO_ERROR_TIMEOUT";
    case AAUDIO_ERROR_WOULD_BLOCK:
      return "AAUDIO_ERROR_WOULD_BLOCK";
    case AAUDIO_ERROR_INVALID_FORMAT:
      return "AAUDIO_ERROR_INVALID_FORMAT";
    case AAUDIO_ERROR_OUT_OF_RANGE:
      return "AAUDIO_ERROR_OUT_OF_RANGE";
    case AAUDIO_ERROR_NO_SERVICE:
      return "AAUDIO_ERROR_NO_SERVICE";
    case AAUDIO_ERROR_INVALID_RATE:
      return "AAUDIO_ERROR_INVALID_RATE";
    default:
      return "Unrecognized AAudio error.";
  }
}

ACL_EXPORT const char* AAudio_convertStreamStateToText(
    aaudio_stream_state_t s) {
  switch (s) {
    case AAUDIO_STREAM_STATE_UNINITIALIZED:
      return "AAUDIO_STREAM_STATE_UNINITIALIZED";
    case AAUDIO_STREAM_STATE_UNKNOWN:
      return "AAUDIO_STREAM_STATE_UNKNOWN";
    case AAUDIO_STREAM_STATE_OPEN:
      return "AAUDIO_STREAM_STATE_OPEN";
    case AAUDIO_STREAM_STATE_STARTING:
      return "AAUDIO_STREAM_STATE_STARTING";
    case AAUDIO_STREAM_STATE_STARTED:
      return "AAUDIO_STREAM_STATE_STARTED";
    case AAUDIO_STREAM_STATE_PAUSING:
      return "AAUDIO_STREAM_STATE_PAUSING";
    case AAUDIO_STREAM_STATE_PAUSED:
      return "AAUDIO_STREAM_STATE_PAUSED";
    case AAUDIO_STREAM_STATE_FLUSHING:
      return "AAUDIO_STREAM_STATE_FLUSHING";
    case AAUDIO_STREAM_STATE_FLUSHED:
      return "AAUDIO_STREAM_STATE_FLUSHED";
    case AAUDIO_STREAM_STATE_STOPPING:
      return "AAUDIO_STREAM_STATE_STOPPING";
    case AAUDIO_STREAM_STATE_STOPPED:
      return "AAUDIO_STREAM_STATE_STOPPED";
    case AAUDIO_STREAM_STATE_CLOSING:
      return "AAUDIO_STREAM_STATE_CLOSING";
    case AAUDIO_STREAM_STATE_CLOSED:
      return "AAUDIO_STREAM_STATE_CLOSED";
    case AAUDIO_STREAM_STATE_DISCONNECTED:
      return "AAUDIO_STREAM_STATE_DISCONNECTED";
    default:
      return "Unrecognized AAudio state.";
  }
}

// ── Builder
ACL_EXPORT aaudio_result_t
AAudio_createStreamBuilder(AAudioStreamBuilder** builder) {
  if (builder == NULL)
    return AAUDIO_ERROR_NULL;
  AAudioStreamBuilder* b = (AAudioStreamBuilder*)calloc(1, sizeof(*b));
  if (b == NULL)
    return AAUDIO_ERROR_NO_MEMORY;
  b->p.direction = AAUDIO_DIRECTION_OUTPUT;
  b->p.sharing = AAUDIO_SHARING_MODE_SHARED;
  b->p.performance = AAUDIO_PERFORMANCE_MODE_NONE;
  b->p.usage = AAUDIO_USAGE_MEDIA;
  b->p.content = AAUDIO_CONTENT_TYPE_MUSIC;
  b->p.spatialization = AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO;
  b->p.input_preset = AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
  b->p.capture_policy = AAUDIO_ALLOW_CAPTURE_BY_ALL;
  b->p.session = AAUDIO_SESSION_ID_NONE;
  *builder = b;
  return AAUDIO_OK;
}

#define ACL_BUILDER_SET(fn, type, field)                         \
  ACL_EXPORT void fn(AAudioStreamBuilder* builder, type value) { \
    if (builder != NULL)                                         \
      builder->p.field = value;                                  \
  }
ACL_BUILDER_SET(AAudioStreamBuilder_setDeviceId, int32_t, device_id)
ACL_BUILDER_SET(AAudioStreamBuilder_setSampleRate, int32_t, sample_rate)
ACL_BUILDER_SET(AAudioStreamBuilder_setChannelCount, int32_t, channels)
ACL_BUILDER_SET(AAudioStreamBuilder_setSamplesPerFrame, int32_t, channels)
ACL_BUILDER_SET(AAudioStreamBuilder_setFormat, aaudio_format_t, format)
ACL_BUILDER_SET(AAudioStreamBuilder_setSharingMode,
                aaudio_sharing_mode_t,
                sharing)
ACL_BUILDER_SET(AAudioStreamBuilder_setDirection, aaudio_direction_t, direction)
ACL_BUILDER_SET(AAudioStreamBuilder_setBufferCapacityInFrames,
                int32_t,
                capacity_frames)
ACL_BUILDER_SET(AAudioStreamBuilder_setPerformanceMode,
                aaudio_performance_mode_t,
                performance)
ACL_BUILDER_SET(AAudioStreamBuilder_setUsage, aaudio_usage_t, usage)
ACL_BUILDER_SET(AAudioStreamBuilder_setContentType,
                aaudio_content_type_t,
                content)
ACL_BUILDER_SET(AAudioStreamBuilder_setSpatializationBehavior,
                aaudio_spatialization_behavior_t,
                spatialization)
ACL_BUILDER_SET(AAudioStreamBuilder_setIsContentSpatialized, bool, spatialized)
ACL_BUILDER_SET(AAudioStreamBuilder_setInputPreset,
                aaudio_input_preset_t,
                input_preset)
ACL_BUILDER_SET(AAudioStreamBuilder_setAllowedCapturePolicy,
                aaudio_allowed_capture_policy_t,
                capture_policy)
ACL_BUILDER_SET(AAudioStreamBuilder_setSessionId, aaudio_session_id_t, session)
ACL_BUILDER_SET(AAudioStreamBuilder_setPrivacySensitive,
                bool,
                privacy_sensitive)
ACL_BUILDER_SET(AAudioStreamBuilder_setFramesPerDataCallback,
                int32_t,
                frames_per_callback)

// A mask is its channels: the count is what the output is opened with.
ACL_EXPORT void AAudioStreamBuilder_setChannelMask(AAudioStreamBuilder* builder,
                                                   aaudio_channel_mask_t mask) {
  if (builder == NULL)
    return;
  builder->p.channel_mask = mask;
  builder->p.channels = __builtin_popcount((unsigned)mask);
}

// Who the stream is attributed to: nothing here asks.
ACL_EXPORT void AAudioStreamBuilder_setPackageName(AAudioStreamBuilder* builder,
                                                   const char* name) {
  (void)builder;
  (void)name;
}
ACL_EXPORT void AAudioStreamBuilder_setAttributionTag(
    AAudioStreamBuilder* builder,
    const char* tag) {
  (void)builder;
  (void)tag;
}

ACL_EXPORT void AAudioStreamBuilder_setDataCallback(
    AAudioStreamBuilder* builder,
    AAudioStream_dataCallback callback,
    void* userData) {
  if (builder == NULL)
    return;
  builder->p.data_cb = callback;
  builder->p.data_user = userData;
}

ACL_EXPORT void AAudioStreamBuilder_setErrorCallback(
    AAudioStreamBuilder* builder,
    AAudioStream_errorCallback callback,
    void* userData) {
  if (builder == NULL)
    return;
  builder->p.error_cb = callback;
  builder->p.error_user = userData;
}

static void acl_stream_free(AAudioStream* s) {
  free(s->cb_buf);
  free(s->pcm16);
  pthread_cond_destroy(&s->changed);
  pthread_mutex_destroy(&s->lock);
  free(s);
}

ACL_EXPORT aaudio_result_t
AAudioStreamBuilder_openStream(AAudioStreamBuilder* builder,
                               AAudioStream** stream) {
  if (builder == NULL || stream == NULL)
    return AAUDIO_ERROR_NULL;
  *stream = NULL;
  if (builder->p.direction != AAUDIO_DIRECTION_OUTPUT)
    return AAUDIO_ERROR_UNAVAILABLE;  // no input
  if (acl_audio_out_ops == NULL)
    return AAUDIO_ERROR_NO_SERVICE;

  struct AclParams p = builder->p;
  if (p.sample_rate == AAUDIO_UNSPECIFIED)
    p.sample_rate = kDefaultRate;
  if (p.channels == AAUDIO_UNSPECIFIED)
    p.channels = 2;
  if (p.format == AAUDIO_UNSPECIFIED)
    p.format = AAUDIO_FORMAT_PCM_FLOAT;
  if (p.sample_rate < kMinRate || p.sample_rate > kMaxRate)
    return AAUDIO_ERROR_INVALID_RATE;
  if (p.channels < 1 || p.channels > kMaxChannels)
    return AAUDIO_ERROR_OUT_OF_RANGE;
  if (p.format != AAUDIO_FORMAT_PCM_I16 && p.format != AAUDIO_FORMAT_PCM_FLOAT)
    return AAUDIO_ERROR_INVALID_FORMAT;
  if (p.frames_per_callback < 0 || p.frames_per_callback > kMaxCallbackFrames ||
      p.capacity_frames < 0)
    return AAUDIO_ERROR_OUT_OF_RANGE;

  AAudioStream* s = (AAudioStream*)calloc(1, sizeof(*s));
  if (s == NULL)
    return AAUDIO_ERROR_NO_MEMORY;
  s->p = p;
  pthread_mutex_init(&s->lock, NULL);
  // Timed waits are measured on CLOCK_MONOTONIC.
  pthread_condattr_t attr;
  pthread_condattr_init(&attr);
  pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
  pthread_cond_init(&s->changed, &attr);
  pthread_condattr_destroy(&attr);

  // 10 ms: what the app is told one burst is, and one callback's size where
  // it did not choose one.
  s->burst_frames = p.sample_rate / 100;
  s->cb_frames =
      p.frames_per_callback != 0 ? p.frames_per_callback : s->burst_frames;
  s->buffer_frames =
      p.capacity_frames != 0 ? p.capacity_frames : s->burst_frames * 4;
  s->p.capacity_frames = s->buffer_frames;

  // The larger of a callback and a burst: AAudioStream_write converts through
  // pcm16 a burst at a time.
  const size_t chunk =
      (size_t)(s->cb_frames > s->burst_frames ? s->cb_frames : s->burst_frames);
  const size_t samples = chunk * (size_t)p.channels;
  s->cb_buf = calloc(samples, (size_t)acl_bytes_per_sample(p.format));
  s->pcm16 = (int16_t*)calloc(samples, sizeof(int16_t));
  if (s->cb_buf == NULL || s->pcm16 == NULL) {
    acl_stream_free(s);
    return AAUDIO_ERROR_NO_MEMORY;
  }
  s->out = acl_audio_out_ops->open(p.sample_rate, p.channels);
  if (s->out == NULL) {
    acl_stream_free(s);
    return AAUDIO_ERROR_UNAVAILABLE;
  }
  s->state = AAUDIO_STREAM_STATE_OPEN;
  *stream = s;
  return AAUDIO_OK;
}

ACL_EXPORT aaudio_result_t
AAudioStreamBuilder_delete(AAudioStreamBuilder* builder) {
  if (builder == NULL)
    return AAUDIO_ERROR_NULL;
  free(builder);
  return AAUDIO_OK;
}

// ── Stream
ACL_EXPORT aaudio_result_t AAudioStream_requestStart(AAudioStream* s) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  pthread_mutex_lock(&s->lock);
  const aaudio_stream_state_t st = s->state;
  if (st == AAUDIO_STREAM_STATE_STARTED || st == AAUDIO_STREAM_STATE_STARTING ||
      st == AAUDIO_STREAM_STATE_CLOSED || st == AAUDIO_STREAM_STATE_CLOSING) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  const bool stale = s->thread_running;
  pthread_mutex_unlock(&s->lock);
  if (stale)
    acl_end_thread(s);  // one that ended by its callback's STOP

  acl_audio_out_ops->start(s->out);
  pthread_mutex_lock(&s->lock);
  s->thread_stop = false;
  if (s->p.data_cb != NULL) {
    if (pthread_create(&s->thread, NULL, acl_callback_thread, s) != 0) {
      pthread_mutex_unlock(&s->lock);
      acl_audio_out_ops->stop(s->out);
      return AAUDIO_ERROR_INTERNAL;
    }
    s->thread_running = true;
  }
  acl_set_state(s, AAUDIO_STREAM_STATE_STARTED);
  pthread_mutex_unlock(&s->lock);
  return AAUDIO_OK;
}

// Stop, pause and flush: the output stops and drops what it holds. A pause
// that kept it would need the output to hold its queue, and a4l's track drops
// it on stop; an app that resumes hears the next buffer it supplies.
static aaudio_result_t acl_halt(AAudioStream* s, aaudio_stream_state_t end) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  pthread_mutex_lock(&s->lock);
  if (s->state == AAUDIO_STREAM_STATE_CLOSED ||
      s->state == AAUDIO_STREAM_STATE_CLOSING) {
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_ERROR_INVALID_STATE;
  }
  s->thread_stop = true;
  pthread_mutex_unlock(&s->lock);
  // The stop lets a write the thread is blocked in return.
  acl_audio_out_ops->stop(s->out);
  acl_end_thread(s);
  pthread_mutex_lock(&s->lock);
  acl_set_state(s, end);
  pthread_mutex_unlock(&s->lock);
  return AAUDIO_OK;
}

ACL_EXPORT aaudio_result_t AAudioStream_requestStop(AAudioStream* s) {
  return acl_halt(s, AAUDIO_STREAM_STATE_STOPPED);
}
ACL_EXPORT aaudio_result_t AAudioStream_requestPause(AAudioStream* s) {
  return acl_halt(s, AAUDIO_STREAM_STATE_PAUSED);
}
ACL_EXPORT aaudio_result_t AAudioStream_requestFlush(AAudioStream* s) {
  return acl_halt(s, AAUDIO_STREAM_STATE_FLUSHED);
}

// release() gives up the device and leaves the handle; close() also frees it.
ACL_EXPORT aaudio_result_t AAudioStream_release(AAudioStream* s) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  pthread_mutex_lock(&s->lock);
  const bool closed = s->state == AAUDIO_STREAM_STATE_CLOSED;
  pthread_mutex_unlock(&s->lock);
  if (closed)
    return AAUDIO_OK;
  acl_halt(s, AAUDIO_STREAM_STATE_CLOSING);
  acl_audio_out_ops->close(s->out);
  s->out = NULL;
  pthread_mutex_lock(&s->lock);
  acl_set_state(s, AAUDIO_STREAM_STATE_CLOSED);
  pthread_mutex_unlock(&s->lock);
  return AAUDIO_OK;
}

ACL_EXPORT aaudio_result_t AAudioStream_close(AAudioStream* s) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  AAudioStream_release(s);
  // Closing from the stream's own callback would free what the thread is
  // running on; the header forbids it, and the handle is left to leak.
  pthread_mutex_lock(&s->lock);
  const bool own =
      s->thread_running && pthread_equal(pthread_self(), s->thread);
  pthread_mutex_unlock(&s->lock);
  if (!own)
    acl_stream_free(s);
  return AAUDIO_OK;
}

ACL_EXPORT aaudio_stream_state_t AAudioStream_getState(AAudioStream* s) {
  if (s == NULL)
    return AAUDIO_STREAM_STATE_UNKNOWN;
  pthread_mutex_lock(&s->lock);
  const aaudio_stream_state_t st = s->state;
  pthread_mutex_unlock(&s->lock);
  return st;
}

ACL_EXPORT aaudio_result_t
AAudioStream_waitForStateChange(AAudioStream* s,
                                aaudio_stream_state_t inputState,
                                aaudio_stream_state_t* nextState,
                                int64_t timeoutNanoseconds) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  struct timespec until;
  clock_gettime(CLOCK_MONOTONIC, &until);
  if (timeoutNanoseconds > 0) {
    until.tv_sec += (time_t)(timeoutNanoseconds / 1000000000LL);
    until.tv_nsec += (long)(timeoutNanoseconds % 1000000000LL);
    if (until.tv_nsec >= 1000000000L) {
      until.tv_sec += 1;
      until.tv_nsec -= 1000000000L;
    }
  }
  aaudio_result_t result = AAUDIO_OK;
  pthread_mutex_lock(&s->lock);
  while (s->state == inputState) {
    if (timeoutNanoseconds <= 0 ||
        pthread_cond_timedwait(&s->changed, &s->lock, &until) == ETIMEDOUT) {
      if (s->state == inputState)
        result = AAUDIO_ERROR_TIMEOUT;
      break;
    }
  }
  if (nextState != NULL)
    *nextState = s->state;
  pthread_mutex_unlock(&s->lock);
  return result;
}

ACL_EXPORT aaudio_result_t AAudioStream_read(AAudioStream* s,
                                             void* buffer,
                                             int32_t numFrames,
                                             int64_t timeoutNanoseconds) {
  (void)buffer;
  (void)numFrames;
  (void)timeoutNanoseconds;
  return s == NULL ? AAUDIO_ERROR_NULL : AAUDIO_ERROR_INVALID_STATE;  // output
}

// For a stream with no data callback. The write blocks until the output has
// taken the frames whatever the timeout says: the output has no timed write.
ACL_EXPORT aaudio_result_t AAudioStream_write(AAudioStream* s,
                                              const void* buffer,
                                              int32_t numFrames,
                                              int64_t timeoutNanoseconds) {
  (void)timeoutNanoseconds;
  if (s == NULL || buffer == NULL)
    return AAUDIO_ERROR_NULL;
  if (numFrames < 0)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  if (s->p.data_cb != NULL)
    return AAUDIO_ERROR_INVALID_STATE;  // the callback is what supplies data
  pthread_mutex_lock(&s->lock);
  const bool started = s->state == AAUDIO_STREAM_STATE_STARTED;
  pthread_mutex_unlock(&s->lock);
  if (!started)
    return AAUDIO_ERROR_INVALID_STATE;
  const size_t frame_bytes =
      (size_t)s->p.channels * (size_t)acl_bytes_per_sample(s->p.format);
  int32_t done = 0;
  while (done < numFrames) {
    int32_t n = numFrames - done;
    if (n > s->burst_frames)
      n = s->burst_frames;  // what pcm16 holds
    const int taken =
        acl_queue(s, (const uint8_t*)buffer + (size_t)done * frame_bytes, n);
    if (taken <= 0)
      break;
    done += taken;
    if (taken < n)
      break;
  }
  return done;
}

ACL_EXPORT aaudio_result_t
AAudioStream_setBufferSizeInFrames(AAudioStream* s, int32_t numFrames) {
  if (s == NULL)
    return AAUDIO_ERROR_NULL;
  if (numFrames < s->burst_frames)
    numFrames = s->burst_frames;
  if (numFrames > s->p.capacity_frames)
    numFrames = s->p.capacity_frames;
  s->buffer_frames = numFrames;
  return numFrames;
}

ACL_EXPORT int32_t AAudioStream_getBufferSizeInFrames(AAudioStream* s) {
  return s != NULL ? s->buffer_frames : 0;
}
ACL_EXPORT int32_t AAudioStream_getFramesPerBurst(AAudioStream* s) {
  return s != NULL ? s->burst_frames : 0;
}
ACL_EXPORT int32_t AAudioStream_getBufferCapacityInFrames(AAudioStream* s) {
  return s != NULL ? s->p.capacity_frames : 0;
}
ACL_EXPORT int32_t AAudioStream_getFramesPerDataCallback(AAudioStream* s) {
  return s != NULL ? s->p.frames_per_callback : 0;
}
ACL_EXPORT int32_t AAudioStream_getXRunCount(AAudioStream* s) {
  (void)s;
  return 0;  // not counted
}
ACL_EXPORT int32_t AAudioStream_getSampleRate(AAudioStream* s) {
  return s != NULL ? s->p.sample_rate : 0;
}
ACL_EXPORT int32_t AAudioStream_getChannelCount(AAudioStream* s) {
  return s != NULL ? s->p.channels : 0;
}
ACL_EXPORT int32_t AAudioStream_getSamplesPerFrame(AAudioStream* s) {
  return s != NULL ? s->p.channels : 0;
}
ACL_EXPORT int32_t AAudioStream_getDeviceId(AAudioStream* s) {
  return s != NULL ? s->p.device_id : 0;
}
ACL_EXPORT aaudio_format_t AAudioStream_getFormat(AAudioStream* s) {
  return s != NULL ? s->p.format : AAUDIO_FORMAT_INVALID;
}
ACL_EXPORT aaudio_sharing_mode_t AAudioStream_getSharingMode(AAudioStream* s) {
  return s != NULL ? s->p.sharing : AAUDIO_SHARING_MODE_SHARED;
}
ACL_EXPORT aaudio_performance_mode_t
AAudioStream_getPerformanceMode(AAudioStream* s) {
  return s != NULL ? s->p.performance : AAUDIO_PERFORMANCE_MODE_NONE;
}
ACL_EXPORT aaudio_direction_t AAudioStream_getDirection(AAudioStream* s) {
  (void)s;
  return AAUDIO_DIRECTION_OUTPUT;
}

ACL_EXPORT int64_t AAudioStream_getFramesWritten(AAudioStream* s) {
  if (s == NULL)
    return 0;
  pthread_mutex_lock(&s->lock);
  const int64_t n = s->frames_written;
  pthread_mutex_unlock(&s->lock);
  return n;
}

// Frames the device has played: the timestamp's position where there is one,
// what was written until then.
ACL_EXPORT int64_t AAudioStream_getFramesRead(AAudioStream* s) {
  if (s == NULL)
    return 0;
  int64_t frames = 0, ns = 0;
  if (s->out != NULL && acl_audio_out_ops->timestamp(s->out, &frames, &ns) != 0)
    return frames;
  return AAudioStream_getFramesWritten(s);
}

ACL_EXPORT aaudio_session_id_t AAudioStream_getSessionId(AAudioStream* s) {
  return s != NULL ? s->p.session : AAUDIO_SESSION_ID_NONE;
}

// The output's clock is CLOCK_MONOTONIC, the one the header says to ask for.
ACL_EXPORT aaudio_result_t AAudioStream_getTimestamp(AAudioStream* s,
                                                     clockid_t clockid,
                                                     int64_t* framePosition,
                                                     int64_t* timeNanoseconds) {
  if (s == NULL || framePosition == NULL || timeNanoseconds == NULL)
    return AAUDIO_ERROR_NULL;
  if (clockid != CLOCK_MONOTONIC)
    return AAUDIO_ERROR_ILLEGAL_ARGUMENT;
  if (s->out == NULL ||
      acl_audio_out_ops->timestamp(s->out, framePosition, timeNanoseconds) == 0)
    return AAUDIO_ERROR_INVALID_STATE;  // none yet: the stream has just started
  return AAUDIO_OK;
}

ACL_EXPORT aaudio_usage_t AAudioStream_getUsage(AAudioStream* s) {
  return s != NULL ? s->p.usage : AAUDIO_USAGE_MEDIA;
}
ACL_EXPORT aaudio_content_type_t AAudioStream_getContentType(AAudioStream* s) {
  return s != NULL ? s->p.content : AAUDIO_CONTENT_TYPE_MUSIC;
}
ACL_EXPORT aaudio_spatialization_behavior_t
AAudioStream_getSpatializationBehavior(AAudioStream* s) {
  return s != NULL ? s->p.spatialization : AAUDIO_SPATIALIZATION_BEHAVIOR_AUTO;
}
ACL_EXPORT bool AAudioStream_isContentSpatialized(AAudioStream* s) {
  return s != NULL && s->p.spatialized;
}
ACL_EXPORT aaudio_input_preset_t AAudioStream_getInputPreset(AAudioStream* s) {
  return s != NULL ? s->p.input_preset : AAUDIO_INPUT_PRESET_VOICE_RECOGNITION;
}
ACL_EXPORT aaudio_allowed_capture_policy_t
AAudioStream_getAllowedCapturePolicy(AAudioStream* s) {
  return s != NULL ? s->p.capture_policy : AAUDIO_ALLOW_CAPTURE_BY_ALL;
}
ACL_EXPORT bool AAudioStream_isPrivacySensitive(AAudioStream* s) {
  return s != NULL && s->p.privacy_sensitive;
}
ACL_EXPORT aaudio_channel_mask_t AAudioStream_getChannelMask(AAudioStream* s) {
  return s != NULL ? s->p.channel_mask : AAUDIO_UNSPECIFIED;
}
