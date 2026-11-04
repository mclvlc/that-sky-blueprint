#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include "includes/htmod.h"
#include "bp.h"
#include "i18n.h"

static i32 gModLoaded = 0;
static CRITICAL_SECTION gLogLock;
static i32 gLogLockInit = 0;
static HTHandle gHotkeyToggle = HT_INVALID_HANDLE;
static HTHandle gHotkeyF3 = HT_INVALID_HANDLE;
static char gLogPath[MAX_PATH] = {0};

// ---------------------------------------------------------------------------
// 日志（内存 + 文件双写，便于外部观察）
// ---------------------------------------------------------------------------

static void makeDirs(const char *path) {
  char tmp[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s", path);
  for (char *p = tmp + 3; *p; p++) {
    if (*p == '\\' || *p == '/') {
      char c = *p;
      *p = 0;
      CreateDirectoryA(tmp, NULL);
      *p = c;
    }
  }
  CreateDirectoryA(tmp, NULL);
}

static int dirWritable(const char *dir) {
  char probe[MAX_PATH];
  snprintf(probe, sizeof(probe), "%s\\.write_test", dir);
  FILE *f = fopen(probe, "wb");
  if (!f) return 0;
  fclose(f);
  remove(probe);
  return 1;
}

// 运行期数据目录：%APPDATA%\sky-blueprint\<steam|cn>，失败时退回 <mod>\data
static void initFolders(void) {
  char loaderMods[MAX_PATH] = {0};
  HTGetModFolder(loaderMods, sizeof(loaderMods));
  if (loaderMods[0])
    snprintf(gCtx.modFolder, sizeof(gCtx.modFolder), "%s/sky-blueprint", loaderMods);

  const char *appdata = getenv("APPDATA");
#if BP_CN_LIVE || BP_CN_DIAG
  const char *edition = "cn";
#else
  const char *edition = "steam";
#endif

  char want[MAX_PATH] = {0};
  if (appdata && appdata[0])
    snprintf(want, sizeof(want), "%s\\sky-blueprint\\%s", appdata, edition);

  if (want[0]) {
    makeDirs(want);
    if (dirWritable(want))
      snprintf(gCtx.dataFolder, sizeof(gCtx.dataFolder), "%s", want);
  }
  if (!gCtx.dataFolder[0] && gCtx.modFolder[0]) {
    snprintf(gCtx.dataFolder, sizeof(gCtx.dataFolder), "%s\\data", gCtx.modFolder);
    makeDirs(gCtx.dataFolder);
  }

  if (gCtx.dataFolder[0]) {
    snprintf(gCtx.capFolder, sizeof(gCtx.capFolder), "%s\\captures", gCtx.dataFolder);
    snprintf(gCtx.recFolder, sizeof(gCtx.recFolder), "%s\\records", gCtx.dataFolder);
    makeDirs(gCtx.capFolder);
    makeDirs(gCtx.recFolder);
  }

  if (gCtx.dataFolder[0])
    snprintf(gLogPath, sizeof(gLogPath), "%s\\bp.log", gCtx.dataFolder);
  else
    snprintf(gLogPath, sizeof(gLogPath), "bp.log");
}

int bpDumpOk(unsigned idx, unsigned cap) {
  return idx > 0 && idx <= cap;
}

// ---------------------------------------------------------------------------
// 服务器地址
//
// 国际服：启动时读 <游戏目录>\data\AppInfo.tgc 的第二行（非空行计数），
//         这一行就是客户端实际连接的服务器地址。
// 国服：地址由网易固定，读不到也不该改；用抓到的 Host（cnlive 写 server.txt）。
// 结果写到 <data>\server.txt，Python 工具部署时直接读。
// ---------------------------------------------------------------------------
void bpWriteServerHost(const char *host) {
  if (!gCtx.dataFolder[0] || !host || !host[0]) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\server.txt", gCtx.dataFolder);
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "%s\n", host);
    fclose(f);
  }
}

