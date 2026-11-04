// ---------------------------------------------------------------------------
// TLS 明文截获（双向）
//
//   SSL_write_ex  0x1D45CB0  (ssl, buf, num, written)  出站请求
//   SSL_read_ex   0x1D45D10  (ssl, buf, num, readbytes) 入站响应（先调用后填 buf）
//
// 只保存看起来像 HTTP 明文的数据，落盘为 tls_out_XX.bin / tls_in_XX.bin。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bp.h"

#define CJSON_HIDE_SYMBOLS
#include "../libraries/htmodloader/includes/cJSON/cJSON.h"

#define BP_SSL_WRITE_EX_RVA 0x1D45F20   // 0.35.2
#define BP_SSL_READ_EX_RVA  0x1D45F80   // 0.35.2
#define BP_TLS_MAX_SAMPLES  300
#define BP_TLS_CAPTURE_BYTES 16384

#ifndef BP_ENABLE_SSL_READ
// SSL_read_ex 的 detour 曾导致崩溃，先单独验证 write 侧
#define BP_ENABLE_SSL_READ 0
#endif

typedef int (*PFN_SslWriteEx)(u64, const void *, int, u64 *);
static PFN_SslWriteEx g_origSslWriteEx = NULL;
typedef int (*PFN_SslReadEx)(u64, void *, int, u64 *);
static PFN_SslReadEx g_origSslReadEx = NULL;

static volatile u32 gWriteCalls = 0;
static volatile u32 gReadCalls = 0;
static volatile u32 gOutSaved = 0;
static volatile u32 gInSaved = 0;

typedef struct {
  u32 len;
  u32 captured;
  char data[BP_TLS_CAPTURE_BYTES];
} TlsSample;

static TlsSample *gOutSamples = NULL;
static TlsSample *gInSamples = NULL;

// 返回从 p 开始可以安全读取的字节数（最多 want），跨页时按页截断
static u32 safeLen(const void *p, u32 want) {
  if (!p || want == 0)
    return 0;
  u64 addr = (u64)p;
  u64 end = addr + want;
  while (addr < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
      return (u32)(addr - (u64)p);
    if (mbi.State != MEM_COMMIT)
      return (u32)(addr - (u64)p);
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
      return (u32)(addr - (u64)p);
    u64 regionEnd = (u64)mbi.BaseAddress + mbi.RegionSize;
    if (regionEnd <= addr)
      return (u32)(addr - (u64)p);
    addr = regionEnd;
  }
  return want;
}

static i32 readable(const void *p, u64 len) {
  if (!p || len == 0)
    return 0;
  u64 addr = (u64)p;
  u64 end = addr + len;
  if (end < addr)              // 溢出
    return 0;

  while (addr < end) {
    MEMORY_BASIC_INFORMATION mbi;
    if (!VirtualQuery((LPCVOID)addr, &mbi, sizeof(mbi)))
      return 0;
    if (mbi.State != MEM_COMMIT)
      return 0;
    if (!(mbi.Protect & (PAGE_READONLY | PAGE_READWRITE |
                         PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE)))
      return 0;
    u64 regionEnd = (u64)mbi.BaseAddress + mbi.RegionSize;
    if (regionEnd <= addr)     // 防止死循环
      return 0;
    addr = regionEnd;
  }
  return 1;
}

// ---------------------------------------------------------------------------
// 出站凭证 / 当前场景捕获（供游戏内一键部署使用）
//
// 从出站 HTTP 请求头/体里提取 user、session、当前 pool_name/pool_type，
// 变化时写到 <mod>/session.json。deployd.py 直接读它，用户不需要手动复制 token。
// ---------------------------------------------------------------------------
static char gSessUser[80] = {0};
static char gSessToken[96] = {0};
static char gSessLevel[128] = {0};
static char gSessPool[64] = {0};
static char gSessHost[128] = {0};       // 当前连接的服务器（官服/私服会自动切）
static char gSessSkyLevel[32] = {0};    // x-sky-level-id
static char gSessSessionId[80] = {0};   // X-Session-ID

static const char *memFind(const char *hay, u32 n, const char *needle) {
  u32 m = (u32)strlen(needle);
  if (!m || n < m) return NULL;
  for (u32 i = 0; i + m <= n; i++)
    if (memcmp(hay + i, needle, m) == 0)
      return hay + i;
  return NULL;
}

static i32 copyValue(const char *buf, u32 len, const char *prefix,
                     char *out, size_t outsz) {
  const char *p = memFind(buf, len, prefix);
  if (!p) return 0;
  p += strlen(prefix);
  const char *end = buf + len;
  size_t i = 0;
  while (p + i < end && i + 1 < outsz) {
    char c = p[i];
    if (c == '"' || c == '\r' || c == '\n' || c == ' ' || c == '}' || c == ',')
      break;
    out[i] = c;
    i++;
  }
  out[i] = 0;
  return i > 0;
}

