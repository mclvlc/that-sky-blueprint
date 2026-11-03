// ---------------------------------------------------------------------------
// 手动 inline hook：绕开 MinHook 的指令边界问题。
//
// 目标序言通常是:
//   B8 38 00 00 00   mov eax, 0x38     ← 恰好 5 字节，与 jmp rel32 同长
//   E8 ?? ?? ?? ??   call __chkstk     ← MinHook 复制 6 字节会把这个 call 截断
// 因此只覆盖前 5 字节，trampoline 里只放那条 mov + 跳回。
//
// 目标与 mod DLL 通常相距 > 2GB，直接 jmp rel32 会溢出。中间加 relay：
// 目标 jmp rel32 -> relay（分配在目标 ±2GB 内），relay 用 FF 25 绝对跳转到 detour。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp.h"

#define BP_RELAY_OFF 16

static u8 *g_tramp = NULL;         // 保存原始 5 字节 + jmp 回
static u8 *g_target = NULL;
static void *g_detour = NULL;
static i32 g_installed = 0;

static i64 dist(i64 a, i64 b) {
  i64 d = a - b;
  return d < 0 ? -d : d;
}

// 在目标 ±2GB 内分配一小块可执行内存（rel32 可达）
static u8 *allocNear(u8 *target, size_t size) {
  u8 *p = (u8 *)VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE,
                             PAGE_EXECUTE_READWRITE);
  if (p && dist((i64)p, (i64)target) < 0x70000000LL)
    return p;
  if (p)
    VirtualFree(p, 0, MEM_RELEASE);

  static const i64 hints[] = {
    0x10000000LL, -0x10000000LL, 0x20000000LL, -0x20000000LL,
    0x08000000LL, -0x08000000LL, 0x30000000LL, -0x30000000LL,
    0x04000000LL, -0x04000000LL
  };
  for (size_t i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
    u8 *hint = (u8 *)((i64)target + hints[i]);
    p = (u8 *)VirtualAlloc(hint, size, MEM_COMMIT | MEM_RESERVE,
                           PAGE_EXECUTE_READWRITE);
    if (p && dist((i64)p, (i64)target) < 0x70000000LL)
      return p;
    if (p)
      VirtualFree(p, 0, MEM_RELEASE);
  }
  return NULL;
}

// 安装并返回 trampoline；不记录全局状态（支持同时挂多个目标）
static void *installCore(void *target, void *detour) {
  u8 *t = (u8 *)target;

  u8 *mem = allocNear(t, 64);
  if (!mem)
    return NULL;

  memcpy(mem, t, 5);

  mem[5] = 0xE9;
  i64 back = (i64)((u8 *)target + 5) - (i64)(mem + 10);
  *(i32 *)(mem + 6) = (i32)back;

  u8 *relay = mem + BP_RELAY_OFF;
  relay[0] = 0xFF;
  relay[1] = 0x25;
  *(u32 *)(relay + 2) = 0;                 // jmp qword ptr [rip+0]
  *(u64 *)(relay + 6) = (u64)detour;       // 绝对地址

  FlushInstructionCache(GetCurrentProcess(), mem, 64);

  DWORD oldProt = 0;
  if (!VirtualProtect(t, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
    VirtualFree(mem, 0, MEM_RELEASE);
    return NULL;
  }

  t[0] = 0xE9;
  i64 fwd = (i64)relay - (i64)(t + 5);
  *(i32 *)(t + 1) = (i32)fwd;

  FlushInstructionCache(GetCurrentProcess(), t, 5);
  VirtualProtect(t, 5, oldProt, &oldProt);

  return mem;                        // 调用方把它当 "orig" 用
}

void *bpManualHookInstall(void *target, void *detour) {
  void *mem = installCore(target, detour);
  if (!mem)
    return NULL;
  g_tramp = (u8 *)mem;
  g_target = (u8 *)target;
  g_detour = detour;
  g_installed = 1;
  return mem;
}

void *bpManualHookInstallSlot(void *target, void *detour) {
  return installCore(target, detour);
}

void bpManualHookUninstall(void) {
  if (!g_installed || !g_target || !g_tramp)
    return;
  DWORD oldProt = 0;
  if (VirtualProtect(g_target, 5, PAGE_EXECUTE_READWRITE, &oldProt)) {
    memcpy(g_target, g_tramp, 5);    // 还原原始指令
    FlushInstructionCache(GetCurrentProcess(), g_target, 5);
    VirtualProtect(g_target, 5, oldProt, &oldProt);
  }
  VirtualFree(g_tramp, 0, MEM_RELEASE);
  g_tramp = NULL;
  g_installed = 0;
}
