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

#include "nvs_config.h"

static const char *TAG = "T16_OTA";

/** Manifest JSON is small; cap the response body generously. */
#define ROTA_MANIFEST_MAX 4096u
/** Boot settle before the first check — let WiFi + SNTP come up (R-C03). */
#define ROTA_BOOT_SETTLE_MS 30000u
/** Backoff ceiling on repeated failure (R-C09). */
#define ROTA_BACKOFF_MAX_S (24u * 3600u)

/** NVS key (system ns) for the operator-uploaded pinned cert. ≤ 15 chars. */
static const char K_OTA_CERT[] = "ota_cert";

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

/* T16 last-check status, for /api/ota/status (task 3.9). Single writer (T16). */
static rota_status_t s_status = { 0, -1, 0, 0u, {0} };

void rota_status_get(rota_status_t *out)
{
    if (out != NULL) *out = s_status;   /* small struct copy; benign torn read */
}

/** Check-outcome audit row: LOG_SYSTEM value_a=22 (rota_tds.md §4.4). */
static void audit_check(int16_t sub)
{
    log_event_t e = {};
    e.timestamp  = (uint32_t)time(NULL);
    e.event_type = (uint8_t)LOG_SYSTEM;
    e.initiator  = (uint8_t)LOG_BY_SYSTEM;
    e.value_a    = 22;
    e.value_b    = sub;   /* 0 no update · 1 update found · 2 unreachable/HTTP · 3 skip · 4 auth fail */
    log_post(&e);
    s_status.last_result      = sub;
    s_status.last_check_epoch = (int64_t)e.timestamp;
}

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

/** Extract the "version" string field from a manifest JSON body. */
static bool manifest_version(const char *json, char *out, size_t cap)
{
    const char *p = strstr(json, "\"version\"");
    if (p == NULL) return false;
    p = strchr(p, ':'); if (p == NULL) return false;
    p = strchr(p, '"'); if (p == NULL) return false;
    p++;
    const char *e = strchr(p, '"'); if (e == NULL) return false;
    size_t n = (size_t)(e - p);
    if (n == 0 || n + 1u > cap) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
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

    char cert[ROTA_CERT_MAX];
    if (rota_cert_get(cert, sizeof(cert)) < 0) {
        audit_check(2);
        return false;
    }

    int status = 0; char *body = NULL; size_t blen = 0;
    /* Serialise the TLS handshake with T14 (R-C07, gh#23 heap budget). */
    xSemaphoreTake(MX_TLS, portMAX_DELAY);
    esp_err_t err = rota_https_get(url, req, cert, cfg.ota_secret,
                                   ROTA_MANIFEST_MAX, &status, &body, &blen);
    xSemaphoreGive(MX_TLS);

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
        char ver[40];
        if (!manifest_version(body, ver, sizeof(ver))) {
            ESP_LOGW(TAG, "manifest 200 but no parseable version");
            audit_check(2);
        } else {
            snprintf(s_status.offered_ver, sizeof(s_status.offered_ver), "%s", ver);
            if (semver_cmp(ver, FIRMWARE_VERSION) > 0) {
                ESP_LOGI(TAG, "UPDATE AVAILABLE: %s > running %s "
                              "(download/verify/apply = tasks 3.6-3.8, not yet implemented)",
                         ver, FIRMWARE_VERSION);
                audit_check(1);
            } else {
                ESP_LOGI(TAG, "up to date: offered %s, running %s", ver, FIRMWARE_VERSION);
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
        if (!reached) {
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
