# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format) + AOSP androidfw binary-XML
# layout (public Apache-2.0 headers). No GPL-3.0 sources.
#
# Emit an APK with a media entry point (Gate G2.e, automotive media path):
#   package com.aclcxx.hello;
#   public class MusicService extends android.service.media.MediaBrowserService {
#     public BrowserRoot onGetRoot(String pkg, int uid, Bundle hints) {
#       return new BrowserRoot("aclcxx_root", null);
#     }
#     public void onLoadChildren(String parentId, Result<List<MediaItem>> result) {
#       ArrayList<MediaItem> items = new ArrayList<>();
#       items.add(new MediaItem(new MediaDescription.Builder()
#           .setTitle("Song One").build(), MediaItem.FLAG_PLAYABLE));
#       items.add(new MediaItem(new MediaDescription.Builder()
#           .setTitle("Song Two").build(), MediaItem.FLAG_PLAYABLE));
#       result.sendResult(items);
#     }
#   }
# `aclcxx run media.apk --service=...` hosts it in-process (no Views, no Binder):
# onCreate(), onGetRoot() -> root, then onLoadChildren() -> the browse items.
# Independent of the C++ decoder.
#
# Usage: make_media_apk.py <output.apk>
import hashlib
import struct
import sys
import zipfile
import zlib

from make_axml import build_axml

MBS = "Landroid/service/media/MediaBrowserService;"
BROWSER_ROOT = "Landroid/service/media/MediaBrowserService$BrowserRoot;"
RESULT = "Landroid/service/media/MediaBrowserService$Result;"
MEDIA_ITEM = "Landroid/media/browse/MediaBrowser$MediaItem;"
MEDIA_DESC = "Landroid/media/MediaDescription;"
BUILDER = "Landroid/media/MediaDescription$Builder;"
ARRAYLIST = "Ljava/util/ArrayList;"
SESSION = "Landroid/media/session/MediaSession;"
TOKEN = "Landroid/media/session/MediaSession$Token;"
METADATA = "Landroid/media/MediaMetadata;"
MMBUILDER = "Landroid/media/MediaMetadata$Builder;"
TITLE_KEY = "android.media.metadata.TITLE"
TITLE = "Now Playing Track"

STRINGS = [
    "Lcom/aclcxx/hello/MusicService;",  # 0
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
    "aclcxx_root",                      # 22
    "Song One",                         # 23
    "Song Two",                         # 24
    "LLIL",                             # 25 onGetRoot shorty
    "VLL",                              # 26 (String,ref)V shorty (BrowserRoot.<init>, onLoadChildren)
    "ZL",                               # 27 add
    "LL",                               # 28 setTitle
    "L",                                # 29 build
    "VLI",                              # 30 MediaItem.<init>
    "VL",                               # 31 sendResult
    SESSION,                            # 32
    TOKEN,                              # 33
    METADATA,                           # 34
    MMBUILDER,                          # 35
    "onCreate",                         # 36
    "setMetadata",                      # 37
    "getSessionToken",                  # 38
    "setSessionToken",                  # 39
    "putString",                        # 40
    "aclcxx",                           # 41 session tag
    TITLE_KEY,                          # 42
    TITLE,                              # 43
    "LLL",                              # 44 putString shorty (String,String)->Builder
    "Lcom/aclcxx/hello/MusicCallback;",             # 45 transport callback class
    "Landroid/media/session/MediaSession$Callback;",  # 46 its superclass
    "Landroid/util/Log;",               # 47
    "onPlay",                           # 48
    "onPause",                          # 49
    "setCallback",                      # 50
    "i",                                # 51
    "aclcxx.app",                       # 52
    "onPlay: playback started",         # 53
    "onPause: playback paused",         # 54
    "ILL",                              # 55 Log.i shorty (String,String)->int
]
S = {s: i for i, s in enumerate(STRINGS)}  # "V" (idx 12) is the ()V / no-arg shorty
TYPES = ["Lcom/aclcxx/hello/MusicService;", MBS, BROWSER_ROOT, RESULT,
         "Landroid/os/Bundle;", "Ljava/lang/String;", "Ljava/lang/Object;",
         "Ljava/lang/CharSequence;", ARRAYLIST, MEDIA_DESC, BUILDER, MEDIA_ITEM,
         "V", "I", "Z", SESSION, TOKEN, METADATA, MMBUILDER,
         "Lcom/aclcxx/hello/MusicCallback;",
         "Landroid/media/session/MediaSession$Callback;", "Landroid/util/Log;"]
