// ---------------------------------------------------------------------------
// 国服（NetEase 0.16.7 build 421709）实际抓取
//
//   写: 0x21A22D0  (SSL_write)  -> 解析请求头/体，提取 session/user/host/pool
//   读: 0x21A2210  (SSL_read)   -> 按 SSL 对象重组 HTTP 响应，提取 recording link
//
// 复用国际服的 session.json / links.txt 约定，autofetch.py 可直接消费。
// ---------------------------------------------------------------------------

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>
#include "bp.h"
#define CJSON_HIDE_SYMBOLS
#include "../libraries/htmodloader/includes/cJSON/cJSON.h"

#define CN_READ_RVA  0x21A2210
#define CN_WRITE_RVA 0x21A22D0
#define CN_CTX 8
#define CN_BUF 65536

typedef int (*PFN_Io)(u64, void *, int);
static PFN_Io g_trampRead = NULL;
static PFN_Io g_trampWrite = NULL;
static PFN_Io g_trampW2 = NULL;
static PFN_Io g_trampW3 = NULL;

static char gUser[80] = {0};
static char gSess[96] = {0};
static char gLevel[128] = {0};
static char gPool[64] = {0};
static char gHost[160] = {0};
static char gRecId[80] = {0};
static char gLastSession[640] = {0};
static u8 gSession2[256];
static u32 gSession2Len = 0;
static char gSkyLevel[32] = {0};
static char gSessionId[80] = {0};
static volatile LONG gReqIdx = 0;

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

static int containsCI(const char *hay, u32 n, const char *needle) {
  u32 m = (u32)strlen(needle);
  if (!m || n < m) return 0;
  for (u32 i = 0; i + m <= n; i++) {
    u32 j = 0;
    while (j < m) {
      char a = hay[i + j], b = needle[j];
      if (a >= 'A' && a <= 'Z') a += 32;
      if (b >= 'A' && b <= 'Z') b += 32;
      if (a != b) break;
      j++;
    }
    if (j == m) return 1;
  }
  return 0;
}

static void captureHeaderIds(const char *buf, u32 len);

// 把请求的完整头部/样本落盘，用于找出国服认证字段
static void dumpHeaders(const char *buf, u32 len) {
  if (len < 4) return;
  if (!(memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "POST", 4) == 0 ||
        memcmp(buf, "PUT ", 4) == 0 || memcmp(buf, "HEAD", 4) == 0))
    return;

  u32 hdrEnd = len;
  for (u32 i = 0; i + 3 < len; i++) {
    if (buf[i] == '\r' && buf[i+1] == '\n' &&
        buf[i+2] == '\r' && buf[i+3] == '\n') {
      hdrEnd = i;
      break;
    }
  }
  if (hdrEnd > 2048) hdrEnd = 2048;
  captureHeaderIds(buf, hdrEnd);

  // 环形保存完整请求（最多 64 个）
  u32 idx = (u32)InterlockedIncrement((LONG *)&gReqIdx) % 64;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\cn_req_%02u.bin", gCtx.capFolder, idx);
  FILE *rf = fopen(path, "wb");
  if (rf) {
    u32 take = len > 65536 ? 65536 : len;
    fwrite(buf, 1, take, rf);
    fclose(rf);
  }

  // 只把可能含认证/身份的行追加到 cn_hdr.log
  snprintf(path, sizeof(path), "%s\\cn_hdr.log", gCtx.dataFolder);
  FILE *f = fopen(path, "a");
  if (!f) return;

  fprintf(f, "=== #%u len=%u ===\n", idx, len);
  u32 start = 0;
  for (u32 i = 0; i <= hdrEnd; i++) {
    if (i == hdrEnd || buf[i] == '\n') {
      u32 linelen = i - start;
      while (linelen && (buf[start + linelen - 1] == '\r'))
        linelen--;
      if (linelen > 0) {
        int interesting =
          containsCI(buf + start, linelen, "auth") ||
          containsCI(buf + start, linelen, "token") ||
          containsCI(buf + start, linelen, "session") ||
          containsCI(buf + start, linelen, "cookie") ||
          containsCI(buf + start, linelen, "user") ||
          containsCI(buf + start, linelen, "host") ||
          containsCI(buf + start, linelen, "x-");
        if (interesting || start == 0) {
          char tmp[512];
          u32 n = linelen < sizeof(tmp) - 1 ? linelen : sizeof(tmp) - 1;
          memcpy(tmp, buf + start, n);
          tmp[n] = 0;
          fprintf(f, "%.*s\n", (int)n, tmp);
        }
      }
      start = i + 1;
    }
  }
  fclose(f);
}

