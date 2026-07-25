// SPDX-License-Identifier: Apache-2.0
// Provenance: original acl benchmark fixture. No GPL-3.0 sources.
package com.acl.jitbench;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class MainActivity extends Activity {
    // A hot method called enough times to cross ART's JIT compile threshold; under -Xint it stays
    // interpreted the whole time. The wall-clock difference is the JIT speedup on app compute.
    static long work(int n, int salt) {
        long s = 0;
        for (int i = 0; i < n; i++) s += ((long) i * 31 + salt) % 997;
        return s;
    }

    @Override
    protected void onCreate(Bundle b) {
        super.onCreate(b);
        long warmSum = 0;
        for (int i = 0; i < 30000; i++) warmSum += work(2000, i);   // warm the JIT
        long t0 = System.nanoTime();
        long sum = 0;
        for (int i = 0; i < 30000; i++) sum += work(2000, i);       // timed run
        long ms = (System.nanoTime() - t0) / 1_000_000L;
        Log.i("acl.jitbench", "compute 30000x work(2000): " + ms + "ms (sum=" + sum + ")");
    }
}
