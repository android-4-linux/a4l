// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l. The functions, types and constants are those of
// the NDK's public SLES/OpenSLES.h and SLES/OpenSLES_Android.h; the behavior
// follows the OpenSL ES 1.0.1 specification and those headers. No AOSP
// implementation was read or copied. No GPL-3.0 sources.
//
// a4l's libOpenSLES.so: an OpenSL ES engine with an audio recorder over a4l's
// audio recorder. TARGET code, compiled for bionic. Android's libOpenSLES
// talks to AudioFlinger over Binder; there is neither here. The library used
// to be the NDK's link stub, whose slCreateEngine returns without making an
// engine: WhatsApp's voice note recorder took the 0 it happened to return for
// success and called through the engine it did not get.
//
// What there is:
// - An engine object, with SL_IID_ENGINE.
// - An audio recorder: 16-bit PCM from the default audio input into an Android
//   simple buffer queue, with SL_IID_RECORD, the buffer queue under either of
//   its two names, and SL_IID_ANDROIDCONFIGURATION. A thread of the recorder's
//   own fills the app's buffers in the order they were queued and calls the
//   queue's callback after each. The PCM comes from the host through
//   acl_audio_in_ops (libandroid.so).
// - An output mix object, which apps make with the engine before anything
//   else. Nothing plays through it.
//
// What there is not:
// - An audio player. CreateAudioPlayer fails with
//   SL_RESULT_FEATURE_UNSUPPORTED, as do the other Create calls.
// - Recording in anything but 16-bit PCM; markers, position updates and the
//   duration limit (stored and reported back, no event is sent); asynchronous
//   Realize; effects; every other interface.
// - The interface IDs' values. An ID is compared by address, here and in apps;
//   the 16 bytes behind each are distinct and are not the ones Khronos
//   assigned.
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#include "acl_audio_in.h"

#define ACL_EXPORT __attribute__((visibility("default")))

// In libandroid.so: the host's input calls, or NULL where there is no host
// audio (a build without it).
extern const struct acl_audio_in_ops* acl_audio_in_ops;

// ---- interface IDs ----------------------------------------------------------

#define ACL_IID(name, a, b, c, d, n0, n1, n2, n3, n4, n5) \
  static const struct SLInterfaceID_ acl_iid_##name = {   \
      a, b, c, d, {n0, n1, n2, n3, n4, n5}};              \
  ACL_EXPORT const SLInterfaceID SL_IID_##name = &acl_iid_##name

