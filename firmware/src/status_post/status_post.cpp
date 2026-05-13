/**
 * @file status_post.cpp
 * @brief T14 — Status website POST task implementation.
 *
 * Cycle:
 *  1. Wait 1 s.
 *  2. Read cfg shadow; if disabled / WiFi down / OTA in progress / SNTP not
 *     yet synced, skip this iteration.
 *  3. If status_interval_s has elapsed since the last attempt, snapshot,
 *     format the canonical JSON, and POST to status_url. Log the outcome to
 *     T9 (SD always; NVS only on streak transitions).
 *  4. (Phase E) maybe_upload_log() — log rotation / daily fallback.
 */

#include "status_post.h"
#include "status_json.h"

#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../../../drivers/sdCard/src/sd_storage.h"
#include "cfg_limits.h"

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <esp_log.h>
#include <esp_heap_caps.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static const char *TAG = "T14";

/* ============================================================
 * Compile-time tunables
 * ============================================================ */
#define T14_TICK_MS               1000u   /**< Wake-up cadence */
#define T14_HTTP_TIMEOUT_MS       5000u   /**< Per-request HTTP timeout (status POST) */
#define T14_LOG_HTTP_TIMEOUT_MS  30000u   /**< Per-request HTTP timeout (log upload) */
#define T14_JSON_BUF_BYTES        2048u   /**< Max canonical-payload size */
#define T14_LOG_MAX_BYTES   (5UL*1024UL*1024UL)  /**< Spec ceiling — see api.php § 3.3 */
#define T14_LOG_READ_CHUNK       4096u   /**< SD read chunk into the upload buffer */

/* ============================================================
 * Module-private state
 *
 * The "last attempt" fields are read by /api/web GET to render the live
 * indicator on the Web tab; access is racy but each field is a primitive
 * type written in one store, so the reader sees a coherent snapshot.
 * ============================================================ */
static uint32_t s_last_post_unix    = 0u;     /**< Unix UTC of last POST attempt */
static bool     s_last_post_ok      = true;   /**< Outcome of last POST */
static bool     s_streak_known      = false;  /**< s_last_post_ok has been set at least once */
static TickType_t s_last_post_tick  = 0;      /**< Tick of last successful interval reset */
static bool     s_streak_logged_fail = false; /**< Suppress repeat NVS-ring entries during a failure streak */

/* Log-upload state */
static uint32_t s_last_log_unix     = 0u;     /**< Unix UTC of last log-upload attempt */
static bool     s_last_log_ok       = true;
static bool     s_log_known         = false;
static int      s_last_min_checked  = -1;     /**< Daily-window edge-detector */

/* ============================================================
 * HTTP POST helper — returns true if the server accepted the payload.
 *
 * Branches on URL scheme: https:// uses WiFiClientSecure with cert
 * validation disabled (setInsecure); plain http:// goes through the
 * default HTTPClient transport. The shared secret is sent in the
 * sourceidentifier header per spec §3.1.
 * ============================================================ */
