# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §A01.2 (media-app bridge). No GPL-3.0 sources.
#
# hosts/media/ — MediaBrowserService / MediaSession bridge. Renders no UI;
# publishes browse-tree snapshots + playback state (seqlock) and a
# MediaSession->MPRIS bridge, over the audio slice -> PipeWire. Gate G1.5a.
# Supports media3 first, legacy support-lib via compat shim (RA-2).
# Captured so src/runtime can link_with it (the runtime drives the bridge); also
# added to module_libs so the unit-test binary links it (nm-gate probe).
host_media_lib = static_library(
  'acl_host_media',
  files('media_bridge.cpp'),
  include_directories : [config_inc, core_inc],
)
module_libs += host_media_lib
