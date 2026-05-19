/**
 * @file status_post.cpp
 * @brief T14 — Status website POST task.
 *
 * Owns the controller side of the status-website integration documented in
 * `design/technical-spec-statusWebsite.md`:
 *  - POST <cfg.status_url>                  (status JSON, every cfg.status_interval_s)
 *  - POST <cfg.status_url>?action=log&file=NAME  (CSV log file, daily + on T9 rotation)
 *
 * **a.6.35 hardening** (this revision) — see plan
 * `design/maturationPlan_alpha6.32-6.35.md` §"Alpha 6.35".
 *
 * Items landed:
 *
 *   A. **Shared secret header**. Every POST + log-upload request attaches
 *      `sourceidentifier: <cfg.status_secret>` if the secret is non-empty.
 *      Server-side check is the server's problem; we just forward the value
 *      that T11's `/api/web` validator already ensured is ≥ CFG_MIN_SECRET_LEN.
 *
 *   B. **Canonical status JSON**. `build_canonical_status_json` replaces the
 *      placeholder `build_min_status_json`. The expose mask comes from
 *      `cfg.status_expose`, allowing operators to hide tiles from the public
 *      dashboard. `include_disabled_setpoints = false` — the public dashboard
 *      gets the RH setpoints omitted when RH control is off; the local GUI
 *      uses the same builder with `true` so it can dim the rows instead.
 *
 *   C. **SD CSV log upload**. Two triggers:
 *        - **Daily**: T14 main loop polls local clock; when minute and hour
 *          both equal `cfg.log_upload_h:cfg.log_upload_m` and we haven't yet
 *          uploaded today's most-recent closed file, fire `do_log_upload`.
 *        - **On rotation**: T9 sets the `T14_NOTIFY_LOG_ROTATED` bit via
 *          xTaskNotify when it closes a CSV. T14 waits for it via
 *          xTaskNotifyWait at the bottom of each cycle (timeout = 1 s), reads
 *          `event_logger_last_rotated()`, and (subject to item E) uploads.
 *      Upload uses streaming `esp_http_client_open(fsize)` + 4 KB-chunk loop
 *      via `storage_sd_read` → `esp_http_client_write`. 4 KB chunks bound the
 *      per-write mbedTLS heap demand (gh#23) regardless of total file size.
 *      gh#25 dedup latch: on 2xx, `dm_set_log_last_up(filename)` persists to
 *      NVS so the same daily/rotation trigger doesn't re-upload the same file.
 *
 *   D. **`status_enable` master flag** gate. When 0, the loop idles without
 *      POSTing; `s_last_str` becomes `"DISABLED"` so the GUI Web tab shows
 *      the operator state. URL-empty and interval≤0 stay as additional gates.
 *
 *   E. **`log_upload_rot` rotation flag** gate. The `T14_NOTIFY_LOG_ROTATED`
 *      handler is wrapped in `if (cfg.log_upload_rot != 0)`. Daily-window
 *      upload is independent of this flag — `rot=0` means "daily-only".
 *
 *   F. **`s_last_log_str` updates**. Mirrors `s_last_str`: rendered on the
 *      Web tab via `status_post_last_log_str()`. Format identical to status
 *      POST outcome ("OK ts" / "FAIL ts code=N").
 *
 * **gh#23 mbedTLS mitigations (max_frag_len, single cipher, session-ticket
 * reuse) remain deferred to a follow-on alpha** that lands paired with the
 * T15 supervisor re-enable. The 4 KB chunk size in the streaming upload is
 * the only gh#23-shaped piece in this alpha — it bounds per-write demand
 * regardless of total transfer size.
 *
 * @author Greenhouse Controller project
 */

#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_err.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "status_post.h"
#include "status_json.h"                          /* build_canonical_status_json (a.6.35) */
#include "../types/app_types.h"
#include "../data_manager/data_manager.h"
#include "../event_logger/event_logger.h"
#include "../system_id/system_id.h"

