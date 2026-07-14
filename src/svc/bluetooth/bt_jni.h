// SPDX-License-Identifier: Apache-2.0
// Provenance: acl native seam binding org.acl.internal.BluetoothBridge (clean-room)
// to the svc/bluetooth host BlueZ backend. No GPL-3.0 sources.
#ifndef ACL_SVC_BLUETOOTH_BT_JNI_H
#define ACL_SVC_BLUETOOTH_BT_JNI_H

#include <jni.h>

namespace acl::bt {

// Create the host Bluetooth backend (make_bt_backend) and RegisterNatives on
// org.acl.internal.BluetoothBridge. Best-effort: on any failure the class keeps no
// native and the android.bluetooth shims report an absent/off adapter. Safe to call
// once at host-ART boot, after the acl boot classpath is up.
void register_bt_natives(JNIEnv* env);

}  // namespace acl::bt

#endif  // ACL_SVC_BLUETOOTH_BT_JNI_H
