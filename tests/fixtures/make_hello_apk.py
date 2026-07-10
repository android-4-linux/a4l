# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout. No GPL-3.0 sources.
#
# Emit a tiny, self-contained "hello world" APK for the end-to-end run-path test:
# a single class com.aclcxx.hello.Main whose
#   public static void main(String[]) { System.out.println("<MSG>"); }
# is translatable by the DEX->JVM converter and runnable on the JVM backend, plus
# a binary AndroidManifest.xml whose launcher activity resolves to that class (so
# `aclcxx run hello.apk` needs no --main). Independent of the C++ decoder.
#
# Usage: make_hello_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile

from make_axml import build_axml

MSG = "aclcxx: hello from a real APK"


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


# Interned strings (order arbitrary; the parser is order-independent).
STRINGS = [
    "Lcom/aclcxx/hello/Main;",  # 0  this class
    "Ljava/lang/Object;",       # 1  superclass
    "Ljava/lang/String;",       # 2  String
    "[Ljava/lang/String;",      # 3  String[]
    "Ljava/lang/System;",       # 4  System
    "Ljava/io/PrintStream;",    # 5  PrintStream
    "V",                        # 6  void
    "VL",                       # 7  shorty (Object)->void
    "main",                     # 8  method name
    "out",                      # 9  field name
    "println",                  # 10 method name
    MSG,                        # 11 printed message
]
S = {s: i for i, s in enumerate(STRINGS)}

# type_ids -> string index (kept ascending, as the format requires).
TYPES = ["Lcom/aclcxx/hello/Main;", "Ljava/lang/Object;", "Ljava/lang/String;",
         "[Ljava/lang/String;", "Ljava/lang/System;", "Ljava/io/PrintStream;", "V"]
T = {t: i for i, t in enumerate(TYPES)}


def build_dex():
    header_size = 112
    n_strings, n_types, n_protos, n_fields, n_methods, n_classdefs = \
        len(STRINGS), len(TYPES), 2, 1, 2, 1

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_field_ids = off_proto_ids + n_protos * 12
    off_method_ids = off_field_ids + n_fields * 8
    off_class_defs = off_method_ids + n_methods * 8
    off_data = off_class_defs + n_classdefs * 32

    data = bytearray()

    # string_data_items: uleb128(utf16 count) + MUTF-8 bytes + 0x00
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(off_data + len(data))
        data.extend(uleb128(len(s)) + b + b"\x00")

    # type_list for main's parameters (String[]) and println's (String).
    data.extend(align4(bytearray()))
    type_list_argv_off = off_data + len(data)
    data.extend(align4(bytearray(
        struct.pack("<I", 1) + struct.pack("<H", T["[Ljava/lang/String;"]))))
    data.extend(align4(bytearray()))
    type_list_str_off = off_data + len(data)
    data.extend(align4(bytearray(
        struct.pack("<I", 1) + struct.pack("<H", T["Ljava/lang/String;"]))))

    # code for main: System.out.println(MSG)
    #   sget-object v0, System.out ; const-string v1, MSG
    #   invoke-virtual {v0,v1}, PrintStream.println(String)V ; return-void
    code_main_off = off_data + len(data)
    insns = [0x0062, 0x0000,        # sget-object v0, field0 (System.out)
             0x011A, S[MSG],        # const-string v1, MSG
             0x206E, 0x0001, 0x0010,  # invoke-virtual {v0,v1}, method1 (println)
             0x000E]                # return-void
    body = struct.pack("<HHHHII", 2, 1, 2, 0, 0, len(insns))  # regs,ins,outs,tries,dbg,size
    for u in insns:
        body += struct.pack("<H", u)
    data.extend(align4(bytearray(body)))

    # class_data_item: 0 static, 0 instance, 1 direct (main), 0 virtual
    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(0))
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main_off)  # method0 main, public static
    data.extend(cd)

    # map_list (parser ignores it, but emit a valid one).
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

    # fixed tables
    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = struct.pack("<III", S["VL"], T["V"], type_list_argv_off)   # main (String[])V
    proto_ids += struct.pack("<III", S["VL"], T["V"], type_list_str_off)   # println (String)V
    field_ids = struct.pack("<HHI", T["Ljava/lang/System;"],
                            T["Ljava/io/PrintStream;"], S["out"])          # f0 System.out
    method_ids = struct.pack("<HHI", T["Lcom/aclcxx/hello/Main;"], 0, S["main"])   # m0 main
    method_ids += struct.pack("<HHI", T["Ljava/io/PrintStream;"], 1, S["println"])  # m1 println
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/aclcxx/hello/Main;"], 0x1,
                             T["Ljava/lang/Object;"], 0, 0xFFFFFFFF, 0,
                             class_data_off, 0)

    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)
    struct.pack_into("<I", header, 0x34, map_off)
    struct.pack_into("<I", header, 0x38, n_strings)
    struct.pack_into("<I", header, 0x3C, off_string_ids)
    struct.pack_into("<I", header, 0x40, n_types)
    struct.pack_into("<I", header, 0x44, off_type_ids)
    struct.pack_into("<I", header, 0x48, n_protos)
    struct.pack_into("<I", header, 0x4C, off_proto_ids)
    struct.pack_into("<I", header, 0x50, n_fields)
    struct.pack_into("<I", header, 0x54, off_field_ids)
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        field_ids + method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib_adler(bytes(dex[12:])))
    return bytes(dex)


def zlib_adler(b):
    import zlib
    return zlib.adler32(b) & 0xFFFFFFFF


def build_apk():
    return {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.Main"),
        "classes.dex": build_dex(),
    }


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_hello_apk.py <output.apk>\n")
        return 2
    with zipfile.ZipFile(sys.argv[1], "w") as z:
        for name, blob in build_apk().items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