#include "sd_storage.h"                           /* storage_sd_read / file_size (a.6.35 log upload) */

static const char *TAG = "T14_STA";

/** Re-check interval when status_url is empty, interval==0, or status_enable==0. */
#define STATUS_IDLE_RECHECK_MS  60000u

/** Connect timeout for esp_http_client. Generous for high-latency cells. */
#define STATUS_HTTP_TIMEOUT_MS  10000u

/**
 * Canonical-JSON build buffer size. The spec body is typically 600–900 B
 * with all tiles exposed; 2 KB gives ~2× safety margin against future tile
 * additions and is well under T11's 4 KB asynchronous-receive cap. Heap-
 * allocated per cycle so we don't burn the task stack.
 */
#define STATUS_JSON_BUF_BYTES   2048u

/**
 * SD-log streaming chunk size. 4 KB matches the gh#23 follow-on tuning
 * (max_frag_len=1024 lands separately; per-write demand here is what bounds
 * mbedTLS per-handshake heap). Smaller chunks waste TLS overhead, larger
 * chunks blow up per-write heap. 4 KB is the sweet spot empirically.
 */
#define LOG_UPLOAD_CHUNK_BYTES  4096u

/** xTaskNotifyWait timeout at the bottom of each cycle (poll cadence). */
#define CYCLE_WAIT_MS           1000u

/* ============================================================
 * Module state — exposed via accessor functions in status_post.h
 * ============================================================ */

/** Heartbeat counter — incremented at the top of each loop tick. */
static volatile uint32_t s_heartbeat = 0;

/** Cumulative heap drop bytes (still 0 in a.6.35; gh#24 accumulator lands later). */
static volatile uint32_t s_heap_drop_bytes = 0;

/** Last-attempt outcome strings (rendered on T8 LCD + web Web tab). */
static char s_last_str[64]     = {0};
static char s_last_log_str[64] = {0};

/** Last local-time minute we triggered the daily upload, to avoid firing
 *  multiple times within the same minute. -1 = none yet this boot. */
static int s_last_daily_min = -1;

/* ============================================================
 * Public accessors — implementations required by status_post.h
 * ============================================================ */

bool status_post_backoff_active(void)
{
    /* Still no circuit breaker in a.6.35 — re-introduces with T15 supervisor. */
    return false;
}

uint32_t status_post_heartbeat(void)         { return s_heartbeat; }
uint32_t status_post_heap_drop_bytes(void)   { return s_heap_drop_bytes; }
void     status_post_force_teardown(void)    { /* no persistent TLS state yet */ }

void status_post_last_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return;
    strncpy(buf, s_last_str, cap - 1);
    buf[cap - 1] = '\0';
}

void status_post_last_log_str(char *buf, size_t cap)
{
    if (buf == NULL || cap == 0) return;
    strncpy(buf, s_last_log_str, cap - 1);
    buf[cap - 1] = '\0';
}

/* ============================================================
 * HTTP event callback — minimal (no session-ticket reuse yet)
 * ============================================================ */

static esp_err_t http_event_cb(esp_http_client_event_t *evt)
{
    switch (evt->event_id) {
        case HTTP_EVENT_ERROR:           ESP_LOGW(TAG, "[T14] HTTP_EVENT_ERROR"); break;
        case HTTP_EVENT_ON_CONNECTED:    ESP_LOGD(TAG, "[T14] HTTP_EVENT_ON_CONNECTED"); break;
        case HTTP_EVENT_ON_FINISH:       ESP_LOGD(TAG, "[T14] HTTP_EVENT_ON_FINISH"); break;
        case HTTP_EVENT_DISCONNECTED:    ESP_LOGD(TAG, "[T14] HTTP_EVENT_DISCONNECTED"); break;
        default:                         break;
    }
    return ESP_OK;
}

