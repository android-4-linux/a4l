// SPDX-License-Identifier: Apache-2.0
// Provenance: original test helper; the freedesktop Desktop Notifications spec
// (org.freedesktop.Notifications); sdbus-c++ usage after
// jwinarske/sdbus-cpp-examples (Apache-2.0). No GPL-3.0 sources.
//
// A stand-in notification daemon for run_notify_test.py. It owns
// org.freedesktop.Notifications on the session bus it is started on, prints
// each Notify and CloseNotification call on stdout, and invokes the first
// notification's "a0" action a moment after it is posted, as a user's click on
// the button would.
#include <sdbus-c++/sdbus-c++.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

namespace {

constexpr const char* kService = "org.freedesktop.Notifications";
constexpr const char* kPath = "/org/freedesktop/Notifications";
constexpr const char* kIface = "org.freedesktop.Notifications";

using ImageData = sdbus::Struct<int32_t,
                                int32_t,
                                int32_t,
                                bool,
                                int32_t,
                                int32_t,
                                std::vector<uint8_t>>;

// "WxH/rowstride/alpha/bits/channels/bytes/RRGGBBAA" for the image-data hint,
// the last field being its first pixel; "none" without one.
std::string describe_image(const std::map<std::string, sdbus::Variant>& hints) {
  const auto it = hints.find("image-data");
  if (it == hints.end())
    return "none";
  const auto img = it->second.get<ImageData>();
  const auto& px = std::get<6>(img);
  char buf[96];
  std::snprintf(buf, sizeof buf, "%dx%d/%d/%d/%d/%d/%zu/%02x%02x%02x%02x",
                std::get<0>(img), std::get<1>(img), std::get<2>(img),
                std::get<3>(img) ? 1 : 0, std::get<4>(img), std::get<5>(img),
                px.size(), px.size() > 0 ? px[0] : 0, px.size() > 1 ? px[1] : 0,
                px.size() > 2 ? px[2] : 0, px.size() > 3 ? px[3] : 0);
  return buf;
}

}  // namespace

int main() {
  auto conn = sdbus::createSessionBusConnection(sdbus::ServiceName{kService});
  auto object = sdbus::createObject(*conn, sdbus::ObjectPath{kPath});
  uint32_t next_id = 1;
  bool invoked = false;

  auto notify = [&](const std::string& app, uint32_t replaces,
                    const std::string& /*icon*/, const std::string& summary,
                    const std::string& body,
                    const std::vector<std::string>& actions,
                    const std::map<std::string, sdbus::Variant>& hints,
                    int32_t /*timeout*/) -> uint32_t {
    const uint32_t id = replaces != 0 ? replaces : next_id++;
    std::string list;
    bool has_a0 = false;
    for (std::size_t i = 0; i + 1 < actions.size(); i += 2) {
      list += (list.empty() ? "" : ",") + actions[i] + ":" + actions[i + 1];
      has_a0 = has_a0 || actions[i] == "a0";
    }
    const auto resident = hints.find("resident");
    std::printf(
        "STUB: notify id=%u replaces=%u app=%s summary=%s body=%s actions=%s "
        "image=%s resident=%d\n",
        id, replaces, app.c_str(), summary.c_str(), body.c_str(), list.c_str(),
        describe_image(hints).c_str(),
        resident != hints.end() && resident->second.get<bool>() ? 1 : 0);
    std::fflush(stdout);
    if (has_a0 && !invoked) {
      invoked = true;
      // After the reply: the caller learns the id from it.
      std::thread([&object, id] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        object->emitSignal("ActionInvoked")
            .onInterface(kIface)
            .withArguments(id, std::string{"a0"});
      }).detach();
    }
    return id;
  };
  auto close = [&](uint32_t id) {
    std::printf("STUB: close id=%u\n", id);
    std::fflush(stdout);
    object->emitSignal("NotificationClosed")
        .onInterface(kIface)
        .withArguments(id, uint32_t{3});
  };
  auto capabilities = [] {
    return std::vector<std::string>{"actions", "body"};
  };
  auto information = [] {
    return std::make_tuple(std::string{"a4l test stub"}, std::string{"a4l"},
                           std::string{"1"}, std::string{"1.2"});
  };

  object
      ->addVTable(
          sdbus::registerMethod("Notify").implementedAs(notify),
          sdbus::registerMethod("CloseNotification").implementedAs(close),
          sdbus::registerMethod("GetCapabilities").implementedAs(capabilities),
          sdbus::registerMethod("GetServerInformation")
              .implementedAs(information),
          sdbus::registerSignal("ActionInvoked")
              .withParameters<uint32_t, std::string>(),
          sdbus::registerSignal("NotificationClosed")
              .withParameters<uint32_t, uint32_t>())
      .forInterface(sdbus::InterfaceName{kIface});

  std::printf("STUB: ready\n");
  std::fflush(stdout);
  conn->enterEventLoop();
  return 0;
}
