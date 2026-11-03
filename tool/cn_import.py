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

# 分块落在 %APPDATA%\sky-blueprint\cn\records\
MOD_DIRS = bpdata.record_dirs("cn")
OUT = os.path.join(bpdata.data_dir("steam"), "blueprints")   # 与 mod 默认目录一致
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


def scan_chunks(mod_dirs):
    groups = {}
    files = []
    for d in mod_dirs:
        files += sorted(glob.glob(os.path.join(d, "cn_chunk_*.bin")))
    for p in files:
        try:
            raw = open(p, "rb").read().decode("utf-8", "replace")
        except Exception:
            continue
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
    return groups


def process_once(mod_dirs, out_dir, index):
    groups = scan_chunks(mod_dirs)
    link = latest_link(mod_dirs)
    changed = 0
    for stid, blocks in groups.items():
        blocks.sort(key=lambda b: float(b.get("chet", 0)))
        sig = [len(blocks), max((float(b.get("chet", 0)) for b in blocks), default=0)]
        if index.get(stid) == sig:
            continue
        try:
            env = skybp.build_envelope(blocks, source=link)
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
