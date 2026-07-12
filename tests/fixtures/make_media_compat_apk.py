# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK whose media entry point extends the ANDROIDX support-library base
# (the class most real Auto media apps, e.g. Spotify, actually extend):
#   package com.acl.hello;
#   public class MusicService extends androidx.media.MediaBrowserServiceCompat {
#     public BrowserRoot onGetRoot(String pkg, int uid, Bundle hints) {
#       return new BrowserRoot("acl_root", null);
#     }
#     public void onLoadChildren(String parentId, Result<List<MediaItem>> result) {
#       ArrayList<MediaItem> items = new ArrayList<>();
#       items.add(new MediaItem(new MediaDescriptionCompat.Builder()
#           .setTitle("Song One").build(), MediaItem.FLAG_PLAYABLE));
#       items.add(new MediaItem(new MediaDescriptionCompat.Builder()
#           .setTitle("Song Two").build(), MediaItem.FLAG_PLAYABLE));
#       result.sendResult(items);
#     }
#   }
# `acl run media_compat.apk --service=...` recognizes the androidx base and walks
# onGetRoot -> onLoadChildren in-process (no Views, no Binder). Independent of the
# C++ decoder.
#
# Usage: make_media_compat_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MBS = "Landroidx/media/MediaBrowserServiceCompat;"
BROWSER_ROOT = "Landroidx/media/MediaBrowserServiceCompat$BrowserRoot;"
RESULT = "Landroidx/media/MediaBrowserServiceCompat$Result;"
MEDIA_ITEM = "Landroidx/media/MediaBrowserCompat$MediaItem;"
MEDIA_DESC = "Landroidx/media/MediaDescriptionCompat;"
BUILDER = "Landroidx/media/MediaDescriptionCompat$Builder;"
ARRAYLIST = "Ljava/util/ArrayList;"

