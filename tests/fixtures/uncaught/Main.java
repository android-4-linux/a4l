// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test fixture; public Android API only. No GPL-3.0 sources.
//
// A thread fails, in the way ACL_TEST_END names:
//   throw   the thread "worker-x" throws an exception nothing catches
//   direct  the thread hands an exception to its handler itself, as kotlinx.coroutines does,
//           which does not pass the pre-handler Thread.dispatchUncaughtException runs
//   main    a message on the main thread throws
//   exit    the thread "quitter" calls System.exit(3)
// After throw, direct and main the main thread logs "UNCAUGHT: still running" half a second
// later and exits with status 0.
package com.acl.uncaught;

import android.app.Activity;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;

public class Main extends Activity {
    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        final String how = System.getenv("ACL_TEST_END");
        Log.i("acl", "UNCAUGHT: ending by " + how);
        if (!"exit".equals(how)) {
            new Handler(Looper.getMainLooper()).postDelayed(new Runnable() {
                @Override public void run() {
                    Log.i("acl", "UNCAUGHT: still running");
                    System.exit(0);
                }
            }, 500);
        }
        if ("exit".equals(how)) {
            new Thread(new Runnable() {
                @Override public void run() {
                    System.exit(3);
                }
            }, "quitter").start();
        } else if ("main".equals(how)) {
            new Handler(Looper.getMainLooper()).post(new Runnable() {
                @Override public void run() {
                    throw new IllegalStateException("boom main");
                }
            });
        } else if ("direct".equals(how)) {
            new Thread(new Runnable() {
                @Override public void run() {
                    final Thread t = Thread.currentThread();
                    t.getUncaughtExceptionHandler().uncaughtException(t,
                            new IllegalStateException("boom direct"));
                }
            }, "worker-d").start();
        } else {
            new Thread(new Runnable() {
                @Override public void run() {
                    throw new IllegalStateException("boom");
                }
            }, "worker-x").start();
        }
    }
}