/* ============================================================
 * Q3 helpers — LOG_SYSTEM rows for T14 outcomes
 *
 * Follows the value_a encoding documented in event_logger.h:
 *   value_a=0 = T14 outcome/skip (failure or skip), value_b sub-code
 *   value_a=1 = T14 success,                       value_b sub-code
 *
 * value_b sub-codes (apply to both 0 and 1):
 *   0 = status POST outcome
 *   1 = log upload outcome
 *   2 = daily-slot fired but no closed file on SD (value_a=0 only)
 *   3 = daily-slot fired but precondition blocked (value_a=0 only)
 * ============================================================ */

static void post_log(int16_t value_a, int16_t value_b)
{
    log_event_t evt = {};
    evt.timestamp  = (uint32_t)time(NULL);
    evt.event_type = (uint8_t)LOG_SYSTEM;
    evt.initiator  = (uint8_t)LOG_BY_WEB;
    evt.value_a    = value_a;
    evt.value_b    = value_b;
    log_post(&evt);
}

/* ============================================================
 * Format a "OK YYYY-MM-DD HH:MM:SS" / "FAIL ... code=N" string into a buf.
 * ============================================================ */
static void format_outcome(char *buf, size_t cap, bool ok, int status_code)
{
    char ts[20] = {0};
    time_t now = time(NULL);
    struct tm tm_now;
    localtime_r(&now, &tm_now);
    strftime(ts, sizeof(ts), "%Y-%m-%d %H:%M:%S", &tm_now);

    if (ok) {
        snprintf(buf, cap, "OK %s", ts);
    } else {
        snprintf(buf, cap, "FAIL %s code=%d", ts, status_code);
    }
}

/* ============================================================
 * Internal: build the canonical status JSON (a.6.35 item B)
 *
 * Uses dm_status_snapshot to fill a status_snapshot_t, then calls the
 * shared build_canonical_status_json with cfg.status_expose as the mask
 * and include_disabled_setpoints=false (public dashboard policy).
 * ============================================================ */
static size_t build_status_body(char *buf, size_t cap, const cfg_shadow_t *cfg)
{
    if (buf == NULL || cap == 0) return 0;

    status_snapshot_t snap = {};
    dm_status_snapshot(&snap);

    return build_canonical_status_json(buf, cap, &snap,
                                       (uint32_t)cfg->status_expose,
                                       /* include_disabled_setpoints = */ false);
}

/* ============================================================
 * Internal: execute one HTTPS status POST (item A + B)
 *
 * Returns true on HTTP 2xx, false otherwise. Records s_last_str + Q3 row.
 * ============================================================ */
