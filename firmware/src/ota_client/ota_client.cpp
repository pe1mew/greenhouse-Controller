/**
 * @file ota_client.cpp
 * @brief ROTA client request layer — implementation (rota_tds.md §4).
 *
 * @author Greenhouse Controller project
 * @since 2.2.0 (ROTA)
 */

#include "ota_client.h"
#include "ota_cert_default.h"
#include "../system_id/system_id.h"
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../network_manager/network_manager.h"
#include "../event_logger/event_logger.h"

#include <string.h>
#include <strings.h>   /* strcasecmp for SHA-256 hex compare */
#include <stdlib.h>
#include <time.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <freertos/event_groups.h>
#include <esp_log.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <mbedtls/md.h>
#include <mbedtls/sha256.h>

#include "nvs_config.h"
#include "../ota_manager/ota_manager.h"        /* 3.8 apply — reuse the T13 push-OTA machinery */
#include "../relay_controller/relay_controller.h" /* 3.8 quiet gate — t2_get_window_states */
#include "../web_server/web_server.h"           /* 3.8 quiet gate — web_any_active_session */
#include "../ui_display/ui_display.h"           /* 3.8 quiet gate — ui_pin_session_active */

static const char *TAG = "T16_OTA";

/** Manifest JSON is small; cap the response body generously. */
#define ROTA_MANIFEST_MAX 4096u
/** Boot settle before the first check — let WiFi + SNTP come up (R-C03). */
#define ROTA_BOOT_SETTLE_MS 30000u
/** Backoff ceiling on repeated failure (R-C09). */
#define ROTA_BACKOFF_MAX_S (24u * 3600u)
/** Artefact-size ceilings (R-C04): firmware < app partition, assets = LittleFS. */
#define ROTA_FW_MAX     0x1E0000u   /* 1 966 080 B — leaves margin under the 2 MB app bank */
#define ROTA_ASSETS_MAX 0x100000u   /* 1 048 576 B — the LittleFS partition size */

/** NVS key (system ns) for the operator-uploaded pinned cert. ≤ 15 chars. */
static const char K_OTA_CERT[] = "ota_cert";
/** NVS key (system ns) for the accepted-release high-water `seq` (R-V02). */
static const char K_FW_HIWATER[] = "fw_hiwater";

/** Wall-clock sanity floor — 2020-01-01 UTC. Below this, SNTP has not run. */
#define ROTA_EPOCH_FLOOR 1577836800LL

/* Lowercase-hex encode @p n bytes into @p out (needs 2*n + 1). */
static void to_hex(const uint8_t *in, size_t n, char *out)
{
    static const char H[] = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        out[2 * i]     = H[(in[i] >> 4) & 0x0F];
        out[2 * i + 1] = H[in[i] & 0x0F];
    }
    out[2 * n] = '\0';
}

bool rota_build_auth_header(const char *secret, const char *request_uri,
                            char *out, size_t out_len)
{
    if (secret == NULL || secret[0] == '\0' ||
        request_uri == NULL || out == NULL || out_len < ROTA_AUTH_HDR_LEN) {
        return false;
    }

    /* Wall time must be valid — the server rejects skew > ±300 s, and a
     * pre-SNTP clock (1970 / DS1307 pre-seed) would fail every time. */
    time_t now = time(NULL);
    if ((long long)now < ROTA_EPOCH_FLOOR) {
        ESP_LOGW(TAG, "auth header: wall time not valid yet (%lld) — need SNTP",
                 (long long)now);
        return false;
    }

    char id[13];
    system_mac_str(id, sizeof(id));          /* full MAC, 12 lowercase hex */

    uint8_t nb[8];
    esp_fill_random(nb, sizeof(nb));
    char nonce[17];
    to_hex(nb, sizeof(nb), nonce);           /* 16 hex */

    char ts[24];   /* int64 is ≤ 20 digits; sized to satisfy -Wformat-truncation */
    (void)snprintf(ts, sizeof(ts), "%lld", (long long)now);

    /* msg = id "|" ts "|" nonce "|" request_uri  (rota_tds.md §4.2) */
    char msg[256];
    int n = snprintf(msg, sizeof(msg), "%s|%s|%s|%s", id, ts, nonce, request_uri);
    if (n < 0 || (size_t)n >= sizeof(msg)) {
        ESP_LOGW(TAG, "auth header: request_uri too long");
        return false;
    }

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA256);
    if (info == NULL) {
        return false;
    }
    unsigned char h[32];
    if (mbedtls_md_hmac(info, (const unsigned char *)secret, strlen(secret),
                        (const unsigned char *)msg, (size_t)n, h) != 0) {
        return false;
    }
    char machex[65];
    to_hex(h, sizeof(h), machex);            /* 64 hex */

    int w = snprintf(out, out_len, "%s:%s:%s:%s", id, ts, nonce, machex);
    return (w > 0 && (size_t)w < out_len);
}

