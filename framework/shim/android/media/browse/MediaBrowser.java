// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/media/
// browse/MediaBrowser.MediaItem): public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.media.browse.MediaBrowser.MediaItem
// (Gate G2.e). The browse tree is a list of MediaItems, each wrapping a
// MediaDescription plus browsable/playable flags. Only MediaItem is needed by the
// media host today (the MediaBrowser client itself lands with the app-side session
// connection). Docs-derived shim, NOT vendored AOSP.
package android.media.browse;

import android.media.MediaDescription;

public final class MediaBrowser {
  private MediaBrowser() {}

  public static class MediaItem {
    public static final int FLAG_BROWSABLE = 1 << 0;
    public static final int FLAG_PLAYABLE = 1 << 1;

    private final MediaDescription description;
    private final int flags;

    public MediaItem(MediaDescription description, int flags) {
      if (description == null) {
        throw new IllegalArgumentException("description cannot be null");
      }
      this.description = description;
      this.flags = flags;
    }

    public MediaDescription getDescription() { return description; }
    public int getFlags() { return flags; }
    public String getMediaId() { return description.getMediaId(); }
    public boolean isBrowsable() { return (flags & FLAG_BROWSABLE) != 0; }
    public boolean isPlayable() { return (flags & FLAG_PLAYABLE) != 0; }
  }
}