// 从请求头抓 x-sky-level-id / X-Session-ID（部署时需要）
static void captureHeaderIds(const char *buf, u32 len) {
  char v[128];
  if (copyValue(buf, len, "\r\nx-sky-level-id: ", v, sizeof(v))) {
    strncpy(gSkyLevel, v, sizeof(gSkyLevel) - 1);
    gSkyLevel[sizeof(gSkyLevel) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nX-Session-ID: ", v, sizeof(v))) {
    strncpy(gSessionId, v, sizeof(gSessionId) - 1);
    gSessionId[sizeof(gSessionId) - 1] = 0;
  }
}

// 保存非请求行（body）样本：国服 create/update 的字段名未知，先全部落盘
static void saveBodySample(const char *buf, u32 len) {
  if (len < 32) return;
  if (memcmp(buf, "GET ", 4) == 0 || memcmp(buf, "POST", 4) == 0 ||
      memcmp(buf, "PUT ", 4) == 0 || memcmp(buf, "HEAD", 4) == 0)
    return;

  u32 idx = (u32)InterlockedIncrement((LONG *)&gReqIdx) % 64;
  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\cn_body_%02u.bin", gCtx.capFolder, idx);
  FILE *f = fopen(path, "wb");
  if (f) {
    u32 take = len > 65536 ? 65536 : len;
    fwrite(buf, 1, take, f);
    fclose(f);
  }
  char clean[300] = {0};
  u32 n = len < 280 ? len : 280;
  for (u32 i = 0; i < n; i++) {
    char c = buf[i];
    clean[i] = (c == '\r' || c == '\n') ? ' ' :
               (c >= 0x20 && c < 0x7F ? c : '*');
  }
  bpLog("CN body: %s", clean);
}

static void saveSession(void) {
  char sig[640];
  // host 不参与变化判断：NetEase 各服务域名频繁切换，不应触发重写/日志
  snprintf(sig, sizeof(sig), "%s|%s|%s|%s",
           gUser, gSess, gLevel, gPool);
  if (strcmp(sig, gLastSession) == 0) return;
  if (!gCtx.dataFolder[0]) return;

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\session.json", gCtx.dataFolder);
  FILE *f = fopen(path, "w");
  if (!f) return;
  fprintf(f,
    "{\"user\":\"%s\",\"session\":\"%s\",\"level\":\"%s\","
    "\"pool_type\":\"%s\",\"host\":\"%s\",\"sky_level_id\":\"%s\","
    "\"session_id\":\"%s\",\"updated\":%lld}\n",
    gUser, gSess, gLevel, gPool, gHost, gSkyLevel, gSessionId,
    (long long)GetTickCount64());
  fclose(f);
  strncpy(gLastSession, sig, sizeof(gLastSession) - 1);
  // 国服地址由网易固定，直接把抓到的 Host 交给部署工具
  if (gHost[0])
    bpWriteServerHost(gHost);
  // stage 文档可能不带 user_id，用当前登录用户兜底
  if (gUser[0])
    bpCaptureSetOwnerFallback(gUser);
  bpLog("CN session: user=%s level=%s pool=%s host=%s",
        gUser, gLevel, gPool, gHost);
}

// 记录每个 SSL 上待下载的 ugc Range，响应体到达时按起始偏移落盘
typedef struct {
  u64 ssl;
  u32 start;
  u32 end;
  int active;
} CnPend;
static CnPend gPend[CN_CTX];

static CnPend *pendFor(u64 ssl) {
  for (int i = 0; i < CN_CTX; i++)
    if (gPend[i].ssl == ssl)
      return &gPend[i];
  for (int i = 0; i < CN_CTX; i++) {
    if (gPend[i].ssl == 0) {
      gPend[i].ssl = ssl;
      return &gPend[i];
    }
  }
  gPend[0].ssl = ssl;
  return &gPend[0];
}

static void captureRequest(const char *buf, u32 len, u64 ssl) {
  char v[160];

  if (memFind(buf, len, "/service/recording/api/v2/ugc")) {
    const char *r = memFind(buf, len, "Range: bytes=");
    if (r) {
      u32 s = 0, e = 0;
      sscanf(r + 13, "%u-%u", &s, &e);
      CnPend *p = pendFor(ssl);
      p->start = s;
      p->end = e;
      p->active = 1;
      bpLog("CN ugc range: %u-%u", s, e);
    }
  }

  // 保存版：不再落盘请求体/头部（避免网易 SDK 心跳刷盘）
  if (copyValue(buf, len, "\r\nAuthorization: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nauthorization: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-netease-session: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-session: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nsession: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-session-token: ", v, sizeof(v)) ||
      copyValue(buf, len, "\"session\":\"", v, sizeof(v))) {
    strncpy(gSess, v, sizeof(gSess) - 1);
    gSess[sizeof(gSess) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nuser-id: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nx-user-id: ", v, sizeof(v)) ||
      copyValue(buf, len, "\"user\":\"", v, sizeof(v))) {
    strncpy(gUser, v, sizeof(gUser) - 1);
    gUser[sizeof(gUser) - 1] = 0;
  }
  if (copyValue(buf, len, "\r\nHost: ", v, sizeof(v)) ||
      copyValue(buf, len, "\r\nhost: ", v, sizeof(v))) {
    strncpy(gHost, v, sizeof(gHost) - 1);
    gHost[sizeof(gHost) - 1] = 0;
  }

  int isRec = 0;
  if (copyValue(buf, len, "\"pool_type\":\"", v, sizeof(v))) {
    if (strncmp(v, "Recording", 9) == 0) {
      strncpy(gPool, v, sizeof(gPool) - 1);
      gPool[sizeof(gPool) - 1] = 0;
      isRec = 1;
    }
  }
  if (copyValue(buf, len, "\"pool_name\":\"", v, sizeof(v))) {
    if (isRec && strncmp(v, "enable_", 7) != 0) {
      strncpy(gLevel, v, sizeof(gLevel) - 1);
      gLevel[sizeof(gLevel) - 1] = 0;
    }
  }

  saveSession();
}

// ---------------------------------------------------------------------------
// 响应重组 + link 提取
// ---------------------------------------------------------------------------
typedef struct {
  u64 ssl;
  u32 len;
  char buf[CN_BUF];
} CnCtx;

static CnCtx gCtxs[CN_CTX];

static CnCtx *ctxFor(u64 ssl) {
  for (int i = 0; i < CN_CTX; i++)
    if (gCtxs[i].ssl == ssl)
      return &gCtxs[i];
  for (int i = 0; i < CN_CTX; i++) {
    if (gCtxs[i].ssl == 0) {
      gCtxs[i].ssl = ssl;
      gCtxs[i].len = 0;
      return &gCtxs[i];
    }
  }
  // 满了就复用第一个
  gCtxs[0].ssl = ssl;
  gCtxs[0].len = 0;
  return &gCtxs[0];
}

static void saveLink(const char *url) {
  if (!gCtx.dataFolder[0]) return;

  // 从 /ugc/{user}/{recording}.ndjson 提取 recording id，用于分块文件命名
  const char *slash = strrchr(url, '/');
  if (slash) {
    const char *name = slash + 1;
    size_t n = strcspn(name, ".");
    if (n > 0 && n < sizeof(gRecId)) {
      memcpy(gRecId, name, n);
      gRecId[n] = 0;
    }
  }

  char path[MAX_PATH];
  snprintf(path, sizeof(path), "%s\\links.txt", gCtx.dataFolder);
  FILE *f = fopen(path, "a");
  if (f) {
    fprintf(f, "%s\n", url);
    fclose(f);
  }
  bpLog("CN recording link: %s", url);
  bpListNotifyViewed(url);
}

static u8 b64Value(unsigned char c) {
  if (c >= 'A' && c <= 'Z') return (u8)(c - 'A');
  if (c >= 'a' && c <= 'z') return (u8)(c - 'a' + 26);
  if (c >= '0' && c <= '9') return (u8)(c - '0' + 52);
  if (c == '+') return 62;
  if (c == '/') return 63;
  return 0xFF;
}

static int decodeBase64(const char *src, u32 len, u8 *out, u32 cap, u32 *outLen) {
  u32 n = 0;
  u32 acc = 0;
  int bits = 0;
  for (u32 i = 0; i < len; i++) {
    unsigned char c = (unsigned char)src[i];
    if (c == '=') break;
    if (c == '\r' || c == '\n' || c == ' ' || c == '\t') continue;
    u8 v = b64Value(c);
    if (v == 0xFF) return 0;
    acc = (acc << 6) | v;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      if (n >= cap) return 0;
      out[n++] = (u8)(acc >> bits);
    }
  }
  *outLen = n;
  return 1;
}

static void rc4Crypt(const u8 *key, u32 keyLen, const u8 *in, u32 len, u8 *out) {
  u8 s[256];
  for (int i = 0; i < 256; i++) s[i] = (u8)i;
  u32 j = 0;
  for (u32 i = 0; i < 256; i++) {
    j = (j + s[i] + key[i % keyLen]) & 0xFF;
    u8 t = s[i]; s[i] = s[j]; s[j] = t;
  }
  u32 i = 0; j = 0;
  for (u32 n = 0; n < len; n++) {
    i = (i + 1) & 0xFF;
    j = (j + s[i]) & 0xFF;
    u8 t = s[i]; s[i] = s[j]; s[j] = t;
    out[n] = in[n] ^ s[(s[i] + s[j]) & 0xFF];
  }
}

static int gunzipData(const u8 *src, u32 len, u8 *dst, u32 cap, u32 *outLen) {
  z_stream z;
  memset(&z, 0, sizeof(z));
  z.next_in = (Bytef *)src;
  z.avail_in = len;
  z.next_out = dst;
  z.avail_out = cap;
  if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) return 0;
  int rc = inflate(&z, Z_FINISH);
  *outLen = (u32)z.total_out;
  inflateEnd(&z);
  return rc == Z_STREAM_END && *outLen <= cap;
}

