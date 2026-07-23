// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.wifi.SupplicantState; public enum surface from
// AOSP android.jar (API 33) stubs (Apache-2.0). acl derives only the two states its
// host view can distinguish (associated / not). No GPL-3.0 sources.
package android.net.wifi;

/**
 * Association state of the wireless link. The host network daemon exposes only
 * "associated with an access point" or not, so acl reports {@link #COMPLETED} or
 * {@link #DISCONNECTED}; the intermediate supplicant handshake states exist for API
 * compatibility (apps switch over this enum) but are never produced.
 */
public enum SupplicantState {
    DISCONNECTED,
    INTERFACE_DISABLED,
    INACTIVE,
    SCANNING,
    AUTHENTICATING,
    ASSOCIATING,
    ASSOCIATED,
    FOUR_WAY_HANDSHAKE,
    GROUP_HANDSHAKE,
    COMPLETED,
    DORMANT,
    UNINITIALIZED,
    INVALID;

    /** True once the link is usable for data (the platform's isValidState semantics). */
    public static boolean isValidState(SupplicantState state) {
        return state != null && state != UNINITIALIZED && state != INVALID;
    }
}