static bool do_status_post(const cfg_shadow_t *cfg)
{
    /* Heap-allocate the body buffer; status_snapshot_t plus the formatted
     * JSON are large enough that stashing them on the task stack would push
     * us close to the 6 KB T14 stack ceiling. */
    char *body = (char *)heap_caps_malloc(STATUS_JSON_BUF_BYTES, MALLOC_CAP_INTERNAL);
    if (body == NULL) {
        ESP_LOGW(TAG, "[T14] heap_caps_malloc(%u) for JSON body failed",
                 (unsigned)STATUS_JSON_BUF_BYTES);
        snprintf(s_last_str, sizeof(s_last_str), "ALLOC_FAIL");
        post_log(0, 0);   /* value_a=0 outcome=failure, sub=status POST */
        return false;
    }
    const size_t body_len = build_status_body(body, STATUS_JSON_BUF_BYTES, cfg);
    if (body_len == 0 || body_len >= STATUS_JSON_BUF_BYTES) {
        ESP_LOGW(TAG, "[T14] status JSON build returned %u (cap=%u)",
                 (unsigned)body_len, (unsigned)STATUS_JSON_BUF_BYTES);
        heap_caps_free(body);
        snprintf(s_last_str, sizeof(s_last_str), "JSON_FAIL");
        post_log(0, 0);
        return false;
    }

    esp_http_client_config_t cfg_http = {};
    cfg_http.url                          = cfg->status_url;
    cfg_http.method                       = HTTP_METHOD_POST;
    cfg_http.timeout_ms                   = STATUS_HTTP_TIMEOUT_MS;
    cfg_http.event_handler                = http_event_cb;
    cfg_http.transport_type               = HTTP_TRANSPORT_OVER_SSL;
    cfg_http.crt_bundle_attach            = esp_crt_bundle_attach;
    cfg_http.keep_alive_enable            = true;
    cfg_http.buffer_size                  = 1024;
    cfg_http.buffer_size_tx               = 1024;
    cfg_http.skip_cert_common_name_check  = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
    if (client == NULL) {
        ESP_LOGW(TAG, "[T14] esp_http_client_init failed");
        heap_caps_free(body);
        snprintf(s_last_str, sizeof(s_last_str), "INIT_FAIL");
        post_log(0, 0);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "application/json");
    /* a.6.35 item A: shared-secret header. Skip when secret is empty —
     * server-side reject is the server's problem. */
    if (cfg->status_secret[0] != '\0') {
        esp_http_client_set_header(client, "sourceidentifier", cfg->status_secret);
    }
    esp_http_client_set_post_field(client, body, (int)body_len);

    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_perform(client);
    const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    const int status_code = esp_http_client_get_status_code(client);
    const int content_len = esp_http_client_get_content_length(client);
    esp_http_client_cleanup(client);
    heap_caps_free(body);

    const bool ok = (err == ESP_OK && status_code >= 200 && status_code < 300);
    format_outcome(s_last_str, sizeof(s_last_str), ok, status_code);

    if (ok) {
        ESP_LOGI(TAG, "[T14] status POST OK: status=%d resp_len=%d elapsed=%lld ms (body=%u B)",
                 status_code, content_len, elapsed_ms, (unsigned)body_len);
        post_log(1, 0);   /* value_a=1 outcome=success, sub=status POST */
    } else {
        ESP_LOGW(TAG, "[T14] status POST FAIL: err=%s status=%d elapsed=%lld ms",
                 esp_err_to_name(err), status_code, elapsed_ms);
        post_log(0, 0);
    }
    return ok;
}

/* ============================================================
 * Internal: stream an SD CSV file via HTTPS POST (a.6.35 item C)
 *
 * URL: <cfg.status_url>?action=log&file=<filename>
 * Body: raw CSV content, streamed in 4 KB chunks.
 *
 * Returns true on HTTP 2xx, false otherwise.
 * On success: dm_set_log_last_up(filename) is called (gh#25 dedup latch).
 *
 * @param filename  Bare filename (no path), e.g. "20260507143022.csv".
 * @param cfg       Snapshot taken by the caller (used for url + secret).
 * ============================================================ */