static void captureSession2FromLogin(const u8 *plain, u32 len) {
  if (!plain || !len) return;
  cJSON *root = cJSON_ParseWithLength((const char *)plain, len);
  if (!root) return;
  cJSON *v = cJSON_GetObjectItemCaseSensitive(root, "session2");
  if (cJSON_IsString(v) && v->valuestring) {
    u32 n = (u32)strlen(v->valuestring);
    if (n && n <= sizeof(gSession2)) {
      memcpy(gSession2, v->valuestring, n);
      gSession2Len = n;
      // 交给 Python 部署工具（本地敏感文件，不要外传）
      if (gCtx.dataFolder[0]) {
        char p[MAX_PATH];
        snprintf(p, sizeof(p), "%s\\session2.txt", gCtx.dataFolder);
        FILE *sf = fopen(p, "w");
        if (sf) { fputs(v->valuestring, sf); fclose(sf); }
      }
      bpLog("CN session2 captured (%u bytes)", n);
    }
  }
  cJSON_Delete(root);
}

static int decodeCnResponse(const char *headers, u32 headerLen,
                            const char *body, u32 bodyLen,
                            u8 *out, u32 outCap, u32 *outLen) {
  int isGzip = containsCI(headers, headerLen, "content-encoding: gzip");
  int isSession = containsCI(headers, headerLen, "content-encoding: session-valid");
  int isGzipSession = containsCI(headers, headerLen, "content-encoding: gzip-session-valid");

  if (!isGzip && !isSession && !isGzipSession) return 0;
  if (isGzipSession || isSession) {
    if (!gSession2Len || bodyLen > outCap) return 0;
    rc4Crypt(gSession2, gSession2Len, (const u8 *)body, bodyLen, out);
    if (isGzipSession) {
      u8 *tmp = (u8 *)malloc(outCap);
      if (!tmp) return 0;
      memcpy(tmp, out, bodyLen);
      int ok = gunzipData(tmp, bodyLen, out, outCap, outLen);
      free(tmp);
      return ok;
    }
    *outLen = bodyLen;
    return 1;
  }

  return gunzipData((const u8 *)body, bodyLen, out, outCap, outLen);
}

