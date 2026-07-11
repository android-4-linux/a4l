// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.model.Template docs (developer.android.com/reference/
// androidx/car/app/model/Template): the marker interface all templates implement.
// No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.model.Template (Gate G1.5b).
// The real interface is a Bundleable marker; the acl CAL host reads concrete
// template subtypes (ListTemplate, …) via their acl* accessors in-process. NOT
// vendored androidx.
package androidx.car.app.model;

public interface Template {}
