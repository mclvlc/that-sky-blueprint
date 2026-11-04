#!/usr/bin/env python3
"""把共享空间蓝图写回服务器。

接口（实测 0.35.2）：
    POST /service/stage/api/v1/set
    {
      "user", "user_id", "session",
      "sequence": 1, "version": 0,
      "stage_id": "DuskStart_CNDL", "level_id": "DuskStart",
      "user_id": "<同 user>",              # 客户端会重复一次，服务端只认一个
      "challenge_duration": 0, "challenge_max_wings": -1,
      "show_global_radar": true,
      "updatedAt": "",
      "props": [
        {"version":0,"type":"unlock","name":"CharSkyKid_Prop_...","uniqueID":1,
         "pos":"(x,y,z)","ori":"(x,y,z,w)","scale":"(1,1,1)",
         "userDataBool":false,"dye":""}
      ]
    }

只能写自己的空间（user_id 与当前登录用户一致），因为服务器的 stage 是按
stage_id + 用户存的。

新建共享空间（世界里的那根蜡烛/空间入口）走的是 social-feed：

    POST /service/social-feed/api/v1/create
    {"pool_type":"SharedSpaceCandle","pool_name":"DuskStart","message":"...",
     "private":false,"comments_enabled":true,"children_enabled":false,
     "do_payment":true,
     "extra_data":{"pos":"(x,y,z)","ori":"(x,y,z,w)","scale":"(1,1,1)",
                   "resource_id":"","prompt_used":"","sub_type":0,
                   "chunk_count":0,"user_id":"00000000-..."}}

stage 数据按 stage_id + 用户存，所以“新建空间 + 部署蓝图” =
    1. 先 set props（stage/api/v1/set）
    2. 再 create SharedSpaceCandle feed（决定空间在世界里的位置）

用法:
    python stage_deploy.py <file.skyblueprint> [--host HOST] [--dry-run] [--force]
    python stage_deploy.py <file.skyblueprint> --verify      # 写完后再 get 一次核对
    python stage_deploy.py <file.skyblueprint> --create \
        --pos "(-88.6,64.4,-761.4)" --ori "(0,-0.42,0,0.91)" --message "测试空间"
"""
import argparse
import json
import os
import sys
import uuid
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

UA = "Sky-Live-com.tgc.sky.win/0.35.2.426499 (83DE; win 10.0.22631; zh-Hans)"
X_LEVEL = "1509719447"
TOOL_DIR = os.path.dirname(os.path.abspath(__file__))
CONFIG = os.path.join(TOOL_DIR, "server.json")
SESSION_FILE = os.path.join(bpdata.data_dir("steam"), "session.json")


def resolve_host(host):
    if host:
        return host
    env = os.environ.get("SKY_SERVER")
    if env:
        return env
    h = bpdata.server_host("steam")
    if h:
        return h
    try:
        with open(CONFIG, encoding="utf-8") as f:
            h = json.load(f).get("host")
        if h:
            return h
    except Exception:
        pass
    return ""


def api(base, path, payload, user, session, dry_run=False):
    body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode()
    if dry_run:
        print("  [dry-run] POST %s  %d bytes" % (path, len(body)))
        return {"status": "OK", "_http": 200}
    req = urllib.request.Request(
        base + path, data=body, method="POST",
        headers={
            "Content-Type": "application/json",
            "User-Agent": UA,
            "user-id": user,
            "x-user-id": user,
            "session": session,
            "x-session-token": session,
            "X-Session-ID": str(uuid.uuid4()),
            "x-sky-level-id": X_LEVEL,
        })
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            txt = r.read().decode("utf-8", "replace")
            code = r.status
    except urllib.error.HTTPError as e:
        txt = e.read().decode("utf-8", "replace")
        code = e.code
    try:
        obj = json.loads(txt)
    except Exception:
        obj = {"raw": txt}
    obj["_http"] = code
    return obj


def load_session():
    with open(SESSION_FILE, encoding="utf-8") as f:
        return json.load(f)


def normalize_props(props):
    """只保留 set 需要的字段；uniqueID 缺失/重复时自动补。"""
    out = []
    used = set()
    nxt = 1
    for p in props:
        if not isinstance(p, dict) or not p.get("name"):
            continue
        uid = p.get("uniqueID")
        if not isinstance(uid, int) or uid in used:
            while nxt in used:
                nxt += 1
            uid = nxt
        used.add(uid)
        out.append({
            "version": p.get("version", 0),
            "type": p.get("type", "unlock"),
            "name": p["name"],
            "uniqueID": uid,
            "pos": p.get("pos", "(0,0,0)"),
            "ori": p.get("ori", "(0,0,0,1)"),
            "scale": p.get("scale", "(1,1,1)"),
            "userDataBool": bool(p.get("userDataBool", False)),
            "dye": p.get("dye", ""),
        })
    return out


def build_payload(doc, user, session):
    props = normalize_props(doc.get("props") or [])
    return {
        "user": user,
        "user_id": user,
        "session": session,
        "sequence": int(doc.get("sequence") or 1),
        "version": int(doc.get("version") or 0),
        "stage_id": doc.get("stage_id") or "",
        "level_id": doc.get("level_id") or "",
        "challenge_duration": int(doc.get("challenge_duration") or 0),
        "challenge_max_wings": int(doc.get("challenge_max_wings") or -1),
        "show_global_radar": bool(doc.get("show_global_radar", True)),
        "updatedAt": "",
        "props": props,
    }


