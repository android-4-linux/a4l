// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l; the native methods and the metadata key numbers
// are those of AOSP's android.media.MediaMetadataRetriever (Apache-2.0).
// No GPL-3.0 sources.
//
// android.media.MediaMetadataRetriever over media::probe_media: the duration,
// size, rotation and tracks of a media file. No frame is decoded here:
// getFrameAtTime and the other picture calls return null, which they may for
// a file they cannot give a frame of.
#include "runtime/media_codec_jni.h"

#include "acl_config.h"

#if ACL_HAVE_GL

#if ACL_HAVE_FFMPEG
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

#include "log/log.h"
#include "media/video_decoder.h"
#endif

namespace acl::runtime {
namespace {

#if ACL_HAVE_FFMPEG

constexpr const char* kTag = "acl.media.retriever";

// MediaMetadataRetriever.METADATA_KEY_*
constexpr jint kKeyDuration = 9;
constexpr jint kKeyNumTracks = 10;
constexpr jint kKeyMimeType = 12;
constexpr jint kKeyHasAudio = 16;
constexpr jint kKeyHasVideo = 17;
constexpr jint kKeyVideoWidth = 18;
constexpr jint kKeyVideoHeight = 19;
constexpr jint kKeyBitrate = 20;
constexpr jint kKeyVideoRotation = 24;
constexpr jint kKeyCaptureFrameRate = 25;
constexpr jint kKeyVideoFrameCount = 32;
constexpr jint kKeySampleRate = 38;
constexpr jint kKeyVideoCodecMimeType = 40;

struct Retriever {
  std::mutex lock;
  bool have = false;  // a data source was set and read
  media::MediaInfo info;
};

jfieldID g_context = nullptr;  // MediaMetadataRetriever.mNativeContext (J)

Retriever* of(JNIEnv* env, jobject thiz) {
  return g_context == nullptr
             ? nullptr
             : reinterpret_cast<Retriever*>(env->GetLongField(thiz, g_context));
}

void throw_new(JNIEnv* env, const char* cls, const char* message) {
  if (jclass c = env->FindClass(cls)) {
    env->ThrowNew(c, message);
    env->DeleteLocalRef(c);
  }
}

// Reads `source` (a path or URL FFmpeg opens) into the retriever, or throws
// what MediaMetadataRetriever does for a source it cannot read.
void set_source(JNIEnv* env, jobject thiz, const std::string& source) {
  Retriever* r = of(env, thiz);
  if (r == nullptr) {
    throw_new(env, "java/lang/IllegalStateException", "No retriever available");
    return;
  }
  media::MediaInfo info;
  std::string error;
  const bool ok = media::probe_media(source, &info, &error);
  {
    const std::lock_guard<std::mutex> hold(r->lock);
    r->have = ok;
    r->info = ok ? info : media::MediaInfo{};
  }
  if (!ok) {
    log::w(kTag, "data source not read: {}", error);
    throw_new(env, "java/lang/RuntimeException", "setDataSource failed");
  }
}

void JNICALL native_init(JNIEnv* env, jclass cls) {
  g_context = env->GetFieldID(cls, "mNativeContext", "J");
  if (g_context == nullptr)
    env->ExceptionClear();
}

void JNICALL native_setup(JNIEnv* env, jobject thiz) {
  if (g_context != nullptr)
    env->SetLongField(thiz, g_context,
                      reinterpret_cast<jlong>(new Retriever()));
}

// release() and native_finalize(): either may come first, and both may come.
void JNICALL release(JNIEnv* env, jobject thiz) {
  Retriever* r = of(env, thiz);
  if (r == nullptr)
    return;
  env->SetLongField(thiz, g_context, 0);
  delete r;
}

// setDataSource(String) for a URL. A local path comes through the
// FileDescriptor form. Only the schemes an app has reason to name are passed
// on: FFmpeg reads many more, some of which read other files.
void JNICALL set_data_source_uri(JNIEnv* env,
                                 jobject thiz,
                                 jobject /*httpService*/,
                                 jstring uri,
                                 jobjectArray /*keys*/,
                                 jobjectArray /*values*/) {
  if (uri == nullptr) {
    throw_new(env, "java/lang/IllegalArgumentException", "Null pointer");
    return;
  }
  const char* s = env->GetStringUTFChars(uri, nullptr);
  if (s == nullptr)
    return;
  std::string source(s);
  env->ReleaseStringUTFChars(uri, s);
  if (source.rfind("file://", 0) == 0)
    source.erase(0, 7);
  const bool allowed = (!source.empty() && source[0] == '/') ||
                       source.rfind("http://", 0) == 0 ||
                       source.rfind("https://", 0) == 0;
  if (!allowed) {
    throw_new(env, "java/lang/IllegalArgumentException",
              "setDataSource failed: unsupported URI");
    return;
  }
  set_source(env, thiz, source);
}

// setDataSource(FileDescriptor, offset, length). The caller closes its
// descriptor when this returns, so the file is read now, through /proc. A
// range inside a larger file (an asset in an APK) is read with FFmpeg's
// subfile protocol.
void JNICALL set_data_source_fd(JNIEnv* env,
                                jobject thiz,
                                jobject file_descriptor,
                                jlong offset,
                                jlong length) {
  if (file_descriptor == nullptr || offset < 0 || length < 0) {
    throw_new(env, "java/lang/IllegalArgumentException", "Null pointer");
    return;
  }
  jclass fd_cls = env->GetObjectClass(file_descriptor);
  const jfieldID descriptor = env->GetFieldID(fd_cls, "descriptor", "I");
  env->DeleteLocalRef(fd_cls);
  const jint fd = descriptor != nullptr
                      ? env->GetIntField(file_descriptor, descriptor)
                      : -1;
  if (env->ExceptionCheck() || fd < 0) {
    env->ExceptionClear();
    throw_new(env, "java/lang/IllegalArgumentException",
              "Invalid file descriptor");
    return;
  }
  char source[128];
  // 0x7ffffffffffffffL: "to the end of the file" (AssetFileDescriptor's
  // UNKNOWN_LENGTH arrives as that from MediaMetadataRetriever).
  if (offset == 0)
    std::snprintf(source, sizeof(source), "/proc/self/fd/%d", fd);
  else if (length >= 0x7ffffffffffffffLL - offset)  // or the sum overflows
    std::snprintf(source, sizeof(source),
                  "subfile,,start,%lld,,:/proc/self/fd/%d",
                  static_cast<long long>(offset), fd);
  else {
    const long long end = static_cast<long long>(offset) + length;
    std::snprintf(source, sizeof(source),
                  "subfile,,start,%lld,end,%lld,,:/proc/self/fd/%d",
                  static_cast<long long>(offset), end, fd);
  }
  set_source(env, thiz, source);
}

// setDataSource(MediaDataSource): the app's own reader. Not supported.
void JNICALL set_data_source_callback(JNIEnv* env, jobject, jobject) {
  throw_new(env, "java/lang/IllegalArgumentException",
            "setDataSource failed: MediaDataSource is not supported");
}

jstring JNICALL extract_metadata(JNIEnv* env, jobject thiz, jint key) {
  Retriever* r = of(env, thiz);
  if (r == nullptr)
    return nullptr;
  media::MediaInfo info;
  {
    const std::lock_guard<std::mutex> hold(r->lock);
    if (!r->have)
      return nullptr;
    info = r->info;
  }
  // A key the file has no value for is null, as on Android.
  std::string value;
  switch (key) {
    case kKeyDuration:
      if (info.duration_us > 0)
        value = std::to_string(info.duration_us / 1000);  // milliseconds
      break;
    case kKeyNumTracks:
      value = std::to_string(info.tracks);
      break;
    case kKeyMimeType:
      value = info.mime;
      break;
    case kKeyHasAudio:
      if (info.has_audio)
        value = "yes";
      break;
    case kKeyHasVideo:
      if (info.has_video)
        value = "yes";
      break;
    case kKeyVideoWidth:
      if (info.has_video)
        value = std::to_string(info.width);
      break;
    case kKeyVideoHeight:
      if (info.has_video)
        value = std::to_string(info.height);
      break;
    case kKeyBitrate:
      if (info.bitrate > 0)
        value = std::to_string(info.bitrate);
      break;
    case kKeyVideoRotation:
      if (info.has_video)
        value = std::to_string(info.rotation);
      break;
    case kKeyCaptureFrameRate:
      if (info.frame_rate > 0)
        value = std::to_string(info.frame_rate);
      break;
    case kKeyVideoFrameCount:
      if (info.video_frames > 0)
        value = std::to_string(info.video_frames);
      break;
    case kKeySampleRate:
      if (info.sample_rate > 0)
        value = std::to_string(info.sample_rate);
      break;
    case kKeyVideoCodecMimeType:
      value = info.video_mime;
      break;
    default:
      break;
  }
  return value.empty() ? nullptr : env->NewStringUTF(value.c_str());
}

// No picture is made: null, which each of these may return.
jobject JNICALL
no_frame_at_time(JNIEnv*, jobject, jlong, jint, jint, jint, jobject) {
  return nullptr;
}
jobject JNICALL no_image_at_index(JNIEnv*, jobject, jint, jobject) {
  return nullptr;
}
jobject JNICALL no_thumbnail(JNIEnv*, jobject, jint, jobject, jint, jint) {
  return nullptr;
}
jbyteArray JNICALL no_embedded_picture(JNIEnv*, jobject, jint) {
  return nullptr;
}

#endif  // ACL_HAVE_FFMPEG

}  // namespace

void register_media_metadata_retriever_natives(JNIEnv* env) {
#if ACL_HAVE_FFMPEG
  jclass c = env->FindClass("android/media/MediaMetadataRetriever");
  if (c == nullptr) {
    env->ExceptionClear();
    return;
  }
  const JNINativeMethod m[] = {
      {const_cast<char*>("native_init"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&native_init)},
      {const_cast<char*>("native_setup"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&native_setup)},
      {const_cast<char*>("native_finalize"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&release)},
      {const_cast<char*>("release"), const_cast<char*>("()V"),
       reinterpret_cast<void*>(&release)},
      {const_cast<char*>("_setDataSource"),
       const_cast<char*>("(Landroid/os/IBinder;Ljava/lang/String;"
                         "[Ljava/lang/String;[Ljava/lang/String;)V"),
       reinterpret_cast<void*>(&set_data_source_uri)},
      {const_cast<char*>("_setDataSource"),
       const_cast<char*>("(Ljava/io/FileDescriptor;JJ)V"),
       reinterpret_cast<void*>(&set_data_source_fd)},
      {const_cast<char*>("_setDataSource"),
       const_cast<char*>("(Landroid/media/MediaDataSource;)V"),
       reinterpret_cast<void*>(&set_data_source_callback)},
      {const_cast<char*>("nativeExtractMetadata"),
       const_cast<char*>("(I)Ljava/lang/String;"),
       reinterpret_cast<void*>(&extract_metadata)},
      {const_cast<char*>("_getFrameAtTime"),
       const_cast<char*>(
           "(JIIILandroid/media/MediaMetadataRetriever$BitmapParams;)"
           "Landroid/graphics/Bitmap;"),
       reinterpret_cast<void*>(&no_frame_at_time)},
      {const_cast<char*>("_getImageAtIndex"),
       const_cast<char*>(
           "(ILandroid/media/MediaMetadataRetriever$BitmapParams;)"
           "Landroid/graphics/Bitmap;"),
       reinterpret_cast<void*>(&no_image_at_index)},
      {const_cast<char*>("getThumbnailImageAtIndex"),
       const_cast<char*>(
           "(ILandroid/media/MediaMetadataRetriever$BitmapParams;II)"
           "Landroid/graphics/Bitmap;"),
       reinterpret_cast<void*>(&no_thumbnail)},
      {const_cast<char*>("getEmbeddedPicture"), const_cast<char*>("(I)[B"),
       reinterpret_cast<void*>(&no_embedded_picture)},
  };
  // One at a time: a framework that lacks one of these still gets the rest.
  for (const JNINativeMethod& one : m) {
    if (env->RegisterNatives(c, &one, 1) != JNI_OK) {
      env->ExceptionClear();
      log::w(kTag, "not bound: {}{}", one.name, one.signature);
    }
  }
  env->DeleteLocalRef(c);
#else
  (void)env;
#endif
}

}  // namespace acl::runtime

#endif  // ACL_HAVE_GL
