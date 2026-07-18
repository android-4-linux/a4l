// SPDX-License-Identifier: Apache-2.0
// Provenance: freedesktop XDG Desktop Portal org.freedesktop.portal.Settings API
// (color-scheme in the org.freedesktop.appearance namespace) → android.content.res.
// Configuration.uiMode UI_MODE_NIGHT_* (AOSP android.jar stubs, Apache-2.0). No GPL-3.0 sources.
#pragma once

namespace acl::appearance {

// The desktop's preferred color scheme, from the XDG Settings portal
// (org.freedesktop.appearance color-scheme):
//   0 = no preference, 1 = prefer dark, 2 = prefer light.
// Returns -1 when the portal is unreachable or sdbus is not built (caller keeps the default).
int query_color_scheme() noexcept;

}  // namespace acl::appearance
