# that-sky-blueprint

光遇 (Sky: Children of the Light) 留影蓝图保存 / 复制工具 —— 把游戏里的留影（Recording）
抓取成可离线保存的 `.skyblueprint` 文件，再通过服务器 API 重新发布一份。
**仅供本机 / 私有服务器研究，不要在官方多人环境滥用。**

> [!WARNING]
> **AI 生成项目**：本项目的代码与文档由 AI（Claude）在人工指导下生成，并经过实机验证。
> 代码可读性、边界处理和长期维护都不如人工项目，请自行审阅后再使用。
>
> **当前能力有限**：目前只验证了**留影（Recording）** 的保存与复制；
> 留言（Message）、共享空间（Shared Space）尚未打通。
>
> **兼容性**：mod 里的地址是**硬编码到具体游戏版本**的（开发时是 Steam 版 0.35.2.426499）。
> 游戏更新后大概率直接失效，需要重新定位函数地址。
>
> **服务器差异**：部署走的是服务器的 `recording` / `social-feed` HTTP API，
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
│  ├─ deploy.py             把蓝图上传为新的留影 + social feed
│  ├─ deployd.py            一键部署守护进程（配合游戏内按钮）
│  ├─ cn_import.py          国服分块 -> 蓝图
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
- **国服只读抓取**：国服客户端可以抓取录制分块并转成蓝图（部署未完成）。

## 环境要求

- Windows 10 / 11
- Steam 版 Sky（开发验证版本 **0.35.2.426499**），或网易国服客户端（仅抓取）
- [HTModLoader](https://github.com/that-sky-project) **1.8.1+**（含 SDK）
- MSYS2 + mingw64 g++（`/c/msys64/ucrt64/bin`）
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

### 2. 安装

把 `sky-blueprint.dll` 和 `manifest.json` 放进：

```text
<游戏目录>/htmodloader/mods/sky-blueprint/
```

启动游戏，按 **F2** 打开面板。

### 3. 配置服务器地址

服务器地址不写死，需要自己指定（部署用）：

```powershell
$env:SKY_SERVER = "https://your.server"
```

如果留影链接里的域名需要替换（部分环境会写占位域名），在 `tool/server.json` 里配置：

```json
{ "rewrite": { "old.example.com": "new.example.com" } }
```

### 4. 抓取一个留影

1. 游戏里点开一个留影（让它完整播放或至少加载出下载请求）
2. mod 会把链接写进 `%APPDATA%\sky-blueprint\steam\links.txt`
3. 运行下载器：

```bash
python tool/autofetch.py          # 持续监控
python tool/autofetch.py --once   # 处理一次
```

蓝图会生成到 `%APPDATA%\sky-blueprint\steam\blueprints\`，也就是 mod 面板默认读取的目录。

### 5. 复制到服务器

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

### 6. 手动查看 / 校验

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
    captures\     调试抓包（有数量上限）
    records\      国服录制原始分块
```

可以用环境变量 `SKY_DATA_ROOT` 改根目录。

## 已知限制

- **只支持留影**：留言、共享空间请勿期待。
- **版本绑定**：hook 地址按 0.35.2 定位，游戏更新会失效。
- **多人留影依赖源元数据**：需要先抓到源留影的 `p0id/p1id/...`（mod 会自动从 feed 响应里提取），否则复制出来只有一个人。
- **位置是地图坐标**：部署时的 `pos` 是蓝图里的世界坐标，不同地图不能直接套用。
- **国服部署未完成**：国服只做了抓取，创建/发布流程尚未打通。
- **抓包会上限**：调试抓包默认限流，不是完整流量记录。

## 风险与免责声明

使用本工具前请确认你能接受以下风险：

1. **封号风险**：修改客户端、伪造录制和调用未公开 API 都可能违反游戏服务条款，
   可能导致账号被限制或封禁。
2. **服务器条款**：请只在你有权限的私有服务器上测试；不要在官方服务器或他人的
   服务器上制造公共内容。
3. **数据与隐私**：mod 会在本地记录 session token、user id 和请求内容
   （默认写在 `%APPDATA%` 下）。不要把 `session.json`、`links.txt`、`captures\`
   分享给他人，也不要提交到仓库。
4. **稳定性**：mod 通过 inline hook 修改游戏函数，可能崩溃、丢失未保存内容，
   或与其他 mod 冲突。
5. **无担保**：本项目按 GPL-3.0 “原样”提供，不提供任何明示或暗示的担保；
   使用造成的一切后果由使用者自行承担。

## 相关项目

- [that-sky-pianoplay](https://github.com/that-sky-project/that-sky-pianoplay) —— 同一生态下的
  乐器自动演奏工具，本项目的 README 结构参考了它。
- [HTModLoader](https://github.com/that-sky-project) —— 本 mod 依赖的加载器与 SDK。

## 许可证

[GPL-3.0](LICENSE)
