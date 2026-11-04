#!/usr/bin/env python3
"""skyblueprint —— 光遇留影 / 共享空间 / 留言蓝图的提取、校验、读写。

文件格式（v2）:
    SKYBP2\n
    {
      "format": "skyblueprint/2",
      "kind": "recording" | "shared_space" | "message" | "unknown",
      "created": "2026-11-04T12:00:00Z",
      "source": "https://.../recording/....ndjson",
      "meta": {
        "stid": "...", "ppsp": "(0,196.041,0)",
        "duration": 120.0, "chunks": 30, "chunk_span": 4.0,
        "derived_block": true
      },
      "summary": {
        "notes": 1872, "events": 1976, "anims": 0,
        "placeables": 1, "players": 2, "trajectory": 1200,
        "strs": 9, "key_min": 0, "key_max": 14
      },
      "block":  { ... },        # 跨分块合并后的游戏原生 block（派生，便于直接使用）
      "chunks": [ { ... } ],    # 服务器下发的原始分块，逐字节保留
      "integrity": {"algo": "sha256",
                    "chunks_sha256": "...", "block_sha256": "..."}
    }

兼容旧格式:
    SKYBP1\n + 单个游戏原生 block JSON

分块语义（实测 0.35.2）:
    ndjson 每一行是一个约 4 秒的分块，chet 为该分块结束时间；
    nts / anms 里的时间相对分块起点，因此合并时用
    absolute = 上一块的 chet + 相对时间。

用法:
    python skybp.py show     <file.skyblueprint>
    python skybp.py space    <file.skyblueprint> [--json]      # 共享空间物件列表
    python skybp.py validate <file.skyblueprint> [--quiet]
    python skybp.py list     <dir> [--json]
    python skybp.py pack     <input.ndjson|block.json> <out.skyblueprint> [--kind K] [--source URL] [--pool TYPE]
    python skybp.py unpack   <file.skyblueprint> <out.json>
    python skybp.py export   <file.skyblueprint> <out> --as block|chunks|ndjson|notes|csv
    python skybp.py align    <源.skyblueprint> <目标参考.skyblueprint> [输出] [--plac]
    python skybp.py extract  <raw_capture.bin> <outdir>
    python skybp.py selftest [sample.ndjson]
"""
import copy
import csv
import hashlib
import io
import json
import os
import re
import sys
import time
import uuid

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")
except Exception:
    pass

MAGIC_V1 = b"SKYBP1\n"
MAGIC_V2 = b"SKYBP2\n"
FORMAT_V2 = "skyblueprint/2"
BS = chr(92)

BLOCK_HINT_KEYS = ("stfs", "sts", "strs", "ofts", "nts", "anms", "plac", "plyr")


# ---------------------------------------------------------------------------
# 基础判断
# ---------------------------------------------------------------------------
def is_block(obj) -> bool:
    """是否像游戏原生的留影/场景 block。"""
    return isinstance(obj, dict) and any(k in obj for k in BLOCK_HINT_KEYS)


def is_uuid(s) -> bool:
    try:
        uuid.UUID(str(s))
        return True
    except Exception:
        return False


def _num(v, default=0.0) -> float:
    try:
        return float(v)
    except (TypeError, ValueError):
        return default


def canonical(obj) -> bytes:
    """稳定的 JSON 序列化，用于校验和。"""
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"),
                      sort_keys=False).encode("utf-8")


def sha256(obj) -> str:
    return hashlib.sha256(canonical(obj)).hexdigest()


def iso_now() -> str:
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


# ---------------------------------------------------------------------------
# 从捕获流中提取 block（保留旧功能）
# ---------------------------------------------------------------------------
def _balanced_json(text, start):
    """从 start 处的 '{' 开始，返回配平的 JSON 子串（跳过字符串内的括号）。"""
    depth = 0
    in_str = False
    esc = False
    for i in range(start, min(len(text), start + 4 * 1024 * 1024)):
        c = text[i]
        if in_str:
            if esc:
                esc = False
            elif c == BS:
                esc = True
            elif c == '"':
                in_str = False
            continue
        if c == '"':
            in_str = True
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[start:i + 1]
    return None


def extract_blocks(raw: bytes):
    """从抓包流中找出所有留影/场景 block。"""
    text = raw.decode("latin-1")
    out = []
    seen = set()
    for m in re.finditer(r'\{"ver":', text):
        frag = _balanced_json(text, m.start())
        if not frag or len(frag) < 64:
            continue
        try:
            obj = json.loads(frag.encode("latin-1").decode("utf-8", "replace"))
        except Exception:
            continue
        if not any(k in obj for k in BLOCK_HINT_KEYS):
            continue
        key = (obj.get("stid", ""), len(frag))
        if key in seen:
            continue
        seen.add(key)
        out.append(obj)
    return out


