#!/usr/bin/env python3
"""gen_cn_font.py - build Chinese font assets for re3's (widened) CJK pipeline.

re3 reuses the game's Japanese font path for CJK: a big glyph atlas indexed by
codepoint, full-width layout. This tool produces assets that drop straight into
the Japanese slots (JAPANESE.GXT + a FONTJAP atlas), with the RE3_CHINESE build
widening the atlas geometry to 64 cols x 32 rows (see docs/11 and Font.cpp).

Codepoint / atlas contract (MUST match Font.cpp when RE3_CHINESE):
  * The print loop subtracts 0x20 from every wchar before indexing, and the
    CJK draw uses cell = (wchar-0x20), col = cell % COLS, row = cell // COLS.
  * So a glyph placed in atlas cell i is stored in the GXT as wchar (i + 0x20).
  * ASCII is placed in cells 0..94 so that wchar == the ASCII code itself
    (' ' -> cell 0 -> 0x20, 'A' -> cell 33 -> 0x41): ASCII text needs no
    remapping and renders from the same atlas.
  * Chinese glyphs occupy cells 95.. (wchar 0x7F..).
  * Control tokens (~g~ etc.): the '~' is stored as JAP_TERMINATION (0x8000|'~')
    and the inner letters as plain ASCII, exactly like the stock JAPANESE.GXT.

Outputs:
  * out-gxt  : JAPANESE.GXT (TKEY + TDAT, the codepoint scheme above)
  * out-png  : FONTJAP atlas (COLS x rows of CELL px, white glyphs, alpha=shape)
  * out-map  : char <-> cell/codepoint map (debug)

Then pack the PNG with tools/png2txd.py as texture 'FONTJAP' into FONTS_J.TXD.

This is an offline tool; not built by CMake. Requires Pillow.
"""
import argparse
import configparser
import json
import re
import struct
import sys

try:
    from PIL import Image, ImageFont, ImageDraw
except ImportError:
    sys.exit("Pillow required: pip install pillow")

JAP_TERM = 0x8000 | ord('~')  # 0x807E, how the JP GXT stores '~'
CTRL_RE = re.compile(r"~[A-Za-z0-9_]*~")
ASCII_CELLS = 95  # cells 0..94 hold ASCII 0x20..0x7E (' '..'~')


def load_translation(path):
    """Load {key: value} from the upstream Sergeanur/GXT txt format
    ([KEY]\\nvalue\\n\\n) or an INI [GXT] section. BOM-tolerant."""
    raw = open(path, encoding="utf-8-sig", errors="replace").read()
    is_txt = path.lower().endswith(".txt") or (
        "[GXT]" not in raw and re.search(r"^\[[^\]]+\]\s*$", raw, re.M))
    if is_txt:
        entries = {}
        cur, buf = None, []
        for line in raw.splitlines():
            m = re.match(r"^\[([^\]]+)\]\s*$", line)
            if m:
                if cur is not None:
                    entries[cur] = "\n".join(buf).strip()
                cur, buf = m.group(1), []
            else:
                buf.append(line)
        if cur is not None:
            entries[cur] = "\n".join(buf).strip()
        return entries
    cfg = configparser.ConfigParser(interpolation=None)
    cfg.read(path, encoding="utf-8")
    if "GXT" not in cfg:
        sys.exit("no [GXT] section in %s" % path)
    return dict(cfg["GXT"])


def collect_glyphs(entries):
    """Unique non-ASCII characters actually used across all values, sorted."""
    glyphs = set()
    for v in entries.values():
        for ch in v:
            if ord(ch) >= 0x80:
                glyphs.add(ch)
    return sorted(glyphs)


def build_charmap(glyphs):
    """Assign each Chinese glyph an atlas cell starting at ASCII_CELLS, and thus
    a GXT codepoint (cell + 0x20). Returns {char: (cell, codepoint)}."""
    m = {}
    for i, ch in enumerate(glyphs):
        cell = ASCII_CELLS + i
        m[ch] = (cell, cell + 0x20)
    return m


def encode_value(v, charmap):
    """Encode one string to a list of GXT wchars following the JP scheme.

    Control tokens: in the stock JAPANESE.gxt EVERY character of a ~...~ token
    (the tildes, the tag letter, and the inner ACTION name) carries the 0x8000
    bit, e.g. ~k~~PED_FIREWEAPON~ -> 0x807E 0x806B 0x807E 0x807E 0x8050 ... .
    The engine's token parser (Messages.cpp, Font.cpp) matches on that flag, so
    we must OR 0x8000 onto the whole token, not just the tildes."""
    out = []
    i = 0
    while i < len(v):
        ch = v[i]
        if ch == '~':
            out.append(JAP_TERM)  # 0x8000 | '~'
            i += 1
            while i < len(v) and v[i] != '~':
                out.append(0x8000 | (ord(v[i]) & 0xFF))  # flag inner token chars
                i += 1
            if i < len(v):  # closing '~'
                out.append(JAP_TERM)
                i += 1
            continue
        o = ord(ch)
        if o < 0x80:
            out.append(o)               # ASCII passes through (cell o-0x20)
        else:
            out.append(charmap[ch][1])  # Chinese -> assigned codepoint
        i += 1
    return out