static bool do_status_post(const cfg_shadow_t *cfg, const char *body, size_t body_len)
{
    HTTPClient http;
    bool       opened = false;

    /* Heap-allocated TLS client — only constructed for https:// URLs so we
     * don't pay the WiFiClientSecure footprint for plain HTTP. */
    WiFiClientSecure *secure = nullptr;

    if (strncmp(cfg->status_url, "https://", 8) == 0) {
        secure = new WiFiClientSecure();
        if (secure == nullptr) {
            ESP_LOGW(TAG, "WiFiClientSecure alloc failed");
            return false;
        }
        secure->setInsecure();   /* No cert validation — see impact analysis */
        opened = http.begin(*secure, cfg->status_url);
    } else {
        opened = http.begin(cfg->status_url);
    }

    if (!opened) {
        ESP_LOGW(TAG, "http.begin() failed");
        if (secure) { delete secure; }
        return false;
    }

    http.setTimeout(T14_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type", "application/json");
    /* Never log the secret value. */
    http.addHeader("sourceidentifier", cfg->status_secret);

    int code = http.POST((uint8_t *)body, body_len);
    http.end();
    if (secure) { delete secure; }

    bool ok = (code == 204 || (code >= 200 && code < 300));
    if (!ok) {
        ESP_LOGW(TAG, "POST failed code=%d", code);
    }
    return ok;
}

/* ============================================================
 * Log a POST outcome.
 *
 * Per design decision D4 in the implementation plan:
 *  - Every attempt writes to T9 (SD CSV always; NVS ring only on streak
 *    transitions and the first failure of a streak).
 *
 * Today log_post() writes to both. Until a SD-only variant is wired into
 * T9 we approximate by simply skipping the log_post() call when we are
 * mid-streak; SD-only logging will be added in Phase E (it requires a new
 * helper in event_logger). This keeps Phase C scope-bounded and avoids
 * burning the NVS ring during transient outages.
 * ============================================================ */
static void log_post_outcome(bool ok)
{
    bool transition = (s_streak_known && (ok != s_last_post_ok));
    bool first_fail = (!ok && !s_streak_logged_fail);

    if (!s_streak_known || transition || first_fail) {
        log_event_t ev = {};
        ev.timestamp  = (uint32_t)time(NULL);
        ev.event_type = (uint8_t)LOG_SYSTEM;
        ev.initiator  = (uint8_t)LOG_BY_WEB;
        ev.channel    = 0u;
        ev.param_id   = (uint8_t)LOG_PARAM_NONE;
        ev.value_a    = ok ? 1 : 0;
        ev.value_b    = 0;        /* 0 = status post outcome (see Phase E for log-upload code) */
        log_post(&ev);
    }

    s_last_post_unix = (uint32_t)time(NULL);
    s_last_post_ok   = ok;
    s_streak_known   = true;
    if (ok) { s_streak_logged_fail = false; }
    else    { s_streak_logged_fail = true;  }
}

/* ============================================================
 * Predicate — should we attempt a POST this tick?
 * ============================================================ */
static bool ready_to_post(const cfg_shadow_t *cfg)
{
    if (cfg->status_enable == 0)            { return false; }
    if (cfg->status_url[0] == '\0')         { return false; }
    if (!WiFi.isConnected())                { return false; }
    if (cfg->current_unix_ts < 1700000000UL) { return false; }   /* Not yet NTP-synced */
    if (xEventGroupGetBits(EG1) & EG1_BIT_OTA_IN_PROGRESS) { return false; }

    int32_t interval = cfg->status_interval_s;
    if (interval < 60)  { interval = 60;  }
    if (interval > 300) { interval = 300; }

    TickType_t now = xTaskGetTickCount();
    if (s_last_post_tick == 0) { return true; }   /* First post after enable */
    return (now - s_last_post_tick) >= pdMS_TO_TICKS((uint32_t)interval * 1000UL);
}

/* ============================================================
 * Public — last-attempt formatters for the /api/web GET handler.
 * ============================================================ */
static void format_last(char *buf, size_t cap, uint32_t unix_ts, bool ok, bool known)
{
    if (buf == NULL || cap == 0u) { return; }
    if (!known || unix_ts == 0u) {
        buf[0] = '\0';
        return;
    }
    struct tm tm_info;
    time_t ts = (time_t)unix_ts;
    localtime_r(&ts, &tm_info);
    char when[24];
    strftime(when, sizeof(when), "%Y-%m-%d %H:%M:%S", &tm_info);
    snprintf(buf, cap, "%s %s", ok ? "OK" : "FAIL", when);
}

void status_post_last_str(char *buf, size_t cap)
{
    format_last(buf, cap, s_last_post_unix, s_last_post_ok, s_streak_known);
}

void status_post_last_log_str(char *buf, size_t cap)
{
    format_last(buf, cap, s_last_log_unix, s_last_log_ok, s_log_known);
}

/* ============================================================
 * HTTP POST helper for log uploads (?action=log).
 *
 * Reads the entire SD file into a PSRAM buffer (max 5 MB per spec; T9
 * rotates at 512 KB so the realistic cap is a few hundred KB) and POSTs
 * it as raw bytes with Content-Type: text/plain. The shared-secret header
 * mirrors the status-POST path. On success the body is freed.
 * ============================================================ */
static bool do_log_upload(const cfg_shadow_t *cfg, const char *filename)
{
    if (filename == NULL || filename[0] == '\0') { return false; }

    /* Build absolute path; storage_sd_* expects a leading slash. T9's
     * filename scheme is "YYYYMMDDHHMMSS.csv" (18 chars) so a 32-byte path
     * buffer is comfortably oversized. */
    char path[32];
    snprintf(path, sizeof(path), "/%s", filename);

    uint32_t fsize = storage_sd_file_size(path);
    if (fsize == 0u || fsize > T14_LOG_MAX_BYTES) {
        ESP_LOGW(TAG, "log upload: size %lu out of bounds for %s",
                 (unsigned long)fsize, filename);
        return false;
    }

    /* Allocate the body in PSRAM. +1 lets storage_sd_read keep its NUL
     * terminator semantics without overflowing. */
    uint8_t *body = (uint8_t *)heap_caps_malloc(fsize + 1u, MALLOC_CAP_SPIRAM);
    if (body == nullptr) {
        body = (uint8_t *)malloc(fsize + 1u);
    }
    if (body == nullptr) {
        ESP_LOGW(TAG, "log upload: alloc %lu bytes failed", (unsigned long)fsize);
        return false;
    }

    size_t total = 0;
    while (total < fsize) {
        size_t want   = (fsize - total > T14_LOG_READ_CHUNK) ? T14_LOG_READ_CHUNK
                                                              : (fsize - total);
        size_t got    = 0;
        storage_status_t rc = storage_sd_read(path, total,
                                              (char *)(body + total), want + 1u, &got);
        if (rc != STORAGE_OK || got == 0u) {
            ESP_LOGW(TAG, "log upload: read failed at offset %u (rc=%d got=%u)",
                     (unsigned)total, (int)rc, (unsigned)got);
            free(body);
            return false;
        }
        total += got;
    }

    /* Build target URL with the ?action=log query param. */
    char url[CFG_MAX_URL_LEN + 16] = {};
    snprintf(url, sizeof(url), "%s?action=log", cfg->status_url);

    HTTPClient http;
    WiFiClientSecure *secure = nullptr;
    bool opened = false;

    if (strncmp(cfg->status_url, "https://", 8) == 0) {
        secure = new WiFiClientSecure();
        if (secure == nullptr) { free(body); return false; }
        secure->setInsecure();
        opened = http.begin(*secure, url);
    } else {
        opened = http.begin(url);
    }
    if (!opened) {
        ESP_LOGW(TAG, "log upload: http.begin failed");
        free(body);
        if (secure) { delete secure; }
        return false;
    }

    http.setTimeout(T14_LOG_HTTP_TIMEOUT_MS);
    http.addHeader("Content-Type",     "text/plain");
    http.addHeader("sourceidentifier", cfg->status_secret);

    int code = http.POST(body, total);
    http.end();
    free(body);
    if (secure) { delete secure; }

    bool ok = (code == 204 || (code >= 200 && code < 300));
    if (ok) {
        ESP_LOGI(TAG, "log upload OK: %s (%u bytes)", filename, (unsigned)total);
    } else {
        ESP_LOGW(TAG, "log upload FAILED: %s code=%d", filename, code);
    }
    return ok;
}

static void log_upload_outcome(bool ok, const char *filename)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_WEB;
    ev.channel    = 0u;
    ev.param_id   = (uint8_t)LOG_PARAM_NONE;
    ev.value_a    = ok ? 1 : 0;
    ev.value_b    = 1;        /* 1 = log upload (vs. 0 = status post) */
    log_post(&ev);

    s_last_log_unix = (uint32_t)time(NULL);
    s_last_log_ok   = ok;
    s_log_known     = true;
    (void)filename;
}

