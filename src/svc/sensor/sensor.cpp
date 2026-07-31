// SPDX-License-Identifier: Apache-2.0
// Provenance: Linux IIO sysfs ABI (Documentation/ABI/testing/sysfs-bus-iio — in_accel_*_raw,
// in_accel_scale, in_anglvel_*, in_illuminance_*, in_proximity_*, name; kernel public ABI),
// Android SDK SensorEvent SI units. Prior art (mapping only, not code): Intel android-iio-
// sensors-hal (Apache-2.0). No GPL-3.0 sources.

#include "svc/sensor/sensor.h"

#include <dirent.h>
#include <time.h>

#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>

#include "log/log.h"

namespace acl::svc::sensor {
// Keyboard-driven tilt (ACL_TILT_KEYS): the input path (present) sets a normalized tilt in [-1,1] from
// the arrow keys, and the synthetic accelerometer turns it into a tilted gravity vector — so tilt games
// (libGDX doodledudette etc.) are playable on a desktop with no real accelerometer. External linkage so
// the exported C setter below can reach it; read unqualified by SyntheticSource in the anon namespace.
std::atomic<float> g_kbd_tilt_x{0.0f};
std::atomic<float> g_kbd_tilt_y{0.0f};
namespace {
constexpr const char* kTag = "acl.svc.sensor";

std::int64_t now_ns() {
  timespec ts{};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return static_cast<std::int64_t>(ts.tv_sec) * 1000000000LL + ts.tv_nsec;
}

// ── sysfs helpers (IIO) ──────────────────────────────────────────────────────
bool read_str(const std::string& path, std::string* out) {
  std::ifstream f(path);
  if (!f) return false;
  std::getline(f, *out);
  return true;
}
bool read_float(const std::string& path, float* out) {
  std::string s;
  if (!read_str(path, &s) || s.empty()) return false;
  errno = 0;
  char* end = nullptr;
  const float v = std::strtof(s.c_str(), &end);
  if (end == s.c_str()) return false;
  *out = v;
  return true;
}
bool exists(const std::string& path) {
  std::ifstream f(path);
  return f.good();
}

// ── Synthetic source: a deterministic accelerometer (always available) ───────
// The working example on hardware without IIO sensors (desktops): reports gravity along -Z with
// a small, time-varying wobble so the stream is visibly live. Enough for an app to enumerate a
// TYPE_ACCELEROMETER and receive a moving SensorEvent stream.
class SyntheticSource final : public SensorSource {
 public:
  const char* source_name() const override { return "synthetic"; }
  std::vector<SensorDesc> enumerate() override {
    SensorDesc a{};
    a.type = kTypeAccelerometer;
    a.name = "acl Synthetic Accelerometer";
    a.vendor = "acl";
    a.max_range = 2.0f * 9.80665f;  // ±2g
    a.resolution = 9.80665f / 256.0f;
    a.power = 0.0f;
    a.min_delay_us = 10000;  // up to 100 Hz
    a.value_count = 3;
    return {a};
  }
  bool read(int type, SensorSample* out) override {
    if (type != kTypeAccelerometer) return false;
    constexpr float kG = 9.80665f;
    const float tx = g_kbd_tilt_x.load(std::memory_order_relaxed);
    const float ty = g_kbd_tilt_y.load(std::memory_order_relaxed);
    out->value_count = 3;
    if (tx != 0.0f || ty != 0.0f) {
      // Keyboard tilt (ACL_TILT_KEYS): report gravity as if the device were tilted. tx/ty in [-1,1] are
      // the sine of the lean about the Y/X axes; Z keeps whatever gravity is left so |g| stays ~9.8.
      const float cx = std::sqrt(std::max(0.0f, 1.0f - tx * tx - ty * ty));
      out->values[0] = kG * tx;
      out->values[1] = kG * ty;
      out->values[2] = kG * cx;
    } else {
      // No tilt input: device face-up with a small time-varying sway so the stream is visibly live.
      const double t = static_cast<double>(now_ns()) / 1e9;
      const float wob = 0.15f * static_cast<float>(std::sin(t));
      out->values[0] = wob;
      out->values[1] = wob * 0.5f;
      out->values[2] = kG;
    }
    out->accuracy = 3;
    return true;
  }
};

// ── IIO source: real Linux sensors via /sys/bus/iio/devices ──────────────────
// Reads the raw-channel + scale sysfs interface (no buffer/trigger streaming — a simple
// per-read sysfs poll, which the hub schedules at the sensor's rate). Enumerates whatever the
// kernel exposes; on a machine with no IIO sensors this yields nothing and the hub falls back
// to the synthetic source.
class IioSource final : public SensorSource {
 public:
  const char* source_name() const override { return "iio"; }

