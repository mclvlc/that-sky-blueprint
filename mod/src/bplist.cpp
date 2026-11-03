// ---------------------------------------------------------------------------
// 蓝图列表（游戏内 UI）
//
// 扫描一个目录下的 *.skyblueprint，解析出摘要并显示。
// 支持 v2 envelope（SKYBP2）和旧的 SKYBP1 单 block。
// 不安装任何 hook，纯文件读取，风险与看日志相同。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bp.h"          // 先引入 windows.h，shellapi.h 依赖它

#define CJSON_HIDE_SYMBOLS
#include "../libraries/htmodloader/includes/cJSON/cJSON.h"

#include <shellapi.h>

#define BP_LIST_MAX 256
#define BP_LIST_READ_MAX (16 * 1024 * 1024)

typedef struct {
  char file[260];
  char path[MAX_PATH];
  char stid[80];
  char kind[24];
  char created[40];
  char note[192];
  char timeStr[32];
  long long viewTime;   // 最近查看的 Unix 秒；0=没看过
  char source[512];
  double duration;
  int chunks;
  int notes;
  int anims;
  int placeables;
  int players;
  int format;
  unsigned long long size;
  long long mtime;
  int valid;
  char error[160];
  char plac[4][96];
  char placPos[64];
  char firstPos[64];
  char ppsp[64];
  int placCount;
} BpListItem;

static BpListItem gItems[BP_LIST_MAX];
static int gItemCount = 0;
static char gDir[MAX_PATH] = {0};
static char gDirEdit[MAX_PATH] = {0};
static char gFilter[128] = {0};
static char gStatus[320] = {0};
static int gInited = 0;
static int gSelected = -1;
static int gVisible = 0;

// ---------------------------------------------------------------------------
// 小工具
// ---------------------------------------------------------------------------

static cJSON *jobj(cJSON *o, const char *k) {
  if (!o) return NULL;
  return cJSON_GetObjectItemCaseSensitive(o, k);
}

static double jnum(cJSON *o, const char *k, double def) {
  cJSON *v = jobj(o, k);
  return cJSON_IsNumber(v) ? v->valuedouble : def;
}

static int jint(cJSON *o, const char *k, int def) {
  return (int)jnum(o, k, (double)def);
}