# ---------------------------------------------------------------------------
# 分块解析 / 合并
# ---------------------------------------------------------------------------
def content_player(block):
    """返回承载内容的玩家对象。

    不能只取「第一个有 nts/anms/plac 的」：录制者(plyr[0])偶尔也有几条
    anms，会抢在回放幽灵(plyr[1])前面。按事件数量打分取最高的那个。
    """
    players = block.get("plyr") if isinstance(block, dict) else None
    if not isinstance(players, list):
        return None

    best = None
    best_score = -1
    for p in players:
        if not isinstance(p, dict):
            continue
        score = (3 * len(_event_list(p.get("nts")))
                 + 2 * len(_event_list(p.get("anms")))
                 + len(_event_list(p.get("plac"))))
        if score > best_score:
            best = p
            best_score = score
    if best is not None:
        return best
    for p in reversed(players):
        if isinstance(p, dict):
            return p
    return None


def _event_list(value):
    return value if isinstance(value, list) else []


def is_placeable(e) -> bool:
    """plac 里混有 [1, time] 这类非物件事件，只有带非空资源名的才是物件。"""
    return (isinstance(e, list) and len(e) >= 4
            and isinstance(e[2], str) and e[2] != "")


def _merge_events(frames, bases, pi, key):
    """把各分块里某个玩家的 nts/anms 合并成绝对时间事件表。"""
    events = []
    for f, base in zip(frames, bases):
        players = f.get("plyr") or []
        if pi >= len(players) or not isinstance(players[pi], dict):
            continue
        for e in _event_list(players[pi].get(key)):
            if not isinstance(e, list) or len(e) < 3:
                continue
            if not isinstance(e[1], (int, float)):
                continue
            e2 = list(e)
            e2[1] = round(base + float(e[1]), 4)
            events.append(e2)
    events.sort(key=lambda e: (e[1], e[0], json.dumps(e[2:], ensure_ascii=False)))
    out = []
    prev = None
    for e in events:
        sig = (e[0], e[1], json.dumps(e[2:], ensure_ascii=False))
        if sig != prev:
            out.append(e)
            prev = sig
    return out


def _merge_placeables(frames, pi):
    """合并物件列表。

    注意：plac 不是「本块新增」，而是「当前场景快照 + 事件」，同一顶帐篷
    会在多个分块里重复出现，且时间字段不同。因此按「物件身份」去重：
    (defId, 资源名, 位置, 旋转/缩放)，忽略第 1 位的相对时间，保留最后一次
    出现的版本。
    """
    order = []
    latest = {}
    for f in frames:
        players = f.get("plyr") or []
        if pi >= len(players) or not isinstance(players[pi], dict):
            continue
        for e in _event_list(players[pi].get("plac")):
            if not is_placeable(e):
                continue
            sig = json.dumps([e[0], e[2], e[3], e[4:]],
                             ensure_ascii=False, sort_keys=True)
            if sig not in latest:
                order.append(sig)
            latest[sig] = copy.deepcopy(e)
    return [latest[sig] for sig in order]


def _chunk_bases(frames):
    """每块的起点 = 上一块的 chet；第一块从 0 开始。"""
    bases = []
    prev = 0.0
    for f in frames:
        bases.append(prev)
        prev = max(prev, _num(f.get("chet"), prev))
    return bases


def merge_frames(frames):
    """把多个 4 秒分块合并成一个完整的游戏原生 block。

    合并规则：
      - nts / anms：时间改为绝对时间（分块起点 + 相对时间），排序去重
      - sts        ：按分块顺序拼接（每块自带一个完整起始帧）
      - plac       ：并集去重
      - strs       ：并集，保持首次出现顺序
      - chet       ：最大 chet，即总时长
    """
    frames = [f for f in frames if is_block(f)]
    if not frames:
        raise ValueError("没有可合并的 block")

    bases = _chunk_bases(frames)
    template = copy.deepcopy(frames[-1])

    n_players = max(len(f.get("plyr") or []) for f in frames)
    merged_players = []
    for pi in range(n_players):
        src = None
        for f in reversed(frames):
            players = f.get("plyr") or []
            if pi < len(players) and isinstance(players[pi], dict):
                src = players[pi]
                break
        if src is None:
            continue

        p = copy.deepcopy(src)

        sts = []
        for f in frames:
            players = f.get("plyr") or []
            if pi < len(players) and isinstance(players[pi], dict):
                sts.extend(copy.deepcopy(_event_list(players[pi].get("sts"))))
        if sts:
            p["sts"] = sts
        else:
            p.pop("sts", None)

        for key in ("nts", "anms"):
            events = _merge_events(frames, bases, pi, key)
            if events:
                p[key] = events
            else:
                p.pop(key, None)

        plac = _merge_placeables(frames, pi)
        if plac:
            p["plac"] = plac
        else:
            p.pop("plac", None)

        # ofts 在 0.35.2 里是会话哈希表，不是音符；保留最后一次出现的原值
        for key in ("ofts", "shts", "phdr"):
            for f in reversed(frames):
                players = f.get("plyr") or []
                if pi < len(players) and isinstance(players[pi], dict) and key in players[pi]:
                    p[key] = copy.deepcopy(players[pi][key])
                    break

        merged_players.append(p)

    template["plyr"] = merged_players

    seen = set()
    strs = []
    for f in frames:
        for s in _event_list(f.get("strs")):
            if s not in seen:
                seen.add(s)
                strs.append(s)
    if strs:
        template["strs"] = strs
    else:
        template.pop("strs", None)

    if not template.get("mky"):
        for f in frames:
            if f.get("mky"):
                template["mky"] = copy.deepcopy(f["mky"])
                break

    template["chet"] = max(_num(f.get("chet"), 0.0) for f in frames)
    return template


