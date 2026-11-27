// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code; gpsd's JSON protocol as gpsd_json(5)
// documents it (the TPV object and its fields). No gpsd code. No GPL-3.0
// sources.
#ifndef ACL_SVC_LOCATION_GPSD_TPV_H
#define ACL_SVC_LOCATION_GPSD_TPV_H

#include <string>
#include <string_view>

#include "svc/location/location.h"

namespace acl::svc::location {

// One line of what gpsd sends a watcher. True, with `fix` filled, when the
// line is a TPV report that has a position: mode 2 (2D) or 3 (3D) and a
// latitude and longitude in range. Any other line (VERSION, DEVICES, SKY, a
// TPV with no fix yet, something that is not a JSON object) is false.
// `mode`, when given, gets the TPV's mode, or -1 if the line is not a TPV.
//
// What is taken: lat, lon; altHAE as the altitude (the height above the WGS84
// ellipsoid, which is what android.location.Location's altitude is); speed
// (m/s); track (degrees from true north); eph, else the larger of epx and
// epy, as the accuracy; time (UTC, ISO 8601), else the time now.
bool fix_from_gpsd_line(std::string_view line,
                        LocationFix* fix,
                        int* mode = nullptr);

// "host", "host:port", ":port" or "[ipv6]:port" as a host and a port for
// getaddrinfo. An empty host is localhost and an empty port 2947, gpsd's.
// False for something that is none of these.
bool parse_gpsd_endpoint(std::string_view text,
                         std::string* host,
                         std::string* port);

}  // namespace acl::svc::location

#endif  // ACL_SVC_LOCATION_GPSD_TPV_H
