<!-- SPDX-License-Identifier: Apache-2.0 -->
<!-- Provenance: original a4l documentation; gpsd(8), gpsd_json(5) and the GeoClue2 D-Bus API.
     No GPL-3.0 sources. -->

# Location on a Raspberry Pi 4 or 5, and other systems with a GNSS receiver

An app's `android.location.LocationManager` is fed by one source (`src/svc/location`).
`ACL_LOCATION` names it:

| `ACL_LOCATION` | Source |
|---|---|
| unset, `geoclue` | GeoClue2 over D-Bus, where a4l was built with sdbus-c++; none otherwise |
| `gpsd` | a GNSS receiver behind gpsd |
| `none` | none: no provider is enabled and no fix arrives |

`ACL_LOCATION_LATLON=lat,lon[,accuracy]` overrides all of them with one fixed point,
delivered once. It is for tests.

gpsd is not looked for unasked. A desktop that has gpsd installed has its socket listening
with no receiver behind it, and GeoClue2 is the source that works there.

a4l logs the source when an app first asks for a location:

```
location source: gpsd at localhost:2947
gpsd: connected to localhost:2947
gpsd: the receiver has no fix yet
gpsd: the receiver has a fix (3D)
```

## gpsd

a4l reads gpsd's JSON socket. It is not linked with libgps, so one a4l binary works with
any gpsd that speaks the protocol. `ACL_GPSD` is where the daemon is, `host`, `host:port`,
`:port` or `[ipv6]:port`; it is `localhost:2947` when not set.

```sh
ACL_LOCATION=gpsd a4l run app.apk --present
ACL_LOCATION=gpsd ACL_GPSD=192.168.1.20 a4l run app.apk --present   # a receiver elsewhere
```

A fix is delivered for every position report gpsd sends, about one a second and with some
receivers two. From the report a4l takes the latitude and longitude, the height above the
WGS84 ellipsoid (`altHAE`, with a 3D fix), the speed, the track as the bearing, `eph` as
the accuracy (the larger of `epx` and `epy` without it) and the time.

The daemon does not have to be running when the app starts, and may be restarted under
it: a4l retries, a second apart at first and up to thirty.

Until the receiver has a fix there is no location, and the provider still reads as
enabled, as on a phone that is indoors.

### Setting gpsd up on Debian

```sh
sudo apt install gpsd gpsd-clients
```

A USB receiver that is plugged in after that is added by gpsd's udev rule. For one that was
already plugged in, or a serial one, name it in `/etc/default/gpsd`:

```sh
DEVICES="/dev/ttyACM0"
GPSD_OPTIONS="-n"
```

`-n` has gpsd read the receiver before a client connects, so the first fix is not waited
for after the app starts. Then:

```sh
sudo systemctl restart gpsd.socket gpsd.service
cgps        # from gpsd-clients: the fix as gpsd has it
```

`acl-location-probe` (built with a4l, not installed) prints what a4l gets:

```sh
ACL_LOCATION=gpsd acl-location-probe
```

## What was checked, and what was not

On x86-64 Fedora with gpsd 3.27.5: a gpsd started for the purpose, reading made-up NMEA
(RMC, GGA, GSA) from a pty, and `acl-location-probe` on it. The position, height, speed,
bearing and time were the ones sent; gpsd gave two reports a second for that input.

The `location-gpsd` test runs the probe against a stand-in gpsd: the handshake, a report
split across two packets, a connection the daemon drops and the reconnection, and no
daemon at all.

Not checked: an app (the path from the hub to `LocationManager` is the one the other
sources use), a remote gpsd, IPv6, and a gpsd older than 3.20, which has no `altHAE` and
so gives no height.