def kind_from_pool(pool_type):
    """服务器侧的 pool_type -> 蓝图类型（比内容启发式可靠）。"""
    p = (pool_type or "").strip().lower()
    if not p:
        return None
    if p.startswith("sharedspace") or p.startswith("shared_space"):
        return "shared_space"
    if p.startswith("recording"):
        return "recording"
    if p.startswith("message"):
        return "message"
    return None


def detect_kind(block, pool_type=None) -> str:
    """判断 blueprint 类型：优先用服务器给出的 pool_type，其次看内容。"""
    hinted = kind_from_pool(pool_type)
    if hinted:
        return hinted
    if not isinstance(block, dict):
        return "unknown"
    ghost = content_player(block) or {}
    notes = _event_list(ghost.get("nts") or block.get("nts"))
    anims = _event_list(ghost.get("anms") or block.get("anms"))
    plac = [e for e in _event_list(ghost.get("plac") or block.get("plac"))
            if is_placeable(e)]
    if notes or anims:
        return "recording"
    if plac:
        return "shared_space"
    for k in ("msg", "message", "text", "content", "cntn"):
        if block.get(k):
            return "message"
    return "unknown"


def space_objects(block):
    """提取共享空间里的物件（跨玩家、跨分块去重）。

    返回 [{"def_id", "name", "pos", "right", "up", "forward", "scale", "scale_vec"}]
    """
    players = block.get("plyr") if isinstance(block, dict) else None
    if not isinstance(players, list):
        return []
    out = []
    seen = set()
    for p in players:
        if not isinstance(p, dict):
            continue
        for e in _event_list(p.get("plac")):
            if not is_placeable(e):
                continue
            sig = json.dumps([e[0], e[2], e[3], e[4:]], ensure_ascii=False, sort_keys=True)
            if sig in seen:
                continue
            seen.add(sig)
            obj = {"def_id": e[0], "name": e[2], "pos": e[3]}
            if len(e) >= 7:
                obj["right"], obj["up"], obj["forward"] = e[4], e[5], e[6]
            if len(e) >= 8:
                obj["scale"] = e[7]
            if len(e) >= 10:
                obj["scale_vec"] = e[9]
            out.append(obj)
    return out


def summarize(block, chunks=None) -> dict:
    """从合并后的 block 计算摘要（列表/校验都靠它）。"""
    summary = {
        "notes": 0, "events": 0, "anims": 0,
        "placeables": 0, "players": 0, "trajectory": 0,
        "strs": 0, "key_min": None, "key_max": None,
        "objects": 0,
    }
    if not isinstance(block, dict):
        return summary

    players = block.get("plyr") or []
    summary["players"] = len([p for p in players if isinstance(p, dict)])
    summary["strs"] = len(_event_list(block.get("strs")))

    keys = []
    for p in players:
        if not isinstance(p, dict):
            continue
        summary["trajectory"] += len(_event_list(p.get("sts")))
        summary["placeables"] += sum(1 for e in _event_list(p.get("plac"))
                                     if is_placeable(e))
        for e in _event_list(p.get("nts")):
            if not isinstance(e, list) or len(e) < 3:
                continue
            summary["events"] += 1
            if e[0] == 3 and isinstance(e[2], int):
                summary["notes"] += 1
                keys.append(e[2])
        summary["anims"] += len(_event_list(p.get("anms")))

    if keys:
        summary["key_min"] = min(keys)
        summary["key_max"] = max(keys)
    summary["objects"] = len(space_objects(block))
    if chunks is not None:
        summary["chunks"] = len(chunks)
    return summary


# ---------------------------------------------------------------------------
# 跨地图对齐（纯平移）
#
# 不用硬编码偏移：由「源蓝图参考点」和「目标服同一物理位置的参考留影」
# 两点相减得到 delta，再平移源蓝图里所有 sts / plac 坐标。
# ---------------------------------------------------------------------------
def _vec(s):
    if not isinstance(s, str):
        return None
    t = s.strip().strip("()")
    parts = [p.strip() for p in t.split(",")]
    if len(parts) != 3:
        return None
    try:
        return tuple(float(p) for p in parts)
    except ValueError:
        return None


def _fmt_vec(v):
    return "%g,%g,%g" % (v[0], v[1], v[2])


def _anchor_pos(env, anchor="first"):
    """取参考点：first=第一帧玩家位置，plac=第一个物件位置。"""
    block = env.get("block") or {}
    g = content_player(block) or {}

    if anchor == "plac":
        for e in _event_list(g.get("plac")):
            if is_placeable(e):
                v = _vec(e[3])
                if v:
                    return v

    for p in block.get("plyr") or []:
        if not isinstance(p, dict):
            continue
        for e in _event_list(p.get("sts")):
            if isinstance(e, list) and len(e) > 1:
                v = _vec(e[1])
                if v:
                    return v

    return _vec(block.get("ppsp"))


