// SPDX-License-Identifier: Apache-2.0
// Provenance: original test harness; libwayland-server API (freedesktop, MIT);
// xdg-shell protocol (freedesktop, MIT). No GPL-3.0 sources.
//
// A minimal test compositor for the presenter's close path (Tier C). It advertises
// wl_compositor + wl_shm (via libwayland's shm helper) + xdg_wm_base, walks a client
// through the xdg-shell handshake, and once the client maps its window (commits a
// buffer) sends xdg_toplevel.close — the one thing headless Weston can't be told to
// do. The presenter under test must react to close (OnToplevelClose) and exit
// cleanly. The compositor terminates when the client disconnects (or after a safety
// timeout), so the test can't hang.
//
// Only enough of each interface is implemented to reach the first buffer commit;
// unused requests are left null (never sent by the client at these versions).

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <sys/mman.h>
#include <unistd.h>

#include <wayland-server.h>
#include <xkbcommon/xkbcommon.h>

#include "text-input-unstable-v3-server-protocol.h"
#include "xdg-shell-server-protocol.h"

namespace {

wl_display* g_display = nullptr;
std::string g_keymap;  // xkb keymap text, sent so client-side key-repeat can arm
wl_resource* g_text_input = nullptr;  // the client's zwp_text_input_v3, to commit text into
std::uint32_t g_serial = 0;
wl_resource* g_pointer = nullptr;   // the client's wl_pointer, for injecting a tap
wl_resource* g_keyboard = nullptr;  // the client's wl_keyboard, for injecting a key
wl_resource* g_toplevel = nullptr;  // the mapped xdg_toplevel (for a delayed close)
bool g_repeat_mode = false;         // hold a key (repeat_info) and delay the close
wl_event_source* g_close_timer = nullptr;
void arm_shutdown();  // defined below; called after close is sent

struct Surf {
  wl_resource* xdg_surface = nullptr;
  wl_resource* xdg_toplevel = nullptr;
  wl_resource* frame_cb = nullptr;
  wl_resource* pending_buffer = nullptr;
  bool configured = false;
  bool close_sent = false;
};

Surf* surf_of(wl_resource* r) { return static_cast<Surf*>(wl_resource_get_user_data(r)); }

// ── wl_region (never exercised, but create_region must yield a live resource) ──
void region_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void region_add(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void region_subtract(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
const struct wl_region_interface region_impl = {region_destroy, region_add, region_subtract};

// ── wl_surface ────────────────────────────────────────────────────────────────
void surface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void surface_attach(wl_client*, wl_resource* r, wl_resource* buffer, int32_t, int32_t) {
  surf_of(r)->pending_buffer = buffer;
}
void surface_damage(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void surface_frame(wl_client* c, wl_resource* r, uint32_t callback) {
  Surf* s = surf_of(r);
  s->frame_cb = wl_resource_create(c, &wl_callback_interface, 1, callback);
}
void surface_set_opaque_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_set_input_region(wl_client*, wl_resource*, wl_resource*) {}
void surface_commit(wl_client*, wl_resource* r) {
  Surf* s = surf_of(r);
  // First commit after the xdg role exists: send the initial configure.
  if (s->xdg_surface != nullptr && !s->configured) {
    s->configured = true;
    xdg_surface_send_configure(s->xdg_surface, ++g_serial);
    return;
  }
  // The window is now mapped (a buffer was attached) — inject input, then close.
  if (s->xdg_toplevel != nullptr && s->pending_buffer != nullptr && !s->close_sent) {
    s->close_sent = true;
    g_toplevel = s->xdg_toplevel;
    if (s->frame_cb != nullptr) {
      wl_callback_send_done(s->frame_cb, 1);
      wl_resource_destroy(s->frame_cb);
      s->frame_cb = nullptr;
    }
    wl_buffer_send_release(s->pending_buffer);
    if (g_keyboard != nullptr) {
      wl_array keys;
      wl_array_init(&keys);
      wl_keyboard_send_enter(g_keyboard, ++g_serial, r, &keys);
      wl_array_release(&keys);
    }
    if (g_repeat_mode) {
      // Hold KEY_A with a repeat rate so the client's repeat timerfd fires; close
      // after a delay so several repeats accumulate.
      if (g_keyboard != nullptr && wl_resource_get_version(g_keyboard) >= 4) {
        wl_keyboard_send_repeat_info(g_keyboard, 40 /*keys/s*/, 40 /*delay ms*/);
        wl_keyboard_send_key(g_keyboard, ++g_serial, 10, 30 /*KEY_A held*/,
                             WL_KEYBOARD_KEY_STATE_PRESSED);
      }
      std::fprintf(stderr, "close-compositor: repeat mode -> held key, delayed close\n");
      wl_event_source_timer_update(g_close_timer, 500);
    } else {
      // Inject a pointer tap (onTouchEvent) and a single key press+release
      // (onKeyDown/onKeyUp), then close immediately.
      if (g_pointer != nullptr) {
        wl_pointer_send_enter(g_pointer, ++g_serial, r, wl_fixed_from_int(100), wl_fixed_from_int(50));
        wl_pointer_send_button(g_pointer, ++g_serial, 1, 0x110 /*BTN_LEFT*/, WL_POINTER_BUTTON_STATE_PRESSED);
        wl_pointer_send_button(g_pointer, ++g_serial, 2, 0x110, WL_POINTER_BUTTON_STATE_RELEASED);
      }
      if (g_keyboard != nullptr) {
        wl_keyboard_send_key(g_keyboard, ++g_serial, 10, 30 /*KEY_A*/, WL_KEYBOARD_KEY_STATE_PRESSED);
        wl_keyboard_send_key(g_keyboard, ++g_serial, 11, 30, WL_KEYBOARD_KEY_STATE_RELEASED);
      }
      // If the app enabled text input, act as its IME: enter, a composing preedit
      // ("hel"), then a final commit ("hello").
      if (g_text_input != nullptr) {
        zwp_text_input_v3_send_enter(g_text_input, r);
        zwp_text_input_v3_send_delete_surrounding_text(g_text_input, 1, 0);
        zwp_text_input_v3_send_preedit_string(g_text_input, "hel", 3, 3);
        zwp_text_input_v3_send_commit_string(g_text_input, "hello");
        zwp_text_input_v3_send_done(g_text_input, ++g_serial);
      }
      std::fprintf(stderr, "close-compositor: window mapped -> sent tap + key + IME + xdg_toplevel.close\n");
      xdg_toplevel_send_close(s->xdg_toplevel);
      arm_shutdown();
    }
  }
}
const struct wl_surface_interface surface_impl = {
    surface_destroy, surface_attach, surface_damage, surface_frame,
    surface_set_opaque_region, surface_set_input_region, surface_commit};

void surface_res_destroy(wl_resource* r) { delete surf_of(r); }

// ── wl_compositor ─────────────────────────────────────────────────────────────
void comp_create_surface(wl_client* c, wl_resource* r, uint32_t id) {
  auto* s = new Surf();
  wl_resource* sr = wl_resource_create(c, &wl_surface_interface, wl_resource_get_version(r), id);
  wl_resource_set_implementation(sr, &surface_impl, s, surface_res_destroy);
}
void comp_create_region(wl_client* c, wl_resource*, uint32_t id) {
  wl_resource* rr = wl_resource_create(c, &wl_region_interface, 1, id);
  wl_resource_set_implementation(rr, &region_impl, nullptr, nullptr);
}
const struct wl_compositor_interface compositor_impl = {comp_create_surface, comp_create_region};

// ── xdg_positioner (never exercised) ──────────────────────────────────────────
void positioner_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct xdg_positioner_interface positioner_impl = {positioner_destroy};

// ── xdg_toplevel ──────────────────────────────────────────────────────────────
void toplevel_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void toplevel_set_parent(wl_client*, wl_resource*, wl_resource*) {}
void toplevel_set_title(wl_client*, wl_resource*, const char*) {}
void toplevel_set_app_id(wl_client*, wl_resource*, const char*) {}
const struct xdg_toplevel_interface toplevel_impl = {
    toplevel_destroy, toplevel_set_parent, toplevel_set_title, toplevel_set_app_id};

// ── xdg_surface ───────────────────────────────────────────────────────────────
void xdgsurface_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void xdgsurface_get_toplevel(wl_client* c, wl_resource* r, uint32_t id) {
  wl_resource* tl = wl_resource_create(c, &xdg_toplevel_interface, wl_resource_get_version(r), id);
  wl_resource_set_implementation(tl, &toplevel_impl, nullptr, nullptr);
  // Link the toplevel back to the wl_surface's state (xdg_surface user_data is Surf*).
  surf_of(r)->xdg_toplevel = tl;
}
void xdgsurface_get_popup(wl_client*, wl_resource*, uint32_t, wl_resource*, wl_resource*) {}
void xdgsurface_set_window_geometry(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void xdgsurface_ack_configure(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_surface_interface xdg_surface_impl = {
    xdgsurface_destroy, xdgsurface_get_toplevel, xdgsurface_get_popup,
    xdgsurface_set_window_geometry, xdgsurface_ack_configure};

// ── xdg_wm_base ───────────────────────────────────────────────────────────────
void wmbase_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void wmbase_create_positioner(wl_client* c, wl_resource*, uint32_t id) {
  wl_resource* pr = wl_resource_create(c, &xdg_positioner_interface, 1, id);
  wl_resource_set_implementation(pr, &positioner_impl, nullptr, nullptr);
}
void wmbase_get_xdg_surface(wl_client* c, wl_resource*, uint32_t id, wl_resource* surface) {
  Surf* s = surf_of(surface);
  wl_resource* xs = wl_resource_create(c, &xdg_surface_interface, 1, id);
  // The xdg_surface shares the wl_surface's Surf* so commit() can see the role.
  wl_resource_set_implementation(xs, &xdg_surface_impl, s, nullptr);
  s->xdg_surface = xs;
}
void wmbase_pong(wl_client*, wl_resource*, uint32_t) {}
const struct xdg_wm_base_interface wmbase_impl = {
    wmbase_destroy, wmbase_create_positioner, wmbase_get_xdg_surface, wmbase_pong};

// ── wl_pointer (server side; sends enter/button to the client) ────────────────
void pointer_set_cursor(wl_client*, wl_resource*, uint32_t, wl_resource*, int32_t, int32_t) {}
void pointer_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_pointer_interface pointer_impl = {pointer_set_cursor, pointer_release};
void pointer_res_destroy(wl_resource*) { g_pointer = nullptr; }

// ── wl_keyboard (server side; sends key events to the client) ─────────────────
void keyboard_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_keyboard_interface keyboard_impl = {keyboard_release};
void keyboard_res_destroy(wl_resource*) { g_keyboard = nullptr; }

// ── wl_seat ───────────────────────────────────────────────────────────────────
void seat_get_pointer(wl_client* c, wl_resource* r, uint32_t id) {
  g_pointer = wl_resource_create(c, &wl_pointer_interface, wl_resource_get_version(r), id);
  wl_resource_set_implementation(g_pointer, &pointer_impl, nullptr, pointer_res_destroy);
}
// Send the xkb keymap so the client's KeyboardHandler can arm client-side repeat
// (which only fires once a keymap is known).
void send_keymap(wl_resource* kbd) {
  if (g_keymap.empty()) return;
  const std::size_t size = g_keymap.size() + 1;
  int fd = memfd_create("keymap", MFD_CLOEXEC);
  if (fd < 0) return;
  if (ftruncate(fd, static_cast<off_t>(size)) == 0) {
    void* p = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
      std::memcpy(p, g_keymap.c_str(), size);
      munmap(p, size);
      wl_keyboard_send_keymap(kbd, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, fd,
                              static_cast<uint32_t>(size));
    }
  }
  close(fd);
}

void seat_get_keyboard(wl_client* c, wl_resource* r, uint32_t id) {
  g_keyboard = wl_resource_create(c, &wl_keyboard_interface, wl_resource_get_version(r), id);
  wl_resource_set_implementation(g_keyboard, &keyboard_impl, nullptr, keyboard_res_destroy);
  send_keymap(g_keyboard);
}
void seat_get_touch(wl_client*, wl_resource*, uint32_t) {}
void seat_release(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
const struct wl_seat_interface seat_impl = {
    seat_get_pointer, seat_get_keyboard, seat_get_touch, seat_release};

// ── zwp_text_input_v3 (server side; sends commit_string to the client) ────────
void ti_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void ti_enable(wl_client*, wl_resource*) {}
void ti_disable(wl_client*, wl_resource*) {}
void ti_set_surrounding_text(wl_client*, wl_resource*, const char*, int32_t, int32_t) {}
void ti_set_text_change_cause(wl_client*, wl_resource*, uint32_t) {}
void ti_set_content_type(wl_client*, wl_resource*, uint32_t, uint32_t) {}
void ti_set_cursor_rectangle(wl_client*, wl_resource*, int32_t, int32_t, int32_t, int32_t) {}
void ti_commit(wl_client*, wl_resource*) {}  // events are sent on window map, below
const struct zwp_text_input_v3_interface ti_impl = {
    ti_destroy, ti_enable, ti_disable, ti_set_surrounding_text,
    ti_set_text_change_cause, ti_set_content_type, ti_set_cursor_rectangle, ti_commit};
void ti_res_destroy(wl_resource*) { g_text_input = nullptr; }

// ── zwp_text_input_manager_v3 ─────────────────────────────────────────────────
void tim_destroy(wl_client*, wl_resource* r) { wl_resource_destroy(r); }
void tim_get_text_input(wl_client* c, wl_resource* r, uint32_t id, wl_resource* /*seat*/) {
  g_text_input = wl_resource_create(c, &zwp_text_input_v3_interface, wl_resource_get_version(r), id);
  wl_resource_set_implementation(g_text_input, &ti_impl, nullptr, ti_res_destroy);
}
const struct zwp_text_input_manager_v3_interface tim_impl = {tim_destroy, tim_get_text_input};

// ── globals ───────────────────────────────────────────────────────────────────
void bind_text_input_manager(wl_client* c, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(c, &zwp_text_input_manager_v3_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &tim_impl, nullptr, nullptr);
}
void bind_seat(wl_client* c, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(c, &wl_seat_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &seat_impl, nullptr, nullptr);
  wl_seat_send_capabilities(r, WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD);
}
void bind_compositor(wl_client* c, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(c, &wl_compositor_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &compositor_impl, nullptr, nullptr);
}
void bind_wmbase(wl_client* c, void*, uint32_t version, uint32_t id) {
  wl_resource* r = wl_resource_create(c, &xdg_wm_base_interface, static_cast<int>(version), id);
  wl_resource_set_implementation(r, &wmbase_impl, nullptr, nullptr);
}

// Terminate a short moment after close is sent — enough for the client to react
// and disconnect — so the compositor exits on its own after the interesting event.
// (We do NOT terminate on plain client disconnect: the probe connects once just to
// check availability and disconnects before the real presentation connection.)
int on_shutdown(void*) { wl_display_terminate(g_display); return 0; }
wl_event_source* g_shutdown_timer = nullptr;
void arm_shutdown() {
  if (g_shutdown_timer != nullptr) wl_event_source_timer_update(g_shutdown_timer, 1000);
}

// Repeat mode: after holding a key long enough for repeats to fire, close.
int on_delayed_close(void*) {
  if (g_toplevel != nullptr) xdg_toplevel_send_close(g_toplevel);
  arm_shutdown();
  return 0;
}

int on_timeout(void*) {
  std::fprintf(stderr, "close-compositor: safety timeout\n");
  wl_display_terminate(g_display);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  const char* socket = argc > 1 ? argv[1] : "aclcxx-close";
  g_repeat_mode = argc > 2 && std::string_view(argv[2]) == "repeat";

  // Compile the default (RMLVO) xkb keymap once, to send to clients.
  if (xkb_context* ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS)) {
    if (xkb_keymap* km = xkb_keymap_new_from_names(ctx, nullptr, XKB_KEYMAP_COMPILE_NO_FLAGS)) {
      if (char* s = xkb_keymap_get_as_string(km, XKB_KEYMAP_FORMAT_TEXT_V1)) {
        g_keymap = s;
        std::free(s);
      }
      xkb_keymap_unref(km);
    }
    xkb_context_unref(ctx);
  }

  g_display = wl_display_create();
  if (g_display == nullptr) { std::fprintf(stderr, "close-compositor: wl_display_create failed\n"); return 1; }
  if (wl_display_add_socket(g_display, socket) != 0) {
    std::fprintf(stderr, "close-compositor: cannot add socket %s\n", socket);
    return 1;
  }
  wl_display_init_shm(g_display);  // provides wl_shm/pool/buffer
  wl_global_create(g_display, &wl_compositor_interface, 4, nullptr, bind_compositor);
  wl_global_create(g_display, &xdg_wm_base_interface, 1, nullptr, bind_wmbase);
  wl_global_create(g_display, &wl_seat_interface, 4, nullptr, bind_seat);  // v4: wl_keyboard.repeat_info
  wl_global_create(g_display, &zwp_text_input_manager_v3_interface, 1, nullptr, bind_text_input_manager);

  wl_event_loop* loop = wl_display_get_event_loop(g_display);
  wl_event_source* timer = wl_event_loop_add_timer(loop, on_timeout, nullptr);
  wl_event_source_timer_update(timer, 20000);  // 20s safety net
  g_shutdown_timer = wl_event_loop_add_timer(loop, on_shutdown, nullptr);  // armed after close
  g_close_timer = wl_event_loop_add_timer(loop, on_delayed_close, nullptr);  // repeat mode

  std::fprintf(stderr, "close-compositor: ready on %s\n", socket);
  wl_display_run(g_display);
  wl_display_destroy(g_display);
  return 0;
}
