#!/usr/bin/env python3
"""国服（网易）共享空间部署：加密请求 -> stage set / 新建空间入口。

国服的请求体是 RC4(session2) + Base64，包在 {"user":..., "body":"..."} 里；
响应按 content-encoding 解码：
    gzip                  -> gunzip
    session-valid         -> RC4(session2)
    gzip-session-valid    -> RC4(session2) 后 gunzip

依赖 mod 写在数据目录里的：
    session.json   {user, session, host, sky_level_id, session_id}
    session2.txt   login 响应里的 session2

用法:
    python cn_deploy.py <file.skyblueprint> [--create] [--dry-run]
"""
import argparse
import base64
import gzip
import json
import os
import sys
import time
import uuid
import urllib.error
import urllib.request

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import skybp  # noqa: E402

CN_UA = "Sky-Main-549608622/0.16.7.421709 (83DE; win 10.0.22631; zh-Hans)"
DATA = bpdata.data_dir("cn")
SESSION_FILE = os.path.join(DATA, "session.json")
SESSION2_FILE = os.path.join(DATA, "session2.txt")

DEFAULT_SESSION_ID = "2a6c40ec-4d07-4fbf-92ab-fc5cee970220"


def rc4(key: bytes, data: bytes) -> bytes:
    s = list(range(256))
    j = 0
    for i in range(256):
        j = (j + s[i] + key[i % len(key)]) & 0xFF
        s[i], s[j] = s[j], s[i]
    i = j = 0
    out = bytearray()
    for b in data:
        i = (i + 1) & 0xFF
        j = (j + s[i]) & 0xFF
        s[i], s[j] = s[j], s[i]
        out.append(b ^ s[(s[i] + s[j]) & 0xFF])
    return bytes(out)


def load_cfg():
    with open(SESSION_FILE, encoding="utf-8") as f:
        sess = json.load(f)
    with open(SESSION2_FILE, encoding="utf-8") as f:
        s2 = f.read().strip().encode()
    host = sess.get("host") or bpdata.server_host("cn")
    return sess, s2, host


def encrypt_body(user: str, s2: bytes, inner: dict) -> bytes:
    plain = json.dumps(inner, ensure_ascii=False, separators=(",", ":")).encode()
    enc = base64.b64encode(rc4(s2, plain)).decode()
    return json.dumps({"user": user, "body": enc}, separators=(",", ":")).encode()


def decode_response(headers, body: bytes, s2: bytes):
    enc = (headers.get("content-encoding") or "").lower()
    if enc == "gzip":
        return json.loads(gzip.decompress(body).decode("utf-8", "replace"))
    if enc == "session-valid":
        return json.loads(rc4(s2, body).decode("utf-8", "replace"))
    if enc == "gzip-session-valid":
        return json.loads(gzip.decompress(rc4(s2, body)).decode("utf-8", "replace"))
    try:
        return json.loads(body.decode("utf-8", "replace"))
    except Exception:
        return {"raw_len": len(body)}


def api(host, path, sess, s2, inner, dry_run=False):
    user = sess["user"]
    head = {
        "Content-Type": "application/json",
        "User-Agent": CN_UA,
        "X-Session-ID": sess.get("session_id") or DEFAULT_SESSION_ID,
        "trace-id": uuid.uuid4().hex[:7],
        "user-id": user,
        "session": sess["session"],
        "x-user-id": user,
        "x-session-token": sess["session"],
        "x-sky-level-id": sess.get("sky_level_id") or "1509719447",
    }
    body = encrypt_body(user, s2, inner)
    if dry_run:
        print("  [dry-run] POST %s  inner=%d bytes  body=%d bytes"
              % (path, len(json.dumps(inner, separators=(",", ":"))), len(body)))
        return {"status": "OK", "_http": 200}
    req = urllib.request.Request("https://" + host + path, data=body,
                                 method="POST", headers=head)
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            raw = r.read()
            code = r.status
            hdrs = {k.lower(): v for k, v in r.headers.items()}
    except urllib.error.HTTPError as e:
        raw = e.read()
        code = e.code
        hdrs = {k.lower(): v for k, v in e.headers.items()}
    obj = decode_response(hdrs, raw, s2)
    if isinstance(obj, dict):
        obj["_http"] = code
    else:
        obj = {"_http": code, "data": obj}
    return obj


def normalize_props(props):
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
            "name": p["name"],
            "ori": p.get("ori", "(0,0,0,1)"),
            "pos": p.get("pos", "(0,0,0)"),
            "scale": p.get("scale", "(1,1,1)"),
            "type": p.get("type", "unlock"),
            "uniqueID": uid,
            "userDataBool": bool(p.get("userDataBool", False)),
            "dye": p.get("dye", ""),
            "version": p.get("version", 0),
        })
    return out