/* ── HTTPS GET ─────────────────────────────────────────────────────────── */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
    bool   overflow;
} body_acc_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    if (e->event_id == HTTP_EVENT_ON_DATA && e->user_data != NULL) {
        body_acc_t *a = (body_acc_t *)e->user_data;
        if (a->overflow) {
            return ESP_OK;                    /* keep draining, discard */
        }
        if (a->len + (size_t)e->data_len > a->cap) {
            a->overflow = true;
            return ESP_OK;
        }
        memcpy(a->buf + a->len, e->data, (size_t)e->data_len);
        a->len += (size_t)e->data_len;
    }
    return ESP_OK;
}

esp_err_t rota_https_get(const char *url, const char *request_uri,
                         const char *cert_pem, const char *secret,
                         size_t max_body,
                         int *out_status, char **out_body, size_t *out_len)
{
    if (out_status) *out_status = 0;
    if (out_body)   *out_body   = NULL;
    if (out_len)    *out_len    = 0;
    if (url == NULL || request_uri == NULL || cert_pem == NULL ||
        secret == NULL || out_status == NULL || out_body == NULL || out_len == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    char auth[ROTA_AUTH_HDR_LEN];
    if (!rota_build_auth_header(secret, request_uri, auth, sizeof(auth))) {
        return ESP_ERR_INVALID_STATE;         /* no valid clock / bad args */
    }

    /* PSRAM-preferred body buffer (the assets download can be ~1.4 MB). */
    char *buf = (char *)heap_caps_malloc(max_body + 1u, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = (char *)malloc(max_body + 1u);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }
    body_acc_t acc = { buf, 0u, max_body, false };

    esp_http_client_config_t cfg = {};
    cfg.url                = url;
    cfg.transport_type     = HTTP_TRANSPORT_OVER_SSL;
    cfg.cert_pem           = cert_pem;        /* PINNED cert — not the CA bundle (R-A02) */
    cfg.crt_bundle_attach  = NULL;
    cfg.timeout_ms         = 15000;
    cfg.event_handler      = http_evt;
    cfg.user_data          = &acc;
    cfg.skip_cert_common_name_check = false;

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c == NULL) {
        free(buf);
        return ESP_FAIL;
    }
    esp_http_client_set_header(c, "X-OTA-Auth", auth);

    esp_err_t err = esp_http_client_perform(c);
    if (err == ESP_OK) {
        *out_status = esp_http_client_get_status_code(c);
        if (acc.overflow) {
            ESP_LOGW(TAG, "GET %s: body exceeded cap %u", request_uri,
                     (unsigned)max_body);
            err = ESP_ERR_INVALID_SIZE;
        } else {
            buf[acc.len] = '\0';
            *out_body = buf;
            *out_len  = acc.len;
            buf = NULL;                        /* ownership transferred */
        }
    } else {
        ESP_LOGW(TAG, "GET %s failed: %s", request_uri, esp_err_to_name(err));
    }

    esp_http_client_cleanup(c);
    if (buf != NULL) {
        free(buf);
    }
    return err;
}

/* ── Pinned-cert storage (R-A03/A04) ───────────────────────────────────── */

/** A stored value looks like a cert only if it begins a PEM block. */
static bool looks_like_pem(const char *s)
{
    return s != NULL && strncmp(s, "-----BEGIN", 10) == 0;
}

int rota_cert_get(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0u) {
        return -1;
    }
    /* Prefer the operator-uploaded cert; fall back to the embedded default. */
    buf[0] = '\0';
    (void)nvs_cfg_get_str(NVS_NS_SYSTEM, K_OTA_CERT, buf, cap);
    if (!looks_like_pem(buf)) {
        size_t n = strlen(OTA_DEFAULT_CERT_PEM);
        if (n + 1u > cap) {
            return -1;
        }
        memcpy(buf, OTA_DEFAULT_CERT_PEM, n + 1u);
        return (int)n;
    }
    return (int)strlen(buf);
}

