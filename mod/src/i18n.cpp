#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp.h"
#include "i18n.h"

int gUiLang = 0;

static void cfgPath(char *out, size_t n) {
  if (gCtx.dataFolder[0])
    snprintf(out, n, "%s\\ui.cfg", gCtx.dataFolder);
  else
    out[0] = 0;
}

void bpI18nInit(void) {
  char path[MAX_PATH];
  cfgPath(path, sizeof(path));
  if (!path[0]) return;
  FILE *f = fopen(path, "rb");
  if (!f) return;
  char buf[32] = {0};
  if (fgets(buf, sizeof(buf), f))
    gUiLang = atoi(buf) ? 1 : 0;
  fclose(f);
}

void bpI18nSetLang(int lang) {
  gUiLang = lang ? 1 : 0;
  char path[MAX_PATH];
  cfgPath(path, sizeof(path));
  if (!path[0]) return;
  FILE *f = fopen(path, "w");
  if (f) {
    fprintf(f, "%d\n", gUiLang);
    fclose(f);
  }
}

const char *tr(const char *en, const char *zh) {
  return gUiLang ? zh : en;
}