static void saveSessionIfChanged(void) {
  static char last[768] = {0};
  char sig[768];
  snprintf(sig, sizeof(sig), "%s|%s|%s|%s|%s|%s",
           gSessUser, gSessToken, gSessLevel, gSessPool, gSessHost, gSessSkyLevel);
  if (strcmp(sig, last) == 0) return;
  if (!gCtx.dataFolder[0]) return;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\session.json", gCtx.dataFolder);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "{\"user\":\"%s\",\"session\":\"%s\",\"level\":\"%s\",\"pool_type\":\"%s\","
    "\"host\":\"%s\",\"sky_level_id\":\"%s\",\"session_id\":\"%s\",\"updated\":%lld}\n",
    gSessUser, gSessToken, gSessLevel, gSessPool,
    gSessHost, gSessSkyLevel, gSessSessionId,
    (long long)GetTickCount64());
  fclose(f);
  strncpy(last, sig, sizeof(last) - 1);

  // server.txt 兜底：AppInfo.tgc 没写出来时，用抓到的 Host
  if (gSessHost[0]) {
    char spath[MAX_PATH];
    snprintf(spath, sizeof(spath), "%s\\server.txt", gCtx.dataFolder);
    int have = 0;
    FILE *sf = fopen(spath, "rb");
    if (sf) {
      char buf[8] = {0};
      if (fgets(buf, sizeof(buf), sf) && buf[0] && buf[0] != '\n' && buf[0] != '\r')
        have = 1;
      fclose(sf);
    }
    if (!have)
      bpWriteServerHost(gSessHost);
  }

  // 不记录 token 本身
  bpLog("session captured: user=%s host=%s level=%s pool=%s",
        gSessUser, gSessHost, gSessLevel, gSessPool);
}

static void captureSession(const char *buf, u32 len) {
  char v[128];

  if (copyValue(buf, len, "\r\nHost: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nhost: ", v, sizeof(v))) {
    strncpy(gSessHost, v, sizeof(gSessHost) - 1);
    gSessHost[sizeof(gSessHost) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nx-sky-level-id: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nX-Sky-Level-Id: ", v, sizeof(v))) {
    strncpy(gSessSkyLevel, v, sizeof(gSessSkyLevel) - 1);
    gSessSkyLevel[sizeof(gSessSkyLevel) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nX-Session-ID: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-session-id: ", v, sizeof(v))) {
    strncpy(gSessSessionId, v, sizeof(gSessSessionId) - 1);
    gSessSessionId[sizeof(gSessSessionId) - 1] = 0;
  }

  if (copyValue(buf, len, "\r\nsession: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-session-token: ", v, sizeof(v)) ||
      copyValue(buf, len, "\"session\":\"", v, sizeof(v))) {
    strncpy(gSessToken, v, sizeof(gSessToken) - 1);
    gSessToken[sizeof(gSessToken) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nuser-id: ", v, sizeof(v)) ||
      copyValue(buf, len, "\"user\":\"", v, sizeof(v))) {
    strncpy(gSessUser, v, sizeof(gSessUser) - 1);
    gSessUser[sizeof(gSessUser) - 1] = 0;
  }
  // 先看 pool_type：只认 recording 类
  int isRec = 0;
  if (copyValue(buf, len, "\"pool_type\":\"", v, sizeof(v))) {
    if (strncmp(v, "Recording", 9) == 0) {
      strncpy(gSessPool, v, sizeof(gSessPool) - 1);
      gSessPool[sizeof(gSessPool) - 1] = 0;
      isRec = 1;
    }
  }
  // 只有 recording 类请求里的 pool_name 才是可靠的地图/子场景名；
  // 忽略 enable_stage_* 这类 AB 开关值。
  if (copyValue(buf, len, "\"pool_name\":\"", v, sizeof(v))) {
    if (isRec && strncmp(v, "enable_", 7) != 0) {
      strncpy(gSessLevel, v, sizeof(gSessLevel) - 1);
      gSessLevel[sizeof(gSessLevel) - 1] = 0;
    }
  }
  saveSessionIfChanged();
}

// ---------------------------------------------------------------------------
// feed 多人元数据（p0id / p1id / p2id / ...）
//
// 从出站 social-feed/create 请求体、入站 feed 响应里提取 players，
// 按 resource_id 合并到 <mod>/feed_meta.json，部署时由 deploy.py 读取。
// ---------------------------------------------------------------------------
static CRITICAL_SECTION gFeedLock;
static int gFeedLockReady = 0;
static char gFeedPath[MAX_PATH] = {0};

static void feedPathInit(void) {
  if (!gFeedPath[0] && gCtx.dataFolder[0])
    snprintf(gFeedPath, sizeof(gFeedPath), "%s\\feed_meta.json", gCtx.dataFolder);
}

static int isPidKey(const char *k) {
  if (!k || k[0] != 'p')
    return 0;
  const char *p = k + 1;
  if (*p < '0' || *p > '9')
    return 0;
  while (*p >= '0' && *p <= '9')
    p++;
  return p[0] == 'i' && p[1] == 'd' && p[2] == 0;
}

static int copyStrField(cJSON *entry, cJSON *src, const char *key) {
  cJSON *v = src ? cJSON_GetObjectItemCaseSensitive(src, key) : NULL;
  if (!cJSON_IsString(v) || !v->valuestring || !v->valuestring[0])
    return 0;
  cJSON_DeleteItemFromObjectCaseSensitive(entry, key);
  cJSON_AddStringToObject(entry, key, v->valuestring);
  return 1;
}

static int copyNumField(cJSON *entry, cJSON *src, const char *key) {
  cJSON *v = src ? cJSON_GetObjectItemCaseSensitive(src, key) : NULL;
  if (!cJSON_IsNumber(v))
    return 0;
  cJSON_DeleteItemFromObjectCaseSensitive(entry, key);
  cJSON_AddNumberToObject(entry, key, v->valuedouble);
  return 1;
}

static void mergeFeedMeta(const char *resource, cJSON *feed, cJSON *extra) {
  if (!resource || !resource[0])
    return;
  if (!gFeedLockReady)
    return;
  feedPathInit();
  if (!gFeedPath[0])
    return;

  EnterCriticalSection(&gFeedLock);

  cJSON *root = NULL;
  FILE *f = fopen(gFeedPath, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n > 0 && n < 4 * 1024 * 1024) {
      char *data = (char *)malloc((size_t)n + 1);
      if (data) {
        size_t got = fread(data, 1, (size_t)n, f);
        data[got] = 0;
        root = cJSON_Parse(data);
        free(data);
      }
    }
    fclose(f);
  }
  if (!cJSON_IsObject(root)) {
    if (root) cJSON_Delete(root);
    root = cJSON_CreateObject();
  }

  cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, resource);
  if (!cJSON_IsObject(entry)) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, resource);
    entry = cJSON_CreateObject();
    cJSON_AddItemToObject(root, resource, entry);
  }

  int changed = 0;
  cJSON *it = NULL;
  if (cJSON_IsObject(extra)) {
    cJSON_ArrayForEach(it, extra) {
      if (!cJSON_IsString(it) || !it->string || !it->valuestring)
        continue;
      if (!isPidKey(it->string))
        continue;
      cJSON_DeleteItemFromObjectCaseSensitive(entry, it->string);
      cJSON_AddStringToObject(entry, it->string, it->valuestring);
      changed = 1;
    }
  }

  // 留影 / 共享空间的类型信息：部署和蓝图分类都要用
  static const char *feedStrKeys[] = {
    "pool_type", "pool_name", "level_id", "social_feed_id",
    "author_id", "message", "content",
  };
  for (size_t i = 0; i < sizeof(feedStrKeys) / sizeof(feedStrKeys[0]); i++)
    changed |= copyStrField(entry, feed, feedStrKeys[i]);

  static const char *extraStrKeys[] = { "pos", "ori", "scale" };
  for (size_t i = 0; i < sizeof(extraStrKeys) / sizeof(extraStrKeys[0]); i++)
    changed |= copyStrField(entry, extra, extraStrKeys[i]);

  changed |= copyNumField(entry, extra, "chunk_count");

  if (changed) {
    char *s = cJSON_PrintUnformatted(root);
    char tmp[MAX_PATH];
    snprintf(tmp, sizeof(tmp), "%s.tmp", gFeedPath);
    FILE *w = fopen(tmp, "wb");
    if (w) {
      if (s) fputs(s, w);
      fclose(w);
      MoveFileExA(tmp, gFeedPath, MOVEFILE_REPLACE_EXISTING);
    }
    if (s) cJSON_free(s);
    bpLog("feed meta: %s", resource);
  }

  cJSON_Delete(root);
  LeaveCriticalSection(&gFeedLock);
}

