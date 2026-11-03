// ---------------------------------------------------------------------------
// Lua 桥：借 sky-lua.dll 的 Lua 5.x C API，在游戏主线程执行脚本。
//
// 抓 lua_State 有两套方案：
//   1) hook 游戏 Game::Update（game+0x20）—— 默认关闭：piano-rpc 已 hook 同一
//      地址，两个独立 MinHook 实例共存会让 trampoline 互相嵌套而崩溃。
//   2) hook sky-lua.dll 自己导出的 luaL_loadstring / lua_pcallk —— 只改 mod 自己
//      的代码段，与任何游戏侧 hook 都不冲突。默认启用。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "bp.h"

#ifndef BP_ENABLE_UPDATE_HOOK
// 重要：piano-rpc 依赖 Game::Update 这个 hook 点来注册 piano.* API。
// 占用它会导致 piano-rpc 注册失败（已实测 "hook Game::Update failed: 0"），
// 从而失去「让游戏自己发声」的能力。因此必须让出，改用 PeekMessageW。
#define BP_ENABLE_UPDATE_HOOK 0
#endif

#define BP_GAME_UPDATE_RVA 0x116EC90
#define BP_GAME_LUASTATE_OFFSET 0x20

typedef struct lua_State lua_State;
typedef int (*PFN_luaL_loadstring)(lua_State *L, const char *s);
typedef int (*PFN_lua_pcallk)(lua_State *L, int nargs, int nresults, int errfunc,
                             void *ctx, void *k);
typedef const char *(*PFN_lua_tolstring)(lua_State *L, int idx, unsigned long long *len);
typedef void (*PFN_lua_settop)(lua_State *L, int idx);
typedef int (*PFN_lua_gettop)(lua_State *L);

// 原始 API（从 sky-lua.dll 解析）
static PFN_luaL_loadstring g_luaL_loadstring = NULL;
static PFN_lua_pcallk g_lua_pcall = NULL;
static PFN_lua_tolstring g_lua_tolstring = NULL;
static PFN_lua_settop g_lua_settop = NULL;
static PFN_lua_gettop g_lua_gettop = NULL;

// hook 后的跳板
static PFN_luaL_loadstring g_origLoadstring = NULL;
static PFN_lua_pcallk g_origPcall = NULL;

static lua_State *g_luaState = NULL;
static void *g_gamePtr = NULL;
static volatile u32 gUpdateTicks = 0;
static volatile u32 gLuaHookHits = 0;

static char gPendingScript[65536];
static volatile i32 gPendingFlag = 0;
static i32 gScriptRunCount = 0;
static volatile i32 gInDetour = 0;

static void (*gResultCb)(const char *result) = NULL;

// ---------------------------------------------------------------------------

static i32 resolveLuaApi(void) {
  HMODULE hSkyLua = GetModuleHandleA("sky-lua.dll");
  if (!hSkyLua) {
    bpLog("lua: sky-lua.dll not loaded");
    return 0;
  }

  g_luaL_loadstring = (PFN_luaL_loadstring)GetProcAddress(hSkyLua, "luaL_loadstring");
  g_lua_pcall = (PFN_lua_pcallk)GetProcAddress(hSkyLua, "lua_pcallk");
  g_lua_tolstring = (PFN_lua_tolstring)GetProcAddress(hSkyLua, "lua_tolstring");
  g_lua_settop = (PFN_lua_settop)GetProcAddress(hSkyLua, "lua_settop");
  g_lua_gettop = (PFN_lua_gettop)GetProcAddress(hSkyLua, "lua_gettop");
  if (!g_lua_pcall)
    g_lua_pcall = (PFN_lua_pcallk)GetProcAddress(hSkyLua, "lua_pcall");

  if (!g_luaL_loadstring || !g_lua_pcall || !g_lua_tolstring || !g_lua_settop) {
    bpLog("lua: API missing (loadstring=%p pcall=%p tolstring=%p settop=%p)",
      g_luaL_loadstring, g_lua_pcall, g_lua_tolstring, g_lua_settop);
    return 0;
  }

  bpLog("lua: API resolved");
  return 1;
}

