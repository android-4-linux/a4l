// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l code. No GPL-3.0 sources.
#ifndef ACL_SVC_LOCATION_GPSD_SOURCE_H
#define ACL_SVC_LOCATION_GPSD_SOURCE_H

#include <memory>
#include <string>

#include "svc/location/location.h"

namespace acl::svc::location {

// Fixes from a gpsd at `host`:`port` (names for getaddrinfo), read over its
// JSON socket: the receiver's own reports, about one a second while it has a
// fix. The daemon does not have to be there when this starts, and may go and
// come back; the source keeps trying.
std::unique_ptr<LocationSource> make_gpsd_source(std::string host,
                                                 std::string port);

}  // namespace acl::svc::location

#endif  // ACL_SVC_LOCATION_GPSD_SOURCE_H
