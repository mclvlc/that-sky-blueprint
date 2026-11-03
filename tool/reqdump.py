#!/usr/bin/env python3
"""解析 mod 抓到的出站请求 req_*.bin，提取 HTTP 头、session token 和 JSON 字段。

用法:
    python reqdump.py [--dir CAPTURE_DIR] [--all]
    默认目录：<数据目录>/steam/captures
"""
import glob
import json
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402

MOD_DIR = os.path.join(bpdata.data_dir("steam"), "captures")

INTERESTING = re.compile(
    r"^(x-session-token|session|authorization|user-id|x-user-id|"
    r"x-session-id|cookie|content-length|content-type)", re.I)


def parse_one(path, verbose=False):
    with open(path, "rb") as f:
        data = f.read()

    text = data.decode("latin-1")
    m = re.match(r"(GET|POST|PUT|DELETE|HEAD|OPTIONS|PATCH) (\S+)", text)
    if not m:
        if verbose:
            print("%s  [非 HTTP / 可能是 WebSocket 或分片] %d 字节  %r"
                  % (os.path.basename(path), len(data), data[:16]))
        return None

    method, url = m.group(1), m.group(2)
    head, _, body = text.partition("\r\n\r\n")
    lines = head.split("\r\n")

    print("%s  %s %s  body=%d" % (os.path.basename(path), method, url, len(body)))
    for line in lines[1:]:
        if INTERESTING.match(line):
            print("    " + line[:300])

    result = {"file": path, "method": method, "url": url, "headers": {}}
    for line in lines[1:]:
        if ":" in line:
            k, v = line.split(":", 1)
            result["headers"][k.strip().lower()] = v.strip()

    if body.strip():
        preview = body[:160].replace("\r", " ").replace("\n", " ")
        print("    body: " + preview)
        try:
            obj = json.loads(body)
            result["json"] = obj
            for k in ("session", "recording_id", "chunk", "user_id", "user",
                      "social_feed_id", "feed_id"):
                if k in obj:
                    print("    %s = %s" % (k, str(obj[k])[:80]))
            if "recording_data" in obj:
                rd = obj["recording_data"]
                if isinstance(rd, dict):
                    print("    recording_data: stid=%s chet=%s chln=%s keys=%s"
                          % (rd.get("stid"), rd.get("chet"), rd.get("chln"),
                             list(rd.keys())[:10]))
        except Exception:
            pass

    return result


def main():
    args = sys.argv[1:]
    mod_dir = MOD_DIR
    for i, a in enumerate(args):
        if a == "--dir" and i + 1 < len(args):
            mod_dir = args[i + 1]

    files = sorted(glob.glob(os.path.join(mod_dir, "req_*.bin")))
    if not files:
        print("没有 req_*.bin：")
        print("  1) 出站 hook 是否安装（bp.log 里应有 OUT-REQ manual hook OK）")
        print("  2) 游戏重启后是否触发过新的网络请求（打开一个留影即可）")
        return 1

    print("共 %d 个请求样本\n" % len(files))
    results = []
    for p in files:
        r = parse_one(p, verbose="--all" in args)
        if r:
            results.append(r)

    # 汇总 token
    tokens = []
    for r in results:
        h = r["headers"]
        for k in ("x-session-token", "session", "authorization"):
            if k in h and h[k] not in tokens:
                tokens.append(h[k])
        j = r.get("json") or {}
        if isinstance(j, dict) and j.get("session") and j["session"] not in tokens:
            tokens.append(j["session"])

    if tokens:
        print("\n=== 可能的 session / 凭证 ===")
        for t in tokens:
            print("  " + t[:120])
    return 0


if __name__ == "__main__":
    sys.exit(main())