static const char *jstr(cJSON *o, const char *k) {
  cJSON *v = jobj(o, k);
  return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

static int arrLen(cJSON *v) {
  return cJSON_IsArray(v) ? cJSON_GetArraySize(v) : 0;
}

static int dirExists(const char *p) {
  DWORD a = GetFileAttributesA(p);
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

static void trimEol(char *s) {
  size_t n = strlen(s);
  while (n && (s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == ' '))
    s[--n] = 0;
}

static int containsCI(const char *hay, const char *needle) {
  if (!needle[0]) return 1;
  size_t nl = strlen(needle);
  for (const char *p = hay; *p; p++) {
    size_t i = 0;
    while (i < nl && p[i] &&
           (p[i] == needle[i] ||
            (p[i] >= 'A' && p[i] <= 'Z' && p[i] + 32 == needle[i]) ||
            (needle[i] >= 'A' && needle[i] <= 'Z' && needle[i] + 32 == p[i])))
      i++;
    if (i == nl) return 1;
  }
  return 0;
}

static char *readWholeFile(const char *path, size_t *outLen) {
  FILE *f = fopen(path, "rb");
  if (!f) return NULL;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
  long n = ftell(f);
  if (n <= 0 || n > BP_LIST_READ_MAX) { fclose(f); return NULL; }
  rewind(f);
  char *buf = (char *)malloc((size_t)n + 1);
  if (!buf) { fclose(f); return NULL; }
  size_t got = fread(buf, 1, (size_t)n, f);
  fclose(f);
  buf[got] = 0;
  if (outLen) *outLen = got;
  return buf;
}

// ---------------------------------------------------------------------------
// JSON 解析
// ---------------------------------------------------------------------------

// 与 tool/skybp.py 的 content_player() 保持一致：按事件数量打分取最高
static cJSON *contentPlayer(cJSON *block) {
  cJSON *players = jobj(block, "plyr");
  if (!cJSON_IsArray(players)) return NULL;

  cJSON *best = NULL;
  int bestScore = -1;
  cJSON *p = NULL;
  cJSON_ArrayForEach(p, players) {
    if (!cJSON_IsObject(p)) continue;
    int score = 3 * arrLen(jobj(p, "nts"))
              + 2 * arrLen(jobj(p, "anms"))
              +     arrLen(jobj(p, "plac"));
    if (score > bestScore) {
      bestScore = score;
      best = p;
    }
  }
  return best;
}

static int countNotes(cJSON *player) {
  cJSON *nts = jobj(player, "nts");
  int n = 0;
  cJSON *e = NULL;
  cJSON_ArrayForEach(e, nts) {
    cJSON *t = cJSON_GetArrayItem(e, 0);
    if (cJSON_IsNumber(t) && (int)t->valuedouble == 3)
      n++;
  }
  return n;
}

static void collectPlaceables(BpListItem *it, cJSON *player) {
  cJSON *plac = jobj(player, "plac");
  cJSON *e = NULL;
  it->placCount = 0;
  it->placPos[0] = 0;
  cJSON_ArrayForEach(e, plac) {
    if (it->placCount >= 4) break;
    cJSON *name = cJSON_GetArrayItem(e, 2);
    if (cJSON_IsString(name) && name->valuestring) {
      if (!it->placPos[0]) {
        cJSON *pos = cJSON_GetArrayItem(e, 3);
        if (cJSON_IsString(pos) && pos->valuestring)
          snprintf(it->placPos, sizeof(it->placPos), "%s", pos->valuestring);
      }
      snprintf(it->plac[it->placCount], sizeof(it->plac[0]), "%s", name->valuestring);
      it->placCount++;
    }
  }
}

static void formatFileTime(long long t, char *out, size_t n) {
  FILETIME ft;
  ft.dwLowDateTime = (DWORD)(t & 0xFFFFFFFFULL);
  ft.dwHighDateTime = (DWORD)((t >> 32) & 0xFFFFFFFFULL);
  FILETIME lt;
  if (!FileTimeToLocalFileTime(&ft, &lt)) lt = ft;
  SYSTEMTIME st;
  if (!FileTimeToSystemTime(&lt, &st)) {
    snprintf(out, n, "-");
    return;
  }
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d",
           st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
}

static void formatUnixTime(long long t, char *out, size_t n) {
  time_t tt = (time_t)t;
  struct tm *lt = localtime(&tt);
  if (!lt) {
    snprintf(out, n, "-");
    return;
  }
  snprintf(out, n, "%04d-%02d-%02d %02d:%02d",
           lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday,
           lt->tm_hour, lt->tm_min);
}

// 取 URL 的文件名（去掉 .ndjson / query），用于匹配 recording id
static void urlBase(const char *url, char *out, size_t n) {
  out[0] = 0;
  if (!url || !url[0]) return;
  const char *slash = strrchr(url, '/');
  const char *b = slash ? slash + 1 : url;
  const char *dot = strchr(b, '.');
  const char *q = strchr(b, '?');
  const char *end = b + strlen(b);
  if (dot && dot < end) end = dot;
  if (q && q < end) end = q;
  size_t len = (size_t)(end - b);
  if (len >= n) len = n - 1;
  memcpy(out, b, len);
  out[len] = 0;
}

static void parseBlueprint(BpListItem *it) {
  formatFileTime(it->mtime, it->timeStr, sizeof(it->timeStr));
  size_t len = 0;
  char *data = readWholeFile(it->path, &len);
  if (!data) {
    snprintf(it->error, sizeof(it->error), "Cannot read file");
    return;
  }

  it->format = 0;
  char *json = data;
  if (len > 7 && memcmp(data, "SKYBP2\n", 7) == 0) {
    it->format = 2;
    json = data + 7;
  } else if (len > 7 && memcmp(data, "SKYBP1\n", 7) == 0) {
    it->format = 1;
    json = data + 7;
  } else {
    it->format = 0;   // 裸 block
  }

  cJSON *root = cJSON_Parse(json);
  if (!root || !cJSON_IsObject(root)) {
    snprintf(it->error, sizeof(it->error), "JSON parse failed");
    if (root) cJSON_Delete(root);
    free(data);
    return;
  }

  cJSON *block = (it->format == 2) ? jobj(root, "block") : root;
  if (!cJSON_IsObject(block)) {
    snprintf(it->error, sizeof(it->error), "missing block");
    cJSON_Delete(root);
    free(data);
    return;
  }

  cJSON *meta = jobj(root, "meta");
  cJSON *summary = jobj(root, "summary");
  cJSON *ghost = contentPlayer(block);

  snprintf(it->stid, sizeof(it->stid), "%s",
           it->format == 2 ? jstr(meta, "stid") : jstr(block, "stid"));
  if (!it->stid[0])
    snprintf(it->stid, sizeof(it->stid), "%s", jstr(block, "stid"));

  it->duration = it->format == 2 ? jnum(meta, "duration", 0) : jnum(block, "chet", 0);
  it->chunks = it->format == 2 ? jint(meta, "chunks", 1) : 1;

  if (it->format == 2 && cJSON_IsObject(summary)) {
    it->notes = jint(summary, "notes", 0);
    it->anims = jint(summary, "anims", 0);
    it->placeables = jint(summary, "placeables", 0);
    it->players = jint(summary, "players", 0);
    snprintf(it->kind, sizeof(it->kind), "%s", jstr(root, "kind"));
    snprintf(it->created, sizeof(it->created), "%s", jstr(root, "created"));
    snprintf(it->source, sizeof(it->source), "%s", jstr(root, "source"));
  } else {
    // v1 / 裸 block：自己统计
    it->notes = countNotes(ghost);
    it->anims = arrLen(jobj(ghost, "anms"));
    it->players = arrLen(jobj(block, "plyr"));
    int placTotal = 0;
    cJSON *p = NULL;
    cJSON_ArrayForEach(p, jobj(block, "plyr"))
      placTotal += arrLen(jobj(p, "plac"));
    it->placeables = placTotal;
    if (it->notes || it->anims)
      snprintf(it->kind, sizeof(it->kind), "recording");
    else if (it->placeables)
      snprintf(it->kind, sizeof(it->kind), "shared_space");
    else
      snprintf(it->kind, sizeof(it->kind), "unknown");
  }
  if (!it->kind[0])
    snprintf(it->kind, sizeof(it->kind), "unknown");

  collectPlaceables(it, ghost);

  // 没有物件时，用角色第一帧的位置作为部署锚点
  snprintf(it->ppsp, sizeof(it->ppsp), "%s", jstr(block, "ppsp"));
  {
    cJSON *sts = jobj(ghost, "sts");
    cJSON *s0 = cJSON_GetArrayItem(sts, 0);
    cJSON *p1 = cJSON_GetArrayItem(s0, 1);
    if (cJSON_IsString(p1) && p1->valuestring)
      snprintf(it->firstPos, sizeof(it->firstPos), "%s", p1->valuestring);
  }

  it->valid = 1;

  cJSON_Delete(root);
  free(data);
}

// ---------------------------------------------------------------------------
// 目录扫描
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// 备注：<blueprint dir>/notes.json，按 stid（缺失时用文件名）持久化
// ---------------------------------------------------------------------------
static const char *noteKey(const BpListItem *it) {
  return it->stid[0] ? it->stid : it->file;
}

static void loadNotes(void) {
  for (int i = 0; i < gItemCount; i++)
    gItems[i].note[0] = 0;
  if (!gDir[0]) return;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\notes.json", gDir);
  size_t len = 0;
  char *data = readWholeFile(path, &len);
  if (!data) return;

  cJSON *root = cJSON_Parse(data);
  free(data);
  if (!cJSON_IsObject(root)) {
    if (root) cJSON_Delete(root);
    return;
  }

  for (int i = 0; i < gItemCount; i++) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, noteKey(&gItems[i]));
    if (cJSON_IsString(v) && v->valuestring)
      snprintf(gItems[i].note, sizeof(gItems[i].note), "%s", v->valuestring);
  }
  cJSON_Delete(root);
}