// 共享空间入口的位置：SharedSpaceCandle feed 的 extra_data.pos 就是空间在
// 世界里的坐标。按作者存一份，部署新空间时可以直接沿用。
static void mergeSpacePos(const char *owner, cJSON *feed, cJSON *extra) {
  if (!owner || !owner[0] || !gCtx.dataFolder[0])
    return;
  cJSON *pos = cJSON_GetObjectItemCaseSensitive(extra, "pos");
  if (!cJSON_IsString(pos) || !pos->valuestring || !pos->valuestring[0])
    return;
  if (!gFeedLockReady)
    return;

  char path[MAX_PATH], tmp[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\space_pos.json", gCtx.dataFolder);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  EnterCriticalSection(&gFeedLock);
  cJSON *root = NULL;
  FILE *f = fopen(path, "rb");
  if (f) {
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    rewind(f);
    if (n > 0 && n < 1024 * 1024) {
      char *data = (char *)malloc((size_t)n + 1);
      if (data) {
        size_t got = fread(data, 1, (size_t)n, f);
        data[got] = 0;
        root = cJSON_Parse(data);
        free(data);
      }
    }
    fclose(f);
  }
  if (!cJSON_IsObject(root)) {
    if (root) cJSON_Delete(root);
    root = cJSON_CreateObject();
  }

  cJSON *entry = cJSON_GetObjectItemCaseSensitive(root, owner);
  if (!cJSON_IsObject(entry)) {
    cJSON_DeleteItemFromObjectCaseSensitive(root, owner);
    entry = cJSON_CreateObject();
    cJSON_AddItemToObject(root, owner, entry);
  }
  copyStrField(entry, extra, "pos");
  copyStrField(entry, extra, "ori");
  copyStrField(entry, feed, "level_id");
  copyStrField(entry, feed, "message");
  copyStrField(entry, feed, "social_feed_id");

  char *s = cJSON_PrintUnformatted(root);
  FILE *w = fopen(tmp, "wb");
  if (w) {
    if (s) fputs(s, w);
    fclose(w);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
  }
  if (s) cJSON_free(s);
  cJSON_Delete(root);
  LeaveCriticalSection(&gFeedLock);
}

static int isSpacePool(cJSON *feed) {
  cJSON *pool = cJSON_GetObjectItemCaseSensitive(feed, "pool_type");
  return cJSON_IsString(pool) && pool->valuestring &&
         strncmp(pool->valuestring, "SharedSpace", 11) == 0;
}

static void captureFeedFromJson(cJSON *root) {
  if (!cJSON_IsObject(root))
    return;

  cJSON *feeds = cJSON_GetObjectItemCaseSensitive(root, "feeds");
  if (cJSON_IsArray(feeds)) {
    cJSON *f = NULL;
    cJSON_ArrayForEach(f, feeds) {
      if (!cJSON_IsObject(f))
        continue;
      cJSON *extra = cJSON_GetObjectItemCaseSensitive(f, "extra_data");
      cJSON *res = NULL;
      if (cJSON_IsObject(extra))
        res = cJSON_GetObjectItemCaseSensitive(extra, "resource_id");
      if (!cJSON_IsString(res))
        res = cJSON_GetObjectItemCaseSensitive(f, "resource_id");
      if (cJSON_IsString(res) && res->valuestring)
        mergeFeedMeta(res->valuestring, f, extra);
      if (isSpacePool(f) && cJSON_IsObject(extra)) {
        cJSON *author = cJSON_GetObjectItemCaseSensitive(f, "author_id");
        if (cJSON_IsString(author) && author->valuestring)
          mergeSpacePos(author->valuestring, f, extra);
      }
    }
    return;
  }

  cJSON *extra = cJSON_GetObjectItemCaseSensitive(root, "extra_data");
  cJSON *res = NULL;
  if (cJSON_IsObject(extra))
    res = cJSON_GetObjectItemCaseSensitive(extra, "resource_id");
  if (cJSON_IsString(res) && res->valuestring)
    mergeFeedMeta(res->valuestring, root, extra);
  if (isSpacePool(root) && cJSON_IsObject(extra)) {
    cJSON *author = cJSON_GetObjectItemCaseSensitive(root, "author_id");
    if (cJSON_IsString(author) && author->valuestring)
      mergeSpacePos(author->valuestring, root, extra);
  }
}

// 共享空间：stage/api/v1/get 返回的文档
// {"status":"OK","stage_id":"DuskStart_CNDL","level_id":"DuskStart",
//  "user_id":"<owner>","sequence":1,"version":0,
//  "props":[{"name":"CharSkyKid_Prop_...","pos":"(x,y,z)","ori":"(x,y,z,w)",
//            "scale":"(1,1,1)","uniqueID":6,...}]}
static void sanitizeName(const char *in, char *out, size_t n) {
  size_t j = 0;
  for (size_t i = 0; in && in[i] && j + 1 < n; i++) {
    char c = in[i];
    int ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
             (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    out[j++] = ok ? c : '_';
  }
  out[j] = 0;
}

static void captureStageFromJson(cJSON *root) {
  if (!cJSON_IsObject(root) || !gCtx.dataFolder[0])
    return;
  cJSON *sid = cJSON_GetObjectItemCaseSensitive(root, "stage_id");
  cJSON *props = cJSON_GetObjectItemCaseSensitive(root, "props");
  if (!cJSON_IsString(sid) || !sid->valuestring || !sid->valuestring[0])
    return;
  if (!cJSON_IsArray(props))
    return;

  cJSON *uid = cJSON_GetObjectItemCaseSensitive(root, "user_id");
  const char *owner = (cJSON_IsString(uid) && uid->valuestring && uid->valuestring[0])
                      ? uid->valuestring : "unknown";

  char dir[MAX_PATH];
  snprintf(dir, sizeof(dir), "%s\\stages", gCtx.dataFolder);
  CreateDirectoryA(dir, NULL);

  char safeSid[128], safeOwner[128];
  sanitizeName(sid->valuestring, safeSid, sizeof(safeSid));
  sanitizeName(owner, safeOwner, sizeof(safeOwner));

  char path[MAX_PATH], tmp[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\%s__%s.json", dir, safeSid, safeOwner);
  snprintf(tmp, sizeof(tmp), "%s.tmp", path);

  char *s = cJSON_PrintUnformatted(root);
  if (!s)
    return;
  if (gFeedLockReady)
    EnterCriticalSection(&gFeedLock);
  FILE *f = fopen(tmp, "wb");
  if (f) {
    fputs(s, f);
    fclose(f);
    MoveFileExA(tmp, path, MOVEFILE_REPLACE_EXISTING);
  }
  if (gFeedLockReady)
    LeaveCriticalSection(&gFeedLock);
  cJSON_free(s);

  bpListNotifyViewedStage(sid->valuestring, owner);
  bpLog("stage captured: %s owner=%s props=%d",
        sid->valuestring, owner, cJSON_GetArraySize(props));
}

static void captureFeedMeta(const char *buf, u32 len) {
  if (!buf || len < 32 || !gFeedLockReady)
    return;
  if (!memFind(buf, len, "p0id") &&
      !memFind(buf, len, "p1id") &&
      !memFind(buf, len, "\"feeds\""))
    return;

  const char *p = memFind(buf, len, "{");
  if (!p)
    return;
  u32 n = (u32)(buf + len - p);
  char *copy = (char *)malloc(n + 1);
  if (!copy)
    return;
  memcpy(copy, p, n);
  copy[n] = 0;
  cJSON *root = cJSON_Parse(copy);
  free(copy);
  if (!root)
    return;
  captureFeedFromJson(root);
  cJSON_Delete(root);
}

// ---------------------------------------------------------------------------
// 分片 feed 响应累积：同一 SSL 上多次 SSL_read 的数据拼成完整 JSON 再解析
// ---------------------------------------------------------------------------
#define BP_FEED_CTX 8
#define BP_FEED_BUF (256 * 1024)

typedef struct {
  u64 ssl;
  u32 len;
  char buf[BP_FEED_BUF];
} FeedCtx;

static FeedCtx *gFeedCtx = NULL;

static FeedCtx *feedCtxFor(u64 ssl) {
  if (!gFeedCtx)
    return NULL;
  for (int i = 0; i < BP_FEED_CTX; i++)
    if (gFeedCtx[i].ssl == ssl)
      return &gFeedCtx[i];
  for (int i = 0; i < BP_FEED_CTX; i++) {
    if (gFeedCtx[i].ssl == 0) {
      gFeedCtx[i].ssl = ssl;
      gFeedCtx[i].len = 0;
      return &gFeedCtx[i];
    }
  }
  gFeedCtx[0].ssl = ssl;
  gFeedCtx[0].len = 0;
  return &gFeedCtx[0];
}

static void feedAccumulate(u64 ssl, const char *buf, u32 len) {
  if (!gFeedCtx || !buf || !len || !gFeedLockReady)
    return;
  FeedCtx *c = feedCtxFor(ssl);
  if (!c)
    return;

  // 只对可能是 feed / stage 响应的数据开始累积
  if (c->len == 0 &&
      !memFind(buf, len, "\"feeds\"") &&
      !memFind(buf, len, "\"extra_data\"") &&
      !memFind(buf, len, "p0id") &&
      !memFind(buf, len, "\"stage_id\"") &&
      !memFind(buf, len, "Content-Type: application/json"))
    return;

  if ((u64)c->len + len > BP_FEED_BUF) {
    c->len = 0;
    if (len > BP_FEED_BUF)
      return;
  }
  memcpy(c->buf + c->len, buf, len);
  c->len += len;

  while (c->len > 0) {
    u32 start = 0;
    int haveStart = 0;
    for (u32 i = 0; i < c->len; i++) {
      if (c->buf[i] == '{') {
        start = i;
        haveStart = 1;
        break;
      }
    }
    if (!haveStart) {
      c->len = 0;
      break;
    }

    int depth = 0, inStr = 0, esc = 0;
    u32 end = 0;
    int found = 0;
    for (u32 i = start; i < c->len; i++) {
      char ch = c->buf[i];
      if (inStr) {
        if (esc) esc = 0;
        else if (ch == '\\') esc = 1;
        else if (ch == '"') inStr = 0;
        continue;
      }
      if (ch == '"') {
        inStr = 1;
      } else if (ch == '{') {
        depth++;
      } else if (ch == '}') {
        depth--;
        if (depth == 0) {
          end = i;
          found = 1;
          break;
        }
      }
    }

    if (!found) {
      if (start > 0) {
        memmove(c->buf, c->buf + start, c->len - start);
        c->len -= start;
      }
      break;
    }

    u32 objlen = end - start + 1;
    char *obj = (char *)malloc(objlen + 1);
    if (obj) {
      memcpy(obj, c->buf + start, objlen);
      obj[objlen] = 0;
      cJSON *root = cJSON_Parse(obj);
      if (root) {
        captureStageFromJson(root);
        captureFeedFromJson(root);
        cJSON_Delete(root);
      }
      free(obj);
    }

    u32 consumed = end + 1;
    memmove(c->buf, c->buf + consumed, c->len - consumed);
    c->len -= consumed;
  }
}

// 判断是否 HTTP 明文（请求行或响应行）
static i32 looksHttp(const u8 *b, u32 len) {
  if (len < 8) return 0;
  if (b[0]=='H'&&b[1]=='T'&&b[2]=='T'&&b[3]=='P'&&b[4]=='/') return 1;
  if (b[0]=='G'&&b[1]=='E'&&b[2]=='T'&&b[3]==' ') return 1;
  if (b[0]=='P'&&b[1]=='O'&&b[2]=='S'&&b[3]=='T'&&b[4]==' ') return 1;
  if (b[0]=='P'&&b[1]=='U'&&b[2]=='T'&&b[3]==' ') return 1;
  if (b[0]=='D'&&b[1]=='E'&&b[2]=='L'&&b[3]=='E') return 1;
  if (b[0]=='H'&&b[1]=='E'&&b[2]=='A'&&b[3]=='D') return 1;
  if (b[0]=='O'&&b[1]=='P'&&b[2]=='T'&&b[3]=='I') return 1;
  return 0;
}

static void saveSample(TlsSample *arr, volatile u32 *counter, u32 *capacity,
                       const void *buf, u32 len, u32 limit) {
  if (!arr || !buf || len < 256)     // 留影 block 分片都远大于 256 字节
    return;

  u32 idx = (*counter) % BP_TLS_MAX_SAMPLES;   // 环形覆盖，始终保留最新数据

  u32 take = len < limit ? len : limit;
  if (take > BP_TLS_CAPTURE_BYTES) take = BP_TLS_CAPTURE_BYTES;
  take = safeLen(buf, take);      // 跨页时截断，而不是整段放弃
  if (take < 32)
    return;
  // 跳过全零缓冲区（TLS 填充 / 未使用的输出缓冲）
  {
    const u8 *z = (const u8 *)buf;
    i32 nonzero = 0;
    for (u32 i = 0; i < take && i < 64; i++)
      if (z[i]) { nonzero = 1; break; }
    if (!nonzero)
      return;
  }
  // 跳过 remote config（包含 "vars":{ 或 enable_* 特性开关）这类噪音
  {
    const char *t = (const char *)buf;
    u32 scan = take < 900 ? take : 900;
    for (u32 i = 0; i + 7 < scan; i++) {
      if (memcmp(t + i, "\"vars\":{", 8) == 0) return;
      if (memcmp(t + i, "enable_", 7) == 0 &&
          (i == 0 || t[i - 1] == '"' || t[i - 1] == ','))
        return;
    }
  }
  (void)looksHttp;

  TlsSample *s = &arr[idx];
  s->len = len;
  s->captured = take;
  memcpy(s->data, buf, take);
  *capacity = take;
  *counter = idx + 1;

  // 自动提取 /service/recording/api/v2/get 响应里的 link 地址。
  // 判据：同一段数据里同时出现 seek_table 和 "link":"http
  {
    const char *t = (const char *)buf;
    if (strstr(t, "seek_table") && strstr(t, "\"link\"")) {
      const char *lp = strstr(t, "\"link\":\"");
      if (lp) {
        lp += 8;
        const char *le = strchr(lp, '"');
        if (le && (le - lp) < 500) {
          char url[512];
          size_t n = (size_t)(le - lp);
          memcpy(url, lp, n);
          url[n] = 0;
          if (gCtx.dataFolder[0]) {
            char lp2[MAX_PATH];
            snprintf(lp2, sizeof(lp2), "%s\\links.txt", gCtx.dataFolder);
            FILE *lf = fopen(lp2, "a");
            if (lf) { fprintf(lf, "%s\n", url); fclose(lf); }
          }
          bpLog("recording link captured: %s", url);
          bpListNotifyViewed(url);
        }
      }
    }
  }

  // 立刻落盘：之前依赖 bpTlsDumpReport 周期性写，导致大量数据只留在内存里丢失
  if (gCtx.capFolder[0] && bpDumpOk(idx, 400)) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\cap_%04u.bin", gCtx.capFolder, idx);
    FILE *fp = fopen(path, "wb");
    if (fp) {
      fwrite(s->data, 1, take, fp);
      fclose(fp);
    }
  }
}

// 注意：0x1D45F20 是「读」型函数（内部 lea r9,[rsp+0x58] 自建 written，
// 成功时返回读取字节数）。数据必须在 orig 返回之后读，否则只能拿到残留内容。
// 留影 ndjson 检测：记录是否命中、长度（本步只检测，不改写内容）
static volatile u32 gNdjsonHits = 0;
static volatile u32 gNdjsonMaxLen = 0;

static int detour_SslWriteEx(u64 ssl, void *buf, int num, u64 *written) {
  gWriteCalls++;
  int ret = g_origSslWriteEx ? g_origSslWriteEx(ssl, buf, num, written) : 0;

  if (ret > 0 && buf) {
    // 入站 feed 响应（get_by_ids / get_user_feeds）里的多人 pNid
    feedAccumulate(ssl, (const char *)buf, (u32)ret);
    captureFeedMeta((const char *)buf, (u32)ret);

    const char *t = (const char *)buf;
    if (strstr(t, "ver" "\"" ":1") &&
        strstr(t, "stid" "\"")) {
      gNdjsonHits++;
      if ((u32)ret > gNdjsonMaxLen) gNdjsonMaxLen = (u32)ret;
      if (gNdjsonHits <= 12) {
        char clean[240] = {0};
        u32 n = (u32)ret < 180 ? (u32)ret : 180;
        for (u32 i = 0; i < n; i++) {
          char c = t[i];
          clean[i] = (c == 0x0D || c == 0x0A) ? '.' : c;
        }
        bpLog("NDJSON hit#%u len=%d: %s", gNdjsonHits, ret, clean);
      }
    }
    static u32 cap = 0;
    saveSample(gOutSamples, &gOutSaved, &cap, buf, (u32)ret, (u32)num);
  }
  return ret;
}

// 出站侧（0x1D45F80）：实测该函数是 3 参数 (ssl, buf, num)，内部自己 lea r9 建 written。
// 之前按 4 参数 hook 会崩；先用 3 参数 + 只计数验证稳定性。
typedef int (*PFN_SslOut)(u64, void *, int);
static PFN_SslOut g_origSslOut = NULL;
static volatile u32 gOutReqCalls = 0;

static int detour_SslReadEx(u64 ssl, void *buf, int num, u64 *readbytes) {
  (void)ssl; (void)readbytes;
  gReadCalls++;
  (void)num; (void)buf;
  return 0;
}

void bpTlsDumpReport(void) {
  bpLog("TLS write=%u read=%u  outSaved=%u inSaved=%u",
    gWriteCalls, gReadCalls, gOutSaved, gInSaved);

  for (u32 i = 0; i < gOutSaved; i++) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\tls_out_%02u.bin", gCtx.capFolder, i);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(gOutSamples[i].data, 1, gOutSamples[i].captured, f); fclose(f); }
    const char *hit = "?";
    if (strstr(gOutSamples[i].data, "social-feed")) hit = "SOCIAL-FEED";
    else if (strstr(gOutSamples[i].data, "recording")) hit = "RECORDING";
    else if (strstr(gOutSamples[i].data, "message")) hit = "MESSAGE";
    else if (strstr(gOutSamples[i].data, "placeable")) hit = "PLACEABLE";
    else if (strstr(gOutSamples[i].data, "feed_id")) hit = "FEED";
    else if (strncmp(gOutSamples[i].data, "HTTP/", 5) == 0) hit = "http";
    bpLog("  OUT#%03u len=%u %s", i, gOutSamples[i].len, hit);
  }
  for (u32 i = 0; i < gInSaved; i++) {
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\tls_in_%02u.bin", gCtx.capFolder, i);
    FILE *f = fopen(path, "wb");
    if (f) { fwrite(gInSamples[i].data, 1, gInSamples[i].captured, f); fclose(f); }
    bpLog("  IN#%02u len=%u", i, gInSamples[i].len);
  }
}

static void hookSslEx(void) {
  HMODULE hGame = GetModuleHandleA(NULL);

  if (!gOutSamples)
    gOutSamples = (TlsSample *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
      sizeof(TlsSample) * BP_TLS_MAX_SAMPLES);
  if (!gInSamples)
    gInSamples = (TlsSample *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
      sizeof(TlsSample) * BP_TLS_MAX_SAMPLES);
  if (!gOutSamples || !gInSamples) {
    bpLog("TLS dump: alloc failed");
    return;
  }

  u8 *w = (u8 *)hGame + BP_SSL_WRITE_EX_RVA;
  u8 *r = (u8 *)hGame + BP_SSL_READ_EX_RVA;
  bpLog("SSL_write_ex@%p bytes: %02X %02X %02X %02X", w, w[0], w[1], w[2], w[3]);
  bpLog("SSL_read_ex @%p bytes: %02X %02X %02X %02X", r, r[0], r[1], r[2], r[3]);

  // 序言校验：游戏更新后地址会变，校验失败就不装 hook（宁可没功能，也不要闪退）
  static const u8 expect[] = { 0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8 };
  if (memcmp(w, expect, sizeof(expect)) != 0) {
    bpLog("SSL_write_ex prologue MISMATCH -> skip hook (game version changed?)");
    return;
  }
  if (memcmp(r, expect, sizeof(expect)) != 0) {
    bpLog("SSL_read_ex prologue MISMATCH -> skip hook (game version changed?)");
    return;
  }

#ifndef BP_ENABLE_SSL_OUT
#define BP_ENABLE_SSL_OUT 1
#endif
#if BP_ENABLE_SSL_OUT
  HTStatus s1 = HTAsmHookCreateRaw(gModuleDll, w, (LPVOID)detour_SslWriteEx,
    (LPVOID *)&g_origSslWriteEx);
  HTStatus e1 = (s1 == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, w) : HT_FAIL;
  bpLog("OUT hook(0x1D45CB0) create=%d enable=%d orig=%p", (int)s1, (int)e1, g_origSslWriteEx);
#else
  bpLog("OUT hook(0x1D45CB0) DISABLED for isolation test");
  (void)detour_SslWriteEx; (void)w;
#endif

#if BP_ENABLE_SSL_READ
  HTStatus s2 = HTAsmHookCreateRaw(gModuleDll, r, (LPVOID)detour_SslReadEx,
    (LPVOID *)&g_origSslReadEx);
  HTStatus e2 = (s2 == HT_SUCCESS) ? HTAsmHookEnable(gModuleDll, r) : HT_FAIL;
  bpLog("SSL_read_ex hook create=%d enable=%d orig=%p", (int)s2, (int)e2, g_origSslReadEx);
#else
  bpLog("SSL_read_ex hook DISABLED (stability test)");
  (void)detour_SslReadEx; (void)r;
#endif
}

static int detour_SslOut(u64 ssl, void *buf, int num) {
  u32 idx = (u32)InterlockedIncrement((LONG *)&gOutReqCalls);

  // 先记一行“确实进来了”，便于区分「没命中」和「命中后崩溃」
  if (idx <= 12)
    bpLog("OUT entry #%u ssl=%p buf=%p num=%d", idx, (void *)ssl, buf, num);

  // 关键：出站数据必须在 orig 之前抓。orig 返回后调用方可能已经释放/复用
  // 这块缓冲区，再读就是野指针——这正是之前“无日志直接闪退”的原因。
  if (buf && num > 0 && gCtx.dataFolder[0]) {
    u32 want = (u32)num;
    if (want > 65536) want = 65536;          // 防御异常大的长度
    u32 take = safeLen(buf, want);           // 逐页校验，避免跨页越界
    if (take >= 8) {
      const u8 *b = (const u8 *)buf;
      i32 http = looksHttp(b, take) || (take >= 5 && memcmp(b, "HTTP/", 5) == 0);

      // 自动提取 user / session / 当前 level，供游戏内一键部署使用
      if (memFind((const char *)buf, take, "\r\nsession: ") ||
          memFind((const char *)buf, take, "\"session\":\""))
        captureSession((const char *)buf, take);

      // 自动提取多人 feed 的 p0id/p1id/...（social-feed/create 请求体）
      if (memFind((const char *)buf, take, "\"extra_data\"") ||
          memFind((const char *)buf, take, "p0id"))
        captureFeedMeta((const char *)buf, take);

      // 写操作（stage set / recording create|update / feed create）不参与限流，
      // 这些是部署链路唯一可参考的样本
      // stage set 的请求体和请求头是两次 SSL_write：第二次只有 JSON。
      // 只要同时出现 stage_id + props 就当作写请求抓下来。
      int stageBody = memFind((const char *)buf, take, "\"stage_id\"") != NULL &&
                      memFind((const char *)buf, take, "\"props\"") != NULL;
      int writeReq = stageBody ||
          memFind((const char *)buf, take, "/service/stage/api/v1/set") != NULL ||
          memFind((const char *)buf, take, "/service/stage/api/v1/create") != NULL ||
          memFind((const char *)buf, take, "/service/stage/api/v1/update") != NULL ||
          memFind((const char *)buf, take, "/service/recording/api/v2/create") != NULL ||
          memFind((const char *)buf, take, "/service/recording/api/v2/update") != NULL ||
          memFind((const char *)buf, take, "/service/social-feed/api/v1/create") != NULL;

      if (writeReq || bpDumpOk(idx, 300)) {
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\req_%05u.bin", gCtx.capFolder, idx);
        FILE *fp = fopen(path, "wb");
        if (fp) { fwrite(buf, 1, take, fp); fclose(fp); }
      }

      if (http || idx <= 12) {
        char clean[900] = {0};
        u32 n = take < 860 ? take : 860;
        for (u32 i = 0; i < n; i++) {
          char c = ((const char *)buf)[i];
          clean[i] = (c == 0x0D || c == 0x0A) ? ' ' :
                     (c >= 0x20 && c < 0x7F ? c : '*');
        }
        bpLog("OUT req#%u num=%d take=%u%s: %s", idx, num, take,
              http ? " HTTP" : "", clean);
      }
    }
  }

  return g_origSslOut ? g_origSslOut(ssl, buf, num) : 0;
}

void bpTlsOutHookInit(void) {
  HMODULE hGame = GetModuleHandleA(NULL);
  u8 *addr = (u8 *)hGame + BP_SSL_READ_EX_RVA;    // 0x1D45F80 = 出站写入
  bpLog("OUT-REQ cand %p bytes: %02X %02X %02X %02X %02X %02X",
    addr, addr[0], addr[1], addr[2], addr[3], addr[4], addr[5]);

  static const u8 expect[] = { 0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8 };
  if (memcmp(addr, expect, sizeof(expect)) != 0) {
    bpLog("OUT-REQ prologue MISMATCH -> skip");
    return;
  }

  // 用手动 hook：只覆盖前 5 字节（mov eax,0x38），避免 MinHook 截断后续 call
  void *tramp = bpManualHookInstall(addr, (void *)detour_SslOut);
  if (!tramp) {
    bpLog("OUT-REQ manual hook FAILED");
    return;
  }
  g_origSslOut = (PFN_SslOut)tramp;
  bpLog("OUT-REQ manual hook OK, tramp=%p", tramp);
}

void bpTlsDumpInit(void) {
#if BP_SAFE_MODE
  bpLog("SAFE MODE: TLS hook skipped");
  return;
#endif
  if (!gFeedLockReady) {
    InitializeCriticalSection(&gFeedLock);
    gFeedLockReady = 1;
  }
  if (!gFeedCtx)
    gFeedCtx = (FeedCtx *)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                    sizeof(FeedCtx) * BP_FEED_CTX);
  hookSslEx();
}
