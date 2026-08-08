#!/usr/bin/env python3
"""gen_cn_font.py - build a Chinese font atlas + GXT for re3 (see docs/11).

Given a translation file (INI: [GXT] key = value, values in real Unicode) and a
TTF/TTC font, this produces everything re3 needs to show Simplified Chinese with
zero runtime cost, reusing the game's texture-atlas font model:

  * chinese.gxt        standard GTA3 GXT (TKEY + TDAT wchar16); each CJK glyph is
                       encoded as its assigned atlas cell index, ASCII stays ASCII
  * fonts_c.png        the glyph atlas (COLS x rows of fixed cells)
  * cn_charmap.json    real-char <-> assigned-codepoint map (debug / reuse)
  * cn_font_widths.inc  optional advance-width table (C array)

Codepoint assignment:
  ASCII (< 0x80) is kept as-is so it renders via the existing western font when
  the value is small; CJK/other glyphs are assigned sequential indices starting
  at --base (default 0x0100), which double as atlas cell indices. re3's PrintChar
  maps cell = c % COLS, row = c / COLS for the Chinese font (mirrors FONTJAP).

This is an offline tool; not built by CMake. Requires Pillow.
"""
import argparse
import configparser
import json
import struct
import sys

try:
    from PIL import Image, ImageFont, ImageDraw
except ImportError:
    sys.exit("Pillow required: pip install pillow")


def load_translation(path):
    """Load a translation as {key: value} (real Unicode). Accepts either:
      * the upstream Sergeanur/GXT txt format ([KEY]\\nvalue\\n\\n), or
      * an INI with a [GXT] section (gxt-utils style).
    Chosen by extension (.txt -> upstream) with a content sniff fallback."""
    import re
    raw = open(path, encoding="utf-8-sig", errors="replace").read()  # strip BOM
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
    """Return sorted list of unique non-ASCII characters used across all values."""
    glyphs = set()
    for v in entries.values():
        for ch in v:
            if ord(ch) >= 0x80:
                glyphs.add(ch)
    return sorted(glyphs)


def build_charmap(glyphs, base):
    """Assign each non-ASCII glyph a sequential codepoint starting at base.
    Returns {char: codepoint}. base should clear ASCII (>= 0x100 recommended)."""
    charmap = {}
    code = base
    for ch in glyphs:
        charmap[ch] = code
        code += 1
    return charmap


def render_atlas(glyphs, font_path, cell, cols, px):
    """Render glyphs into a fixed-grid atlas. cell = pixel size of a cell,
    cols = cells per row, px = font pixel size (<= cell). Returns an RGBA image
    with white glyphs and coverage in the alpha channel, matching how the game's
    font textures store glyphs (colour is tinted at draw time, alpha is shape)."""
    n = len(glyphs)
    rows = (n + cols - 1) // cols
    # Render coverage into an L image first, then expand to white+alpha.
    cov = Image.new("L", (cols * cell, rows * cell), 0)
    font = ImageFont.truetype(font_path, px)
    draw = ImageDraw.Draw(cov)
    for i, ch in enumerate(glyphs):
        cx = (i % cols) * cell
        cy = (i // cols) * cell
        bbox = draw.textbbox((0, 0), ch, font=font)
        gw = bbox[2] - bbox[0]
        gh = bbox[3] - bbox[1]
        ox = cx + (cell - gw) // 2 - bbox[0]
        oy = cy + (cell - gh) // 2 - bbox[1]
        draw.text((ox, oy), ch, fill=255, font=font)
    white = Image.new("RGBA", cov.size, (255, 255, 255, 0))
    white.putalpha(cov)
    return white, rows


def to_gxt(entries, charmap):
    """Pack entries into a standard GTA3 GXT. ASCII chars use their own code,
    CJK chars use the assigned codepoint. Returns bytes."""
    all_values = b""
    offsets = {}
    cur = 0
    # Deterministic order: sort keys like the game's tools do.
    ordered = sorted(entries.keys(), key=lambda k: k.upper())
    for key in ordered:
        offsets[key] = cur
        for ch in entries[key]:
            code = ord(ch) if ord(ch) < 0x80 else charmap[ch]
            all_values += struct.pack("<H", code)
        all_values += b"\x00\x00"
        cur = len(all_values)

    tdat = b"TDAT" + struct.pack("<I", len(all_values)) + all_values

    all_keys = b""
    for key in ordered:
        k = key.upper().encode("ascii")[:8]
        all_keys += struct.pack("<I8s", offsets[key], k)
    tkey = b"TKEY" + struct.pack("<I", len(ordered) * 12) + all_keys

    return tkey + tdat


def main():
    ap = argparse.ArgumentParser(description="Build re3 Chinese font atlas + GXT")
    ap.add_argument("translation", help="INI translation ([GXT] key=value, Unicode)")
    ap.add_argument("--font", default="/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
                    help="TTF/TTC font path")
    ap.add_argument("--out-gxt", default="chinese.gxt")
    ap.add_argument("--out-png", default="fonts_c.png")
    ap.add_argument("--out-map", default="cn_charmap.json")
    ap.add_argument("--base", type=lambda x: int(x, 0), default=0x0100,
                    help="first assigned codepoint (default 0x100)")
    ap.add_argument("--cell", type=int, default=32, help="atlas cell size in px")
    ap.add_argument("--px", type=int, default=28, help="glyph render size in px")
    ap.add_argument("--cols", type=int, default=64, help="atlas columns")
    args = ap.parse_args()

    entries = load_translation(args.translation)
    glyphs = collect_glyphs(entries)
    print("entries: %d, unique CJK glyphs: %d" % (len(entries), len(glyphs)))
    if not glyphs:
        print("warning: no non-ASCII glyphs found; is the translation Unicode?")

    charmap = build_charmap(glyphs, args.base)
    top = args.base + len(glyphs) - 1 if glyphs else args.base
    if top > 0xFFFF:
        sys.exit("too many glyphs: assigned codepoint 0x%X exceeds 16-bit" % top)

    atlas, rows = render_atlas(glyphs, args.font, args.cell, args.cols, args.px)
    atlas.save(args.out_png)
    print("atlas: %dx%d px, %d cols x %d rows, saved %s"
          % (atlas.width, atlas.height, args.cols, rows, args.out_png))

    gxt = to_gxt(entries, charmap)
    with open(args.out_gxt, "wb") as f:
        f.write(gxt)
    print("gxt: %d bytes, saved %s" % (len(gxt), args.out_gxt))

    with open(args.out_map, "w", encoding="utf-8") as f:
        json.dump({"base": args.base, "cols": args.cols, "cell": args.cell,
                   "map": {ch: code for ch, code in charmap.items()}},
                  f, ensure_ascii=False, indent=1)
    print("charmap: saved %s" % args.out_map)


if __name__ == "__main__":
    main()
