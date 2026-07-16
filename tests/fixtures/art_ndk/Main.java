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
// Both ways a native can bind are exercised here:
//   answer()        RegisterNatives'd by the .so's JNI_OnLoad — the .so hands acl the pointer.
//   nativeAnswer()  bound by NAME: the .so exports Java_com_acl_ndktest_Main_nativeAnswer and
//                   nothing registers it. ART binds such a native by dlsym'ing that symbol on a
//                   handle *it* dlopen'ed; the lib lives in the bionic linker's namespace, so ART
//                   has no handle and the lookup can never succeed. acl resolves the name itself
//                   at load time and hands ART a trampoline (src/ndk/native_bind.h).
//
// The native answer() up-calls helper(20, 30) (a varargs CallStaticIntMethod back across the
// boundary into ART) and returns 4200 + 50 = 4250 — so the printed value proves the whole
// round trip ran, not merely that the library loaded. nativeAnswer() returns a flat 42.
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

    // Bound by NAME: the .so exports Java_com_acl_ndktest_Main_nativeAnswer, nothing registers it.
    public static native int nativeAnswer();

    // Up-called BY the native, from bionic %fs back into ART's glibc %fs.
    public static int helper(int a, int b) {
        return a + b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i("acl", "Main.onCreate: bionic native answer()=" + answer()
                     + " nativeAnswer()=" + nativeAnswer());

        // Opt-in (run_ndk_thread_test.py): call the same native from a NON-main thread. Only the
        // thread that bootstrapped the bionic linker has a bionic TCB, so acl must refuse this
        // loudly. Left out of the default run because the refusal aborts the process by design.
        if (System.getenv("ACL_NDK_TEST_WORKER") != null) {
            Thread t = new Thread(new Runnable() {
                public void run() {
                    Log.i("acl", "WORKER: calling nativeAnswer() off the main thread");
                    Log.i("acl", "WORKER: nativeAnswer()=" + nativeAnswer());
                }
            });
            t.start();
            try {
                t.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
        }
    }
}
