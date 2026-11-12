// SPDX-License-Identifier: Apache-2.0
// Provenance: PipeWire pw_registry / pw_metadata / pw_node / pw_device
// SPA_PARAM_Props + SPA_PARAM_Route (channelVolumes, mute) public API +
// examples (MIT); default-sink tracking after jwinarske/pw_dart (Apache-2.0).
// No GPL-3.0 sources. See pw_volume.h.
//
// The audible volume of a hardware sink lives on its DEVICE's Route param (the
// ALSA mixer), not the sink node's Props — PipeWire mirrors the route volume
// into the node on read but a node-Props set does not reach the hardware. So we
// set the device Route when the sink has one (device.id + card.profile.device),
// and fall back to node Props for pure-software sinks.
#include "audio/pw_volume.h"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "acl_config.h"  // ACL_HAVE_PIPEWIRE — must precede the guarded PipeWire includes
#include "log/log.h"

#if ACL_HAVE_PIPEWIRE
#include <pipewire/extensions/metadata.h>  // pw_metadata / PW_TYPE_INTERFACE_Metadata (default sink)
#include <pipewire/pipewire.h>
#include <spa/param/audio/raw.h>  // SPA_AUDIO_MAX_CHANNELS
#include <spa/param/param.h>
#include <spa/param/props.h>
#include <spa/param/route.h>  // SPA_PARAM_Route (hardware/device volume)
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/pod/parser.h>
#include <spa/utils/result.h>
#endif

namespace acl::audio {
namespace {

constexpr std::string_view kTag = "acl.audio.vol";

class NullVolume : public IPwVolume {
 public:
  bool available() const override { return false; }
  float volume() const override { return 1.0f; }
  bool muted() const override { return false; }
  bool set_volume(float) override { return false; }
  bool set_muted(bool) override { return false; }
  int channels() const override { return 2; }
};

#if ACL_HAVE_PIPEWIRE

std::string json_name(const char* value) {
  if (value == nullptr)
    return {};
  const std::string v(value);
  const auto k = v.find("\"name\"");
  if (k == std::string::npos)
    return {};
  const auto q1 = v.find('"', v.find(':', k));
  if (q1 == std::string::npos)
    return {};
  const auto q2 = v.find('"', q1 + 1);
  if (q2 == std::string::npos)
    return {};
  return v.substr(q1 + 1, q2 - q1 - 1);
}

class PwVolume : public IPwVolume {
 public:
  bool start() {
    pw_init(nullptr, nullptr);
    loop_ = pw_thread_loop_new("acl-audio-vol", nullptr);
    if (loop_ == nullptr)
      return false;
    pw_thread_loop_lock(loop_);
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (context_ == nullptr) {
      fail();
      return false;
    }
    core_ = pw_context_connect(context_, nullptr, 0);
    if (core_ == nullptr) {
      fail();
      return false;
    }
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (registry_ == nullptr) {
      fail();
      return false;
    }

    static const pw_registry_events revents = [] {
      pw_registry_events e{};
      e.version = PW_VERSION_REGISTRY_EVENTS;
      e.global = &PwVolume::on_global;
      e.global_remove = &PwVolume::on_global_remove;
      return e;
    }();
    pw_registry_add_listener(registry_, &registry_hook_, &revents, this);

    if (pw_thread_loop_start(loop_) != 0) {
      fail();
      return false;
    }
    // Settle in stages: registry globals + bind "default" metadata → its
    // default.audio.sink property → bind the sink node + its device → the
    // device Route (volume).
    for (int i = 0; i < 4; ++i)
      roundtrip();
    pw_thread_loop_unlock(loop_);
    return true;
  }
  ~PwVolume() override {
    if (loop_ != nullptr)
      pw_thread_loop_stop(loop_);
    clear_target_locked();
    if (metadata_ != nullptr)
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_));
    if (registry_ != nullptr)
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
    if (core_ != nullptr)
      pw_core_disconnect(core_);
    if (context_ != nullptr)
      pw_context_destroy(context_);
    if (loop_ != nullptr)
      pw_thread_loop_destroy(loop_);
  }

