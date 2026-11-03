#!/usr/bin/env python3
"""监控 mod 抓到的 links.txt，自动下载留影 ndjson 并生成完整的 .skyblueprint。

与旧版的区别：
  - 不再只取最后一帧（那只是最后 4 秒），而是保留全部分块并跨块合并
  - 自动校验、去重、写索引 index.json
  - 支持 ndjson / JSON 数组 / 单个 block（留言、共享空间）

用法:
    python autofetch.py            # 持续监控 links.txt
    python autofetch.py --once     # 处理完当前 link 就退出
    python autofetch.py --dir OUT  # 指定输出目录
    python autofetch.py --force    # 已存在的 stid 也重新生成
"""
import gzip
import json
import os
import sys
import time
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

LINKS = bpdata.find("links.txt", "steam")   # <data>/links.txt
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT_DIR = os.path.join(bpdata.data_dir("steam"), "blueprints")   # mod 默认读取的位置

# 可选的域名替换表。不同服务器/加速环境会把 link 里的域名写成别的样子，
# 需要时在 tool/server.json 里配置，例如：
#     {"rewrite": {"old.example.com": "new.example.com"}}
# 也可以用环境变量 SKY_HOST_REWRITE="old=new,old2=new2"。
HOST_REWRITE = {}

UA = "Sky-Live-com.tgc.sky.win"


def load_rewrite():
    global HOST_REWRITE
    env = os.environ.get("SKY_HOST_REWRITE", "")
    if env:
        for pair in env.split(","):
            if "=" in pair:
                a, b = pair.split("=", 1)
                HOST_REWRITE[a.strip()] = b.strip()
    cfg = os.path.join(os.path.dirname(os.path.abspath(__file__)), "server.json")
    if os.path.exists(cfg):
        try:
            with open(cfg, encoding="utf-8") as f:
                HOST_REWRITE.update(json.load(f).get("rewrite") or {})
        except Exception:
            pass


def rewrite(url: str) -> str:
    # 官服 link 是 s3://bucket/key，直接转成 HTTPS 即可
    if url.startswith("s3://"):
        return "https://" + url[5:]
    for fake, real in HOST_REWRITE.items():
        if fake in url:
            return url.replace(fake, real)
    return url


def _get(url: str) -> bytes:
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
        enc = (r.headers.get("Content-Encoding") or "").lower()
    if enc == "gzip" or data[:2] == b"\x1f\x8b":
        data = gzip.decompress(data)
    return data


def fetch(url: str, tries=3):
    # 先试原地址；server.json 里配了 rewrite 时，再试替换后的地址。
    candidates = [url]
    alt = rewrite(url)
    if alt not in candidates:
        candidates.append(alt)
    last = None
    for i in range(tries):
        for u in candidates:
            try:
                return _get(u)
            except Exception as e:
                last = e
        time.sleep(1.5 * (i + 1))
    raise last


def load_index(path):
    if not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_index(path, index):
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(index, f, ensure_ascii=False, indent=1, sort_keys=True)
    os.replace(tmp, path)


def write_bytes(path, data):
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(data)
    os.replace(tmp, path)


def process(url: str, out_dir: str, force=False):
    real = rewrite(url)
    print("  下载", real)
    try:
        data = fetch(real)
    except Exception as e:
        print("  失败:", e)
        return None

    frames = skybp.parse_frames(data)
    if not frames:
        print("  警告：响应里没有可识别的 block（%d 字节）" % len(data))
        return None

    env = skybp.build_envelope(frames, source=real)
    stid = (env.get("block") or {}).get("stid") or (env.get("meta") or {}).get("stid")
    if not stid:
        stid = os.path.basename(real.split("?")[0]).rsplit(".", 1)[0] or "recording"

    bp_path = os.path.join(out_dir, stid + ".skyblueprint")
    nd_path = os.path.join(out_dir, stid + ".ndjson")

    if os.path.exists(bp_path) and not force:
        print("  已存在，跳过:", os.path.basename(bp_path))
        return {
            "stid": stid, "file": os.path.basename(bp_path),
            "skipped": True, "source": url, "real": real,
        }

    os.makedirs(out_dir, exist_ok=True)
    write_bytes(nd_path, data)
    skybp.pack(env, bp_path)

    errors, warnings = skybp.validate(env)
    s = env["summary"]
    m = env["meta"]
    print("  蓝图 -> %s" % bp_path)
    print("    kind=%s  duration=%ss  chunks=%d  notes=%d  placeables=%d"
          % (env["kind"], m.get("duration"), len(frames), s["notes"], s["placeables"]))
    for w in warnings:
        print("    warn:", w)
    if errors:
        print("    ERROR:", errors)

    return {
        "stid": stid,
        "file": os.path.basename(bp_path),
        "ndjson": os.path.basename(nd_path),
        "kind": env["kind"],
        "duration": m.get("duration"),
        "chunks": len(frames),
        "notes": s["notes"],
        "placeables": s["placeables"],
        "source": url,       # links.txt 里的原始地址，用于去重
        "real": real,        # 实际下载地址
        "created": env["created"],
        "ok": not errors,
    }


def read_links(paths):
    if isinstance(paths, str):
        paths = [paths]
    urls = []
    for path in paths:
        if not os.path.exists(path):
            continue
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                u = line.strip()
                if u and u not in urls:
                    urls.append(u)
    return urls


def emit(*parts):
    # 日志被重定向到文件时也要立即可见
    print(*parts, flush=True)


def report(entry):
    if entry is None:
        return
    if entry.get("skipped"):
        emit("  [skip] %s" % entry["stid"])
    else:
        emit("  [ok]   %s  notes=%s  %s"
             % (entry["stid"], entry.get("notes"), "OK" if entry.get("ok") else "有错误"))


def main():
    args = sys.argv[1:]
    once = "--once" in args
    force = "--force" in args
    out_dir = OUT_DIR
    links_paths = []
    for i, a in enumerate(args):
        if a == "--dir" and i + 1 < len(args):
            out_dir = args[i + 1]
        if a == "--links" and i + 1 < len(args):
            links_paths.append(args[i + 1])
    if not links_paths:
        links_paths = [LINKS]

    load_rewrite()
    os.makedirs(out_dir, exist_ok=True)
    index_path = os.path.join(out_dir, "index.json")
    index = load_index(index_path)

    # 只把「已经成功处理过」的 source 视为已处理；
    # 不能把 links.txt 里现有的所有链接都塞进 seen，否则一条都不会处理。
    seen = set()
    for rec in index.values():
        if isinstance(rec, dict) and rec.get("source"):
            seen.add(rec["source"])

    emit("监控 links.txt（Ctrl+C 停止）  ->", out_dir)
    for lp in links_paths:
        if os.path.exists(lp):
            emit("  %s: %d 条" % (lp, len(read_links(lp))))

    while True:
        attempted = set()
        try:
            for url in read_links(links_paths):
                if url in attempted:
                    continue
                if not force and (url in seen or rewrite(url) in seen):
                    continue
                attempted.add(url)
                emit("[新 link] %s" % url)
                entry = process(url, out_dir, force=force)
                if entry:
                    seen.add(url)
                    seen.add(rewrite(url))
                    if not entry.get("skipped"):
                        index[entry["stid"]] = entry
                        save_index(index_path, index)
                    report(entry)
            if once:
                break
            time.sleep(2)
        except KeyboardInterrupt:
            break
        except Exception as e:
            emit("  循环异常: %s" % e)
            time.sleep(3)

    emit("完成。共 %d 个蓝图。" % len(index))
    return 0


if __name__ == "__main__":
    sys.exit(main())
