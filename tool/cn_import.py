#!/usr/bin/env python3
"""监控国服录制分块 cn_chunk_*.bin（默认 %APPDATA%\\sky-blueprint\\cn\\records），自动转换成 .skyblueprint。

国服录制下载的每个分块是一行：
    {"status":"OK","recording_data":{ ...原生 block... }}
按 stid 分组、按 chet 排序后即可生成蓝图。

用法:
    python cn_import.py            # 持续监控
    python cn_import.py --once     # 处理一次
    python cn_import.py --mod DIR --out DIR
"""
import glob
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

# 分块现在落在 %APPDATA%\sky-blueprint\cn\records\，旧的 mod 目录只读回退
# SKY_EDITION=cn 时输出到 %APPDATA%\sky-blueprint\cn\blueprints（国服面板默认目录）
EDITION = os.environ.get("SKY_EDITION", "")
MOD_DIRS = bpdata.record_dirs(EDITION or "cn")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EDITION:
    OUT = os.path.join(bpdata.data_dir(EDITION), "blueprints")
else:
    OUT = os.path.join(ROOT, "samples", "fetched")
INDEX = os.path.join(OUT, "cn_import_index.json")


def load_index():
    try:
        with open(INDEX, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_index(idx):
    os.makedirs(OUT, exist_ok=True)
    with open(INDEX, "w", encoding="utf-8", newline="\n") as f:
        json.dump(idx, f, ensure_ascii=False, indent=1, sort_keys=True)


def latest_link(mod_dirs):
    lines = []
    for d in mod_dirs:
        p = os.path.join(d, "links.txt")
        if not os.path.exists(p):
            continue
        lines += [l.strip() for l in open(p, encoding="utf-8", errors="replace") if l.strip()]
    return lines[-1] if lines else ""


def _rec_id_from_name(path):
    """cn_chunk_<recording_id>_<start>.bin -> recording_id"""
    name = os.path.basename(path)
    if not name.startswith("cn_chunk_"):
        return ""
    rest = name[len("cn_chunk_"):]
    if "_" not in rest:
        return ""
    head, _ = rest.rsplit("_", 1)
    return head if len(head) >= 32 else ""


def load_feed_meta():
    p = os.path.join(bpdata.data_dir(EDITION or "steam"), "feed_meta.json")
    try:
        with open(p, encoding="utf-8") as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


def scan_chunks(mod_dirs):
    groups = {}
    stid_rec = {}
    files = []
    for d in mod_dirs:
        files += sorted(glob.glob(os.path.join(d, "cn_chunk_*.bin")))
    for p in files:
        try:
            raw = open(p, "rb").read().decode("utf-8", "replace")
        except Exception:
            continue
        rec_id = _rec_id_from_name(p)
        for line in raw.splitlines():
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except Exception:
                continue
            rd = obj.get("recording_data")
            if isinstance(rd, dict) and rd.get("stid"):
                groups.setdefault(rd["stid"], []).append(rd)
                if rec_id:
                    stid_rec.setdefault(rd["stid"], rec_id)
    return groups, stid_rec


def process_once(mod_dirs, out_dir, index):
    groups, stid_rec = scan_chunks(mod_dirs)
    feed_meta = load_feed_meta()
    link = latest_link(mod_dirs)
    changed = 0
    for stid, blocks in groups.items():
        blocks.sort(key=lambda b: float(b.get("chet", 0)))
        fm0 = feed_meta.get(stid_rec.get(stid, "")) or {}
        sig = [len(blocks), max((float(b.get("chet", 0)) for b in blocks), default=0),
               bool(fm0.get("message"))]
        if index.get(stid) == sig:
            continue
        try:
            fm = fm0
            extra = {k: fm.get(k) for k in
                     ("level_id", "pos", "ori", "message", "social_feed_id")}
            env = skybp.build_envelope(blocks, source=link,
                                       pool_type=fm.get("pool_type") or None,
                                       extra_meta=extra)
            path = os.path.join(out_dir, stid + ".skyblueprint")
            skybp.pack(env, path)
            s = env["summary"]
            print("wrote %s  chunks=%d notes=%d placeables=%d duration=%s"
                  % (path, len(blocks), s["notes"], s["placeables"],
                     env["meta"]["duration"]), flush=True)
            index[stid] = sig
            changed += 1
        except Exception as e:
            print("FAIL %s: %s" % (stid, e), flush=True)
    if changed:
        save_index(index)
    return changed


def main():
    args = sys.argv[1:]
    once = "--once" in args
    mod_dirs = list(MOD_DIRS)
    out_dir = OUT
    for i, a in enumerate(args):
        if a == "--mod" and i + 1 < len(args):
            mod_dirs = [args[i + 1]]
        if a == "--out" and i + 1 < len(args):
            out_dir = args[i + 1]

    os.makedirs(out_dir, exist_ok=True)
    index = load_index()
    print("CN 导入监控:", ", ".join(mod_dirs), "->", out_dir, flush=True)
    while True:
        try:
            process_once(mod_dirs, out_dir, index)
        except Exception as e:
            print("loop error:", e, flush=True)
        if once:
            break
        time.sleep(2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
