#!/usr/bin/env python3
"""给旧蓝图补上 meta.message（部署时自动填充 Message 框用）。

来源优先级：
  1. 蓝图自身 message.text
  2. feed_meta.json：source 里的 recording id -> message
  3. space_pos.json：meta.owner_id -> message

用法:
    python backfill_caption.py [--dir DIR] [--edition steam|cn] [--dry-run]
默认 --edition steam 时扫描 samples/fetched，cn 扫描 %APPDATA%\\sky-blueprint\\cn\\blueprints。
"""
import argparse
import glob
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402


def load_json(path):
    try:
        with open(path, encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def rec_id_from_source(source):
    if not source:
        return ""
    base = os.path.basename(source.split("?")[0])
    return base.rsplit(".", 1)[0]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dir", default="")
    ap.add_argument("--edition", default="steam")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()

    data = bpdata.data_dir(args.edition)
    out = args.dir or (os.path.join(os.path.dirname(os.path.dirname(
        os.path.abspath(__file__))), "samples", "fetched")
        if args.edition == "steam" else os.path.join(data, "blueprints"))
    feed_meta = load_json(os.path.join(data, "feed_meta.json"))
    space_pos = load_json(os.path.join(data, "space_pos.json"))

    filled = 0
    skipped = 0
    for p in glob.glob(os.path.join(out, "*.skyblueprint")):
        try:
            env = skybp.load(p)
        except Exception:
            continue
        meta = env.get("meta") or {}
        if meta.get("message"):
            continue
        cap = ""
        msg = env.get("message")
        if isinstance(msg, dict) and msg.get("text"):
            cap = msg["text"]
        if not cap:
            fm = feed_meta.get(rec_id_from_source(env.get("source"))) or {}
            cap = fm.get("message") or fm.get("content") or ""
        if not cap:
            sp = space_pos.get(meta.get("owner_id") or "") or {}
            cap = sp.get("message") or ""
        if not cap:
            skipped += 1
            continue
        meta["message"] = cap
        env["meta"] = meta
        filled += 1
        if not args.dry_run:
            skybp.pack(env, p)
        print("%s  %s  <- %s" % ("[dry]" if args.dry_run else "[ok] ",
                                 os.path.basename(p), cap[:40]))
    print("filled=%d  no-source=%d" % (filled, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
