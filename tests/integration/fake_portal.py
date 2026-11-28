# SPDX-License-Identifier: Apache-2.0
# Provenance: original a4l test helper; the freedesktop OpenURI portal's D-Bus interface
# (org.freedesktop.portal.OpenURI, method OpenURI). No GPL-3.0 sources.
#
# A stand-in for xdg-desktop-portal that answers OpenURI and opens nothing: it takes the
# portal's name on the bus it is given, writes each address it is asked to open to a file,
# one a line, and returns a request handle. Prints "ready" once it has the name.
#
# Needs PyGObject; exits 77 without it.
#
# Usage: fake_portal.py <bus address> <record file>
import sys

try:
    from gi.repository import Gio, GLib
except ImportError:
    raise SystemExit(77)

INTERFACE = """<node>
  <interface name='org.freedesktop.portal.OpenURI'>
    <method name='OpenURI'>
      <arg type='s' name='parent_window' direction='in'/>
      <arg type='s' name='uri' direction='in'/>
      <arg type='a{sv}' name='options' direction='in'/>
      <arg type='o' name='handle' direction='out'/>
    </method>
  </interface>
</node>"""


def main():
    if len(sys.argv) != 3:
        sys.stderr.write("usage: fake_portal.py <bus address> <record file>\n")
        return 2
    address, record = sys.argv[1], sys.argv[2]
    bus = Gio.DBusConnection.new_for_address_sync(
        address,
        Gio.DBusConnectionFlags.AUTHENTICATION_CLIENT
        | Gio.DBusConnectionFlags.MESSAGE_BUS_CONNECTION, None, None)

    def called(_bus, _sender, _path, _interface, _method, parameters, invocation):
        with open(record, "a", encoding="utf-8") as fh:
            fh.write(parameters.unpack()[1] + "\n")
        invocation.return_value(GLib.Variant(
            "(o)", ("/org/freedesktop/portal/desktop/request/a4l/1",)))

    bus.register_object("/org/freedesktop/portal/desktop",
                        Gio.DBusNodeInfo.new_for_xml(INTERFACE).interfaces[0],
                        called, None, None)
    bus.call_sync("org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
                  "RequestName",
                  GLib.Variant("(su)", ("org.freedesktop.portal.Desktop", 0)), None,
                  Gio.DBusCallFlags.NONE, -1, None)
    sys.stdout.write("ready\n")
    sys.stdout.flush()
    GLib.MainLoop().run()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
