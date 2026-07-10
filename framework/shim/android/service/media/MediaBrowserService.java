// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK docs (developer.android.com/reference/android/service/
// media/MediaBrowserService and its BrowserRoot/Result nested types): the public
// media-browse API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for android.service.media.MediaBrowserService
// (Gate G2.e, automotive media path). A media app subclasses it and implements
// onGetRoot (browse-tree root) and onLoadChildren (children of a node). The real
// class exposes these over a MediaSession/Binder connection; per ADR-3 the aclcxx
// media host calls them in-process, no Binder. This slice carries onGetRoot +
// BrowserRoot; onLoadChildren/Result (the browse-tree walk) land in the next slice.
// Docs-derived shim, NOT vendored AOSP.
package android.service.media;

import android.os.Bundle;

public abstract class MediaBrowserService extends android.app.Service {
  public MediaBrowserService() {}

  // Return the browse-tree root for a connecting client, or null to reject.
  public abstract BrowserRoot onGetRoot(String clientPackageName, int clientUid,
                                        Bundle rootHints);

  // The root of a browse tree returned by onGetRoot.
  public static final class BrowserRoot {
    private final String rootId;
    private final Bundle extras;

    public BrowserRoot(String rootId, Bundle extras) {
      if (rootId == null) {
        throw new IllegalArgumentException("rootId cannot be null");
      }
      this.rootId = rootId;
      this.extras = extras;
    }

    public String getRootId() { return rootId; }
    public Bundle getExtras() { return extras; }
  }
}
