# 留影数据与 `.skyblueprint` 格式

本文记录保存侧的实测结论，方便二次开发。

## 1. 留影的原始数据

服务器返回的留影下载文件是 **ndjson**（每行一个 JSON block），也可以是一个 JSON 数组。
每一行是一个约 **4 秒**的分块：

```json
{"ver":1,"stid":"...","chet":4.0,"nts":[...],"anms":[...],"sts":[...],"plac":[...]}
```

关键语义：

- `chet`：这一块的**结束时间**（4、8、12……），不是块内偏移。
- `nts`（音符）/ `anms`（动画）里的时间是**相对块起点**的，范围 0–4 秒。
- 相邻块的音符集合几乎不重叠 —— 它是**增量流**，不是快照。
- 合并公式：`绝对时间 = 上一块的 chet + 本块相对时间`。
- `sts`（轨迹）每块自带一个起始帧，按块顺序拼接。
- `plac`（放置/收回事件）取并集，`strs`（字符串）取并集。

> 只取最后一行会丢掉约 95% 的音符 —— 这是早期版本最大的坑。

## 2. `.skyblueprint` v2

```
SKYBP2\n
{
  "format": "skyblueprint/2",
  "kind": "recording|shared_space|message|unknown",
  "created": "2026-01-01T00:00:00Z",
  "source": "<原始下载地址>",
  "meta": {
    "stid": "...", "ppsp": "...", "duration": 23.85,
    "chunks": 6, "chunk_span": 4.0, "derived_block": true
  },
  "summary": {
    "notes": 0, "events": 0, "anims": 0, "placeables": 0,
    "players": 1, "trajectory": 0, "strs": 0,
    "key_min": 0, "key_max": 0
  },
  "block":  { ... },   // 跨块合并后的派生 block，便于直接使用
  "chunks": [ ... ],   // 原始分块，逐字节保留
  "integrity": {
    "algo": "sha256",
    "chunks_sha256": "...",
    "block_sha256": "..."
  }
}
```

兼容旧的 `SKYBP1\n` + 单个原生 block。

### 合并规则（`skybp.merge_frames`）

1. 按 `chet` 排序分块。
2. `nts` / `anms` 时间加上前面所有块的 `chet`。
3. `sts` 按顺序拼接（保留每块首帧）。
4. `plac` / `strs` 去重合并。
5. `chet` 取最大值，`stid` 取第一块。

### 校验（`skybp.validate`）

- 头部 magic 正确、JSON 可解析
- `chunks` 非空且每块有 `stid` / `chet`
- `summary` 与 `block` 的统计一致
- `integrity` 里的两个 SHA-256 与实际内容一致

## 3. 部署时的注意事项

- `stid` 默认会被换成新的 UUID，否则服务器可能认为是同一条留影。
- 多人留影需要 `extra_data` 里带上 `p0id`、`p1id`……（来自源 feed 的响应），
  这些 ID 由 mod 在抓取时写进 `feed_meta.json`。
- `pos` 是**地图世界坐标**，不是相对锚点；换地图后要重新指定。
- 分块必须按顺序用 `recording/api/v2/update` 上传，最后再创建 social feed。
