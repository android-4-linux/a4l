// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.model.Row + Row.Builder docs (developer.android.com/
// reference/androidx/car/app/model/Row): title + text, built via Builder. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.model.Row (Gate G1.5b). A
// CAL app builds rows for a list template; the acl CAL host reads them in-process
// via the acl* accessors (not androidx API). NOT vendored androidx.
package androidx.car.app.model;

public final class Row {
  private final String title;
  private final String text;

  Row(String title, String text) {
    this.title = title;
    this.text = text;
  }

  public String aclTitle() { return title; }
  public String aclText() { return text; }

  public static final class Builder {
    private String title = "";
    private String text = "";

    public Builder() {}

    public Builder setTitle(CharSequence title) {
      this.title = title == null ? "" : title.toString();
      return this;
    }

    public Builder addText(CharSequence text) {
      this.text = text == null ? "" : text.toString();
      return this;
    }

    public Row build() { return new Row(title, text); }
  }
}
