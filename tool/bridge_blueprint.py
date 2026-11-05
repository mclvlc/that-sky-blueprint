#!/usr/bin/env python3
"""把国服蓝图复制到国际服蓝图目录，方便在另一个服的 mod 面板里部署。

国服 block 和国际服只差一个 stfs 字段（多一个 "en"），其余结构相同；
本工具默认原样复制（部署时 stid 会重新生成）：

    python bridge_blueprint.py <file.skyblueprint>
    python bridge_blueprint.py <file.skyblueprint> --to-dir samples/fetched
    python bridge_blueprint.py --dir C:\\...\\cn\\blueprints --all

复制后的文件名加 `bridge_` 前缀，避免和原文件混淆。
"""
import argparse
import glob
import json
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CN = os.path.join(bpdata.data_dir("cn"), "blueprints")
DEFAULT_INTL = os.path.join(ROOT, "samples", "fetched")


def bridge(path, out_dir):
    env = skybp.load(path)
    name = "bridge_" + os.path.basename(path)
    dst = os.path.join(out_dir, name)
    os.makedirs(out_dir, exist_ok=True)
    shutil.copyfile(path, dst)
    meta = env.get("meta") or {}
    print("copied -> %s  kind=%s chunks=%s caption=%s"
          % (dst, env.get("kind"), meta.get("chunks"),
             (meta.get("message") or "")[:30]))
    return dst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("files", nargs="*")
    ap.add_argument("--dir", default="", help="批量：源目录")
    ap.add_argument("--to-dir", default=DEFAULT_INTL)
    ap.add_argument("--all", action="store_true", help="复制 --dir 下全部蓝图")
    args = ap.parse_args()

    files = list(args.files)
    if args.dir and (args.all or not files):
        files += sorted(glob.glob(os.path.join(args.dir, "*.skyblueprint")))
    if not files:
        print("用法: python bridge_blueprint.py <file.skyblueprint> 或 --dir DIR --all")
        return 2

    for p in files:
        try:
            bridge(p, args.to_dir)
        except Exception as e:
            print("FAIL %s: %s" % (p, e))
    return 0


if __name__ == "__main__":
    sys.exit(main())
