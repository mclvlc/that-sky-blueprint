#!/usr/bin/env python3
"""把 mod 抓到的共享空间（stage 文档）转换成 .skyblueprint。

共享空间的内容不在留影 ndjson 里，而是服务器下发的 stage 文档：

    POST /service/stage/api/v1/get
    {"stage_id":"DuskStart_CNDL","level_id":"DuskStart",
     "user_id":"<owner>","sequence":1,"version":0,
     "props":[{"name":"CharSkyKid_Prop_...","uniqueID":6,
               "pos":"(x,y,z)","ori":"(x,y,z,w)","scale":"(1,1,1)", ...}],
     "updated_at":"...","challenge_duration":0,...}

mod 把这些文档原样写到 <数据目录>/stages/<stage_id>__<owner>.json，
本脚本把它转成 kind=shared_space 的蓝图：
  - space       原始 stage 文档（完整保留）
  - block       合成的 plac 列表（方便列表/预览/后续部署）
  - meta        stage_id / level_id / owner_id / sequence / version

用法:
    python stage_import.py            # 持续监控
    python stage_import.py --once     # 处理一次
    python stage_import.py --stages DIR --out DIR
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

STAGE_DIR = os.path.join(bpdata.data_dir("steam"), "stages")
OUT = os.path.join(bpdata.data_dir("steam"), "blueprints")   # 与 mod 默认目录一致
INDEX = os.path.join(OUT, "stage_index.json")

NS = uuid.UUID("6f9619ff-8b86-d011-b42d-00c04fc964ff")


def strip_parens(s):
    return (s or "").strip().strip("()").replace(" ", "")


def parse_vec(s):
    t = strip_parens(s)
    if not t:
        return None
    parts = t.split(",")
    try:
        vals = [float(p) for p in parts]
    except ValueError:
        return None
    return vals


def fmt_vec(vals):
    return ",".join("%g" % v for v in vals)


def quat_to_basis(ori):
    """四元数 (x,y,z,w) -> 三个基向量字符串（right/up/forward）。"""
    q = parse_vec(ori)
    if not q or len(q) != 4:
        return "1,0,0", "0,1,0", "0,0,1"
    x, y, z, w = q
    right = [1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)]
    up = [2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)]
    fwd = [2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)]
    return fmt_vec(right), fmt_vec(up), fmt_vec(fwd)


def make_block(doc, stid):
    """把 stage props 转成游戏原生 plac 结构（位置是 stage 局部坐标）。"""
    props = [p for p in (doc.get("props") or []) if isinstance(p, dict) and p.get("name")]
    props.sort(key=lambda p: (p.get("uniqueID") is None, p.get("uniqueID") or 0))
    plac = []
    for p in props:
        right, up, fwd = quat_to_basis(p.get("ori"))
        plac.append([
            0,                 # defId：stage 文档里没有，占位
            0.0,               # 时间：静态空间固定 0
            p["name"],
            strip_parens(p.get("pos")),
            right, up, fwd,
            0, 0,
            strip_parens(p.get("scale")) or "1,1,1",
        ])
    return {
        "ver": 1,
        "stfs": [],            # stage 文档没有状态字段表
        "plyr": [{"plac": plac}],
        "stid": stid,
        "ppsp": "0,0,0",
        "chet": 0.0,
        "chln": 0,
    }


def load_space_pos():
    """mod 从 SharedSpace feed 抓到的“作者 -> 空间世界坐标”。"""
    p = os.path.join(bpdata.data_dir("steam"), "space_pos.json")
    try:
        with open(p, encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def convert(path, out_dir):
    with open(path, encoding="utf-8") as f:
        doc = json.load(f)
    if not isinstance(doc, dict) or not doc.get("stage_id"):
        return None
    if not isinstance(doc.get("props"), list):
        return None

    stage_id = doc.get("stage_id") or ""
    owner = doc.get("user_id") or "unknown"
    level = doc.get("level_id") or ""
    stid = str(uuid.uuid5(NS, "stage/%s/%s" % (stage_id, owner)))

    feed = load_space_pos().get(owner) or {}
    block = make_block(doc, stid)
    env = skybp.build_envelope(
        [block],
        kind="shared_space",
        source="stage://%s/%s/%s" % (level, stage_id, owner),
        pool_type="SharedSpaceStage",
        extra_meta={
            "stage_id": stage_id,
            "level_id": level,
            "owner_id": owner,
            "sequence": doc.get("sequence"),
            "version": doc.get("version"),
            "updated_at": doc.get("updated_at"),
            "coordinate_space": "stage_local",
            # 空间入口在世界里的坐标（来自 SharedSpaceCandle feed）
            "pos": feed.get("pos"),
            "ori": feed.get("ori"),
        },
    )
    env["space"] = doc

    name = "space_%s_%s.skyblueprint" % (stage_id, owner[:8])
    out_path = os.path.join(out_dir, name)
    skybp.pack(env, out_path)
    return {
        "file": name,
        "stage_id": stage_id,
        "owner": owner,
        "props": len(doc.get("props") or []),
        "updated_at": doc.get("updated_at"),
    }


def load_index():
    try:
        with open(INDEX, encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def save_index(idx, out_dir=None):
    out_dir = out_dir or OUT
    path = os.path.join(out_dir, "stage_index.json")
    os.makedirs(out_dir, exist_ok=True)
    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8", newline="\n") as f:
        json.dump(idx, f, ensure_ascii=False, indent=1, sort_keys=True)
    os.replace(tmp, path)


def signature(doc, sp=None):
    sp = sp or {}
    return [doc.get("sequence"), doc.get("version"), doc.get("updated_at"),
            len(doc.get("props") or []), sp.get("pos"), sp.get("ori")]


def process_once(stage_dir, out_dir, index):
    changed = 0
    space_pos = load_space_pos()
    for path in sorted(glob.glob(os.path.join(stage_dir, "*.json"))):
        try:
            with open(path, encoding="utf-8") as f:
                doc = json.load(f)
        except Exception:
            continue
        key = "%s|%s" % (doc.get("stage_id"), doc.get("user_id"))
        sig = signature(doc, space_pos.get(doc.get("user_id")))
        if index.get(key) == sig:
            continue
        try:
            info = convert(path, out_dir)
            if info:
                index[key] = sig
                changed += 1
                print("wrote %s  props=%d  %s" % (info["file"], info["props"],
                                                  info["stage_id"]), flush=True)
        except Exception as e:
            print("FAIL %s: %s" % (os.path.basename(path), e), flush=True)
    if changed:
        save_index(index, out_dir)
    return changed


def main():
    args = sys.argv[1:]
    once = "--once" in args
    stage_dir, out_dir = STAGE_DIR, OUT
    for i, a in enumerate(args):
        if a == "--stages" and i + 1 < len(args):
            stage_dir = args[i + 1]
        if a == "--out" and i + 1 < len(args):
            out_dir = args[i + 1]
    os.makedirs(out_dir, exist_ok=True)
    os.makedirs(stage_dir, exist_ok=True)
    index = load_index()
    print("共享空间监控:", stage_dir, "->", out_dir, flush=True)
    while True:
        try:
            process_once(stage_dir, out_dir, index)
        except Exception as e:
            print("loop error:", e, flush=True)
        if once:
            break
        time.sleep(2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
