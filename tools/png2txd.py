#!/usr/bin/env python3
"""png2txd.py - pack PNG image(s) into a RenderWare 3.6 D3D8 texture dictionary
(.txd) that librw/re3 can load.

Writes one uncompressed 32-bit texture per PNG, format C8888 (D3DFMT_A8R8G8B8,
i.e. BGRA byte order). This is enough for the Chinese font atlas; the game loads
it via CTxdStore::LoadTxd exactly like the stock fonts.txd.

Chunk layout produced (matches vendor/librw d3d8 reader/writer):
  TEXDICTIONARY
    STRUCT { int16 numTex; int16 deviceId=1(d3d8) }
    for each texture:
      TEXTURENATIVE
        STRUCT { u32 platform=8; u32 filterAddressing;
                 char name[32]; char mask[32];
                 u32 format; i32 hasAlpha; u16 w; u16 h;
                 u8 depth; u8 numLevels; u8 type; u8 compression;
                 [u32 size; pixels]* per level }
        EXTENSION { } (empty)
    EXTENSION { } (empty)

Usage: png2txd.py out.txd name1=img1.png [name2=img2.png ...]
       png2txd.py out.txd img.png            (texture name = file stem)

Requires Pillow.
"""
import struct
import sys

try:
    from PIL import Image
except ImportError:
    sys.exit("Pillow required: pip install pillow")

RW_VERSION = 0x0C02FFFF  # RW 3.6, as used by GTA3 PC (matches stock fonts.txd)

ID_STRUCT = 0x0001
ID_EXTENSION = 0x0003
ID_TEXTURENATIVE = 0x0015
ID_TEXDICTIONARY = 0x0016

PLATFORM_D3D8 = 8
FMT_C8888 = 0x0500
TYPE_TEXTURE = 0x04
FILTER_LINEAR = 2  # Texture::LINEAR
ADDR_WRAP = 1      # Texture::WRAP


def chunk(cid, body):
    return struct.pack("<III", cid, len(body), RW_VERSION) + body


def texture_native(name, img):
    """Build a TEXTURENATIVE chunk for a PIL image as C8888 (BGRA)."""
    img = img.convert("RGBA")
    w, h = img.size
    # BGRA byte order (D3DFMT_A8R8G8B8 little-endian).
    px = img.tobytes()  # RGBA
    bgra = bytearray(len(px))
    bgra[0::4] = px[2::4]  # B
    bgra[1::4] = px[1::4]  # G
    bgra[2::4] = px[0::4]  # R
    bgra[3::4] = px[3::4]  # A
    data = bytes(bgra)

    name_b = name.encode("ascii")[:31].ljust(32, b"\x00")
    mask_b = b"\x00" * 32
    filter_addr = FILTER_LINEAR | (ADDR_WRAP << 8) | (ADDR_WRAP << 12)

    struct_body = struct.pack("<I", PLATFORM_D3D8)
    struct_body += struct.pack("<I", filter_addr)
    struct_body += name_b + mask_b
    struct_body += struct.pack("<I", FMT_C8888)
    struct_body += struct.pack("<i", 1)          # hasAlpha
    struct_body += struct.pack("<HH", w, h)
    struct_body += struct.pack("<BBBB", 32, 1, TYPE_TEXTURE, 0)  # depth,levels,type,compression
    struct_body += struct.pack("<I", len(data)) + data           # level 0

    body = chunk(ID_STRUCT, struct_body)
    body += chunk(ID_EXTENSION, b"")  # empty texture extension
    return chunk(ID_TEXTURENATIVE, body)


def main():
    if len(sys.argv) < 3:
        sys.exit("usage: png2txd.py out.txd [name=]img.png ...")
    out = sys.argv[1]
    specs = sys.argv[2:]

    textures = []
    for s in specs:
        if "=" in s:
            name, path = s.split("=", 1)
        else:
            path = s
            name = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        textures.append((name, Image.open(path)))

    struct_body = struct.pack("<hh", len(textures), 1)  # numTex, deviceId=d3d8
    body = chunk(ID_STRUCT, struct_body)
    for name, img in textures:
        body += texture_native(name, img)
        print("  + texture '%s' %dx%d" % (name, img.width, img.height))
    body += chunk(ID_EXTENSION, b"")  # empty dictionary extension
    txd = chunk(ID_TEXDICTIONARY, body)

    with open(out, "wb") as f:
        f.write(txd)
    print("wrote %s (%d bytes, %d textures)" % (out, len(txd), len(textures)))


if __name__ == "__main__":
    main()