  bool available() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return target_id_ != SPA_ID_INVALID;
  }
  float volume() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return volume_;
  }
  bool muted() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return mute_;
  }
  int channels() const override {
    std::lock_guard<std::mutex> lk(mu_);
    return channels_ > 0 ? channels_ : 2;
  }
  bool set_volume(float v) override {
    if (v < 0.0f)
      v = 0.0f;
    if (v > 1.0f)
      v = 1.0f;
    {
      std::lock_guard<std::mutex> lk(mu_);
      volume_ = v;
    }
    return push_props();
  }
  bool set_muted(bool m) override {
    {
      std::lock_guard<std::mutex> lk(mu_);
      mute_ = m;
    }
    return push_props();
  }

 private:
  struct Node {
    std::string name;
    bool sink = false;
    uint32_t device_id =
        SPA_ID_INVALID;     // owning Audio/Device (hardware volume lives there)
    int route_device = -1;  // card.profile.device (which route device on it)
  };

  void fail() {
    if (registry_ != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
      registry_ = nullptr;
    }
    if (core_ != nullptr) {
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_ != nullptr) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    pw_thread_loop_unlock(loop_);
    pw_thread_loop_destroy(loop_);
    loop_ = nullptr;
  }

  void roundtrip() {
    static const pw_core_events cevents = [] {
      pw_core_events e{};
      e.version = PW_VERSION_CORE_EVENTS;
      e.done = &PwVolume::on_core_done;
      return e;
    }();
    spa_hook core_hook{};
    pw_core_add_listener(core_, &core_hook, &cevents, this);
    sync_seq_ = pw_core_sync(core_, PW_ID_CORE, 0);
    pending_sync_ = true;
    while (pending_sync_)
      pw_thread_loop_wait(loop_);
    spa_hook_remove(&core_hook);
  }
  static void on_core_done(void* data, uint32_t id, int seq) {
    auto* self = static_cast<PwVolume*>(data);
    if (id == PW_ID_CORE && seq == self->sync_seq_) {
      self->pending_sync_ = false;
      pw_thread_loop_signal(self->loop_, false);
    }
  }

  // ── registry: sink nodes (+ their device) and the "default" metadata
  // ─────────────
  static void on_global(void* data,
                        uint32_t id,
                        uint32_t /*perm*/,
                        const char* type,
                        uint32_t /*version*/,
                        const spa_dict* props) {
    auto* self = static_cast<PwVolume*>(data);
    if (type != nullptr && std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      const char* mc = props != nullptr
                           ? spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)
                           : nullptr;
      const char* nn =
          props != nullptr ? spa_dict_lookup(props, PW_KEY_NODE_NAME) : nullptr;
      if (mc != nullptr && std::strcmp(mc, "Audio/Sink") == 0 &&
          nn != nullptr) {
        Node n;
        n.name = nn;
        n.sink = true;
        if (const char* dev = spa_dict_lookup(props, PW_KEY_DEVICE_ID))
          n.device_id = atoi_u32(dev);
        if (const char* rd = spa_dict_lookup(props, "card.profile.device"))
          n.route_device = atoi(rd);
        self->nodes_[id] = n;
        if (self->default_name_ == n.name)
          self->retarget(id);
      }
    } else if (type != nullptr &&
               std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
      const char* mn = props != nullptr
                           ? spa_dict_lookup(props, PW_KEY_METADATA_NAME)
                           : nullptr;
      if (mn != nullptr && std::strcmp(mn, "default") == 0 &&
          self->metadata_ == nullptr) {
        self->bind_metadata(id);
      }
    }
  }
  static uint32_t atoi_u32(const char* s) {
    return static_cast<uint32_t>(strtoul(s, nullptr, 10));
  }

  static void on_global_remove(void* data, uint32_t id) {
    auto* self = static_cast<PwVolume*>(data);
    self->nodes_.erase(id);
    if (id == self->target_id_)
      self->clear_target();  // headphones/monitor unplugged
  }

  void bind_metadata(uint32_t id) {
    metadata_ = static_cast<pw_metadata*>(pw_registry_bind(
        registry_, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
    if (metadata_ == nullptr)
      return;
    static const pw_metadata_events mevents = [] {
      pw_metadata_events e{};
      e.version = PW_VERSION_METADATA_EVENTS;
      e.property = &PwVolume::on_metadata_property;
      return e;
    }();
    pw_metadata_add_listener(metadata_, &metadata_hook_, &mevents, this);
  }
  static int on_metadata_property(void* data,
                                  uint32_t subject,
                                  const char* key,
                                  const char* /*type*/,
                                  const char* value) {
    auto* self = static_cast<PwVolume*>(data);
    if (subject == 0 && key != nullptr &&
        std::strcmp(key, "default.audio.sink") == 0) {
      self->default_name_ = json_name(value);
      for (const auto& [nid, node] : self->nodes_) {
        if (node.sink && node.name == self->default_name_) {
          self->retarget(nid);
          break;
        }
      }
    }
    return 0;
  }

  // ── target: bind the sink node + (for hardware) its device, and follow the
  // volume ───
  void retarget(uint32_t id) {
    if (id == target_id_)
      return;
    auto it = nodes_.find(id);
    if (it == nodes_.end())
      return;
    clear_target();
    const Node n = it->second;
    node_ = static_cast<pw_node*>(pw_registry_bind(
        registry_, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (node_ == nullptr)
      return;
    {
      std::lock_guard<std::mutex> lk(mu_);
      target_id_ = id;
      route_device_ = n.route_device;
    }
    static const pw_node_events nevents = [] {
      pw_node_events e{};
      e.version = PW_VERSION_NODE_EVENTS;
      e.param = &PwVolume::on_node_param;
      return e;
    }();
    pw_node_add_listener(node_, &node_hook_, &nevents, this);
    const uint32_t nids[1] = {SPA_PARAM_Props};
    pw_node_subscribe_params(node_, const_cast<uint32_t*>(nids), 1);

    // Hardware sink → bind its device and follow the active OUTPUT Route (the
    // audible volume).
    if (n.device_id != SPA_ID_INVALID) {
      device_ = static_cast<pw_device*>(
          pw_registry_bind(registry_, n.device_id, PW_TYPE_INTERFACE_Device,
                           PW_VERSION_DEVICE, 0));
      if (device_ != nullptr) {
        static const pw_device_events devents = [] {
          pw_device_events e{};
          e.version = PW_VERSION_DEVICE_EVENTS;
          e.param = &PwVolume::on_device_param;
          return e;
        }();
        pw_device_add_listener(device_, &device_hook_, &devents, this);
        const uint32_t dids[1] = {SPA_PARAM_Route};
        pw_device_subscribe_params(device_, const_cast<uint32_t*>(dids), 1);
      }
    }
    log::i(kTag, "master output -> {} (node {}, device {})", default_name_, id,
           n.device_id);
  }
  void clear_target() {
    if (node_ != nullptr) {
      spa_hook_remove(&node_hook_);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node_));
      node_ = nullptr;
    }
    if (device_ != nullptr) {
      spa_hook_remove(&device_hook_);
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(device_));
      device_ = nullptr;
    }
    std::lock_guard<std::mutex> lk(mu_);
    target_id_ = SPA_ID_INVALID;
    route_index_ = -1;
    route_device_ = -1;
  }
  void clear_target_locked() {  // destructor path, loop already stopped
    if (node_ != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(node_));
      node_ = nullptr;
    }
    if (device_ != nullptr) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy*>(device_));
      device_ = nullptr;
    }
  }

  // Software-sink fallback volume (mirrors hardware on read; authoritative for
  // pure-software sinks).
  static void on_node_param(void* data,
                            int /*seq*/,
                            uint32_t id,
                            uint32_t /*index*/,
                            uint32_t /*next*/,
                            const spa_pod* param) {
    if (id != SPA_PARAM_Props || param == nullptr)
      return;
    auto* self = static_cast<PwVolume*>(data);
    if (self->route_index_ >= 0)
      return;  // hardware sink: the Route is authoritative
    self->read_volume_props(param);
  }
  // Hardware volume: the device Route matching our card.profile.device.
  static void on_device_param(void* data,
                              int /*seq*/,
                              uint32_t id,
                              uint32_t /*index*/,
                              uint32_t /*next*/,
                              const spa_pod* param) {
    if (id != SPA_PARAM_Route || param == nullptr)
      return;
    auto* self = static_cast<PwVolume*>(data);
    int32_t r_index = -1, r_device = -1;
    uint32_t r_dir = SPA_DIRECTION_INPUT;
    const spa_pod* props = nullptr;
    spa_pod_parser p;
    spa_pod_parser_pod(&p, param);
    if (spa_pod_parser_get_object(
            &p, SPA_TYPE_OBJECT_ParamRoute, nullptr, SPA_PARAM_ROUTE_index,
            SPA_POD_Int(&r_index), SPA_PARAM_ROUTE_direction,
            SPA_POD_Id(&r_dir), SPA_PARAM_ROUTE_device, SPA_POD_Int(&r_device),
            SPA_PARAM_ROUTE_props, SPA_POD_OPT_Pod(&props)) < 0)
      return;
    if (r_dir != SPA_DIRECTION_OUTPUT)
      return;  // the sink's active output route
    {
      std::lock_guard<std::mutex> lk(self->mu_);
      self->route_index_ = r_index;
      self->route_device_ = r_device;
    }
    if (props != nullptr)
      self->read_volume_props(props);
  }
  void read_volume_props(const spa_pod* props) {
    float vols[SPA_AUDIO_MAX_CHANNELS];
    if (const spa_pod_prop* p =
            spa_pod_find_prop(props, nullptr, SPA_PROP_channelVolumes)) {
      const uint32_t n = spa_pod_copy_array(&p->value, SPA_TYPE_Float, vols,
                                            SPA_AUDIO_MAX_CHANNELS);
      if (n > 0) {
        std::lock_guard<std::mutex> lk(mu_);
        volume_ = vols[0];
        channels_ = static_cast<int>(n);
      }
    }
    if (const spa_pod_prop* p =
            spa_pod_find_prop(props, nullptr, SPA_PROP_mute)) {
      bool m = false;
      if (spa_pod_get_bool(&p->value, &m) == 0) {
        std::lock_guard<std::mutex> lk(mu_);
        mute_ = m;
      }
    }
  }

  // Build a Props sub-pod (channelVolumes + mute) into builder `b`.
  const spa_pod* build_props(spa_pod_builder* b, float v, bool m, int ch) {
    float vols[SPA_AUDIO_MAX_CHANNELS];
    if (ch > static_cast<int>(SPA_AUDIO_MAX_CHANNELS))
      ch = static_cast<int>(SPA_AUDIO_MAX_CHANNELS);
    for (int i = 0; i < ch; ++i)
      vols[i] = v;
    spa_pod_frame f;
    spa_pod_builder_push_object(b, &f, SPA_TYPE_OBJECT_Props, SPA_PARAM_Props);
    spa_pod_builder_prop(b, SPA_PROP_mute, 0);
    spa_pod_builder_bool(b, m);
    spa_pod_builder_prop(b, SPA_PROP_channelVolumes, 0);
    spa_pod_builder_array(b, sizeof(float), SPA_TYPE_Float,
                          static_cast<uint32_t>(ch), vols);
    return static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f));
  }

  bool push_props() {
    if (loop_ == nullptr)
      return false;
    pw_thread_loop_lock(loop_);
    float v;
    bool m;
    int ch, ridx, rdev;
    {
      std::lock_guard<std::mutex> lk(mu_);
      v = volume_;
      m = mute_;
      ch = channels_ > 0 ? channels_ : 2;
      ridx = route_index_;
      rdev = route_device_;
    }
    std::uint8_t buffer[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    bool ok = false;
    if (device_ != nullptr && ridx >= 0) {
      // Hardware: set the device Route {index, device, props}.
      spa_pod_frame f;
      spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamRoute,
                                  SPA_PARAM_Route);
      spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_index, 0);
      spa_pod_builder_int(&b, ridx);
      spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_device, 0);
      spa_pod_builder_int(&b, rdev);
      spa_pod_builder_prop(&b, SPA_PARAM_ROUTE_props, 0);
      build_props(&b, v, m, ch);
      const spa_pod* pod =
          static_cast<const spa_pod*>(spa_pod_builder_pop(&b, &f));
      pw_device_set_param(device_, SPA_PARAM_Route, 0, pod);
      ok = true;
    } else if (node_ != nullptr) {
      const spa_pod* pod = build_props(&b, v, m, ch);
      pw_node_set_param(node_, SPA_PARAM_Props, 0, pod);
      ok = true;
    }
    pw_thread_loop_unlock(loop_);
    return ok;
  }

  pw_thread_loop* loop_ = nullptr;
  pw_context* context_ = nullptr;
  pw_core* core_ = nullptr;
  pw_registry* registry_ = nullptr;
  pw_metadata* metadata_ = nullptr;
  pw_node* node_ = nullptr;
  pw_device* device_ = nullptr;
  spa_hook registry_hook_{};
  spa_hook metadata_hook_{};
  spa_hook node_hook_{};
  spa_hook device_hook_{};
  std::map<uint32_t, Node> nodes_;
  std::string default_name_;
  int sync_seq_ = 0;
  bool pending_sync_ = false;

  mutable std::mutex mu_;
  uint32_t target_id_ = SPA_ID_INVALID;
  int route_index_ =
      -1;  // >=0 → hardware Route to set; -1 → software node Props
  int route_device_ = -1;
  float volume_ = 1.0f;
  bool mute_ = false;
  int channels_ = 2;
};

#endif  // ACL_HAVE_PIPEWIRE

}  // namespace

std::unique_ptr<IPwVolume> make_pw_volume() {
#if ACL_HAVE_PIPEWIRE
  auto v = std::make_unique<PwVolume>();
  if (v->start()) {
    log::i(kTag,
           "pipewire volume controller up (following default.audio.sink)");
    return v;
  }
  log::w(kTag, "pipewire volume unavailable; master volume is a no-op");
#endif
  return std::make_unique<NullVolume>();
}

}  // namespace acl::audio
