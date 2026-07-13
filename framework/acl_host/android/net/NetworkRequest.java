// SPDX-License-Identifier: Apache-2.0
// Provenance: clean-room android.net.NetworkRequest (+ Builder); public Builder
// method surface from AOSP android.jar (API 33) stubs (Apache-2.0). acl has a
// single default network, so a request is just a filter apps hand to
// registerNetworkCallback; the fields are accepted and ignored. No GPL-3.0 sources.
package android.net;

public class NetworkRequest {
    NetworkRequest() {}

    public static class Builder {
        public Builder() {}
        public Builder addTransportType(int transportType) { return this; }
        public Builder removeTransportType(int transportType) { return this; }
        public Builder addCapability(int capability) { return this; }
        public Builder removeCapability(int capability) { return this; }
        public Builder clearCapabilities() { return this; }
        public NetworkRequest build() { return new NetworkRequest(); }
    }
}
