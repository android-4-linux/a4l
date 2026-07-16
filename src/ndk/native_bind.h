// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl. Name mangling per the public JNI spec (Oracle JNI 6, "Resolving Native
// Method Names"); native-method discovery via public java.lang.Class/java.lang.reflect.Method
// reflection (Android SDK docs). ART binding contract read from AOSP art/runtime/jni/java_vm_ext.cc
// (Apache-2.0). No GPL-3.0 sources.
#pragma once

#include "acl_config.h"
#if ACL_HAVE_JNI
#include <jni.h>

namespace acl::ndk {
class BionicRuntime;

// The class whose System.loadLibrary triggered this load — what the natives should be bound to.
// Returns a local ref, or nullptr if it can't be determined.
//
// `caller` is nativeLoad's third argument, but it is null exactly when it matters: libcore's
// Runtime.loadLibrary0 passes callerClass only on the boot-classloader path; for an app (a
// PathClassLoader) it calls the 2-arg nativeLoad(filename, loader) overload, which hardcodes
// caller=null. So when it is null, recover the same class from the Java stack — the frame below
// java.lang.{Runtime,System} — and resolve it through `loader` (the ClassLoader nativeLoad was
// handed), without initializing it: it is mid-<clinit> right now, which is what called us.
jclass loading_class(JNIEnv* env, jobject caller, jobject loader);

// Bind, by NAME, the native methods `klass` declares against the already-loaded bionic lib
// `handle`, and return how many were bound (0 is normal and not an error; -1 on a reflection
// failure, with any pending exception cleared).
//
// Why this is needed at all: ART binds a native method lazily, by dlsym'ing Java_pkg_Class_method
// on the handles *it* dlopen'ed (JavaVMExt::FindCodeForNativeMethod -> Libraries::FindNativeMethod).
// A bionic app lib is opened by the bionic linker instead, so ART holds no handle for it and that
// lookup can never succeed — it throws UnsatisfiedLinkError at the first call. Nor can the lazy
// path be hooked: ClassLinker::RegisterNative CHECKs a non-null address, so the JVMTI
// NativeMethodBind event only fires for an address ART already found. So acl binds eagerly instead,
// at load time, resolving the names itself and handing ART down-call trampolines via RegisterNatives.
//
// Scope: `klass` is the loading class (see loading_class above). That covers the canonical NDK
// idiom — a class loads its lib in its own static initializer and declares the natives it needs.
// Natives declared in OTHER classes are NOT covered; see docs/ndk-scope.md. Methods a JNI_OnLoad
// already RegisterNatives'd are left alone.
int bind_class_natives(BionicRuntime& rt, void* handle, JNIEnv* env, jclass klass);

}  // namespace acl::ndk
#endif  // ACL_HAVE_JNI
