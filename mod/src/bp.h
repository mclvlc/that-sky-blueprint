#ifndef __BP_H__
#define __BP_H__

#include <windows.h>
#include "includes/htmodloader.h"
#include "imgui.h"
#include "aliases.h"

#define BP_VERSION "0.1.0"

// 国服（NetEase 0.16.7）移植诊断模式：不安装任何 hook，只扫描内存写 cn_scan.log
#ifndef BP_CN_DIAG
#define BP_CN_DIAG 0
#endif
// 国服实际抓取模式（读写 hook + session/link 提取）
#ifndef BP_CN_LIVE
#define BP_CN_LIVE 0
#endif

// 是否允许执行 Lua 脚本（开启后可用于驱动游戏，如 piano.play）
#ifndef BP_ENABLE_LUA_EXEC
#define BP_ENABLE_LUA_EXEC 0   // 部署侧实验期间关闭：直接调 StartRecording / 创建录制事件都会崩
#endif

// ---------------------------------------------------------------------------
// 安全模式：不安装任何 hook，只做只读探测（读版本、校验地址、抓取元信息）。
// 原因：游戏从 0.35.1 升到 0.35.2 后，所有硬编码 RVA 失效，装 hook 会直接闪退。
// 重新定位地址期间必须保持为 1。
// ---------------------------------------------------------------------------
#ifndef BP_SAFE_MODE
#define BP_SAFE_MODE 0
#endif

#if HTML_VERSION < 10801
#error "HTML SDK version must >= v1.8.1"
#endif

// ---------------------------------------------------------------------------
// 全局上下文
// ---------------------------------------------------------------------------

#define BP_MAX_LOG 32
#define BP_LOG_LINE 512

typedef struct {
  i32 showMenu;
  i32 ready;

  // 游戏信息（HTGetGameStatus）
  LPVOID gameBase;
  DWORD pid;
  HTGameEdition edition;
  u32 loaderVersion;
  char loaderName[64];

  // 模块基址
  LPVOID skyBase;
  u32 skyImageSize;

  // 本 mod 的安装目录（只放 dll/manifest/配置）
  char modFolder[MAX_PATH];

  // 运行期数据目录：%APPDATA%\sky-blueprint\<steam|cn>
  // session/feed_meta/links/deploy 队列/日志/抓包都写这里，避免污染 mod 本体
  char dataFolder[MAX_PATH];
  char capFolder[MAX_PATH];   // <data>/captures  调试抓包
  char recFolder[MAX_PATH];   // <data>/records   录制的原始分块

  // 探测日志（环形）
  char log[BP_MAX_LOG][BP_LOG_LINE];
  i32 logCount;
} BpContext;

extern BpContext gCtx;
extern HMODULE gModuleDll;

// 追加一行日志（线程安全，可被 hook 线程调用）
void bpLog(const char *fmt, ...);

// 调试抓包限流：idx 从 1 开始，超过 cap 就不再落盘（避免几千个 bin 文件）
int bpDumpOk(unsigned idx, unsigned cap);

// UI
void bpUIWindowMain(void);

// 蓝图列表
void bpListInit(void);
void bpListRefresh(void);
void bpListUI(void);
void bpListNotifyViewed(const char *url);

// 探测初始化（扫描、hook）
void bpProbeInit(void);
i32 bpProbeReloadIfChanged(void);
void bpProbeRequeue(void);
void bpClosureAnalyze(void);
void bpWriteRpcReport(void);
void bpNetTraceInit(void);
void bpNetTraceReport(void);
void bpTlsDumpInit(void);
void bpTlsOutHookInit(void);
void *bpManualHookInstall(void *target, void *detour);
void *bpManualHookInstallSlot(void *target, void *detour);
void bpManualHookUninstall(void);
void bpTlsDumpReport(void);
void bpTlsOutInit(void);
void bpTlsOutReport(void);
void bpCnDiagInit(void);
void bpCnLiveInit(void);

// Lua 桥
void bpLuaInit(void);
void bpLuaQueueScript(const char *src);
void bpLuaSetResultHook(void (*cb)(const char *result));
void bpLuaStatus(char *buf, int len);
void bpLuaScanState(void);
i32 bpLuaHasState(void);
i32 bpLuaPending(void);
void bpLuaWatchdog(void);
void bpLuaForceReset(void);
i32 bpLuaSelfTest(void);
i32 bpLuaRunPending(void);

#endif