static void readAppInfoHost(void) {
#if BP_CN_LIVE || BP_CN_DIAG
  bpLog("server: CN build (host captured from traffic)");
  return;
#else
  char exe[MAX_PATH];
  if (!GetModuleFileNameA(NULL, exe, sizeof(exe))) return;
  char *slash = strrchr(exe, '\\');
  if (slash) *slash = 0;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\data\\AppInfo.tgc", exe);
  FILE *f = fopen(path, "rb");
  if (!f) {
    bpLog("server: AppInfo.tgc not found (%s)", path);
    return;
  }

  char line[512];
  int n = 0;
  char host[256] = {0};
  while (fgets(line, sizeof(line), f)) {
    size_t len = strlen(line);
    while (len && (line[len - 1] == '\r' || line[len - 1] == '\n' ||
                   line[len - 1] == ' ' || line[len - 1] == '\t'))
      line[--len] = 0;
    char *s = line;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) continue;
    if (++n == 2) {
      strncpy(host, s, sizeof(host) - 1);
      break;
    }
  }
  fclose(f);
  if (!host[0]) {
    bpLog("server: AppInfo.tgc has no second line");
    return;
  }

  // 允许 "https://host/path" 这种写法，只取 host
  char *p = host;
  if (strncmp(p, "https://", 8) == 0) p += 8;
  else if (strncmp(p, "http://", 7) == 0) p += 7;
  char *e = p;
  while (*e && *e != '/' && *e != ' ' && *e != '\t') e++;
  *e = 0;

  bpWriteServerHost(p);
  bpLog("server: AppInfo.tgc -> %s", p);
#endif
}

static void openLogFile(void) {
  initFolders();
  FILE *f = fopen(gLogPath, "w");
  if (f) {
    fprintf(f, "== sky-blueprint v" BP_VERSION " log start ==\n");
    fclose(f);
  }
}

void bpLog(const char *fmt, ...) {
  char line[BP_LOG_LINE];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  if (gLogLockInit)
    EnterCriticalSection(&gLogLock);

  i32 idx = gCtx.logCount % BP_MAX_LOG;
  strncpy(gCtx.log[idx], line, BP_LOG_LINE - 1);
  gCtx.log[idx][BP_LOG_LINE - 1] = 0;
  gCtx.logCount++;

  if (gLogPath[0]) {
    FILE *f = fopen(gLogPath, "a");
    if (f) {
      fprintf(f, "%s\n", line);
      fclose(f);
    }
  }

  if (gLogLockInit)
    LeaveCriticalSection(&gLogLock);
}

// ---------------------------------------------------------------------------
// 工具
// ---------------------------------------------------------------------------

static inline i32 checkLoaderVersion(void) {
  HMODULE hWinhttp = GetModuleHandleA("winhttp.dll");
  if (!hWinhttp)
    return 0;

  FARPROC fn = GetProcAddress(hWinhttp, "HTGetLoaderVersion");
  if (!fn)
    return 0;

  u32 ver = 0;
  ((void (__fastcall *)(u32 *))fn)(&ver);
  return ver >= 10801;
}

// ---------------------------------------------------------------------------
// Mod 入口
// ---------------------------------------------------------------------------

static const char *KEY_NAME_TOGGLE = "0 Toggle panel";

extern "C" __declspec(dllexport) HTStatus HTMLAPI HTModOnInit(
  void *reserved
) {
  InitializeCriticalSection(&gLogLock);
  gLogLockInit = 1;

  openLogFile();
  bpI18nInit();
  bpLog("ui lang=%d", gUiLang);

  if (!checkLoaderVersion()) {
    bpLog("ERROR: HT mod loader >= 1.8.1 required");
    return HT_FAIL;
  }

  HTGameStatus st = {0};
  HTGetGameStatus(&st);
  gCtx.gameBase = st.baseAddr;
  gCtx.pid = st.pid;
  gCtx.edition = st.edition;

  HTGetLoaderVersion(&gCtx.loaderVersion);
  HTGetLoaderVersionName(gCtx.loaderName, sizeof(gCtx.loaderName));

  gCtx.skyBase = GetModuleHandleA(NULL);
  gCtx.showMenu = 1;

  gHotkeyToggle = HTHotkeyRegister(gModuleDll, KEY_NAME_TOGGLE, HTKey_F2);
  gHotkeyF3 = HTHotkeyRegister(gModuleDll, "1 Run probe", HTKey_F3);

  bpLog("sky-blueprint v" BP_VERSION " init ok");
  bpLog("mod=%s", gCtx.modFolder);
  bpLog("data=%s", gCtx.dataFolder[0] ? gCtx.dataFolder : "(mod folder)");
  readAppInfoHost();
  bpLog("game base=%p pid=%lu edition=%d", gCtx.gameBase, gCtx.pid, (int)gCtx.edition);
  bpLog("loader=%s v%u", gCtx.loaderName, gCtx.loaderVersion);

#if BP_CN_LIVE
  bpCnLiveInit();
#elif BP_CN_DIAG
  bpCnDiagInit();
#else
  bpProbeInit();
#endif

  gModLoaded = 1;
  bpLog("init done");
  return HT_SUCCESS;
}