static void saveNotes(void) {
  if (!gDir[0]) return;

  cJSON *root = cJSON_CreateObject();
  for (int i = 0; i < gItemCount; i++) {
    if (gItems[i].note[0])
      cJSON_AddStringToObject(root, noteKey(&gItems[i]), gItems[i].note);
  }

  char *s = cJSON_PrintUnformatted(root);
  char path[MAX_PATH], tmp[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\notes.json", gDir);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    if (s) fputs(s, f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
  }
  if (s) cJSON_free(s);
  cJSON_Delete(root);
}

static int cmpItems(const void *a, const void *b) {
  const BpListItem *x = (const BpListItem *)a;
  const BpListItem *y = (const BpListItem *)b;
  // 最近查看过的排最前；没看过的按保存时间倒序
  if (x->viewTime != y->viewTime)
    return x->viewTime < y->viewTime ? 1 : -1;
  if (x->mtime < y->mtime) return 1;
  if (x->mtime > y->mtime) return -1;
  return 0;
}

// ---------------------------------------------------------------------------
// 最近查看：<blueprint dir>/viewed.json，按 recording id 匹配
// ---------------------------------------------------------------------------
static void viewedPath(char *out, size_t n) {
  snprintf(out, n, "%s\\viewed.json", gDir);
}

