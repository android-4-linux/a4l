// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.media.MediaCommunicationManager; public type surface
// from AOSP android.jar (API 33) stubs (Apache-2.0). The real class ships in the
// updatable com.android.media apex, which is absent from an apex-split host boot
// classpath. No GPL-3.0 sources.
package android.media;

/**
 * acl media shim. The real MediaCommunicationManager lives in the com.android.media
 * mainline module, so it is missing from acl's boot classpath — and
 * {@code MediaSessionManager}'s constructor casts to it
 * ({@code mCommunicationManager = (MediaCommunicationManager) context.getSystemService(
 * MEDIA_COMMUNICATION_SERVICE)}). Without the type on the boot classpath that cast
 * fails to resolve with NoClassDefFoundError, the MediaSessionManager constructor
 * throws, {@code getSystemService(MEDIA_SESSION_SERVICE)} is null, and every
 * MediaSession / MediaSessionCompat construction NPEs — which takes down every media
 * app (and the automotive media path) at startup.
 *
 * <p>This class therefore exists primarily so that cast resolves. acl does not serve
 * MEDIA_COMMUNICATION_SERVICE, so the field is simply null and no instance of this
 * class is created; the members below exist for API shape.
 *
 * <p>MediaSession2 (which this manager brokers) has no router in acl, so the token
 * list is empty. Session2 callback registration is not provided: nothing would ever
 * fire it, and its callback type is itself apex-only.
 */
public class MediaCommunicationManager {
    /** Not instantiated by acl (MEDIA_COMMUNICATION_SERVICE is not served). */
    MediaCommunicationManager() {}

    /** Version of the media communication service; acl brokers none. */
    public int getVersion() { return 0; }

    /**
     * Active MediaSession2 tokens. acl has no Session2 router, so this is always empty.
     * Declared with the raw list type on purpose: generics erase, so this stays binary
     * compatible with the platform's {@code List<Session2Token>} without dragging the
     * apex-only Session2Token onto acl's boot classpath.
     */
    @SuppressWarnings("rawtypes")
    public java.util.List getSession2Tokens() { return java.util.Collections.emptyList(); }

    /** No media-key router in acl; the event is accepted and dropped. */
    public void dispatchMediaKeyEvent(android.view.KeyEvent keyEvent, boolean asSystemService) {}
}
