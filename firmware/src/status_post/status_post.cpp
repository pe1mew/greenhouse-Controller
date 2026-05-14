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
#include "../../../drivers/nvs/src/nvs_config.h"   /* Breaker NVS persistence (gh#18 Phase 2) */
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
#define T14_TICK_MS                  1000u   /**< Wake-up cadence */
#define T14_HTTP_CONNECT_TIMEOUT_MS  3000u   /**< TCP-connect timeout (gh#18 Phase 1) */
#define T14_HTTP_TIMEOUT_MS          5000u   /**< Per-request HTTP timeout (status POST) */
#define T14_LOG_HTTP_TIMEOUT_MS     30000u   /**< Per-request HTTP timeout (log upload — body is large, keep at 30 s) */
#define T14_JSON_BUF_BYTES           2048u   /**< Max canonical-payload size */
#define T14_LOG_MAX_BYTES   (5UL*1024UL*1024UL)  /**< Spec ceiling — see api.php § 3.3 */
#define T14_LOG_READ_CHUNK           4096u   /**< SD read chunk into the upload buffer */

/* ============================================================
 * Per-endpoint breaker state (gh#18 Phase 1+2).
 *
 * Two independent breakers: one for the periodic status POST, one for the
 * daily log upload. They fail at very different rates and on different
 * time scales, so they need independent state.
 *
 * Phase 1 (1.17.34) introduced the struct with last_unix/last_ok/known/
 * streak_logged_fail (refactor of the loose statics from 1.17.30).
 * Phase 2 (1.17.35) adds:
 *  - open_until_unix    NVS-persisted. Breaker is open while
 *                        time(NULL) < open_until_unix; 0 = closed.
 *  - hold_phase         NVS-persisted. Index into BREAKER_PHASE_S[]:
 *                        0 = closed (no backoff window in flight),
 *                        1 = 60 s, 2 = 5 min, 3 = 30 min, 4 = 1 h (cap).
 *  - consec_fail        RAM only. Resets on success. Advances hold_phase
 *                        when it reaches BREAKER_FAIL_THRESHOLD.
 *  - consec_ok          RAM only. Resets on fail. Regresses hold_phase
 *                        when it reaches BREAKER_OK_TO_REGRESS.
 *
 * The fields are read by /api/web GET (via format_last) and by
 * status_post_backoff_active() — access is racy but each field is a
 * primitive type written in one store, so the reader sees a coherent
 * snapshot.
 * ============================================================ */
typedef struct {
    /* Phase 1 fields */
    uint32_t last_unix;            /**< Unix UTC of last attempt */
    bool     last_ok;              /**< Outcome of last attempt */
    bool     known;                /**< true once last_ok has been set at least this boot */
    bool     streak_logged_fail;   /**< Suppress repeat fail-events during a failure streak */
    /* Phase 2 fields */
    uint32_t open_until_unix;      /**< NVS-persisted. Breaker open while now < this. 0 = closed. */
    uint8_t  hold_phase;           /**< NVS-persisted. 0..4 escalation phase. */
    uint8_t  consec_fail;          /**< RAM only. Consecutive failures since last success. */
    uint8_t  consec_ok;            /**< RAM only. Consecutive successes since last failure. */
} t14_breaker_t;

static t14_breaker_t s_post_breaker = {};   /* zero-init: closed, no history */
static t14_breaker_t s_log_breaker  = {};

/* Exponential backoff schedule for the breaker. Index by hold_phase. */
static const uint32_t BREAKER_PHASE_S[] = {
        0u,        /* 0 — closed (no backoff in flight) */
       60u,        /* 1 — 60 s */
      300u,        /* 2 — 5 min */
     1800u,        /* 3 — 30 min */
     3600u,        /* 4 — 1 h, capped */
};
#define BREAKER_FAIL_THRESHOLD   3u   /* consecutive fails before phase advance */
#define BREAKER_OK_TO_REGRESS    5u   /* consecutive successes before phase regress */
#define BREAKER_MAX_PHASE        4u

/* NVS keys for breaker persistence (namespace NVS_NS_SYSTEM). */
static const char K_BK_POST_UNTIL[] = "t14_post_until";
static const char K_BK_POST_PHASE[] = "t14_post_phase";
static const char K_BK_LOG_UNTIL[]  = "t14_log_until";
static const char K_BK_LOG_PHASE[]  = "t14_log_phase";

