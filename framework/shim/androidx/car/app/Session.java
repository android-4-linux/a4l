// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.Session docs (developer.android.com/reference/androidx/
// car/app/Session): a Session creates the first Screen. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.Session (Gate G1.5b). The
// real Session's onCreateScreen takes an Intent; this shim uses a no-arg form the
// acl CAL host calls in-process to obtain the root Screen. NOT vendored androidx.
package androidx.car.app;

public abstract class Session {
  public Session() {}

  public abstract Screen onCreateScreen();
}
