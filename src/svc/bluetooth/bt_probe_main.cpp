// SPDX-License-Identifier: Apache-2.0
// Provenance: acl dev tool; prints the svc/bluetooth backend + derived adapter state.
// No GPL-3.0 sources.
//
// Prints which host Bluetooth daemon acl selects and the derived BluetoothAdapter
// state (power/address/name/discovering + bonded devices), so the BlueZ -> android.bluetooth
// mapping can be eyeballed on desktop/device.
#include <cstdio>

#include "svc/bluetooth/bt_backend.h"

int main() {
  auto backend = acl::bt::make_bt_backend();
  const acl::bt::AndroidBtState s = backend->snapshot();
  std::printf("acl bluetooth backend: %s\n", backend->name());
  std::printf("  present      = %s\n", s.present ? "true" : "false");
  std::printf("  state        = %d  (10 off, 12 on)\n", s.state);
  std::printf("  address      = %s\n", s.address.c_str());
  std::printf("  name         = %s\n", s.name.c_str());
  std::printf("  discovering  = %s\n", s.discovering ? "true" : "false");
  std::printf("  bonded (%zu):\n", s.bonded.size());
  for (const acl::bt::BtDevice& d : s.bonded)
    std::printf("    %s  \"%s\"  class=0x%x  bond=%d  connected=%s\n", d.address.c_str(),
                d.name.c_str(), d.cls, d.bond_state, d.connected ? "y" : "n");
  return 0;
}