static void runScriptNow(const char *src) {
  lua_State *L = g_luaState;
  if (!L) {
    bpLog("lua: no state yet");
    return;
  }

  int base = g_lua_gettop ? g_lua_gettop(L) : 0;
  int rc = g_luaL_loadstring(L, src);
  if (rc != 0) {
    const char *err = g_lua_tolstring(L, -1, NULL);
    bpLog("lua: load error (%d): %s", rc, err ? err : "?");
    if (gResultCb) gResultCb(err ? err : "load error");
    g_lua_settop(L, base);
    return;
  }

  rc = g_lua_pcall(L, 0, 1, 0, NULL, NULL);
  if (rc != 0) {
    const char *err = g_lua_tolstring(L, -1, NULL);
    bpLog("lua: pcall error (%d): %s", rc, err ? err : "?");
    if (gResultCb) gResultCb(err ? err : "pcall error");
    g_lua_settop(L, base);
    return;
  }

  const char *res = g_lua_tolstring(L, -1, NULL);
  bpLog("lua: script done, result %d bytes", res ? (int)strlen(res) : 0);
  if (gResultCb) gResultCb(res ? res : "");
  g_lua_settop(L, base);
  gScriptRunCount++;
}

// ---------------------------------------------------------------------------
// sky-lua.dll 导出函数 hook
// ---------------------------------------------------------------------------

static int detour_loadstring(lua_State *L, const char *s) {
  gLuaHookHits++;
  if (L)
    g_luaState = L;

#if BP_ENABLE_LUA_EXEC
  if (gPendingFlag && L && !gInDetour) {
    gInDetour = 1;
    gPendingFlag = 0;
    runScriptNow(gPendingScript);
    gInDetour = 0;
  }
#endif

  return g_origLoadstring ? g_origLoadstring(L, s) : -1;
}

static int detour_pcallk(lua_State *L, int a, int b, int c, void *d, void *e) {
  gLuaHookHits++;
  if (L)
    g_luaState = L;
  return g_origPcall ? g_origPcall(L, a, b, c, d, e) : -1;
}

// ---------------------------------------------------------------------------
// 主线程执行点：hook user32!PeekMessageW。
// 游戏的 Windows 消息泵运行在主线程，Lua 的所有调用都放到这里执行，
// 避免在 ImGui 渲染线程并发访问 lua_State 导致崩溃。
// ---------------------------------------------------------------------------

typedef BOOL (WINAPI *PFN_PeekMessageW)(LPMSG, HWND, UINT, UINT, UINT);
static PFN_PeekMessageW g_origPeekMessage = NULL;
static volatile u32 gPumpTicks = 0;

static BOOL WINAPI detour_PeekMessageW(LPMSG msg, HWND hwnd, UINT mn, UINT mx, UINT rm) {
  gPumpTicks++;
  if (gPumpTicks <= 3)
    bpLog("PeekMessageW hit #%u", gPumpTicks);

  // lua_State 可能晚于本 mod 初始化，持续低频重扫直到捕获
  if (!g_luaState && (gPumpTicks % 90) == 0)
    bpLuaScanState();

  // 定期输出采集报告
  if (gPumpTicks == 800 || gPumpTicks == 4000 || gPumpTicks == 12000) {
    bpTlsDumpReport();
    bpTlsOutReport();
  }

  // 周期性重新排队探测脚本 + 看门狗（RenderGui 不是每帧调用，不可靠，改在这里）
  if (gPumpTicks % 300 == 0) {
    bpLuaWatchdog();
    bpProbeRequeue();
  }

  if (gPumpTicks == 600 || gPumpTicks == 2400) {
    bpWriteRpcReport();
    bpNetTraceReport();
    bpTlsDumpReport();
  }

#if BP_ENABLE_LUA_EXEC
  if (!gInDetour && gPendingFlag && g_luaState && g_luaL_loadstring) {
    gInDetour = 1;
    gPendingFlag = 0;
    bpLog("exec on pump thread, tick=%u", gPumpTicks);
    runScriptNow(gPendingScript);
    gInDetour = 0;
  }
#endif

  return g_origPeekMessage ? g_origPeekMessage(msg, hwnd, mn, mx, rm) : FALSE;
}

