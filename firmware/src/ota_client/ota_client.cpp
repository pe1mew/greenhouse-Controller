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

#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <esp_log.h>
#include <esp_random.h>
#include <esp_heap_caps.h>
#include <esp_http_client.h>
#include <mbedtls/md.h>

#include "nvs_config.h"

static const char *TAG = "T16_OTA";

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