T = {t: i for i, t in enumerate(TYPES)}
CALLBACK = "Landroid/media/session/MediaSession$Callback;"
MUSIC_CB = "Lcom/aclcxx/hello/MusicCallback;"
LOG = "Landroid/util/Log;"


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
    n_types, n_protos, n_fields, n_methods, n_classdefs = len(TYPES), 17, 0, 26, 2

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
    tl_objstr = type_list(["Ljava/lang/Object;", "Ljava/lang/String;"])  # MediaSession.<init>
    tl_metadata = type_list([METADATA])          # setMetadata
    tl_token = type_list([TOKEN])                # setSessionToken
    tl_2str = type_list(["Ljava/lang/String;", "Ljava/lang/String;"])  # putString / Log.i
    tl_callback = type_list([CALLBACK])          # setCallback

    # <init>()V : super(); return-void   (v0=this)
    code_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0003, 0x0000,  # invoke-direct {v0}, MediaBrowserService.<init> (m3)
        0x000E,
    ]))
    # onGetRoot(String,int,Bundle)BrowserRoot : return new BrowserRoot("aclcxx_root", null)
    code_getroot = off_data + len(data)
    data.extend(code_item(7, 4, 3, [
        0x0022, T[BROWSER_ROOT],   # new-instance v0, BrowserRoot
        0x011A, S["aclcxx_root"],  # const-string v1, "aclcxx_root"
        0x0212,                    # const/4 v2, 0 (null Bundle)
        0x3070, 0x0004, 0x0210,    # invoke-direct {v0,v1,v2}, BrowserRoot.<init> (m4)
        0x0011,                    # return-object v0
    ]))
    # onLoadChildren(String,Result)V : build two items, sendResult
    #   v0=list v1=builder/flag v2=title v3=desc v4=item ; v5=this v6=parentId v7=result
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

    # onCreate()V : publish a MediaSession with now-playing metadata + token.
    #   v0=session v1=builder/metadata/token v2=key v3=value ; v5=this (registers=6, ins=1)
    code_oncreate = off_data + len(data)
    data.extend(code_item(6, 1, 3, [
        0x0022, T[SESSION],       # new-instance v0, MediaSession
        0x011A, S["aclcxx"],      # const-string v1, "aclcxx"
        0x3070, 0x000D, 0x0150,   # invoke-direct {v0,v5,v1}, MediaSession.<init>(Object,String) (m13)
        0x0122, T[MMBUILDER],     # new-instance v1, MediaMetadata$Builder
        0x1070, 0x0011, 0x0001,   # invoke-direct {v1}, Builder.<init> (m17)
        0x021A, S[TITLE_KEY],     # const-string v2, METADATA_KEY_TITLE
        0x031A, S[TITLE],         # const-string v3, "Now Playing Track"
        0x306E, 0x0012, 0x0321,   # invoke-virtual {v1,v2,v3}, putString (m18)
        0x010C,                   # move-result-object v1
        0x106E, 0x0013, 0x0001,   # invoke-virtual {v1}, build (m19)
        0x010C,                   # move-result-object v1
        0x206E, 0x000E, 0x0010,   # invoke-virtual {v0,v1}, setMetadata (m14)
        0x0122, T[MUSIC_CB],      # new-instance v1, MusicCallback
        0x1070, 0x0015, 0x0001,   # invoke-direct {v1}, MusicCallback.<init> (m21)
        0x206E, 0x0014, 0x0010,   # invoke-virtual {v0,v1}, setCallback (m20)
        0x106E, 0x000F, 0x0000,   # invoke-virtual {v0}, getSessionToken (m15)
        0x010C,                   # move-result-object v1
        0x206E, 0x0010, 0x0015,   # invoke-virtual {v5,v1}, setSessionToken (m16)
        0x000E,                   # return-void
    ]))

    # MusicCallback methods: <init> super Callback.<init>; onPlay/onPause -> Log.i
    code_cb_init = off_data + len(data)
    data.extend(code_item(1, 1, 1, [
        0x1070, 0x0018, 0x0000, 0x000E,  # invoke-direct {v0}, Callback.<init> (m24); return-void
    ]))
    code_cb_play = off_data + len(data)
    data.extend(code_item(3, 1, 2, [
        0x001A, S["aclcxx.app"], 0x011A, S["onPlay: playback started"],
        0x2071, 0x0019, 0x0010, 0x000E,  # invoke-static {v0,v1}, Log.i (m25); return-void
    ]))
    code_cb_pause = off_data + len(data)
    data.extend(code_item(3, 1, 2, [
        0x001A, S["aclcxx.app"], 0x011A, S["onPause: playback paused"],
        0x2071, 0x0019, 0x0010, 0x000E,
    ]))

    # class_data 0: MusicService — 1 direct (<init>), 3 virtual (onGetRoot m1,
    # onLoadChildren m2, onCreate m12)
    class_data_off = off_data + len(data)
    cd = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(3))
    cd += uleb128(0) + uleb128(0x1) + uleb128(code_init)          # m0 <init>, public
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_getroot)       # +1 -> m1 onGetRoot
    cd += uleb128(1) + uleb128(0x1) + uleb128(code_loadchildren)  # +1 -> m2 onLoadChildren
    cd += uleb128(10) + uleb128(0x1) + uleb128(code_oncreate)     # +10 -> m12 onCreate
    data.extend(cd)
    # class_data 1: MusicCallback — 1 direct (<init> m21), 2 virtual (onPlay m22, onPause m23)
    class_data_cb_off = off_data + len(data)
    cd2 = bytearray(uleb128(0) + uleb128(0) + uleb128(1) + uleb128(2))
    cd2 += uleb128(21) + uleb128(0x1) + uleb128(code_cb_init)     # m21 <init>
    cd2 += uleb128(22) + uleb128(0x1) + uleb128(code_cb_play)     # m22 onPlay
    cd2 += uleb128(1) + uleb128(0x1) + uleb128(code_cb_pause)     # +1 -> m23 onPause
    data.extend(cd2)

    data.extend(align4(bytearray()))
    map_off = off_data + len(data)
    items = [
        (0x0000, 1, 0), (0x0001, n_strings, off_string_ids),
        (0x0002, n_types, off_type_ids), (0x0003, n_protos, off_proto_ids),
        (0x0005, n_methods, off_method_ids), (0x0006, n_classdefs, off_class_defs),
        (0x2002, n_strings, string_data_off[0]), (0x1001, 11, tl_getroot),
        (0x2001, 7, code_init), (0x2000, 2, class_data_off),
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
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_object)              # 8 sendResult
    proto_ids += struct.pack("<III", S["VLL"], T["V"], tl_objstr)             # 9 MediaSession.<init>
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_metadata)            # 10 setMetadata
    proto_ids += struct.pack("<III", S["L"], T[TOKEN], 0)                     # 11 getSessionToken
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_token)               # 12 setSessionToken
    proto_ids += struct.pack("<III", S["LLL"], T[MMBUILDER], tl_2str)         # 13 putString
    proto_ids += struct.pack("<III", S["L"], T[METADATA], 0)                  # 14 build (metadata)
    proto_ids += struct.pack("<III", S["VL"], T["V"], tl_callback)            # 15 setCallback
    proto_ids += struct.pack("<III", S["ILL"], T["I"], tl_2str)               # 16 Log.i

    def m(cls, proto, name):
        return struct.pack("<HHI", T[cls], proto, S[name])
    method_ids = b"".join([
        m("Lcom/aclcxx/hello/MusicService;", 0, "<init>"),      # m0
        m("Lcom/aclcxx/hello/MusicService;", 1, "onGetRoot"),   # m1
        m("Lcom/aclcxx/hello/MusicService;", 3, "onLoadChildren"),  # m2
        m(MBS, 0, "<init>"),                                    # m3
        m(BROWSER_ROOT, 2, "<init>"),                           # m4
        m(ARRAYLIST, 0, "<init>"),                              # m5
        m(ARRAYLIST, 4, "add"),                                 # m6
        m(BUILDER, 0, "<init>"),                                # m7
        m(BUILDER, 5, "setTitle"),                              # m8
        m(BUILDER, 6, "build"),                                 # m9
        m(MEDIA_ITEM, 7, "<init>"),                             # m10
        m(RESULT, 8, "sendResult"),                             # m11
        m("Lcom/aclcxx/hello/MusicService;", 0, "onCreate"),    # m12
        m(SESSION, 9, "<init>"),                                # m13
        m(SESSION, 10, "setMetadata"),                          # m14
        m(SESSION, 11, "getSessionToken"),                      # m15
        m(MBS, 12, "setSessionToken"),                          # m16
        m(MMBUILDER, 0, "<init>"),                              # m17
        m(MMBUILDER, 13, "putString"),                          # m18
        m(MMBUILDER, 14, "build"),                              # m19
        m(SESSION, 15, "setCallback"),                          # m20
        m(MUSIC_CB, 0, "<init>"),                               # m21
        m(MUSIC_CB, 0, "onPlay"),                               # m22
        m(MUSIC_CB, 0, "onPause"),                              # m23
        m(CALLBACK, 0, "<init>"),                               # m24 (super)
        m(LOG, 16, "i"),                                        # m25 Log.i
    ])
    class_defs = struct.pack("<IIIIIIII",
                             T["Lcom/aclcxx/hello/MusicService;"], 0x1, T[MBS], 0,
                             0xFFFFFFFF, 0, class_data_off, 0)
    class_defs += struct.pack("<IIIIIIII",
                              T[MUSIC_CB], 0x1, T[CALLBACK], 0,
                              0xFFFFFFFF, 0, class_data_cb_off, 0)

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
        sys.stderr.write("usage: make_media_apk.py <output.apk>\n")
        return 2
    entries = {
        "AndroidManifest.xml": build_axml(
            utf8=True, package="com.aclcxx.hello",
            launcher="com.aclcxx.hello.MusicService"),
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
