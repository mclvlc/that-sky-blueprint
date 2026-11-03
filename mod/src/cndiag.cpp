// ---------------------------------------------------------------------------
// 国服（NetEase 0.16.7）移植诊断
//
// 1) 只读扫描游戏镜像，寻找 OpenSSL 读写包装函数和 API 字符串
// 2) 对找到的候选函数挂「只计数 + 前后 64 字节预览」的 hook
//    - 预览里出现 GET/POST/HTTP 的，就是读/写方向
// 结果写到 <mod>/cn_scan.log
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "bp.h"

#define CN_MAX_HOOKS 8

static FILE *gScan = NULL;
static CRITICAL_SECTION gScanLock;
static int gScanLockInit = 0;

static void *gPatA[CN_MAX_HOOKS];
static int gPatACount = 0;
static void *gTramp[CN_MAX_HOOKS] = {0};
static volatile LONG gCalls[CN_MAX_HOOKS] = {0};

static void scanLog(const char *fmt, ...) {
  char line[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(line, sizeof(line), fmt, ap);
  va_end(ap);

  if (gScanLockInit) EnterCriticalSection(&gScanLock);
  if (gScan) {
    fprintf(gScan, "%s\n", line);
    fflush(gScan);
  }
  if (gScanLockInit) LeaveCriticalSection(&gScanLock);

  bpLog("%s", line);
}

// 返回 1 表示预览内容像 HTTP 请求/响应
static int preview(void *buf, int num, char *out, size_t outsz) {
  out[0] = 0;
  if (!buf || num <= 0) {
    snprintf(out, outsz, "-");
    return 0;
  }

  MEMORY_BASIC_INFORMATION mbi;
  if (!VirtualQuery(buf, &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT ||
      !(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                       PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
    snprintf(out, outsz, "(unreadable)");
    return 0;
  }

  u64 avail = (u64)mbi.BaseAddress + mbi.RegionSize - (u64)buf;
  u32 take = (u32)(num > 64 ? 64 : num);
  if ((u64)take > avail) take = (u32)avail;

  const char *p = (const char *)buf;
  size_t j = 0;
  for (u32 i = 0; i < take && j + 1 < outsz; i++)
    out[j++] = (p[i] >= 0x20 && p[i] < 0x7F) ? p[i] : '.';
  out[j] = 0;

  if (take >= 4 &&
      (memcmp(p, "GET ", 4) == 0 || memcmp(p, "POST", 4) == 0 ||
       memcmp(p, "PUT ", 4) == 0 || memcmp(p, "HTTP", 4) == 0 ||
       memcmp(p, "HEAD", 4) == 0))
    return 1;
  return 0;
}

static int handle(int idx, u64 ssl, void *buf, int num) {
  LONG n = InterlockedIncrement(&gCalls[idx]);
  char pre[96], post[96];
  int preHttp = preview(buf, num, pre, sizeof(pre));

  int ret = 0;
  if (gTramp[idx])
    ret = ((int (*)(u64, void *, int))gTramp[idx])(ssl, buf, num);

  int postHttp = preview(buf, num, post, sizeof(post));

  if (n <= 60 || preHttp || postHttp || (n % 2000) == 0) {
    scanLog("CALL%d n=%ld ssl=%p buf=%p num=%d pre%s=[%s] post%s=[%s] ret=%d",
            idx, (long)n, (void *)ssl, buf, num,
            preHttp ? "*" : "", pre, postHttp ? "*" : "", post, ret);
  }
  return ret;
}

static int det0(u64 s, void *b, int n) { return handle(0, s, b, n); }
static int det1(u64 s, void *b, int n) { return handle(1, s, b, n); }
static int det2(u64 s, void *b, int n) { return handle(2, s, b, n); }
static int det3(u64 s, void *b, int n) { return handle(3, s, b, n); }
static int det4(u64 s, void *b, int n) { return handle(4, s, b, n); }
static int det5(u64 s, void *b, int n) { return handle(5, s, b, n); }
static int det6(u64 s, void *b, int n) { return handle(6, s, b, n); }
static int det7(u64 s, void *b, int n) { return handle(7, s, b, n); }

static void *detourFor(int i) {
  switch (i) {
    case 0: return (void *)det0;
    case 1: return (void *)det1;
    case 2: return (void *)det2;
    case 3: return (void *)det3;
    case 4: return (void *)det4;
    case 5: return (void *)det5;
    case 6: return (void *)det6;
    default: return (void *)det7;
  }
}

static const char *needles[] = {
  "/service/recording/api/v2/create",
  "/service/recording/api/v2/update",
  "/service/recording/api/v2/get",
  "/service/social-feed/api/v1/create",
  "/service/social-feed/api/v1/get_user_feeds",
  "/account/auth/login",
  "x-session-token",
  "recording_data",
  "seek_table",
  "SubmitRecording",
  "LoadRecordingFromJson",
  "ReplayRecordRpc",
};

static void scanRegion(const u8 *p, size_t n, u64 base) {
  // 1) 已知包装函数序言：B8 38 00 00 00 E8 <rel32> 48 2B E0 45 85 C0
  u32 hits = 0;
  for (size_t i = 0; i + 16 < n; i++) {
    if (p[i] == 0xB8 && p[i+1] == 0x38 && p[i+2] == 0 && p[i+3] == 0 &&
        p[i+4] == 0 && p[i+5] == 0xE8 &&
        p[i+10] == 0x48 && p[i+11] == 0x2B && p[i+12] == 0xE0 &&
        p[i+13] == 0x45 && p[i+14] == 0x85 && p[i+15] == 0xC0) {
      u32 rel = *(const u32 *)(p + i + 6);
      u64 target = (u64)(p + i) + 10 + rel;
      if (gPatACount < CN_MAX_HOOKS)
        gPatA[gPatACount++] = (void *)(p + i);
      if (hits < 20)
        scanLog("PAT-A wrapper va=%p rva=0x%llX call=%p rva=0x%llX",
                p + i, (unsigned long long)((u64)(p + i) - base),
                (void *)target, (unsigned long long)(target - base));
      hits++;
    }
  }
  if (hits) scanLog("PAT-A total=%u", hits);

  // 2) 只看 48 2B E0 45 85 C0（sub rsp,rax; test r8d,r8d），输出前面 8 字节
  hits = 0;
  for (size_t i = 8; i + 6 < n; i++) {
    if (p[i] == 0x48 && p[i+1] == 0x2B && p[i+2] == 0xE0 &&
        p[i+3] == 0x45 && p[i+4] == 0x85 && p[i+5] == 0xC0) {
      if (hits < 20) {
        size_t s = i - 8;
        scanLog("PAT-B va=%p rva=0x%llX pre=%02X %02X %02X %02X %02X %02X %02X %02X",
                p + i, (unsigned long long)((u64)(p + i) - base),
                p[s], p[s+1], p[s+2], p[s+3], p[s+4], p[s+5], p[s+6], p[s+7]);
      }
      hits++;
    }
  }
  if (hits) scanLog("PAT-B total=%u", hits);

  // 3) 关键字符串
  for (size_t k = 0; k < sizeof(needles) / sizeof(needles[0]); k++) {
    const char *needle = needles[k];
    size_t m = strlen(needle);
    u32 found = 0;
    for (size_t i = 0; i + m < n; i++) {
      if (memcmp(p + i, needle, m) == 0) {
        if (found < 6) {
          char ctx[80] = {0};
          size_t cn = 64;
          if (i + cn > n) cn = n - i;
          for (size_t j = 0; j < cn; j++) {
            char c = (char)p[i + j];
            ctx[j] = (c >= 0x20 && c < 0x7F) ? c : '.';
          }
          scanLog("STR %s va=%p rva=0x%llX ctx=%s", needle, p + i,
                  (unsigned long long)((u64)(p + i) - base), ctx);
        }
        found++;
        i += m - 1;
      }
    }
    if (found) scanLog("STR %s total=%u", needle, found);
  }
}

void bpCnDiagInit(void) {
  InitializeCriticalSection(&gScanLock);
  gScanLockInit = 1;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\cn_scan.log", gCtx.dataFolder);
  gScan = fopen(path, "w");
  if (!gScan) {
    bpLog("cn diag: cannot open %s", path);
    return;
  }

  scanLog("== sky-blueprint CN diag ==");
  scanLog("game base=%p pid=%lu edition=%d", gCtx.gameBase, gCtx.pid, (int)gCtx.edition);
  scanLog("loader=%s v%u", gCtx.loaderName, gCtx.loaderVersion);

  HMODULE h = GetModuleHandleA(NULL);
  if (!h) {
    scanLog("no game module");
    return;
  }

  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
  if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
    scanLog("DOS header invalid");
    return;
  }
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)h + dos->e_lfanew);
  if (nt->Signature != IMAGE_NT_SIGNATURE) {
    scanLog("NT header invalid");
    return;
  }

  u64 base = (u64)h;
  u32 size = nt->OptionalHeader.SizeOfImage;
  scanLog("image base=%p size=0x%X entryRva=0x%X",
          h, size, nt->OptionalHeader.AddressOfEntryPoint);

  u64 addr = base, end = base + size;
  while (addr < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
      break;

    u64 rstart = (u64)mbi.BaseAddress;
    u64 rend = rstart + mbi.RegionSize;
    if (rstart < base) rstart = base;
    if (rend > end) rend = end;
    if (rend <= rstart)
      break;

    DWORD prot = mbi.Protect;
    int readable = (mbi.State == MEM_COMMIT) &&
      (prot & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ |
               PAGE_EXECUTE_READWRITE));

    if (readable) {
      scanLog("region %p-0x%llX prot=0x%X state=0x%X",
              (void *)rstart, (unsigned long long)(rend - rstart), prot, mbi.State);
      scanRegion((const u8 *)rstart, (size_t)(rend - rstart), base);
    }
    addr = rend;
  }

  // 4) 对 PAT-A 候选挂只计数 hook
  scanLog("== installing %d count-only hooks ==", gPatACount);
  for (int i = 0; i < gPatACount; i++) {
    gTramp[i] = bpManualHookInstallSlot(gPatA[i], detourFor(i));
    scanLog("HOOK%d target=%p tramp=%p %s", i, gPatA[i], gTramp[i],
            gTramp[i] ? "OK" : "FAILED");
  }

  scanLog("== done (hooks active; open a recording / send a request) ==");
  // 保持 gScan 打开，供 hook 运行时写日志
}
