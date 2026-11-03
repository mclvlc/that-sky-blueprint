// ---------------------------------------------------------------------------
// 从 Lua CClosure 反解 lua_CFunction（wrapper），并从 wrapper 里提取它调用的
// C++ 方法地址。目的是把 Lua 可见的游戏 API 映射回 Sky.exe 里的真实函数。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <string.h>
#include "bp.h"

typedef struct { const char *name; u64 addr; } BpClosureEntry;

static BpClosureEntry gEntries[32];
static i32 gEntryCount = 0;

// 判断一个指针是否落在 Sky.exe 的代码段内
static i32 inSkyText(u64 p) {
  HMODULE h = GetModuleHandleA(NULL);
  if (!h) return 0;
  IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)h;
  IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((u8 *)h + dos->e_lfanew);
  IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
  for (i32 i = 0; i < nt->FileHeader.NumberOfSections; i++, sec++) {
    if (!(sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))
      continue;
    u64 start = (u64)h + sec->VirtualAddress;
    u64 end = start + sec->Misc.VirtualSize;
    if (p >= start && p < end)
      return 1;
  }
  return 0;
}

// 从 wrapper 里扫 E8 rel32（call）并返回第一个落在 Sky.exe 代码段外的目标，
// 以及落在代码段内的目标列表
static void scanWrapper(u64 wrapper, const char *name) {
  u8 *p = (u8 *)wrapper;
  bpLog("[%s] wrapper=%p", name, (void *)wrapper);

  i32 calls = 0;
  for (i32 off = 0; off < 96; off++) {
    if (p[off] == 0xE8) {
      i32 rel = *(i32 *)(p + off + 1);
      u64 target = wrapper + off + 5 + rel;
      bpLog("    call@+0x%02X -> %p %s", off, (void *)target,
        inSkyText(target) ? "(game text)" : "");
      calls++;
      if (calls >= 6)
        break;
    }
  }
  if (calls == 0)
    bpLog("    no direct call found in first 96 bytes");

  // 打印前 24 字节便于人工判断
  char hex[128] = {0};
  for (i32 i = 0; i < 24; i++) {
    char tmp[4];
    snprintf(tmp, sizeof(tmp), "%02X ", p[i]);
    strncat(hex, tmp, sizeof(hex) - strlen(hex) - 1);
  }
  bpLog("    bytes: %s", hex);
}

// Lua 侧把 closure 地址写进 closures.txt，格式: name=0xADDR
void bpClosureAnalyze(void) {
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s/closures.txt", gCtx.modFolder);

  FILE *f = fopen(path, "r");
  if (!f) {
    bpLog("no closures.txt");
    return;
  }

  static char line[512];
  gEntryCount = 0;
  while (fgets(line, sizeof(line), f) && gEntryCount < 32) {
    char *eq = strchr(line, '=');
    if (!eq) continue;
    *eq = 0;
    u64 addr = 0;
    sscanf(eq + 1, "0x%llX", &addr);
    if (!addr) continue;
    gEntries[gEntryCount].name = _strdup(line);
    gEntries[gEntryCount].addr = addr;
    gEntryCount++;
  }
  fclose(f);

  bpLog("closure entries: %d", gEntryCount);

  for (i32 i = 0; i < gEntryCount; i++) {
    u64 closure = gEntries[i].addr;
    // 在 closure 结构前 64 字节里找落在 Sky.exe 代码段的指针 = lua_CFunction
    u64 wrapper = 0;
    for (i32 off = 0; off < 64; off += 8) {
      u64 v = *(u64 *)(closure + off);
      if (inSkyText(v)) {
        wrapper = v;
        bpLog("[%s] closure+0x%X = %p (sky text)", gEntries[i].name, off, (void *)v);
        break;
      }
    }
    if (!wrapper) {
      bpLog("[%s] no code pointer found near closure %p", gEntries[i].name, (void *)closure);
      continue;
    }
    scanWrapper(wrapper, gEntries[i].name);
  }
}
