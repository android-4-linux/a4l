// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx docs (MediaBrowserCompat.MediaItem, support-library media):
// public API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.media MediaBrowserCompat.MediaItem —
// the support-library twin of android.media.browse.MediaBrowser.MediaItem. A browse
// tree is a list of MediaItems, each wrapping a MediaDescriptionCompat plus
// browsable/playable flags; the acl media host reads them. Docs-derived shim, NOT
// vendored androidx.
package androidx.media;

public final class MediaBrowserCompat {
  private MediaBrowserCompat() {}

  public static class MediaItem {
    public static final int FLAG_BROWSABLE = 1 << 0;
    public static final int FLAG_PLAYABLE = 1 << 1;

    private final MediaDescriptionCompat description;
    private final int flags;

    public MediaItem(MediaDescriptionCompat description, int flags) {
      if (description == null) {
        throw new IllegalArgumentException("description cannot be null");
      }
      this.description = description;
      this.flags = flags;
    }

    public MediaDescriptionCompat getDescription() { return description; }
    public int getFlags() { return flags; }
    public String getMediaId() { return description.getMediaId(); }
    public boolean isBrowsable() { return (flags & FLAG_BROWSABLE) != 0; }
    public boolean isPlayable() { return (flags & FLAG_PLAYABLE) != 0; }
  }
}
