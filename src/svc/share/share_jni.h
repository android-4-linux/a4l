// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.HostShare to the xdg-desktop-portal
// OpenURI method (org.freedesktop.portal.Desktop, session bus). Freedesktop portal spec. No GPL-3.0 sources.
#ifndef ACL_SVC_SHARE_SHARE_JNI_H
#define ACL_SVC_SHARE_SHARE_JNI_H

#include <jni.h>

namespace acl::share {

// RegisterNatives on org.acl.internal.HostShare (nOpenUri) — a Share of a URL asks the host's
// xdg-desktop-portal to open it (browser / handler chooser), the sandbox-correct way to hand a URI
// to the desktop. Best-effort; safe to call once at host-ART boot. Built only with sdbus-c++.
void register_share_natives(JNIEnv* env);

}  // namespace acl::share

#endif  // ACL_SVC_SHARE_SHARE_JNI_H