STRINGS = [
    "Lcom/acl/hello/MusicService;",  # 0
    MBS,                                # 1
    BROWSER_ROOT,                       # 2
    RESULT,                             # 3
    "Landroid/os/Bundle;",              # 4
    "Ljava/lang/String;",              # 5
    "Ljava/lang/Object;",              # 6
    "Ljava/lang/CharSequence;",        # 7
    ARRAYLIST,                          # 8
    MEDIA_DESC,                         # 9
    BUILDER,                            # 10
    MEDIA_ITEM,                         # 11
    "V",                                # 12
    "I",                                # 13
    "Z",                                # 14
    "<init>",                           # 15
    "onGetRoot",                        # 16
    "onLoadChildren",                   # 17
    "add",                              # 18
    "setTitle",                         # 19
    "build",                            # 20
    "sendResult",                       # 21
    "acl_root",                         # 22
    "Song One",                         # 23
    "Song Two",                         # 24
    "LLIL",                             # 25 onGetRoot shorty
    "VLL",                              # 26 (String,ref)V shorty
    "ZL",                               # 27 add
    "LL",                               # 28 setTitle
    "L",                                # 29 build
    "VLI",                              # 30 MediaItem.<init>
    "VL",                               # 31 sendResult
]
S = {s: i for i, s in enumerate(STRINGS)}
TYPES = ["Lcom/acl/hello/MusicService;", MBS, BROWSER_ROOT, RESULT,
         "Landroid/os/Bundle;", "Ljava/lang/String;", "Ljava/lang/Object;",
         "Ljava/lang/CharSequence;", ARRAYLIST, MEDIA_DESC, BUILDER, MEDIA_ITEM,
         "V", "I", "Z"]
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
    n_strings = len(STRINGS)
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 9, 0, 12, 1

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

    def type_list(types):
        nonlocal data
        data.extend(align4(bytearray()))
        off = off_data + len(data)
        data.extend(align4(bytearray(
            struct.pack("<I", len(types)) + b"".join(struct.pack("<H", T[t]) for t in types))))
        return off

    tl_getroot = type_list(["Ljava/lang/String;", "I", "Landroid/os/Bundle;"])
    tl_rootctor = type_list(["Ljava/lang/String;", "Landroid/os/Bundle;"])
    tl_loadchildren = type_list(["Ljava/lang/String;", RESULT])
    tl_object = type_list(["Ljava/lang/Object;"])
    tl_charseq = type_list(["Ljava/lang/CharSequence;"])
    tl_mediaitem = type_list([MEDIA_DESC, "I"])

    # <init>()V : super(); return-void   (v0=this)
    code_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0003, 0x0000,  # invoke-direct {v0}, MediaBrowserServiceCompat.<init> (m3)
        0x000E,
    ]))
    # onGetRoot(String,int,Bundle)BrowserRoot : return new BrowserRoot("acl_root", null)
    code_getroot = off_data + len(data)
    data.extend(code_item(7, 4, 3, [
        0x0022, T[BROWSER_ROOT],   # new-instance v0, BrowserRoot
        0x011A, S["acl_root"],     # const-string v1, "acl_root"
        0x0212,                    # const/4 v2, 0 (null Bundle)
        0x3070, 0x0004, 0x0210,    # invoke-direct {v0,v1,v2}, BrowserRoot.<init> (m4)
        0x0011,                    # return-object v0
    ]))

    # onLoadChildren(String,Result)V : build two items, sendResult
    def item(title):
        return [
            0x0122, T[BUILDER],        # new-instance v1, Builder
            0x1070, 0x0007, 0x0001,    # invoke-direct {v1}, Builder.<init> (m7)
            0x021A, S[title],          # const-string v2, title
            0x206E, 0x0008, 0x0021,    # invoke-virtual {v1,v2}, setTitle (m8)
            0x010C,                    # move-result-object v1
            0x106E, 0x0009, 0x0001,    # invoke-virtual {v1}, build (m9)
            0x030C,                    # move-result-object v3
            0x0422, T[MEDIA_ITEM],     # new-instance v4, MediaItem
            0x2112,                    # const/4 v1, 2 (FLAG_PLAYABLE)
            0x3070, 0x000A, 0x0134,    # invoke-direct {v4,v3,v1}, MediaItem.<init> (m10)
            0x206E, 0x0006, 0x0040,    # invoke-virtual {v0,v4}, ArrayList.add (m6)
        ]
    code_loadchildren = off_data + len(data)
    insns = [0x0022, T[ARRAYLIST], 0x1070, 0x0005, 0x0000]  # new ArrayList; <init> (m5)
    insns += item("Song One") + item("Song Two")
    insns += [0x206E, 0x000B, 0x0007, 0x000E]  # sendResult (m11) {v7,v0} ; return-void
    data.extend(code_item(8, 3, 3, insns))

    # class_data: 1 direct (<init> m0), 2 virtual (onGetRoot m1, onLoadChildren m2)
    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(2))
    cd += uleb128(0) + uleb128(0x1) + uleb128(code_init)          # m0 <init>
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_getroot)       # +1 -> m1 onGetRoot
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_loadchildren)  # +1 -> m2 onLoadChildren
    data.extend(cd)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 6, tl_getroot),
        (0x2001, 3, code_init), (0x2000, 1, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)
    data.extend(align4(map_body))
    data_size = len(data)

    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)
    proto_ids = b""
    proto_ids += struct.pack("<III", S["V"], T["V"], 0)                       # 0 ()V
    proto_ids += struct.pack("<III", S["LLIL"], T[BROWSER_ROOT], tl_getroot)  # 1 onGetRoot
    proto_ids += struct.pack("<III", S["VLL"], T["V"], tl_rootctor)           # 2 BrowserRoot.<init>
    proto_ids += struct.pack("<III", S["VLL"], T["V"], tl_loadchildren)       # 3 onLoadChildren
    proto_ids += struct.pack("<III", S["ZL"], T["Z"], tl_object)              # 4 add
    proto_ids += struct.pack("<III", S["LL"], T[BUILDER], tl_charseq)         # 5 setTitle
    proto_ids += struct.pack("<III", S["L"], T[MEDIA_DESC], 0)                # 6 build
    proto_ids += struct.pack("<III", S["VLI"], T["V"], tl_mediaitem)          # 7 MediaItem.<init>
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_object)              # 8 sendResult (Object)V

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m("Lcom/acl/hello/MusicService;", 0, "<init>"),      # m0
        m("Lcom/acl/hello/MusicService;", 1, "onGetRoot"),   # m1
        m("Lcom/acl/hello/MusicService;", 3, "onLoadChildren"),  # m2
        m(MBS, 0, "<init>"),                                    # m3
        m(BROWSER_ROOT, 2, "<init>"),                           # m4
        m(ARRAYLIST, 0, "<init>"),                              # m5
        m(ARRAYLIST, 4, "add"),                                 # m6
        m(BUILDER, 0, "<init>"),                                # m7
        m(BUILDER, 5, "setTitle"),                              # m8
        m(BUILDER, 6, "build"),                                 # m9
        m(MEDIA_ITEM, 7, "<init>"),                             # m10
        m(RESULT, 8, "sendResult"),                             # m11 sendResult(Object)V
    ])

    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/acl/hello/MusicService;"], 0x1, T[MBS], 0,
                             0xFFFFFFFF, 0, class_data_off, 0)

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
    struct.pack_into("<I", header, 0x54, 0)
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
        sys.stderr.write("usage: make_media_compat_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.acl.hello",
            launcher="com.acl.hello.MusicService"),
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
