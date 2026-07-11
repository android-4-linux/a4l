# SPDX-License-Identifier: Apache-2.0
# Provenance: plan.md §A01.2 (media-app bridge). No GPL-3.0 sources.
#
# hosts/media/ — MediaBrowserService / MediaSession bridge. Renders no UI;
# publishes browse-tree snapshots + playback state (seqlock) and a
# MediaSession->MPRIS bridge, over the audio slice -> PipeWire. Gate G1.5a.
# Supports media3 first, legacy support-lib via compat shim (RA-2).
module_libs += static_library(
  'acl_host_media',
  files('media_bridge.cpp'),
  include_directories : config_inc,
)
