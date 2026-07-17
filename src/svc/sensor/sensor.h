// SPDX-License-Identifier: Apache-2.0
// Provenance: Linux IIO sysfs ABI (Documentation/ABI/testing/sysfs-bus-iio — in_accel_*_raw,
// _scale, sampling_frequency; kernel public ABI), Android SDK android.hardware.Sensor /
// SensorEvent (SI units, TYPE_* values). Intel's android-iio-sensors-hal (Apache-2.0) is the
// prior art for the IIO→Android mapping; iio-sensor-proxy's D-Bus API (net.hadess.SensorProxy)
// is the optional high-level source — its GPL code is NOT used, only its public interface. No GPL-3.0 sources.
//
// acl's sensor hub: the native underside of android.hardware.SensorManager. A pluggable
// SensorSource seam (same idea as the GL backend seam) so the platform's sensors — raw Linux
// IIO, iio-sensor-proxy, or a synthetic default — feed the one SensorManager shim above without
// it caring which. Sources are stateless readers; the hub owns enumeration, rate scheduling
// (a poll thread), and delivery to the JNI listener.

#ifndef ACL_SVC_SENSOR_SENSOR_H
#define ACL_SVC_SENSOR_SENSOR_H

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace acl::svc::sensor {

// The android.hardware.Sensor.TYPE_* subset acl maps (SI-unit sensors).
enum SensorType : int {
  kTypeAccelerometer = 1,   // m/s^2, 3 values
  kTypeMagneticField = 2,   // uT, 3 values
  kTypeGyroscope = 4,       // rad/s, 3 values
  kTypeLight = 5,           // lux, 1 value
  kTypeProximity = 8,       // cm, 1 value
};

// Mirrors the fields android.hardware.Sensor needs (the JNI hands these to AclSensorManager,
// which reflects them into a real Sensor object).
struct SensorDesc {
  int handle = 0;           // acl-assigned, 1-based, stable for the process
  int type = 0;             // Android Sensor.TYPE_*
  std::string name;
  std::string vendor;
  int version = 1;
  float max_range = 0.0f;
  float resolution = 0.0f;
  float power = 0.0f;       // mA
  int min_delay_us = 0;     // fastest sampling period (0 = on-change)
  int value_count = 3;      // floats per SensorEvent
};

// One reading (android.hardware.SensorEvent).
struct SensorSample {
  int handle = 0;
  int value_count = 0;
  float values[16] = {};
  int accuracy = 3;         // SensorManager.SENSOR_STATUS_ACCURACY_HIGH
  std::int64_t timestamp_ns = 0;
};

// A backend providing zero or more sensors and reading their current value by Android type.
// Sources are stateless w.r.t. scheduling — the hub calls read() at each sensor's rate. A new
// platform backend is a new subclass here; nothing above the hub changes.
class SensorSource {
 public:
  virtual ~SensorSource() = default;
  virtual const char* source_name() const = 0;
  virtual std::vector<SensorDesc> enumerate() = 0;  // handle left 0 (the hub assigns handles)
  virtual bool enable(int /*type*/, int /*period_us*/) { return true; }
  virtual void disable(int /*type*/) {}
  virtual bool read(int type, SensorSample* out) = 0;  // fill *out for the given Android type
};

// Pushed one sample per delivery (the JNI forwards to AclSensorManager.onSensorEvent).
using EventFn = std::function<void(const SensorSample&)>;

// The process-wide hub: aggregates the compiled-in sources (dedup by type, first source wins —
// IIO > proxy > synthetic), assigns handles, and runs a poll thread that reads each enabled
// sensor at its period and delivers via the listener.
class SensorHub {
 public:
  ~SensorHub();
  SensorHub(const SensorHub&) = delete;
  SensorHub& operator=(const SensorHub&) = delete;

  static SensorHub& instance();

  const std::vector<SensorDesc>& sensors() const { return sensors_; }

  bool enable(int handle, int period_us);  // start delivering; period clamped to min_delay
  void disable(int handle);
  void set_listener(EventFn fn);           // the JNI sink (may be called from the poll thread)

 private:
  SensorHub();
  void poll_loop();

  struct Active {
    int handle = 0;
    int type = 0;
    SensorSource* source = nullptr;
    std::int64_t period_ns = 0;
    std::int64_t next_due_ns = 0;
  };

  std::vector<std::unique_ptr<SensorSource>> sources_;
  std::vector<SensorDesc> sensors_;                  // aggregated, deduped
  std::vector<SensorSource*> owners_;                // owners_[i] provides sensors_[i]
  std::vector<Active> active_;                       // currently enabled
  EventFn listener_;
  struct Impl;
  std::unique_ptr<Impl> impl_;                       // thread + sync (keeps this header STL-only)
};

}  // namespace acl::svc::sensor

#endif  // ACL_SVC_SENSOR_SENSOR_H
