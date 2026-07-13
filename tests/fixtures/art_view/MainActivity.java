// SPDX-License-Identifier: Apache-2.0
// Provenance: original test fixture; Android SDK android.app.Activity (public API). No GPL-3.0 sources.
//
// The real app entry point: an Activity whose onCreate sets a programmatic content view.
// Probes the Activity/Window frontier on host ART (attach + PhoneWindow/DecorView), the layer
// above the now-working View traversal.
package com.acl.hello;

import android.app.Activity;
import android.os.Bundle;

public class MainActivity extends Activity {
    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(new Panel(this));
    }
}