esp_err_t rota_cert_set(const char *pem)
{
    if (pem == NULL || pem[0] == '\0') {
        return rota_cert_clear();
    }
    if (strlen(pem) + 1u > ROTA_CERT_MAX || !looks_like_pem(pem)) {
        return ESP_ERR_INVALID_ARG;
    }
    return (nvs_cfg_set_str(NVS_NS_SYSTEM, K_OTA_CERT, pem) == NVS_CFG_OK)
               ? ESP_OK : ESP_FAIL;
}

esp_err_t rota_cert_clear(void)
{
    /* Store an empty string → rota_cert_get() falls back to the default. */
    return (nvs_cfg_set_str(NVS_NS_SYSTEM, K_OTA_CERT, "") == NVS_CFG_OK)
               ? ESP_OK : ESP_FAIL;
}

bool rota_cert_is_custom(void)
{
    char probe[16] = {0};
    (void)nvs_cfg_get_str(NVS_NS_SYSTEM, K_OTA_CERT, probe, sizeof(probe));
    return looks_like_pem(probe);   /* only the "-----BEGIN" prefix is needed */
}

/* ── T16 task — periodic manifest check (rota_tds.md §2.4) ─────────────── */

/* T16 last-check status, for /api/ota/check (task 3.9). Single writer (T16). */
static rota_status_t s_status = { 0, -1, 0, 0u, {0}, -1, -1 };

/* True while a verified update is downloaded but its apply is deferred (waiting
 * for the night window / quiet gate). Drives the status-shield "update pending"
 * badge. Single writer (T16); a stale read is harmless for a UI hint. */
static bool s_update_pending = false;

void rota_status_get(rota_status_t *out)
{
    if (out != NULL) *out = s_status;   /* small struct copy; benign torn read */
}

bool rota_update_pending(void)
{
    return s_update_pending;
}

/** Post a LOG_SYSTEM audit row (rota_tds.md §4.4). */
static void audit_row(int16_t a, int16_t b)
{
    log_event_t e = {};
    e.timestamp  = (uint32_t)time(NULL);
    e.event_type = (uint8_t)LOG_SYSTEM;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.value_a    = a;
    e.value_b    = b;
    log_post(&e);
}
/** Check outcome (22): 0 no-update · 1 update · 2 unreachable · 3 skip · 4 auth-fail. */
static void audit_check(int16_t sub)
{
    audit_row(22, sub);
    s_status.last_result      = sub;
    s_status.last_check_epoch = (int64_t)time(NULL);
    if (sub == 0) s_update_pending = false;   /* no newer release → nothing pending */
}
/** Download/verify outcome (23): 0 ok · 1 TLS/pin · 2 SHA/size · 3 downgrade · 4 min_version. */
static void audit_dl(int16_t sub)    { audit_row(23, sub); s_status.last_dl = sub; }
/** Apply outcome (24): 0 committed · 1 deferred · 2 failed. */
static void audit_apply(int16_t sub) { audit_row(24, sub); s_status.last_apply = sub; }

/** Dotted-numeric SemVer compare: >0 if a newer than b, 0 equal, <0 older. */
static int semver_cmp(const char *a, const char *b)
{
    while (*a || *b) {
        long na = strtol(a, (char **)&a, 10);
        long nb = strtol(b, (char **)&b, 10);
        if (na != nb) return (na > nb) ? 1 : -1;
        if (*a == '.') a++;
        if (*b == '.') b++;
        if (*a == '-' || *b == '-') break;   /* ignore pre-release tail for now */
    }
    return 0;
}

/* ── Manifest parsing (ad-hoc, house style — no cJSON dep) ─────────────── */

/** Extract a JSON string field `"key":"value"` from a flat manifest body. */
static bool json_str_field(const char *json, const char *key, char *out, size_t cap)
{
    char pat[48];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn < 0 || (size_t)pn >= sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (p == NULL) return false;
    p = strchr(p + pn, ':'); if (p == NULL) return false;
    p = strchr(p, '"');      if (p == NULL) return false;   /* opening quote of value */
    p++;
    const char *e = strchr(p, '"'); if (e == NULL) return false;
    size_t n = (size_t)(e - p);
    if (n == 0 || n + 1u > cap) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}