// clang-format off
ACL_IID(3DCOMMIT, 0x4b75031e, 0x3292, 0xbce3, 0xd45a, 0x35, 0xc4, 0xc0, 0xb8, 0x4a, 0x9e);
ACL_IID(3DDOPPLER, 0x8e4c518e, 0xfd98, 0x6a2c, 0x48b5, 0x71, 0xd0, 0xc7, 0xc9, 0x16, 0xa0);
ACL_IID(3DGROUPING, 0xb7ebd5e4, 0xe3f9, 0x947b, 0xa2c3, 0x1d, 0xf8, 0x1a, 0x7d, 0xdc, 0x8b);
ACL_IID(3DLOCATION, 0xa9b10f78, 0x0caf, 0x7ee9, 0x6252, 0x81, 0xc7, 0x34, 0x20, 0xe5, 0x31);
ACL_IID(3DMACROSCOPIC, 0xc2860d7a, 0xc6dd, 0x2354, 0xdcf8, 0x3f, 0x56, 0xbb, 0x74, 0x40, 0x15);
ACL_IID(3DSOURCE, 0x50921d78, 0x5d25, 0xea8c, 0x0037, 0x4f, 0x88, 0x79, 0x01, 0x1a, 0x30);
ACL_IID(ANDROIDACOUSTICECHOCANCELLATION, 0xeb885130, 0x3016, 0xbfe7, 0x2399, 0x29, 0xe2, 0x6c, 0xcf, 0x12, 0x33);
ACL_IID(ANDROIDAUTOMATICGAINCONTROL, 0x9359b153, 0x6477, 0x5ce4, 0xb274, 0x90, 0x01, 0x07, 0xec, 0xd3, 0x2e);
ACL_IID(ANDROIDBUFFERQUEUESOURCE, 0xd1029904, 0xe659, 0x1b0a, 0x6a95, 0x5d, 0xdb, 0x3a, 0xcf, 0x8d, 0x98);
ACL_IID(ANDROIDCONFIGURATION, 0x511cdc84, 0x9f51, 0x4524, 0xaa51, 0xa8, 0x35, 0xfb, 0x0b, 0x9a, 0xc1);
ACL_IID(ANDROIDEFFECT, 0x1a1c2e51, 0x89b0, 0xacb5, 0xaca3, 0xdb, 0x7d, 0xa2, 0x38, 0x76, 0x66);
ACL_IID(ANDROIDEFFECTCAPABILITIES, 0xde38c666, 0x5860, 0x0587, 0x92aa, 0xf8, 0x04, 0x98, 0x9a, 0x61, 0x5c);
ACL_IID(ANDROIDEFFECTSEND, 0xc78f41b5, 0x0cff, 0xf21c, 0xa902, 0x51, 0x41, 0x0a, 0x71, 0xed, 0x6b);
ACL_IID(ANDROIDNOISESUPPRESSION, 0x102c432a, 0x6160, 0x09a9, 0xe3e5, 0x2d, 0x3d, 0x47, 0x2f, 0x3e, 0xbe);
ACL_IID(ANDROIDSIMPLEBUFFERQUEUE, 0x009a2cd8, 0x476b, 0x8f30, 0xfee2, 0x03, 0x2c, 0x00, 0xde, 0x4b, 0x05);
ACL_IID(AUDIODECODERCAPABILITIES, 0xf805870d, 0x02d4, 0x2a44, 0xc0bb, 0x7f, 0x61, 0xe1, 0xfc, 0x88, 0x9d);
ACL_IID(AUDIOENCODER, 0x0c96801f, 0x7c00, 0xd2fd, 0x0f39, 0x00, 0xe2, 0xf6, 0x82, 0xed, 0x97);
ACL_IID(AUDIOENCODERCAPABILITIES, 0x7d6e5599, 0xfdfa, 0xbb47, 0x51b3, 0x6f, 0x4e, 0xee, 0xbb, 0x65, 0xc6);
ACL_IID(AUDIOIODEVICECAPABILITIES, 0x9753c28e, 0xc134, 0x3d78, 0x1ed9, 0x65, 0x75, 0xe6, 0xff, 0x94, 0x86);
ACL_IID(BASSBOOST, 0x35ff7fa6, 0xe807, 0xe016, 0xac6c, 0x09, 0x84, 0x0b, 0xdd, 0x1c, 0x8c);
ACL_IID(BUFFERQUEUE, 0x02d5c45f, 0x5ef6, 0x572a, 0x1eaf, 0x9b, 0xcc, 0xc4, 0x7c, 0x14, 0x55);
ACL_IID(DEVICEVOLUME, 0x49608ff0, 0x1f3d, 0x0543, 0x0eca, 0x38, 0x76, 0x54, 0x89, 0x88, 0xe2);
ACL_IID(DYNAMICINTERFACEMANAGEMENT, 0x3062f900, 0x5063, 0x13ae, 0x6e0d, 0xd0, 0xb3, 0xb1, 0x7e, 0x49, 0x92);
ACL_IID(DYNAMICSOURCE, 0xe4377176, 0x0094, 0xed58, 0x4f0c, 0xbb, 0x13, 0x88, 0xa4, 0xc9, 0xf3);
ACL_IID(EFFECTSEND, 0x3368a901, 0xa212, 0xcd02, 0xc2be, 0x60, 0x7a, 0x65, 0xb9, 0x3b, 0xae);
ACL_IID(ENGINE, 0x6505aed2, 0xc26f, 0x85b2, 0xfed4, 0x9d, 0x84, 0xa4, 0x41, 0xeb, 0xbb);
ACL_IID(ENGINECAPABILITIES, 0xa27f8194, 0xaa2a, 0x54d5, 0xda17, 0x14, 0x0b, 0x1b, 0xf2, 0xbb, 0x7f);
ACL_IID(ENVIRONMENTALREVERB, 0x1a33bd32, 0x8270, 0x0242, 0xed57, 0x78, 0x47, 0x8f, 0x9a, 0x10, 0xdf);
ACL_IID(EQUALIZER, 0xb9c969be, 0xc116, 0xbe22, 0x32bc, 0x97, 0x0f, 0x71, 0x6c, 0x26, 0x75);
ACL_IID(LED, 0x9af05d90, 0x761c, 0x9ee6, 0x1bbb, 0x33, 0x02, 0x85, 0x06, 0x26, 0x45);
ACL_IID(METADATAEXTRACTION, 0x39ba3d06, 0x3a46, 0x0897, 0xcc83, 0x53, 0x44, 0xf1, 0xef, 0xba, 0xbe);
ACL_IID(METADATATRAVERSAL, 0xc10217ba, 0x9758, 0xccff, 0x6729, 0xca, 0x2f, 0xaf, 0x81, 0x7d, 0x8c);
ACL_IID(MIDIMESSAGE, 0x1a75e2ab, 0xf393, 0xf90f, 0xa6e5, 0x22, 0xe3, 0x75, 0xf5, 0x7a, 0x59);
ACL_IID(MIDIMUTESOLO, 0xa1a222ae, 0x275e, 0x7afe, 0x088c, 0x1e, 0xcb, 0xe2, 0xb6, 0x98, 0xac);
ACL_IID(MIDITEMPO, 0x0992565d, 0x4ad9, 0xf3a3, 0x1478, 0xa4, 0xab, 0x47, 0x71, 0x6a, 0x09);
ACL_IID(MIDITIME, 0x8a0c5c7c, 0xc2df, 0x0efb, 0x0cb1, 0x28, 0xe9, 0x59, 0x36, 0xa2, 0x54);
ACL_IID(MUTESOLO, 0x3566da3a, 0x73b0, 0x39b6, 0xb066, 0x19, 0x9c, 0xa1, 0x43, 0x4e, 0x7d);
ACL_IID(NULL, 0x0be5e83d, 0xbd70, 0x4e5a, 0x3e0d, 0x96, 0xbf, 0x46, 0x5c, 0xb4, 0xc0);
ACL_IID(OBJECT, 0x1248c97c, 0xf7f9, 0x07e2, 0x9f89, 0xd6, 0x60, 0x79, 0x66, 0x11, 0x89);
ACL_IID(OUTPUTMIX, 0x2493b5b3, 0x189a, 0x78f7, 0xb156, 0x6e, 0x5d, 0xea, 0xaf, 0xa0, 0x18);
ACL_IID(PITCH, 0xaf21c5a6, 0x54bc, 0xb868, 0x5106, 0x35, 0x72, 0xf7, 0x3f, 0x17, 0xb4);
ACL_IID(PLAY, 0x1bf672c3, 0xc891, 0x9bfb, 0x4179, 0x94, 0x03, 0x56, 0x41, 0xf1, 0x8b);
ACL_IID(PLAYBACKRATE, 0x1edf904b, 0x2e86, 0x63f8, 0x419e, 0x45, 0x8f, 0xa7, 0x2b, 0x04, 0xaa);
ACL_IID(PREFETCHSTATUS, 0x7711111c, 0x7ac6, 0x7a9c, 0xa046, 0x52, 0x23, 0xb0, 0xb3, 0x4e, 0x69);
ACL_IID(PRESETREVERB, 0xf0364341, 0xba9b, 0xff6a, 0xa5e1, 0x27, 0x99, 0x86, 0x92, 0x6a, 0xc8);
ACL_IID(RATEPITCH, 0xf344f301, 0xe416, 0x4242, 0x1a83, 0x78, 0xf2, 0x01, 0xaf, 0xe6, 0x19);
ACL_IID(RECORD, 0x56e0b5b7, 0x576e, 0xa5b9, 0x6789, 0xab, 0x88, 0x5d, 0x62, 0x8c, 0xa3);
ACL_IID(SEEK, 0x3064333c, 0xc6fc, 0xa703, 0x6866, 0xff, 0xdd, 0x44, 0xa4, 0x5a, 0x0b);
ACL_IID(THREADSYNC, 0x27516a54, 0x3380, 0x5237, 0x4309, 0x24, 0x6e, 0x0b, 0x24, 0xbf, 0x2d);
ACL_IID(VIBRA, 0x00ee6272, 0xcb29, 0x5249, 0x54a9, 0x86, 0x07, 0xf0, 0x6c, 0xf5, 0xc3);
ACL_IID(VIRTUALIZER, 0x71c9d1a9, 0xc20a, 0x407c, 0x86bb, 0xd4, 0x70, 0x22, 0xb3, 0x0b, 0xb8);
ACL_IID(VISUALIZATION, 0xd3882bb0, 0xda16, 0xadb9, 0x2a73, 0x83, 0xbf, 0x37, 0x73, 0x33, 0xce);
ACL_IID(VOLUME, 0x47b16bfe, 0x298f, 0x88fd, 0xa5ab, 0xe4, 0x9d, 0x04, 0x64, 0xcf, 0xb0);
// clang-format on

