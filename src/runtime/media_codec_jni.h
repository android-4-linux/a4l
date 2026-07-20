// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.MediaCodec native contract from AOSP frameworks/base/media/java/android/
// media/MediaCodec.java (the Java-declared native method names/signatures + the mNativeContext field)
// and the MediaCodec buffer/state model from the SDK docs. The native underside of the REAL vendored
// android.media.MediaCodec, driving acl's media::CodecDecoder (zero-copy VAAPI/V4L2 decode) into the
// configured output Surface's ImageReader BufferQueue via queue_imported ([[flutter-texture-bridge]]
// M5). No GPL-3.0 sources.
#ifndef ACL_RUNTIME_MEDIA_CODEC_JNI_H
#define ACL_RUNTIME_MEDIA_CODEC_JNI_H

#include <jni.h>

namespace acl::runtime {

// Register acl's native underside for android.media.MediaCodec over media::CodecDecoder. No-op (and
// harmless) when the class isn't on the classpath or GL/FFmpeg is unavailable. Called from the
// libmedia_jni.so load interception, alongside register_image_reader_natives.
void register_media_codec_natives(JNIEnv* env);

// Register acl's native underside for android.media.MediaCodecList — advertises acl's HW decoder so
// media3/ExoPlayer's MediaCodecUtil discovers it (the gate before it creates a MediaCodec).
void register_media_codec_list_natives(JNIEnv* env);

}  // namespace acl::runtime

#endif  // ACL_RUNTIME_MEDIA_CODEC_JNI_H