def _shift_pos(s, delta):
    v = _vec(s)
    if v is None:
        return s
    return _fmt_vec((v[0] + delta[0], v[1] + delta[1], v[2] + delta[2]))


def align_blueprint(src, ref, out_path, anchor="first"):
    """把 src 对齐到 ref 的参考点，输出新的 .skyblueprint。"""
    a_src = _anchor_pos(src, anchor)
    a_ref = _anchor_pos(ref, anchor)
    if a_src is None or a_ref is None:
        raise ValueError("无法取得参考点（源或目标缺少 sts/plac）")

    delta = (a_ref[0] - a_src[0], a_ref[1] - a_src[1], a_ref[2] - a_src[2])
    chunks = copy.deepcopy(src.get("chunks") or [src.get("block")])
    shifted = 0

    for c in chunks:
        for p in c.get("plyr") or []:
            if not isinstance(p, dict):
                continue
            for e in _event_list(p.get("sts")):
                if isinstance(e, list) and len(e) > 1 and isinstance(e[1], str):
                    nv = _shift_pos(e[1], delta)
                    if nv != e[1]:
                        e[1] = nv
                        shifted += 1
            for e in _event_list(p.get("plac")):
                if (isinstance(e, list) and len(e) > 3
                        and isinstance(e[3], str)):
                    nv = _shift_pos(e[3], delta)
                    if nv != e[3]:
                        e[3] = nv
                        shifted += 1

    env = build_envelope(chunks, source=src.get("source"),
                         kind=src.get("kind"))
    env["meta"]["alignment"] = {
        "anchor": anchor,
        "source": list(a_src),
        "target": list(a_ref),
        "delta": list(delta),
        "shifted": shifted,
    }
    pack(env, out_path)
    return delta, shifted


# ---------------------------------------------------------------------------
# envelope 构建 / 读写
# ---------------------------------------------------------------------------
def build_envelope(frames, kind=None, source=None, created=None, require_block=True,
                   pool_type=None, extra_meta=None):
    """由一组分块（或单个 block）构建 v2 envelope。

    pool_type 来自服务器 feed（RecordingCandle / SharedSpaceShrine / ...），
    有值时用它决定 kind，而不是靠内容猜测。
    """
    frames = [f for f in frames if isinstance(f, dict)]
    if not frames:
        raise ValueError("没有 block 数据")
    if require_block and not any(is_block(f) for f in frames):
        raise ValueError("数据里没有游戏原生 block（缺少 stfs/plyr/stid 等字段）")

    stids = [f.get("stid") for f in frames if f.get("stid")]
    stid = stids[-1] if stids else ""

    block = merge_frames(frames) if len(frames) > 1 else copy.deepcopy(frames[0])
    k = kind or detect_kind(block, pool_type)

    duration = _num(block.get("chet"), 0.0)
    chets = [_num(f.get("chet"), 0.0) for f in frames if "chet" in f]
    spans = []
    prev = 0.0
    for c in chets:
        if c > prev:
            spans.append(round(c - prev, 4))
        prev = max(prev, c)
    chunk_span = max(spans) if spans else 0.0

    meta = {
        "stid": stid,
        "ppsp": block.get("ppsp"),
        "duration": duration,
        "chunks": len(frames),
        "chunk_span": chunk_span,
        "derived_block": len(frames) > 1,
    }
    if pool_type:
        meta["pool_type"] = pool_type
    if isinstance(extra_meta, dict):
        for key, value in extra_meta.items():
            if value in (None, "", [], {}):
                continue
            meta.setdefault(key, value)
    env = {
        "format": FORMAT_V2,
        "kind": k,
        "created": created or iso_now(),
        "source": source or "",
        "meta": meta,
        "summary": summarize(block, frames),
        "block": block,
        "chunks": frames,
        "integrity": {
            "algo": "sha256",
            "chunks_sha256": sha256(frames),
            "block_sha256": sha256(block),
        },
    }
    return env


def pack(env, path):
    body = json.dumps(env, ensure_ascii=False, separators=(",", ":"))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(MAGIC_V2)
        f.write(body.encode("utf-8"))
    os.replace(tmp, path)


def _legacy_envelope(block, path):
    env = build_envelope([block], created=iso_now())
    env["format"] = "skyblueprint/1"
    env["meta"]["derived_block"] = False
    env["integrity"] = {}
    env["source"] = ""
    env["_legacy"] = True
    return env


