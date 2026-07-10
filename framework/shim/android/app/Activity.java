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
import android.view.View;

public class Activity extends android.view.ContextThemeWrapper {
  private View mContentView;
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

  // The View-stack entry point (Phase 2): the app hands its root view here in
  // onCreate; the aclcxx ViewRoot driver retrieves it (aclcxxContentView) after the
  // launch lifecycle and runs the measure/layout/draw traversal. Real setContentView
  // routes through getWindow().setContentView into a DecorView; this stores the root
  // directly until the Window/PhoneWindow stack lands.
  public void setContentView(View view) { mContentView = view; }
  public void setContentView(int layoutResID) {}
  public View aclcxxContentView() { return mContentView; }
  public void finish() {}
}
