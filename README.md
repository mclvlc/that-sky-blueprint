# that-sky-blueprint

光遇 (Sky: Children of the Light) 蓝图保存 / 复制工具 —— 把游戏里的留影（Recording）
和共享空间（Shared Space）抓取成可离线保存的 `.skyblueprint` 文件，
再通过服务器 API 重新发布一份。
**仅供本机 / 私有服务器研究，不要在官方多人环境滥用。**

> [!WARNING]
> **AI 生成项目**：本项目的代码与文档由 AI（Claude）在人工指导下生成，并经过实机验证。
> 代码可读性、边界处理和长期维护都不如人工项目，请自行审阅后再使用。
>
> **当前能力**：**留影（Recording）**、**共享空间（Shared Space）**、**留言（Message）**
> 都验证了保存与复制。共享空间的部署是**整份覆盖**，不是追加；留言只能复制文本和位置。
>
> **兼容性**：mod 里的地址是**硬编码到具体游戏版本**的（开发时是 Steam 版 0.35.2.426499）。
> 游戏更新后大概率直接失效，需要重新定位函数地址。
>
> **服务器差异**：部署走的是服务器的 `recording` / `stage` / `social-feed` HTTP API，
> 不同服务器（尤其是私服）的实现、字段和鉴权都可能不同，**未必适配所有服务器**。
>
> **使用风险**：见文末「风险与免责声明」。使用本工具可能违反游戏服务条款并导致封号。

## 项目结构

```text
that-sky-blueprint/
├─ mod/                     HTModLoader C++ mod（sky-blueprint.dll）
│  ├─ src/
│  │  ├─ mod.cpp            入口、日志、数据目录
│  │  ├─ bplist.cpp         游戏内蓝图列表、备注、最近查看、一键部署
│  │  ├─ tlsdump.cpp        TLS 抓包（留影 link / session / 多人 feed 元数据）
│  │  ├─ cnlive.cpp         国服只读抓取（分块落盘）
│  │  ├─ manualhook.cpp     手写 inline hook（避开 MinHook 的长度问题）
│  │  └─ ui.cpp / probe.cpp / luabridge.cpp / ...
│  ├─ build.sh              构建脚本（CN=1/CN=2 编译国服变体）
│  ├─ Makefile
│  └─ manifest.json
├─ tool/                    Python 工具
│  ├─ skybp.py              .skyblueprint 读写 / 校验 / 列表 / 导出
│  ├─ autofetch.py          监控 links.txt，下载 ndjson 并合并成蓝图
│  ├─ deploy.py             把留影蓝图上传为新的 recording + social feed
│  ├─ stage_import.py       共享空间 stage 文档 -> 蓝图
│  ├─ stage_deploy.py       共享空间蓝图 -> stage/api/v1/set（+ 新建空间入口）
│  ├─ message_import.py     留言 feed -> 蓝图（只导出打开过的）
│  ├─ message_deploy.py     留言蓝图 -> social-feed/create
│  ├─ deployd.py            一键部署守护进程（留影 / 共享空间自动分流）
│  ├─ cn_import.py          国服分块 -> 蓝图（含 feed 文案）
│  ├─ cn_deploy.py          国服部署（加密请求：留影 / 共享空间 / 留言）
│  ├─ bridge_blueprint.py   把国服蓝图复制到国际服蓝图目录
│  ├─ backfill_caption.py   给旧蓝图补 meta.message（部署时自动填 Message）
│  ├─ bpdata.py             数据目录解析
│  └─ reqdump.py            解析抓到的请求（排障用）
└─ docs/
   └─ BLUEPRINT_FORMAT.md   .skyblueprint 格式与分块语义
```

## 目前能做什么

- **保存留影**：点开一个留影后，mod 会记录它的下载链接和当前 session，Python 工具把
  ndjson 分块全部下载回来，按 4 秒一块合并成完整录制（不会只留最后一帧）。
- **结构化保存**：输出 `.skyblueprint`（JSON 信封 + 原始分块 + SHA-256 校验），
  可以脱离游戏离线查看、合并、导出。
- **游戏内列表**：F2 面板里列出所有蓝图，支持备注、最近查看、打开目录、修改部署参数。
- **复制留影**：通过服务器 API 重新创建 recording 和 social feed，会带上原留影里的
  其他玩家 ID（多人留影可保留多人）。
- **保存共享空间**：共享空间的内容是 `stage/api/v1/get` 下发的 stage 文档
  （物件名 + 局部坐标 + 四元数朝向 + 缩放），mod 自动抓取并转成 `kind=shared_space`
  的蓝图，`space` 字段保留原始文档，`block.plyr[0].plac` 是合成的物件列表。
