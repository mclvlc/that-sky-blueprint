#include "bp.h"

HMODULE gModuleDll = NULL;
BpContext gCtx = {0};

BOOL APIENTRY DllMain(
  HMODULE hModule,
  DWORD dwReason,
  LPVOID lpReserved
) {
  if (dwReason == DLL_PROCESS_ATTACH) {
    gModuleDll = hModule;
  }

  return TRUE;
}