def load(path) -> dict:
    """读取 .skyblueprint（v1/v2 均可），也接受裸 block JSON。"""
    with open(path, "rb") as f:
        data = f.read()

    if data.startswith(MAGIC_V2):
        env = json.loads(data[len(MAGIC_V2):].decode("utf-8"))
        if not isinstance(env, dict) or "block" not in env:
            raise ValueError("不是有效的 SKYBP2 文件")
        return env

    if data.startswith(MAGIC_V1):
        block = json.loads(data[len(MAGIC_V1):].decode("utf-8"))
        return _legacy_envelope(block, path)

    text = data.decode("utf-8-sig", "replace").strip()
    # 裸 block / ndjson
    try:
        obj = json.loads(text)
    except Exception:
        obj = None
    if isinstance(obj, dict) and is_block(obj):
        return _legacy_envelope(obj, path)
    if isinstance(obj, dict) and "block" in obj:
        return obj
    frames = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except Exception:
            continue
        if isinstance(o, dict):
            frames.append(o)
    if frames:
        return build_envelope(frames)
    raise ValueError("无法识别的文件格式: %s" % path)


# ---------------------------------------------------------------------------
# 校验
# ---------------------------------------------------------------------------
def validate(env) -> tuple:
    """返回 (errors, warnings)。"""
    errors = []
    warnings = []

    if not isinstance(env, dict):
        return (["不是 JSON 对象"], [])

    fmt = env.get("format")
    if fmt not in ("skyblueprint/2", "skyblueprint/1"):
        errors.append("format 字段异常: %r" % (fmt,))

    block = env.get("block")
    if not isinstance(block, dict):
        errors.append("缺少 block 对象")
        return (errors, warnings)

    if block.get("ver") is None:
        warnings.append("block 缺少 ver")
    if not block.get("stid"):
        warnings.append("block 缺少 stid")
    elif not is_uuid(block["stid"]):
        warnings.append("stid 不是标准 UUID: %r" % (block["stid"],))

    stfs = block.get("stfs")
    stage_space = bool(env.get("space")) and env.get("kind") == "shared_space"
    if not isinstance(stfs, list) or not stfs:
        if not stage_space:      # stage 文档转出来的空间本来就没有状态字段表
            warnings.append("block 缺少 stfs 字段表")
    elif not all(isinstance(x, str) for x in stfs):
        errors.append("stfs 必须是字符串数组")

    players = block.get("plyr")
    if not isinstance(players, list) or not players:
        errors.append("block 缺少 plyr")

    chunks = env.get("chunks")
    if not isinstance(chunks, list):
        errors.append("缺少 chunks 数组")
        chunks = []

    prev_chet = -1.0
    for i, c in enumerate(chunks):
        if not isinstance(c, dict):
            errors.append("chunks[%d] 不是对象" % i)
            continue
        if not is_block(c):
            errors.append("chunks[%d] 不是游戏原生 block" % i)
        chet = c.get("chet")
        if chet is not None:
            try:
                chet = float(chet)
            except (TypeError, ValueError):
                errors.append("chunks[%d].chet 不是数字" % i)
                continue
            if chet + 1e-6 < prev_chet:
                errors.append("chunks[%d].chet 回退: %s -> %s" % (i, prev_chet, chet))
            prev_chet = max(prev_chet, chet)

    # 事件结构
    ghost = content_player(block) or {}
    for key in ("nts", "anms"):
        for j, e in enumerate(_event_list(ghost.get(key))):
            if not isinstance(e, list) or len(e) < 3:
                errors.append("%s[%d] 结构异常（需要 [type,time,key]）" % (key, j))
                break
            if not isinstance(e[1], (int, float)):
                errors.append("%s[%d].time 不是数字" % (key, j))
                break
            if not isinstance(e[2], int):
                warnings.append("%s[%d].value 不是整数" % (key, j))
                break

    # 合并后的 nts 时间必须有序；原始单分块可能本来就是乱序的（游戏按事件类型
    # 分批下发），不能因此判定文件损坏。
    times = [e[1] for e in _event_list(ghost.get("nts"))
             if isinstance(e, list) and len(e) > 1 and isinstance(e[1], (int, float))]
    if any(b < a - 1e-6 for a, b in zip(times, times[1:])):
        if (env.get("meta") or {}).get("derived_block"):
            errors.append("合并后的 nts 时间不是递增的")
        else:
            warnings.append("nts 未按时间排序（原始分块，正常）")

    # 摘要一致性
    summary = env.get("summary")
    if isinstance(summary, dict):
        recomputed = summarize(block, chunks)
        for k in ("notes", "events", "anims", "placeables", "players", "trajectory", "strs"):
            if k in summary and summary[k] != recomputed.get(k):
                warnings.append("summary.%s=%s 与实际 %s 不一致"
                                % (k, summary[k], recomputed.get(k)))

    # 校验和
    integ = env.get("integrity") or {}
    if integ:
        if chunks and "chunks_sha256" in integ:
            if sha256(chunks) != integ["chunks_sha256"]:
                errors.append("chunks 校验和不匹配（文件被改动过）")
        if "block_sha256" in integ:
            if sha256(block) != integ["block_sha256"]:
                errors.append("block 校验和不匹配（文件被改动过）")
    else:
        warnings.append("没有 integrity 校验和（旧格式）")

    # 内容层面
    if isinstance(summary, dict):
        if (env.get("kind") == "recording"
                and summary.get("notes", 0) == 0
                and summary.get("anims", 0) == 0):
            warnings.append("recording 里没有音符或动画")
        if (len(chunks) > 1
                and summary.get("events", 0) == 0
                and summary.get("placeables", 0) == 0
                and summary.get("trajectory", 0) == 0):
            warnings.append("多个分块但合并后没有任何内容")

    return (errors, warnings)


