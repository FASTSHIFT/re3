#!/usr/bin/env python3
"""translate_doubao.py - translate GTA3 text batches to Simplified Chinese via
the Volcengine Ark (Doubao) API.

Reads spike/batches/batch_NN.json, sends each batch to the model with the
translation prompt from tools/make_translation_batches.py, and writes
spike/batches/batch_NN.zh.json. Skips batches already translated (idempotent /
resumable). Validates that control codes (~g~ etc.) are preserved per key and
retries a batch a few times on JSON/validation failure.

Auth: reads ARK_API_KEY from the environment (see hello_doubao.py).
Requires: pip install volcenginesdkarkruntime
"""
import argparse
import glob
import json
import os
import re
import sys
import time

try:
    from volcenginesdkarkruntime import Ark
except ImportError:
    sys.exit("SDK missing: pip install volcenginesdkarkruntime")

# Reuse the exact prompt the batch tool defines, so the offline handoff doc and
# the API path stay identical.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from make_translation_batches import PROMPT, CTRL_RE  # noqa: E402


def extract_text(resp):
    """Pull the assistant's text out of an Ark responses.create result,
    skipping reasoning items."""
    # Convenience attribute on newer SDKs.
    t = getattr(resp, "output_text", None)
    if t:
        return t
    parts = []
    for item in getattr(resp, "output", []) or []:
        if getattr(item, "type", None) == "message":
            for c in getattr(item, "content", []) or []:
                if getattr(c, "type", None) == "output_text":
                    parts.append(c.text)
    return "".join(parts)


def parse_json_object(text):
    """Extract the first top-level JSON object from model output (tolerates
    stray prose or code fences)."""
    text = text.strip()
    if text.startswith("```"):
        text = re.sub(r"^```[a-zA-Z]*\n?", "", text)
        text = re.sub(r"\n?```$", "", text)
    start = text.find("{")
    end = text.rfind("}")
    if start < 0 or end < 0:
        raise ValueError("no JSON object in output")
    return json.loads(text[start:end + 1])


def codes(s):
    return sorted(CTRL_RE.findall(s))


def translate_batch(client, model, src, max_tries=3):
    """Translate one {key: english} dict, return {key: chinese}. Retries on
    parse/validation errors, feeding back the problem."""
    result = {}          # accumulated good translations across rounds
    todo = dict(src)     # keys still needing a (re)translation
    extra = ""           # targeted feedback appended on retries
    last_err = None
    for attempt in range(1, max_tries + 1):
        user = PROMPT + extra + "\n\nInput JSON:\n" + json.dumps(todo, ensure_ascii=False)
        try:
            resp = client.responses.create(
                model=model, input=user, thinking={"type": "disabled"})
            out = parse_json_object(extract_text(resp))
        except Exception as e:
            last_err = "api/parse: %s" % e
            time.sleep(1.5 * attempt)
            continue

        # Accept keys whose control codes match; re-queue the rest.
        bad = {}
        for k in todo:
            if k in out and codes(todo[k]) == codes(out[k]):
                result[k] = out[k]
            else:
                bad[k] = todo[k]
        if not bad:
            return result, None

        # Build targeted feedback listing the exact codes each bad key must keep.
        lines = ["\nThe previous attempt for these keys DROPPED or CHANGED control",
                 "codes. Re-translate ONLY these, keeping EXACTLY the listed codes",
                 "in order, same count:"]
        for k in list(bad)[:20]:
            lines.append("  %s must contain codes: %s" % (k, " ".join(CTRL_RE.findall(bad[k]))))
        extra = "\n".join(lines)
        todo = bad
        last_err = "badcodes=%d (e.g. %s)" % (len(bad), list(bad)[:3])

    # Out of tries: keep whatever validated; caller falls back to English for
    # the rest so the batch still completes.
    return (result, None) if result else (None, last_err)


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default="spike/batches")
    ap.add_argument("--model", default="doubao-seed-2-1-pro-260628")
    ap.add_argument("--base-url", default="https://ark.cn-beijing.volces.com/api/v3")
    ap.add_argument("--only", default="", help="translate only this batch id, e.g. 001")
    ap.add_argument("--force", action="store_true", help="retranslate even if .zh.json exists")
    args = ap.parse_args()

    key = os.getenv("ARK_API_KEY")
    if not key:
        sys.exit("ARK_API_KEY not set")
    client = Ark(base_url=args.base_url, api_key=key)

    batches = sorted(glob.glob(os.path.join(args.dir, "batch_*.json")))
    batches = [b for b in batches if not b.endswith(".zh.json")]
    if args.only:
        batches = [b for b in batches if ("batch_%s.json" % args.only) in b]

    total_ok = total_fail = 0
    for b in batches:
        zh = b[:-5] + ".zh.json"
        nn = re.search(r"batch_(\d+)\.json$", b).group(1)
        if os.path.exists(zh) and not args.force:
            print("batch %s: already done, skip" % nn)
            continue
        src = json.load(open(b, encoding="utf-8"))
        t0 = time.time()
        out, err = translate_batch(client, args.model, dict(src))
        if err:
            print("batch %s: FAILED (%s)" % (nn, err))
            total_fail += 1
            continue
        # Preserve full key set/order from the source batch; keep any value the
        # model returned, fall back to source for anything it dropped.
        merged = {k: out.get(k, src[k]) for k in src}
        json.dump(merged, open(zh, "w", encoding="utf-8"), ensure_ascii=False, indent=1)
        print("batch %s: OK %d keys in %.1fs -> %s"
              % (nn, len(merged), time.time() - t0, os.path.basename(zh)))
        total_ok += 1

    print("done: %d ok, %d failed" % (total_ok, total_fail))


if __name__ == "__main__":
    main()
