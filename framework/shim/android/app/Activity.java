// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/app/Activity):
// the documented lifecycle-callback surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.app.Activity (Gate G2.d). Real
// Activity extends ContextThemeWrapper -> ContextWrapper -> Context and its closure
// reaches SystemServer/Binder (535 classes / 148 denylist edges — see
// docs/framework-bootstrap.md "Measured"), so per ADR-0002 it is SHIMMED at the
// Manager boundary rather than vendored. This shim carries only the lifecycle
// callbacks the aclcxx driver invokes; setContentView / Context services stay out
// of scope until the View stack (Phase 2). Docs-derived shim, NOT vendored AOSP.
package android.app;

import android.os.Bundle;

public class Activity {
  public Activity() {}

  // The lifecycle callbacks the aclcxx native driver calls, in order, to launch
  // the app's Activity subclass. Subclasses override these; the base is a no-op
  // (subclasses call super.onCreate(...) etc.).
  protected void onCreate(Bundle savedInstanceState) {}
  protected void onStart() {}
  protected void onResume() {}
  protected void onPause() {}
  protected void onStop() {}
  protected void onDestroy() {}

  // Documented no-op-friendly accessors so simple onCreate bodies link. Real
  // implementations arrive with the View stack / Context services.
  public void setContentView(int layoutResID) {}
  public void finish() {}
}
