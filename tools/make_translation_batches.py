#!/usr/bin/env python3
"""make_translation_batches.py - split the GTA3 source text into translation
batches for an LLM, and merge the filled batches back into a translation INI.

Workflow (see docs/11):

  1. Parse Sergeanur/GXT "III PC/american.txt" into key->English pairs.
  2. `split`  : emit batches/batch_NN.json, each a small JSON dict {key: english}
                plus a shared PROMPT.txt describing exactly how to translate.
     A cheap model fills each batch's English values with Chinese IN PLACE,
     writing batches/batch_NN.zh.json (same keys, Chinese values).
  3. `merge`  : combine all batch_NN.zh.json into translation.ini ([GXT] section)
                ready for tools/gen_cn_font.py.

Control codes like ~g~ ~w~ ~h~ ~1~ ~k~ and ~k~~ACTION~ MUST be preserved
verbatim; the prompt instructs the model accordingly and merge validates it.
"""
import argparse
import json
import os
import re
import sys

CTRL_RE = re.compile(r"~[A-Za-z0-9_]*~")

PROMPT = """\
You are translating the in-game text of Grand Theft Auto III into Simplified
Chinese (简体中文) for a fan port. You are given a JSON object mapping string
keys to English source text. Return a JSON object with the SAME keys, where each
value is the Simplified Chinese translation of the English value.

HARD RULES (violating any of these breaks the game):
1. Keep every key exactly as given. Do not add, remove, rename, or reorder keys.
2. Preserve ALL control codes verbatim, in place: tokens like ~g~ ~w~ ~h~ ~r~
   ~b~ ~y~ ~p~ ~1~ ~k~ and key tokens like ~k~~VEHICLE_ACCELERATE~. Never
   translate, delete, reorder, or add spaces inside them. They may appear at the
   start, middle, or end of a value; keep their exact positions relative to text.
3. Do NOT translate ALL-CAPS placeholder tokens inside ~k~...~ (e.g.
   ~VEHICLE_ENTER_EXIT~). Leave them exactly as-is.
4. Output ONLY a valid JSON object, UTF-8, no comments, no markdown fences.
5. Keep translations concise - this is for a 320x240 screen. Prefer short,
   natural game Chinese (e.g. "WASTED"->"死亡", "BUSTED"->"被捕").
6. Proper nouns: translate place/character names to common Chinese GTA3
   conventions when well known; otherwise keep the English name.
7. If a value is empty or only control codes/punctuation, return it unchanged.
8. Do NOT translate these special keys - copy their value verbatim: LETTER1
   (a font charset definition), DEFNAM (the default player name), and any value
   that is purely ASCII letters/dashes with no real words to translate.

Example input:
  {"1001": "BUSTED", "HELP2_A": "Press the ~h~/ button~w~ when running to ~h~sprint."}
Example output:
  {"1001": "被捕", "HELP2_A": "奔跑时按 ~h~/ 键~w~可以~h~冲刺。"}
"""


def parse_source(path):
    txt = open(path, encoding="utf-8-sig", errors="replace").read()  # strip BOM
    entries = {}
    cur, buf = None, []
    for line in txt.splitlines():
        m = re.match(r"^\[([^\]]+)\]", line)
        if m:
            if cur is not None:
                entries[cur] = "\n".join(buf).strip()
            cur, buf = m.group(1), []
        else:
            buf.append(line)
    if cur is not None:
        entries[cur] = "\n".join(buf).strip()
    return entries


def codes(s):
    return CTRL_RE.findall(s)


def cmd_split(args):
    entries = parse_source(args.source)
    os.makedirs(args.outdir, exist_ok=True)
    with open(os.path.join(args.outdir, "PROMPT.txt"), "w", encoding="utf-8") as f:
        f.write(PROMPT)

    keys = list(entries.keys())
    nb = 0
    for i in range(0, len(keys), args.size):
        chunk = {k: entries[k] for k in keys[i:i + args.size]}
        path = os.path.join(args.outdir, "batch_%03d.json" % nb)
        json.dump(chunk, open(path, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        nb += 1
    print("wrote %d entries into %d batches (size %d) in %s"
          % (len(keys), nb, args.size, args.outdir))
    print("PROMPT.txt written. For each batch_NN.json, have the model produce")
    print("batch_NN.zh.json (same keys, Chinese values) using PROMPT.txt.")


def cmd_merge(args):
    src = parse_source(args.source)
    out = {}
    missing_files = []
    warned = 0
    nb = 0
    while True:
        zpath = os.path.join(args.outdir, "batch_%03d.zh.json" % nb)
        opath = os.path.join(args.outdir, "batch_%03d.json" % nb)
        if not os.path.exists(opath):
            break
        if not os.path.exists(zpath):
            missing_files.append(zpath)
            nb += 1
            continue
        zh = json.load(open(zpath, encoding="utf-8"))
        for k, v in zh.items():
            # Validate control codes match the source (order-insensitive multiset).
            if k in src and sorted(codes(src[k])) != sorted(codes(v)):
                sys.stderr.write("WARN %s: control codes differ\n  EN=%s\n  ZH=%s\n"
                                 % (k, src[k], v))
                warned += 1
            out[k] = v
        nb += 1

    if missing_files:
        sys.stderr.write("Missing %d batch translations:\n  %s\n"
                         % (len(missing_files), "\n  ".join(missing_files)))

    # Fill any untranslated keys with the English source so the GXT stays complete.
    filled = 0
    for k, v in src.items():
        if k not in out:
            out[k] = v
            filled += 1

    # Preserve the ORIGINAL source key order (upstream groups related strings).
    ordered_keys = list(src.keys())

    if args.out:
        import configparser
        cfg = configparser.ConfigParser(interpolation=None)
        cfg["GXT"] = {k: out[k] for k in ordered_keys}
        with open(args.out, "w", encoding="utf-8") as f:
            cfg.write(f)

    if args.out_txt:
        # Emit the upstream Sergeanur/GXT txt format ([KEY]\nvalue\n\n), in the
        # original source order, so it drops into a GXT repo and their compiler.
        with open(args.out_txt, "w", encoding="utf-8") as f:
            for k in ordered_keys:
                f.write("[%s]\n%s\n\n" % (k, out[k]))

    outs = ", ".join(p for p in (args.out, args.out_txt) if p)
    print("merged %d keys -> %s (%d fell back to English, %d code warnings)"
          % (len(out), outs, filled, warned))


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    sub = ap.add_subparsers(dest="cmd", required=True)

    sp = sub.add_parser("split", help="split source into batch JSONs + PROMPT")
    sp.add_argument("source", help="american.txt (Sergeanur/GXT format)")
    sp.add_argument("--outdir", default="batches")
    sp.add_argument("--size", type=int, default=200, help="entries per batch")
    sp.set_defaults(func=cmd_split)

    mp = sub.add_parser("merge", help="merge batch_NN.zh.json into translation.ini")
    mp.add_argument("source", help="american.txt (for validation + fallback)")
    mp.add_argument("--outdir", default="batches")
    mp.add_argument("--out", default="translation.ini",
                    help="output INI for gen_cn_font.py ('' to skip)")
    mp.add_argument("--out-txt", default="",
                    help="also emit upstream Sergeanur/GXT txt (e.g. 'III PC/chinese.txt')")
    mp.set_defaults(func=cmd_merge)

    args = ap.parse_args()
    args.func(args)


if __name__ == "__main__":
    main()
