#!/usr/bin/env python3
"""把一个 .skyblueprint 部署成新的游戏内留影（recording + social feed）。

流程（全部走服务器 HTTP API，不需要在游戏里重新录制）：
  1. POST /service/recording/api/v2/create   上传第 0 块，创建 recording_id
  2. POST /service/recording/api/v2/update   逐块上传剩余分块
  3. POST /service/social-feed/api/v1/create 创建 feed，让留影出现在世界里

用法:
    python deploy.py <file.skyblueprint> --user USER --session SESS \
        --level Prairie_ButterflyFields --pool-type RecordingCandle \
        --pos "(111.683,196.041,-431.1)" --ori "(0,0.687,0,0.726)" \
        --message "Blueprint Test"

    --dry-run   只打印请求，不发送
    --limit N   只上传前 N 块（调试）
    --no-feed   只创建 recording，不创建 social feed

session 也可以放在 tool/session.json（{"user": "...", "session": "..."}）。
"""
import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.request
import uuid

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

# 服务器地址不写死：用 --host 或环境变量 SKY_SERVER 指定（例如 https://your.server）
BASE = os.environ.get("SKY_SERVER", "").rstrip("/")
UA = "Sky-Live-com.tgc.sky.win/0.35.2.426499 (83DE; win 10.0.22631; zh-Hans)"
X_LEVEL = "1509719447"
X_SESSION_ID = ""
SESSION_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "session.json")
DEFAULT_FEED_META = os.path.join(bpdata.data_dir("steam"), "feed_meta.json")


def load_feed_meta(path):
    if not path or not os.path.exists(path):
        return {}
    try:
        with open(path, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def source_recording_id(env):
    """源蓝图对应的 recording id（link 文件名的 UUID）。"""
    src = env.get("source") or ""
    if not src:
        return ""
    base = os.path.basename(src.split("?")[0])
    return base.rsplit(".", 1)[0]


def load_session(args):
    if args.user and args.session:
        return args.user, args.session
    if os.path.exists(SESSION_FILE):
        with open(SESSION_FILE, encoding="utf-8") as f:
            d = json.load(f)
        return d.get("user"), d.get("session")
    return None, None


def api(path, payload, user, session, dry_run=False, verbose=False):
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    if dry_run:
        print("  [dry-run] POST %s  %d bytes" % (path, len(body)))
        return {"status": "OK", "_http": 200,
                "recording_id": payload.get("recording_id", ""),
                "link": "(dry-run)", "social_feed_id": "(dry-run)",
                "seek_table": {}}
    req = urllib.request.Request(
        BASE + path, data=body, method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": UA,
            "user-id": user,
            "x-user-id": user,
            "session": session,
            "x-session-token": session,
            "X-Session-ID": X_SESSION_ID or str(uuid.uuid4()),
            "x-sky-level-id": X_LEVEL,
        })
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            txt = r.read().decode("utf-8", "replace")
            code = r.status
    except urllib.error.HTTPError as e:
        txt = e.read().decode("utf-8", "replace")
        code = e.code

    if verbose or code != 200:
        print("  HTTP %d  %s" % (code, txt[:600]))
    try:
        obj = json.loads(txt)
    except Exception:
        obj = {"status": "error", "raw": txt}
    obj["_http"] = code
    return obj


