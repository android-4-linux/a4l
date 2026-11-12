// SPDX-License-Identifier: Apache-2.0
// Provenance: Android SDK android.location.LocationManager / Location public
// surface (AOSP android.jar stubs, Apache-2.0); host mapping from the
// freedesktop GeoClue2 D-Bus API (org.freedesktop.GeoClue2, docs at
// www.freedesktop.org/software/geoclue); sdbus-cpp usage pattern after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// acl's location hub: the native underside of android.location.LocationManager.
// A pluggable LocationSource seam (host GeoClue2, or a synthetic fixed point)
// feeds the one LocationManager shim above without it caring which. Unlike
// svc/sensor (a poll thread), location is push-driven — the source delivers a
// fix whenever the platform reports one (the GeoClue2 async D-Bus loop).

#ifndef ACL_SVC_LOCATION_LOCATION_H
#define ACL_SVC_LOCATION_LOCATION_H

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace acl::svc::location {

// One position report (android.location.Location). Optional fields
// (altitude/speed/bearing) are gated by their has_* flags — GeoClue leaves them
// unset when the source can't determine them, and Android's
// Location.hasAltitude()/hasSpeed()/hasBearing() must then read false.
struct LocationFix {
  double latitude = 0.0;     // degrees
  double longitude = 0.0;    // degrees
  double accuracy_m = 0.0;   // horizontal accuracy radius, meters (>= 0)
  double altitude_m = 0.0;   // meters above the WGS84 ellipsoid
  double speed_mps = 0.0;    // ground speed, meters/second
  double bearing_deg = 0.0;  // direction of travel, degrees [0,360)
  std::int64_t timestamp_ms =
      0;  // UTC time of the fix, unix epoch milliseconds
  bool has_altitude = false;
  bool has_speed = false;
  bool has_bearing = false;
};

using LocationSink = std::function<void(const LocationFix&)>;

// A backend that produces position fixes. start() begins asynchronous delivery
// (the sink is called on the backend's own thread); stop() ends it. A new
// platform backend is a new subclass — nothing above the hub changes.
class LocationSource {
 public:
  virtual ~LocationSource() = default;
  virtual const char* name() const = 0;
  virtual bool start(LocationSink sink) = 0;
  virtual void stop() = 0;
};

// Pick the best available source: an ACL_LOCATION_LATLON override (synthetic,
// for tests) wins; otherwise the host GeoClue2 source when sdbus-c++ is
// compiled in and the service is reachable; otherwise a null source that yields
// no fixes. Never returns null.
std::unique_ptr<LocationSource> make_location_source();

// Process-wide owner of the source and the last known fix, behind
// android.location.LocationManager.
class LocationHub {
 public:
  static LocationHub& instance();

  // Begin updates (idempotent). Returns true if a real source is delivering.
  // Safe to call from the app thread when the first LocationListener registers.
  bool start();
  void stop();

  // The native → Java delivery callback (set once by location_jni). Invoked on
  // the source thread.
  void set_listener(LocationSink listener);

  // Most recent fix, if any (for getLastKnownLocation). Returns false when none
  // seen yet.
  bool last(LocationFix* out) const;

  // Whether a real (non-null) source backs this hub — drives
  // LocationManager.isProviderEnabled().
  bool available() const;

  const char* source_name() const;

 private:
  LocationHub() = default;
  void on_fix(const LocationFix& f);

  mutable std::mutex m_;
  std::unique_ptr<LocationSource> source_;
  LocationSink listener_;
  LocationFix last_;
  bool have_last_ = false;
  bool started_ = false;
};

}  // namespace acl::svc::location

#endif  // ACL_SVC_LOCATION_LOCATION_H
