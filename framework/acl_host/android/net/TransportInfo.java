// SPDX-License-Identifier: Apache-2.0
// Provenance: android.net.TransportInfo public surface (AOSP android.jar api-33 stub; Android SDK
// docs). A marker interface for transport-specific network info (WifiInfo implements it). It is a
// module/@SystemApi type absent from acl's framework compile classpath, so acl provides a clean-room
// boot-classpath shim — enough for NetworkCapabilities.getTransportInfo()'s return type to resolve
// and for `x instanceof WifiInfo` guards to compile/run. No GPL-3.0 sources.
package android.net;

/** Marker for transport-specific network info returned by NetworkCapabilities.getTransportInfo(). */
public interface TransportInfo {
}