/* Emit a diagnostic LOG_SYSTEM event for the daily-fallback slot when the
 * slot fires but no actual upload was attempted. Without this, the web
 * GUI's "Last log upload" indicator stays empty indefinitely with no way
 * to tell whether the slot ever ran. Encoding (extends the existing
 * value_b convention in log_upload_outcome):
 *   value_a = 0    (slot-skip marker — distinguishes from value_a=1 success)
 *   value_b = 2    daily slot hit but no closed file existed on SD
 *   value_b = 3    daily slot hit but a precondition blocked it
 *                  (status disabled / URL empty / WiFi down / pre-NTP / OTA)
 * Documented in event_logger.h alongside the LOG_SYSTEM value_a table.
 * Only called from the daily-fallback path, never from the rotation path.
 */
static void log_upload_skip(uint8_t reason_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_WEB;
    ev.channel    = 0u;
    ev.param_id   = (uint8_t)LOG_PARAM_NONE;
    ev.value_a    = 0;            /* 0 = slot fired without upload */
    ev.value_b    = (int16_t)reason_b;
    log_post(&ev);
}

/* Attempt to upload @p candidate if it differs from the last-uploaded record. */
static void try_log_upload(const cfg_shadow_t *cfg, const char *candidate)
{
    if (candidate == NULL || candidate[0] == '\0')              { return; }
    if (strcmp(candidate, cfg->log_last_up) == 0)               { return; }   /* dedup */

    bool ok = do_log_upload(cfg, candidate);
    log_upload_outcome(ok, candidate);
    if (ok) {
        dm_set_log_last_up(candidate);
    }
}