  std::vector<SensorDesc> enumerate() override {
    devices_.clear();
    std::vector<SensorDesc> out;
    DIR* d = ::opendir(kIioRoot);
    if (d == nullptr) return out;
    for (dirent* e = ::readdir(d); e != nullptr; e = ::readdir(d)) {
      const std::string n = e->d_name;
      if (n.rfind("iio:device", 0) != 0) continue;
      const std::string base = std::string(kIioRoot) + "/" + n;
      std::string dev_name = "IIO";
      read_str(base + "/name", &dev_name);
      // Accelerometer (the mapped example; gyro/magn/light/prox follow the same shape).
      if (exists(base + "/in_accel_x_raw")) {
        SensorDesc a{};
        a.type = kTypeAccelerometer;
        a.name = dev_name + " Accelerometer";
        a.vendor = "iio";
        a.max_range = 4.0f * 9.80665f;
        a.min_delay_us = 10000;
        a.value_count = 3;
        float scale = 1.0f;
        read_float(base + "/in_accel_scale", &scale);
        a.resolution = scale;
        out.push_back(a);
        devices_.push_back({kTypeAccelerometer, base, "in_accel", scale});
      }
    }
    ::closedir(d);
    return out;
  }

  bool read(int type, SensorSample* out) override {
    for (const Dev& dv : devices_) {
      if (dv.type != type) continue;
      float scale = dv.scale;
      // per-read scale may change (rare); refresh cheaply
      read_float(dv.base + "/" + dv.prefix + "_scale", &scale);
      float x = 0, y = 0, z = 0;
      const bool ok = read_float(dv.base + "/" + dv.prefix + "_x_raw", &x) &&
                      read_float(dv.base + "/" + dv.prefix + "_y_raw", &y) &&
                      read_float(dv.base + "/" + dv.prefix + "_z_raw", &z);
      if (!ok) return false;
      out->value_count = 3;
      out->values[0] = x * scale;
      out->values[1] = y * scale;
      out->values[2] = z * scale;
      out->accuracy = 3;
      return true;
    }
    return false;
  }

 private:
  static constexpr const char* kIioRoot = "/sys/bus/iio/devices";
  struct Dev {
    int type;
    std::string base;
    std::string prefix;  // e.g. "in_accel"
    float scale;
  };
  std::vector<Dev> devices_;
};

}  // namespace

// ── SensorHub ────────────────────────────────────────────────────────────────
struct SensorHub::Impl {
  std::thread thread;
  std::mutex mtx;
  std::condition_variable cv;
  std::atomic<bool> running{false};
  bool wake = false;
};

SensorHub::SensorHub() : impl_(std::make_unique<Impl>()) {
  // Backend order = dedup priority: real IIO first, synthetic as the always-present fallback.
  sources_.push_back(std::make_unique<IioSource>());
  sources_.push_back(std::make_unique<SyntheticSource>());

  int next_handle = 1;
  for (auto& src : sources_) {
    for (SensorDesc d : src->enumerate()) {
      bool dup = false;
      for (const SensorDesc& have : sensors_)
        if (have.type == d.type) { dup = true; break; }
      if (dup) continue;  // an earlier (higher-priority) source already provides this type
      d.handle = next_handle++;
      // Remember which source owns this handle (parallel to sensors_ order).
      owners_.push_back(src.get());
      sensors_.push_back(std::move(d));
    }
  }
  log::i(kTag, "sensor hub: {} sensor(s) across {} source(s)", sensors_.size(), sources_.size());
}

SensorHub::~SensorHub() {
  if (impl_->running.exchange(false)) {
    { std::lock_guard<std::mutex> lk(impl_->mtx); impl_->wake = true; }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) impl_->thread.join();
  }
}

