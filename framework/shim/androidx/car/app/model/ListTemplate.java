// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.model.ListTemplate + ListTemplate.Builder docs
// (developer.android.com/reference/androidx/car/app/model/ListTemplate): a titled
// template wrapping a single ItemList. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.model.ListTemplate (Gate
// G1.5b, Tier-1). The acl CAL host reads the title + list in-process via acl*
// accessors. NOT vendored androidx.
package androidx.car.app.model;

public final class ListTemplate implements Template {
  private final String title;
  private final ItemList list;

  ListTemplate(String title, ItemList list) {
    this.title = title;
    this.list = list;
  }

  public String aclTitle() { return title; }
  public ItemList aclList() { return list; }

  public static final class Builder {
    private String title = "";
    private ItemList list;

    public Builder() {}

    public Builder setTitle(CharSequence title) {
      this.title = title == null ? "" : title.toString();
      return this;
    }

    public Builder setSingleList(ItemList list) {
      this.list = list;
      return this;
    }

    public ListTemplate build() { return new ListTemplate(title, list); }
  }
}
