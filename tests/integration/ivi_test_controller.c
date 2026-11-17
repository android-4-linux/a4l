// SPDX-License-Identifier: Apache-2.0
// Provenance: original a4l test code against Weston's public ivi-layout plugin
// API (ivi-layout-export.h, MIT). No GPL-3.0 sources.
//
// A minimal ivi-shell controller for run_ivi_shell_test.py. ivi-shell shows
// nothing by itself: a controller module puts each ivi surface on a layer and
// gives it a size, which the client is told with ivi_surface.configure. This
// one puts every surface on one layer at A4L_IVI_SIZE (WxH), and logs what it
// did so the test can read it.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <ivi-layout-export.h>
#include <libweston/libweston.h>

static const struct ivi_layout_interface* api;
static struct weston_compositor* compositor;
static struct ivi_layout_layer* layer;
static struct ivi_layout_surface* placed;
static struct wl_listener on_configure, on_create;
static int width = 640, height = 400;

static void created(struct wl_listener* l, void* data) {
  (void)l;
  weston_log("a4l-ctl: surface %u created\n",
             api->get_id_of_surface((struct ivi_layout_surface*)data));
}

// Runs when a surface gets content. Outputs are enabled after modules load,
// so the layer is made here and not in wet_module_init.
static void configured(struct wl_listener* l, void* data) {
  (void)l;
  struct ivi_layout_surface* s = data;
  if (s == placed)
    return;
  if (layer == NULL) {
    if (wl_list_empty(&compositor->output_list))
      return;
    struct weston_output* out =
        wl_container_of(compositor->output_list.next, out, link);
    layer = api->layer_create_with_dimension(4000, out->width, out->height);
    if (layer == NULL)
      return;
    api->layer_set_visibility(layer, true);
    api->screen_add_layer(out, layer);
    api->commit_changes();
#ifdef A4L_HAVE_SCREEN_READY
    api->screen_ready(out);  // nothing is painted on the output before this
#endif
  }
  placed = s;
  api->layer_add_surface(layer, s);
  api->surface_set_destination_rectangle(s, 0, 0, width, height);
  api->surface_set_visibility(s, true);
  api->commit_changes();
  weston_log("a4l-ctl: surface %u -> %dx%d\n", api->get_id_of_surface(s), width,
             height);
}

WL_EXPORT int wet_module_init(struct weston_compositor* ec,
                              int* argc,
                              char* argv[]) {
  (void)argc;
  (void)argv;
  api = ivi_layout_get_api(ec);
  if (api == NULL)
    return -1;
  compositor = ec;
  const char* e = getenv("A4L_IVI_SIZE");
  if (e != NULL)
    sscanf(e, "%dx%d", &width, &height);
  on_configure.notify = configured;
  api->add_listener_configure_surface(&on_configure);
  on_create.notify = created;
  api->add_listener_create_surface(&on_create);
  weston_log("a4l-ctl: up, %dx%d\n", width, height);
  return 0;
}
