# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit a tiny, self-contained "hello world" APK for the end-to-end run-path test:
# a single class com.aclcxx.hello.Main whose
#   public static void main(String[]) { System.out.println(<MSG or Probe.tag()>); }
# is translatable by the DEX->JVM converter and runnable on the JVM backend, plus
# a binary AndroidManifest.xml whose launcher activity resolves to that class (so
# `aclcxx run hello.apk` needs no --main). Independent of the C++ decoder.
#
# In --probe mode, main() instead calls com.aclcxx.fw.Probe.tag() — a class that is
# NOT bundled in the APK, so it must be supplied via the framework boot classpath.
# This proves the --framework / boot_classpath seam (Gate G2.a).
#
# Usage: make_hello_apk.py [--probe] <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MSG = "aclcxx: hello from a real APK"
PROBE_CLASS = "com.aclcxx.fw.Probe"
LOG_TAG = "aclcxx.app"
LOG_MSG = "hello via android.util.Log"


def uleb128(n):
    out = bytearray()
    while True:
        b = n & 0x7F
        n >>= 7
        if n:
            out.append(b | 0x80)
        else:
            out.append(b)
            return bytes(out)


def align4(buf):
    while len(buf) % 4 != 0:
        buf += b"\x00"
    return buf


def build_dex(mode="hello"):
    # Interned strings; the Probe/Log entities are always present but only
    # referenced by main() in the matching mode (unused refs are valid and never
    # resolved).
    STRINGS = [
        "Lcom/aclcxx/hello/Main;",   # 0  this class
        "Ljava/lang/Object;",        # 1  superclass
        "Ljava/lang/String;",        # 2  String
        "[Ljava/lang/String;",       # 3  String[]
        "Ljava/lang/System;",        # 4  System
        "Ljava/io/PrintStream;",     # 5  PrintStream
        "V",                         # 6  void
        "VL",                        # 7  shorty (Object)->void
        "L",                         # 8  shorty ()->Object
        "main",                      # 9
        "out",                       # 10
        "println",                   # 11
        "tag",                       # 12
        "Lcom/aclcxx/fw/Probe;",     # 13 framework-supplied (not bundled)
        MSG,                         # 14
        "I",                         # 15 int (Log.i return type / shorty)
        "Landroid/util/Log;",        # 16 framework class (native underside)
        "i",                         # 17
        "ILL",                       # 18 shorty (String,String)->int
        LOG_TAG,                     # 19
        LOG_MSG,                     # 20
    ]
    S = {s: i for i, s in enumerate(STRINGS)}
    TYPES = ["Lcom/aclcxx/hello/Main;", "Ljava/lang/Object;", "Ljava/lang/String;",
             "[Ljava/lang/String;", "Ljava/lang/System;", "Ljava/io/PrintStream;",
             "V", "Lcom/aclcxx/fw/Probe;", "I", "Landroid/util/Log;"]
    T = {t: i for i, t in enumerate(TYPES)}

    header_size = 112
    n_strings, n_types, n_protos, n_fields, n_methods, n_classdefs = \
        len(STRINGS), len(TYPES), 4, 1, 4, 1

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_field_ids = off_proto_ids + n_protos * 12
    off_method_ids = off_field_ids + n_fields * 8
    off_class_defs = off_method_ids + n_methods * 8
    off_data = off_class_defs + n_classdefs * 32

    data = bytearray()
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(off_data + len(data))
        data.extend(uleb128(len(s)) + b + b"\x00")

    data.extend(align4(bytearray()))
    type_list_argv_off = off_data + len(data)
    data.extend(align4(bytearray(
        struct.pack("<I", 1) + struct.pack("<H", T["[Ljava/lang/String;"]))))
    data.extend(align4(bytearray()))
    type_list_str_off = off_data + len(data)
    data.extend(align4(bytearray(
        struct.pack("<I", 1) + struct.pack("<H", T["Ljava/lang/String;"]))))
    data.extend(align4(bytearray()))
    type_list_2str_off = off_data + len(data)
    data.extend(align4(bytearray(
        struct.pack("<I", 2) + struct.pack("<H", T["Ljava/lang/String;"]) * 2)))

    code_main_off = off_data + len(data)
    if mode == "probe":
        # System.out.println(Probe.tag()) — Probe is on the boot classpath only.
        insns = [0x0062, 0x0000,          # sget-object v0, System.out (f0)
                 0x0071, 0x0002, 0x0000,  # invoke-static {}, Probe.tag() (m2)
                 0x010C,                  # move-result-object v1
                 0x206E, 0x0001, 0x0010,  # invoke-virtual {v0,v1}, println (m1)
                 0x000E]                  # return-void
    elif mode == "log":
        # android.util.Log.i(LOG_TAG, LOG_MSG) — Log is a framework class whose
        # native underside is provided by aclcxx (result int ignored).
        insns = [0x001A, S[LOG_TAG],      # const-string v0, LOG_TAG
                 0x011A, S[LOG_MSG],      # const-string v1, LOG_MSG
                 0x2071, 0x0003, 0x0010,  # invoke-static {v0,v1}, Log.i (m3)
                 0x000E]                  # return-void
    else:
        # System.out.println(MSG) — fully self-contained.
        insns = [0x0062, 0x0000,          # sget-object v0, System.out
                 0x011A, S[MSG],          # const-string v1, MSG
                 0x206E, 0x0001, 0x0010,  # invoke-virtual {v0,v1}, println
                 0x000E]                  # return-void
    body = struct.pack("<HHHHII", 2, 1, 2, 0, 0, len(insns))
    for u in insns:
        body += struct.pack("<H", u)
    data.extend(align4(bytearray(body)))

    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(0))
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main_off)  # method0 main, public static
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0),
        (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids),
        (0x0003, n_protos, off_proto_ids),
        (0x0004, n_fields, off_field_ids),
        (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]),
        (0x1001, 2, type_list_argv_off),
        (0x2001, 1, code_main_off),
        (0x2000, 1, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = struct.pack("<III", S["VL"], T["V"], type_list_argv_off)   # main (String[])V
    proto_ids += struct.pack("<III", S["VL"], T["V"], type_list_str_off)   # println (String)V
    proto_ids += struct.pack("<III", S["L"], T["Ljava/lang/String;"], 0)   # tag ()String
    proto_ids += struct.pack("<III", S["ILL"], T["I"], type_list_2str_off)  # (String,String)I
    field_ids = struct.pack("<HHI", T["Ljava/lang/System;"],
                            T["Ljava/io/PrintStream;"], S["out"])          # f0 System.out
    method_ids = struct.pack("<HHI", T["Lcom/aclcxx/hello/Main;"], 0, S["main"])   # m0
    method_ids += struct.pack("<HHI", T["Ljava/io/PrintStream;"], 1, S["println"])  # m1
    method_ids += struct.pack("<HHI", T["Lcom/aclcxx/fw/Probe;"], 2, S["tag"])      # m2
    method_ids += struct.pack("<HHI", T["Landroid/util/Log;"], 3, S["i"])           # m3 Log.i
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/aclcxx/hello/Main;"], 0x1,
                             T["Ljava/lang/Object;"], 0, 0xFFFFFFFF, 0,
                             class_data_off, 0)

    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)
    struct.pack_into("<I", header, 0x34, map_off)
    for off, (size, val) in {
        0x38: (n_strings, off_string_ids), 0x40: (n_types, off_type_ids),
        0x48: (n_protos, off_proto_ids), 0x50: (n_fields, off_field_ids),
        0x58: (n_methods, off_method_ids), 0x60: (n_classdefs, off_class_defs),
    }.items():
        struct.pack_into("<I", header, off, size)
        struct.pack_into("<I", header, off + 4, val)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        field_ids + method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


def build_apk(mode="hello"):
    return {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.Main"),
        "classes.dex": build_dex(mode=mode),
    }


def main():
    args = sys.argv[1:]
    mode = "hello"
    if "--probe" in args:
        mode = "probe"
    elif "--log" in args:
        mode = "log"
    args = [a for a in args if a not in ("--probe", "--log")]
    if len(args) != 1:
        sys.stderr.write("usage: make_hello_apk.py [--probe|--log] <output.apk>\n")
        return 2
    with zipfile.ZipFile(args[0], "w") as z:
        for name, blob in build_apk(mode=mode).items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