- **部署共享空间**：`stage_deploy.py` 调 `stage/api/v1/set` 写回物件，
  再用 `social-feed/api/v1/create` 新建空间入口（`SharedSpaceCandle`）。
  注意这是**整份覆盖**，不是追加。
- **保存留言**：留言（`MessageStone` / `MessageBoat` 等）没有 recording，内容就是
  social feed 本身（文本 + 位置）。mod 把 feed 收进 `messages.json`，只为你**打开过**
  的留言生成 `kind=message` 蓝图，避免附近留言刷屏。
- **发布留言**：`message_deploy.py` 调 `social-feed/api/v1/create` 发一条新留言，
  文本和位置可覆盖；面板选中 `kind=message` 蓝图点 Deploy 同样可用。
- **国服抓取/部署**：国服请求体是 `RC4(session2) + Base64`，响应按
  `gzip` / `session-valid` / `gzip-session-valid` 解码后复用国际服的解析。
  已实测可保存并部署国服留影、共享空间、留言（`cn_deploy.py`）。
- **文案自动填充**：抓取时顺手保存 feed 的 `message`（留影配文、共享空间入口文案、
  留言文本），面板选中蓝图时自动填进 `Message` 框；旧蓝图可用
  `backfill_caption.py` 补。

## 环境要求