// ---------------------------------------------------------------------------
// Game::Update hook（默认禁用）
// ---------------------------------------------------------------------------

typedef void (*PFN_GameUpdate)(u64, u64, u64, u64);
static PFN_GameUpdate g_origGameUpdate = NULL;

static void detour_GameUpdate(u64 a1, u64 a2, u64 a3, u64 a4) {
  gUpdateTicks++;
  if (gUpdateTicks <= 3)
    bpLog("Game::Update hit #%u a1=%p", gUpdateTicks, (void *)a1);

  // 用 Update 计数驱动一次性报告（每帧必调，比 PeekMessageW 可靠）
  if (gUpdateTicks >= 400 && (gUpdateTicks % 1500) == 400) {
    bpTlsDumpReport();
    bpTlsOutReport();
  }
  if (!g_gamePtr && a1 > 0x10000 && a1 < 0x00007FFFFFFFFFFFULL)
    g_gamePtr = (void *)a1;
  if (g_gamePtr && !g_luaState) {
    void *cand = *(void **)((u8 *)g_gamePtr + BP_GAME_LUASTATE_OFFSET);
    if (cand && (u64)cand > 0x10000 && (u64)cand < 0x00007FFFFFFFFFFFULL)
      g_luaState = (lua_State *)cand;
  }
#if BP_ENABLE_LUA_EXEC
  if (gPendingFlag && g_luaState && !gInDetour) {
    gInDetour = 1;
    gPendingFlag = 0;
    runScriptNow(gPendingScript);
    gInDetour = 0;
  }
#endif
  if (g_origGameUpdate)
    g_origGameUpdate(a1, a2, a3, a4);
}

// ---------------------------------------------------------------------------
// 扫描 sky-lua.dll 的数据段，按 Lua 5.x lua_State 的结构特征定位 gGameLuaState。
//   +0x08 = tt   == LUA_TTHREAD(8)
//   +0x10 = top      栈顶指针（有效内存）
//   +0x18 = l_G      global_State*（有效内存）
// ---------------------------------------------------------------------------

static u64 gLastScanTick = 0;

void bpLuaScanState(void) {
  HMODULE h = GetModuleHandleA("sky-lua.dll");
  if (!h) return;

  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)h + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);

  i32 found = 0;
  for (i32 i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
    if (!(sec->Characteristics & IMAGE_SCN_MEM_WRITE))
      continue;

    u8 *base = (u8 *)h + sec->VirtualAddress;
    u32 size = sec->Misc.VirtualSize;
    if (size == 0 || size > 0x2000000)
      continue;

    for (u32 off = 0; off + 8 <= size; off += 8) {
      u64 v = *(u64 *)(base + off);
      if (v < 0x10000ULL || v > 0x00007FFFFFFFFFFFULL || (v & 7))
        continue;

      MEMORY_BASIC_INFORMATION mbi;
      if (!VirtualQuery((LPCVOID)v, &mbi, sizeof(mbi)))
        continue;
      if (mbi.State != MEM_COMMIT)
        continue;
      if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                           PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
        continue;

      u8 *p = (u8 *)v;
      if (p[8] != 8)             // LUA_TTHREAD
        continue;

      u64 top = *(u64 *)(p + 0x10);
      u64 g = *(u64 *)(p + 0x18);
      if (top < 0x10000ULL || top > 0x00007FFFFFFFFFFFULL)
        continue;
      if (g < 0x10000ULL || g > 0x00007FFFFFFFFFFFULL)
        continue;

      bpLog("luaState cand %p (slot=%p+0x%X top=%p G=%p)",
        p, base, off, (void *)top, (void *)g);
      if (!g_luaState)
        g_luaState = (lua_State *)p;
      found++;
      if (found >= 6)
        break;
    }
    if (found >= 6)
      break;
  }
  bpLog("luaState scan done: %d candidate(s), chosen=%p", found, g_luaState);
}

