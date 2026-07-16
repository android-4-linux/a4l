// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK docs (android.app.Activity
// lifecycle, System.loadLibrary) + the JNI spec (RegisterNatives). No GPL-3.0 sources.
//
// The app half of the bionic-NDK end-to-end (ADR-5 / R-6, docs/ndk-scope.md): a real
// Activity whose native method is implemented by a *bionic-ABI* lib/x86_64/libndktest.so
// (built by tools/build-ndk-repro.sh). On host ART, System.loadLibrary -> Runtime.nativeLoad
// is routed by acl's gated hook (jvm_vm register_ndk_loadlibrary_override) into the
// in-process bionic linker, which drives the .so's JNI_OnLoad across the two-libc %fs
// boundary; JNI_OnLoad FindClass'es THIS class and RegisterNatives-binds answer().
//
// answer() is deliberately RegisterNatives-bound, not name-resolved: the .so is loaded by
// the bionic linker, so ART holds no dlopen handle to dlsym Java_..._answer against.
// Name-based binding of a bionic native is a separate rung (see docs/ndk-scope.md).
//
// The native answer() up-calls helper(20, 30) (a varargs CallStaticIntMethod back across the
// boundary into ART) and returns 4200 + 50 = 4250 — so the printed value proves the whole
// round trip ran, not merely that the library loaded.
package com.acl.ndktest;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class Main extends Activity {
    static {
        System.loadLibrary("ndktest");   // -> Runtime.nativeLoad -> acl's bionic loader
    }

    // Bound by the bionic .so's JNI_OnLoad via RegisterNatives.
    public static native int answer();

    // Up-called BY the native, from bionic %fs back into ART's glibc %fs.
    public static int helper(int a, int b) {
        return a + b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i("acl", "Main.onCreate: bionic native answer()=" + answer());
    }
}