static void processResponse(const char *body, u32 len) {
  if (len < 16) return;

  // 复用国际服的解析：共享空间 stage 文档 / feed（多人、空间坐标、留言）
  bpCaptureStageJson(body, len);
  bpCaptureFeedJson(body, len);

  // 从 get_user_feeds 等响应里学习当前 level/pool
  {
    char v[160];
    int isRec = 0;
    if (copyValue(body, len, "\"pool_type\":\"", v, sizeof(v))) {
      if (strncmp(v, "Recording", 9) == 0) {
        strncpy(gPool, v, sizeof(gPool) - 1);
        gPool[sizeof(gPool) - 1] = 0;
        isRec = 1;
      }
    }
    if (copyValue(body, len, "\"pool_name\":\"", v, sizeof(v))) {
      if (isRec && strncmp(v, "enable_", 7) != 0 && v[0]) {
        strncpy(gLevel, v, sizeof(gLevel) - 1);
        gLevel[sizeof(gLevel) - 1] = 0;
      }
    }
    saveSession();
  }

  if (!memFind(body, len, "seek_table")) return;
  if (!memFind(body, len, "\"link\"")) return;

  const char *p = memFind(body, len, "\"link\":\"");
  if (!p) return;
  p += 8;
  const char *end = body + len;
  const char *q = p;
  while (q < end && *q != '"') q++;
  if (q >= end || q - p >= 500) return;

  char url[512];
  memcpy(url, p, (size_t)(q - p));
  url[q - p] = 0;
  saveLink(url);
}