extern "C" __declspec(dllexport) HTStatus HTMLAPI HTModOnEnable(void *) {
  bpLog("mod enabled");
  return HT_SUCCESS;
}

extern "C" __declspec(dllexport) HTStatus HTMLAPI HTModOnDisable(void *) {
  return HT_SUCCESS;
}

// ImGui 默认字体没有中文字形，备注/留言文本会显示成 "?"。
// 从系统字体目录加载一个简体中文字体作为默认字体（只尝试一次）。
static void ensureCjkFont(void) {
  static int tried = 0;
  if (tried) return;
  tried = 1;

  ImGuiIO &io = ImGui::GetIO();
  static const char *cands[] = {
    "C:\\Windows\\Fonts\\msyh.ttc",     // 微软雅黑
    "C:\\Windows\\Fonts\\msyhbd.ttc",
    "C:\\Windows\\Fonts\\Deng.ttf",     // 等线
    "C:\\Windows\\Fonts\\simhei.ttf",   // 黑体
    "C:\\Windows\\Fonts\\simsun.ttc",   // 宋体
  };
  for (size_t i = 0; i < sizeof(cands) / sizeof(cands[0]); i++) {
    FILE *f = fopen(cands[i], "rb");
    if (!f) continue;
    fclose(f);

    ImFontConfig cfg;
    cfg.OversampleH = 2;
    cfg.OversampleV = 2;
    cfg.PixelSnapH = true;
    ImFont *font = io.Fonts->AddFontFromFileTTF(
      cands[i], 17.0f, &cfg,
      io.Fonts->GetGlyphRangesChineseSimplifiedCommon());
    if (font) {
      io.FontDefault = font;
      bpLog("cjk font loaded: %s", cands[i]);
      return;
    }
  }
  bpLog("cjk font: no system Chinese font found");
}

extern "C" __declspec(dllexport) void HTMLAPI HTModRenderGui(
  f32 timeElapesed,
  void *reserved
) {
  if (!gModLoaded)
    return;

  ensureCjkFont();

  static i32 prevPressed = 0;
  if (gHotkeyToggle != HT_INVALID_HANDLE) {
    i32 pressed = HTHotkeyPressed(gHotkeyToggle) ? 1 : 0;
    if (pressed && !prevPressed)
      gCtx.showMenu = !gCtx.showMenu;
    prevPressed = pressed;
  }

  // Lua 通道：周期性从文件重新加载并排队脚本（不依赖文件时间戳，热重载实测不可靠）。
  // 真正的执行发生在 PeekMessageW 的 detour 中（主线程）。
#if !BP_CN_DIAG && !BP_CN_LIVE
  {
    static u32 frame = 0;
    static i32 tries = 0;
    frame++;

    if (!bpLuaHasState() && tries < 20 && (frame % 30) == 0) {
      tries++;
      bpLuaScanState();
    }

#if BP_ENABLE_LUA_EXEC
    // 每约 3 秒用内存缓存重新排队一次，确保能采到用户操作后的最新状态
    // 周期重排已移到 PeekMessageW（RenderGui 不是每帧调用，不可靠）
#endif

    if ((frame % 600) == 0)
      bpLog("render tick=%u state=%d pending=%d", frame,
        (int)bpLuaHasState(), (int)bpLuaPending());

    // F3：手动立即执行一次探测脚本（不依赖周期调度）
    static i32 prevF3 = 0;
    if (gHotkeyF3 != HT_INVALID_HANDLE) {
      i32 p3 = HTHotkeyPressed(gHotkeyF3) ? 1 : 0;
      if (p3 && !prevF3) {
        bpLog("F3 pressed -> force reset + requeue");
        bpLuaForceReset();
        bpProbeRequeue();
      }
      prevF3 = p3;
    }
  }
#endif

  if (gCtx.showMenu)
    bpUIWindowMain();
}