def get_stage(host, sess, s2, stage_id, level, dry_run=False):
    inner = {"user": sess["user"], "user_id": sess["user"],
             "session": sess["session"], "stage_id": stage_id,
             "level_id": level, "updated_at": ""}
    return api(host, "/service/stage/api/v1/get", sess, s2, inner, dry_run)


def deploy_space(blueprint, create=False, dry_run=False):
    env = skybp.load(blueprint)
    doc = env.get("space")
    if not isinstance(doc, dict) or not doc.get("props"):
        print("失败：蓝图里没有 space/props（不是共享空间）")
        return 1

    sess, s2, host = load_cfg()
    stage_id = doc.get("stage_id") or ""
    level = doc.get("level_id") or ""
    props = normalize_props(doc.get("props") or [])
    print("蓝图   : %s" % blueprint)
    print("空间   : %s / %s  props=%d" % (stage_id, level, len(props)))

    cur = get_stage(host, sess, s2, stage_id, level, dry_run)
    seq = doc.get("sequence") or 1
    ver = doc.get("version") or 0
    if cur.get("_http") == 200 and isinstance(cur.get("sequence"), (int, float)):
        seq = int(cur["sequence"])
        ver = int(cur.get("version") or 0)
        print("  服务器当前: sequence=%s props=%d"
              % (seq, len(cur.get("props") or [])))

    inner = {
        "user": sess["user"], "user_id": sess["user"],
        "session": sess["session"],
        "sequence": seq, "version": ver,
        "stage_id": stage_id, "level_id": level,
        "challenge_duration": doc.get("challenge_duration", -1),
        "challenge_max_wings": doc.get("challenge_max_wings", -1),
        "show_global_radar": bool(doc.get("challenge_show_goal_radar",
                                          doc.get("show_global_radar", True))),
        "updatedAt": "",
        "props": props,
    }
    res = api(host, "/service/stage/api/v1/set", sess, s2, inner, dry_run)
    print("  set -> HTTP %s  %s" % (res.get("_http"),
                                    json.dumps(res, ensure_ascii=False)[:200]))
    if res.get("_http") != 200:
        return 1

    if create:
        feed = {
            "user": sess["user"], "user_id": sess["user"],
            "session": sess["session"],
            "pool_type": "SharedSpaceShrine",
            "pool_name": level,
            "message": stage_id,
            "private": False,
            "comments_enabled": False,
            "children_enabled": False,
            "do_payment": False,
            "return_buff": "false",
            "extra_data": {
                "ori": "(0,0,0,1)",
                "scale": "(1,1,1)",
                "resource_id": "",
                "prompt_used": "",
                "sub_type": 0,
                "chunk_count": 0,
                "user_id": "00000000-0000-0000-0000-000000000000",
                "stage_locator": "%s;%s" % (level, stage_id),
                "mc_torch_type": 0,
            },
        }
        fres = api(host, "/service/social-feed/api/v1/create", sess, s2, feed, dry_run)
        print("  feed -> HTTP %s  %s" % (fres.get("_http"),
                                         json.dumps(fres, ensure_ascii=False)[:200]))

    if not dry_run:
        got = get_stage(host, sess, s2, stage_id, level)
        names = [p.get("name") for p in (got.get("props") or []) if isinstance(p, dict)]
        print("  get -> %d props: %s" % (len(names), ", ".join(names[:6])))
    return 0