/* ============================================================
 * maybe_upload_log() — called on every T14 cycle.
 *
 * Two trigger paths, both deduplicated against cfg.log_last_up:
 *  (1) On rotation — event_logger_last_rotated() signals a freshly-closed
 *      file; cheap polling, fires within one T14 tick.
 *  (2) Daily fallback — once per minute we evaluate the local clock; on
 *      the configured H:M edge we scan SD for the newest closed file.
 *
 * Diagnostic visibility (1.17.27): when the daily-slot fires but no upload
 * is attempted, log_upload_skip() emits a LOG_SYSTEM event so the SD log
 * records that the slot ran. Without this, the web GUI's "Last log upload"
 * indicator would stay empty for weeks of normal long-running operation
 * (an active file under the 512 KB rotation threshold has no "closed" peer
 * for newest_closed() to return), giving no clue whether the feature is
 * working. The on-rotation path remains silent on skip — rotations are
 * frequent and intentional.
 * ============================================================ */
static void maybe_upload_log(const cfg_shadow_t *cfg)
{
    /* Daily-slot edge detection — runs first so we can record a diagnostic
     * event even when preconditions block the actual upload below.
     * Requires a plausible Unix timestamp (post-NTP, or RTC-seeded). */
    bool daily_slot_hit = false;
    if (cfg->current_unix_ts >= 1700000000UL) {
        time_t now = (time_t)cfg->current_unix_ts;
        struct tm lt;
        localtime_r(&now, &lt);
        if (lt.tm_min != s_last_min_checked) {
            s_last_min_checked = lt.tm_min;
            if (lt.tm_hour == cfg->log_upload_h &&
                lt.tm_min  == cfg->log_upload_m) {
                daily_slot_hit = true;
            }
        }
    }

    /* Master preconditions for any upload attempt. If the daily slot just
     * fired and a precondition blocks us, emit log_upload_skip(3) so the
     * blocking reason is visible in the log instead of silently dropped. */
    bool preconditions_ok = true;
    if      (cfg->status_enable == 0)                              preconditions_ok = false;
    else if (cfg->status_url[0] == '\0')                           preconditions_ok = false;
    else if (!WiFi.isConnected())                                  preconditions_ok = false;
    else if (cfg->current_unix_ts < 1700000000UL)                  preconditions_ok = false;
    else if (xEventGroupGetBits(EG1) & EG1_BIT_OTA_IN_PROGRESS)    preconditions_ok = false;

    if (!preconditions_ok) {
        if (daily_slot_hit) {
            log_upload_skip(3);            /* slot fired but blocked */
        }
        return;
    }

    /* (1) Rotation path — silent on miss (rotations are routine). */
    if (cfg->log_upload_rot) {
        char rotated[24] = {};
        if (event_logger_last_rotated(rotated, sizeof(rotated))) {
            try_log_upload(cfg, rotated);
        }
    }

    /* (2) Daily fallback — force a rotation first so today's accumulated
     * data becomes a closed file, then upload that file. Without this, a
     * controller emitting events slowly (one SENSOR every 30 s ≈ 36 KB/day,
     * 512 KB rotation threshold ≈ 14 days) would have nothing new to send
     * at the daily slot — the gh#8 issue this code path was designed to
     * fix. force_rotate has a 5 s timeout; on failure (SD unmounted, or
     * T9 too busy to honour the request in time) we fall through to the
     * pre-1.17.28 behaviour: try whatever newest_closed currently is, or
     * log a slot-with-no-file skip. */
    if (daily_slot_hit) {
        (void)event_logger_force_rotate(5000u);

        char newest[24] = {};
        if (event_logger_newest_closed(newest, sizeof(newest))) {
            try_log_upload(cfg, newest);
        } else {
            log_upload_skip(2);            /* slot fired, no closed file */
        }
    }
}

/* ============================================================
 * Task entry
 * ============================================================ */
void task_status_post(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "T14 started — status reporting (idle until configured)");

    /* JSON buffer in BSS — sized once, reused every cycle, no heap churn. */
    static char json_buf[T14_JSON_BUF_BYTES];

    for (;;) {
        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);

        if (ready_to_post(&cfg)) {
            status_snapshot_t snap;
            dm_status_snapshot(&snap);

            /* include_disabled_setpoints=false: the public dashboard
             * receives no rh_max_active/rh_min_active fields when RH ctrl
             * is off — inert configuration is not worth shipping over the
             * wire. */
            size_t n = build_canonical_status_json(json_buf, sizeof(json_buf),
                                                    &snap, (uint32_t)cfg.status_expose,
                                                    false);
            bool ok = false;
            if (n > 0u) {
                ok = do_status_post(&cfg, json_buf, n);
            } else {
                ESP_LOGW(TAG, "JSON build failed");
            }
            log_post_outcome(ok);
            s_last_post_tick = xTaskGetTickCount();
        }

        maybe_upload_log(&cfg);

        vTaskDelay(pdMS_TO_TICKS(T14_TICK_MS));
    }
}