- Windows 10 / 11
- Steam 版 Sky（开发验证版本 **0.35.2.426499**），或网易国服客户端（仅抓取）
- [HTModLoader](https://github.com/that-sky-project) **1.8.1+**（含 SDK）
- MSYS2 + mingw64 g++（`/c/msys64/ucrt64/bin`）
- zlib 开发包（`mingw-w64-ucrt-x86_64-zlib`，国服解码用）
- Python 3.8+

构建前把 HTModLoader SDK 放到：

```text
mod/libraries/htmodloader/
├─ includes/htmodloader/includes/...
├─ includes/imgui-1.92.2b/
├─ includes/cJSON/
└─ lib/htmodloader.lib
```

（SDK 不在本仓库中分发，请从 HTModLoader 发布包获取。）

## 快速上手

### 1. 构建 mod

```bash
cd mod
bash build.sh              # 国际服
CN=2 bash build.sh         # 国服（只读抓取）
# 产物: mod/dist/sky-blueprint.dll
```

### 2. 自动构建（GitHub Actions）

推一个 `v*` 标签就会触发 `.github/workflows/build-release.yml`：

```bash
git tag v0.1.0
git push origin v0.1.0
```

工作流会下载 HTModLoader SDK（Release 附件里的 `sdk/`）、编译 `sky-blueprint.dll`，
并把 `sky-blueprint-<tag>.zip`（dll + manifest + README）发布到 GitHub Releases。
不推 tag 时也可以在 Actions 页面手动运行（只上传 artifact，不发 Release）。

### 3. 安装

把 `sky-blueprint.dll` 和 `manifest.json` 放进：

```text
<游戏目录>/htmodloader/mods/sky-blueprint/
```

启动游戏，按 **F2** 打开面板。面板标题右侧可以勾选 **Chinese / 中文** 切换界面语言
（选择保存到 `<数据目录>/ui.cfg`，默认英文）。中文字体会自动从系统字体目录加载，
所以备注、留言文本里的中文在两种界面下都能正常显示。

### 4. 服务器地址（自动识别）

mod 启动时会自己确定部署地址，写到 `%APPDATA%\sky-blueprint\<steam|cn>\server.txt`：

- **国际服**：读 `<游戏目录>\data\AppInfo.tgc` 的第二行（客户端实际连接的服务器）
- **国服**：客户端地址由网易固定，mod 直接使用实际请求里的 `Host`

Python 工具的解析顺序：

```
--host > SKY_SERVER > server.txt > session.json.host > tool/server.json
```

切换官服/私服后重启游戏即可，不用改配置。读不到 AppInfo 时可以手动指定：

```powershell
$env:SKY_SERVER = "https://your.server"
```

如果留影链接里的域名需要替换（部分环境会写占位域名），在 `tool/server.json` 里配置：

```json
{ "rewrite": { "old.example.com": "new.example.com" } }
```

### 5. 抓取一个留影

1. 游戏里点开一个留影（让它完整播放或至少加载出下载请求）
2. mod 会把链接写进 `%APPDATA%\sky-blueprint\steam\links.txt`
3. 运行下载器：

```bash
python tool/autofetch.py          # 持续监控
python tool/autofetch.py --once   # 处理一次
```

蓝图会生成到 `%APPDATA%\sky-blueprint\steam\blueprints\`，也就是 mod 面板默认读取的目录。

### 6. 复制到服务器

**方式 A：游戏内一键部署（推荐）**

```bash
python tool/deployd.py
```

然后在面板里选中蓝图、填好位置 / 朝向 / 地图，点 **Deploy**。
mod 写队列文件，`deployd.py` 读取当前 session 并调用服务器 API，结果显示在面板上。

**方式 B：命令行**

```bash
python tool/deploy.py samples/xxx.skyblueprint \
    --host your.server \
    --user <user-id> --session <session> \
    --level Prairie_ButterflyFields \
    --pos "(111.68,196.04,-431.1)" --ori "(0,0.687,0,0.726)" \
    --message "Blueprint"
```

### 7. 共享空间（保存 / 部署）

打开一个共享空间后，mod 会抓取 `stage/api/v1/get` 的响应并写到：

```text
%APPDATA%\sky-blueprint\steam\stages\<stage_id>__<owner>.json
```

转换器（持续运行）会把它变成 `space_<stage_id>_<owner8>.skyblueprint`：

```bash
python tool/stage_import.py            # 持续监控
python tool/stage_import.py --once     # 处理一次
```

部署（只能写自己的空间）：

```bash
# 写入 props，并新建一个空间入口（需要世界坐标）
python tool/stage_deploy.py space_xxx.skyblueprint \
    --create --pos "(-88.625,64.443,-761.447)" --ori "(0,-0.421,-0,0.907)" \
    --message "测试空间" --verify

# 只写 props（不新建入口）
python tool/stage_deploy.py space_xxx.skyblueprint

# 复制别人的空间到自己的空间（整份覆盖）
python tool/stage_deploy.py other_space.skyblueprint --force --verify
```

游戏内也可以：选中 `shared_space` 蓝图 → Deploy，面板会自动带上源空间的世界坐标。

### 8. 留言（保存 / 发布）

点开一条留言后，mod 记录它的 feed（`messages.json` + `message_views.json`），
转换器只为“看过”的留言生成蓝图：

```bash
python tool/message_import.py            # 持续监控
python tool/message_import.py --once
python tool/message_import.py --list     # 列出抓到的留言（* = 打开过）
```

发布（文本 / 位置可以改）：

```bash
python tool/message_deploy.py message_xxx.skyblueprint
python tool/message_deploy.py message_xxx.skyblueprint --message "新文本" --pos "(x,y,z)"
```

### 9. 国服（网易）

国服客户端需要 CN 版 DLL（`CN=2 bash build.sh`），数据目录是 `%APPDATA%\sky-blueprint\cn\`。

```bash
# 保存（三路并行）
SKY_EDITION=cn python tool/cn_import.py       # 留影分块 -> 蓝图
SKY_EDITION=cn python tool/stage_import.py    # 共享空间 stage
SKY_EDITION=cn python tool/message_import.py  # 留言

# 一键部署守护进程（国服队列独立）
SKY_EDITION=cn python tool/deployd.py

# 手动部署（加密请求）
python tool/cn_deploy.py <file.skyblueprint>              # 共享空间
python tool/cn_deploy.py <file.skyblueprint> --recording  # 留影
python tool/cn_deploy.py <file.skyblueprint>              # 留言（自动识别 message 段）
```

国服机制：

- 请求体：`{"user":"…","body": Base64(RC4(session2, json))}`
- 响应头 `content-encoding` 决定解码方式（`gzip` / `session-valid` / `gzip-session-valid`）
- `session2` 从登录响应提取，mod 写到 `cn\session2.txt`（**敏感文件，不要外传**）
- 部署需要 `cn\session.json`（user/session/host）与 `cn\session2.txt`，由 mod 自动维护
- 构建需要 zlib（MSYS2 里装 `mingw-w64-ucrt-x86_64-zlib`）

### 10. 手动查看 / 校验

```bash
python tool/skybp.py list  %APPDATA%\sky-blueprint\steam\blueprints
python tool/skybp.py show  <file.skyblueprint>
python tool/skybp.py validate <file.skyblueprint>
python tool/skybp.py export <file> out.json --as notes
```

## 数据目录

mod 不会往安装目录写运行文件，全部落在：

```text
%APPDATA%\sky-blueprint\steam\     国际服
%APPDATA%\sky-blueprint\cn\        国服
    bp.log  links.txt  session.json  feed_meta.json
    deploy_queue.json  deploy_result.txt  deploy_result.json
    blueprints\   蓝图（.skyblueprint / index.json）
    stages\       共享空间的原始 stage 文档
    space_pos.json  作者 -> 空间世界坐标（部署新空间用）
    messages.json   抓到的留言 feed（最多 600 条）
    message_views.json  留言的查看时间（决定哪些生成蓝图）
    captures\     调试抓包（有数量上限）
    records\      国服录制原始分块
```

可以用环境变量 `SKY_DATA_ROOT` 改根目录。

## 已知限制

- **版本绑定**：hook 地址按 0.35.2 定位，游戏更新会失效。
- **多人留影依赖源元数据**：需要先抓到源留影的 `p0id/p1id/...`（mod 会自动从 feed 响应里提取），否则复制出来只有一个人。
- **位置是地图坐标**：留影部署的 `pos` 是蓝图里的世界坐标，不同地图不能直接套用。
- **共享空间是整份覆盖**：`stage/api/v1/set` 会替换该 stage 上的全部物件，不能追加；部署前先保存现有空间。
- **只能写自己的空间**：stage 按 `stage_id + user_id` 存。复制别人的空间要加 `--force`，且会写进你自己的空间。
- **stage 坐标是局部的**：物件坐标相对 stage 原点，只能写回同名 `stage_id`（同一张地图的同一位置）；新建入口的位置由 `--pos` 决定。
- **跨服 sequence 不一致**：蓝图里的 `sequence` 是来源服务器的值，工具会先读目标服当前 `sequence/version` 再写入，直接发会得到 `400 Invalid Request`。
- **新建入口可能受限**：`social-feed/api/v1/create` 带 `do_payment:true`，服务器可能限制每日次数或消耗货币。
- **留言只有文本和位置**：没有附带录制内容；“复制留言”= 在原位置再发一条一样的。
- **留言的 `personality_tag` 固定为 1**：样例不足，不同取值是否影响展示还没确认。
- **国服部署依赖 session2**：`cn\session2.txt` 是登录密钥派生物，泄漏等于泄露账号会话。
- **跨服搬运**：国服 block 的 `stfs` 多一个 `en` 字段，结构基本通用；用 `tool/bridge_blueprint.py` 把国服蓝图复制到国际服目录后再部署。国服专属动画/物件在国际服可能缺失。
- **文案来源**：蓝图配文来自抓取时的 feed；早期没抓到 feed 的蓝图需要用 `tool/backfill_caption.py` 补，补不到的就保持默认。
- **抓包会上限**：调试抓包默认限流（写请求除外），不是完整流量记录。

## 风险与免责声明

使用本工具前请确认你能接受以下风险：

1. **封号风险**：修改客户端、伪造录制和调用未公开 API 都可能违反游戏服务条款，
   可能导致账号被限制或封禁。
2. **服务器条款**：请只在你有权限的私有服务器上测试；不要在官方服务器或他人的
   服务器上制造公共内容。
3. **数据与隐私**：mod 会在本地记录 session token、user id 和请求内容
   （默认写在 `%APPDATA%` 下）。不要把 `session.json`、`links.txt`、`captures\`
   分享给他人，也不要提交到仓库。
4. **覆盖风险**：部署共享空间会整份覆盖你当前的物件列表；部署留影会在世界里
   多出一个留影。操作前请先用工具保存现有蓝图，便于回滚。
5. **国服会话密钥**：`session2.txt`、`session.json` 不要上传或分享；国服的请求加解密
   属于逆向实现，服务器更新后可能失效。
6. **稳定性**：mod 通过 inline hook 修改游戏函数，可能崩溃、丢失未保存内容，
   或与其他 mod 冲突。
7. **无担保**：本项目按 GPL-3.0 “原样”提供，不提供任何明示或暗示的担保；
   使用造成的一切后果由使用者自行承担。

## 相关项目

- [that-sky-pianoplay](https://github.com/that-sky-project/that-sky-pianoplay) —— 同一生态下的
  乐器自动演奏工具，本项目的 README 结构参考了它。
- [HTModLoader](https://github.com/that-sky-project) —— 本 mod 依赖的加载器与 SDK。
- [that-sky-requestdecode](https://github.com/that-sky-project/that-sky-requestdecode) —— 国服请求/响应解密思路（session2 + RC4 + gzip）参考了该项目。

## 许可证

[GPL-3.0](LICENSE)
