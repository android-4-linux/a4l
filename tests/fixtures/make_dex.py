# SPDX-License-Identifier: Apache-2.0
# Provenance: original test fixture generator; public Dalvik .dex format spec
# (source.android.com/docs/core/runtime/dex-format). No GPL-3.0 sources.
#
# Encode a minimal but structurally-valid classes.dex holding one class
#   public class HelloDex {
#     public static void main(String[] args) { return; }
#     public static int answer() { return 42; }
#   }
# with a correct Adler-32 checksum and SHA-1 signature, for the dex/ parser
# tests. Independent of the C++ parser (both follow the documented format).
#
# Usage: make_dex.py <output.dex>
import hashlib
import struct
import sys
import zlib

NO_INDEX = 0xFFFFFFFF


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


# Interned strings (order is arbitrary; the parser is order-independent).
STRINGS = [
    "LHelloDex;",            # 0  type: this class
    "Ljava/lang/Object;",    # 1  type: superclass
    "V",                     # 2  type/shorty: void
    "I",                     # 3  type/shorty: int
    "[Ljava/lang/String;",   # 4  type: String[]
    "VL",                    # 5  shorty: (Object-ish) -> void
    "main",                  # 6  method name
    "answer",                # 7  method name
    "HelloDex.java",         # 8  source file
    "misc",                  # 9  method name (untranslatable -> stubbed)
]
S = {s: i for i, s in enumerate(STRINGS)}

# type_ids -> string index for each descriptor.
TYPES = ["LHelloDex;", "Ljava/lang/Object;", "V", "I", "[Ljava/lang/String;"]
T = {t: i for i, t in enumerate(TYPES)}


def code_item(regs, ins, outs, insns_u16):
    # registers_size, ins_size, outs_size, tries_size, debug_off, insns_size,
    # insns[], then padding to 4.
    body = struct.pack("<HHHHII", regs, ins, outs, 0, 0, len(insns_u16))
    for u in insns_u16:
        body += struct.pack("<H", u)
    return align4(bytearray(body))