// ---------------------------------------------------------------------------
// 主线程执行点（方案 B）：Game::Update 已被 piano-rpc hook，读它的 jmp 目标，
// 再 hook 那个 detour 函数 —— 相当于搭它的便车，在同一线程、同一时机执行脚本。
// ---------------------------------------------------------------------------

typedef void (*PFN_UpdateDetour)(u64, u64, u64, u64);
static PFN_UpdateDetour g_origOtherDetour = NULL;
static volatile u32 gOtherTicks = 0;

static void detour_OtherUpdate(u64 a1, u64 a2, u64 a3, u64 a4) {
  gOtherTicks++;
  if (gOtherTicks <= 3)
    bpLog("other-detour hit #%u", gOtherTicks);

  if (!gInDetour && gPendingFlag && g_luaState && g_luaL_loadstring) {
    gInDetour = 1;
    gPendingFlag = 0;
    runScriptNow(gPendingScript);
    gInDetour = 0;
  }

  if (g_origOtherDetour)
    g_origOtherDetour(a1, a2, a3, a4);
}

static void hookExistingUpdateDetour(void) {
  HMODULE hGame = GetModuleHandleA(NULL);
  u8 *p = (u8 *)hGame + BP_GAME_UPDATE_RVA;
  bpLog("Game::Update bytes: %02X %02X %02X %02X %02X %02X",
    p[0], p[1], p[2], p[3], p[4], p[5]);

  void *detour = NULL;
  if (p[0] == 0xE9) {
    i32 rel = *(i32 *)(p + 1);
    detour = (void *)(p + 5 + rel);
  } else if (p[0] == 0xFF && p[1] == 0x25) {
    i32 rel = *(i32 *)(p + 2);
    detour = *(void **)(p + 6 + rel);
  }

  if (!detour) {
    bpLog("no existing detour on Game::Update (bytes not jmp)");
    return;
  }
  bpLog("existing Game::Update detour at %p", detour);

  HTStatus st = HTAsmHookCreateRaw(gModuleDll, detour,
    (LPVOID)detour_OtherUpdate, (LPVOID *)&g_origOtherDetour);
  HTStatus en = (st == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, detour) : HT_FAIL;
  bpLog("hook existing detour create=%d enable=%d (orig=%p)",
    (int)st, (int)en, g_origOtherDetour);
}

// ---------------------------------------------------------------------------
// WriteRpc hook：网络 RPC 序列化入口（piano-rpc 日志验证过的锚点 0x1BC3F0）。
// 用它观察留影 / 留言 / 共享空间相关 RPC 的实际流量。
// ---------------------------------------------------------------------------

#define BP_WRITE_RPC_RVA 0x1BC3F0

typedef void (*PFN_WriteRpc)(u64, u64, u64, u64);
static PFN_WriteRpc g_origWriteRpc = NULL;
static volatile u32 gWriteRpcCount = 0;
static volatile u32 gWriteRpcLogged = 0;
static volatile u64 gWriteRpcSample[6][12];   // this + 前 10 个 qword + count