static void loadViewed(void) {
  for (int i = 0; i < gItemCount; i++)
    gItems[i].viewTime = 0;
  if (!gDir[0]) return;

  char path[MAX_PATH];
  viewedPath(path, sizeof(path));
  size_t len = 0;
  char *data = readWholeFile(path, &len);
  if (!data) return;

  cJSON *root = cJSON_Parse(data);
  free(data);
  if (!cJSON_IsObject(root)) {
    if (root) cJSON_Delete(root);
    return;
  }

  for (int i = 0; i < gItemCount; i++) {
    cJSON *v = cJSON_GetObjectItemCaseSensitive(root, noteKey(&gItems[i]));
    if (cJSON_IsNumber(v) && v->valuedouble > 0) {
      gItems[i].viewTime = (long long)v->valuedouble;
      formatUnixTime(gItems[i].viewTime, gItems[i].timeStr,
                     sizeof(gItems[i].timeStr));
    }
  }
  cJSON_Delete(root);
}

static void saveViewed(void) {
  if (!gDir[0]) return;

  cJSON *root = cJSON_CreateObject();
  for (int i = 0; i < gItemCount; i++) {
    if (gItems[i].viewTime > 0)
      cJSON_AddNumberToObject(root, noteKey(&gItems[i]),
                              (double)gItems[i].viewTime);
  }

  char *s = cJSON_PrintUnformatted(root);
  char path[MAX_PATH], tmp[MAX_PATH];
  viewedPath(path, sizeof(path));
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    if (s) fputs(s, f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
  }
  if (s) cJSON_free(s);
  cJSON_Delete(root);
}

static void markViewed(const char *url) {
  char base[128];
  urlBase(url, base, sizeof(base));
  if (!base[0]) return;

  long long now = (long long)time(NULL);
  int marked = 0;

  char selPath[MAX_PATH] = {0};
  if (gSelected >= 0 && gSelected < gItemCount)
    snprintf(selPath, sizeof(selPath), "%s", gItems[gSelected].path);

  for (int i = 0; i < gItemCount; i++) {
    char sb[128];
    urlBase(gItems[i].source, sb, sizeof(sb));
    if (strcmp(sb, base) == 0 || strcmp(gItems[i].stid, base) == 0) {
      gItems[i].viewTime = now;
      formatUnixTime(now, gItems[i].timeStr, sizeof(gItems[i].timeStr));
      marked++;
    }
  }
  if (!marked) return;

  saveViewed();

  if (gItemCount > 1)
    qsort(gItems, (size_t)gItemCount, sizeof(BpListItem), cmpItems);

  if (selPath[0]) {
    for (int i = 0; i < gItemCount; i++) {
      if (strcmp(gItems[i].path, selPath) == 0) {
        gSelected = i;
        break;
      }
    }
  }
  bpLog("viewed: %s", base);
}

// 网络线程 -> UI 线程的待处理查看事件
static CRITICAL_SECTION gViewLock;
static int gViewLockInit = 0;
static char gPendingView[512];
static volatile LONG gViewSeq = 0;

void bpListNotifyViewed(const char *url) {
  if (!gViewLockInit || !url || !url[0]) return;
  EnterCriticalSection(&gViewLock);
  snprintf(gPendingView, sizeof(gPendingView), "%s", url);
  InterlockedIncrement(&gViewSeq);
  LeaveCriticalSection(&gViewLock);
}

static void bpListPollViewed(void) {
  if (!gViewLockInit) return;
  static LONG seen = 0;
  LONG seq = InterlockedCompareExchange(&gViewSeq, 0, 0);
  if (seq == seen) return;

  char url[512] = {0};
  EnterCriticalSection(&gViewLock);
  snprintf(url, sizeof(url), "%s", gPendingView);
  seen = seq;
  LeaveCriticalSection(&gViewLock);

  markViewed(url);
}