/** Extract a JSON numeric field `"key": <int>` from a flat manifest body. */
static bool json_num_field(const char *json, const char *key, long *out)
{
    char pat[48];
    int pn = snprintf(pat, sizeof(pat), "\"%s\"", key);
    if (pn < 0 || (size_t)pn >= sizeof(pat)) return false;
    const char *p = strstr(json, pat);
    if (p == NULL) return false;
    p = strchr(p + pn, ':'); if (p == NULL) return false;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *end = NULL;
    long v = strtol(p, &end, 10);
    if (end == p) return false;
    *out = v;
    return true;
}

/** Resolved manifest (rota_tds.md §4.3). */
typedef struct {
    char    version[24];
    int32_t seq;
    char    min_version[24];   /* "" if absent */
    char    fw_file[80];
    char    fw_sha256[65];
    long    fw_size;
    char    assets_file[80];
    char    assets_sha256[65];
    long    assets_size;
} rota_manifest_t;

static bool parse_manifest(const char *body, rota_manifest_t *m)
{
    memset(m, 0, sizeof(*m));
    long seq = 0, fsz = 0, asz = 0;
    bool ok =
        json_str_field(body, "version",       m->version,       sizeof(m->version)) &&
        json_num_field(body, "seq",           &seq) &&
        json_str_field(body, "fw_file",       m->fw_file,       sizeof(m->fw_file)) &&
        json_str_field(body, "fw_sha256",     m->fw_sha256,     sizeof(m->fw_sha256)) &&
        json_num_field(body, "fw_size",       &fsz) &&
        json_str_field(body, "assets_file",   m->assets_file,   sizeof(m->assets_file)) &&
        json_str_field(body, "assets_sha256", m->assets_sha256, sizeof(m->assets_sha256)) &&
        json_num_field(body, "assets_size",   &asz);
    (void)json_str_field(body, "min_version", m->min_version, sizeof(m->min_version)); /* optional */
    m->seq = (int32_t)seq; m->fw_size = fsz; m->assets_size = asz;
    return ok;
}

/** SHA-256 of @p len bytes → 64 lowercase hex + NUL into @p out (≥65). */
static bool sha256_hex(const uint8_t *data, size_t len, char *out)
{
    uint8_t dg[32];
    if (mbedtls_sha256(data, len, dg, 0) != 0) return false;   /* 0 = SHA-256 */
    to_hex(dg, 32, out);
    return true;
}

/* ── 3.7 Download + verify (R-C04/C05/R-R06) ──────────────────────────── */

/**
 * @brief GET one artefact via download.php, verify size + SHA-256 (R-C05).
 *
 * @return ESP_OK with *out_buf (heap, caller frees) on a fully-verified match;
 *   an esp_err_t otherwise (nothing kept): ESP_ERR_INVALID_STATE = server 204
 *   (auth/pin), ESP_ERR_INVALID_SIZE / ESP_ERR_INVALID_CRC = size / hash miss.
 */
static esp_err_t rota_download_verify(const cfg_shadow_t *cfg, const char *cert,
                                      const char *file_kw, const char *version,
                                      const char *expect_sha, long expect_size,
                                      uint8_t **out_buf, size_t *out_len)
{
    *out_buf = NULL; *out_len = 0;

    char req[112];
    (void)snprintf(req, sizeof(req), "/download.php?file=%s&v=%s", file_kw, version);
    const char *slash = "";
    size_t ul = strlen(cfg->ota_url);
    if (ul > 0 && cfg->ota_url[ul - 1] != '/') slash = "/";
    char url[288];
    int un = snprintf(url, sizeof(url), "%s%sdownload.php?file=%s&v=%s",
                      cfg->ota_url, slash, file_kw, version);
    if (un < 0 || (size_t)un >= sizeof(url)) return ESP_ERR_INVALID_ARG;

    int status = 0; char *body = NULL; size_t blen = 0;
    xSemaphoreTake(MX_TLS, portMAX_DELAY);           /* serialise TLS with T14 (R-C07) */
    esp_err_t err = rota_https_get(url, req, cert, cfg->ota_secret,
                                   (size_t)expect_size + 64u, &status, &body, &blen);
    xSemaphoreGive(MX_TLS);

    if (err != ESP_OK || status != 200 || body == NULL) {
        if (body != NULL) free(body);
        ESP_LOGW(TAG, "download %s: err=%s status=%d", file_kw, esp_err_to_name(err), status);
        if (err == ESP_ERR_INVALID_SIZE) return ESP_ERR_INVALID_SIZE;  /* body exceeded cap */
        return (status == 204) ? ESP_ERR_INVALID_STATE : ESP_FAIL;
    }
    if ((long)blen != expect_size) {
        ESP_LOGW(TAG, "download %s: size %u != manifest %ld",
                 file_kw, (unsigned)blen, expect_size);
        free(body);
        return ESP_ERR_INVALID_SIZE;
    }
    char got[65];
    if (!sha256_hex((const uint8_t *)body, blen, got) ||
        strcasecmp(got, expect_sha) != 0) {
        ESP_LOGW(TAG, "download %s: SHA-256 mismatch", file_kw);
        free(body);
        return ESP_ERR_INVALID_CRC;
    }
    ESP_LOGI(TAG, "download %s: %u B, SHA-256 OK", file_kw, (unsigned)blen);
    *out_buf = (uint8_t *)body; *out_len = blen;
    return ESP_OK;
}