def create_feed(base, user, session, level, pos, ori, message, dry_run=False):
    """新建世界里的共享空间入口（SharedSpaceCandle）。"""
    payload = {
        "user": user,
        "user_id": user,
        "session": session,
        "pool_type": "SharedSpaceCandle",
        "pool_name": level,
        "message": message,
        "private": False,
        "comments_enabled": True,
        "children_enabled": False,
        "do_payment": True,
        "extra_data": {
            "pos": pos,
            "ori": ori,
            "scale": "(1,1,1)",
            "resource_id": "",
            "prompt_used": "",
            "sub_type": 0,
            "chunk_count": 0,
            "user_id": "00000000-0000-0000-0000-000000000000",
        },
    }
    return api(base, "/service/social-feed/api/v1/create", payload,
               user, session, dry_run)


def deploy(blueprint, host, dry_run=False, force=False, verify=False,
           create=False, pos="", ori="", message="Blueprint"):
    env = skybp.load(blueprint)
    doc = env.get("space")
    if not isinstance(doc, dict) or not doc.get("props"):
        print("失败：蓝图里没有 space/props（不是共享空间）")
        return 1

    sess = load_session()
    user, session = sess.get("user"), sess.get("session")
    if not user or not session:
        print("失败：没有 session（先打开一次留影或空间，让 mod 抓取）")
        return 1

    owner = doc.get("user_id") or ""
    if owner and owner != user and not force:
        print("失败：这是 %s 的空间，只能写自己的空间（--force 可强行尝试）" % owner)
        return 1

    stage_id = doc.get("stage_id") or ""
    level = doc.get("level_id") or ""
    payload = build_payload(doc, user, session)
    print("蓝图   : %s" % blueprint)
    print("空间   : %s / %s   props=%d   sequence=%s version=%s"
          % (stage_id, level, len(payload["props"]),
             payload["sequence"], payload["version"]))
    print("所有者 : %s" % (owner or user))

    base = resolve_host(host)
    if not base:
        print("失败：缺少服务器地址（--host 或环境变量 SKY_SERVER 或 tool/server.json）")
        return 1
    if not base.startswith("http"):
        base = "https://" + base

    # sequence/version 必须和服务器当前值一致（官服/私服各自计数）。
    # 蓝图里带的是来源服务器的值，直接发会被 400 Invalid Request。
    if not dry_run:
        cur = api(base, "/service/stage/api/v1/get",
                  {"user": user, "user_id": user, "session": session,
                   "stage_id": stage_id, "level_id": level, "updated_at": ""},
                  user, session)
        if cur.get("_http") == 200 and isinstance(cur.get("sequence"), (int, float)):
            old = payload["sequence"]
            payload["sequence"] = int(cur["sequence"])
            payload["version"] = int(cur.get("version") or 0)
            cur_props = len(cur.get("props") or [])
            print("  服务器当前: sequence=%s props=%d%s"
                  % (payload["sequence"], cur_props,
                     "" if old == payload["sequence"]
                     else "  (蓝图 %s)" % old))

    res = api(base, "/service/stage/api/v1/set", payload, user, session, dry_run)
    print("  set -> HTTP %s  %s" % (res.get("_http"), json.dumps(res, ensure_ascii=False)[:300]))
    if res.get("_http") != 200 or res.get("status") not in (None, "OK"):
        return 1

    if create:
        if not pos:
            print("失败：--create 需要 --pos（世界坐标）")
            return 1
        feed = create_feed(base, user, session, level or "DuskStart",
                           pos, ori or "(0,0,0,1)", message, dry_run)
        print("  feed -> HTTP %s  %s"
              % (feed.get("_http"), json.dumps(feed, ensure_ascii=False)[:300]))
        if feed.get("_http") != 200:
            return 1

    if verify and not dry_run:
        q = {"user": user, "user_id": user, "session": session,
             "stage_id": stage_id, "level_id": level, "updated_at": ""}
        got = api(base, "/service/stage/api/v1/get", q, user, session)
        props = got.get("props") or []
        names = [p.get("name") for p in props if isinstance(p, dict)]
        print("  get -> %d props: %s" % (len(names), ", ".join(names[:8])))
    return 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blueprint")
    ap.add_argument("--host", default="")
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--force", action="store_true")
    ap.add_argument("--verify", action="store_true")
    ap.add_argument("--create", action="store_true",
                    help="同时新建世界里的共享空间入口（SharedSpaceCandle）")
    ap.add_argument("--pos", default="", help="新空间的世界坐标 \"(x,y,z)\"")
    ap.add_argument("--ori", default="(0,0,0,1)")
    ap.add_argument("--message", default="Blueprint")
    args = ap.parse_args()
    return deploy(args.blueprint, args.host, args.dry_run, args.force, args.verify,
                  args.create, args.pos, args.ori, args.message)


if __name__ == "__main__":
    sys.exit(main())
