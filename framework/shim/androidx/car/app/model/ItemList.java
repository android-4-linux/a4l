// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx.car.app.model.ItemList + ItemList.Builder docs
// (developer.android.com/reference/androidx/car/app/model/ItemList): an ordered
// list of items (Rows), built via addItem. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.car.app.model.ItemList (Gate G1.5b).
// The acl CAL host reads the rows in-process via aclRows(). NOT vendored androidx.
package androidx.car.app.model;

import java.util.ArrayList;
import java.util.List;

public final class ItemList {
  private final List<Row> rows;

  ItemList(List<Row> rows) { this.rows = rows; }

  public List<Row> aclRows() { return rows; }

  public static final class Builder {
    private final ArrayList<Row> rows = new ArrayList<>();

    public Builder() {}

    public Builder addItem(Row row) {
      rows.add(row);
      return this;
    }

    public ItemList build() { return new ItemList(rows); }
  }
}
