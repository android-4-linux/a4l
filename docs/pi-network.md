<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l documentation; the NetworkManager D-Bus API and Android's
     ConnectivityManager and WifiManager public documentation. No GPL-3.0 sources. -->

# Networking on a Raspberry Pi 4 or 5, and other systems on a wired network

An app's `android.net.ConnectivityManager` and `android.net.wifi.WifiManager` are answered
from the daemon that runs the host's networking (`src/svc/net`), over the system bus. a4l
takes the first of ConnMan, NetworkManager and systemd-networkd that it finds, in that
order. `ACL_NET_BACKEND` names one instead: `connman`, `networkmanager` or `networkd`. With
none of them running, or in a build without sdbus-c++, the app is told there is no network.

Raspberry Pi OS runs NetworkManager. a4l logs its choice when the app starts:

```
connectivity backend: networkmanager
wifi natives registered (backend: networkmanager; enabled=false connected=false ssid="")
```

What the app is told is the host's state:

| The app asks | NetworkManager's answer |
|---|---|
| is there an active network | `State` is connected, global |
| `NET_CAPABILITY_VALIDATED` | `Connectivity` is full |
| metered | `Metered` is yes, or guessed yes |
| `TRANSPORT_WIFI`, `TRANSPORT_ETHERNET`, `TRANSPORT_CELLULAR` | `PrimaryConnectionType` |
| `WifiManager.isWifiEnabled()` | `WirelessEnabled` |
| `WifiInfo`: SSID, BSSID, signal, link speed, frequency | the Wi-Fi device's active access point |

Only the NetworkManager backend has the Wi-Fi rows. Under the others the app is told Wi-Fi
is off.

## A wired network shown as Wi-Fi

An app written for a phone may do its work on the local network only when it believes it
is on Wi-Fi: it looks for its speakers, its cast targets or its printer when the transport
is Wi-Fi and `WifiManager` names a network, and not otherwise. A board on Ethernet reaches
the same devices, and the app never looks.

`ACL_NET_WIRED_AS_WIFI` shows the app an active wired network as a Wi-Fi one. It is off
unless set, since it is not the host's state.

| `ACL_NET_WIRED_AS_WIFI` | |
|---|---|
| unset, empty, `0`, `no`, `off`, `false` | off |
| `1`, `yes`, `on`, `true` | on; the network is named `Wired` |
| anything else | on; the value is the network's name |

```sh
ACL_NET_WIRED_AS_WIFI=1 a4l run app.apk --present
ACL_NET_WIRED_AS_WIFI="Home network" a4l run app.apk --present
```

With it on, and a wired network active:

- the active network's transport is `TRANSPORT_WIFI`, and `getActiveNetworkInfo()` is of
  type Wi-Fi;
- `isWifiEnabled()` is true, and `getConnectionInfo()` is a connected network with the
  name above, the address `02:00:00:00:00:00` that Android gives an app which may not see
  the real one, and a signal of -50 dBm. Link speed and frequency are unknown.

If the host is on an access point of its own as well, that one is named, with its real
details. A host that is offline, or on a cellular network, is shown as it is. Nothing else
changes: `WifiInfo.getIpAddress()` is 0 with or without it, and a scan finds nothing.

It works with every backend, as it is applied to what the backend reports. a4l logs it:

```
a wired network is shown to the app as the Wi-Fi network "Wired" (ACL_NET_WIRED_AS_WIFI)
```

## Checking

`acl-net-probe`, built beside `a4l` and not installed, prints what an app would be told.
It takes the same two settings:

```
$ ACL_NET_WIRED_AS_WIFI=1 acl-net-probe
backend   : networkmanager
connected : yes   (getActiveNetwork() != null)
validated : yes   (NET_CAPABILITY_VALIDATED)
metered   : no   (!NET_CAPABILITY_NOT_METERED)
transport : WIFI

-- WifiManager (android.net.wifi) --
enabled   : yes   (isWifiEnabled())
connected : yes   (associated with an AP)
ssid      : Wired
bssid     : 02:00:00:00:00:00
rssi      : -50 dBm
linkSpeed : -1 Mbps
frequency : 0 MHz
```
