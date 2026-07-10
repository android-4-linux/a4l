# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK with a real Android entry point (Gate G2.d):
#   package com.aclcxx.hello;
#   public class MainActivity extends android.app.Activity {
#     protected void onCreate(android.os.Bundle b) {
#       super.onCreate(b);
#       android.util.Log.i("aclcxx.app", "MainActivity.onCreate ran");
#     }
#   }
# The manifest launcher resolves to it, so `aclcxx run` drives the Activity
# lifecycle (onCreate -> onStart -> onResume) against the vendored/shimmed
# android.app.Activity — instead of a static main(). Independent of the C++ decoder.
#
# Usage: make_activity_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

STRINGS = [
    "Lcom/aclcxx/hello/MainActivity;",  # 0
    "Landroid/app/Activity;",           # 1  superclass (framework)
    "Landroid/os/Bundle;",              # 2  onCreate param (framework)
    "Landroid/util/Log;",               # 3  framework class w/ aclcxx native
    "Ljava/lang/String;",               # 4
    "V",                                # 5
    "I",                                # 6
    "VL",                               # 7  shorty (Bundle)->void
    "ILL",                              # 8  shorty (String,String)->int
    "<init>",                           # 9
    "onCreate",                         # 10
    "i",                                # 11
    "aclcxx.app",                       # 12
    "MainActivity.onCreate ran",        # 13
    "Landroid/media/AudioManager;",     # 14
    "Ljava/lang/Object;",               # 15 getSystemService return
    "Landroid/content/Context;",        # 16 getSystemService owner
    "audio",                            # 17 Context.AUDIO_SERVICE
    "getSystemService",                 # 18
    "setStreamVolume",                  # 19
    "LL",                               # 20 getSystemService shorty (String)->Object
    "VIII",                             # 21 setStreamVolume shorty
]
S = {s: i for i, s in enumerate(STRINGS)}
AUDIO_MGR = "Landroid/media/AudioManager;"
CONTEXT = "Landroid/content/Context;"
TYPES = ["Lcom/aclcxx/hello/MainActivity;", "Landroid/app/Activity;",
         "Landroid/os/Bundle;", "Landroid/util/Log;", "Ljava/lang/String;", "V", "I",
         AUDIO_MGR, "Ljava/lang/Object;", CONTEXT]
T = {t: i for i, t in enumerate(TYPES)}


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