static bool do_log_upload(const char *filename, const cfg_shadow_t *cfg)
{
    if (filename == NULL || filename[0] == '\0') {
        ESP_LOGW(TAG, "[T14] do_log_upload: empty filename");
        return false;
    }

    /* SD path has a leading '/'; the bare name we get from event_logger
     * APIs and from cfg.log_last_up does not. Build the SD path here. */
    char sd_path[40];
    snprintf(sd_path, sizeof(sd_path), "/%s", filename);
    const uint32_t file_size = storage_sd_file_size(sd_path);
    if (file_size == 0) {
        ESP_LOGW(TAG, "[T14] do_log_upload: %s missing or empty", sd_path);
        snprintf(s_last_log_str, sizeof(s_last_log_str), "FAIL nofile");
        post_log(0, 1);   /* value_a=0 outcome=failure, sub=log upload */
        return false;
    }

    /* Build full URL: <status_url>?action=log&file=<filename>. T11's URL
     * validator already rejected ? and # in cfg.status_url, so we can
     * concatenate safely. */
    char full_url[200];
    int u = snprintf(full_url, sizeof(full_url),
                     "%s?action=log&file=%s", cfg->status_url, filename);
    if (u < 0 || (size_t)u >= sizeof(full_url)) {
        ESP_LOGW(TAG, "[T14] do_log_upload: URL too long");
        snprintf(s_last_log_str, sizeof(s_last_log_str), "FAIL urllen");
        post_log(0, 1);
        return false;
    }

    esp_http_client_config_t cfg_http = {};
    cfg_http.url                          = full_url;
    cfg_http.method                       = HTTP_METHOD_POST;
    cfg_http.timeout_ms                   = STATUS_HTTP_TIMEOUT_MS;
    cfg_http.event_handler                = http_event_cb;
    cfg_http.transport_type               = HTTP_TRANSPORT_OVER_SSL;
    cfg_http.crt_bundle_attach            = esp_crt_bundle_attach;
    cfg_http.keep_alive_enable            = true;
    cfg_http.buffer_size                  = 1024;
    cfg_http.buffer_size_tx               = 1024;
    cfg_http.skip_cert_common_name_check  = false;

    esp_http_client_handle_t client = esp_http_client_init(&cfg_http);
    if (client == NULL) {
        ESP_LOGW(TAG, "[T14] log upload: esp_http_client_init failed");
        snprintf(s_last_log_str, sizeof(s_last_log_str), "FAIL init");
        post_log(0, 1);
        return false;
    }

    esp_http_client_set_header(client, "Content-Type", "text/csv");
    if (cfg->status_secret[0] != '\0') {
        esp_http_client_set_header(client, "sourceidentifier", cfg->status_secret);
    }

    /* esp_http_client_open with the known total size establishes the
     * Content-Length header so the server can preallocate / validate.
     * Streamed-write mode opens the connection without sending a body
     * up-front. */
    const int64_t t0 = esp_timer_get_time();
    esp_err_t err = esp_http_client_open(client, (int)file_size);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T14] log upload: esp_http_client_open failed: %s",
                 esp_err_to_name(err));
        esp_http_client_cleanup(client);
        snprintf(s_last_log_str, sizeof(s_last_log_str), "FAIL open");
        post_log(0, 1);
        return false;
    }

    /* Heap-allocate the chunk buffer to keep stack usage flat. */
    uint8_t *chunk = (uint8_t *)heap_caps_malloc(LOG_UPLOAD_CHUNK_BYTES, MALLOC_CAP_INTERNAL);
    if (chunk == NULL) {
        ESP_LOGW(TAG, "[T14] log upload: heap_caps_malloc(%u) failed",
                 (unsigned)LOG_UPLOAD_CHUNK_BYTES);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        snprintf(s_last_log_str, sizeof(s_last_log_str), "FAIL alloc");
        post_log(0, 1);
        return false;
    }

    /* Streaming loop: read N bytes from SD, write to HTTPS, repeat. */
    uint32_t offset = 0;
    bool write_ok = true;
    while (offset < file_size) {
        const uint32_t want = (file_size - offset > LOG_UPLOAD_CHUNK_BYTES)
                                ? LOG_UPLOAD_CHUNK_BYTES
                                : (file_size - offset);
        size_t got = 0;
        /* storage_sd_read NUL-terminates → buf_len must include the NUL.
         * We pass want+1 and discard the NUL — but the +1 might exceed our
         * allocation. Instead pass want bytes, accept the truncated NUL
         * inside that count, and clamp the actual write to (got). */
        storage_status_t rrc = storage_sd_read(sd_path, offset,
                                               (char *)chunk, (size_t)(want + 1u), &got);
        if (rrc != STORAGE_OK || got == 0) {
            ESP_LOGW(TAG, "[T14] log upload: storage_sd_read failed at %u (rc=%d, got=%u)",
                     (unsigned)offset, (int)rrc, (unsigned)got);
            write_ok = false;
            break;
        }
        const int wn = esp_http_client_write(client, (const char *)chunk, got);
        if (wn < 0 || (size_t)wn != got) {
            ESP_LOGW(TAG, "[T14] log upload: esp_http_client_write returned %d at %u/%u",
                     wn, (unsigned)offset, (unsigned)file_size);
            write_ok = false;
            break;
        }
        offset += (uint32_t)got;
    }
    heap_caps_free(chunk);

    int status_code = -1;
    int content_len = -1;
    if (write_ok) {
        const int64_t fh = esp_http_client_fetch_headers(client);
        if (fh < 0) {
            ESP_LOGW(TAG, "[T14] log upload: fetch_headers failed (%lld)",
                     (long long)fh);
            write_ok = false;
        } else {
            status_code = esp_http_client_get_status_code(client);
            content_len = esp_http_client_get_content_length(client);
        }
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);

    const int64_t elapsed_ms = (esp_timer_get_time() - t0) / 1000;
    const bool ok = write_ok && status_code >= 200 && status_code < 300;
    format_outcome(s_last_log_str, sizeof(s_last_log_str), ok, status_code);

    if (ok) {
        ESP_LOGI(TAG, "[T14] log upload OK: file=%s size=%u status=%d resp_len=%d elapsed=%lld ms",
                 filename, (unsigned)file_size, status_code, content_len, elapsed_ms);
        post_log(1, 1);   /* value_a=1 outcome=success, sub=log upload */
        /* gh#25 dedup latch — persist the just-uploaded filename so the
         * daily-trigger and rotation-trigger paths both skip a repeat. */
        dm_set_log_last_up(filename);
    } else {
        ESP_LOGW(TAG, "[T14] log upload FAIL: file=%s status=%d elapsed=%lld ms",
                 filename, status_code, elapsed_ms);
        post_log(0, 1);
    }
    return ok;
}