/* ============================================================
 * Persistent TLS client (gh#18 Phase 1).
 *
 * Pre-1.17.34 allocated a fresh WiFiClientSecure on the heap inside every
 * POST and log-upload call, forcing a fresh mbedTLS handshake each time
 * (~6-9 KB of heap-in-flight per attempt). The static instance below is
 * module-scope BSS; with http.setReuse(true) + Connection: keep-alive
 * headers the underlying TCP socket — and therefore the established TLS
 * session — persists across calls. setInsecure() is applied once when
 * s_secure_inited transitions to true.
 *
 * Reset to fresh state on:
 *  - WiFi disconnect (edge-detected at top of task loop)
 *  - HTTPClient error return ≤ 0 (connection-lost / refused / DNS / etc.)
 * Both paths call s_secure.stop() + s_secure_inited = false so the next
 * attempt does a clean handshake on the next available WiFi association.
 * ============================================================ */
static WiFiClientSecure s_secure;
static bool             s_secure_inited = false;
static bool             s_wifi_was_connected = false;   /**< edge tracker */

/* ============================================================
 * Supervisor integration state (gh#18 Phase 4, since 1.18.0).
 *
 * `s_heartbeat` is incremented at the top of every task loop iteration.
 * The supervisor (T15) treats no-advance-for-60s as "T14 stuck" and force-
 * respawns. Single primitive, no mutex needed.
 *
 * `s_heap_drop_bytes` accumulates the per-call free-heap delta whenever
 * the value drops across an HTTPS call. Negative deltas are clamped to
 * zero so a transient free doesn't reset the counter (real leaks are
 * monotonic). Supervisor compares against a 64 KB ceiling.
 * ============================================================ */
static volatile uint32_t s_heartbeat        = 0u;
static volatile uint32_t s_heap_drop_bytes  = 0u;

/* ============================================================
 * Other module-private state (unchanged across the Phase-1 refactor).
 * ============================================================ */
static TickType_t s_last_post_tick  = 0;       /**< Tick of last successful interval reset */
static int        s_last_min_checked = -1;     /**< Daily-window edge-detector */

/* ============================================================
 * Breaker helpers (gh#18 Phase 2).
 *
 * Three operations:
 *  - breaker_load() — populate open_until_unix and hold_phase from NVS at
 *    task entry. RAM-only counters (consec_*) start at 0.
 *  - breaker_open() — non-mutating predicate. Returns true iff the breaker
 *    is currently in backoff (now_unix < open_until_unix). Pre-NTP
 *    (now_unix < 1700000000) is treated as "not in backoff" because
 *    comparing against an absolute Unix time is meaningless before the
 *    clock has been synced. The pre-NTP guard in ready_to_post() already
 *    blocks the attempt for an orthogonal reason.
 *  - breaker_record() — mutating. On fail: increment consec_fail; when it
 *    reaches BREAKER_FAIL_THRESHOLD, advance hold_phase (capped) and set
 *    open_until_unix = now + BREAKER_PHASE_S[hold_phase]; persist both
 *    fields to NVS. On success: clear consec_fail; if breaker was open,
 *    clear open_until_unix and persist. When consec_ok reaches
 *    BREAKER_OK_TO_REGRESS, regress hold_phase by one step (persisted).
 *    No-op pre-NTP — see above.
 *
 * NVS write strategy: only on phase transitions (and on clearing the
 * open window). Steady-state success or steady-state fail-before-threshold
 * touches NVS zero times, keeping wear well under 100 writes/year.
 * ============================================================ */
static void breaker_load(t14_breaker_t *b,
                         const char *k_until, const char *k_phase)
{
    int32_t v = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, k_until, 0, &v);
    b->open_until_unix = (v < 0) ? 0u : (uint32_t)v;
    nvs_cfg_get_i32_or_default(NVS_NS_SYSTEM, k_phase, 0, &v);
    if (v < 0)                              { v = 0; }
    if (v > (int32_t)BREAKER_MAX_PHASE)     { v = (int32_t)BREAKER_MAX_PHASE; }
    b->hold_phase  = (uint8_t)v;
    b->consec_fail = 0u;
    b->consec_ok   = 0u;
}

static bool breaker_open(const t14_breaker_t *b, uint32_t now_unix)
{
    if (now_unix < 1700000000UL) { return false; }
    return (b->open_until_unix > now_unix);
}