def deploy_recording(blueprint, level="", pool_type="", pool_name="",
                     message="Blueprint", dry_run=False):
    """国服留影：create -> update chunks -> social-feed create。"""
    env = skybp.load(blueprint)
    chunks = env.get("chunks") or ([env["block"]] if env.get("block") else [])
    if not chunks:
        print("失败：蓝图里没有分块")
        return 1

    sess, s2, host = load_cfg()
    level = level or (env.get("meta") or {}).get("level_id") or ""
    if not level:
        print("失败：缺少 level_id（面板里的 Level 字段）")
        return 1
    pool_type = pool_type or "RecordingShrine"
    if not pool_name:
        # 神坛的神龛场景名：MainStreet 下是 MainStreetAviaryMemoryShrine
        if pool_type == "RecordingShrine" and level == "MainStreet":
            pool_name = "MainStreetAviaryMemoryShrine"
        else:
            pool_name = level
    new_stid = str(uuid.uuid4())
    prepared = []
    for c in chunks:
        d = json.loads(json.dumps(c, ensure_ascii=False))
        d.setdefault("ssu", "")
        d["stid"] = new_stid
        prepared.append(d)

    print("蓝图   : %s" % blueprint)
    print("留影   : %s / %s   chunks=%d" % (pool_type, level, len(prepared)))

    base = {"user": sess["user"], "user_id": sess["user"], "session": sess["session"]}
    res = api(host, "/service/recording/api/v2/create", sess, s2, dict(
        base, recording_id="", level_id=level, pool_type=pool_type,
        recording_end=0, recording_data=prepared[0]), dry_run)
    print("  create -> HTTP %s  %s" % (res.get("_http"),
                                       json.dumps(res, ensure_ascii=False)[:200]))
    rid = res.get("recording_id") or ""
    if not rid:
        print("失败：create 没有返回 recording_id")
        return 1
    print("  recording_id=%s" % rid)

    for i in range(1, len(prepared)):
        upd = dict(base, recording_id=rid, chunk=i,
                   recording_end=(1 if i == len(prepared) - 1 else 0),
                   recording_data=prepared[i])
        r = api(host, "/service/recording/api/v2/update", sess, s2, upd, dry_run)
        if r.get("_http") != 200:
            print("  update %d -> HTTP %s  %s"
                  % (i, r.get("_http"), json.dumps(r, ensure_ascii=False)[:200]))
            return 1
    print("  update 1..%d OK" % (len(prepared) - 1))

    feed = dict(base, pool_type=pool_type, pool_name=pool_name,
                message=message, private=False, comments_enabled=True,
                children_enabled=True, do_payment=False, return_buff="false",
                extra_data={
                    "ori": "(0,0,0,1)",
                    "scale": "(1,1,1)",
                    "resource_id": rid,
                    "prompt_used": "",
                    "sub_type": 0,
                    "chunk_count": len(prepared),
                    "p0id": sess["user"],
                    "user_id": "00000000-0000-0000-0000-000000000000",
                    "mc_torch_type": 0,
                    "personality_tag": 1,
                })
    fres = api(host, "/service/social-feed/api/v1/create", sess, s2, feed, dry_run)
    print("  feed -> HTTP %s  %s" % (fres.get("_http"),
                                     json.dumps(fres, ensure_ascii=False)[:200]))
    return 0 if fres.get("_http") == 200 else 1


def deploy_message(blueprint, message_text="", dry_run=False):
    """国服留言（含打坐神坛 SeasonMeditationShrineNormal）。"""
    env = skybp.load(blueprint)
    msg = env.get("message")
    if not isinstance(msg, dict):
        print("失败：蓝图里没有 message 段")
        return 1
    text = message_text or msg.get("text") or ""
    if not text.strip():
        print("失败：留言文本为空")
        return 1
    pool_type = msg.get("pool_type") or "MessageStone"
    pool_name = msg.get("level") or ""

    sess, s2, host = load_cfg()
    inner = {
        "user": sess["user"], "user_id": sess["user"],
        "session": sess["session"],
        "pool_type": pool_type,
        "pool_name": pool_name,
        "message": text,
        "private": False,
        "comments_enabled": True,
        "children_enabled": False,
        "do_payment": False,
        "return_buff": "false",
        "extra_data": {
            "ori": msg.get("ori") or "(0,0,0,1)",
            "scale": "(1,1,1)",
            "resource_id": "",
            "prompt_used": "",
            "sub_type": 0,
            "chunk_count": 0,
            "user_id": "00000000-0000-0000-0000-000000000000",
            "mc_torch_type": 0,
        },
    }
    print("蓝图   : %s" % blueprint)
    print("留言   : %s / %s" % (pool_type, pool_name))
    print("文本   : %s" % text[:80].replace("\n", " "))
    res = api(host, "/service/social-feed/api/v1/create", sess, s2, inner, dry_run)
    print("  create -> HTTP %s  %s" % (res.get("_http"),
                                       json.dumps(res, ensure_ascii=False)[:200]))
    return 0 if res.get("_http") == 200 else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("blueprint")
    ap.add_argument("--recording", action="store_true", help="部署留影而不是共享空间")
    ap.add_argument("--create", action="store_true", help="共享空间：同时创建空间入口 feed")
    ap.add_argument("--level", default="")
    ap.add_argument("--pool-type", default="")
    ap.add_argument("--pool-name", default="")
    ap.add_argument("--message", default="Blueprint")
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    if args.recording:
        return deploy_recording(args.blueprint, args.level, args.pool_type,
                                args.pool_name, args.message, args.dry_run)
    env = skybp.load(args.blueprint)
    if isinstance(env.get("message"), dict):
        return deploy_message(args.blueprint, args.message, args.dry_run)
    return deploy_space(args.blueprint, args.create, args.dry_run)


if __name__ == "__main__":
    sys.exit(main())