void bpListRefresh(void) {
  gItemCount = 0;
  gSelected = -1;

  if (!gDir[0] || !dirExists(gDir)) {
    snprintf(gStatus, sizeof(gStatus), "Folder not found: %s", gDir[0] ? gDir : "(not set)");
    return;
  }

  char pattern[MAX_PATH];
  snprintf(pattern, sizeof(pattern), "%s\\*.skyblueprint", gDir);

  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(pattern, &fd);
  if (h == INVALID_HANDLE_VALUE) {
    snprintf(gStatus, sizeof(gStatus), "Scan failed: %s", pattern);
    return;
  }

  do {
    if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    if (gItemCount >= BP_LIST_MAX) break;

    BpListItem *it = &gItems[gItemCount];
    memset(it, 0, sizeof(*it));
    snprintf(it->file, sizeof(it->file), "%s", fd.cFileName);
    snprintf(it->path, sizeof(it->path), "%s\\%s", gDir, fd.cFileName);
    it->size = ((unsigned long long)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    it->mtime = ((long long)fd.ftLastWriteTime.dwHighDateTime << 32)
              | fd.ftLastWriteTime.dwLowDateTime;
    parseBlueprint(it);
    gItemCount++;
  } while (FindNextFileA(h, &fd));

  FindClose(h);

  loadNotes();
  loadViewed();

  if (gItemCount > 1)
    qsort(gItems, (size_t)gItemCount, sizeof(BpListItem), cmpItems);

  snprintf(gStatus, sizeof(gStatus), "%d blueprints  |  %s", gItemCount, gDir);
  bpLog("blueprint list: %d files in %s", gItemCount, gDir);
}

// ---------------------------------------------------------------------------
// 配置
// ---------------------------------------------------------------------------

static void configPath(char *out, size_t n) {
  snprintf(out, n, "%s\\blueprints.cfg", gCtx.modFolder);
}

static void loadConfig(void) {
  gDir[0] = 0;
  char cfg[MAX_PATH];
  configPath(cfg, sizeof(cfg));
  FILE *f = fopen(cfg, "rb");
  if (f) {
    if (fgets(gDir, sizeof(gDir), f)) trimEol(gDir);
    fclose(f);
  }
  if (gDir[0] && dirExists(gDir))
    goto done;

  // 1) 数据目录下的 blueprints/
  {
    char cand[MAX_PATH];
    snprintf(cand, sizeof(cand), "%s\\blueprints", gCtx.dataFolder);
    if (!dirExists(cand))
      CreateDirectoryA(cand, NULL);
    snprintf(gDir, sizeof(gDir), "%s", cand);
  }

done:
  snprintf(gDirEdit, sizeof(gDirEdit), "%s", gDir);
}

static void saveConfig(void) {
  char cfg[MAX_PATH];
  configPath(cfg, sizeof(cfg));
  FILE *f = fopen(cfg, "wb");
  if (f) {
    fprintf(f, "%s\n", gDir);
    fclose(f);
  }
}

void bpListInit(void) {
  if (gInited) return;
  gInited = 1;
  InitializeCriticalSection(&gViewLock);
  gViewLockInit = 1;
  loadConfig();
  bpListRefresh();
}

// ---------------------------------------------------------------------------
// UI
// ---------------------------------------------------------------------------

static void openFolder(const char *dir) {
  if (dir && dir[0])
    ShellExecuteA(NULL, "open", dir, NULL, NULL, SW_SHOWNORMAL);
}

// ---------------------------------------------------------------------------
// 一键部署：写队列文件，由 tool/deployd.py 处理
// ---------------------------------------------------------------------------

static char gNoteEdit[192] = {0};
static char gDeployMsg[128] = "Blueprint";
static char gDeployPos[64] = "";
static char gDeployOri[64] = "(0,0.687,0,0.726)";
static char gDeployLevel[128] = "Prairie_ButterflyFields";
static char gDeployPool[64] = "RecordingCandle";
static int gDeployEdited = 0;
static int gDeploySel = -1;
static char gDeploySelPath[MAX_PATH] = {0};
static char gDeployStatus[512] = {0};
static char gSessInfo[256] = {0};
static u32 gLastStatusCheck = 0;

static void setDeployPos(const char *raw) {
  if (!raw || !raw[0]) return;
  if (raw[0] == '(')
    snprintf(gDeployPos, sizeof(gDeployPos), "%s", raw);
  else
    snprintf(gDeployPos, sizeof(gDeployPos), "(%s)", raw);
}

static void jsonEscape(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in[i] && j + 2 < n; i++) {
    char c = in[i];
    if (c == '\\' || c == '"') { out[j++] = '\\'; out[j++] = c; }
    else if (c == '\n') { out[j++] = '\\'; out[j++] = 'n'; }
    else if (c != '\r') out[j++] = c;
  }
  out[j] = 0;
}

