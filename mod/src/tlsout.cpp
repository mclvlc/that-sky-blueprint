// ---------------------------------------------------------------------------
// 出站写路径分析（0xDB670）
//
// 栈回溯已证明：0xDB670 -> OpenSSL写入(0x1D45C60) -> BIO_write -> send
// 它是出站方向中可安全 hook 的最上层，因此在这里探查参数结构，
// 目标是找到「未加密的 HTTP 明文缓冲」。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "bp.h"

#define BP_TLSEND_RVA 0xDB670
#define BP_OUT_SAMPLES 6

typedef u64 (*PFN_TlsSend)(u64, u64, u64);
static PFN_TlsSend g_origTlsSend = NULL;

static volatile u32 gTlsSendCalls = 0;
static volatile u32 gTlsSendSaved = 0;

typedef struct {
  u64 a1, a2, a3;
  u64 q[12];        // a2 指向的前 12 个 qword
  u32 frames;
  void *stack[16];
} OutSample;

static OutSample *gOut = NULL;

static i32 readable(const void *p, u64 len) {
  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(p, &mbi, sizeof(mbi))) return 0;
  if (mbi.State != MEM_COMMIT) return 0;
  if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) return 0;
  u64 avail = (u64)mbi.BaseAddress + mbi.RegionSize - (u64)p;
  return avail >= len;
}

static u64 detour_TlsSend(u64 a1, u64 a2, u64 a3) {
  gTlsSendCalls++;

  // 只记录标量参数，不做栈回溯、不读内存（那些在渲染/网络线程里不安全）
  if (gOut && gTlsSendSaved < BP_OUT_SAMPLES) {
    u32 idx = gTlsSendSaved;
    gTlsSendSaved = idx + 1;
    OutSample *s = &gOut[idx];
    s->a1 = a1;
    s->a2 = a2;
    s->a3 = a3;
    s->frames = 0;
  }

  return g_origTlsSend ? g_origTlsSend(a1, a2, a3) : 0;
}

void bpTlsOutReport(void) {
  bpLog("TlsSend calls=%u saved=%u", gTlsSendCalls, gTlsSendSaved);
  HMODULE hGame = GetModuleHandleA(NULL);

  for (u32 i = 0; i < gTlsSendSaved; i++) {
    OutSample *s = &gOut[i];
    bpLog("  #%u a1=%p a2=%p a3=%p (a3 as int=%d)",
      i, (void *)s->a1, (void *)s->a2, (void *)s->a3, (int)s->a3);
    (void)hGame;
  }
}

void bpTlsOutInit(void) {
#if BP_SAFE_MODE
  bpLog("SAFE MODE: TlsSend hook skipped");
  return;
#endif
  HMODULE hGame = GetModuleHandleA(NULL);
  u8 *addr = (u8 *)hGame + BP_TLSEND_RVA;

  if (!gOut)
    gOut = (OutSample *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
      sizeof(OutSample) * BP_OUT_SAMPLES);
  if (!gOut) { bpLog("out: alloc failed"); return; }

  bpLog("TlsSend@%p bytes: %02X %02X %02X %02X", addr, addr[0], addr[1], addr[2], addr[3]);
  static const u8 expect[] = { 0x55, 0x56, 0x57, 0x48, 0x81, 0xEC };
  if (memcmp(addr, expect, sizeof(expect)) != 0) {
    bpLog("TlsSend prologue MISMATCH -> skip hook");
    return;
  }
  HTStatus st = HTAsmHookCreateRaw(gModuleDll, addr, (LPVOID)detour_TlsSend,
    (LPVOID *)&g_origTlsSend);
  HTStatus en = (st == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, addr) : HT_FAIL;
  bpLog("TlsSend hook create=%d enable=%d orig=%p", (int)st, (int)en, g_origTlsSend);
}