# ---------------------------------------------------------------------------
# 展示
# ---------------------------------------------------------------------------
def describe(env) -> str:
    block = env.get("block") or {}
    s = env.get("summary") or {}
    m = env.get("meta") or {}
    lines = []
    pool = m.get("pool_type")
    lines.append("format   : %s   kind=%s%s"
                 % (env.get("format"), env.get("kind"),
                    ("   pool=%s" % pool) if pool else ""))
    lines.append("stid     : %s" % (block.get("stid") or m.get("stid") or "-"))
    lines.append("source   : %s" % (env.get("source") or "-"))
    lines.append("created  : %s" % (env.get("created") or "-"))
    lines.append("duration : %ss   chunks=%s   span=%ss"
                 % (m.get("duration", block.get("chet", "-")),
                    m.get("chunks", len(env.get("chunks") or [])),
                    m.get("chunk_span", "-")))
    lines.append("ppsp     : %s" % (block.get("ppsp", "-")))
    lines.append("notes    : %s  (events=%s, keys %s..%s)"
                 % (s.get("notes"), s.get("events"), s.get("key_min"), s.get("key_max")))
    lines.append("anims    : %s   placeables=%s   players=%s   trajectory=%s"
                 % (s.get("anims"), s.get("placeables"), s.get("players"), s.get("trajectory")))

    strs = block.get("strs") or []
    if strs:
        lines.append("strs(%d)  : %s" % (len(strs), ", ".join(map(str, strs[:12]))))

    objs = space_objects(block)
    if objs:
        names = {}
        for o in objs:
            names[o["name"]] = names.get(o["name"], 0) + 1
        kinds = ", ".join("%s×%d" % (n, c) for n, c in sorted(names.items())[:8])
        lines.append("objects  : %d  (%s)" % (len(objs), kinds))
        for o in objs[:8]:
            lines.append("    %s @ %s" % (o["name"], o["pos"]))

    for i, p in enumerate((block.get("plyr") or [])[:3]):
        if not isinstance(p, dict):
            continue
        sts = p.get("sts") or []
        first = sts[0] if sts else []
        pos = first[1] if len(first) > 1 else "?"
        lines.append("player%d  : sts=%d nts=%d anms=%d plac=%d  first=%s"
                     % (i, len(sts), len(p.get("nts") or []), len(p.get("anms") or []),
                        len(p.get("plac") or []), pos))
    return "\n".join(lines)


def list_blueprints(root) -> list:
    rows = []
    for dirpath, _, files in os.walk(root):
        for name in files:
            if not name.endswith(".skyblueprint"):
                continue
            path = os.path.join(dirpath, name)
            try:
                env = load(path)
                s = env.get("summary") or {}
                m = env.get("meta") or {}
                rows.append({
                    "file": os.path.relpath(path, root),
                    "stid": (env.get("block") or {}).get("stid") or m.get("stid") or "",
                    "kind": env.get("kind") or "unknown",
                    "duration": m.get("duration", 0),
                    "chunks": m.get("chunks", 1),
                    "notes": s.get("notes", 0),
                    "placeables": s.get("placeables", 0),
                    "size": os.path.getsize(path),
                    "mtime": int(os.path.getmtime(path)),
                    "error": None,
                })
            except Exception as e:
                rows.append({
                    "file": os.path.relpath(path, root), "stid": "", "kind": "?",
                    "duration": 0, "chunks": 0, "notes": 0, "placeables": 0,
                    "size": os.path.getsize(path), "mtime": int(os.path.getmtime(path)),
                    "error": str(e),
                })
    rows.sort(key=lambda r: r["mtime"], reverse=True)
    return rows


def print_list(rows, as_json=False):
    if as_json:
        print(json.dumps(rows, ensure_ascii=False, indent=1))
        return
    if not rows:
        print("（没有 .skyblueprint 文件）")
        return
    print("%-36s %-12s %7s %6s %7s %8s  %s"
          % ("STID", "KIND", "SEC", "CHUNK", "NOTES", "SIZE", "FILE"))
    for r in rows:
        if r["error"]:
            print("%-36s %-12s %7s %6s %7s %8d  %s  [ERROR: %s]"
                  % ("-", "?", "-", "-", "-", r["size"], r["file"], r["error"]))
        else:
            print("%-36s %-12s %7.1f %6d %7d %8d  %s"
                  % (r["stid"], r["kind"], r["duration"], r["chunks"],
                     r["notes"], r["size"], r["file"]))
    print("共 %d 个蓝图" % len(rows))