/* ============================================================
 * upload_pending — a.6.35.2 multi-file backlog drainer
 *
 * Walks every closed CSV on the SD card whose name is lex-greater than
 * @p cfg->log_last_up (which corresponds to chronologically newer, given
 * the YYYYMMDDHHMMSS filename scheme), oldest first, calling do_log_upload
 * on each. do_log_upload's success path persists `cfg.log_last_up` via
 * dm_set_log_last_up, so each successful upload advances the dedup latch
 * one step.
 *
 * On the first upload failure the loop bails. The next trigger (daily,
 * on-rotation, or a fresh xTaskNotify after re-enable) re-runs this and
 * resumes from the last successfully delivered file because the latch
 * only advanced for files that returned 2xx.
 *
 * Caller's cfg snapshot is intentionally NOT re-read mid-loop — we track
 * an iteration-local `after` instead. dm_set_log_last_up updates the MX4
 * shadow synchronously so the *next* dm_cfg_snapshot will reflect the
 * advanced latch on the following T14 main-loop iteration.
 *
 * Why this exists (was a real gap in a.6.35): the previous implementation
 * looked at exactly one file per trigger — `event_logger_last_rotated()`
 * for on-rotation and `event_logger_newest_closed()` for daily. If WiFi
 * was down (or the status server returned 5xx) across a rotation, the
 * stranded middle file was unreachable: the rotation trigger's
 * s_last_closed got overwritten by the next rotation, and the daily
 * trigger's newest_closed always returned the lex-max which equalled the
 * latch on the next successful round. The middle file just sat on SD
 * until SD_MAX_FILES eventually deleted it — quietly, without ever
 * reaching the server.
 *
 * @return Count of files successfully uploaded this call (≥0). Zero is
 *         the steady-state when no closed file is newer than the latch.
 * ============================================================ */
