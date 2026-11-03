#include <stdio.h>
#include <string.h>
#include "bp.h"

// 脚本内容内存缓存：首次读取后保存，供周期性重新排队使用
static void bpScriptPath(char *out, size_t n);   // 前置声明

static char gScriptCache[65536] = {0};
static volatile i32 gScriptCached = 0;

void bpProbeRequeue(void) {
  // 优先从文件重新读取（支持热更新脚本），失败则退回内存缓存
  char path[MAX_PATH];
  bpScriptPath(path, sizeof(path));

  FILE *f = fopen(path, "rb");
  if (f) {
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    if (n > 0) {
      memcpy(gScriptCache, buf, n + 1);
      gScriptCached = 1;
      bpLuaQueueScript(buf);
      return;
    }
  }
  if (!gScriptCached || !gScriptCache[0])
    return;
  bpLuaQueueScript(gScriptCache);
}

// ---------------------------------------------------------------------------
// 探测层：定位游戏内部函数、执行探测脚本。
// ---------------------------------------------------------------------------

static void dumpModules(void) {
  static const char *names[] = {
    "sky-blueprint.dll", "hsc-main.dll", "sky-lua.dll", "piano-rpc.dll",
    "skylog.dll", "modtest.dll", "that-sky-helper.dll", "winhttp.dll"
  };
  for (i32 i = 0; i < (i32)(sizeof(names) / sizeof(names[0])); i++) {
    HMODULE h = GetModuleHandleA(names[i]);
    bpLog("module %-20s -> %p", names[i], h);
  }
}

// Lua 脚本执行结果落盘
static void onLuaResult(const char *result) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/lua_result.txt", gCtx.dataFolder);
  FILE *f = fopen(path, "w");
  if (!f) {
    bpLog("cannot write %s", path);
    return;
  }
  fprintf(f, "%s", result ? result : "");
  fclose(f);
  bpLog("lua result written (%d bytes)", result ? (int)strlen(result) : 0);
}

// 读取探测脚本并排队执行
static FILETIME gScriptWriteTime = {0};

static void rememberScriptTime(const char *path) {
  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    gScriptWriteTime = fad.ftLastWriteTime;
}

// 返回 1 表示脚本已变化并重新排队
static void bpScriptPath(char *out, size_t n) {
  // 优先用本 DLL 的实际路径推导，避免 HTGetModFolder 拼接不可靠
  char dll[MAX_PATH] = {0};
  if (gModuleDll && GetModuleFileNameA(gModuleDll, dll, MAX_PATH)) {
    char *slash = strrchr(dll, (char)0x5C);
    char *slash2 = strrchr(dll, '/');
    if (slash2 > slash) slash = slash2;
    if (slash) {
      *(slash + 1) = 0;
      snprintf(out, n, "%sprobe.lua", dll);
      return;
    }
  }
  if (gCtx.modFolder[0]) {
    snprintf(out, n, "%s/probe.lua", gCtx.modFolder);
    return;
  }
  snprintf(out, n, "probe.lua");
}

i32 bpProbeReloadIfChanged(void) {
  char path[MAX_PATH];
  bpScriptPath(path, sizeof(path));

  WIN32_FILE_ATTRIBUTE_DATA fad;
  if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fad))
    return 0;

  // 忽略时间戳比较：周期性强制重载，确保能采到最新状态
  gScriptWriteTime = fad.ftLastWriteTime;

  FILE *f = fopen(path, "rb");
  if (!f) return 0;
  static char buf[65536];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);

  bpLog("probe script reloaded (%d bytes)", (int)n);
  bpLuaQueueScript(buf);
  return 1;
}

static void queueProbeScript(void) {
  char path[MAX_PATH];
  bpScriptPath(path, sizeof(path));

  FILE *f = fopen(path, "rb");
  if (!f) {
    bpLog("no probe script at %s", path);
    return;
  }

  static char buf[65536];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  buf[n] = 0;
  fclose(f);

  rememberScriptTime(path);
  memcpy(gScriptCache, buf, n + 1);
  gScriptCached = 1;
  bpLog("probe script loaded (%d bytes)", (int)n);
  bpLuaQueueScript(buf);
}

void bpProbeInit(void) {
  HMODULE hGame = GetModuleHandleA(NULL);
  gCtx.skyBase = hGame;

  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)hGame;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)hGame + dos->e_lfanew);
  gCtx.skyImageSize = nt->OptionalHeader.SizeOfImage;

  bpLog("Sky.exe base=%p size=0x%X", hGame, gCtx.skyImageSize);

  dumpModules();

  // 验证 HTSigScan 可用
  static const HTAsmSig probeSig = {
    .sig = "48 89 5C 24 ?  57  48 83 EC",
    .indirect = HT_SCAN_DIRECT,
    .offset = 0
  };
  LPVOID hit = HTSigScan(&probeSig);
  bpLog("HTSigScan probe -> %p (+0x%llX)", hit,
    hit ? (u64)((u8 *)hit - (u8 *)hGame) : 0ULL);

  bpLuaSetResultHook(onLuaResult);
  (void)bpClosureAnalyze;
  bpLuaInit();
  queueProbeScript();
}
