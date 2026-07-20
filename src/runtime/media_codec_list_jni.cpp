// SPDX-License-Identifier: Apache-2.0
// Provenance: android.media.MediaCodecList native contract from AOSP frameworks/base/media/java/
// android/media/MediaCodecList.java (native_getCodecCount / getCodecName / getCanonicalName /
// getAttributes / getSupportedTypes / getCodecCapabilities / native_getGlobalSettings /
// findCodecByName / native_init) and the MediaCodecInfo.CodecCapabilities / CodecProfileLevel model
// (MediaCodecInfo.java). Advertises acl's zero-copy HW decoder so media3/ExoPlayer's MediaCodecUtil
// discovers it (the gate before it creates a MediaCodec — see media_codec_jni.cpp). No GPL-3.0 sources.

#include "acl_config.h"

#if ACL_HAVE_GL

#include <jni.h>

#include <cstring>

#include "log/log.h"

namespace acl::runtime {
namespace {
constexpr const char* kTag = "acl.media.codeclist";

// MediaCodecInfo flags + constants.
constexpr jint FLAG_IS_HARDWARE_ACCELERATED = (1 << 3);
constexpr jint COLOR_FormatSurface = 0x7F000789;         // 2130708361
constexpr jint COLOR_FormatYUV420Flexible = 0x7F420888;  // 2135033992
constexpr jint COLOR_FormatYUV420Planar = 19;
// H.264 CodecProfileLevel values (MediaCodecInfo.CodecProfileLevel).
constexpr jint AVCProfileBaseline = 0x01, AVCProfileMain = 0x02, AVCProfileHigh = 0x08;
constexpr jint AVCLevel52 = 0x10000;

struct ProfLev { jint profile, level; };

// The codecs acl advertises. One zero-copy HW decoder per type; H.264 first (the video_player common
// case). HEVC/VP9 can join once this path is exercised end-to-end by ExoPlayer.
const ProfLev kAvcProfiles[] = {
    {AVCProfileBaseline, AVCLevel52}, {AVCProfileMain, AVCLevel52}, {AVCProfileHigh, AVCLevel52}};
struct CodecEntry {
  const char* name;
  const char* mime;
  const ProfLev* profiles;
  int nprofiles;
};
const CodecEntry kCodecs[] = {
    {"acl.h264.decoder", "video/avc", kAvcProfiles, 3},
};
constexpr int kCodecCount = sizeof(kCodecs) / sizeof(kCodecs[0]);

// Cached ids.
jclass g_profilelevel_cls = nullptr;
jmethodID g_profilelevel_ctor = nullptr;
jfieldID g_pl_profile_fid = nullptr, g_pl_level_fid = nullptr;
jclass g_caps_cls = nullptr;
jmethodID g_caps_ctor = nullptr;  // CodecCapabilities([CPL;[IZ;Map;Map)
jclass g_hashmap_cls = nullptr;
jmethodID g_hashmap_ctor = nullptr, g_hashmap_put = nullptr;

jobject new_hashmap(JNIEnv* env) {
  return g_hashmap_cls != nullptr ? env->NewObject(g_hashmap_cls, g_hashmap_ctor) : nullptr;
}
void put_str(JNIEnv* env, jobject m, const char* k, const char* v) {
  jstring jk = env->NewStringUTF(k), jv = env->NewStringUTF(v);
  env->CallObjectMethod(m, g_hashmap_put, jk, jv);
  env->DeleteLocalRef(jk); env->DeleteLocalRef(jv);
}

// ── android.media.MediaCodecList natives (all static) ─────────────────────────────────────────────
void JNICALL MCL_native_init(JNIEnv*, jclass) {}

jint JNICALL MCL_native_getCodecCount(JNIEnv*, jclass) { return kCodecCount; }

jstring JNICALL MCL_getCodecName(JNIEnv* env, jclass, jint index) {
  if (index < 0 || index >= kCodecCount) return env->NewStringUTF("");
  return env->NewStringUTF(kCodecs[index].name);
}
jstring JNICALL MCL_getCanonicalName(JNIEnv* env, jclass c, jint index) { return MCL_getCodecName(env, c, index); }

jint JNICALL MCL_getAttributes(JNIEnv*, jclass, jint) {
  return FLAG_IS_HARDWARE_ACCELERATED;  // a HW decoder; not encoder / vendor / software-only
}

jobjectArray JNICALL MCL_getSupportedTypes(JNIEnv* env, jclass, jint index) {
  jclass str = env->FindClass("java/lang/String");
  if (index < 0 || index >= kCodecCount) return env->NewObjectArray(0, str, nullptr);
  jobjectArray arr = env->NewObjectArray(1, str, nullptr);
  env->SetObjectArrayElement(arr, 0, env->NewStringUTF(kCodecs[index].mime));
  env->DeleteLocalRef(str);
  return arr;
}

jobject JNICALL MCL_getCodecCapabilities(JNIEnv* env, jclass, jint index, jstring /*type*/) {
  if (index < 0 || index >= kCodecCount || g_caps_cls == nullptr) return nullptr;
  const CodecEntry& e = kCodecs[index];

  // CodecProfileLevel[] — the profiles/levels ExoPlayer checks the stream against.
  jobjectArray profLevs = env->NewObjectArray(e.nprofiles, g_profilelevel_cls, nullptr);
  for (int i = 0; i < e.nprofiles; ++i) {
    jobject pl = env->NewObject(g_profilelevel_cls, g_profilelevel_ctor);
    env->SetIntField(pl, g_pl_profile_fid, e.profiles[i].profile);
    env->SetIntField(pl, g_pl_level_fid, e.profiles[i].level);
    env->SetObjectArrayElement(profLevs, i, pl);
    env->DeleteLocalRef(pl);
  }

  // colorFormats — Surface output + a flexible YUV (a HW surface decoder).
  const jint cfs[] = {COLOR_FormatSurface, COLOR_FormatYUV420Flexible, COLOR_FormatYUV420Planar};
  jintArray colFmts = env->NewIntArray(3);
  env->SetIntArrayRegion(colFmts, 0, 3, cfs);

  // defaultFormatMap needs "mime" (the ctor reads it); capabilitiesMap can be minimal — VideoCapabilities
  // then uses permissive platform limits (initWithPlatformLimits), so typical sizes are accepted.
  jobject defFmt = new_hashmap(env);
  put_str(env, defFmt, "mime", e.mime);
  jobject caps = new_hashmap(env);
  put_str(env, caps, "mime", e.mime);
  put_str(env, caps, "max-concurrent-instances", "16");

  jobject result = env->NewObject(g_caps_cls, g_caps_ctor, profLevs, colFmts, JNI_FALSE, defFmt, caps);
  if (env->ExceptionCheck()) { env->ExceptionDescribe(); env->ExceptionClear(); result = nullptr; }
  env->DeleteLocalRef(profLevs); env->DeleteLocalRef(colFmts);
  env->DeleteLocalRef(defFmt); env->DeleteLocalRef(caps);
  return result;
}

jobject JNICALL MCL_native_getGlobalSettings(JNIEnv* env, jclass) {
  jobject m = new_hashmap(env);
  if (m != nullptr) put_str(env, m, "max-concurrent-instances", "16");
  return m;
}

jint JNICALL MCL_findCodecByName(JNIEnv* env, jclass, jstring name) {
  if (name == nullptr) return -1;
  const char* s = env->GetStringUTFChars(name, nullptr);
  int idx = -1;
  if (s != nullptr) {
    for (int i = 0; i < kCodecCount; ++i)
      if (std::strcmp(s, kCodecs[i].name) == 0) { idx = i; break; }
    env->ReleaseStringUTFChars(name, s);
  }
  return idx;
}

bool bind(JNIEnv* env, jclass c, const JNINativeMethod* m, int n) {
  const bool ok = env->RegisterNatives(c, m, n) == JNI_OK;
  if (!ok) env->ExceptionClear();
  return ok;
}
}  // namespace

void register_media_codec_list_natives(JNIEnv* env) {
  if (env == nullptr) return;
  jclass mcl = env->FindClass("android/media/MediaCodecList");
  if (mcl == nullptr) { env->ExceptionClear(); return; }

  if (jclass pl = env->FindClass("android/media/MediaCodecInfo$CodecProfileLevel"); pl != nullptr) {
    g_profilelevel_cls = static_cast<jclass>(env->NewGlobalRef(pl));
    g_profilelevel_ctor = env->GetMethodID(pl, "<init>", "()V");
    g_pl_profile_fid = env->GetFieldID(pl, "profile", "I");
    g_pl_level_fid = env->GetFieldID(pl, "level", "I");
    env->DeleteLocalRef(pl);
  }
  if (jclass cc = env->FindClass("android/media/MediaCodecInfo$CodecCapabilities"); cc != nullptr) {
    g_caps_cls = static_cast<jclass>(env->NewGlobalRef(cc));
    g_caps_ctor = env->GetMethodID(
        cc, "<init>",
        "([Landroid/media/MediaCodecInfo$CodecProfileLevel;[IZLjava/util/Map;Ljava/util/Map;)V");
    env->DeleteLocalRef(cc);
  }
  if (jclass hm = env->FindClass("java/util/HashMap"); hm != nullptr) {
    g_hashmap_cls = static_cast<jclass>(env->NewGlobalRef(hm));
    g_hashmap_ctor = env->GetMethodID(hm, "<init>", "()V");
    g_hashmap_put = env->GetMethodID(hm, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
    env->DeleteLocalRef(hm);
  }
  if (env->ExceptionCheck()) env->ExceptionClear();

  static const JNINativeMethod m[] = {
      {const_cast<char*>("native_init"), const_cast<char*>("()V"), reinterpret_cast<void*>(&MCL_native_init)},
      {const_cast<char*>("native_getCodecCount"), const_cast<char*>("()I"), reinterpret_cast<void*>(&MCL_native_getCodecCount)},
      {const_cast<char*>("getCodecName"), const_cast<char*>("(I)Ljava/lang/String;"), reinterpret_cast<void*>(&MCL_getCodecName)},
      {const_cast<char*>("getCanonicalName"), const_cast<char*>("(I)Ljava/lang/String;"), reinterpret_cast<void*>(&MCL_getCanonicalName)},
      {const_cast<char*>("getAttributes"), const_cast<char*>("(I)I"), reinterpret_cast<void*>(&MCL_getAttributes)},
      {const_cast<char*>("getSupportedTypes"), const_cast<char*>("(I)[Ljava/lang/String;"), reinterpret_cast<void*>(&MCL_getSupportedTypes)},
      {const_cast<char*>("getCodecCapabilities"),
       const_cast<char*>("(ILjava/lang/String;)Landroid/media/MediaCodecInfo$CodecCapabilities;"),
       reinterpret_cast<void*>(&MCL_getCodecCapabilities)},
      {const_cast<char*>("native_getGlobalSettings"), const_cast<char*>("()Ljava/util/Map;"), reinterpret_cast<void*>(&MCL_native_getGlobalSettings)},
      {const_cast<char*>("findCodecByName"), const_cast<char*>("(Ljava/lang/String;)I"), reinterpret_cast<void*>(&MCL_findCodecByName)},
  };
  const bool ok = bind(env, mcl, m, static_cast<int>(sizeof(m) / sizeof(m[0])));
  env->DeleteLocalRef(mcl);
  log::write(ok ? log::Level::Info : log::Level::Warn, kTag,
             ok ? "MediaCodecList natives bound (advertising acl.h264.decoder)"
                : "MediaCodecList native bind incomplete");
}

}  // namespace acl::runtime

#endif  // ACL_HAVE_GL
