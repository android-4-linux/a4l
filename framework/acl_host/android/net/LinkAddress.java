// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.LinkAddress; public method surface from AOSP
// android.jar (API 33) stubs (Apache-2.0). No GPL-3.0 sources.
package android.net;

/** An address on a link with its prefix length, as {@link LinkProperties} lists them. */
public class LinkAddress {
    private final java.net.InetAddress address;
    private final int prefixLength;

    LinkAddress(java.net.InetAddress address, int prefixLength) {
        this.address = address;
        this.prefixLength = prefixLength;
    }

    public java.net.InetAddress getAddress() { return address; }
    public int getPrefixLength() { return prefixLength; }
    /** The IFA_F flags of the address; acl reads none from the host. */
    public int getFlags() { return 0; }
    /** The RT_SCOPE of the address: link (253) for a link-local one, else universe (0). */
    public int getScope() { return address.isLinkLocalAddress() ? 253 : 0; }

    @Override public boolean equals(Object o) {
        if (!(o instanceof LinkAddress)) return false;
        final LinkAddress l = (LinkAddress) o;
        return address.equals(l.address) && prefixLength == l.prefixLength;
    }
    @Override public int hashCode() { return address.hashCode() + 11 * prefixLength; }
    @Override public String toString() { return address.getHostAddress() + "/" + prefixLength; }
}
