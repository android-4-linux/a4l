// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.NetworkRequest (+ Builder); public Builder
// method surface from AOSP android.jar (API 33) stubs (Apache-2.0). acl has a
// single default network, so a request is just a filter apps hand to
// registerNetworkCallback; it keeps what was asked so the asker can read it back, and
// acl does not filter on it. No GPL-3.0 sources.
package android.net;

public class NetworkRequest {
    private final long transports;    // bitmask: (1L << TRANSPORT_*)
    private final long capabilities;  // bitmask: (1L << NET_CAPABILITY_*)
    private final NetworkSpecifier specifier;

    NetworkRequest(long transports, long capabilities, NetworkSpecifier specifier) {
        this.transports = transports;
        this.capabilities = capabilities;
        this.specifier = specifier;
    }

    // What the request asked for. The framework's JobInfo reads these back when a job names a
    // required network (JobInfo.Builder.build() checks getNetworkSpecifier()).
    public boolean hasTransport(int transportType) { return has(transports, transportType); }
    public boolean hasCapability(int capability) { return has(capabilities, capability); }
    public NetworkSpecifier getNetworkSpecifier() { return specifier; }

    private static boolean has(long mask, int bit) {
        return bit >= 0 && bit < 64 && (mask & (1L << bit)) != 0;
    }
    private static long with(long mask, int bit, boolean on) {
        if (bit < 0 || bit >= 64) return mask;
        return on ? mask | (1L << bit) : mask & ~(1L << bit);
    }

    @Override public boolean equals(Object o) {
        if (!(o instanceof NetworkRequest)) return false;
        NetworkRequest r = (NetworkRequest) o;
        return transports == r.transports && capabilities == r.capabilities
                && java.util.Objects.equals(specifier, r.specifier);
    }
    @Override public int hashCode() {
        return java.util.Objects.hash(transports, capabilities, specifier);
    }

    public static class Builder {
        private long transports;
        // A new request asks for NOT_RESTRICTED (13), TRUSTED (14) and NOT_VPN (15), as documented
        // for NetworkRequest.Builder.
        private long capabilities = (1L << 13) | (1L << 14) | (1L << 15);
        private NetworkSpecifier specifier;

        public Builder() {}
        public Builder addTransportType(int transportType) {
            transports = with(transports, transportType, true);
            return this;
        }
        public Builder removeTransportType(int transportType) {
            transports = with(transports, transportType, false);
            return this;
        }
        public Builder addCapability(int capability) {
            capabilities = with(capabilities, capability, true);
            return this;
        }
        public Builder removeCapability(int capability) {
            capabilities = with(capabilities, capability, false);
            return this;
        }
        public Builder clearCapabilities() { capabilities = 0; return this; }
        public Builder setNetworkSpecifier(NetworkSpecifier networkSpecifier) {
            specifier = networkSpecifier;
            return this;
        }
        public NetworkRequest build() {
            return new NetworkRequest(transports, capabilities, specifier);
        }
    }
}
