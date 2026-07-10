// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/app/Service):
// the documented lifecycle-callback surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.app.Service (Gate G2.e, automotive
// media path). Real Service extends ContextWrapper -> Context and its bind path is
// Binder-based; per ADR-3 (single-process, no Binder in v1) the host drives the
// service directly, so onBind()/IBinder are omitted here. Only the lifecycle
// callbacks the aclcxx media host invokes are present. Docs-derived shim, NOT
// vendored AOSP (see docs/framework-bootstrap.md "Measured" for why the closure is
// shimmed, not vendored).
package android.app;

public abstract class Service {
  public Service() {}

  public void onCreate() {}
  public void onDestroy() {}
  // onStartCommand(Intent,...) / onBind(Intent) arrive with the Intent shim + the
  // in-process service dispatch; the media host drives lifecycle directly for now.
}
