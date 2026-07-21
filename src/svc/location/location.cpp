// SPDX-License-Identifier: Apache-2.0
// Provenance: freedesktop GeoClue2 D-Bus API (org.freedesktop.GeoClue2 Manager/Client/Location
// interfaces, public docs) mapped onto Android android.location.Location; sdbus-cpp createProxy/
// callMethod/getProperty/uponSignal idiom after src/svc/net/net_backend.cpp and
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.

#include "svc/location/location.h"

#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <string>

#include "acl_config.h"
#include "log/log.h"

#if ACL_HAVE_SDBUS
#include <sdbus-c++/sdbus-c++.h>
#endif

namespace acl::svc::location {
namespace {
constexpr const char* kTag = "acl.svc.location";

// Parse "lat,lon[,accuracyMeters]" (ACL_LOCATION_LATLON). Returns false if malformed.
bool parse_latlon(const char* s, LocationFix* out) {
  if (s == nullptr || *s == '\0') return false;
  char* end = nullptr;
  const double lat = std::strtod(s, &end);
  if (end == s || *end != ',') return false;
  const char* p = end + 1;
  const double lon = std::strtod(p, &end);
  if (end == p) return false;
  double acc = 10.0;
  if (*end == ',') { const char* q = end + 1; double a = std::strtod(q, &end); if (end != q) acc = a; }
  out->latitude = lat;
  out->longitude = lon;
  out->accuracy_m = acc < 0 ? 0 : acc;
  out->timestamp_ms = static_cast<std::int64_t>(::time(nullptr)) * 1000;
  return true;
}

// A source that emits a single fixed fix on start (ACL_LOCATION_LATLON) — deterministic for the
// probe and Tier A tests, and a stand-in where the host has no GeoClue2.
class SyntheticSource final : public LocationSource {
 public:
  explicit SyntheticSource(const LocationFix& fix) : fix_(fix) {}
  const char* name() const override { return "synthetic"; }
  bool start(LocationSink sink) override {
    if (sink) sink(fix_);
    return true;
  }
  void stop() override {}

 private:
  LocationFix fix_;
};

// A source that yields nothing — GeoClue2 absent and no override. isProviderEnabled() reads false.
class NullSource final : public LocationSource {
 public:
  const char* name() const override { return "none"; }
  bool start(LocationSink) override { return false; }
  void stop() override {}
};

#if ACL_HAVE_SDBUS
// GeoClue2 RequestedAccuracyLevel enum (org.freedesktop.GeoClue2.AccuracyLevel).
enum : uint32_t { kAccExact = 8 };

constexpr const char* kBus = "org.freedesktop.GeoClue2";
constexpr const char* kMgrPath = "/org/freedesktop/GeoClue2/Manager";
constexpr const char* kMgrIface = "org.freedesktop.GeoClue2.Manager";
constexpr const char* kClientIface = "org.freedesktop.GeoClue2.Client";
constexpr const char* kLocIface = "org.freedesktop.GeoClue2.Location";

// GeoClue leaves Altitude at -DBL_MAX and Speed/Heading at -1 when the source can't determine them.
bool alt_known(double v) { return v > -1.0e300; }
bool nonneg_known(double v) { return v >= 0.0; }

// Host GeoClue2 client. Owns an async connection for the LocationUpdated signal and a second
// connection for the synchronous Location property reads (a blocking read on the async connection
// from inside its own signal handler would deadlock the event loop).
class GeoClue2Source final : public LocationSource {
 public:
  const char* name() const override { return "geoclue2"; }

  bool start(LocationSink sink) override {
    sink_ = std::move(sink);
    try {
      conn_ = sdbus::createSystemBusConnection();
      auto mgr = sdbus::createProxy(*conn_, sdbus::ServiceName{kBus}, sdbus::ObjectPath{kMgrPath});
      sdbus::ObjectPath client_path;
      mgr->callMethod("GetClient").onInterface(sdbus::InterfaceName{kMgrIface}).storeResultsTo(client_path);

      const char* did = std::getenv("ACL_GEOCLUE_DESKTOP_ID");
      const std::string desktop_id = (did != nullptr && *did != '\0') ? did : "acl";

      client_ = sdbus::createProxy(*conn_, sdbus::ServiceName{kBus}, client_path);
      const sdbus::InterfaceName ci{kClientIface};
      client_->setProperty("DesktopId").onInterface(ci).toValue(desktop_id);
      client_->setProperty("RequestedAccuracyLevel").onInterface(ci).toValue(uint32_t{kAccExact});
      client_->setProperty("DistanceThreshold").onInterface(ci).toValue(uint32_t{0});
      client_->setProperty("TimeThreshold").onInterface(ci).toValue(uint32_t{0});
      client_->uponSignal("LocationUpdated")
          .onInterface(ci)
          .call([this](const sdbus::ObjectPath&, const sdbus::ObjectPath& np) { on_location(np); });

      conn_->enterEventLoopAsync();
      client_->callMethod("Start").onInterface(ci);
      log::i(kTag, "GeoClue2 client started (DesktopId='{}', accuracy=EXACT)", desktop_id);
      return true;
    } catch (const sdbus::Error& e) {
      // AccessDenied here means GeoClue's policy did not authorize DesktopId (no agent + not in
      // /etc/geoclue/geoclue.conf's allowlist). Report it clearly rather than silently no-op'ing.
      log::w(kTag, "GeoClue2 start failed ({}: {}); no location updates — authorize the DesktopId in "
                   "/etc/geoclue/geoclue.conf or run a GeoClue agent",
             e.getName(), e.getMessage());
      conn_.reset();
      client_.reset();
      return false;
    }
  }

