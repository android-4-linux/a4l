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

    // Bound by name. Two native threads, one after the other, attach to the VM, call
    // callBack() and detach; see Java_com_acl_ndktest_Main_donorReuse in
    // tools/build-ndk-repro.sh.
    public static native int donorReuse();

    // Called by each of donorReuse()'s threads: a native call down from Java on that thread.
    public static int callBack() {
        return answer();
    }

    // Bound by name. Installs a SIGSEGV handler and reports whether a4l's was put back; see
    // Java_com_acl_ndktest_Main_handlerKept in tools/build-ndk-repro.sh.
    public static native int handlerKept();

    // Bound by name. ro.build.version.sdk as the library's native code reads it; see
    // Java_com_acl_ndktest_Main_sdkProperty in tools/build-ndk-repro.sh.
    public static native int sdkProperty();

    // Bound by name. An AAudio output with a data callback, run for 300 ms; see
    // Java_com_acl_ndktest_Main_aaudioPlay in tools/build-ndk-repro.sh.
    public static native int aaudioPlay();

    // Bound by name. An OpenSL ES recorder with a buffer queue, run for 300 ms; see
    // Java_com_acl_ndktest_Main_openslesRecord in tools/build-ndk-repro.sh.
    public static native int openslesRecord();

    // Bound by name. An OpenSL ES player with a buffer queue, run for 400 ms with a pause; see
    // Java_com_acl_ndktest_Main_openslesPlay in tools/build-ndk-repro.sh.
    public static native int openslesPlay();

    // Up-called BY the native, from bionic %fs back into ART's glibc %fs.
    public static int helper(int a, int b) {
        return a + b;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i("acl", "Main.onCreate: bionic native answer()=" + answer()
                     + " nativeAnswer()=" + nativeAnswer());

        Log.i("acl", "PROPS: native ro.build.version.sdk=" + sdkProperty());

        // Opt-in (run_ndk_art_test.py --donor): a native thread that attaches after another
        // has detached, and takes a native call from Java.
        if (System.getenv("ACL_NDK_TEST_DONOR") != null) {
            Log.i("acl", "DONOR: second attached thread got " + donorReuse());
        }

        // Opt-in (run_ndk_art_test.py --aaudio): the library plays through libaaudio.so.
        if (System.getenv("ACL_NDK_TEST_AAUDIO") != null) {
            final int calls = aaudioPlay();
            Log.i("acl", "AAUDIO: result=" + calls + " fed=" + (calls >= 2));
        }

        // Opt-in (run_ndk_art_test.py --opensles): the library records and plays through
        // libOpenSLES.so.
        // 300 ms in 20 ms buffers is 15 callbacks.
        if (System.getenv("ACL_NDK_TEST_OPENSLES") != null) {
            final int calls = openslesRecord();
            Log.i("acl", "OPENSLES: result=" + calls + " paced=" + (calls >= 10 && calls <= 17));
            // 400 ms in 10 ms buffers is 40 callbacks. The output also takes its own
            // buffer's worth ahead at each of the two starts, 80 ms or so each.
            final int played = openslesPlay();
            Log.i("acl", "OPENSLES: played=" + played + " in time=" + (played >= 30 && played <= 70));
        }

        // Opt-in (run_ndk_art_test.py --handler): the library installs a fault handler of its
        // own, as a crash reporter does.
        if (System.getenv("ACL_NDK_TEST_HANDLER") != null) {
            Log.i("acl", "HANDLER: a4l's fault handler back=" + handlerKept());
        }

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