/** Map a rota_download_verify() failure to its audit-23 sub-code. */
static int16_t dl_err_to_sub(esp_err_t e)
{
    return (e == ESP_ERR_INVALID_SIZE || e == ESP_ERR_INVALID_CRC) ? 2   /* SHA/size */
                                                                    : 1;  /* TLS/pin/transport */
}

/* ── 3.8 Apply — night-window ∩ quiet-gate, then feed T13 (R-P01..P06) ── */

/* Scheduler hint: when >0, T16 sleeps this many seconds next cycle instead of
 * the normal interval. Set when an update was downloaded+verified but its apply
 * was deferred (outside the window / quiet gate, R-P04) so the next wake lands
 * in the window rather than one full check-interval later. Single writer/reader
 * (T16), so no lock needed. */
static uint32_t s_apply_wait_s = 0u;

/** Local hour [0,23] from the wall clock (TZ set by T4, R-P01 "local time"). */
static int local_hour_now(void)
{
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    return tmv.tm_hour;
}

/** R-P01 night-window test. lo==hi disables the window (apply any hour). */
static bool in_night_window(int32_t lo, int32_t hi)
{
    if (lo == hi) return true;               /* window disabled = unrestricted */
    int h = local_hour_now();
    if (lo < hi) return (h >= lo && h < hi);
    return (h >= lo || h < hi);              /* window wraps past midnight */
}

/** Seconds until the next apply opportunity: a short retry if already in the
 *  window (quiet-gate may clear), else the time to the next local `lo`:00 with
 *  a spread so a fleet does not re-download in lockstep (R-C02). 0 if disabled. */
static uint32_t secs_to_window(int32_t lo, int32_t hi)
{
    if (lo == hi) return 0u;                    /* window disabled → no wait */
    if (in_night_window(lo, hi)) return 300u;   /* in window; retry quiet gate soon */
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    int now_s = tmv.tm_hour * 3600 + tmv.tm_min * 60 + tmv.tm_sec;
    int delta = (int)lo * 3600 - now_s;
    if (delta <= 0) delta += 24 * 3600;
    return (uint32_t)delta + (esp_random() % 600u);   /* +0..10 min spread */
}

/** R-P02 quiet gate: true only when it is safe to flash + reboot. */
static bool quiet_gate(void)
{
    window_state_t w[3];
    t2_get_window_states(w);
    for (int i = 0; i < 3; i++) {
        if (w[i] == WIN_MOVING_OPEN || w[i] == WIN_MOVING_CLOSE) return false;
    }
    EventBits_t b = xEventGroupGetBits(EG1);
    if (b & (EG1_BIT_WIND_OVERRIDE | EG1_BIT_MOTOR_ALARM | EG1_BIT_CALIBRATING)) return false;
    if (web_any_active_session()) return false;
    if (ui_pin_session_active())  return false;
    return true;
}

/**
 * @brief Apply a downloaded+verified update under the night/quiet policy.
 *
 * Takes ownership of @p fw and @p assets (frees both on every path). On a
 * committed apply the unit reboots via T13 shortly after return.
 */
