#!/usr/bin/env python3
"""把 mod 抓到的留言 feed 转成 .skyblueprint。

留言（MessageStone / MessageBoat / ...）没有 recording，内容就是 social feed 本身：

    {"social_feed_id":"<uuid>:<hash>","pool_type":"MessageBoat",
     "pool_name":"CandleSpace","level_id":"CandleSpace","author_id":"<uuid>",
     "message":"文本","content":"文本",
     "extra_data":{"pos":"(x,y,z)","ori":"(x,y,z,w)", ...},
     "created_at":"...","expire_at":"..."}

mod 把 feed 存到 <数据目录>/messages.json，把用户打开过的留言记到
message_views.json；本脚本只为「看过」的留言生成蓝图。

蓝图结构：
    kind = "message"
    message = {text, pool_type, level, pos, ori, author, social_feed_id, ...}
    block   = {ver, stid}   （占位，让面板/列表能解析）

用法:
    python message_import.py            # 持续监控
    python message_import.py --once
    python message_import.py --list     # 列出抓到的留言
"""
import glob
import json
import os
import sys
import time
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

# SKY_EDITION=cn 时读国服数据目录
EDITION = os.environ.get("SKY_EDITION", "")
DATA = bpdata.data_dir(EDITION or "steam")
MESSAGES = os.path.join(DATA, "messages.json")
VIEWS = os.path.join(DATA, "message_views.json")
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if EDITION:
    OUT = os.path.join(DATA, "blueprints")
else:
    OUT = os.path.join(ROOT, "samples", "fetched")   # 与 mod 面板默认目录一致
INDEX = os.path.join(OUT, "message_index.json")

NS = uuid.UUID("6f9619ff-8b86-d011-b42d-00c04fc964ff")


def load_json(path, default):
    try:
        with open(path, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else default
    except Exception:
        return default


def base_id(sid):
    """social_feed_id 在 feed 里是 <uuid>:<hash>，请求里只带 <uuid>。"""
    return (sid or "").split(":")[0]


def short_feed_id(sid):
    return base_id(sid)[:8]


def message_text(feed):
    for key in ("content", "message", "msg", "text"):
        v = feed.get(key)
        if isinstance(v, str) and v.strip():
            return v
    return ""


def extra(feed, key, default=""):
    ed = feed.get("extra_data")
    if isinstance(ed, dict):
        v = ed.get(key)
        if isinstance(v, str) and v:
            return v
    return default


def build_blueprint(sid, feed):
    text = message_text(feed)
    pool = feed.get("pool_type") or "MessageStone"
    level = feed.get("level_id") or feed.get("pool_name") or ""
    author = feed.get("author_id") or ""
    if not author and isinstance(feed.get("user_id"), str):
        author = feed["user_id"].split(":")[0]
    pos = extra(feed, "pos")
    ori = extra(feed, "ori")
    stid = str(uuid.uuid5(NS, "message/%s" % sid))

    block = {"ver": 1, "stid": stid, "plyr": []}
    env = skybp.build_envelope(
        [block],
        kind="message",
        source="feed://%s/%s/%s/%s" % (pool, level, author, sid),
        pool_type=pool,
        extra_meta={
            "level_id": level,
            "pos": pos,
            "ori": ori,
            "author_id": author,
            "social_feed_id": sid,
            "created_at": feed.get("create_at"),
            "expire_at": feed.get("expire_at"),
        },
    )
    env["message"] = {
        "text": text,
        "pool_type": pool,
        "level": level,
        "pos": pos,
        "ori": ori,
        "author_id": author,
        "social_feed_id": sid,
        "created_at": feed.get("create_at"),
        "expire_at": feed.get("expire_at"),
    }
    env["summary"]["chars"] = len(text)

    name = "message_%s_%s_%s.skyblueprint" % (
        pool, (author[:8] or "unknown"), short_feed_id(sid))
    skybp.pack(env, os.path.join(OUT, name))
    return name, text


def list_messages():
    msgs = load_json(MESSAGES, {})
    views = load_json(VIEWS, {})
    for sid, feed in msgs.items():
        if not isinstance(feed, dict):
            continue
        text = message_text(feed)
        mark = "*" if (base_id(sid) in views or sid in views) else " "
        print("%s %-14s %-16s %s"
              % (mark, feed.get("pool_type", "?"), feed.get("author_id", "")[:8],
                 text[:60].replace("\n", " ")))


def process_once():
    msgs = load_json(MESSAGES, {})
    views = load_json(VIEWS, {})
    index = load_json(INDEX, {})
    os.makedirs(OUT, exist_ok=True)
    changed = 0
    for sid, feed in msgs.items():
        if not isinstance(feed, dict):
            continue
        view_ts = views.get(base_id(sid)) or views.get(sid)
        if not view_ts:
            continue
        text = message_text(feed)
        if not text:
            continue
        # SocialFeedPenpal / SocialFeedComment 不是留言
        if str(feed.get("pool_type") or "").startswith("SocialFeed"):
            continue
        sig = [view_ts, text, feed.get("create_at")]
        if index.get(sid) == sig:
            continue
        try:
            name, _ = build_blueprint(sid, feed)
            index[sid] = sig
            changed += 1
            print("wrote %s  %s" % (name, text[:40].replace("\n", " ")), flush=True)
        except Exception as e:
            print("FAIL %s: %s" % (sid, e), flush=True)
    if changed:
        tmp = INDEX + ".tmp"
        with open(tmp, "w", encoding="utf-8", newline="\n") as f:
            json.dump(index, f, ensure_ascii=False, indent=1, sort_keys=True)
        os.replace(tmp, INDEX)
    return changed


def main():
    args = sys.argv[1:]
    if "--list" in args:
        list_messages()
        return 0
    once = "--once" in args
    print("留言监控:", MESSAGES, "->", OUT, flush=True)
    while True:
        try:
            process_once()
        except Exception as e:
            print("loop error:", e, flush=True)
        if once:
            break
        time.sleep(2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