static int upload_pending(const cfg_shadow_t *cfg)
{
    /* Local cursor — advances per successful upload. Sized 32 to match
     * cfg.log_last_up (33 bytes with NUL). */
    char after[33] = {0};
    strncpy(after, cfg->log_last_up, sizeof(after) - 1u);

    int n_uploaded = 0;
    char next[24];
    /* Safety cap: at most SD_MAX_FILES (=10) closed files exist, so 12
     * iterations is well over any realistic backlog. Prevents an
     * accidental infinite loop if dm_set_log_last_up silently failed. */
    for (int i = 0; i < 12; i++) {
        if (!event_logger_next_pending(after, next, sizeof(next))) {
            break;   /* no more pending files */
        }
        ESP_LOGI(TAG, "[T14] upload_pending: %s (after=\"%s\")", next, after);
        if (!do_log_upload(next, cfg)) {
            ESP_LOGW(TAG, "[T14] upload_pending: stopped at %s — next trigger resumes",
                     next);
            break;   /* failure: leave for the next trigger */
        }
        /* do_log_upload's 2xx path already called dm_set_log_last_up(next).
         * Advance our local cursor too so the next iteration's
         * event_logger_next_pending picks the file after this one. */
        strncpy(after, next, sizeof(after) - 1u);
        after[sizeof(after) - 1u] = '\0';
        n_uploaded++;
    }
    return n_uploaded;
}

/* ============================================================
 * Task entry point
 * ============================================================ */