// ---- objects ----------------------------------------------------------------

enum AclKind { kEngine, kOutputMix, kRecorder };

enum {
  kMinRate = 4000,
  kMaxRate = 192000,
  kMaxChannels = 8,
  kMaxQueued = 256,
  // Frames asked of the host at a time: a read ends within this much of a
  // stop, and a buffer larger than it is filled in steps.
  kMaxReadMs = 20,
};

struct AclObject;

// An interface an app holds: a pointer to `vt` is the SL...Itf. `owner` gets
// back from it to the object.
struct AclItf {
  const void* vt;
  struct AclObject* owner;
};

struct AclBuffer {
  uint8_t* data;
  uint32_t bytes;  // whole frames
};

struct AclObject {
  struct AclItf object;  // first: an SLObjectItf is also the object
  enum AclKind kind;
  SLuint32 state;  // SL_OBJECT_STATE_*

  // An interface is there when the app listed it at creation, or it comes
  // with the kind.
  struct AclItf engine;
  struct AclItf output_mix;
  struct AclItf record;
  struct AclItf queue;
  struct AclItf config;
  bool has_queue;
  bool has_config;

  // A recorder.
  int rate;
  int channels;
  uint32_t max_buffers;
  void* in;  // the host's input, from Realize

  pthread_mutex_t lock;  // everything below
  pthread_cond_t changed;
  SLuint32 record_state;  // SL_RECORDSTATE_*
  bool thread_running;
  bool thread_stop;
  pthread_t thread;
  struct AclBuffer buffers[kMaxQueued];  // a ring
  uint32_t head;
  uint32_t count;
  uint32_t filled;      // bytes of the head buffer filled
  uint32_t generation;  // bumped by Clear
  uint32_t completed;   // buffers handed back
  uint64_t frames;      // frames recorded
  slAndroidSimpleBufferQueueCallback queue_cb;
  void* queue_ctx;
  slRecordCallback record_cb;
  void* record_ctx;
  SLuint32 event_mask;
  SLmillisecond marker;
  SLmillisecond period;
  SLmillisecond limit;
};

static struct AclObject* acl_owner(const void* itf) {
  return itf != NULL ? ((const struct AclItf*)itf)->owner : NULL;
}

static struct AclObject* acl_of_kind(const void* itf, enum AclKind kind) {
  struct AclObject* o = acl_owner(itf);
  return o != NULL && o->kind == kind ? o : NULL;
}

// ---- the recorder's thread --------------------------------------------------

static void* acl_record_thread(void* arg) {
  struct AclObject* o = (struct AclObject*)arg;
  const uint32_t frame_bytes = (uint32_t)o->channels * 2U;
  const int max_frames =
      o->rate * kMaxReadMs / 1000 > 0 ? o->rate * kMaxReadMs / 1000 : 1;
  pthread_mutex_lock(&o->lock);
  for (;;) {
    while (!o->thread_stop &&
           (o->record_state != SL_RECORDSTATE_RECORDING || o->count == 0))
      pthread_cond_wait(&o->changed, &o->lock);
    if (o->thread_stop)
      break;
    const struct AclBuffer buf = o->buffers[o->head];
    const uint32_t generation = o->generation;
    const uint32_t at = o->filled;
    int want = (int)((buf.bytes - at) / frame_bytes);
    if (want > max_frames)
      want = max_frames;
    pthread_mutex_unlock(&o->lock);

    // The app owns the buffer until its callback; Clear takes it back, and
    // what is read into it after that is the app's to ignore.
    const int got =
        acl_audio_in_ops->read(o->in, (int16_t*)(void*)(buf.data + at), want);
    if (got <= 0) {
      // Stopped under us, or the input is gone. Not a spin on a dead input.
      const struct timespec nap = {0, 2 * 1000 * 1000};
      nanosleep(&nap, NULL);
    }

    pthread_mutex_lock(&o->lock);
    if (got <= 0 || generation != o->generation)
      continue;
    o->frames += (uint64_t)got;
    o->filled += (uint32_t)got * frame_bytes;
    if (o->filled < buf.bytes)
      continue;
    o->head = (o->head + 1) % kMaxQueued;
    o->count--;
    o->filled = 0;
    o->completed++;
    const slAndroidSimpleBufferQueueCallback cb = o->queue_cb;
    void* ctx = o->queue_ctx;
    pthread_mutex_unlock(&o->lock);
    if (cb != NULL)
      cb((SLAndroidSimpleBufferQueueItf)&o->queue.vt, ctx);
    pthread_mutex_lock(&o->lock);
  }
  pthread_mutex_unlock(&o->lock);
  return NULL;
}

