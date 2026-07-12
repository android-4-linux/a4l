// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK docs (android.app.Activity
// lifecycle). No GPL-3.0 sources.
//
// A minimal real Activity for the --vm=art path (Gate G2.d on host ART): it
// extends the vendored android.app.Activity shim and overrides onCreate, which
// runs super.onCreate() + android.util.Log.i(getPackageName()). run_activity_art
// compiles this, d8s it into a zipaligned APK (ART mmaps the dex), and asserts
// acl drives the lifecycle on real host ART. Mirrors make_activity_apk.py's
// hand-built MainActivity, but from source so d8 produces an ART-loadable dex.
package com.acl.hello;

import android.app.Activity;
import android.os.Bundle;
import android.util.Log;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        Log.i("acl", "MainActivity.onCreate ran on host ART; pkg=" + getPackageName());
    }
}