static void breaker_record(t14_breaker_t *b, bool ok, uint32_t now_unix,
                           const char *k_until, const char *k_phase)
{
    if (now_unix < 1700000000UL) { return; }   /* don't advance pre-NTP */

    if (ok) {
        b->consec_fail = 0u;
        if (b->consec_ok < 0xFFu) { b->consec_ok++; }

        /* First success after open: clear the until-timestamp. Phase stays
         * at its current escalation level until BREAKER_OK_TO_REGRESS
         * sustained successes have accumulated (prevents 30m→60s→30m yo-yo
         * under intermittent connectivity). */
        if (b->open_until_unix != 0u) {
            b->open_until_unix = 0u;
            (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, k_until, 0);
            ESP_LOGI(TAG, "breaker %s: first success after open — closing window (phase=%u)",
                     k_phase, (unsigned)b->hold_phase);
        }

        /* Regress one phase step after a sustained success streak. */
        if (b->consec_ok >= BREAKER_OK_TO_REGRESS && b->hold_phase > 0u) {
            b->hold_phase--;
            (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, k_phase, (int32_t)b->hold_phase);
            b->consec_ok = 0u;
            ESP_LOGI(TAG, "breaker %s: regressed to phase=%u after %u successes",
                     k_phase, (unsigned)b->hold_phase, (unsigned)BREAKER_OK_TO_REGRESS);
        }
        return;
    }

    /* Failure path. */
    b->consec_ok = 0u;
    if (b->consec_fail < 0xFFu) { b->consec_fail++; }

    if (b->consec_fail >= BREAKER_FAIL_THRESHOLD) {
        if (b->hold_phase < BREAKER_MAX_PHASE) { b->hold_phase++; }
        b->open_until_unix = now_unix + BREAKER_PHASE_S[b->hold_phase];
        b->consec_fail = 0u;   /* reset so the next 3-streak advances again */
        (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, k_phase, (int32_t)b->hold_phase);
        (void)nvs_cfg_set_i32(NVS_NS_SYSTEM, k_until, (int32_t)b->open_until_unix);
        ESP_LOGW(TAG, "breaker %s: advanced to phase=%u — open until +%lu s",
                 k_phase, (unsigned)b->hold_phase,
                 (unsigned long)BREAKER_PHASE_S[b->hold_phase]);
    }
}

/* ============================================================
 * HTTP open / error helpers (gh#18 Phase 1).
 *
 * http_open_for() centralises the per-call setup boilerplate previously
 * duplicated between do_status_post() and do_log_upload(): scheme branch,
 * TLS client init on first https:// call, connect-timeout + response-
 * timeout, keep-alive header, shared-secret header. Returns true if the
 * HTTPClient is ready for caller to add Content-Type and invoke POST/send.
 *
 * http_handle_error() resets the persistent TLS client when an HTTPClient
 * return code indicates a transport-level failure (≤ 0: connection lost,
 * refused, DNS, etc.). Keeping s_secure alive across such failures would
 * leave it pointing at a dead socket; reset so the next attempt re-handshakes.
 * Server-level HTTP errors (4xx, 5xx) are NOT transport failures and leave
 * the TLS session intact for the next call to reuse.
 * ============================================================ */
static bool http_open_for(const cfg_shadow_t *cfg, HTTPClient *http,
                          const char *url, uint32_t response_timeout_ms)
{
    bool opened = false;
    if (strncmp(cfg->status_url, "https://", 8) == 0) {
        if (!s_secure_inited) {
            s_secure.setInsecure();   /* No cert validation — see impact analysis */
            s_secure_inited = true;
        }
        opened = http->begin(s_secure, url);
    } else {
        opened = http->begin(url);
    }
    if (!opened) {
        return false;
    }
    /* Order matters: setReuse(true) before adding the keep-alive header so
     * HTTPClient internally retains the underlying client connection on end(). */
    http->setReuse(true);
    http->setConnectTimeout((int32_t)T14_HTTP_CONNECT_TIMEOUT_MS);
    http->setTimeout(response_timeout_ms);
    http->addHeader("Connection",       "keep-alive");
    http->addHeader("sourceidentifier", cfg->status_secret);
    return true;
}

static void http_handle_error(int code)
{
    /* Transport-level failures: connection lost / refused / send fail / etc.
     * HTTPClient returns negative values from the HTTPC_ERROR_* family. */
    if (code <= 0) {
        s_secure.stop();
        s_secure_inited = false;
    }
}

/* ============================================================
 * Status POST — small JSON body, ~1 KB. The shared TLS session is
 * preferred; failure cases tear it down so the next attempt is fresh.
 * ============================================================ */