static void detour_WriteRpc(u64 a1, u64 a2, u64 a3, u64 a4) {
  // 性能敏感路径：只做内存计数，绝不做文件 I/O
  gWriteRpcCount++;
  if (gWriteRpcLogged < 6 && a2 > 0x10000 && a2 < 0x00007FFFFFFFFFFFULL) {
    u32 idx = gWriteRpcLogged;
    gWriteRpcLogged++;
    // 记录 a1/a2 及 a2 指向的前 10 个 qword，留待主线程解析
    gWriteRpcSample[idx][0] = a1;
    gWriteRpcSample[idx][1] = a2;
    u64 *q = (u64 *)a2;
    for (i32 i = 0; i < 10; i++)
      gWriteRpcSample[idx][2 + i] = q[i];
  }
  if (g_origWriteRpc)
    g_origWriteRpc(a1, a2, a3, a4);
}

static void hookWriteRpc(void) {
  HMODULE hGame = GetModuleHandleA(NULL);
  u8 *addr = (u8 *)hGame + BP_WRITE_RPC_RVA;
  bpLog("WriteRpc@%p bytes: %02X %02X %02X %02X %02X %02X",
    addr, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

  if (addr[0] == 0xE9 || (addr[0] == 0xFF && addr[1] == 0x25)) {
    bpLog("WriteRpc already hooked by another mod, skip");
    return;
  }
  static const u8 expect[] = { 0x55, 0x41, 0x57, 0x41, 0x56, 0x41 };
  if (memcmp(addr, expect, sizeof(expect)) != 0) {
    bpLog("WriteRpc prologue MISMATCH -> skip hook");
    return;
  }

  HTStatus st = HTAsmHookCreateRaw(gModuleDll, addr, (LPVOID)detour_WriteRpc,
    (LPVOID *)&g_origWriteRpc);
  HTStatus en = (st == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, addr) : HT_FAIL;
  bpLog("WriteRpc hook create=%d enable=%d orig=%p",
    (int)st, (int)en, g_origWriteRpc);
}

// ---------------------------------------------------------------------------
// 对外
// ---------------------------------------------------------------------------

void bpLuaForceReset(void) {
  gInDetour = 0;
}

void bpLuaQueueScript(const char *src) {
#if !BP_ENABLE_LUA_EXEC
  (void)src;
  return;   // 安全模式：不排队任何脚本
#else
  if (!src) return;
  strncpy(gPendingScript, src, sizeof(gPendingScript) - 1);
  gPendingScript[sizeof(gPendingScript) - 1] = 0;
  gPendingFlag = 1;
#endif
}

void bpLuaSetResultHook(void (*cb)(const char *result)) {
  gResultCb = cb;
}

void bpWriteRpcReport(void) {
  if (gWriteRpcLogged == 0) {
    bpLog("WriteRpc: no sample yet (count=%u)", gWriteRpcCount);
    return;
  }
  bpLog("WriteRpc total=%u, samples:", gWriteRpcCount);
  HMODULE hGame = GetModuleHandleA(NULL);
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)hGame;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)hGame + dos->e_lfanew);
  u64 imgLo = (u64)hGame + 0x1000;
  u64 imgHi = (u64)hGame + nt->OptionalHeader.SizeOfImage;

  for (u32 i = 0; i < gWriteRpcLogged; i++) {
    bpLog("  #%u this=%p rpc=%p", i + 1,
      (void *)gWriteRpcSample[i][0], (void *)gWriteRpcSample[i][1]);
    for (i32 k = 0; k < 10; k++) {
      u64 v = gWriteRpcSample[i][2 + k];
      if (v >= imgLo && v < imgHi) {
        // 落在模块内：可能是字符串指针，试着读出来
        char buf[80] = {0};
        u8 *sp = (u8 *)v;
        i32 n = 0;
        for (; n < 64 && sp[n] >= 0x20 && sp[n] < 0x7F; n++)
          buf[n] = (char)sp[n];
        buf[n] = 0;
        if (n >= 3)
          bpLog("      [+%02X] %p -> \"%s\"", k * 8, (void *)v, buf);
      }
    }
  }
}

i32 bpLuaHasState(void) {
  return g_luaState != NULL;
}

i32 bpLuaPending(void) {
  return gPendingFlag;
}