static void rota_apply(const rota_manifest_t *m,
                       uint8_t *fw, size_t fw_len,
                       uint8_t *assets, size_t as_len)
{
    cfg_shadow_t cfg; dm_cfg_snapshot(&cfg);

    /* R-P01/P02/P05: apply/commit/reboot only inside the night window with a
     * clear quiet gate (download+verify already happened, any hour). */
    if (!in_night_window(cfg.ota_win_lo, cfg.ota_win_hi) || !quiet_gate()) {
        ESP_LOGI(TAG, "apply deferred: window/quiet gate not met (retry near window)");
        audit_apply(1);                 /* R-P04: nothing committed, retry later */
        s_update_pending = true;        /* verified update waiting → shield badge */
        s_apply_wait_s = secs_to_window(cfg.ota_win_lo, cfg.ota_win_hi);
        free(fw); free(assets);
        return;
    }

    /* Stream the verified firmware into the inactive bank. No commit yet —
     * ota_firmware_end() is deferred until the final gate re-check passes. */
    bool ok = ota_firmware_begin(fw_len);
    for (size_t off = 0; ok && off < fw_len; off += 8192u) {
        size_t n = (fw_len - off < 8192u) ? (fw_len - off) : 8192u;
        ok = ota_firmware_write(fw + off, n);
    }
    free(fw); fw = NULL;                 /* flashed (or aborted) — buffer done */
    if (!ok) {
        ESP_LOGE(TAG, "apply: firmware write failed: %s", ota_get_error());
        audit_apply(2);
        free(assets);
        return;                         /* esp_ota auto-aborted; old bank boots */
    }

    /* R-P03: re-check the quiet gate immediately (< 5 s) before committing.
     * If activity resumed, abort cleanly BEFORE ota_firmware_end() so no
     * FW_DONE fallback timer can strand a firmware-only commit. */
    if (!quiet_gate()) {
        ESP_LOGW(TAG, "apply aborted at final gate — activity resumed");
        ota_firmware_abort();
        audit_apply(1);
        s_update_pending = true;        /* verified update waiting → shield badge */
        s_apply_wait_s = 300u;          /* still in window; retry the quiet gate soon */
        free(assets);
        return;
    }

    /* Commit point: finalise firmware then feed assets back-to-back so the
     * 120 s FW_DONE fallback never fires (ota_assets_begin cancels it). T13's
     * ota_assets_end() switches the boot bank and reboots (R-P06). */
    ok = ota_firmware_end();
    if (ok) ok = ota_assets_begin(as_len);
    if (ok) ok = ota_assets_accumulate(assets, as_len, 0);
    if (ok) ok = ota_assets_end();      /* spawns T13 → extract → bank switch → reboot */
    free(assets); assets = NULL;
    if (!ok) {
        ESP_LOGE(TAG, "apply: commit/assets stage failed: %s", ota_get_error());
        audit_apply(2);
        return;
    }

    /* R-V02: persist the accepted seq high-water mark before the imminent
     * reboot so a replay of this manifest after reboot is rejected. */
    (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, K_FW_HIWATER, m->seq);
    ESP_LOGW(TAG, "ROTA apply committed: %s (seq %ld) — rebooting via T13",
             m->version, (long)m->seq);
    s_update_pending = false;           /* applied — reboot into it imminently */
    audit_apply(0);
    /* T13 reboots shortly (schedule_reboot). */
}

/* ── 3.6 Decision + orchestration (R-V01/V02/V03) ─────────────────────── */