SensorHub& SensorHub::instance() {
  static SensorHub hub;
  return hub;
}

void SensorHub::set_listener(EventFn fn) {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  listener_ = std::move(fn);
}

bool SensorHub::enable(int handle, int period_us) {
  SensorSource* source = nullptr;
  int type = 0;
  std::int64_t min_us = 0;
  for (std::size_t i = 0; i < sensors_.size(); ++i) {
    if (sensors_[i].handle == handle) {
      source = owners_[i];
      type = sensors_[i].type;
      min_us = sensors_[i].min_delay_us;
      break;
    }
  }
  if (source == nullptr) return false;
  if (period_us <= 0) period_us = static_cast<int>(min_us > 0 ? min_us : 20000);
  if (min_us > 0 && period_us < min_us) period_us = static_cast<int>(min_us);
  source->enable(type, period_us);
  {
    std::lock_guard<std::mutex> lk(impl_->mtx);
    for (Active& a : active_)
      if (a.handle == handle) { a.period_ns = period_us * 1000LL; a.next_due_ns = now_ns(); goto woke; }
    active_.push_back({handle, type, source, period_us * 1000LL, now_ns()});
  woke:;
    impl_->wake = true;
    if (!impl_->running.exchange(true)) impl_->thread = std::thread([this] { poll_loop(); });
  }
  impl_->cv.notify_all();
  return true;
}

void SensorHub::disable(int handle) {
  std::lock_guard<std::mutex> lk(impl_->mtx);
  for (std::size_t i = 0; i < active_.size(); ++i) {
    if (active_[i].handle == handle) {
      if (active_[i].source != nullptr) active_[i].source->disable(active_[i].type);
      active_.erase(active_.begin() + static_cast<long>(i));
      break;
    }
  }
}

void SensorHub::poll_loop() {
  while (impl_->running.load()) {
    std::vector<Active> due;
    std::int64_t sleep_ns = 100000000LL;  // 100ms idle cap
    EventFn listener;
    {
      std::unique_lock<std::mutex> lk(impl_->mtx);
      if (active_.empty())
        impl_->cv.wait_for(lk, std::chrono::milliseconds(200), [this] { return impl_->wake || !impl_->running.load(); });
      impl_->wake = false;
      if (!impl_->running.load()) break;
      const std::int64_t t = now_ns();
      for (Active& a : active_) {
        if (a.next_due_ns <= t) {
          due.push_back(a);
          a.next_due_ns = t + a.period_ns;
        }
        const std::int64_t until = a.next_due_ns - t;
        if (until < sleep_ns) sleep_ns = until;
      }
      listener = listener_;
    }
    for (const Active& a : due) {
      SensorSample s{};
      s.handle = a.handle;
      if (a.source->read(a.type, &s)) {
        s.handle = a.handle;
        s.timestamp_ns = now_ns();
        if (listener) listener(s);
      }
    }
    if (sleep_ns > 0) {
      std::unique_lock<std::mutex> lk(impl_->mtx);
      impl_->cv.wait_for(lk, std::chrono::nanoseconds(sleep_ns), [this] { return impl_->wake || !impl_->running.load(); });
    }
  }
}

}  // namespace acl::svc::sensor

// Exported for the input path (present) to drive the synthetic accelerometer from the arrow keys
// (ACL_TILT_KEYS). nx/ny in [-1,1]: left/right and up/down lean. dlsym'd by src/runtime (jvm_vm) so the
// runtime need not link svc/sensor directly. No-op effect unless the synthetic source is the active
// accelerometer (i.e. no real IIO sensor), which is the desktop case these tilt games hit.
extern "C" void acl_sensor_kbd_tilt(float nx, float ny) {
  const float cx = nx < -1.0f ? -1.0f : nx > 1.0f ? 1.0f : nx;
  const float cy = ny < -1.0f ? -1.0f : ny > 1.0f ? 1.0f : ny;
  acl::svc::sensor::g_kbd_tilt_x.store(cx, std::memory_order_relaxed);
  acl::svc::sensor::g_kbd_tilt_y.store(cy, std::memory_order_relaxed);
}