# ---------------------------------------------------------------------------
# 解析外部输入
# ---------------------------------------------------------------------------
def parse_frames(data: bytes):
    """把 ndjson / JSON 数组 / 单个 block 解析成分块列表。"""
    text = data.decode("utf-8-sig", "replace")

    try:
        obj = json.loads(text)
    except Exception:
        obj = None

    if isinstance(obj, list):
        return [x for x in obj if isinstance(x, dict)]
    if isinstance(obj, dict):
        if isinstance(obj.get("chunks"), list):
            return [x for x in obj["chunks"] if isinstance(x, dict)]
        if is_block(obj):
            return [obj]

    frames = []
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            o = json.loads(line)
        except Exception:
            continue
        if isinstance(o, dict) and is_block(o):
            frames.append(o)
    return frames


def export(env, out_path, mode):
    block = env.get("block") or {}
    chunks = env.get("chunks") or [block]

    if mode == "block":
        data = json.dumps(block, ensure_ascii=False, indent=1)
    elif mode == "chunks":
        data = json.dumps(chunks, ensure_ascii=False, indent=1)
    elif mode == "ndjson":
        data = "\n".join(json.dumps(c, ensure_ascii=False, separators=(",", ":"))
                         for c in chunks) + "\n"
    elif mode == "notes":
        ghost = content_player(block) or {}
        notes = [e for e in (ghost.get("nts") or [])
                 if isinstance(e, list) and len(e) >= 3 and e[0] == 3]
        notes.sort(key=lambda e: e[1])
        data = json.dumps({
            "stid": block.get("stid"),
            "duration": (env.get("meta") or {}).get("duration"),
            "count": len(notes),
            "notes": [{"time": e[1], "key": e[2]} for e in notes],
        }, ensure_ascii=False, indent=1)
    elif mode == "csv":
        ghost = content_player(block) or {}
        notes = [e for e in (ghost.get("nts") or [])
                 if isinstance(e, list) and len(e) >= 3 and e[0] == 3]
        notes.sort(key=lambda e: e[1])
        buf = io.StringIO()
        w = csv.writer(buf)
        w.writerow(["time", "key"])
        for e in notes:
            w.writerow([e[1], e[2]])
        data = buf.getvalue()
    else:
        raise ValueError("未知导出类型: %s" % mode)

    with open(out_path, "w", encoding="utf-8", newline="\n") as f:
        f.write(data)


# ---------------------------------------------------------------------------
# 自测
# ---------------------------------------------------------------------------
def resolve_sample():
    """优先选择一个能体现「分块合并」价值的样本（合并后音符数 > 最后一帧）。"""
    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.dirname(here)
    d = os.path.join(root, "samples", "fetched")
    if not os.path.isdir(d):
        return None

    candidates = [os.path.join(d, n) for n in sorted(os.listdir(d))
                  if n.endswith(".ndjson")]
    fallback = candidates[0] if candidates else None
    for path in candidates:
        try:
            with open(path, "rb") as f:
                frames = parse_frames(f.read())
            if len(frames) < 2:
                continue
            env = build_envelope(frames)
            ghost = content_player(frames[-1]) or {}
            last_notes = sum(1 for e in (ghost.get("nts") or [])
                             if isinstance(e, list) and len(e) >= 3 and e[0] == 3)
            if env["summary"]["notes"] > last_notes:
                return path
        except Exception:
            continue
    return fallback


def selftest(path=None):
    path = path or resolve_sample()
    if not path or not os.path.exists(path):
        print("找不到测试样本，请传入 ndjson 路径")
        return 1

    print("样本:", path)
    with open(path, "rb") as f:
        frames = parse_frames(f.read())
    print("分块:", len(frames))
    if not frames:
        print("FAIL: 没有解析出分块")
        return 1

    env = build_envelope(frames, source="selftest")
    errors, warnings = validate(env)
    s = env["summary"]
    print("合并后: notes=%d events=%d anims=%d placeables=%d chunks=%d duration=%s"
          % (s["notes"], s["events"], s["anims"], s["placeables"],
             s.get("chunks"), env["meta"]["duration"]))

    last = frames[-1]
    lghost = content_player(last) or {}
    last_notes = sum(1 for e in (lghost.get("nts") or [])
                     if isinstance(e, list) and len(e) >= 3 and e[0] == 3)
    print("最后一帧 notes=%d（旧流程只会保存这些）" % last_notes)
    if len(frames) > 1 and last_notes > 0 and s["notes"] <= last_notes:
        print("FAIL: 合并后没有比最后一帧多，分块合并可能失效")
        return 1
    if len(frames) > 1 and s.get("chunks") != len(frames):
        print("FAIL: 分块数量没有完整保留")
        return 1
    if s["notes"] == 0:
        print("该样本没有音符（非钢琴内容），跳过最后一帧对比")

    import tempfile
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "t.skyblueprint")
        pack(env, out)
        env2 = load(out)
        e2, w2 = validate(env2)
        if e2:
            print("FAIL: 落盘后校验失败:", e2)
            return 1
        if env2["summary"] != env["summary"]:
            print("FAIL: 落盘前后摘要不一致")
            return 1
        # 导出
        for mode, ext in (("block", ".json"), ("ndjson", ".ndjson"),
                          ("notes", ".notes.json"), ("csv", ".csv")):
            p = os.path.join(td, "t" + ext)
            export(env2, p, mode)
            if os.path.getsize(p) == 0:
                print("FAIL: 导出 %s 为空" % mode)
                return 1
        # 旧格式兼容
        p1 = os.path.join(td, "legacy.skyblueprint")
        with open(p1, "wb") as f:
            f.write(MAGIC_V1 + json.dumps(frames[-1], ensure_ascii=False).encode("utf-8"))
        env3 = load(p1)
        if env3["block"].get("stid") != frames[-1].get("stid"):
            print("FAIL: 旧格式读取失败")
            return 1

    if errors:
        print("FAIL: 校验错误:", errors)
        return 1
    for w in warnings:
        print("  warn:", w)
    print("selftest OK")
    return 0


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------
def usage():
    print(__doc__)