// 看门狗：若 gInDetour 卡住（脚本异常未复位），强制清除
void bpLuaWatchdog(void) {
  static u32 stuck = 0;
  if (gInDetour) {
    if (++stuck > 30) {          // 约 1 秒
      bpLog("watchdog: gInDetour stuck, force reset");
      gInDetour = 0;
      stuck = 0;
    }
  } else {
    stuck = 0;
  }
  // 若 pending 长期未被消费，也重新置位以便下次执行
  if (gPendingFlag && !gInDetour && g_luaState) {
    static u32 pendStuck = 0;
    if (++pendStuck > 60) {
      pendStuck = 0;
    }
  }
}

// 自检：执行 "return tostring(2+2)"，成功返回 1。
i32 bpLuaSelfTest(void) {
  if (!g_luaState || !g_luaL_loadstring)
    return -1;

  int base = g_lua_gettop ? g_lua_gettop(g_luaState) : 0;
  if (g_luaL_loadstring(g_luaState, "return tostring(2+2)") != 0) {
    const char *e = g_lua_tolstring ? g_lua_tolstring(g_luaState, -1, NULL) : "?";
    bpLog("selftest load err: %s", e ? e : "?");
    g_lua_settop(g_luaState, base);
    return -2;
  }
  if (g_lua_pcall(g_luaState, 0, 1, 0, NULL, NULL) != 0) {
    const char *e = g_lua_tolstring ? g_lua_tolstring(g_luaState, -1, NULL) : "?";
    bpLog("selftest pcall err: %s", e ? e : "?");
    g_lua_settop(g_luaState, base);
    return -3;
  }
  const char *r = g_lua_tolstring ? g_lua_tolstring(g_luaState, -1, NULL) : NULL;
  bpLog("selftest result: %s", r ? r : "(null)");
  g_lua_settop(g_luaState, base);

  if (gResultCb) {
    char msg[256];
    snprintf(msg, sizeof(msg),
      "[staged] selftest ok=%d|state=%p|", (r && strcmp(r, "4") == 0), (void *)g_luaState);
    gResultCb(msg);
  }
  return (r && strcmp(r, "4") == 0) ? 1 : 0;
}

// 立刻执行排队中的脚本
i32 bpLuaRunPending(void) {
  if (!gPendingFlag || !g_luaState)
    return 0;
  if (gInDetour)
    return 0;
  gInDetour = 1;
  gPendingFlag = 0;
  runScriptNow(gPendingScript);
  gInDetour = 0;
  return 1;
}

void bpLuaStatus(char *buf, int len) {
  snprintf(buf, len,
    "state=%p scripts=%d pending=%d pump=%u other=%u",
    g_luaState, gScriptRunCount, gPendingFlag, gPumpTicks, gOtherTicks);
}