// ---- SLRecordItf ------------------------------------------------------------

static SLresult acl_set_record_state(SLRecordItf self, SLuint32 state) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || o->in == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  if (state != SL_RECORDSTATE_STOPPED && state != SL_RECORDSTATE_PAUSED &&
      state != SL_RECORDSTATE_RECORDING)
    return SL_RESULT_PARAMETER_INVALID;
  if (state == SL_RECORDSTATE_RECORDING) {
    // Started before the thread is told, so its first read finds an input
    // that is running.
    acl_audio_in_ops->start(o->in);
    pthread_mutex_lock(&o->lock);
    o->record_state = state;
    pthread_cond_broadcast(&o->changed);
    pthread_mutex_unlock(&o->lock);
    return SL_RESULT_SUCCESS;
  }
  pthread_mutex_lock(&o->lock);
  o->record_state = state;
  pthread_mutex_unlock(&o->lock);
  acl_audio_in_ops->stop(o->in);  // lets go the thread's read
  return SL_RESULT_SUCCESS;
}

static SLresult acl_get_record_state(SLRecordItf self, SLuint32* state) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || state == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  *state = o->record_state;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_set_duration_limit(SLRecordItf self, SLmillisecond ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->limit = ms;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_get_record_position(SLRecordItf self, SLmillisecond* ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || ms == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  *ms = (SLmillisecond)(o->frames * 1000U / (uint64_t)o->rate);
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_record_register_callback(SLRecordItf self,
                                             slRecordCallback cb,
                                             void* ctx) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->record_cb = cb;
  o->record_ctx = ctx;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_set_event_mask(SLRecordItf self, SLuint32 flags) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->event_mask = flags;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_get_event_mask(SLRecordItf self, SLuint32* flags) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || flags == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  *flags = o->event_mask;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_set_marker(SLRecordItf self, SLmillisecond ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->marker = ms;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_clear_marker(SLRecordItf self) {
  return acl_set_marker(self, SL_TIME_UNKNOWN);
}

static SLresult acl_get_marker(SLRecordItf self, SLmillisecond* ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || ms == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  *ms = o->marker;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_set_period(SLRecordItf self, SLmillisecond ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || ms == 0)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->period = ms;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_get_period(SLRecordItf self, SLmillisecond* ms) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || ms == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  *ms = o->period;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static const struct SLRecordItf_ kRecordVt = {
    .SetRecordState = acl_set_record_state,
    .GetRecordState = acl_get_record_state,
    .SetDurationLimit = acl_set_duration_limit,
    .GetPosition = acl_get_record_position,
    .RegisterCallback = acl_record_register_callback,
    .SetCallbackEventsMask = acl_set_event_mask,
    .GetCallbackEventsMask = acl_get_event_mask,
    .SetMarkerPosition = acl_set_marker,
    .ClearMarkerPosition = acl_clear_marker,
    .GetMarkerPosition = acl_get_marker,
    .SetPositionUpdatePeriod = acl_set_period,
    .GetPositionUpdatePeriod = acl_get_period,
};

// ---- SLAndroidSimpleBufferQueueItf ------------------------------------------

static SLresult acl_enqueue(SLAndroidSimpleBufferQueueItf self,
                            const void* buffer,
                            SLuint32 size) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || buffer == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  const uint32_t frame_bytes = (uint32_t)o->channels * 2U;
  const uint32_t bytes = size - size % frame_bytes;
  if (bytes == 0)
    return SL_RESULT_PARAMETER_INVALID;
  SLresult r = SL_RESULT_SUCCESS;
  pthread_mutex_lock(&o->lock);
  if (o->count >= o->max_buffers) {
    r = SL_RESULT_BUFFER_INSUFFICIENT;
  } else {
    struct AclBuffer* b = &o->buffers[(o->head + o->count) % kMaxQueued];
    // A recorder writes what the header calls a const buffer.
    b->data = (uint8_t*)(uintptr_t)buffer;
    b->bytes = bytes;
    o->count++;
    pthread_cond_broadcast(&o->changed);
  }
  pthread_mutex_unlock(&o->lock);
  return r;
}

static SLresult acl_queue_clear(SLAndroidSimpleBufferQueueItf self) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->count = 0;
  o->filled = 0;
  o->generation++;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_queue_state(SLAndroidSimpleBufferQueueItf self,
                                SLAndroidSimpleBufferQueueState* state) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL || state == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  state->count = o->count;
  state->index = o->completed;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static SLresult acl_queue_register_callback(
    SLAndroidSimpleBufferQueueItf self,
    slAndroidSimpleBufferQueueCallback cb,
    void* ctx) {
  struct AclObject* o = acl_of_kind(self, kRecorder);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  pthread_mutex_lock(&o->lock);
  o->queue_cb = cb;
  o->queue_ctx = ctx;
  pthread_mutex_unlock(&o->lock);
  return SL_RESULT_SUCCESS;
}

static const struct SLAndroidSimpleBufferQueueItf_ kQueueVt = {
    .Enqueue = acl_enqueue,
    .Clear = acl_queue_clear,
    .GetState = acl_queue_state,
    .RegisterCallback = acl_queue_register_callback,
};

// ---- SLAndroidConfigurationItf ----------------------------------------------

// A recording preset or a performance mode has nothing to choose between here.
static SLresult acl_set_configuration(SLAndroidConfigurationItf self,
                                      const SLchar* key,
                                      const void* value,
                                      SLuint32 size) {
  (void)size;
  if (acl_owner(self) == NULL || key == NULL || value == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_get_configuration(SLAndroidConfigurationItf self,
                                      const SLchar* key,
                                      SLuint32* size,
                                      void* value) {
  (void)value;
  if (acl_owner(self) == NULL || key == NULL || size == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_acquire_java_proxy(SLAndroidConfigurationItf self,
                                       SLuint32 type,
                                       jobject* proxy) {
  (void)self;
  (void)type;
  if (proxy != NULL)
    *proxy = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_release_java_proxy(SLAndroidConfigurationItf self,
                                       SLuint32 type) {
  (void)self;
  (void)type;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLAndroidConfigurationItf_ kConfigVt = {
    .SetConfiguration = acl_set_configuration,
    .GetConfiguration = acl_get_configuration,
    .AcquireJavaProxy = acl_acquire_java_proxy,
    .ReleaseJavaProxy = acl_release_java_proxy,
};

// ---- SLOutputMixItf ---------------------------------------------------------

static SLresult acl_mix_devices(SLOutputMixItf self,
                                SLint32* count,
                                SLuint32* ids) {
  if (acl_of_kind(self, kOutputMix) == NULL || count == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  if (ids != NULL) {
    if (*count < 1)
      return SL_RESULT_BUFFER_INSUFFICIENT;
    ids[0] = SL_DEFAULTDEVICEID_AUDIOOUTPUT;
  }
  *count = 1;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_mix_register_callback(SLOutputMixItf self,
                                          slMixDeviceChangeCallback cb,
                                          void* ctx) {
  (void)cb;
  (void)ctx;
  return acl_of_kind(self, kOutputMix) != NULL ? SL_RESULT_SUCCESS
                                               : SL_RESULT_PARAMETER_INVALID;
}

static SLresult acl_mix_reroute(SLOutputMixItf self,
                                SLint32 count,
                                SLuint32* ids) {
  (void)self;
  (void)count;
  (void)ids;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static const struct SLOutputMixItf_ kOutputMixVt = {
    .GetDestinationOutputDeviceIDs = acl_mix_devices,
    .RegisterDeviceChangeCallback = acl_mix_register_callback,
    .ReRoute = acl_mix_reroute,
};

// ---- SLObjectItf ------------------------------------------------------------

static SLresult acl_realize(SLObjectItf self, SLboolean async) {
  struct AclObject* o = acl_owner(self);
  if (o == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  if (async)
    return SL_RESULT_FEATURE_UNSUPPORTED;
  if (o->state != SL_OBJECT_STATE_UNREALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  if (o->kind == kRecorder) {
    if (acl_audio_in_ops == NULL)
      return SL_RESULT_RESOURCE_ERROR;
    o->in = acl_audio_in_ops->open(o->rate, o->channels);
    if (o->in == NULL)
      return SL_RESULT_RESOURCE_ERROR;
    if (pthread_create(&o->thread, NULL, acl_record_thread, o) != 0) {
      acl_audio_in_ops->close(o->in);
      o->in = NULL;
      return SL_RESULT_RESOURCE_ERROR;
    }
    o->thread_running = true;
  }
  o->state = SL_OBJECT_STATE_REALIZED;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_resume(SLObjectItf self, SLboolean async) {
  (void)async;
  // Nothing here is ever suspended.
  return acl_owner(self) != NULL ? SL_RESULT_PRECONDITIONS_VIOLATED
                                 : SL_RESULT_PARAMETER_INVALID;
}

static SLresult acl_object_state(SLObjectItf self, SLuint32* state) {
  struct AclObject* o = acl_owner(self);
  if (o == NULL || state == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *state = o->state;
  return SL_RESULT_SUCCESS;
}

static bool acl_is_queue_iid(SLInterfaceID iid) {
  return iid == SL_IID_ANDROIDSIMPLEBUFFERQUEUE || iid == SL_IID_BUFFERQUEUE;
}

static SLresult acl_get_interface(SLObjectItf self,
                                  const SLInterfaceID iid,
                                  void* out) {
  struct AclObject* o = acl_owner(self);
  if (o == NULL || iid == NULL || out == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *(void**)out = NULL;
  if (o->state != SL_OBJECT_STATE_REALIZED)
    return SL_RESULT_PRECONDITIONS_VIOLATED;
  const struct AclItf* itf = NULL;
  if (iid == SL_IID_OBJECT)
    itf = &o->object;
  else if (o->kind == kEngine && iid == SL_IID_ENGINE)
    itf = &o->engine;
  else if (o->kind == kOutputMix && iid == SL_IID_OUTPUTMIX)
    itf = &o->output_mix;
  else if (o->kind == kRecorder && iid == SL_IID_RECORD)
    itf = &o->record;
  else if (o->kind == kRecorder && o->has_queue && acl_is_queue_iid(iid))
    itf = &o->queue;
  else if (o->kind == kRecorder && o->has_config &&
           iid == SL_IID_ANDROIDCONFIGURATION)
    itf = &o->config;
  if (itf == NULL)
    return SL_RESULT_FEATURE_UNSUPPORTED;
  *(const void**)out = &itf->vt;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_object_register_callback(SLObjectItf self,
                                             slObjectCallback cb,
                                             void* ctx) {
  (void)cb;
  (void)ctx;
  // Its events are for asynchronous operations and lost resources; there are
  // neither.
  return acl_owner(self) != NULL ? SL_RESULT_SUCCESS
                                 : SL_RESULT_PARAMETER_INVALID;
}

static void acl_abort_async(SLObjectItf self) {
  (void)self;
}

// Not from the recorder's own callback: this waits for the thread that is
// making it.
static void acl_destroy(SLObjectItf self) {
  struct AclObject* o = acl_owner(self);
  if (o == NULL)
    return;
  if (o->kind == kRecorder && o->in != NULL) {
    pthread_mutex_lock(&o->lock);
    o->record_state = SL_RECORDSTATE_STOPPED;
    o->thread_stop = true;
    pthread_cond_broadcast(&o->changed);
    const bool running = o->thread_running;
    pthread_mutex_unlock(&o->lock);
    acl_audio_in_ops->stop(o->in);
    if (running && !pthread_equal(pthread_self(), o->thread))
      pthread_join(o->thread, NULL);
    acl_audio_in_ops->close(o->in);
  }
  pthread_cond_destroy(&o->changed);
  pthread_mutex_destroy(&o->lock);
  free(o);
}

static SLresult acl_set_priority(SLObjectItf self,
                                 SLint32 priority,
                                 SLboolean preemptable) {
  (void)priority;
  (void)preemptable;
  return acl_owner(self) != NULL ? SL_RESULT_SUCCESS
                                 : SL_RESULT_PARAMETER_INVALID;
}

static SLresult acl_get_priority(SLObjectItf self,
                                 SLint32* priority,
                                 SLboolean* preemptable) {
  if (acl_owner(self) == NULL || priority == NULL || preemptable == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *priority = SL_PRIORITY_NORMAL;
  *preemptable = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_set_loss_of_control(SLObjectItf self,
                                        SLint16 count,
                                        SLInterfaceID* ids,
                                        SLboolean enabled) {
  (void)count;
  (void)ids;
  (void)enabled;
  return acl_owner(self) != NULL ? SL_RESULT_FEATURE_UNSUPPORTED
                                 : SL_RESULT_PARAMETER_INVALID;
}

static const struct SLObjectItf_ kObjectVt = {
    .Realize = acl_realize,
    .Resume = acl_resume,
    .GetState = acl_object_state,
    .GetInterface = acl_get_interface,
    .RegisterCallback = acl_object_register_callback,
    .AbortAsyncOperation = acl_abort_async,
    .Destroy = acl_destroy,
    .SetPriority = acl_set_priority,
    .GetPriority = acl_get_priority,
    .SetLossOfControlInterfaces = acl_set_loss_of_control,
};

static const struct SLEngineItf_ kEngineVt;

static struct AclObject* acl_new_object(enum AclKind kind) {
  struct AclObject* o = (struct AclObject*)calloc(1, sizeof *o);
  if (o == NULL)
    return NULL;
  o->kind = kind;
  o->state = SL_OBJECT_STATE_UNREALIZED;
  o->object.vt = &kObjectVt;
  o->engine.vt = &kEngineVt;
  o->output_mix.vt = &kOutputMixVt;
  o->record.vt = &kRecordVt;
  o->queue.vt = &kQueueVt;
  o->config.vt = &kConfigVt;
  o->object.owner = o->engine.owner = o->output_mix.owner = o;
  o->record.owner = o->queue.owner = o->config.owner = o;
  o->record_state = SL_RECORDSTATE_STOPPED;
  o->marker = SL_TIME_UNKNOWN;
  o->period = 1000;
  o->limit = SL_TIME_UNKNOWN;
  pthread_mutex_init(&o->lock, NULL);
  pthread_cond_init(&o->changed, NULL);
  return o;
}

// ---- SLEngineItf ------------------------------------------------------------

// The interfaces an app lists at creation. One it requires and the kind does
// not have fails the creation; one it does not require is left out.
static SLresult acl_take_interfaces(struct AclObject* o,
                                    SLuint32 count,
                                    const SLInterfaceID* ids,
                                    const SLboolean* required) {
  if (count != 0 && (ids == NULL || required == NULL))
    return SL_RESULT_PARAMETER_INVALID;
  for (SLuint32 i = 0; i < count; i++) {
    const SLInterfaceID iid = ids[i];
    bool known = iid == SL_IID_OBJECT;
    if (o->kind == kOutputMix)
      known = known || iid == SL_IID_OUTPUTMIX;
    if (o->kind == kRecorder) {
      if (iid == SL_IID_RECORD) {
        known = true;
      } else if (acl_is_queue_iid(iid)) {
        known = o->has_queue = true;
      } else if (iid == SL_IID_ANDROIDCONFIGURATION) {
        known = o->has_config = true;
      }
    }
    if (!known && required[i])
      return SL_RESULT_FEATURE_UNSUPPORTED;
  }
  return SL_RESULT_SUCCESS;
}

static SLresult acl_create_audio_recorder(SLEngineItf self,
                                          SLObjectItf* recorder,
                                          SLDataSource* source,
                                          SLDataSink* sink,
                                          SLuint32 count,
                                          const SLInterfaceID* ids,
                                          const SLboolean* required) {
  if (recorder == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *recorder = NULL;
  if (acl_of_kind(self, kEngine) == NULL || source == NULL || sink == NULL ||
      source->pLocator == NULL || sink->pLocator == NULL ||
      sink->pFormat == NULL)
    return SL_RESULT_PARAMETER_INVALID;

  const SLDataLocator_IODevice* device =
      (const SLDataLocator_IODevice*)source->pLocator;
  if (device->locatorType != SL_DATALOCATOR_IODEVICE ||
      device->deviceType != SL_IODEVICE_AUDIOINPUT)
    return SL_RESULT_CONTENT_UNSUPPORTED;
  const SLDataLocator_AndroidSimpleBufferQueue* queue =
      (const SLDataLocator_AndroidSimpleBufferQueue*)sink->pLocator;
  if (queue->locatorType != SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE &&
      queue->locatorType != SL_DATALOCATOR_BUFFERQUEUE)
    return SL_RESULT_CONTENT_UNSUPPORTED;
  if (queue->numBuffers == 0 || queue->numBuffers > kMaxQueued)
    return SL_RESULT_PARAMETER_INVALID;

  // The first fields of SLAndroidDataFormat_PCM_EX are SLDataFormat_PCM's, and
  // its sample rate is in milliHertz too.
  const SLDataFormat_PCM* pcm = (const SLDataFormat_PCM*)sink->pFormat;
  if (pcm->formatType == SL_ANDROID_DATAFORMAT_PCM_EX) {
    const SLAndroidDataFormat_PCM_EX* ex =
        (const SLAndroidDataFormat_PCM_EX*)sink->pFormat;
    if (ex->representation != SL_ANDROID_PCM_REPRESENTATION_SIGNED_INT)
      return SL_RESULT_CONTENT_UNSUPPORTED;
  } else if (pcm->formatType != SL_DATAFORMAT_PCM) {
    return SL_RESULT_CONTENT_UNSUPPORTED;
  }
  if (pcm->bitsPerSample != SL_PCMSAMPLEFORMAT_FIXED_16 ||
      pcm->containerSize != 16 || pcm->endianness != SL_BYTEORDER_LITTLEENDIAN)
    return SL_RESULT_CONTENT_UNSUPPORTED;
  if (pcm->numChannels < 1 || pcm->numChannels > kMaxChannels ||
      pcm->samplesPerSec < (SLuint32)kMinRate * 1000U ||
      pcm->samplesPerSec > (SLuint32)kMaxRate * 1000U)
    return SL_RESULT_CONTENT_UNSUPPORTED;

  struct AclObject* o = acl_new_object(kRecorder);
  if (o == NULL)
    return SL_RESULT_MEMORY_FAILURE;
  o->rate = (int)(pcm->samplesPerSec / 1000U);
  o->channels = (int)pcm->numChannels;
  o->max_buffers = queue->numBuffers;
  // The sink is a buffer queue, so the recorder has one whether or not the
  // app also lists the interface.
  o->has_queue = true;
  const SLresult r = acl_take_interfaces(o, count, ids, required);
  if (r != SL_RESULT_SUCCESS) {
    acl_destroy((SLObjectItf)&o->object.vt);
    return r;
  }
  *recorder = (SLObjectItf)&o->object.vt;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_create_output_mix(SLEngineItf self,
                                      SLObjectItf* mix,
                                      SLuint32 count,
                                      const SLInterfaceID* ids,
                                      const SLboolean* required) {
  if (mix == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *mix = NULL;
  if (acl_of_kind(self, kEngine) == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  struct AclObject* o = acl_new_object(kOutputMix);
  if (o == NULL)
    return SL_RESULT_MEMORY_FAILURE;
  const SLresult r = acl_take_interfaces(o, count, ids, required);
  if (r != SL_RESULT_SUCCESS) {
    acl_destroy((SLObjectItf)&o->object.vt);
    return r;
  }
  *mix = (SLObjectItf)&o->object.vt;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_no_device(SLEngineItf self,
                              SLObjectItf* object,
                              SLuint32 device,
                              SLuint32 count,
                              const SLInterfaceID* ids,
                              const SLboolean* required) {
  (void)self;
  (void)device;
  (void)count;
  (void)ids;
  (void)required;
  if (object != NULL)
    *object = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_no_audio_player(SLEngineItf self,
                                    SLObjectItf* player,
                                    SLDataSource* source,
                                    SLDataSink* sink,
                                    SLuint32 count,
                                    const SLInterfaceID* ids,
                                    const SLboolean* required) {
  (void)self;
  (void)source;
  (void)sink;
  (void)count;
  (void)ids;
  (void)required;
  if (player != NULL)
    *player = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_no_midi_player(SLEngineItf self,
                                   SLObjectItf* player,
                                   SLDataSource* midi,
                                   SLDataSource* bank,
                                   SLDataSink* audio,
                                   SLDataSink* vibra,
                                   SLDataSink* led,
                                   SLuint32 count,
                                   const SLInterfaceID* ids,
                                   const SLboolean* required) {
  (void)self;
  (void)midi;
  (void)bank;
  (void)audio;
  (void)vibra;
  (void)led;
  (void)count;
  (void)ids;
  (void)required;
  if (player != NULL)
    *player = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_no_listener(SLEngineItf self,
                                SLObjectItf* object,
                                SLuint32 count,
                                const SLInterfaceID* ids,
                                const SLboolean* required) {
  (void)self;
  (void)count;
  (void)ids;
  (void)required;
  if (object != NULL)
    *object = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_no_metadata_extractor(SLEngineItf self,
                                          SLObjectItf* object,
                                          SLDataSource* source,
                                          SLuint32 count,
                                          const SLInterfaceID* ids,
                                          const SLboolean* required) {
  (void)self;
  (void)source;
  (void)count;
  (void)ids;
  (void)required;
  if (object != NULL)
    *object = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_no_extension_object(SLEngineItf self,
                                        SLObjectItf* object,
                                        void* parameters,
                                        SLuint32 id,
                                        SLuint32 count,
                                        const SLInterfaceID* ids,
                                        const SLboolean* required) {
  (void)self;
  (void)parameters;
  (void)id;
  (void)count;
  (void)ids;
  (void)required;
  if (object != NULL)
    *object = NULL;
  return SL_RESULT_FEATURE_UNSUPPORTED;
}

// The interfaces of each kind of object, for the two Query calls.
static SLuint32 acl_supported(SLuint32 object_id, SLInterfaceID out[4]) {
  switch (object_id) {
    case SL_OBJECTID_ENGINE:
      out[0] = SL_IID_OBJECT;
      out[1] = SL_IID_ENGINE;
      return 2;
    case SL_OBJECTID_OUTPUTMIX:
      out[0] = SL_IID_OBJECT;
      out[1] = SL_IID_OUTPUTMIX;
      return 2;
    case SL_OBJECTID_AUDIORECORDER:
      out[0] = SL_IID_OBJECT;
      out[1] = SL_IID_RECORD;
      out[2] = SL_IID_ANDROIDSIMPLEBUFFERQUEUE;
      out[3] = SL_IID_ANDROIDCONFIGURATION;
      return 4;
    default:
      return 0;
  }
}

static SLresult acl_query_num_interfaces(SLEngineItf self,
                                         SLuint32 object_id,
                                         SLuint32* count) {
  SLInterfaceID ids[4];
  if (acl_of_kind(self, kEngine) == NULL || count == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *count = acl_supported(object_id, ids);
  return *count != 0 ? SL_RESULT_SUCCESS : SL_RESULT_FEATURE_UNSUPPORTED;
}

static SLresult acl_query_interface(SLEngineItf self,
                                    SLuint32 object_id,
                                    SLuint32 index,
                                    SLInterfaceID* iid) {
  SLInterfaceID ids[4];
  if (acl_of_kind(self, kEngine) == NULL || iid == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  const SLuint32 n = acl_supported(object_id, ids);
  if (n == 0)
    return SL_RESULT_FEATURE_UNSUPPORTED;
  if (index >= n)
    return SL_RESULT_PARAMETER_INVALID;
  *iid = ids[index];
  return SL_RESULT_SUCCESS;
}

static SLresult acl_query_num_extensions(SLEngineItf self, SLuint32* count) {
  if (acl_of_kind(self, kEngine) == NULL || count == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *count = 0;
  return SL_RESULT_SUCCESS;
}

static SLresult acl_query_extension(SLEngineItf self,
                                    SLuint32 index,
                                    SLchar* name,
                                    SLint16* length) {
  (void)self;
  (void)index;
  (void)name;
  (void)length;
  return SL_RESULT_PARAMETER_INVALID;  // there is none at any index
}

static SLresult acl_is_extension_supported(SLEngineItf self,
                                           const SLchar* name,
                                           SLboolean* supported) {
  if (acl_of_kind(self, kEngine) == NULL || name == NULL || supported == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *supported = SL_BOOLEAN_FALSE;
  return SL_RESULT_SUCCESS;
}

static const struct SLEngineItf_ kEngineVt = {
    .CreateLEDDevice = acl_no_device,
    .CreateVibraDevice = acl_no_device,
    .CreateAudioPlayer = acl_no_audio_player,
    .CreateAudioRecorder = acl_create_audio_recorder,
    .CreateMidiPlayer = acl_no_midi_player,
    .CreateListener = acl_no_listener,
    .Create3DGroup = acl_no_listener,
    .CreateOutputMix = acl_create_output_mix,
    .CreateMetadataExtractor = acl_no_metadata_extractor,
    .CreateExtensionObject = acl_no_extension_object,
    .QueryNumSupportedInterfaces = acl_query_num_interfaces,
    .QuerySupportedInterfaces = acl_query_interface,
    .QueryNumSupportedExtensions = acl_query_num_extensions,
    .QuerySupportedExtension = acl_query_extension,
    .IsExtensionSupported = acl_is_extension_supported,
};

// ---- the three functions ----------------------------------------------------

ACL_EXPORT SLresult SLAPIENTRY slCreateEngine(SLObjectItf* engine,
                                              SLuint32 option_count,
                                              const SLEngineOption* options,
                                              SLuint32 count,
                                              const SLInterfaceID* ids,
                                              const SLboolean* required) {
  (void)option_count;
  (void)options;  // thread safety is the one option, and there always is
  if (engine == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *engine = NULL;
  struct AclObject* o = acl_new_object(kEngine);
  if (o == NULL)
    return SL_RESULT_MEMORY_FAILURE;
  if (count != 0 && (ids == NULL || required == NULL)) {
    acl_destroy((SLObjectItf)&o->object.vt);
    return SL_RESULT_PARAMETER_INVALID;
  }
  for (SLuint32 i = 0; i < count; i++) {
    if (ids[i] != SL_IID_OBJECT && ids[i] != SL_IID_ENGINE && required[i]) {
      acl_destroy((SLObjectItf)&o->object.vt);
      return SL_RESULT_FEATURE_UNSUPPORTED;
    }
  }
  *engine = (SLObjectItf)&o->object.vt;
  return SL_RESULT_SUCCESS;
}

ACL_EXPORT SLresult SLAPIENTRY
slQueryNumSupportedEngineInterfaces(SLuint32* count) {
  if (count == NULL)
    return SL_RESULT_PARAMETER_INVALID;
  *count = 2;
  return SL_RESULT_SUCCESS;
}

ACL_EXPORT SLresult SLAPIENTRY
slQuerySupportedEngineInterfaces(SLuint32 index, SLInterfaceID* iid) {
  if (iid == NULL || index >= 2)
    return SL_RESULT_PARAMETER_INVALID;
  *iid = index == 0 ? SL_IID_OBJECT : SL_IID_ENGINE;
  return SL_RESULT_SUCCESS;
}
