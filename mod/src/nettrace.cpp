// ---------------------------------------------------------------------------
// 网络栈回溯：hook ws2_32!send，在加密前抓调用栈。
// 调用链大致是  游戏HTTP客户端 -> SSL_write -> BIO_write -> send
// 因此栈回溯能直接把 HTTP 客户端代码地址暴露出来。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "bp.h"

#define BP_STACK_DEPTH 20
#define BP_STACK_SAMPLES 4

typedef int (WINAPI *PFN_send)(SOCKET, const char *, int, int);
static PFN_send g_origSend = NULL;

static volatile u32 gSendCount = 0;
static volatile u32 gSendSampleIdx = 0;
static void *gSendStack[BP_STACK_SAMPLES][BP_STACK_DEPTH];
static volatile u32 gSendFrames[BP_STACK_SAMPLES];
static volatile u32 gSendLen[BP_STACK_SAMPLES];
static volatile u32 gSendWsaCount = 0;

static int WINAPI detour_send(SOCKET s, const char *buf, int len, int flags) {
  gSendCount++;

  u32 idx = (u32)InterlockedIncrement((LONG *)&gSendSampleIdx) - 1;
  if (idx < BP_STACK_SAMPLES) {
    void *frames[BP_STACK_DEPTH] = {0};
    USHORT n = RtlCaptureStackBackTrace(0, BP_STACK_DEPTH, frames, NULL);
    gSendFrames[idx] = n;
    gSendLen[idx] = (u32)len;
    for (i32 i = 0; i < BP_STACK_DEPTH; i++)
      gSendStack[idx][i] = (i < n) ? frames[i] : NULL;
  }

  return g_origSend ? g_origSend(s, buf, len, flags) : -1;
}

static i32 inSkyModule(u64 p) {
  HMODULE h = GetModuleHandleA(NULL);
  if (!h || !p) return 0;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)h + dos->e_lfanew);
  u64 lo = (u64)h + 0x1000;
  u64 hi = (u64)h + nt->OptionalHeader.SizeOfImage;
  return p >= lo && p < hi;
}

typedef struct { LPVOID lpBaseOfDll; DWORD SizeOfImage; LPVOID EntryPoint; } BP_MODINFO;

const char *bpModuleOf(u64 p) {
  static char name[MAX_PATH];
  HMODULE mods[256];
  DWORD need = 0;
  HANDLE hProc = GetCurrentProcess();
  typedef BOOL (WINAPI *PFN_EPM)(HANDLE, HMODULE *, DWORD, LPDWORD);
  static PFN_EPM fnEPM = (PFN_EPM)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32EnumProcessModules");
  if (!fnEPM) fnEPM = (PFN_EPM)GetProcAddress(GetModuleHandleA("psapi.dll"), "EnumProcessModules");
  if (!fnEPM) return "?";
  if (!fnEPM(hProc, mods, sizeof(mods), &need)) return "?";

  u32 n = need / sizeof(HMODULE);
  for (u32 i = 0; i < n; i++) {
    BP_MODINFO mi;
    typedef BOOL (WINAPI *PFN_GMI)(HANDLE, HMODULE, BP_MODINFO *, DWORD);
    static PFN_GMI fnGMI = (PFN_GMI)GetProcAddress(GetModuleHandleA("kernel32.dll"), "K32GetModuleInformation");
    if (!fnGMI) fnGMI = (PFN_GMI)GetProcAddress(GetModuleHandleA("psapi.dll"), "GetModuleInformation");
    if (!fnGMI) return "?";
    if (!fnGMI(hProc, mods[i], &mi, sizeof(mi))) continue;
    u64 lo = (u64)mi.lpBaseOfDll;
    u64 hi = lo + mi.SizeOfImage;
    if (p >= lo && p < hi) {
      GetModuleFileNameA(mods[i], name, MAX_PATH);
      char *slash = strrchr(name, (char)0x5C);
      return slash ? slash + 1 : name;
    }
  }
  return "?";
}

void bpNetTraceReport(void) {
  bpLog("send() calls=%u samples=%u", gSendCount, gSendSampleIdx);

  HMODULE hGame = GetModuleHandleA(NULL);
  for (u32 s = 0; s < BP_STACK_SAMPLES && s < gSendSampleIdx; s++) {
    bpLog("--- sample #%u len=%u frames=%u", s + 1, gSendLen[s], gSendFrames[s]);
    for (u32 f = 0; f < gSendFrames[s] && f < BP_STACK_DEPTH; f++) {
      u64 addr = (u64)gSendStack[s][f];
      if (!addr) continue;
      const char *mod = bpModuleOf(addr);
      u64 rva = 0;
      if (inSkyModule(addr))
        rva = addr - (u64)hGame;
      bpLog("    [%02u] %p %-20s %s%llX", f, (void *)addr, mod,
        rva ? "rva=" : "", (unsigned long long)rva);
    }
  }
}

void bpNetTraceInit(void) {
#if BP_SAFE_MODE
  return;
#endif
  HMODULE hWs2 = GetModuleHandleA("ws2_32.dll");
  if (!hWs2) hWs2 = LoadLibraryA("ws2_32.dll");
  if (!hWs2) {
    bpLog("ws2_32 not loaded");
    return;
  }

  LPVOID target = NULL;
  HTStatus st = HTAsmHookCreateAPI(gModuleDll, L"ws2_32.dll", "send",
    (LPVOID)detour_send, (LPVOID *)&g_origSend, &target);
  HTStatus en = (st == HT_SUCCESS && target)
    ? HTAsmHookEnable(gModuleDll, target) : HT_FAIL;
  bpLog("hook send create=%d enable=%d target=%p orig=%p",
    (int)st, (int)en, target, g_origSend);
}