void bpLuaInit(void) {
#if BP_SAFE_MODE
  bpLog("SAFE MODE: all hooks skipped (0.35.2 address baseline rebuild pending)");
  {
    // 只做只读校验：打印各候选地址的字节，便于对比新旧版本
    HMODULE hGame = GetModuleHandleA(NULL);
    static const struct { const char *name; u32 rva; } probes[] = {
      { "Game::Update(old)", 0x116EC90 },
      { "Game::Update(new)", 0x116ECA0 },
      { "WriteRpc(old)",     0x1BC3F0 },
      { "TlsSend(old)",      0x0DB670 },
      { "SSL_read(old)",     0x1D45CB0 },
      { "SSL_write(old)",    0x1D45D10 },
    };
    for (i32 i = 0; i < (i32)(sizeof(probes)/sizeof(probes[0])); i++) {
      u8 *p2 = (u8 *)hGame + probes[i].rva;
      bpLog("probe %-18s rva=%07X bytes: %02X %02X %02X %02X %02X %02X",
        probes[i].name, probes[i].rva, p2[0], p2[1], p2[2], p2[3], p2[4], p2[5]);
    }
  }
  return;
#endif
  resolveLuaApi();

#if BP_ENABLE_UPDATE_HOOK
  HMODULE hGame = GetModuleHandleA(NULL);
  LPVOID addr = (LPVOID)((u8 *)hGame + BP_GAME_UPDATE_RVA);
  u8 *p = (u8 *)addr;
  bpLog("Game::Update cand %p : %02X %02X %02X %02X", p, p[0], p[1], p[2], p[3]);
  HTStatus st = HTAsmHookCreateRaw(gModuleDll, addr, (LPVOID)detour_GameUpdate,
    (LPVOID *)&g_origGameUpdate);
  HTStatus sen = (st == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, addr) : HT_FAIL;
  bpLog("Game::Update hook create=%d enable=%d origin=%p",
    (int)st, (int)sen, g_origGameUpdate);
  u8 *after = (u8 *)addr;
  bpLog("Game::Update bytes after hook: %02X %02X %02X %02X %02X",
    after[0], after[1], after[2], after[3], after[4]);
#else
  bpLog("Game::Update hook disabled (would conflict with piano-rpc)");
#endif

  if (!g_luaL_loadstring)
    return;

  // hook sky-lua.dll 自己的导出函数（只改 mod 自己的代码段）
  HTStatus s1 = HTAsmHookCreateRaw(gModuleDll, (LPVOID)g_luaL_loadstring,
    (LPVOID)detour_loadstring, (LPVOID *)&g_origLoadstring);
  HTStatus e1 = (s1 == HT_SUCCESS)
    ? HTAsmHookEnable(gModuleDll, (LPVOID)g_luaL_loadstring) : HT_FAIL;
  bpLog("hook luaL_loadstring create=%d enable=%d (orig=%p)",
    (int)s1, (int)e1, g_origLoadstring);

  if (g_lua_pcall && (void *)g_lua_pcall != (void *)g_luaL_loadstring) {
    HTStatus s2 = HTAsmHookCreateRaw(gModuleDll, (LPVOID)g_lua_pcall,
      (LPVOID)detour_pcallk, (LPVOID *)&g_origPcall);
    HTStatus e2 = (s2 == HT_SUCCESS)
      ? HTAsmHookEnable(gModuleDll, (LPVOID)g_lua_pcall) : HT_FAIL;
    bpLog("hook lua_pcallk create=%d enable=%d (orig=%p)",
      (int)s2, (int)e2, g_origPcall);
  }

  // 安装主线程执行点
  HMODULE hUser32 = GetModuleHandleA("user32.dll");
  if (!hUser32)
    hUser32 = LoadLibraryA("user32.dll");
  if (hUser32) {
    LPVOID target = NULL;
    HTStatus sp = HTAsmHookCreateAPI(gModuleDll, L"user32.dll", "PeekMessageW",
      (LPVOID)detour_PeekMessageW, (LPVOID *)&g_origPeekMessage, &target);
    HTStatus ep = (sp == HT_SUCCESS && target)
      ? HTAsmHookEnable(gModuleDll, target) : HT_FAIL;
    bpLog("hook PeekMessageW create=%d enable=%d (target=%p orig=%p)",
      (int)sp, (int)ep, target, g_origPeekMessage);
  } else {
    bpLog("user32.dll not found, no main-thread pump hook");
  }

  // 只有当 Game::Update 的 hook 没成功时，才去搭别人的便车
  if (!g_origGameUpdate)
    hookExistingUpdateDetour();

  hookWriteRpc();
  bpNetTraceInit();
  bpTlsDumpInit();
  // 出站明文（0x1D45F80）：手动 5 字节 hook，detour 在 orig 之前抓缓冲。
  // 之前的崩溃是「orig 返回后再读缓冲」导致野指针，现已修正。
  bpTlsOutHookInit();
  bpTlsOutInit();

  bpLuaScanState();
}