def build_dex():
    header_size = 112
    n_strings = len(STRINGS)
    n_types = len(TYPES)
    n_protos = 2
    n_methods = 3
    n_classdefs = 1

    off_string_ids = header_size
    off_type_ids = off_string_ids + n_strings * 4
    off_proto_ids = off_type_ids + n_types * 4
    off_method_ids = off_proto_ids + n_protos * 12
    off_class_defs = off_method_ids + n_methods * 8
    off_data = off_class_defs + n_classdefs * 32

    # --- data section -------------------------------------------------------
    data = bytearray()

    def emit(chunk):
        off = off_data + len(data)
        data.extend(chunk)
        return off

    # string_data_items: uleb128(utf16 count) + MUTF-8 bytes + 0x00
    string_data_off = []
    for s in STRINGS:
        b = s.encode("utf-8")
        string_data_off.append(emit(uleb128(len(s)) + b + b"\x00"))

    # type_list for main's parameters: (String[])
    data.extend(align4(bytearray()))  # align before a 4-aligned item
    type_list_main_off = off_data + len(data)
    tl = struct.pack("<I", 1) + struct.pack("<H", T["[Ljava/lang/String;"])
    data.extend(align4(bytearray(tl)))

    # code items
    code_main_off = off_data + len(data)
    # return-void (op 0x0e, format 10x) -> code unit 0x000e
    data.extend(code_item(1, 1, 0, [0x000E]))
    code_answer_off = off_data + len(data)
    # const/16 v0, #42 (op 0x13) -> 0x0013, 0x002a ; return v0 (op 0x0f) -> 0x000f
    data.extend(code_item(1, 0, 0, [0x0013, 0x002A, 0x000F]))
    code_misc_off = off_data + len(data)
    # const-string v0, "misc" (op 0x1a, unsupported by the translator -> stubbed);
    # const/4 v0, 0 (0x0012); return v0 (0x000f). Never executed (stub throws).
    data.extend(code_item(1, 0, 0, [0x001A, S["misc"], 0x0012, 0x000F]))

    # class_data_item
    class_data_off = off_data + len(data)
    cd = bytearray()
    cd += uleb128(0)  # static_fields_size
    cd += uleb128(0)  # instance_fields_size
    cd += uleb128(3)  # direct_methods_size
    cd += uleb128(0)  # virtual_methods_size
    # direct methods, sorted by ascending method index; idx diffs are cumulative.
    # m0=main (id 0), m1=answer (id 1), m2=misc (id 2).
    cd += uleb128(0) + uleb128(0x9) + uleb128(code_main_off)     # diff 0 -> idx 0
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_answer_off)   # diff 1 -> idx 1
    cd += uleb128(1) + uleb128(0x9) + uleb128(code_misc_off)     # diff 1 -> idx 2
    data.extend(cd)

    # map_list (4-aligned). Parser ignores it, but emit a valid one.
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
        (0x1001, 1, type_list_main_off),
        (0x2001, 3, code_main_off),
        (0x2000, 1, class_data_off),
    ]
    map_body = bytearray(struct.pack("<I", len(items) + 1))
    for mtype, size, off in items:
        map_body += struct.pack("<HHII", mtype, 0, size, off)
    map_body += struct.pack("<HHII", 0x1000, 0, 1, map_off)  # the map itself
    data.extend(align4(map_body))

    data_size = len(data)

    # --- fixed tables -------------------------------------------------------
    string_ids = b"".join(struct.pack("<I", o) for o in string_data_off)
    type_ids = b"".join(struct.pack("<I", S[t]) for t in TYPES)

    proto_ids = b""
    # proto 0: main -> shorty "VL", return V, params (String[])
    proto_ids += struct.pack("<III", S["VL"], T["V"], type_list_main_off)
    # proto 1: answer -> shorty "I", return I, no params
    proto_ids += struct.pack("<III", S["I"], T["I"], 0)

    method_ids = b""
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 0, S["main"])
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["answer"])
    method_ids += struct.pack("<HHI", T["LHelloDex;"], 1, S["misc"])  # proto ()I

    class_defs = struct.pack(
        "<IIIIIIII",
        T["LHelloDex;"],       # class_idx
        0x1,                    # access_flags = public
        T["Ljava/lang/Object;"],  # superclass_idx
        0,                      # interfaces_off
        S["HelloDex.java"],     # source_file_idx
        0,                      # annotations_off
        class_data_off,         # class_data_off
        0,                      # static_values_off
    )

    # --- header -------------------------------------------------------------
    header = bytearray(header_size)
    header[0:8] = b"dex\n035\x00"
    struct.pack_into("<I", header, 0x24, header_size)
    struct.pack_into("<I", header, 0x28, 0x12345678)  # endian tag
    struct.pack_into("<I", header, 0x34, map_off)
    struct.pack_into("<I", header, 0x38, n_strings)
    struct.pack_into("<I", header, 0x3C, off_string_ids)
    struct.pack_into("<I", header, 0x40, n_types)
    struct.pack_into("<I", header, 0x44, off_type_ids)
    struct.pack_into("<I", header, 0x48, n_protos)
    struct.pack_into("<I", header, 0x4C, off_proto_ids)
    struct.pack_into("<I", header, 0x50, 0)   # field_ids_size
    struct.pack_into("<I", header, 0x54, 0)   # field_ids_off
    struct.pack_into("<I", header, 0x58, n_methods)
    struct.pack_into("<I", header, 0x5C, off_method_ids)
    struct.pack_into("<I", header, 0x60, n_classdefs)
    struct.pack_into("<I", header, 0x64, off_class_defs)
    struct.pack_into("<I", header, 0x68, data_size)
    struct.pack_into("<I", header, 0x6C, off_data)

    dex = bytearray(header) + string_ids + type_ids + proto_ids + \
        method_ids + class_defs + bytes(data)
    struct.pack_into("<I", dex, 0x20, len(dex))  # file_size

    # signature = SHA-1 over everything after the signature field (offset 32).
    sig = hashlib.sha1(bytes(dex[32:])).digest()
    dex[12:32] = sig
    # checksum = Adler-32 over everything after the checksum field (offset 12).
    checksum = zlib.adler32(bytes(dex[12:])) & 0xFFFFFFFF
    struct.pack_into("<I", dex, 0x08, checksum)
    return bytes(dex)


def main():
    if len(sys.argv) != 2:
        sys.stderr.write("usage: make_dex.py <output.dex>\n")
        return 2
    with open(sys.argv[1], "wb") as f:
        f.write(build_dex())
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