def code_item(regs, ins, outs, insns):
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns))
    for u in insns:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings, n_types, n_protos, n_fields, n_methods, n_classdefs = \
        len(STRINGS), len(TYPES), 5, 0, 7, 1

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

    # type_list for onCreate params (Bundle) and Log.i params (String,String).
    data.extend(align4(bytearray()))
    tl_bundle_off = off_data + len(data)
    data.extend(align4(bytearray(struct.pack("<I", 1) + struct.pack("<H", T["Landroid/os/Bundle;"]))))
    data.extend(align4(bytearray()))
    tl_2str_off = off_data + len(data)
    data.extend(align4(bytearray(struct.pack("<I", 2) + struct.pack("<H", T["Ljava/lang/String;"]) * 2)))
    data.extend(align4(bytearray()))
    tl_string_off = off_data + len(data)   # getSystemService(String)
    data.extend(align4(bytearray(struct.pack("<I", 1) + struct.pack("<H", T["Ljava/lang/String;"]))))
    data.extend(align4(bytearray()))
    tl_3int_off = off_data + len(data)     # setStreamVolume(int,int,int)
    data.extend(align4(bytearray(struct.pack("<I", 3) + struct.pack("<H", T["I"]) * 3)))

    # <init>()V : super(); return-void      (v0 = this)
    code_init_off = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0002, 0x0000,   # invoke-direct {v0}, Activity.<init>() (m2)
        0x000E,                   # return-void
    ]))
    # onCreate(Bundle)V : super.onCreate(b); Log.i(TAG, MSG); then use the svc hub:
    #   ((AudioManager) getSystemService("audio")).setStreamVolume(STREAM_MUSIC, 7, 0)
    #   this=v4, bundle=v5 (registers=6, ins=2); temps v0..v3
    code_oncreate_off = off_data + len(data)
    data.extend(code_item(6, 2, 4, [
        0x206F, 0x0003, 0x0054,   # invoke-super {v4,v5}, Activity.onCreate(Bundle) (m3)
        0x001A, S["aclcxx.app"],  # const-string v0, TAG
        0x011A, S["MainActivity.onCreate ran"],  # const-string v1, MSG
        0x2071, 0x0004, 0x0010,   # invoke-static {v0,v1}, Log.i (m4)
        0x001A, S["audio"],       # const-string v0, "audio"
        0x206E, 0x0005, 0x0004,   # invoke-virtual {v4,v0}, getSystemService (m5)
        0x000C,                   # move-result-object v0
        0x001F, T[AUDIO_MGR],     # check-cast v0, AudioManager
        0x3112,                   # const/4 v1, 3  (STREAM_MUSIC)
        0x7212,                   # const/4 v2, 7  (index)
        0x0312,                   # const/4 v3, 0  (flags)
        0x406E, 0x0006, 0x3210,   # invoke-virtual {v0,v1,v2,v3}, setStreamVolume (m6)
        0x000E,                   # return-void
    ]))

    # class_data: 0 static, 0 instance, 1 direct (<init>), 1 virtual (onCreate)
    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(1))
    cd += uleb128(0) + uleb128(0x1) + uleb128(code_init_off)      # m0 <init>, public
    cd += uleb128(1) + uleb128(0x4) + uleb128(code_oncreate_off)  # +1 -> m1 onCreate, protected
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0),
        (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids),
        (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids),
        (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]),
        (0x1001, 4, tl_bundle_off),
        (0x2001, 2, code_init_off),
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
    proto_ids = struct.pack("<III", S["V"], T["V"], 0)                    # 0 ()V
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_bundle_off)      # 1 (Bundle)V
    proto_ids += struct.pack("<III", S["ILL"], T["I"], tl_2str_off)       # 2 (String,String)I
    proto_ids += struct.pack("<III", S["LL"], T["Ljava/lang/Object;"], tl_string_off)  # 3 getSystemService
    proto_ids += struct.pack("<III", S["VIII"], T["V"], tl_3int_off)      # 4 setStreamVolume
    method_ids = struct.pack("<HHI", T["Lcom/aclcxx/hello/MainActivity;"], 0, S["<init>"])   # m0
    method_ids += struct.pack("<HHI", T["Lcom/aclcxx/hello/MainActivity;"], 1, S["onCreate"])  # m1
    method_ids += struct.pack("<HHI", T["Landroid/app/Activity;"], 0, S["<init>"])   # m2 super.<init>
    method_ids += struct.pack("<HHI", T["Landroid/app/Activity;"], 1, S["onCreate"])  # m3 super.onCreate
    method_ids += struct.pack("<HHI", T["Landroid/util/Log;"], 2, S["i"])            # m4 Log.i
    method_ids += struct.pack("<HHI", T[CONTEXT], 3, S["getSystemService"])          # m5
    method_ids += struct.pack("<HHI", T[AUDIO_MGR], 4, S["setStreamVolume"])         # m6
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/aclcxx/hello/MainActivity;"], 0x1,
                             T["Landroid/app/Activity;"], 0, 0xFFFFFFFF, 0,
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
    struct.pack_into("<I", header, 0x50, n_fields)       # 0
    struct.pack_into("<I", header, 0x54, 0)              # field_ids_off (0 when none)
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))
    dex[12:32] = hashlib.sha1(bytes(dex[32:])).digest()
    struct.pack_into("<I", dex, 0x08, zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_activity_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.MainActivity"),
        "classes.dex": build_dex(),
    }
    with zipfile.ZipFile(sys.argv[1], "w") as z:
        for name, blob in entries.items():
            info = zipfile.ZipInfo(name, date_time=(2026, 1, 1, 0, 0, 0))
            info.compress_type = zipfile.ZIP_STORED
            z.writestr(info, blob)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