static void queueDeploy(const char *blueprint) {
  if (!gCtx.dataFolder[0]) return;
  char p[896], m[256], pos[128], ori[128], lvl[192], pool[96];
  jsonEscape(blueprint, p, sizeof(p));
  jsonEscape(gDeployMsg, m, sizeof(m));
  jsonEscape(gDeployPos, pos, sizeof(pos));
  jsonEscape(gDeployOri, ori, sizeof(ori));
  jsonEscape(gDeployLevel, lvl, sizeof(lvl));
  jsonEscape(gDeployPool, pool, sizeof(pool));

  char tmp[MAX_PATH], dst[MAX_PATH];
  snprintf(tmp, sizeof(tmp), "%s\\deploy_queue.json.tmp", gCtx.dataFolder);
  snprintf(dst, sizeof(dst), "%s\\deploy_queue.json", gCtx.dataFolder);
  FILE *f = fopen(tmp, "wb");
  if (!f) {
    snprintf(gDeployStatus, sizeof(gDeployStatus), "Cannot write deploy queue");
    return;
  }
  fprintf(f,
    "{\"blueprint\":\"%s\",\"message\":\"%s\",\"pos\":\"%s\",\"ori\":\"%s\","
    "\"level\":\"%s\",\"pool_type\":\"%s\"}\n",
    p, m, pos, ori, lvl, pool);
  fclose(f);
  MoveFileExA(tmp, dst, MOVEFILE_REPLACE_EXISTING);
  snprintf(gDeployStatus, sizeof(gDeployStatus),
           "Queued. Waiting for deployd.py...");
  bpLog("deploy queued: %s", blueprint);
}

static void loadDeployStatus(void) {
  if (!gCtx.dataFolder[0]) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\deploy_result.txt", gCtx.dataFolder);
  FILE *f = fopen(path, "rb");
  if (!f) return;
  char line[512] = {0};
  fgets(line, sizeof(line), f);
  fclose(f);
  trimEol(line);
  if (line[0]) snprintf(gDeployStatus, sizeof(gDeployStatus), "%s", line);
}

static void sessionInfo(char *out, size_t n) {
  out[0] = 0;
  if (!gCtx.dataFolder[0]) return;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\session.json", gCtx.dataFolder);
  FILE *f = fopen(path, "rb");
  if (!f) {
    snprintf(out, n, "session: not captured yet (open a recording once)");
    return;
  }
  char buf[512] = {0};
  size_t got = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[got] = 0;

  char user[80] = "-", level[128] = "-", pool[64] = "-";
  const char *p = strstr(buf, "\"user\":\"");
  if (p) {
    p += 8;
    const char *e = strchr(p, '"');
    if (e && e - p < 79) { memcpy(user, p, (size_t)(e - p)); user[e - p] = 0; }
  }
  p = strstr(buf, "\"level\":\"");
  if (p) {
    p += 9;
    const char *e = strchr(p, '"');
    if (e && e - p < 127) { memcpy(level, p, (size_t)(e - p)); level[e - p] = 0; }
  }
  p = strstr(buf, "\"pool_type\":\"");
  if (p) {
    p += 13;
    const char *e = strchr(p, '"');
    if (e && e - p < 63) { memcpy(pool, p, (size_t)(e - p)); pool[e - p] = 0; }
  }
  // 旧 session.json 可能残留 MessageBoat 等非 recording pool
  if (strncmp(pool, "Recording", 9) != 0)
    snprintf(pool, sizeof(pool), "RecordingCandle");

  // 用户没手动改过时，用抓到的当前场景预填
  if (!gDeployEdited) {
    if (level[0] && strcmp(level, "-") != 0)
      snprintf(gDeployLevel, sizeof(gDeployLevel), "%s", level);
    if (pool[0] && strcmp(pool, "-") != 0)
      snprintf(gDeployPool, sizeof(gDeployPool), "%s", pool);
  }
  snprintf(out, n, "session: user=%s  level=%s  pool=%s", user, level, pool);
}