/** A newer manifest was offered — gate it, download+verify both, then apply. */
static void rota_handle_update(const rota_manifest_t *m)
{
    /* R-V01/V02: seq must beat the persisted high-water mark (anti-downgrade). */
    int32_t hiwater = 0;
    (void)nvs_cfg_get_i32(NVS_NS_SYSTEM, K_FW_HIWATER, &hiwater);
    if (m->seq <= hiwater) {
        ESP_LOGW(TAG, "reject: seq %ld <= hiwater %ld (downgrade/replay)",
                 (long)m->seq, (long)hiwater);
        s_update_pending = false;       /* not an applicable update */
        audit_dl(3);
        return;
    }
    /* R-V03: refuse a jump the running firmware is too old to take. */
    if (m->min_version[0] != '\0' &&
        semver_cmp(FIRMWARE_VERSION, m->min_version) < 0) {
        ESP_LOGW(TAG, "reject: running %s < min_version %s (manual step needed)",
                 FIRMWARE_VERSION, m->min_version);
        s_update_pending = false;       /* won't apply without manual intervention */
        audit_dl(4);
        return;
    }
    /* R-C04: sanity-check sizes before allocating PSRAM. */
    if (m->fw_size <= 0 || (size_t)m->fw_size > ROTA_FW_MAX ||
        m->assets_size <= 0 || (size_t)m->assets_size > ROTA_ASSETS_MAX) {
        ESP_LOGW(TAG, "reject: implausible size fw=%ld assets=%ld",
                 m->fw_size, m->assets_size);
        audit_dl(2);
        return;
    }

    cfg_shadow_t cfg; dm_cfg_snapshot(&cfg);
    /* Heap, not stack: the 2 KB PEM must not sit on T16's stack while a nested
     * mbedTLS handshake runs (stack-overflow crash loop, fixed 2.2.1). */
    char *cert = (char *)malloc(ROTA_CERT_MAX);
    if (cert == NULL || rota_cert_get(cert, ROTA_CERT_MAX) < 0) {
        free(cert); audit_dl(1); return;
    }

    /* R-C05/R-R06: download AND verify BOTH artefacts before any flash write. */
    uint8_t *fw = NULL, *assets = NULL; size_t fw_len = 0, as_len = 0;
    esp_err_t e = rota_download_verify(&cfg, cert, "fw", m->version,
                                       m->fw_sha256, m->fw_size, &fw, &fw_len);
    if (e == ESP_OK) {
        e = rota_download_verify(&cfg, cert, "assets", m->version,
                                 m->assets_sha256, m->assets_size, &assets, &as_len);
    }
    free(cert);
    if (e != ESP_OK) { free(fw); audit_dl(dl_err_to_sub(e)); return; }

    ESP_LOGI(TAG, "both artefacts verified (fw %u B, assets %u B) — ready to apply",
             (unsigned)fw_len, (unsigned)as_len);
    /* TC-09 heap-budget gate: the TLS handshakes are the internal-RAM low-water.
     * min-since-boot is also on /api/status (heap_min_kb) for the deferred path;
     * logged here too for the committed path (reboot resets the min). */
    ESP_LOGI(TAG, "heap after download/verify: free=%u B, min-since-boot=%u B (internal)",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    audit_dl(0);

    rota_apply(m, fw, fw_len, assets, as_len);   /* takes ownership; frees both */
}

/**
 * @brief Run one check cycle.
 * @return true if the check reached the server (whatever the result); false on
 *         a transport/precondition failure that should trigger backoff.
 */
static bool ota_check_once(void)
{
    cfg_shadow_t cfg;
    dm_cfg_snapshot(&cfg);

    /* Preconditions (R-C03). Disabled/unconfigured is a silent skip (not
     * audited every idle cycle); a blocked-but-enabled check is audited. */
    if (cfg.ota_enable == 0 || cfg.ota_url[0] == '\0' || cfg.ota_secret[0] == '\0') {
        s_update_pending = false;   /* ROTA off/unconfigured → nothing pending */
        return true;   /* nothing to do; normal-interval sleep */
    }
    if (!nm_is_sntp_synced()) {
        ESP_LOGI(TAG, "check skipped: SNTP not synced");
        audit_check(3);
        return false;  /* retry sooner */
    }
    if (xEventGroupGetBits(EG1) & EG1_BIT_OTA_IN_PROGRESS) {
        ESP_LOGI(TAG, "check skipped: OTA already in progress");
        audit_check(3);
        return true;
    }

    /* Build url + request_uri. ota_url is the base (e.g. https://host/); the
     * signed request_uri is the path+query the server sees. */
    char req[160];
    (void)snprintf(req, sizeof(req), "/manifest.php?fw=%s", FIRMWARE_VERSION);
    const char *slash = "";
    size_t ul = strlen(cfg.ota_url);
    if (ul > 0 && cfg.ota_url[ul - 1] != '/') slash = "/";
    char url[256];
    int un = snprintf(url, sizeof(url), "%s%smanifest.php?fw=%s",
                      cfg.ota_url, slash, FIRMWARE_VERSION);
    if (un < 0 || (size_t)un >= sizeof(url)) {
        audit_check(2);
        return false;
    }

    char *cert = (char *)malloc(ROTA_CERT_MAX);   /* heap, not the task stack */
    if (cert == NULL || rota_cert_get(cert, ROTA_CERT_MAX) < 0) {
        free(cert);
        audit_check(2);
        return false;
    }

    int status = 0; char *body = NULL; size_t blen = 0;
    /* Serialise the TLS handshake with T14 (R-C07, gh#23 heap budget). */
    xSemaphoreTake(MX_TLS, portMAX_DELAY);
    esp_err_t err = rota_https_get(url, req, cert, cfg.ota_secret,
                                   ROTA_MANIFEST_MAX, &status, &body, &blen);
    xSemaphoreGive(MX_TLS);
    free(cert);                          /* only needed for the handshake above */

    s_status.last_http = status;        /* 0 = transport failure */
    s_status.checks_total++;
    s_status.offered_ver[0] = '\0';     /* set below only on a parseable 200 */

    bool reached = true;
    if (err != ESP_OK && status == 0) {
        ESP_LOGW(TAG, "manifest GET transport failure: %s", esp_err_to_name(err));
        audit_check(2);
        reached = false;
    } else if (status == 204) {
        ESP_LOGW(TAG, "manifest GET -> 204 (auth rejected by server)");
        audit_check(4);
    } else if (status == 404) {
        ESP_LOGI(TAG, "manifest GET -> 404 (no release offered)");
        audit_check(0);
    } else if (status != 200 || body == NULL) {
        ESP_LOGW(TAG, "manifest GET -> HTTP %d (unexpected)", status);
        audit_check(2);
        reached = false;
    } else {
        rota_manifest_t m;
        if (!parse_manifest(body, &m)) {
            ESP_LOGW(TAG, "manifest 200 but unparseable / missing fields");
            audit_check(2);
        } else {
            snprintf(s_status.offered_ver, sizeof(s_status.offered_ver), "%s", m.version);
            if (semver_cmp(m.version, FIRMWARE_VERSION) > 0) {
                ESP_LOGI(TAG, "update found: %s > running %s (seq %ld)",
                         m.version, FIRMWARE_VERSION, (long)m.seq);
                audit_check(1);
                /* 3.6 → 3.7 → 3.8: gate, download+verify, apply. */
                rota_handle_update(&m);
            } else {
                ESP_LOGI(TAG, "up to date: offered %s, running %s", m.version, FIRMWARE_VERSION);
                audit_check(0);
            }
        }
    }
    if (body != NULL) free(body);
    return reached;
}

void task_ota_client(void *pvParameters)
{
    (void)pvParameters;
    vTaskDelay(pdMS_TO_TICKS(ROTA_BOOT_SETTLE_MS));   /* let WiFi + SNTP settle */

    uint32_t backoff_s = 0u;
    for (;;) {
        bool reached = ota_check_once();

        uint32_t sleep_s;
        if (s_apply_wait_s > 0u) {
            /* An update is verified but its apply was deferred — wake near the
             * next window instead of a full interval later (R-P04). */
            sleep_s = s_apply_wait_s;
            s_apply_wait_s = 0u;
            backoff_s = 0u;
        } else if (!reached) {
            /* Exponential backoff: 1h, 2h, 4h … capped (R-C09). */
            backoff_s = (backoff_s == 0u) ? 3600u
                        : (backoff_s * 2u > ROTA_BACKOFF_MAX_S ? ROTA_BACKOFF_MAX_S
                                                               : backoff_s * 2u);
            sleep_s = backoff_s;
        } else {
            backoff_s = 0u;
            cfg_shadow_t cfg;
            dm_cfg_snapshot(&cfg);
            int32_t h = cfg.ota_check_h;
            if (h < 1) h = 24;
            uint32_t base_s = (uint32_t)h * 3600u;
            /* ±10 % jitter so a fleet never checks in lockstep (R-C02). */
            uint32_t span = base_s / 5u;                 /* 20 % of base */
            uint32_t jit  = (span > 0u) ? (esp_random() % span) : 0u;
            sleep_s = base_s - base_s / 10u + jit;       /* base −10 % + [0,20 %) */
        }
        /* Interruptible sleep: a config change (or any producer) can wake T16
         * early via xTaskNotifyGive(task_t16) to re-check now (R-F04). */
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS((TickType_t)sleep_s * 1000u));
    }
}