def arg_value(args, name, default=None):
    if name in args:
        i = args.index(name)
        if i + 1 < len(args):
            return args[i + 1]
    return default


def main():
    args = sys.argv[1:]
    if not args:
        usage()
        return 0
    cmd = args[0]

    if cmd == "show":
        if len(args) < 2:
            return usage() or 3
        print(describe(load(args[1])))
        return 0

    if cmd == "space":
        if len(args) < 2:
            return usage() or 3
        env = load(args[1])
        objs = space_objects(env.get("block") or {})
        if "--json" in args:
            print(json.dumps(objs, ensure_ascii=False, indent=1))
        else:
            print("%s  kind=%s  objects=%d"
                  % (os.path.basename(args[1]), env.get("kind"), len(objs)))
            for i, o in enumerate(objs):
                print("[%3d] %-34s %s" % (i, o.get("name"), o.get("pos")))
        return 0

    if cmd == "validate":
        if len(args) < 2:
            return usage() or 3
        env = load(args[1])
        errors, warnings = validate(env)
        quiet = "--quiet" in args
        if not quiet:
            print(describe(env))
            print()
        if warnings and not quiet:
            for w in warnings:
                print("warn:", w)
        if errors:
            for e in errors:
                print("ERROR:", e)
            return 1
        print("%s: OK" % os.path.basename(args[1]))
        return 0

    if cmd == "list":
        root = args[1] if len(args) > 1 and not args[1].startswith("--") else "."
        print_list(list_blueprints(root), as_json="--json" in args)
        return 0

    if cmd == "pack":
        if len(args) < 3:
            return usage() or 3
        src, dst = args[1], args[2]
        with open(src, "rb") as f:
            frames = parse_frames(f.read())
        env = build_envelope(
            frames,
            kind=arg_value(args, "--kind"),
            source=arg_value(args, "--source", src),
            pool_type=arg_value(args, "--pool"),
        )
        pack(env, dst)
        print("packed -> %s  (%d chunks, %d notes)" % (dst, len(frames), env["summary"]["notes"]))
        return 0

    if cmd == "unpack":
        if len(args) < 3:
            return usage() or 3
        env = load(args[1])
        with open(args[2], "w", encoding="utf-8", newline="\n") as f:
            json.dump(env, f, ensure_ascii=False, indent=1)
        print("unpacked ->", args[2])
        return 0

    if cmd == "export":
        if len(args) < 3:
            return usage() or 3
        mode = arg_value(args, "--as", "block")
        export(load(args[1]), args[2], mode)
        print("exported (%s) -> %s" % (mode, args[2]))
        return 0

    if cmd == "align":
        if len(args) < 3:
            return usage() or 3
        src = load(args[1])
        ref = load(args[2])
        out = (args[3] if len(args) > 3 and not args[3].startswith("--")
               else os.path.splitext(args[1])[0] + "_aligned.skyblueprint")
        anchor = "plac" if "--plac" in args else "first"
        delta, shifted = align_blueprint(src, ref, out, anchor)
        print("aligned (%s anchor): delta=(%g,%g,%g) shifted=%d -> %s"
              % (anchor, delta[0], delta[1], delta[2], shifted, out))
        return 0

    if cmd == "extract":
        if len(args) < 2:
            return usage() or 3
        raw = open(args[1], "rb").read()
        outdir = args[2] if len(args) > 2 else "blocks"
        os.makedirs(outdir, exist_ok=True)
        blocks = extract_blocks(raw)
        print("提取到 %d 个 block" % len(blocks))
        for i, b in enumerate(blocks):
            name = b.get("stid") or ("block%03d" % i)
            path = os.path.join(outdir, "%s.json" % name)
            if os.path.exists(path):
                path = os.path.join(outdir, "%s_%03d.json" % (name, i))
            with open(path, "w", encoding="utf-8", newline="\n") as f:
                json.dump(b, f, ensure_ascii=False, indent=1)
            env = build_envelope([b])
            print("  [%d] %s  notes=%d" % (i, name, env["summary"]["notes"]))
        return 0

    if cmd == "selftest":
        return selftest(args[1] if len(args) > 1 else None)

    usage()
    return 3


if __name__ == "__main__":
    sys.exit(main())