void task_status_post(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T14] task alive (a.6.35: secret + canonical JSON + SD log upload + gates)");

    /* Initial settling delay — T4 is up but cfg may still be loading. */
    vTaskDelay(pdMS_TO_TICKS(2000));

    uint32_t last_post_ms = 0;

    /* a.6.35.1 — disabled-state tracker. True until proven otherwise so the
     * very first iteration writes `s_last_str = "DISABLED"` if the unit
     * boots with status disabled. Toggles back to true only on disabled→
     * enabled transition so the disabled-branch idle wake-ups don't re-stamp
     * the string every cycle (which would clobber an in-flight `s_last_str`
     * set during a brief active window). */
    bool s_was_disabled = true;

    for (;;) {
        s_heartbeat++;

        cfg_shadow_t cfg = {};
        dm_cfg_snapshot(&cfg);

        /* a.6.35 item D — three gates: master enable, URL set, sane interval.
         * Any failure idles the task; we still consume notify bits so we
         * don't accumulate a stale rotation signal across a re-enable. */
        const bool disabled =
            (cfg.status_enable == 0) ||
            (cfg.status_url[0] == '\0') ||
            (cfg.status_interval_s <= 0);

        if (disabled) {
            /* Only stamp "DISABLED" once per enabled→disabled transition.
             * Otherwise the 60 s idle wake-up would overwrite an in-flight
             * `OK ts`/`FAIL ts ...` string the active branch might be
             * computing in a parallel iteration (no real concurrency since
             * single task — but the future-proofing is cheap). */
            if (!s_was_disabled) {
                snprintf(s_last_str, sizeof(s_last_str), "DISABLED");
                s_was_disabled = true;
            }
            ESP_LOGD(TAG, "[T14] disabled (enable=%ld url=\"%s\" interval=%ld)",
                     (long)cfg.status_enable, cfg.status_url, (long)cfg.status_interval_s);
            /* Idle: wait up to STATUS_IDLE_RECHECK_MS, but wake immediately
             * on any notify bit (T14_NOTIFY_CFG_CHANGED fires from
             * dm_reload_web_cfg() on every /api/web POST, so an operator
             * re-enabling status sees the change reflected within ~1 s
             * instead of waiting out the full idle window). */
            uint32_t drain = 0;
            (void)xTaskNotifyWait(0, ULONG_MAX, &drain, pdMS_TO_TICKS(STATUS_IDLE_RECHECK_MS));
            continue;
        }

        /* a.6.35.1 — disabled→enabled transition. Clear `s_last_str` so the
         * GUI shows `—` (pending) for the brief window between the operator
         * clicking Apply and the first status POST completing. Without this,
         * the form shows `enable=1` while the indicator still reads
         * `DISABLED` — confusing. Also reset `last_post_ms = 0` so the first
         * POST fires on the very next cycle without waiting up to a full
         * `status_interval_s` for the cadence check. */
        if (s_was_disabled) {
            s_last_str[0] = '\0';
            last_post_ms = 0;
            s_was_disabled = false;
            ESP_LOGI(TAG, "[T14] re-enabled (url=%s interval=%lds expose=0x%02lX) — POST imminent",
                     cfg.status_url, (long)cfg.status_interval_s,
                     (unsigned long)cfg.status_expose);
        }

        /* ----------------------------------------------------------------
         * Daily log-upload trigger (a.6.35 item C, daily half;
         * a.6.35.2 multi-file drain).
         *
         * Fires at cfg.log_upload_h:cfg.log_upload_m local time. Uses
         * s_last_daily_min to ensure we only fire once per minute (the
         * outer loop runs at ~1 Hz so without this guard we'd fire
         * 60+ times across the matching minute).
         *
         * upload_pending walks every closed file newer than cfg.log_last_up,
         * draining a backlog if one accumulated (WiFi outage spanning a
         * rotation, etc.). Returns 0 when nothing pending → log the
         * value_a=0, value_b=2 "fired but no fresh file" diagnostic.
         * ---------------------------------------------------------------- */
        time_t now = time(NULL);
        struct tm tm_local;
        localtime_r(&now, &tm_local);
        if (tm_local.tm_hour == cfg.log_upload_h &&
            tm_local.tm_min  == cfg.log_upload_m &&
            tm_local.tm_min  != s_last_daily_min) {
            s_last_daily_min = tm_local.tm_min;
            ESP_LOGI(TAG, "[T14] daily upload trigger @ %02d:%02d (latch=%s)",
                     tm_local.tm_hour, tm_local.tm_min, cfg.log_last_up);
            const int n = upload_pending(&cfg);
            if (n == 0) {
                ESP_LOGD(TAG, "[T14] daily: no pending files, skip");
                post_log(0, 2);   /* daily-slot fired but nothing fresh */
            }
        }
        /* When we cross into a different minute, clear the latch so the
         * next day's matching minute fires again. */
        if (tm_local.tm_min != s_last_daily_min && s_last_daily_min >= 0 &&
            (tm_local.tm_hour != cfg.log_upload_h || tm_local.tm_min != cfg.log_upload_m)) {
            s_last_daily_min = -1;
        }

        /* ----------------------------------------------------------------
         * Status POST cycle (item B body + item A header).
         * ---------------------------------------------------------------- */
        const uint32_t now_ms = (uint32_t)(esp_timer_get_time() / 1000);
        const uint32_t interval_ms = (uint32_t)cfg.status_interval_s * 1000u;
        if (last_post_ms == 0 || (now_ms - last_post_ms) >= interval_ms) {
            (void)do_status_post(&cfg);
            last_post_ms = now_ms;
        }

        /* ----------------------------------------------------------------
         * Cycle wait — block up to 1 s waiting for a rotation notify or
         * timeout. xTaskNotifyWait clears the masked bits on receive
         * (ULONG_MAX), so any pending T14_NOTIFY_LOG_ROTATED bit is
         * consumed atomically.
         * ---------------------------------------------------------------- */
        uint32_t notify = 0;
        BaseType_t got = xTaskNotifyWait(0, ULONG_MAX, &notify,
                                         pdMS_TO_TICKS(CYCLE_WAIT_MS));

        if (got == pdPASS && (notify & T14_NOTIFY_LOG_ROTATED) != 0u) {
            /* a.6.35 item E — rotation upload gated by cfg.log_upload_rot.
             * a.6.35.2 — drains the full backlog of pending files via
             * upload_pending rather than just the most-recently rotated one.
             * Handles the case where multiple rotations happened during a
             * long single upload, or while T14 was unable to reach the
             * server. When log_upload_rot=0, silently consume the notify. */
            if (cfg.log_upload_rot != 0) {
                ESP_LOGI(TAG, "[T14] rotation upload trigger (latch=%s)", cfg.log_last_up);
                (void)upload_pending(&cfg);
            } else {
                ESP_LOGD(TAG, "[T14] rotation notify ignored (log_upload_rot=0)");
            }
        }
    }
}
