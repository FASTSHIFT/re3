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
FMT_PAL8 = 0x2000 | FMT_C8888  # 0x2500: 8-bit palettized, 32-bit palette entries
TYPE_TEXTURE = 0x04
FILTER_LINEAR = 2  # Texture::LINEAR
ADDR_WRAP = 1      # Texture::WRAP


def chunk(cid, body):
    return struct.pack("<III", cid, len(body), RW_VERSION) + body


def read_chunk_header(d, o):
    cid, sz, ver = struct.unpack("<III", d[o:o + 12])
    return cid, sz, ver, o + 12


def split_base_textures(path):
    """Return a list of (name, raw_texturenative_chunk_bytes) from an existing
    TXD, so we can carry stock textures (font1/font2) through verbatim."""
    d = open(path, "rb").read()
    _c, _s, _v, o = read_chunk_header(d, 0)          # TEXDICTIONARY
    _c, s, _v, o2 = read_chunk_header(d, o)          # dict STRUCT
    numtex = struct.unpack("<h", d[o2:o2 + 2])[0]
    o = o2 + s
    out = []
    for _ in range(numtex):
        start = o
        c, s, _v, body = read_chunk_header(d, o)     # TEXTURENATIVE
        assert c == ID_TEXTURENATIVE
        end = body + s
        # name is inside STRUCT: platform(4)+filter(4)+name(32)
        _c2, _s2, _v2, st = read_chunk_header(d, body)
        name = d[st + 8:st + 8 + 32].split(b"\x00")[0].decode("ascii")
        out.append((name, d[start:end]))
        o = end
    return out


def texture_native(name, img, mask=""):
    """Build a TEXTURENATIVE chunk as an 8-bit PALETTIZED D3D8 texture
    (fmt 0x2500), matching the stock GTA3 font textures.

    Why PAL8 and not direct C8888: on a GL3 build librw's d3d8 reader only
    converts PAL4/PAL8 textures into a current-platform (GL3) raster via
    readAsImage(); a direct C8888 D3D8 texture stays an unusable D3D8 raster and
    renders as solid blocks. The stock fonts are PAL8, so we match them.

    Glyph encoding (also matching stock): the alpha channel of the image holds
    the glyph coverage, RGB is white. We emit a 256-entry palette
    palette[i] = (255,255,255, i) and set each pixel's index = its alpha byte,
    so after unpalettization alpha = coverage and the font shader blends it."""
    img = img.convert("RGBA")
    w, h = img.size
    alpha = img.split()[3].tobytes()  # coverage per pixel -> palette index

    # Palette: 256 entries of white with ramped alpha, BGRA byte order.
    pal = bytearray(256 * 4)
    for i in range(256):
        pal[i * 4 + 0] = 255  # B
        pal[i * 4 + 1] = 255  # G
        pal[i * 4 + 2] = 255  # R
        pal[i * 4 + 3] = i    # A = coverage
    data = alpha  # index image (1 byte/pixel) == coverage

    name_b = name.encode("ascii")[:31].ljust(32, b"\x00")
    mask_b = mask.encode("ascii")[:31].ljust(32, b"\x00")
    filter_addr = FILTER_LINEAR | (ADDR_WRAP << 8) | (ADDR_WRAP << 12)

    struct_body = struct.pack("<I", PLATFORM_D3D8)
    struct_body += struct.pack("<I", filter_addr)
    struct_body += name_b + mask_b
    struct_body += struct.pack("<I", FMT_PAL8)
    struct_body += struct.pack("<i", 1)          # hasAlpha
    struct_body += struct.pack("<HH", w, h)
    struct_body += struct.pack("<BBBB", 8, 1, TYPE_TEXTURE, 0)  # depth,levels,type,compression
    struct_body += bytes(pal)                     # 256*4 palette
    struct_body += struct.pack("<I", len(data)) + data          # level 0 indices

    body = chunk(ID_STRUCT, struct_body)
    body += chunk(ID_EXTENSION, b"")  # empty texture extension
    return chunk(ID_TEXTURENATIVE, body)


def main():
    import argparse
    ap = argparse.ArgumentParser(description="Pack PNG(s) into a D3D8 PAL8 TXD")
    ap.add_argument("out", help="output .txd")
    ap.add_argument("specs", nargs="+", help="[name=]img.png (name defaults to file stem)")
    ap.add_argument("--base-txd", default="",
                    help="carry all textures from this TXD verbatim, replacing "
                         "any whose name matches one we generate (e.g. keep the "
                         "stock font1/font2, replace FONTJAP)")
    ap.add_argument("--mask", default="",
                    help="mask name to set on generated textures (e.g. FONTJAP_mask)")
    args = ap.parse_args()

    gen = []
    for s in args.specs:
        if "=" in s:
            name, path = s.split("=", 1)
        else:
            path = s
            name = path.rsplit("/", 1)[-1].rsplit(".", 1)[0]
        gen.append((name, Image.open(path)))
    gen_names = {n.lower() for n, _ in gen}

    chunks = []  # (name, raw_chunk_bytes)
    if args.base_txd:
        for name, raw in split_base_textures(args.base_txd):
            if name.lower() in gen_names:  # RW texture names are case-insensitive
                print("  (replacing base texture '%s')" % name)
                continue
            chunks.append((name, raw))
            print("  = carried base texture '%s'" % name)
    for name, img in gen:
        chunks.append((name, texture_native(name, img, args.mask)))
        print("  + texture '%s' %dx%d%s"
              % (name, img.width, img.height, " mask=" + args.mask if args.mask else ""))

    struct_body = struct.pack("<hh", len(chunks), 0)  # numTex, deviceId (0=unknown, matches stock)
    body = chunk(ID_STRUCT, struct_body)
    for _name, raw in chunks:
        body += raw
    body += chunk(ID_EXTENSION, b"")  # empty dictionary extension
    txd = chunk(ID_TEXDICTIONARY, body)

    with open(args.out, "wb") as f:
        f.write(txd)
    print("wrote %s (%d bytes, %d textures)" % (args.out, len(txd), len(chunks)))


if __name__ == "__main__":
    main()
