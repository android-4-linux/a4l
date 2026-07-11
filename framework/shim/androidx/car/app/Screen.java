// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.Screen docs (developer.android.com/reference/androidx/
// car/app/Screen): a screen returns a Template from onGetTemplate(). No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.Screen (Gate G1.5b). The
// real Screen holds a CarContext and lifecycle; the acl CAL host only needs
// onGetTemplate(), which it calls in-process (no Binder, ADR-3). The app overrides
// it to build its template tree. NOT vendored androidx.
package androidx.car.app;

import androidx.car.app.model.Template;

public abstract class Screen {
  public Screen() {}

  public abstract Template onGetTemplate();
}
