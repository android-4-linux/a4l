// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.CarAppService docs (developer.android.com/reference/
// androidx/car/app/CarAppService): the app entry point that creates a Session.
// No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.CarAppService (Gate G1.5b) —
// the CAL app's manifest entry point. The real class extends android Service and is
// reached over the CAL AIDL host boundary; per ADR-3 the acl CAL host instantiates
// it and calls onCreateSession() in-process (no Binder). NOT vendored androidx.
package androidx.car.app;

public abstract class CarAppService {
  public CarAppService() {}

  public abstract Session onCreateSession();
}