  void stop() override {
    try {
      if (client_) client_->callMethod("Stop").onInterface(sdbus::InterfaceName{kClientIface});
    } catch (const sdbus::Error&) {
    }
    location_.reset();
    client_.reset();
    conn_.reset();
  }

 private:
  // Read the org.freedesktop.GeoClue2.Location object at `path` and push it up. GeoClue ties the
  // Location's access to the connection that owns the client, so the read MUST use conn_ — but conn_
  // runs the async loop and we're inside its LocationUpdated callback, where a synchronous call would
  // deadlock. So fetch all properties with a single async Properties.GetAll and deliver in its reply
  // (also on the loop thread). The proxy is kept in a member so it outlives the pending reply.
  void on_location(const sdbus::ObjectPath& path) {
    location_ = sdbus::createProxy(*conn_, sdbus::ServiceName{kBus}, path);
    location_->callMethodAsync("GetAll")
        .onInterface(sdbus::InterfaceName{"org.freedesktop.DBus.Properties"})
        .withArguments(std::string{kLocIface})
        .uponReplyInvoke([this](std::optional<sdbus::Error> err,
                                std::map<std::string, sdbus::Variant> props) {
          if (err) {
            log::w(kTag, "GeoClue2 location read failed ({}: {})", err->getName(), err->getMessage());
            return;
          }
          deliver_props(props);
        });
  }

  void deliver_props(std::map<std::string, sdbus::Variant>& p) {
    auto d = [&p](const char* k) -> double {
      auto it = p.find(k);
      return it != p.end() ? it->second.get<double>() : 0.0;
    };
    LocationFix f{};
    f.latitude = d("Latitude");
    f.longitude = d("Longitude");
    f.accuracy_m = d("Accuracy");
    const double alt = d("Altitude");
    const double spd = d("Speed");
    const double hdg = d("Heading");
    f.has_altitude = alt_known(alt);
    if (f.has_altitude) f.altitude_m = alt;
    f.has_speed = nonneg_known(spd);
    if (f.has_speed) f.speed_mps = spd;
    f.has_bearing = nonneg_known(hdg);
    if (f.has_bearing) f.bearing_deg = hdg;
    std::int64_t secs = 0;
    if (auto it = p.find("Timestamp"); it != p.end()) {
      auto ts = it->second.get<sdbus::Struct<uint64_t, uint64_t>>();
      secs = static_cast<std::int64_t>(ts.get<0>());
    }
    f.timestamp_ms = secs != 0 ? secs * 1000 : static_cast<std::int64_t>(::time(nullptr)) * 1000;
    if (f.accuracy_m < 0) f.accuracy_m = 0;
    if (sink_) sink_(f);
  }

  LocationSink sink_;
  std::unique_ptr<sdbus::IConnection> conn_;   // async loop: LocationUpdated + async Location reads
  std::unique_ptr<sdbus::IProxy> client_;
  std::unique_ptr<sdbus::IProxy> location_;    // current Location proxy (keeps the async read alive)
};
#endif  // ACL_HAVE_SDBUS

}  // namespace

std::unique_ptr<LocationSource> make_location_source() {
  LocationFix synth{};
  if (parse_latlon(std::getenv("ACL_LOCATION_LATLON"), &synth)) {
    log::i(kTag, "location source: synthetic ({},{} +/-{}m) from ACL_LOCATION_LATLON",
           synth.latitude, synth.longitude, synth.accuracy_m);
    return std::make_unique<SyntheticSource>(synth);
  }
#if ACL_HAVE_SDBUS
  return std::make_unique<GeoClue2Source>();
#else
  log::i(kTag, "location source: none (built without sdbus-c++; set ACL_LOCATION_LATLON to override)");
  return std::make_unique<NullSource>();
#endif
}

LocationHub& LocationHub::instance() {
  static LocationHub hub;
  return hub;
}

void LocationHub::set_listener(LocationSink listener) {
  std::lock_guard<std::mutex> lk(m_);
  listener_ = std::move(listener);
}

bool LocationHub::start() {
  {
    std::lock_guard<std::mutex> lk(m_);
    if (started_) return source_ != nullptr && std::strcmp(source_->name(), "none") != 0;  // already up
    started_ = true;
    source_ = make_location_source();
  }
  // start() the source outside the lock (it may block on D-Bus / call the sink re-entrantly).
  const bool ok = source_->start([this](const LocationFix& f) { on_fix(f); });
  log::i(kTag, "location hub started (source={}, delivering={})", source_->name(), ok);
  return ok;
}

void LocationHub::stop() {
  std::lock_guard<std::mutex> lk(m_);
  if (source_) source_->stop();
  started_ = false;
}

void LocationHub::on_fix(const LocationFix& f) {
  LocationSink cb;
  {
    std::lock_guard<std::mutex> lk(m_);
    last_ = f;
    have_last_ = true;
    cb = listener_;
  }
  if (cb) cb(f);
}

bool LocationHub::last(LocationFix* out) const {
  std::lock_guard<std::mutex> lk(m_);
  if (!have_last_ || out == nullptr) return false;
  *out = last_;
  return true;
}

bool LocationHub::available() const {
  std::lock_guard<std::mutex> lk(m_);
  // A real source is anything but the NullSource ("none").
  return source_ != nullptr && std::strcmp(source_->name(), "none") != 0;
}

const char* LocationHub::source_name() const {
  std::lock_guard<std::mutex> lk(m_);
  return source_ ? source_->name() : "unstarted";
}

}  // namespace acl::svc::location