def deploy_blueprint(blueprint, user, session, level="Prairie_ButterflyFields",
                     pool_type="RecordingCandle",
                     pos="(111.683,196.041,-431.1)", ori="(0,0.687,0,0.726)",
                     message="Blueprint", dry_run=False, limit=0, no_feed=False,
                     verbose=False, keep_stid=False,
                     p0id=None, p1id=None, feed_meta_path=None, log=print):
    """部署一个蓝图，返回结果 dict。"""
    env = skybp.load(blueprint)
    chunks = env.get("chunks") or [env.get("block")]
    if limit and limit > 0:
        chunks = chunks[:limit]
    notes = (env.get("summary") or {}).get("notes", 0)

    # 多人留影：从源 feed 元数据取所有 p0id/p1id/p2id/...，否则其他玩家不渲染
    src_rec = source_recording_id(env)
    fm = load_feed_meta(feed_meta_path or DEFAULT_FEED_META)
    feed_meta = fm.get(src_rec) or {}

    pids = {}
    for k, v in feed_meta.items():
        if re.fullmatch(r"p\d+id", str(k)) and isinstance(v, str) and v:
            pids[k] = v
    if p0id:
        pids["p0id"] = p0id
    if p1id:
        pids["p1id"] = p1id
    pids.setdefault("p0id", user)

    new_stid = (env.get("block") or {}).get("stid") or str(uuid.uuid4())
    if not keep_stid:
        new_stid = str(uuid.uuid4())

    log("蓝图: %s  kind=%s  chunks=%d  notes=%s  stid=%s%s"
        % (blueprint, env.get("kind"), len(chunks), notes, new_stid,
           "" if keep_stid else " (新生成)"))

    prepared = []
    for c in chunks:
        d = json.loads(json.dumps(c, ensure_ascii=False))
        d.setdefault("ssu", "")
        if not keep_stid:
            d["stid"] = new_stid
        prepared.append(d)

    recording_id = str(uuid.uuid4())
    log("[1/3] create recording  recording_id=%s" % recording_id)
    resp = api("/service/recording/api/v2/create", {
        "user": user,
        "user_id": user,
        "session": session,
        "recording_id": recording_id,
        "level_id": level,
        "pool_type": pool_type,
        "recording_data": prepared[0],
    }, user, session, dry_run, verbose)

    if resp.get("_http") != 200 or resp.get("status") != "OK":
        return {"ok": False, "step": "create",
                "error": json.dumps(resp, ensure_ascii=False)[:800]}

    rid = resp.get("recording_id") or recording_id
    log("      OK  recording_id=%s" % rid)
    log("      link=%s" % resp.get("link", ""))

    log("[2/3] upload chunks 1..%d" % (len(prepared) - 1))
    for i in range(1, len(prepared)):
        r = api("/service/recording/api/v2/update", {
            "user": user,
            "user_id": user,
            "session": session,
            "chunk": i,
            "recording_id": rid,
            "recording_data": prepared[i],
        }, user, session, dry_run, verbose)
        if r.get("_http") != 200 or r.get("status") != "OK":
            return {"ok": False, "step": "update:%d" % i,
                    "recording_id": rid,
                    "error": json.dumps(r, ensure_ascii=False)[:500]}
        if i % 10 == 0 or i == len(prepared) - 1:
            log("      chunk %d/%d OK" % (i, len(prepared) - 1))
        time.sleep(0.05)

    log("[3/3] 校验 recording")
    got = api("/service/recording/api/v2/get", {
        "user": user, "user_id": user, "session": session,
        "recording_id": rid, "level_id": level,
    }, user, session, dry_run, verbose)
    seek_chunks = 0
    if got.get("_http") == 200 and got.get("status") == "OK":
        table = got.get("seek_table") or {}
        seek_chunks = len(table)
        log("      seek_table chunks=%d  link=%s"
            % (seek_chunks, got.get("link", "")))
    else:
        log("      校验失败: %s" % json.dumps(got, ensure_ascii=False)[:400])

    if no_feed:
        return {"ok": True, "recording_id": rid, "social_feed_id": "",
                "chunks": len(prepared), "notes": notes, "stid": new_stid,
                "seek_chunks": seek_chunks}

    log("[3/3] create social feed")
    extra = {
        "pos": pos,
        "ori": ori,
        "scale": "(1,1,1)",
        "resource_id": rid,
        "prompt_used": "",
        "sub_type": 0,
        "chunk_count": len(prepared),
        "user_id": "00000000-0000-0000-0000-000000000000",
    }
    extra.update(pids)

    feed = api("/service/social-feed/api/v1/create", {
        "user": user,
        "user_id": user,
        "session": session,
        "pool_type": pool_type,
        "pool_name": level,
        "message": message,
        "private": False,
        "comments_enabled": True,
        "children_enabled": True,
        "do_payment": True,
        "tags": ["none"],
        "extra_data": extra,
    }, user, session, dry_run, verbose)

    if feed.get("_http") != 200:
        return {"ok": False, "step": "feed", "recording_id": rid,
                "error": json.dumps(feed, ensure_ascii=False)[:500]}

    feed_id = feed.get("social_feed_id", "")
    log("      social_feed_id=%s" % feed_id)
    return {"ok": True, "recording_id": rid, "social_feed_id": feed_id,
            "chunks": len(prepared), "notes": notes, "stid": new_stid,
            "seek_chunks": seek_chunks, "pos": pos, "level": level}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blueprint")
    ap.add_argument("--user")
    ap.add_argument("--session")
    ap.add_argument("--level", default="Prairie_ButterflyFields")
    ap.add_argument("--pool-type", default="RecordingCandle")
    ap.add_argument("--pos", default="(111.683,196.041,-431.1)")
    ap.add_argument("--ori", default="(0,0.687,0,0.726)")
    ap.add_argument("--message", default="Blueprint")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--no-feed", action="store_true")
    ap.add_argument("--keep-stid", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--host", default="", help="服务器域名，如 your.server（也可用 SKY_SERVER）")
    ap.add_argument("--x-sky-level-id", default="1509719447")
    ap.add_argument("--session-id", default="")
    ap.add_argument("--ua", default="")
    ap.add_argument("--p0id", default="")
    ap.add_argument("--p1id", default="")
    ap.add_argument("--feed-meta", default=DEFAULT_FEED_META)
    args = ap.parse_args()

    global BASE, UA, X_LEVEL, X_SESSION_ID
    if args.host:
        BASE = "https://" + args.host
    if not BASE:
        print("缺少服务器地址：用 --host your.server 或设置环境变量 SKY_SERVER")
        return 2
    if args.ua:
        UA = args.ua
    X_LEVEL = args.x_sky_level_id
    X_SESSION_ID = args.session_id

    user, session = load_session(args)
    if not user or not session:
        print("缺少 --user / --session（或 tool/session.json）")
        return 2

    res = deploy_blueprint(
        args.blueprint, user, session,
        level=args.level, pool_type=args.pool_type,
        pos=args.pos, ori=args.ori, message=args.message,
        dry_run=args.dry_run, limit=args.limit, no_feed=args.no_feed,
        verbose=args.verbose, keep_stid=args.keep_stid,
        p0id=args.p0id or None, p1id=args.p1id or None,
        feed_meta_path=args.feed_meta)
    if not res.get("ok"):
        print("FAILED: %s" % res.get("error"))
        return 1
    print("\n完成。")
    print("  recording_id : %s" % res["recording_id"])
    print("  social_feed  : %s" % res["social_feed_id"])
    print("  chunks/notes : %s / %s" % (res["chunks"], res["notes"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
