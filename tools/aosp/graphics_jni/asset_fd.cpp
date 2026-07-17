// SPDX-License-Identifier: Apache-2.0
// Provenance: reimplements android.content.res.AssetManager's nativeOpenAssetFd /
// nativeOpenNonAssetFd behavior (AOSP frameworks/base/core/jni/android_util_AssetManager.cpp
// ReturnParcelFileDescriptor, Apache-2.0) which the HOST/layoutlib build stubs to throw
// UnsupportedOperationException("Implement me") (#ifdef __ANDROID__ "Layoutlib does not support
// parcel"). Uses libandroidfw (AssetManager2/Asset) — the SAME copy the graphics vendor lib
// already links, and the AssetManager2* comes straight from the Java side, so no ODR risk. This
// lets AssetManager.openFd() return a real fd on host ART: libGDX's Pixmap length()/AssetFile-
// Descriptor loads, SoundPool/MediaPlayer AssetFileDescriptor sources, etc. No GPL-3.0 sources.

#include <jni.h>
#include <unistd.h>

#include <memory>

#include <androidfw/AssetManager2.h>
#include <androidfw/MutexGuard.h>
#include <nativehelper/JNIHelp.h>
#include <nativehelper/ScopedUtfChars.h>

using ::android::Asset;
using ::android::AssetManager2;
using ::android::ApkAssetsCookie;
using ::android::Guarded;
using ::android::ScopedLock;

namespace {

// android_util_AssetManager.cpp's ReturnParcelFileDescriptor (__ANDROID__ path), re-authored:
// open the asset as an fd + wrap it in a ParcelFileDescriptor, filling out_offsets[start,length].
jobject return_pfd(JNIEnv* env, std::unique_ptr<Asset> asset, jlongArray out_offsets) {
  off64_t start_offset = 0, length = 0;
  int fd = asset->openFileDescriptor(&start_offset, &length);
  asset.reset();
  if (fd < 0) {
    jniThrowException(env, "java/io/FileNotFoundException",
                      "This file can not be opened as a file descriptor; it is probably compressed");
    return nullptr;
  }
  jlong* offsets = reinterpret_cast<jlong*>(env->GetPrimitiveArrayCritical(out_offsets, nullptr));
  if (offsets == nullptr) { close(fd); return nullptr; }
  offsets[0] = start_offset;
  offsets[1] = length;
  env->ReleasePrimitiveArrayCritical(out_offsets, offsets, 0);

  // ParcelFileDescriptor.adoptFd(fd): public, takes the raw fd (a local fd needs no Binder) and
  // takes ownership. Avoids the nativehelper jniCreateFileDescriptor helper (absent here).
  jclass pfd_cls = env->FindClass("android/os/ParcelFileDescriptor");
  if (pfd_cls == nullptr) { close(fd); return nullptr; }
  jmethodID adopt = env->GetStaticMethodID(pfd_cls, "adoptFd", "(I)Landroid/os/ParcelFileDescriptor;");
  if (adopt == nullptr) { env->DeleteLocalRef(pfd_cls); close(fd); return nullptr; }
  jobject pfd = env->CallStaticObjectMethod(pfd_cls, adopt, static_cast<jint>(fd));
  env->DeleteLocalRef(pfd_cls);
  return pfd;
}

Guarded<AssetManager2>& am_from_long(jlong ptr) {
  return *reinterpret_cast<Guarded<AssetManager2>*>(ptr);
}

jobject NativeOpenAssetFd(JNIEnv* env, jclass, jlong ptr, jstring asset_path, jlongArray out_offsets) {
  ScopedUtfChars path(env, asset_path);
  if (path.c_str() == nullptr) return nullptr;  // NPE pending
  ScopedLock<AssetManager2> am(am_from_long(ptr));
  std::unique_ptr<Asset> asset = am->Open(path.c_str(), Asset::ACCESS_RANDOM);
  if (!asset) { jniThrowException(env, "java/io/FileNotFoundException", path.c_str()); return nullptr; }
  return return_pfd(env, std::move(asset), out_offsets);
}

jobject NativeOpenNonAssetFd(JNIEnv* env, jclass, jlong ptr, jint jcookie, jstring asset_path,
                             jlongArray out_offsets) {
  ScopedUtfChars path(env, asset_path);
  if (path.c_str() == nullptr) return nullptr;
  const ApkAssetsCookie cookie = jcookie > 0 ? static_cast<ApkAssetsCookie>(jcookie - 1)
                                             : android::kInvalidCookie;
  ScopedLock<AssetManager2> am(am_from_long(ptr));
  std::unique_ptr<Asset> asset = cookie != android::kInvalidCookie
                                     ? am->OpenNonAsset(path.c_str(), cookie, Asset::ACCESS_RANDOM)
                                     : am->OpenNonAsset(path.c_str(), Asset::ACCESS_RANDOM);
  if (!asset) { jniThrowException(env, "java/io/FileNotFoundException", path.c_str()); return nullptr; }
  return return_pfd(env, std::move(asset), out_offsets);
}

const JNINativeMethod kMethods[] = {
    {const_cast<char*>("nativeOpenAssetFd"),
     const_cast<char*>("(JLjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;"),
     reinterpret_cast<void*>(NativeOpenAssetFd)},
    {const_cast<char*>("nativeOpenNonAssetFd"),
     const_cast<char*>("(JILjava/lang/String;[J)Landroid/os/ParcelFileDescriptor;"),
     reinterpret_cast<void*>(NativeOpenNonAssetFd)},
};

}  // namespace

// acl entry: override the host/layoutlib stubs on android.content.res.AssetManager. RegisterNatives
// replaces the two methods the host libandroid_runtime bound at boot. 0 on success.
extern "C" int register_acl_asset_fd(JNIEnv* env) {
  jclass c = env->FindClass("android/content/res/AssetManager");
  if (c == nullptr) return JNI_ERR;
  const int rc = env->RegisterNatives(c, kMethods, sizeof(kMethods) / sizeof(kMethods[0]));
  env->DeleteLocalRef(c);
  return rc;
}
