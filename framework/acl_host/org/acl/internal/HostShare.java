// SPDX-License-Identifier: Apache-2.0
// Provenance: acl host-integration seam; Intent.ACTION_SEND / chooser mapped to
// xdg-desktop-portal OpenURI. Freedesktop portal spec. No GPL-3.0 sources.
package org.acl.internal;

/**
 * Host-integration seam for sharing. acl runs a single app, so an {@code ACTION_SEND} chooser has
 * no other app to target; for a shared URL the desktop-correct action is to hand it to the host's
 * xdg-desktop-portal ({@code org.freedesktop.portal.OpenURI}), which opens it in the user's default
 * handler (a browser). The native (svc/share, sdbus-c++) is bound at host-ART boot; when it is
 * absent (no sdbus-c++ / no portal) {@link #openUri} is a no-op returning false and the caller falls
 * back to the clipboard.
 */
public final class HostShare {
    private HostShare() {}

    // Bound by svc/share (register_share_natives). Returns true if the portal accepted the URI.
    private static native boolean nOpenUri(String uri);

    /** Ask the host portal to open {@code uri}. Returns false if unavailable (native unbound/threw). */
    public static boolean openUri(String uri) {
        if (uri == null || uri.isEmpty()) return false;
        try {
            return nOpenUri(uri);
        } catch (Throwable t) {
            return false;  // native not registered (no sdbus-c++ / portal) -> caller uses the clipboard
        }
    }
}
