#!/usr/bin/env python3
"""把留言蓝图发布成一条新的留言。

接口（实测 0.35.2）：
    POST /service/social-feed/api/v1/create
    {
      "user","user_id","session",
      "pool_type":"MessageBoat",            # 或 MessageStone
      "pool_name":"Prairie_ButterflyFields",
      "message":"文本",
      "private":false,"comments_enabled":true,"children_enabled":false,
      "do_payment":true,
      "extra_data":{
        "pos":"(x,y,z)","ori":"(x,y,z,w)","scale":"(1,1,1)",
        "resource_id":"","prompt_used":"","sub_type":0,"chunk_count":0,
        "user_id":"00000000-0000-0000-0000-000000000000","personality_tag":1
      }
    }

用法:
    python message_deploy.py <file.skyblueprint> [--pos "(x,y,z)"] [--message 文本]
    python message_deploy.py <file.skyblueprint> --dry-run
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import skybp  # noqa: E402
import stage_deploy  # noqa: E402


def build_payload(msg, user, session, text, pos):
    return {
        "user": user,
        "user_id": user,
        "session": session,
        "pool_type": msg.get("pool_type") or "MessageStone",
        "pool_name": msg.get("level") or "",
        "message": text,
        "private": False,
        "comments_enabled": True,
        "children_enabled": False,
        "do_payment": True,
        "extra_data": {
            "pos": pos,
            "ori": msg.get("ori") or "(0,0,0,1)",
            "scale": "(1,1,1)",
            "resource_id": "",
            "prompt_used": "",
            "sub_type": 0,
            "chunk_count": 0,
            "user_id": "00000000-0000-0000-0000-000000000000",
            "personality_tag": 1,
        },
    }


def deploy(blueprint, host="", dry_run=False, pos_override="", message_override=""):
    env = skybp.load(blueprint)
    msg = env.get("message")
    if not isinstance(msg, dict):
        print("失败：蓝图里没有 message 段（不是留言蓝图）")
        return 1

    text = message_override or msg.get("text") or ""
    if not text.strip():
        print("失败：留言文本为空")
        return 1
    pos = pos_override or msg.get("pos") or ""
    if not pos:
        print("失败：缺少位置（蓝图里没有 pos，用 --pos 指定）")
        return 1

    sess = stage_deploy.load_session()
    user, session = sess.get("user"), sess.get("session")
    if not user or not session:
        print("失败：没有 session（先打开一次留影/留言，让 mod 抓取）")
        return 1

    base = stage_deploy.resolve_host(host)
    if not base:
        print("失败：缺少服务器地址（server.txt / SKY_SERVER / --host）")
        return 1
    if not base.startswith("http"):
        base = "https://" + base

    payload = build_payload(msg, user, session, text, pos)
    print("蓝图   : %s" % blueprint)
    print("留言   : %s / %s" % (payload["pool_type"], payload["pool_name"]))
    print("文本   : %s" % text[:80].replace("\n", " "))
    print("位置   : %s" % pos)

    res = stage_deploy.api(base, "/service/social-feed/api/v1/create",
                           payload, user, session, dry_run)
    print("  create -> HTTP %s  %s"
          % (res.get("_http"), json.dumps(res, ensure_ascii=False)[:300]))
    return 0 if res.get("_http") == 200 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blueprint")
    ap.add_argument("--host", default="")
    ap.add_argument("--pos", default="")
    ap.add_argument("--message", default="")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    return deploy(args.blueprint, args.host, args.dry_run, args.pos, args.message)


if __name__ == "__main__":
    sys.exit(main())
