// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/app/
// ActivityManager): public API surface. Native underside -> svc/ ActivityService.
// No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.app.ActivityManager (svc hub).
// Returned by Context.getSystemService(ACTIVITY_SERVICE). Docs-derived shim, NOT
// vendored AOSP.
package android.app;

public class ActivityManager {
  public ActivityManager() {}

  public int getMemoryClass() { return nativeGetMemoryClass(); }
  public boolean isLowRamDevice() { return nativeIsLowRamDevice(); }

  private static native int nativeGetMemoryClass();
  private static native boolean nativeIsLowRamDevice();
}
