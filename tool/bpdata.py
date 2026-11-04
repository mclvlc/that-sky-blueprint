#!/usr/bin/env python3
"""运行期数据目录。

mod 不把运行文件写进安装目录，统一写到：

    %APPDATA%\\sky-blueprint\\steam\\     国际服（Steam）
    %APPDATA%\\sky-blueprint\\cn\\        国服（网易）

目录结构：

    bp.log  links.txt  session.json  feed_meta.json
    deploy_queue.json  deploy_result.txt  deploy_result.json
    captures\\   调试抓包（req_/cap_/tls_*/cn_req_/cn_body_，有数量上限）
    records\\    录制原始分块（cn_chunk_*.bin）
    blueprints\\ 蓝图目录兜底位置

需要读写别的目录时用环境变量覆盖：
    SKY_DATA_ROOT   默认 %APPDATA%\\sky-blueprint
"""
import os

STEAM_MOD = os.environ.get("SKY_MOD_DIR_STEAM", "")
CN_MOD = os.environ.get("SKY_MOD_DIR_CN", "")


def data_root():
    root = os.environ.get("SKY_DATA_ROOT")
    if root:
        return root
    appdata = os.environ.get("APPDATA") or os.path.expanduser("~")
    return os.path.join(appdata, "sky-blueprint")


def data_dir(edition="steam"):
    return os.path.join(data_root(), edition)


def mod_dir(edition="steam"):
    """mod 安装目录（可选，用于从旧版本迁移）。"""
    return CN_MOD if edition == "cn" else STEAM_MOD


def candidates(name, edition="steam", sub=""):
    """按优先级列出同名文件的位置：数据目录 -> 可选的旧 mod 目录。"""
    d = data_dir(edition)
    out = []
    if sub:
        out.append(os.path.join(d, sub, name))
    out.append(os.path.join(d, name))
    m = mod_dir(edition)
    if m:
        out.append(os.path.join(m, name))
        if sub:
            out.append(os.path.join(m, sub, name))
    return out


def server_host(edition="steam"):
    """mod 写出的当前服务器地址（国际服来自 data/AppInfo.tgc，国服来自抓包）。"""
    path = os.path.join(data_dir(edition), "server.txt")
    try:
        with open(path, encoding="utf-8") as f:
            return f.readline().strip()
    except Exception:
        return ""


def find(name, edition="steam", sub=""):
    """返回第一个已存在的路径；都不存在时返回数据目录下的目标路径。"""
    cands = candidates(name, edition, sub)
    for p in cands:
        if os.path.exists(p):
            return p
    return cands[0]


def find_all(name, edition="steam", sub=""):
    out = []
    for p in candidates(name, edition, sub):
        if os.path.exists(p) and p not in out:
            out.append(p)
    return out


def record_dirs(edition="cn"):
    """录制分块所在目录（新位置优先，可选旧位置兜底）。"""
    d = data_dir(edition)
    out = [os.path.join(d, "records"), d]
    m = mod_dir(edition)
    if m and m not in out:
        out.append(m)
    return out


def ensure_file_dir(path):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    return path