static void consume(CnCtx *c) {
  while (c->len > 0) {
    // 找响应头结束
    const char *he = NULL;
    for (u32 i = 0; i + 3 < c->len; i++) {
      if (c->buf[i] == '\r' && c->buf[i+1] == '\n' &&
          c->buf[i+2] == '\r' && c->buf[i+3] == '\n') {
        he = c->buf + i + 4;
        break;
      }
    }
    if (!he) return;

    u32 headerLen = (u32)(he - c->buf);
    // 解析 Content-Length
    u32 contentLen = 0;
    {
      const char *cl = memFind(c->buf, headerLen, "Content-Length: ");
      if (!cl) cl = memFind(c->buf, headerLen, "content-length: ");
      if (cl) {
        cl += 16;
        while (cl < he && *cl >= '0' && *cl <= '9') {
          contentLen = contentLen * 10 + (u32)(*cl - '0');
          cl++;
        }
      }
    }
    if (contentLen == 0 || headerLen + contentLen > c->len)
      return;   // 还没收完

    // 这个 SSL 上有待处理的 ugc Range：响应体就是录像分块
    {
      CnPend *pd = pendFor(c->ssl);
      if (pd && pd->active) {
        char path[MAX_PATH];
        if (gRecId[0])
          snprintf(path, sizeof(path), "%s\\cn_chunk_%s_%010u.bin",
                   gCtx.recFolder, gRecId, pd->start);
        else
          snprintf(path, sizeof(path), "%s\\cn_chunk_%010u.bin",
                   gCtx.recFolder, pd->start);
        FILE *f = fopen(path, "wb");
        if (f) {
          fwrite(he, 1, contentLen, f);
          fclose(f);
        }
        bpLog("CN chunk start=%u len=%u -> %s", pd->start, contentLen, path);
        pd->active = 0;
      }
    }

    // 解码国服的 gzip / session-valid / gzip-session-valid 响应。
    // session2 从 login 的 gzip 响应提取，stage props 从 gzip-session-valid 解出。
    u8 *decoded = (u8 *)malloc(1024 * 1024);
    u32 decodedLen = 0;
    int didDecode = decoded && decodeCnResponse(c->buf, headerLen, he, contentLen,
                                                 decoded, 1024 * 1024, &decodedLen);
    if (didDecode) {
      captureSession2FromLogin(decoded, decodedLen);
      processResponse((const char *)decoded, decodedLen);
    } else {
      processResponse(he, contentLen);
    }
    if (decoded) free(decoded);

    // 带 Content-Range 的响应是 recording/ugc 的分块数据，直接落盘
    {
      const char *cr = memFind(c->buf, headerLen, "Content-Range:");
      if (cr) {
        char rng[80] = {0};
        const char *q = cr + 14;
        while (q < he && (*q == ' ')) q++;
        size_t j = 0;
        while (q + j < he && q[j] != '\r' && q[j] != '\n' && j + 1 < sizeof(rng))
          j++;
        memcpy(rng, q, j);
        u32 idx = (u32)InterlockedIncrement((LONG *)&gReqIdx) % 300;
        char path[MAX_PATH];
        snprintf(path, sizeof(path), "%s\\cn_chunk_%03u.bin", gCtx.recFolder, idx);
        FILE *f = fopen(path, "wb");
        if (f) {
          fwrite(he, 1, contentLen, f);
          fclose(f);
        }
        bpLog("CN chunk range=%s len=%u -> %s", rng, contentLen, path);
      }
    }

    u32 total = headerLen + contentLen;
    memmove(c->buf, c->buf + total, c->len - total);
    c->len -= total;
  }
}

