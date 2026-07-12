// SPDX-License-Identifier: Apache-2.0
// Provenance: androidx docs (developer.android.com/reference/androidx/media/
// MediaBrowserServiceCompat and its BrowserRoot/Result nested types): the public
// media-browse API surface. No GPL-3.0 sources.
//
// A minimal source-compatible shim for androidx.media.MediaBrowserServiceCompat —
// the support-library twin of android.service.media.MediaBrowserService, and the
// base class most real Android Auto media apps (e.g. Spotify) actually extend. A
// media app subclasses it and implements onGetRoot + onLoadChildren; per ADR-3 the
// acl media host calls them in-process (no Binder). aclValue/aclSent are acl
// accessors, not androidx API. Docs-derived shim, NOT vendored androidx.
package androidx.media;

import android.os.Bundle;

import java.util.List;

public abstract class MediaBrowserServiceCompat extends android.app.Service {
  public MediaBrowserServiceCompat() {}

  // Return the browse-tree root for a connecting client, or null to reject.
  public abstract BrowserRoot onGetRoot(String clientPackageName, int clientUid,
                                        Bundle rootHints);

  // Load the children of `parentId`; the app calls result.sendResult(children).
  public abstract void onLoadChildren(String parentId,
                                      Result<List<MediaBrowserCompat.MediaItem>> result);

  // The receiver an app calls sendResult() on. Real Result is delivered over a
  // MediaBrowserCompat connection; here it holds the value so the in-process host
  // reads it (aclValue/aclSent are acl accessors, not androidx API).
  public static class Result<T> {
    private T value;
    private boolean sent;

    public Result() {}

    public void sendResult(T result) { this.value = result; this.sent = true; }
    public void detach() {}

    public T aclValue() { return value; }
    public boolean aclSent() { return sent; }
  }

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