def to_gxt(entries, charmap):
    all_values = b""
    offsets = {}
    cur = 0
    ordered = list(entries.keys())  # preserve source order
    for key in ordered:
        offsets[key] = cur
        for code in encode_value(entries[key], charmap):
            all_values += struct.pack("<H", code)
        all_values += b"\x00\x00"
        cur = len(all_values)
    tdat = b"TDAT" + struct.pack("<I", len(all_values)) + all_values

    # re3 looks keys up with a BinarySearch using strcmp, so the TKEY entries
    # MUST be sorted by the (uppercased, NUL-truncated) key. TDAT order is
    # irrelevant since entries carry explicit offsets.
    def key8(k):
        return k.upper().encode("ascii")[:8]
    all_keys = b""
    for key in sorted(ordered, key=lambda k: key8(k)):
        all_keys += struct.pack("<I8s", offsets[key], key8(key))
    tkey = b"TKEY" + struct.pack("<I", len(ordered) * 12) + all_keys
    return tkey + tdat


def render_atlas(glyphs, charmap, font_path, cell, cols, rows, px, ascii_font_path, ascii_px):
    """Render ASCII (cells 0..94) then Chinese (95..) into a fixed grid. White
    glyphs with coverage in alpha. Height = rows*cell (must match CJK_ROWS_UV)."""
    W, H = cols * cell, rows * cell
    cov = Image.new("L", (W, H), 0)
    draw = ImageDraw.Draw(cov)
    cn_font = ImageFont.truetype(font_path, px)
    ascii_font = ImageFont.truetype(ascii_font_path or font_path, ascii_px)

    def put(cellidx, ch, font):
        cx = (cellidx % cols) * cell
        cy = (cellidx // cols) * cell
        bbox = draw.textbbox((0, 0), ch, font=font)
        gw, gh = bbox[2] - bbox[0], bbox[3] - bbox[1]
        ox = cx + (cell - gw) // 2 - bbox[0]
        oy = cy + (cell - gh) // 2 - bbox[1]
        draw.text((ox, oy), ch, fill=255, font=font)

    # ASCII 0x20..0x7E into cells 0..94.
    for code in range(0x20, 0x7F):
        put(code - 0x20, chr(code), ascii_font)
    # Chinese glyphs.
    for ch, (cellidx, _code) in charmap.items():
        put(cellidx, ch, cn_font)

    # White glyph, coverage in the ALPHA channel. png2txd.py packs this into a
    # PAL8 texture whose palette ramps alpha with the index (matching the stock
    # FONTJAP), so after librw unpalettizes it on GL3 the font shader blends
    # alpha = coverage. (RGB stays white; the vertex colour tints the glyph.)
    white = Image.new("RGBA", (W, H), (255, 255, 255, 0))
    white.putalpha(cov)
    return white


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("translation", help="chinese.txt (upstream fmt) or INI")
    ap.add_argument("--font", default="spike/zpix.ttf", help="CJK TTF/TTC")
    ap.add_argument("--ascii-font", default="", help="ASCII TTF (default: --font)")
    ap.add_argument("--out-gxt", default="JAPANESE.GXT")
    ap.add_argument("--out-png", default="FONTJAP.png")
    ap.add_argument("--out-map", default="cn_charmap.json")
    ap.add_argument("--cols", type=int, default=64, help="atlas columns (match CJK_COLS)")
    ap.add_argument("--rows", type=int, default=32, help="atlas rows (match CJK_ROWS_UV)")
    ap.add_argument("--cell", type=int, default=16, help="cell px (texW/cols)")
    ap.add_argument("--px", type=int, default=12, help="CJK glyph render px")
    ap.add_argument("--ascii-px", type=int, default=12, help="ASCII glyph render px")
    args = ap.parse_args()

    entries = load_translation(args.translation)
    glyphs = collect_glyphs(entries)
    charmap = build_charmap(glyphs)
    cap = args.cols * args.rows
    used = ASCII_CELLS + len(glyphs)
    print("entries: %d, CJK glyphs: %d, cells used: %d / %d"
          % (len(entries), len(glyphs), used, cap))
    if used > cap:
        sys.exit("atlas too small: need %d cells, have %d (raise --cols/--rows)"
                 % (used, cap))
    top_code = (ASCII_CELLS + len(glyphs) - 1) + 0x20 if glyphs else 0x7E
    if top_code >= 0x8000:
        sys.exit("codepoint 0x%X collides with JAP_TERMINATION range" % top_code)

    atlas = render_atlas(glyphs, charmap, args.font, args.cell, args.cols,
                         args.rows, args.px, args.ascii_font, args.ascii_px)
    atlas.save(args.out_png)
    print("atlas: %dx%d, %d cols x %d rows -> %s"
          % (atlas.width, atlas.height, args.cols, args.rows, args.out_png))

    gxt = to_gxt(entries, charmap)
    open(args.out_gxt, "wb").write(gxt)
    print("gxt: %d bytes -> %s" % (len(gxt), args.out_gxt))

    json.dump({"cols": args.cols, "rows": args.rows, "cell": args.cell,
               "ascii_cells": ASCII_CELLS,
               "map": {ch: {"cell": c, "code": code} for ch, (c, code) in charmap.items()}},
              open(args.out_map, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
    print("charmap -> %s" % args.out_map)


if __name__ == "__main__":
    main()
