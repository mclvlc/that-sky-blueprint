#!/usr/bin/env python3
"""部署守护进程：监听 mod 写出的 deploy_queue.json，自动部署蓝图。

游戏内点击 Blueprints 标签页的 Deploy 后，mod 会写：
    %APPDATA%/sky-blueprint/steam/deploy_queue.json  {blueprint, message, pos, ori, level?, pool_type?}
    %APPDATA%/sky-blueprint/steam/session.json       由 mod 从出站请求自动提取 {user, session, level, pool_type}

本进程处理后写：
    %APPDATA%/sky-blueprint/steam/deploy_result.txt   一行人类可读结果（mod UI 显示）
    %APPDATA%/sky-blueprint/steam/deploy_result.json  结构化结果

用法:
    python deployd.py            # 持续监听
    python deployd.py --once     # 处理完当前队列就退出
"""
import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpdata  # noqa: E402
import deploy  # noqa: E402

# mod 现在把队列/session/结果写到数据目录，旧位置仅作迁移期回退
DATA_DIR = bpdata.data_dir("steam")
QUEUE = os.path.join(DATA_DIR, "deploy_queue.json")
SESSION = os.path.join(DATA_DIR, "session.json")
RESULT_TXT = os.path.join(DATA_DIR, "deploy_result.txt")
RESULT_JSON = os.path.join(DATA_DIR, "deploy_result.json")


def emit(*parts):
    print(*parts, flush=True)


def read_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def write_result(res):
    txt = res.get("line", "ERROR")
    os.makedirs(DATA_DIR, exist_ok=True)
    with open(RESULT_TXT, "w", encoding="utf-8", newline="\n") as f:
        f.write(txt + "\n")
    with open(RESULT_JSON, "w", encoding="utf-8", newline="\n") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)


def stamp():
    return time.strftime("%Y-%m-%d %H:%M:%S")


def process_queue(dry_run=False):
    queue_path = QUEUE
    try:
        q = read_json(queue_path)
    except Exception as e:
        return False

    blueprint = q.get("blueprint") or ""
    message = q.get("message") or "Blueprint"
    pos = q.get("pos") or ""
    ori = q.get("ori") or "(0,0.687,0,0.726)"

    emit("[%s] 队列: %s" % (stamp(), blueprint))
    if not blueprint or not os.path.exists(blueprint):
        res = {"ok": False, "line": "%s  FAIL  blueprint not found: %s" % (stamp(), blueprint),
               "error": "blueprint not found"}
        write_result(res)
        os.remove(queue_path)
        return True

    sess_path = SESSION
    try:
        sess = read_json(sess_path)
    except Exception:
        sess = {}

    user = sess.get("user") or ""
    session = sess.get("session") or ""
    level = q.get("level") or sess.get("level") or "Prairie_ButterflyFields"
    pool_type = q.get("pool_type") or sess.get("pool_type") or "RecordingCandle"

    if not user or not session:
        res = {"ok": False,
               "line": "%s  FAIL  no session captured (open a recording, then retry)" % stamp(),
               "error": "no session"}
        write_result(res)
        os.remove(queue_path)
        emit(res["line"])
        return True

    emit("  user=%s level=%s pool=%s" % (user, level, pool_type))
    # Pos 为空时用蓝图自身的位置：角色首帧 > ppsp > 原点
    if not pos:
        try:
            env = deploy.skybp.load(blueprint)
            ghost = deploy.skybp.content_player(env.get("block") or {}) or {}
            sts = ghost.get("sts") or []
            p = ""
            if sts and isinstance(sts[0], list) and len(sts[0]) > 1:
                p = sts[0][1]
            if not p:
                p = (env.get("meta") or {}).get("ppsp") or ""
            if p:
                pos = p if p.startswith("(") else "(" + p + ")"
        except Exception:
            pass
        if not pos:
            pos = "(0,0,0)"

    try:
        res = deploy.deploy_blueprint(
            blueprint, user, session,
            level=level, pool_type=pool_type, pos=pos, ori=ori,
            message=message, dry_run=dry_run, log=lambda s: emit("  " + s))
    except Exception as e:
        res = {"ok": False, "line": "%s  FAIL  %s" % (stamp(), e), "error": repr(e)}
        write_result(res)
        os.remove(queue_path)
        emit(res["line"])
        return True

    if res.get("ok"):
        line = ("%s  OK  recording=%s  feed=%s  chunks=%s notes=%s"
                % (stamp(), res.get("recording_id"), res.get("social_feed_id"),
                   res.get("chunks"), res.get("notes")))
    else:
        line = "%s  FAIL  %s" % (stamp(), res.get("error", "unknown"))

    res["line"] = line
    write_result(res)
    try:
        os.remove(queue_path)
    except Exception:
        pass
    emit(line)
    return True


def main():
    once = "--once" in sys.argv
    dry_run = "--dry-run" in sys.argv
    emit("部署守护进程启动，监听:", QUEUE)
    if dry_run:
        emit("dry-run 模式：只打印请求，不写入服务器")
    emit("Ctrl+C 停止")
    while True:
        try:
            worked = process_queue(dry_run)
            if once and not worked:
                break
            if once:
                break
            time.sleep(1)
        except KeyboardInterrupt:
            break
        except Exception as e:
            emit("循环异常:", e)
            time.sleep(2)
    emit("退出")
    return 0


if __name__ == "__main__":
    sys.exit(main())