static bool do_status_post(const cfg_shadow_t *cfg, const char *body, size_t body_len)
{
    HTTPClient http;
    if (!http_open_for(cfg, &http, cfg->status_url, T14_HTTP_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "http_open_for failed (status POST)");
        s_secure.stop();
        s_secure_inited = false;
        return false;
    }
    http.addHeader("Content-Type", "application/json");

    int code = http.POST((uint8_t *)body, body_len);
    http.end();

    bool ok = (code == 204 || (code >= 200 && code < 300));
    if (!ok) {
        ESP_LOGW(TAG, "POST failed code=%d", code);
        http_handle_error(code);
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
    t14_breaker_t *b = &s_post_breaker;

    bool transition = (b->known && (ok != b->last_ok));
    bool first_fail = (!ok && !b->streak_logged_fail);

    if (!b->known || transition || first_fail) {
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

    uint32_t now = (uint32_t)time(NULL);
    b->last_unix         = now;
    b->last_ok           = ok;
    b->known             = true;
    b->streak_logged_fail = !ok;

    /* gh#18 Phase 2 — feed the persistent circuit breaker. */
    breaker_record(b, ok, now, K_BK_POST_UNTIL, K_BK_POST_PHASE);
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
    /* gh#18 Phase 2 — circuit breaker. */
    if (breaker_open(&s_post_breaker, cfg->current_unix_ts)) { return false; }

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
    format_last(buf, cap, s_post_breaker.last_unix, s_post_breaker.last_ok,
                s_post_breaker.known);
}

void status_post_last_log_str(char *buf, size_t cap)
{
    format_last(buf, cap, s_log_breaker.last_unix, s_log_breaker.last_ok,
                s_log_breaker.known);
}

/* ============================================================
 * status_post_backoff_active() — gh#18 Phase 2 (since 1.17.35).
 *
 * Returns true when either breaker is in an open backoff window. The JSON
 * flag `mode.net_backoff_active` and the corresponding "Net backoff" web-
 * GUI badge are driven from this. Reader sees open_until_unix as a single
 * 32-bit field — racy with the writer in task_status_post() but coherent
 * (one store, one load; no torn intermediate value).
 *
 * Time source is time(NULL) — same source the breaker writes against in
 * breaker_record(). The pre-NTP guard inside breaker_open() makes this
 * safe to call before SNTP has synced (returns false in that window).
 * ============================================================ */
bool status_post_backoff_active(void)
{
    uint32_t now = (uint32_t)time(NULL);
    return breaker_open(&s_post_breaker, now) ||
           breaker_open(&s_log_breaker,  now);
}

/* ============================================================
 * Supervisor integration entry points (gh#18 Phase 4, since 1.18.0).
 * ============================================================ */

uint32_t status_post_heartbeat(void)
{
    return s_heartbeat;
}

uint32_t status_post_heap_drop_bytes(void)
{
    return s_heap_drop_bytes;
}

void status_post_force_teardown(void)
{
    /* Idempotent close-down of the persistent TLS session. Called by the
     * supervisor immediately before vTaskDelete(task_t14) so the next T14
     * incarnation inherits a fresh client rather than a half-closed socket. */
    if (s_secure_inited) {
        s_secure.stop();
        s_secure_inited = false;
    }
}

/* Helper used by the main task loop to sample heap deltas around HTTPS
 * calls. Keeps the supervisor wiring private to this translation unit. */
static void record_heap_drop(size_t before, size_t after)
{
    if (after < before) {
        uint32_t delta = (uint32_t)(before - after);
        /* Saturating add: protect the supervisor from rollover. */
        if (s_heap_drop_bytes + delta < s_heap_drop_bytes) {
            s_heap_drop_bytes = 0xFFFFFFFFu;
        } else {
            s_heap_drop_bytes += delta;
        }
    }
}

/* ============================================================
 * SDFileChunkedStream — Arduino Stream adapter over an SD-card CSV file.
 *
 * Replaces the pre-1.17.29 "malloc(fsize) + slurp + POST(buf, fsize)"
 * pattern. HTTPClient::sendRequest("POST", Stream*, size_t) pulls bytes
 * from the stream as needed, sending Content-Length: size up front so
 * the server still sees a properly framed request.
 *
 * Peak heap during the upload drops from up to 5 MB (the old PSRAM body)
 * to ~4 KB (the static chunk buffer below). The buffer is static —
 * deliberate: T14 only does one upload at a time, so the slot is reused
 * across uploads and not consumed from the heap.
 * ============================================================ */
class SDFileChunkedStream : public Stream {
public:
    SDFileChunkedStream(const char *path, uint32_t size)
        : m_size(size), m_pos(0), m_buf_pos(0), m_buf_len(0)
    {
        snprintf(m_path, sizeof(m_path), "%s", path);
    }

    /* Print (parent) — write not used; satisfy pure virtual. */
    size_t write(uint8_t) override               { return 0; }
    size_t write(const uint8_t *, size_t) override { return 0; }

    /* Stream interface — used by HTTPClient::sendRequest. */
    int available() override {
        return (int)((m_size - m_pos) + (m_buf_len - m_buf_pos));
    }

    int read() override {
        if (m_buf_pos >= m_buf_len && !refill()) { return -1; }
        return (unsigned char)s_chunk[m_buf_pos++];
    }

    int peek() override {
        if (m_buf_pos >= m_buf_len && !refill()) { return -1; }
        return (unsigned char)s_chunk[m_buf_pos];
    }

    /* Bulk-read path. HTTPClient::sendRequest prefers this over read()
     * one byte at a time — substantially faster for large transfers. */
    size_t readBytes(char *out, size_t len) override {
        size_t written = 0;
        while (written < len) {
            if (m_buf_pos >= m_buf_len && !refill()) { break; }
            size_t avail = m_buf_len - m_buf_pos;
            size_t take  = (avail < (len - written)) ? avail : (len - written);
            memcpy(out + written, s_chunk + m_buf_pos, take);
            m_buf_pos += take;
            written   += take;
        }
        return written;
    }

private:
    static char     s_chunk[T14_LOG_READ_CHUNK];   /* static: BSS, no heap */
    char            m_path[64];
    uint32_t        m_size;       /* total file size in bytes               */
    uint32_t        m_pos;        /* next SD-read offset                    */
    size_t          m_buf_pos;    /* read offset within s_chunk             */
    size_t          m_buf_len;    /* valid bytes in s_chunk                 */

    bool refill() {
        if (m_pos >= m_size) { return false; }
        uint32_t remaining = m_size - m_pos;
        size_t   want      = (remaining > (uint32_t)(sizeof(s_chunk) - 1u))
                              ? (sizeof(s_chunk) - 1u) : (size_t)remaining;
        size_t got = 0;
        storage_status_t rc = storage_sd_read(m_path, m_pos, s_chunk,
                                              want + 1u, &got);
        if (rc != STORAGE_OK || got == 0u) {
            ESP_LOGW(TAG, "log upload: stream refill failed at offset %lu (rc=%d)",
                     (unsigned long)m_pos, (int)rc);
            return false;
        }
        m_pos    += got;
        m_buf_pos = 0;
        m_buf_len = got;
        return true;
    }
};

/* Definition of the static chunk buffer. */
char SDFileChunkedStream::s_chunk[T14_LOG_READ_CHUNK];

/* ============================================================
 * HTTP POST helper for log uploads (?action=log) — since 1.17.29.
 *
 * Streams the SD file via SDFileChunkedStream so peak heap use is the
 * 4 KB chunk buffer instead of the entire file. Content-Length is set
 * from the file size up front; HTTPClient::sendRequest pulls bytes from
 * the stream as it sends. Works identically for http:// and https://.
 * ============================================================ */
static bool do_log_upload(const cfg_shadow_t *cfg, const char *filename)
{
    if (filename == NULL || filename[0] == '\0') { return false; }

    /* Build absolute path; storage_sd_* expects a leading slash. */
    char path[32];
    snprintf(path, sizeof(path), "/%s", filename);

    uint32_t fsize = storage_sd_file_size(path);
    if (fsize == 0u || fsize > T14_LOG_MAX_BYTES) {
        ESP_LOGW(TAG, "log upload: size %lu out of bounds for %s",
                 (unsigned long)fsize, filename);
        return false;
    }

    /* Build target URL with the ?action=log query param. */
    char url[CFG_MAX_URL_LEN + 16] = {};
    snprintf(url, sizeof(url), "%s?action=log", cfg->status_url);

    HTTPClient http;
    if (!http_open_for(cfg, &http, url, T14_LOG_HTTP_TIMEOUT_MS)) {
        ESP_LOGW(TAG, "log upload: http_open_for failed");
        s_secure.stop();
        s_secure_inited = false;
        return false;
    }
    http.addHeader("Content-Type", "text/plain");

    /* Stream-driven POST. The Stream lives on T14's stack; the chunk
     * buffer it reads through is static BSS. Peak heap added: ~0. */
    SDFileChunkedStream stream(path, fsize);
    int code = http.sendRequest("POST", &stream, (size_t)fsize);
    http.end();

    bool ok = (code == 204 || (code >= 200 && code < 300));
    if (ok) {
        ESP_LOGI(TAG, "log upload OK: %s (%lu bytes)", filename,
                 (unsigned long)fsize);
    } else {
        ESP_LOGW(TAG, "log upload FAILED: %s code=%d", filename, code);
        http_handle_error(code);
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

    uint32_t now = (uint32_t)time(NULL);
    s_log_breaker.last_unix          = now;
    s_log_breaker.last_ok            = ok;
    s_log_breaker.known              = true;
    s_log_breaker.streak_logged_fail = !ok;

    /* gh#18 Phase 2 — feed the persistent circuit breaker. */
    breaker_record(&s_log_breaker, ok, now, K_BK_LOG_UNTIL, K_BK_LOG_PHASE);
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
    /* gh#18 Phase 2 — circuit breaker (independent of the status-post breaker). */
    else if (breaker_open(&s_log_breaker, cfg->current_unix_ts))   preconditions_ok = false;

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

    /* gh#18 Phase 2 — recover breaker state from NVS. If a previous boot left
     * the breaker open (open_until_unix in the future) we honour that window;
     * if NTP has not yet synced when ready_to_post() / maybe_upload_log() are
     * evaluated, breaker_open() returns false anyway and the pre-NTP guard
     * already blocks the attempt for an orthogonal reason. */
    breaker_load(&s_post_breaker, K_BK_POST_UNTIL, K_BK_POST_PHASE);
    breaker_load(&s_log_breaker,  K_BK_LOG_UNTIL,  K_BK_LOG_PHASE);
    if (s_post_breaker.hold_phase != 0u || s_post_breaker.open_until_unix != 0u ||
        s_log_breaker.hold_phase  != 0u || s_log_breaker.open_until_unix  != 0u) {
        ESP_LOGI(TAG, "breaker recovered: post phase=%u until=%lu, log phase=%u until=%lu",
                 (unsigned)s_post_breaker.hold_phase,
                 (unsigned long)s_post_breaker.open_until_unix,
                 (unsigned)s_log_breaker.hold_phase,
                 (unsigned long)s_log_breaker.open_until_unix);
    }

    /* JSON buffer in BSS — sized once, reused every cycle, no heap churn. */
    static char json_buf[T14_JSON_BUF_BYTES];

    for (;;) {
        /* gh#18 Phase 4 — supervisor heartbeat. Advanced unconditionally on
         * every iteration so a wedged HTTPS call (the failure mode we are
         * trying to detect) is the only thing that can freeze it. */
        s_heartbeat++;

        cfg_shadow_t cfg;
        dm_cfg_snapshot(&cfg);

        /* WiFi-disconnect edge detection (gh#18 Phase 1).
         * The persistent TLS client (s_secure) holds an open TCP socket
         * across status-POST calls — when the WiFi link drops, that socket
         * is dead. Detect the falling edge and tear down explicitly so the
         * next attempt re-handshakes on the new association instead of
         * silently hanging on a dead descriptor for the full HTTP timeout. */
        bool wifi_now = WiFi.isConnected();
        if (s_wifi_was_connected && !wifi_now && s_secure_inited) {
            s_secure.stop();
            s_secure_inited = false;
        }
        s_wifi_was_connected = wifi_now;

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
                /* gh#18 Phase 4 — sample heap free immediately around the
                 * HTTPS call. Real leaks accumulate; transient handshake
                 * allocations release back to free by the time the call
                 * returns. record_heap_drop() clamps negatives to zero. */
                size_t heap_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                ok = do_status_post(&cfg, json_buf, n);
                size_t heap_after  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
                record_heap_drop(heap_before, heap_after);
            } else {
                ESP_LOGW(TAG, "JSON build failed");
            }
            log_post_outcome(ok);
            s_last_post_tick = xTaskGetTickCount();
        }

        /* gh#18 Phase 4 — same heap-drop sampling around the log-upload
         * (less frequent than status POST but the body is much larger;
         * a leak here would be the dominant supervisor-respawn trigger
         * under sustained outage). */
        size_t heap_before_log = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        maybe_upload_log(&cfg);
        size_t heap_after_log  = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
        record_heap_drop(heap_before_log, heap_after_log);

        vTaskDelay(pdMS_TO_TICKS(T14_TICK_MS));
    }
}