void bpListUI(void) {
  bpListInit();
  bpListPollViewed();

  // 每 2 秒刷新一次部署结果 / session 状态
  {
    u32 now = GetTickCount();
    if (now - gLastStatusCheck > 2000) {
      gLastStatusCheck = now;
      loadDeployStatus();
      sessionInfo(gSessInfo, sizeof(gSessInfo));
    }
  }

  // 目录栏
  ImGui::SetNextItemWidth(-330);
  ImGui::InputText("##dir", gDirEdit, sizeof(gDirEdit));
  ImGui::SameLine();
  if (ImGui::Button("Apply")) {
    snprintf(gDir, sizeof(gDir), "%s", gDirEdit);
    saveConfig();
    bpListRefresh();
  }
  ImGui::SameLine();
  if (ImGui::Button("Refresh"))
    bpListRefresh();
  ImGui::SameLine();
  if (ImGui::Button("Open folder"))
    openFolder(gDir);

  ImGui::SetNextItemWidth(260);
  ImGui::InputTextWithHint("##filter", "filter: stid / kind / file", gFilter, sizeof(gFilter));
  ImGui::SameLine();
  ImGui::TextDisabled("%s", gStatus);

  ImGui::Separator();

  const ImGuiTableFlags flags =
    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg |
    ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable |
    ImGuiTableFlags_SizingFixedFit;

  gVisible = 0;
  if (ImGui::BeginTable("##bplist", 9, flags, ImVec2(0, 250))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("NOTE", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn("TIME", ImGuiTableColumnFlags_WidthFixed, 100);
    ImGui::TableSetupColumn("SEC", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("ID", ImGuiTableColumnFlags_WidthFixed, 130);
    ImGui::TableSetupColumn("KIND", ImGuiTableColumnFlags_WidthFixed, 68);
    ImGui::TableSetupColumn("CHUNK", ImGuiTableColumnFlags_WidthFixed, 44);
    ImGui::TableSetupColumn("NT", ImGuiTableColumnFlags_WidthFixed, 40);
    ImGui::TableSetupColumn("SIZE", ImGuiTableColumnFlags_WidthFixed, 50);
    ImGui::TableSetupColumn("FILE", ImGuiTableColumnFlags_WidthStretch, 1);
    ImGui::TableHeadersRow();

    for (int i = 0; i < gItemCount; i++) {
      BpListItem *it = &gItems[i];
      if (gFilter[0] &&
          !containsCI(it->stid, gFilter) &&
          !containsCI(it->kind, gFilter) &&
          !containsCI(it->file, gFilter) &&
          !containsCI(it->note, gFilter))
        continue;
      gVisible++;

      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      char label[160];
      const char *base = it->note[0] ? it->note : "(no note)";
      size_t j = 0;
      for (size_t k = 0; base[k] && j < 36; k++) {
        char c = base[k];
        label[j++] = (c == '#' || c == '\n' || c == '\r') ? ' ' : c;
      }
      label[j] = 0;
      snprintf(label + j, sizeof(label) - j, "##%d", i);
      if (ImGui::Selectable(label, gSelected == i,
                            ImGuiSelectableFlags_SpanAllColumns))
        gSelected = i;
      if (ImGui::IsItemHovered() && it->note[0])
        ImGui::SetTooltip("%s", it->note);

      ImGui::TableSetColumnIndex(1);
      ImGui::TextUnformatted(it->timeStr);
      ImGui::TableSetColumnIndex(2);
      ImGui::Text("%.1f", it->duration);
      ImGui::TableSetColumnIndex(3);
      ImGui::TextUnformatted(it->stid[0] ? it->stid : "-");
      if (ImGui::IsItemHovered() && it->stid[0])
        ImGui::SetTooltip("%s", it->stid);
      ImGui::TableSetColumnIndex(4);
      ImGui::TextUnformatted(it->valid ? it->kind : "?");
      ImGui::TableSetColumnIndex(5);
      ImGui::Text("%d", it->chunks);
      ImGui::TableSetColumnIndex(6);
      if (it->valid) ImGui::Text("%d", it->notes); else ImGui::TextDisabled("-");
      ImGui::TableSetColumnIndex(7);
      ImGui::Text("%.1fK", (double)it->size / 1024.0);
      ImGui::TableSetColumnIndex(8);
      ImGui::TextUnformatted(it->file);
    }
    ImGui::EndTable();
  }

  if (gVisible != gItemCount)
    ImGui::TextDisabled("Showing %d / %d", gVisible, gItemCount);

  // 详情
  ImGui::Separator();
  if (gSelected >= 0 && gSelected < gItemCount) {
    BpListItem *it = &gItems[gSelected];
    ImGui::TextUnformatted(it->file);
    if (it->valid) {
      ImGui::Text("kind=%s  %.1fs  chunks=%d  notes=%d  anims=%d  placeables=%d  players=%d",
        it->kind, it->duration, it->chunks, it->notes, it->anims,
        it->placeables, it->players);
      ImGui::Text("stid: %s  (format %d, %.1f KB)",
        it->stid, it->format, (double)it->size / 1024.0);
      if (it->created[0])
        ImGui::Text("created: %s", it->created);
      if (it->viewTime > 0)
        ImGui::Text("last viewed: %s", it->timeStr);
      if (it->source[0])
        ImGui::TextWrapped("source: %s", it->source);

      ImGui::Separator();
      ImGui::TextDisabled("Note (saved to notes.json)");
      ImGui::SetNextItemWidth(360);
      ImGui::InputTextWithHint("##note", "add a note...", gNoteEdit, sizeof(gNoteEdit));
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        snprintf(it->note, sizeof(it->note), "%s", gNoteEdit);
        saveNotes();
      }
      ImGui::SameLine();
      if (ImGui::Button("Save note")) {
        snprintf(it->note, sizeof(it->note), "%s", gNoteEdit);
        saveNotes();
      }

      if (it->placCount) {
        ImGui::Text("placeables:");
        for (int i = 0; i < it->placCount; i++)
          ImGui::BulletText("%s", it->plac[i]);
      }

      // 选中项变化时预填部署位置：物件坐标 > 角色首帧 > ppsp
      // 用路径判断而不是索引——列表按时间排序，刷新后同一索引可能指向别的文件
      if (strcmp(gDeploySelPath, it->path) != 0) {
        snprintf(gDeploySelPath, sizeof(gDeploySelPath), "%s", it->path);
        gDeploySel = gSelected;
        snprintf(gNoteEdit, sizeof(gNoteEdit), "%s", it->note);
        const char *raw = NULL;
        if (it->placPos[0])
          raw = it->placPos;
        else if (it->firstPos[0])
          raw = it->firstPos;
        else if (it->ppsp[0] && strncmp(it->ppsp, "(0,", 3) != 0)
          raw = it->ppsp;
        if (raw)
          setDeployPos(raw);
      }

      ImGui::Separator();
      ImGui::TextDisabled("Deploy (deployd.py uploads to private server, creates a new recording)");
      ImGui::SetNextItemWidth(240);
      ImGui::InputText("Message", gDeployMsg, sizeof(gDeployMsg));
      ImGui::SetNextItemWidth(240);
      ImGui::InputText("Pos", gDeployPos, sizeof(gDeployPos));
      ImGui::SetNextItemWidth(240);
      ImGui::InputText("Ori", gDeployOri, sizeof(gDeployOri));
      ImGui::SetNextItemWidth(240);
      if (ImGui::InputText("Level", gDeployLevel, sizeof(gDeployLevel)))
        gDeployEdited = 1;
      ImGui::SetNextItemWidth(240);
      if (ImGui::InputText("Pool", gDeployPool, sizeof(gDeployPool)))
        gDeployEdited = 1;
      if (ImGui::Button("Deploy"))
        queueDeploy(it->path);
      ImGui::SameLine();
      if (ImGui::Button("Refresh status")) {
        loadDeployStatus();
        sessionInfo(gSessInfo, sizeof(gSessInfo));
      }
      if (gSessInfo[0])
        ImGui::TextDisabled("%s", gSessInfo);
      if (gDeployStatus[0])
        ImGui::TextWrapped("%s", gDeployStatus);

      ImGui::Separator();
      if (ImGui::Button("Copy path"))
        ImGui::SetClipboardText(it->path);
      ImGui::SameLine();
      if (ImGui::Button("Reload"))
        parseBlueprint(it);
    } else {
      ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.45f, 1.0f),
                         "Error: %s", it->error[0] ? it->error : "unknown");
      ImGui::TextWrapped("%s", it->path);
    }
  } else {
    ImGui::TextDisabled("Select a row for details");
  }
}
