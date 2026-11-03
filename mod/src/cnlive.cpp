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
#include "bp.h"

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
    "\"pool_type\":\"%s\",\"host\":\"%s\",\"updated\":%lld}\n",
    gUser, gSess, gLevel, gPool, gHost, (long long)GetTickCount64());
  fclose(f);
  strncpy(gLastSession, sig, sizeof(gLastSession) - 1);
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

static void processResponse(const char *body, u32 len) {
  if (len < 16) return;

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

    processResponse(he, contentLen);

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

static int det_write(u64 ssl, void *buf, int num) {
  if (buf && num > 0) {
    u32 take = (u32)num;
    if (take > 65536) take = 65536;
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