// 临时诊断：把国服读到的原始字节落盘（上限 4MB），用于确认小屋 stage 数据
// 是否经过这个 hook、是否被 gzip 压缩。
static void rawDump(const char *buf, u32 len) {
  static FILE *f = NULL;
  static u32 wrote = 0;
  if (!f) {
    char dir[MAX_PATH], path[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\captures", gCtx.dataFolder);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\cn_raw.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    bpLog("CN raw dump -> %s", path);
  }
  if (wrote >= 4 * 1024 * 1024) {
    fclose(f);
    f = NULL;
    return;
  }
  fwrite(buf, 1, len, f);
  fflush(f);
  wrote += len;
}

// 出站请求原始字节（上限 2MB），用于找国服 stage 请求的路径和字段
static void rawDumpW(const char *buf, u32 len) {
  static FILE *f = NULL;
  static u32 wrote = 0;
  if (!f) {
    char dir[MAX_PATH], path[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\captures", gCtx.dataFolder);
    CreateDirectoryA(dir, NULL);
    snprintf(path, sizeof(path), "%s\\cn_raw_w.bin", dir);
    f = fopen(path, "wb");
    if (!f) return;
    bpLog("CN raw w dump -> %s", path);
  }
  if (wrote >= 2 * 1024 * 1024) {
    fclose(f);
    f = NULL;
    return;
  }
  fwrite(buf, 1, len, f);
  fflush(f);
  wrote += len;
}

static int det_read(u64 ssl, void *buf, int num) {
  int ret = g_trampRead ? g_trampRead(ssl, buf, num) : 0;
  if (ret > 0 && buf) {
    CnCtx *c = ctxFor(ssl);
    u32 take = (u32)ret;
    if (take > CN_BUF - c->len) {
      c->len = 0;   // 防御：超长丢弃
    }
    if (take <= CN_BUF - c->len) {
      memcpy(c->buf + c->len, buf, take);
      c->len += take;
      consume(c);
    }
  }
  return ret;
}

// 国服写请求（stage / social-feed create）原样落盘：请求头里带路径，
// 加密的 body 是同一 SSL 上紧接着的另一次 write，所以命中后要连抓 3 秒。
#define CN_DUMP_SLOTS 4
static u64 gDumpSsl[CN_DUMP_SLOTS];
static u32 gDumpUntil[CN_DUMP_SLOTS];
static u32 gDumpSeq = 0;

static void dumpBuffer(const char *buf, u32 len) {
  if (!gCtx.dataFolder[0]) return;
  char dir[MAX_PATH], path[MAX_PATH];
  snprintf(dir, sizeof(dir), "%s\\captures", gCtx.dataFolder);
  CreateDirectoryA(dir, NULL);
  snprintf(path, sizeof(path), "%s\\cn_w_%04u.bin", dir, gDumpSeq++ % 400);
  FILE *f = fopen(path, "wb");
  if (f) {
    fwrite(buf, 1, len, f);
    fclose(f);
    bpLog("CN write captured -> %s (%u bytes)", path, len);
  }
}

static void dumpWriteRequest(u64 ssl, const char *buf, u32 len) {
  int interesting =
      memFind(buf, len, "/service/stage/api/v1/") != NULL ||
      memFind(buf, len, "/service/social-feed/api/v1/create") != NULL ||
      memFind(buf, len, "/service/recording/api/") != NULL;
  if (interesting) {
    dumpBuffer(buf, len);
    for (int i = 0; i < CN_DUMP_SLOTS; i++) {
      if (gDumpSsl[i] == 0 || gDumpSsl[i] == ssl ||
          (int)(gDumpUntil[i] - GetTickCount()) < 0) {
        gDumpSsl[i] = ssl;
        gDumpUntil[i] = GetTickCount() + 3000;
        break;
      }
    }
    return;
  }
  // 命中之后的后续 write（加密 body）
  for (int i = 0; i < CN_DUMP_SLOTS; i++) {
    if (gDumpSsl[i] == ssl && (int)(gDumpUntil[i] - GetTickCount()) > 0)
      dumpBuffer(buf, len);
  }
}

// 解密出站 body（{"user":..,"body":base64(RC4(json))}），用于记录看过的留言
static void decryptOutbound(const char *buf, u32 len) {
  if (!gSession2Len || !buf || len < 32) return;
  if (!memFind(buf, len, "\"body\":\"")) return;
  cJSON *root = cJSON_ParseWithLength(buf, len);
  if (!root) return;
  cJSON *b = cJSON_GetObjectItemCaseSensitive(root, "body");
  if (cJSON_IsString(b) && b->valuestring) {
    u32 blen = (u32)strlen(b->valuestring);
    u8 *enc = (u8 *)malloc(blen ? blen : 1);
    u32 enclen = 0;
    if (enc && decodeBase64(b->valuestring, blen, enc, blen, &enclen) && enclen) {
      u8 *plain = (u8 *)malloc(enclen + 1);
      if (plain) {
        rc4Crypt(gSession2, gSession2Len, enc, enclen, plain);
        plain[enclen] = 0;
        if (memFind((const char *)plain, enclen, "\"social_feed_id\":\"")) {
          bpLog("CN view req decrypted (%u bytes)", enclen);
          // 留一份明文样本，便于对不上时离线查
          char dir[MAX_PATH], path[MAX_PATH];
          snprintf(dir, sizeof(dir), "%s\\captures", gCtx.dataFolder);
          CreateDirectoryA(dir, NULL);
          static u32 vseq = 0;
          snprintf(path, sizeof(path), "%s\\cn_view_%04u.bin", dir, vseq++ % 100);
          FILE *vf = fopen(path, "wb");
          if (vf) { fwrite(plain, 1, enclen, vf); fclose(vf); }
        }
        bpCaptureOutboundJson((const char *)plain, enclen);
        free(plain);
      }
    }
    if (enc) free(enc);
  }
  cJSON_Delete(root);
}

static int det_write(u64 ssl, void *buf, int num) {
  if (buf && num > 0) {
    u32 take = (u32)num;
    if (take > 65536) take = 65536;
    dumpWriteRequest(ssl, (const char *)buf, take);
    decryptOutbound((const char *)buf, take);
    // 明文请求（部分接口不加密）
    if (memFind((const char *)buf, take, "\"social_feed_id\":\""))
      bpCaptureOutboundJson((const char *)buf, take);
    captureRequest((const char *)buf, take, ssl);
  }
  return g_trampWrite ? g_trampWrite(ssl, buf, num) : 0;
}

static int det_w2(u64 ssl, void *buf, int num) {
  if (buf && num > 0) {
    u32 take = (u32)num;
    if (take > 65536) take = 65536;
    captureRequest((const char *)buf, take, ssl);
  }
  return g_trampW2 ? g_trampW2(ssl, buf, num) : 0;
}

static int det_w3(u64 ssl, void *buf, int num) {
  if (buf && num > 0) {
    u32 take = (u32)num;
    if (take > 65536) take = 65536;
    captureRequest((const char *)buf, take, ssl);
  }
  return g_trampW3 ? g_trampW3(ssl, buf, num) : 0;
}

void bpCnLiveInit(void) {
  bpCaptureLocksInit();

  HMODULE h = GetModuleHandleA(NULL);
  if (!h) return;

  u8 *r = (u8 *)h + CN_READ_RVA;
  u8 *w = (u8 *)h + CN_WRITE_RVA;
  static const u8 expect[] = { 0xB8, 0x38, 0x00, 0x00, 0x00, 0xE8 };

  bpLog("CN live: read=%p bytes %02X %02X %02X %02X %02X %02X",
        r, r[0], r[1], r[2], r[3], r[4], r[5]);
  bpLog("CN live: write=%p bytes %02X %02X %02X %02X %02X %02X",
        w, w[0], w[1], w[2], w[3], w[4], w[5]);

  if (memcmp(r, expect, sizeof(expect)) != 0 ||
      memcmp(w, expect, sizeof(expect)) != 0) {
    bpLog("CN live: prologue MISMATCH -> hooks skipped");
    return;
  }

  g_trampRead = (PFN_Io)bpManualHookInstallSlot(r, (void *)det_read);
  g_trampWrite = (PFN_Io)bpManualHookInstallSlot(w, (void *)det_write);

  // 保存版只挂读/写两个；额外 body hook 已移除
  bpLog("CN live hooks: read=%p write=%p", g_trampRead, g_trampWrite);
}
