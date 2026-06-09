/**
 * @file network_manager.cpp
 * @brief T10 — Network Manager task (Phase 6.N.1, minimal).
 *
 * **alpha.6.14 minimal-T10 status** (2026-05-18):
 *
 * The original 1.20.3 file (720 lines, Arduino-WiFi.h + Arduino-HTTPClient
 * based) is archived alongside this file as `network_manager_1.20.3_original
 * .cpp.archived`. Per the migration plan, the 1.20.3 state machine
 * (`WiFi.begin / WL_CONNECTED / configTime / WiFi.softAP / HTTPClient geo
 * lookup`) was always destined for a from-scratch IDF rewrite — the
 * arduino-WiFi APIs don't map 1:1 to esp_wifi/esp_netif/esp_event.
 *
 * This minimal T10 implements the **operationally essential** subset:
 *  1. Posts `net_status_t` to Q5 (consumed by T8's LCD WiFi status page).
 *  2. Sends `DM_NOTIFY_NTP_SYNCED` (TN4) to T4 after the initial NTP sync,
 *     so T4 writes the post-SNTP system time back to the DS1307 RTC.
 *  3. Polls esp_netif/esp_wifi state every NET_POLL_MS, re-posts Q5 on
 *     any change.
 *
 * **Landed since the original minimal-T10:**
 *  - **IP geolocation + timezone sync** (alpha.6.28 / Phase 6.14.X step 1)
 *    via `http://ip-api.com/json` + the verbatim 1.20.3 IANA→POSIX table.
 *  - **AP+STA simultaneous mode** (alpha.6.29 / Phase 6.14.X step 2) —
 *    operator-toggled, NOT a fail-state fallback. SSID `Greenhouse-XXYY`
 *    (last 2 bytes of MAC); PSK from NVS `wifi/ap_psk` or factory default
 *    `0123456789`; enable flag NVS `wifi/ap_enable` polled every
 *    `NET_POLL_MS`; auto-shutdown after `cfg.ap_timeout_min` minutes
 *    (0 = never). The earlier alpha.6.14 header called this "AP fallback"
 *    which was a misreading — 1.20.3 ran AP alongside STA whenever the
 *    operator enabled it (typically for first-time WiFi setup or a wrong-
 *    credentials recovery path; the operator joins the AP, opens the GUI,
 *    sets the real SSID/PSK, then turns the AP off again).
 *
 * **Still deferred to 2.0.1 / 2.1.x:**
 *  - **Exponential backoff state machine** (2 → 4 → 8 → 16 → 32 → 60 s).
 *    esp_wifi's internal reconnect retries at a fixed cadence; sufficient
 *    for the soak.
 *  - **Periodic 24 h NTP resync.** The DS1307 RTC is precise enough for
 *    multi-day operation; the SD-card daily logs and T4 RTC-readback every
 *    minute provide enough monitoring. Add periodic resync if Phase 7
 *    soak shows drift.
 *
 * **Dependencies in place**:
 *  - `nm_wifi_init_blocking()` — boot-time WiFi STA bring-up + SNTP sync.
 *    Defined in SECTION A of this file (folded from the retired
 *    `wifi_tickle.cpp` in 2.0.0-rc.1.3). Called from main.cpp BEFORE
 *    `task_network_manager` spawns, so esp_wifi is already initialised +
 *    connected + SNTP synced by the time T10's monitoring loop starts.
 *    Same event-handler / reconnect-timer semantics as the legacy
 *    wifi_tickle — only the file location changed.
 *  - `Q5` queue (depth 1, xQueueOverwrite) created by `system_globals_init`.
 *  - `task_t4` handle populated by alpha.6.7 spawn.
 *  - `task_t10` handle declared in `system_globals.cpp`, populated by
 *    main.cpp's spawn block (named app_main_stub.cpp through alpha.6.22).
 *
 * @author  Greenhouse Controller project
 */

#include <stdlib.h>   /* atof (alpha.6.28 — geo sync) */
#include <string.h>
#include <stdio.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"        /* rc.1.3 — folded from wifi_tickle */
#include "freertos/timers.h"              /* rc.1.3 — folded from wifi_tickle (reconnect backoff) */

#include "esp_log.h"
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"                    /* rc.1.3 — folded from wifi_tickle */
#include "esp_netif.h"
#include "esp_netif_sntp.h"               /* rc.1.3 — folded from wifi_tickle (esp_netif_sntp_*) */
#include "esp_mac.h"                      /* alpha.6.29 — esp_read_mac for AP SSID */
#include "esp_http_client.h"   /* alpha.6.28 — ip-api.com lookup */
#include "esp_sntp.h"                     /* a.6.33 — periodic 24 h NTP resync */
#include "esp_timer.h"                    /* a.6.33 — esp_timer_get_time for resync cadence */
#include "nvs_flash.h"                    /* rc.1.3 — folded from wifi_tickle */
#include "lwip/ip4_addr.h"

#include "network_manager.h"
#include "../types/app_types.h"           /* Q4, Q5, net_status_t, task_t4 */
#include "../data_manager/data_manager.h" /* DM_NOTIFY_NTP_SYNCED, dm_cfg_snapshot */
#include "../event_logger/event_logger.h" /* a.6.33 — log_post + log_event_t */
#include "nvs_config.h"                   /* alpha.6.28 — NVS_NS_SYSTEM, NVS_NS_WIFI, nvs_cfg_set_str */
#include "tz_table.h"                     /* alpha.6.28 — iana_to_posix() */

static const char *TAG = "T10_NET";

/* ============================================================================
 * SECTION A — Boot-time WiFi init (folded from wifi_tickle.cpp in rc.1.3)
 *
 * This section owns the blocking boot-time WiFi STA bring-up that used to
 * live in firmware/src/wifi_tickle.cpp (alpha.3.2). The function lives in
 * T10's home because T10 already owns the long-running WiFi-state monitor;
 * splitting bring-up across two files was an alpha-era scaffold seam that
 * no longer serves a purpose. Identical event-handler / reconnect-timer
 * semantics — the file move is the only change.
 *
 * The WIFI_EVENT/IP_EVENT handler registered here STAYS REGISTERED after
 * `nm_wifi_init_blocking()` returns and continues to drive all subsequent
 * disconnect events via the exponential-backoff `s_reconnect_timer`
 * (alpha.6.31). `task_network_manager` below assumes this section has run
 * to completion before it starts; main.cpp calls `nm_wifi_init_blocking()`
 * BEFORE spawning T10.
 * ============================================================================ */

/* Event-group bits driven by the WiFi/IP event handlers. */
#define BIT_GOT_IP        (1u << 0)
#define BIT_DISCONNECTED  (1u << 1)

static EventGroupHandle_t  s_wifi_init_evt   = NULL;
static esp_netif_t        *s_sta_netif       = NULL;
static int                 s_retry_count     = 0;   /* boot-time tickle's 3-shot fast path */
static const int           kMaxRetries       = 3;
static char                s_last_ip[16]     = {0}; /* "xxx.xxx.xxx.xxx" + NUL */

/* alpha.6.31 — STA reconnect with infinite retry + exponential back-off.
 * The wifi event handler kicks a one-shot timer on each disconnect; the
 * timer expires and calls esp_wifi_connect(), retry budget never runs out.
 * Back-off ladder mirrors the 1.20.3 design (2 → 4 → 8 → 16 → 32 → 60 s
 * cap) so a permanently-down AP doesn't hammer the radio at full rate.
 * Reset to BACKOFF_INIT_MS on every successful STA_GOT_IP. */
#define BACKOFF_INIT_MS    2000u
#define BACKOFF_MAX_MS    60000u

static TimerHandle_t s_reconnect_timer    = NULL;
static uint32_t      s_reconnect_delay_ms = BACKOFF_INIT_MS;

/**
 * @brief FreeRTOS one-shot timer callback — fires `esp_wifi_connect()` and
 *        bumps the exponential-backoff counter for the next attempt.
 *
 * Runs in the FreeRTOS timer-service task. Cheap (single esp_wifi_connect()
 * call); does NOT block. The timer is created and (re-)armed by
 * `nm_schedule_reconnect()` below, which is invoked from the
 * WIFI_EVENT_STA_DISCONNECTED handler after the 3-shot fast-retry budget
 * is exhausted.
 *
 * Back-off ladder: 2 → 4 → 8 → 16 → 32 → 60 s cap. Reset to
 * `BACKOFF_INIT_MS` on the next successful `IP_EVENT_STA_GOT_IP`.
 *
 * @param  xTimer  Unused — the callback is per-handle but the handle isn't
 *                 needed here.
 * @warning Runs in the timer-service task context (small stack, ~2 KB).
 *          Do NOT do anything heavyweight here — `esp_wifi_connect()` is
 *          deliberately light. Heavy work (TLS handshakes etc.) belongs in
 *          a dedicated task spawned by the disconnect handler.
 */
static void nm_reconnect_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    ESP_LOGI(TAG, "Reconnect attempt (back-off was %lu ms)",
             (unsigned long)s_reconnect_delay_ms);
    esp_wifi_connect();
    /* Bump the back-off for the *next* drop. Reset on STA_GOT_IP. */
    s_reconnect_delay_ms <<= 1;
    if (s_reconnect_delay_ms > BACKOFF_MAX_MS) {
        s_reconnect_delay_ms = BACKOFF_MAX_MS;
    }
}

/**
 * @brief Arm (or re-arm) the reconnect one-shot timer for the next attempt.
 *
 * Creates `s_reconnect_timer` lazily on first call; subsequent calls reuse
 * the same handle. Uses `xTimerChangePeriod` to set the next firing delay
 * (= `s_reconnect_delay_ms`) and to (re-)start the timer in one shot.
 *
 * Called from the WIFI_EVENT_STA_DISCONNECTED handler after the 3-shot
 * fast-retry budget is exhausted. Subsequent disconnects within the same
 * boot reuse the same timer with the back-off-bumped delay.
 *
 * @note   The fallback path (xTimerCreate failure) calls `esp_wifi_connect()`
 *         immediately so the radio at least tries — better than silently
 *         giving up forever.
 * @see    nm_reconnect_timer_cb() — the registered callback.
 */
static void nm_schedule_reconnect(void)
{
    if (s_reconnect_timer == NULL) {
        s_reconnect_timer = xTimerCreate("wifi_reconnect",
                                         pdMS_TO_TICKS(s_reconnect_delay_ms),
                                         pdFALSE, NULL, nm_reconnect_timer_cb);
        if (s_reconnect_timer == NULL) {
            ESP_LOGE(TAG, "xTimerCreate(wifi_reconnect) failed — "
                          "falling back to immediate retry");
            esp_wifi_connect();
            return;
        }
    }
    /* xTimerChangePeriod auto-starts the timer. */
    xTimerChangePeriod(s_reconnect_timer,
                       pdMS_TO_TICKS(s_reconnect_delay_ms), 0);
}

/**
 * @brief Unified handler for `WIFI_EVENT` and `IP_EVENT` (both bases).
 *
 * Registered ONCE during `nm_wifi_init_blocking()` against the default
 * event loop. Stays alive after init returns — drives all subsequent
 * reconnect cycles via the exponential-backoff `s_reconnect_timer`.
 *
 * Event handling:
 *  - `WIFI_EVENT_STA_START` → calls `esp_wifi_connect()`. Doing this from
 *    the event handler (rather than polling) is the structural fix for
 *    gh#21 — the lwIP stack is already up by the time STA_START fires.
 *  - `WIFI_EVENT_STA_DISCONNECTED` → first 3 retries are immediate via
 *    `esp_wifi_connect()`. After that, signals BIT_DISCONNECTED on the
 *    boot-time event group (releasing `nm_wifi_init_blocking()` from its
 *    wait), then hands off to the back-off timer for indefinite retry.
 *  - `IP_EVENT_STA_GOT_IP` → snprintfs the IP into `s_last_ip`, resets the
 *    retry counter + back-off ladder, signals BIT_GOT_IP.
 *  - `IP_EVENT_STA_LOST_IP` → logged only; no state mutation. lwIP usually
 *    recovers via STA_DISCONNECTED + STA_GOT_IP roundtrip.
 *
 * @param  arg          Unused — single-handler, no user data needed.
 * @param  event_base   `WIFI_EVENT` or `IP_EVENT`.
 * @param  event_id     Specific event constant (WIFI_EVENT_STA_START, etc.).
 * @param  event_data   Payload pointer (cast based on event_id).
 * @warning Runs in the event-loop task (priority 20 by default, stack
 *          2304 B). Avoid heavyweight work; the handler should signal +
 *          schedule, never block.
 */
static void nm_wifi_event_handler(void *arg, esp_event_base_t event_base,
                                  int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                /* esp_wifi_start() completed — initiate the connect attempt.
                 * Doing this from the event handler (rather than polling
                 * WiFi.status() == WL_DISCONNECTED) is the structural fix
                 * for gh#21: the lwip stack is already initialised by the
                 * time STA_START fires, so esp_wifi_connect() can't race
                 * against the tcpip_adapter setup. */
                ESP_LOGI(TAG, "WIFI_EVENT_STA_START — calling esp_wifi_connect()");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED: {
                /* Boot-time tickle: kMaxRetries fast attempts so the boot
                 * gate doesn't block forever on a missing AP. After that
                 * we hand off to the back-off timer for indefinite retry.
                 * alpha.6.31 — no terminal "give up". */
                wifi_event_sta_disconnected_t *disc =
                    (wifi_event_sta_disconnected_t *)event_data;
                ESP_LOGW(TAG, "WIFI_EVENT_STA_DISCONNECTED reason=%d retry=%d/%d",
                         (int)disc->reason, s_retry_count, kMaxRetries);

                if (s_retry_count < kMaxRetries) {
                    s_retry_count++;
                    esp_wifi_connect();
                } else {
                    xEventGroupSetBits(s_wifi_init_evt, BIT_DISCONNECTED);
                    nm_schedule_reconnect();
                }
                break;
            }

            default:
                ESP_LOGD(TAG, "WIFI_EVENT id=%ld", (long)event_id);
                break;
        }
    } else if (event_base == IP_EVENT) {
        switch (event_id) {
            case IP_EVENT_STA_GOT_IP: {
                ip_event_got_ip_t *ev = (ip_event_got_ip_t *)event_data;
                snprintf(s_last_ip, sizeof(s_last_ip), IPSTR, IP2STR(&ev->ip_info.ip));
                ESP_LOGI(TAG, "IP_EVENT_STA_GOT_IP ip=%s gw=" IPSTR " netmask=" IPSTR,
                         s_last_ip,
                         IP2STR(&ev->ip_info.gw),
                         IP2STR(&ev->ip_info.netmask));
                s_retry_count        = 0;
                s_reconnect_delay_ms = BACKOFF_INIT_MS;
                xEventGroupSetBits(s_wifi_init_evt, BIT_GOT_IP);
                break;
            }

            case IP_EVENT_STA_LOST_IP:
                ESP_LOGW(TAG, "IP_EVENT_STA_LOST_IP");
                break;

            default:
                ESP_LOGD(TAG, "IP_EVENT id=%ld", (long)event_id);
                break;
        }
    }
}

/** rc.1.5.3 — explicit SNTP-completed latch. Replaces the prior
 *  `time(NULL) > NTP_MIN_EPOCH` heuristic in snapshot_state(). That
 *  heuristic was correct when the system clock was always 0 at boot, but
 *  T4's RTC pre-seed (data_manager.cpp:settimeofday from DS1307, around
 *  boot+500ms) now primes the clock with a plausible 2026 epoch before
 *  T10 ever takes a snapshot — so the heuristic produced a false-positive
 *  "NTP synced" on any unit with a battery-backed RTC and no internet.
 *
 *  This flag is set only when ESP-IDF's `esp_sntp_get_sync_status()`
 *  actually reports `SNTP_SYNC_STATUS_COMPLETED` — once from
 *  `nm_sntp_quick_sync()` at boot, and again from `run_ntp_resync()` on
 *  the periodic 24 h cadence. Monotonic-rising for the boot: once SNTP
 *  has synced, the indicator stays "NTP" even across STA disconnect /
 *  reconnect — the wall clock remains trustworthy and drift is bounded
 *  by the next successful resync. Cleared only by reboot. */
static bool s_sntp_synced = false;

/* rc.1.5.4 — public accessor so data_manager.cpp's status snapshot can
 * read the same flag the LCD does via snapshot_state(). Without this,
 * two surfaces would disagree: the LCD path goes through snapshot_state
 * (correct) but the web GUI path went through `cfg.current_unix_ts >
 * 1700000000UL` (the original time-comparison heuristic), and on a unit
 * with a battery-backed RTC the web GUI showed "NTP synced" while the
 * LCD correctly showed "RTC". Single-line wrapper kept here next to the
 * declaration so future readers see the relationship immediately. */
extern "C" bool nm_is_sntp_synced(void)
{
    return s_sntp_synced;
}

/**
 * @brief Best-effort SNTP synchronisation against pool.ntp.org.
 *
 * Initialises the IDF v5 `esp_netif_sntp_*` module, then polls
 * `esp_sntp_get_sync_status()` every 100 ms for up to 10 s waiting for
 * `SNTP_SYNC_STATUS_COMPLETED`. On success or timeout, deinits the SNTP
 * module so the next call starts clean.
 *
 * rc.1.5.3 — switched the success gate from the prior `time(NULL) >
 * MIN_EPOCH` heuristic to the canonical IDF status query. T4's RTC
 * pre-seed (data_manager.cpp:settimeofday from DS1307, ~boot+500ms)
 * primes the system clock with a plausible 2026 epoch before this
 * function runs, so the old heuristic returned true on iteration 0 —
 * before SNTP could send a single packet — and immediately tore the
 * client down via the deinit below. The status-query approach matches
 * what `run_ntp_resync()` has always done and reflects whether ESP-IDF
 * actually received a sync response. On success we also latch
 * `s_sntp_synced` so `snapshot_state()` can drive the NTP/RTC indicator
 * off real evidence rather than the RTC-primed wall clock.
 *
 * @return `true` if SNTP completed within the 10 s budget; `false` if it
 *         timed out. A `false` return is non-fatal — the DS1307 RTC holds
 *         the last-known wall clock and the periodic 24 h NTP resync
 *         (`run_ntp_resync()`) will retry.
 * @note   The 10 s budget covers DNS resolution (1-2 s on residential
 *         gateways) + UDP/123 RTT + the few SNTP packets needed to converge.
 * @note   Blocks the calling task for up to 10 s. Only called once, from
 *         `nm_wifi_init_blocking()`, before the task graph spawns — never
 *         from inside a running task.
 * @see    run_ntp_resync() — the long-running 24 h resync path.
 * @see    s_sntp_synced   — the module-scope latch this function sets on success.
 */
static bool nm_sntp_quick_sync(void)
{
    ESP_LOGI(TAG, "Starting SNTP (pool.ntp.org)");
    esp_sntp_config_t sntp_cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&sntp_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_netif_sntp_init failed: %s", esp_err_to_name(err));
        return false;
    }

    /* rc.1.5.3 — gate on esp_sntp_get_sync_status() == COMPLETED rather
     * than on `time(NULL) > MIN_EPOCH`. The latter is fooled by T4's
     * RTC pre-seed: the clock is already plausible at boot, so the old
     * loop succeeded in 0 ms without ever waiting for an SNTP response. */
    const int NTP_POLL_ITERS = 100;   /* 100 × 100 ms = 10 s */
    bool synced = false;
    for (int i = 0; i < NTP_POLL_ITERS; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            time_t now = time(NULL);
            ESP_LOGI(TAG, "SNTP synced after %d ms — epoch=%ld", i * 100, (long)now);
            synced = true;
            break;
        }
    }

    if (synced) {
        /* Latch — snapshot_state() reads this; monotonic-rising for the boot. */
        s_sntp_synced = true;
    } else {
        ESP_LOGW(TAG, "SNTP did not complete in budget");
    }

    esp_netif_sntp_deinit();
    return synced;
}

nm_wifi_status_t nm_wifi_init_blocking(uint32_t connect_timeout_ms)
{
    /* Step 1: read credentials from NVS. */
    char ssid[64] = {0};
    char psk[64]  = {0};
    nvs_cfg_get_str(NVS_NS_WIFI, "ssid", ssid, sizeof(ssid));
    nvs_cfg_get_str(NVS_NS_WIFI, "psk",  psk,  sizeof(psk));

    const bool have_sta_creds = (ssid[0] != '\0');
    if (have_sta_creds) {
        ESP_LOGI(TAG, "NVS credentials: ssid='%s' psk=%s",
                 ssid, psk[0] ? "***(set)" : "(empty)");
    } else {
        /* alpha.6.30 — no SSID does NOT short-circuit stack init. T10's AP
         * mode needs esp_wifi_init/_start to have happened even when no
         * STA credentials are configured (recovery flow). */
        ESP_LOGI(TAG, "no SSID in NVS — WiFi stack will init but skip STA-connect");
    }

    s_wifi_init_evt = xEventGroupCreate();
    s_retry_count   = 0;
    s_last_ip[0]    = '\0';
    if (s_wifi_init_evt == NULL) {
        ESP_LOGE(TAG, "xEventGroupCreate failed");
        return NM_WIFI_INIT_FAILED;
    }

    esp_err_t err;
    err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_event_loop_create_default: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }

    if (s_sta_netif == NULL) {
        s_sta_netif = esp_netif_create_default_wifi_sta();
        if (s_sta_netif == NULL) {
            ESP_LOGE(TAG, "esp_netif_create_default_wifi_sta returned NULL");
            return NM_WIFI_INIT_FAILED;
        }
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&init_cfg);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_wifi_init: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }

    err = esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                               &nm_wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT handler: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }
    err = esp_event_handler_instance_register(IP_EVENT, ESP_EVENT_ANY_ID,
                                               &nm_wifi_event_handler, NULL, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT handler: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }

    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_set_mode(STA): %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }

    if (have_sta_creds) {
        wifi_config_t cfg = {};
        strncpy((char *)cfg.sta.ssid,     ssid, sizeof(cfg.sta.ssid)     - 1);
        strncpy((char *)cfg.sta.password, psk,  sizeof(cfg.sta.password) - 1);
        cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        cfg.sta.pmf_cfg.capable    = true;
        cfg.sta.pmf_cfg.required   = false;
        err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_wifi_set_config: %s", esp_err_to_name(err));
            return NM_WIFI_INIT_FAILED;
        }
    }

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start: %s", esp_err_to_name(err));
        return NM_WIFI_INIT_FAILED;
    }

    if (!have_sta_creds) {
        ESP_LOGI(TAG, "WiFi stack up; STA-connect skipped (no SSID) — ready for AP mode");
        return NM_WIFI_NO_SSID;
    }

    ESP_LOGI(TAG, "esp_wifi_start OK — waiting up to %lu ms for STA_GOT_IP",
             (unsigned long)connect_timeout_ms);

    EventBits_t bits = xEventGroupWaitBits(s_wifi_init_evt,
                                           BIT_GOT_IP | BIT_DISCONNECTED,
                                           pdTRUE,   /* clear on exit */
                                           pdFALSE,  /* OR semantics */
                                           pdMS_TO_TICKS(connect_timeout_ms));

    if (bits & BIT_GOT_IP) {
        ESP_LOGI(TAG, "WiFi init: STA up, IP=%s", s_last_ip);
        bool ntp_ok = nm_sntp_quick_sync();
        return ntp_ok ? NM_WIFI_OK : NM_WIFI_OK_NO_NTP;
    }

    if (bits & BIT_DISCONNECTED) {
        ESP_LOGW(TAG, "WiFi init: gave up after %d retries (reason in logs above)",
                 kMaxRetries);
        return NM_WIFI_DISCONNECTED;
    }

    ESP_LOGW(TAG, "WiFi init: STA_GOT_IP not received within %lu ms",
             (unsigned long)connect_timeout_ms);
    return NM_WIFI_CONNECT_TIMEOUT;
}

/* ============================================================================
 * SECTION B — Long-running T10 task (original network_manager content)
 * ============================================================================ */

/** AP-mode latch (alpha.6.29). Forward-declared here because snapshot_state
 *  below reads it. The rest of the AP module-level state + helper functions
 *  live in their own block further down. */
static bool s_ap_active = false;

/** Main-loop tick (ms). Mirrors 1.20.3's NET_POLL_MS for behavioural parity. */
#define NET_POLL_MS  5000u

/** Plausibility threshold for "NTP-synced wall clock" (2023-11-14). */
#define NTP_MIN_EPOCH  ((time_t)1700000000)

/* a.6.33 — periodic 24 h NTP resync.
 *
 * Tracks the timestamp (in esp_timer_get_time microseconds) of the last
 * successful NTP sync — whether boot-time via wifi_tickle, or periodic via
 * run_ntp_resync() below. The check fires inside the main loop: when STA
 * is connected and the elapsed since last sync exceeds NTP_RESYNC_INTERVAL,
 * a single SNTP cycle is kicked off. Geo is NOT re-fetched (location is
 * stable; matches 1.20.3).
 *
 * NTP_RESYNC_INTERVAL_S = 86400 (24 h) for production. For verification
 * builds the symbol may be redefined at compile time via -D.
 */
#ifndef NTP_RESYNC_INTERVAL_S
#define NTP_RESYNC_INTERVAL_S  86400u
#endif
#define NTP_RESYNC_INTERVAL_US ((uint64_t)NTP_RESYNC_INTERVAL_S * 1000000ULL)

/* rc.1.5.6 — retry cadence when boot SNTP failed. Until rc.1.5.6 the
 * periodic resync (above) was gated on `s_last_ntp_sync_us != 0`, so a
 * unit whose boot-time `nm_sntp_quick_sync()` timed out was stranded on
 * RTC for the rest of the boot — the 24 h timer never started. The fix
 * runs the resync on this shorter cadence while `s_last_ntp_sync_us`
 * is still zero, then falls back to the 24 h cadence once the first
 * sync lands. 5 min is short enough to recover within an operator
 * coffee break but long enough to avoid hammering pool.ntp.org under
 * a persistent DNS/UDP outage. Override at compile time with -D. */
#ifndef NTP_RETRY_INTERVAL_S
#define NTP_RETRY_INTERVAL_S   300u
#endif
#define NTP_RETRY_INTERVAL_US  ((uint64_t)NTP_RETRY_INTERVAL_S * 1000000ULL)

static int64_t s_last_ntp_sync_us    = 0;   /* 0 = never synced this boot */
/* rc.1.5.6 — last attempt timestamp (any SNTP try, success or fail).
 * Used by the retry-cadence branch when s_last_ntp_sync_us is still
 * zero. Seeded at T10 task start so the first retry waits a full
 * NTP_RETRY_INTERVAL_S from there, not from boot — avoids an immediate
 * back-to-back SNTP attempt right after the boot-time quick_sync has
 * already tried (and possibly already failed). */
static int64_t s_last_ntp_attempt_us = 0;
/* s_sntp_synced is declared earlier in the file (before nm_sntp_quick_sync,
 * which is its first reader). Kept up there because that function precedes
 * this point textually; the comment block lives next to the declaration. */

/** Post a single LOG_SYSTEM event to Q3 (T9 drains to SD CSV).
 *  a.6.33 — used by start_ap / stop_ap / do_geo_sync to log audit events
 *  with value_a=3 (AP, value_b=1 start / 0 stop) and value_a=4 (geo,
 *  value_b=1 success). Matches the 1.20.3 LOG_SYSTEM value_a encoding
 *  documented in event_logger.h. */
/**
 * @brief Post a SYSTEM event with the given (value_a, value_b) payload to Q3.
 *
 * Thin wrapper around `log_post()` that fills the timestamp + event_type +
 * initiator fields canonically (`LOG_SYSTEM` / `LOG_BY_SYSTEM`). Caller
 * provides the discriminating value_a/value_b per the LOG_SYSTEM subtype
 * table in `event_logger.h`.
 *
 * @param value_a SYSTEM-event subtype (1=STA up/down, 2=NTP synced/timeout,
 *                3=AP up/down, etc.). See `event_logger.h` LOG_SYSTEM table.
 * @param value_b Payload for the chosen subtype (1/0 booleans, or KB
 *                metrics for heap subtypes, etc.).
 * @note Non-blocking — Q3 has overflow accounting. If Q3 is full the event
 *       is dropped (T9 accumulates the drop count separately).
 */
static void log_sys(int16_t value_a, int16_t value_b)
{
    log_event_t ev = {};
    ev.timestamp  = (uint32_t)time(NULL);
    ev.event_type = (uint8_t)LOG_SYSTEM;
    ev.initiator  = (uint8_t)LOG_BY_SYSTEM;
    ev.value_a    = value_a;
    ev.value_b    = value_b;
    log_post(&ev);
}

/**
 * @brief Build a `net_status_t` snapshot from the current esp_wifi/esp_netif state.
 *
 * Captures the four fields T8's LCD WiFi page renders, in a single
 * point-in-time sample. Called every NET_POLL_MS from the main loop;
 * cheap (no syscalls beyond `esp_wifi_sta_get_ap_info` + one netif lookup).
 *
 * Field semantics:
 *  - `client_connected` = `esp_wifi_sta_get_ap_info()` succeeded (we have
 *    a live STA association with the AP).
 *  - `ap_active`        = reflects the module-level `s_ap_active` latch
 *    set by `start_ap()`/`stop_ap()` — single-task-owned by T10.
 *  - `ntp_synced`       = `s_sntp_synced` — set only when ESP-IDF reports
 *    `SNTP_SYNC_STATUS_COMPLETED` from `nm_sntp_quick_sync()` or
 *    `run_ntp_resync()`. rc.1.5.3 replaced the prior
 *    `time(NULL) > NTP_MIN_EPOCH` heuristic, which produced a
 *    false-positive on every boot once T4's DS1307 RTC pre-seed primed the
 *    system clock with a plausible 2026 epoch (LCD showed "NTP" with no
 *    internet, audit log emitted a spurious NTP-synced row).
 *  - `ip_str`           = STA netif IPv4 as dotted-decimal, or "" if not up.
 *
 * @param[out] out  Caller-owned struct; zero-initialised before being
 *                  populated.
 * @warning Reads `s_ap_active` without locking — safe because T10 is the
 *          sole writer (start_ap/stop_ap happen on the T10 task only) and
 *          this function is called only from the T10 task.
 * @see Q5 — consumer queue (depth 1, xQueueOverwrite semantics).
 */
static void snapshot_state(net_status_t *out)
{
    memset(out, 0, sizeof(*out));

    /* STA connection state. */
    wifi_ap_record_t ap = {};
    out->client_connected = (esp_wifi_sta_get_ap_info(&ap) == ESP_OK);

    /* AP state — alpha.6.29: reflects start_ap/stop_ap. The module-level
     * latch (`s_ap_active`) is single-task-owned by T10 so a direct read is
     * safe; no need to query esp_wifi_get_mode here. */
    out->ap_active = s_ap_active;

    /* NTP sync state. rc.1.5.3 — read the explicit s_sntp_synced latch
     * instead of inferring from `time(NULL) > NTP_MIN_EPOCH`. T4's RTC
     * pre-seed now makes the system clock plausible at boot, so the old
     * heuristic could not distinguish "SNTP succeeded" from "RTC battery
     * is alive". See the s_sntp_synced declaration near the top of this
     * file for the full rationale. */
    out->ntp_synced = s_sntp_synced;

    /* IP address — query the default STA netif. */
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif != NULL) {
        esp_netif_ip_info_t ip = {};
        if (esp_netif_get_ip_info(netif, &ip) == ESP_OK &&
            ip.ip.addr != 0) {
            snprintf(out->ip_str, sizeof(out->ip_str), IPSTR, IP2STR(&ip.ip));
        }
    }
}

/**
 * @brief Compare two `net_status_t` snapshots for material equality.
 *
 * "Material" excludes flag-bit jitter that doesn't matter to T8's LCD.
 * In practice the entire struct is compared since every field drives
 * something visible to the operator (status icon, AP indicator, NTP tick,
 * IP address line).
 *
 * @param a  First snapshot. Must be non-NULL.
 * @param b  Second snapshot. Must be non-NULL.
 * @return `true` if all four observed fields match; `false` otherwise.
 * @note   Used by the T10 main loop to suppress redundant Q5 overwrites —
 *         only posts to Q5 when something material has changed.
 */
static bool snapshots_equal(const net_status_t *a, const net_status_t *b)
{
    return a->client_connected == b->client_connected
        && a->ap_active        == b->ap_active
        && a->ntp_synced       == b->ntp_synced
        && (strncmp(a->ip_str, b->ip_str, sizeof(a->ip_str)) == 0);
}

/* ============================================================
 * IP geolocation + timezone sync (alpha.6.28 — Phase 6.14.X)
 *
 * Calls http://ip-api.com/json once after the first NTP sync to obtain
 * lat/lon and IANA timezone from the device's public IP. On success:
 *   - lat_deg/lat_frac and lon_deg/lon_frac are pushed to Q4 (T4 writes
 *     them to NVS and recalculates today's sunrise/sunset).
 *   - tz_str is written directly to NVS_NS_SYSTEM via nvs_cfg_set_str
 *     and applied via setenv/tzset so localtime_r returns the local
 *     wall-clock time immediately.
 *
 * Plain HTTP (not HTTPS) — ip-api.com's free tier doesn't offer TLS.
 * Data is non-sensitive (just lat/lon for sunrise math); the risk of
 * a man-in-the-middle returning bogus coordinates is operationally
 * minor (worst case: sunrise calc drifts by a few minutes).
 *
 * Runs at most once per boot — `s_geo_done` latches after the first
 * successful response.
 * ============================================================ */

static bool s_geo_done = false;

/**
 * @brief HTTP response accumulator — captured by the geo event handler.
 *
 * Caller-owned buffer + capacity; the event handler appends `ON_DATA`
 * chunks until it fills (truncating cleanly with a trailing NUL).
 */
typedef struct {
    char  *buf;  /**< Caller-owned buffer; receives the response body + NUL. */
    size_t cap;  /**< Buffer capacity in bytes (must be ≥ 1 for the NUL). */
    size_t len;  /**< Bytes written so far, excluding the trailing NUL. */
} geo_resp_t;

/**
 * @brief esp_http_client event callback — appends body chunks into a geo_resp_t.
 *
 * Handles `HTTP_EVENT_ON_DATA` only; other events are ignored. Stops
 * appending once the buffer is full so subsequent chunks don't overflow.
 * NUL-terminates after every append so the buffer is always a valid
 * C-string at the point any future strstr/strchr scans it.
 *
 * @param  evt  Event record from esp_http_client. `evt->user_data` is
 *              expected to point to a `geo_resp_t`; non-NULL is the gate.
 * @return Always `ESP_OK` — never aborts the request from the callback.
 */
static esp_err_t geo_http_event_cb(esp_http_client_event_t *evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != NULL) {
        geo_resp_t *r = (geo_resp_t *)evt->user_data;
        if (r->buf == NULL || r->len >= r->cap - 1) return ESP_OK;
        size_t avail = r->cap - 1 - r->len;
        size_t take  = (size_t)evt->data_len < avail ? (size_t)evt->data_len : avail;
        memcpy(r->buf + r->len, evt->data, take);
        r->len += take;
        r->buf[r->len] = '\0';
    }
    return ESP_OK;
}

/**
 * @brief Convert a float coordinate to integer degrees + millidegree fraction.
 *
 * Splits a signed float (e.g. lat/lon in degrees) into two integers
 * suitable for NVS storage. The carry logic handles values like
 * `0.9995` → `deg=1, frac=0` (rather than `deg=0, frac=1000`).
 *
 * Examples:
 *  - `52.3676`  → `deg=52,  frac=368`
 *  - `-4.9`     → `deg=-4,  frac=900`
 *  - `0.0`      → `deg=0,   frac=0`
 *
 * @param      val   Coordinate value in degrees (signed float).
 * @param[out] deg   Integer degrees (signed; can be negative).
 * @param[out] frac  Millidegree fraction (0..999, always non-negative).
 * @note The sign of @p val attaches to @p deg only. @p frac is always
 *       0..999 regardless.
 */
static void float_to_deg_frac(float val, int32_t *deg, int32_t *frac)
{
    bool neg = (val < 0.0f);
    if (neg) val = -val;
    *deg  = (int32_t)val;
    *frac = (int32_t)((val - (float)*deg) * 1000.0f + 0.5f);
    if (*frac >= 1000) { *deg += 1; *frac -= 1000; }   /* carry */
    if (neg) *deg = -*deg;
}

/**
 * @brief Parse the ip-api.com JSON response into lat/lon/timezone.
 *
 * Lightweight ad-hoc parser (no cJSON dep). Searches for the three
 * known keys via `strstr` and extracts the values via `atof`/`memcpy`.
 *
 * Expected body shape:
 * @code
 * {"status":"success","lat":52.37,"lon":4.90,"timezone":"Europe/Amsterdam"}
 * @endcode
 *
 * @param      body     NUL-terminated response body (caller-owned).
 * @param[out] out_lat  Receives the parsed latitude (degrees).
 * @param[out] out_lon  Receives the parsed longitude (degrees).
 * @param[out] out_tz   Receives the parsed IANA timezone key (NUL-terminated).
 * @param      tz_len   Capacity of @p out_tz in bytes (≥ 1 for the NUL).
 * @return `true` if all three fields were present and parsed; `false` on
 *         any parse failure or non-success status.
 * @note A `"status":"fail"` response (e.g. ip-api rate-limiting) returns
 *       `false` cleanly — caller should retry on the next boot.
 */
static bool parse_geo_response(const char *body,
                                float *out_lat, float *out_lon,
                                char *out_tz, size_t tz_len)
{
    if (!strstr(body, "\"status\":\"success\"")) return false;

    const char *p = strstr(body, "\"lat\":");
    if (!p) return false;
    *out_lat = (float)atof(p + 6);

    p = strstr(body, "\"lon\":");
    if (!p) return false;
    *out_lon = (float)atof(p + 6);

    p = strstr(body, "\"timezone\":\"");
    if (!p) return false;
    p += 12;
    const char *end = strchr(p, '"');
    if (!end) return false;
    size_t len = (size_t)(end - p);
    if (len >= tz_len) len = tz_len - 1;
    memcpy(out_tz, p, len);
    out_tz[len] = '\0';
    return true;
}

/**
 * @brief Post a single i32 config update to Q4 (consumed by T4).
 *
 * T4 receives the update, writes the value to NVS under (ns, key), updates
 * its in-RAM `cfg_shadow_t` snapshot, and (if (ns,key) is one of the
 * sunrise-relevant fields) recalculates sunrise/sunset for the new
 * coordinates.
 *
 * @param ns     NVS namespace string ("system", "wifi", etc.). Must match
 *               the namespaces declared in `nvs_config.h`.
 * @param key    NVS key string (typically ≤ 15 chars per NVS rules).
 * @param value  i32 payload — interpreted by T4 based on (ns, key).
 * @note Non-blocking — Q4 is mutex-protected (MX4) with overflow drop.
 *       Callers don't need to lock.
 * @see config_update_t — Q4 message type.
 */
static void post_q4(const char *ns, const char *key, int32_t value)
{
    config_update_t upd = {};
    /* strncpy then explicit NUL — see alpha.6.18 build-trap notes for why
     * snprintf into a 16-byte field is unsafe with -Werror=format-truncation. */
    strncpy(upd.ns,  ns,  sizeof(upd.ns)  - 1);
    strncpy(upd.key, key, sizeof(upd.key) - 1);
    upd.value = value;
    (void)xQueueSend(Q4, &upd, pdMS_TO_TICKS(200));
}

/**
 * @brief One-shot IP geolocation sync via ip-api.com (HTTP, plain).
 *
 * Looks up the controller's public-IP-based lat/lon/timezone and:
 *  -# Posts lat_deg + lat_frac + lon_deg + lon_frac to Q4 so T4 persists
 *     them to NVS and refreshes the sunrise/sunset math.
 *  -# Translates the IANA timezone key into a POSIX TZ string via
 *     `iana_to_posix()`, writes it to NVS, then applies it via
 *     `setenv("TZ", …) + tzset()` so subsequent `localtime_r` calls
 *     return the local wall-clock time.
 *  -# Posts a LOG_SYSTEM `value_a=4, value_b=1` audit event (a.6.33).
 *  -# Sets `s_geo_done = true` so the next T10 main-loop iteration
 *     doesn't re-run.
 *
 * Plain HTTP, NOT HTTPS — ip-api.com's free tier doesn't offer TLS. The
 * MitM risk is operationally minor (worst case: sunrise calc drifts by a
 * few minutes if an attacker injects bogus coordinates).
 *
 * @warning Caller MUST gate on `!s_geo_done` to enforce once-per-boot.
 * @note   Synchronous — blocks the calling task for up to 5 s (HTTP
 *         timeout). Called once from T10's main loop, never on the hot
 *         path.
 * @note   Failure modes are non-fatal: any error path returns silently
 *         without setting `s_geo_done`, so the next boot retries cleanly.
 */
static void do_geo_sync(void)
{
    ESP_LOGI(TAG, "[T10] Geo sync starting via ip-api.com");

    char body[256] = {0};
    geo_resp_t resp = { .buf = body, .cap = sizeof(body), .len = 0 };

    esp_http_client_config_t cfg = {};
    cfg.url            = "http://ip-api.com/json?fields=status,lat,lon,timezone";
    cfg.method         = HTTP_METHOD_GET;
    cfg.timeout_ms     = 5000;
    cfg.event_handler  = geo_http_event_cb;
    cfg.user_data      = &resp;
    cfg.transport_type = HTTP_TRANSPORT_OVER_TCP;   /* HTTP, not HTTPS */
    cfg.buffer_size    = 512;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == NULL) {
        ESP_LOGW(TAG, "[T10] geo_sync: esp_http_client_init failed");
        return;
    }

    esp_err_t err   = esp_http_client_perform(client);
    int status_code = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status_code != 200) {
        ESP_LOGW(TAG, "[T10] geo_sync HTTP failed: err=%s status=%d",
                 esp_err_to_name(err), status_code);
        return;
    }

    float lat = 0.0f, lon = 0.0f;
    char  iana_tz[64] = {};
    if (!parse_geo_response(body, &lat, &lon, iana_tz, sizeof(iana_tz))) {
        ESP_LOGW(TAG, "[T10] geo_sync: failed to parse response: %s", body);
        return;
    }
    ESP_LOGI(TAG, "[T10] geo_sync: lat=%.4f lon=%.4f timezone=\"%s\"",
             (double)lat, (double)lon, iana_tz);

    int32_t lat_deg, lat_frac, lon_deg, lon_frac;
    float_to_deg_frac(lat, &lat_deg, &lat_frac);
    float_to_deg_frac(lon, &lon_deg, &lon_frac);

    /* Update lat/lon via Q4 — T4 writes NVS + refreshes the cfg shadow +
     * recalculates today's sunrise/sunset. */
    post_q4(NVS_NS_SYSTEM, "lat_deg",  lat_deg);
    post_q4(NVS_NS_SYSTEM, "lat_frac", lat_frac);
    post_q4(NVS_NS_SYSTEM, "lon_deg",  lon_deg);
    post_q4(NVS_NS_SYSTEM, "lon_frac", lon_frac);

    /* a.6.33 — audit event: value_a=4 (geo), value_b=1 (success). */
    log_sys(4, 1);

    /* IANA → POSIX TZ. Falls back to "TZ unchanged" if the zone is not in
     * the table — better than overwriting NVS with garbage. */
    const char *posix_tz = iana_to_posix(iana_tz);
    if (posix_tz) {
        (void)nvs_cfg_set_str(NVS_NS_SYSTEM, "tz_str", posix_tz);
        setenv("TZ", posix_tz, 1);
        tzset();
        ESP_LOGI(TAG, "[T10] TZ applied: \"%s\" (IANA=\"%s\")", posix_tz, iana_tz);
    } else {
        ESP_LOGW(TAG, "[T10] geo_sync: IANA zone \"%s\" not in table — TZ unchanged",
                 iana_tz);
    }

    s_geo_done = true;
}

/* ============================================================
 * AP mode (alpha.6.29 — Phase 6.14.X step 2)
 *
 * Operator-toggled soft-AP that runs alongside the STA mode (WIFI_MODE_APSTA).
 * Used for first-time WiFi setup and for credential-recovery: the operator
 * joins `Greenhouse-XXYY` (last 2 bytes of MAC), opens http://192.168.4.1,
 * and writes the real SSID/PSK through the web GUI.
 *
 * Lifecycle:
 *   - At boot: read NVS `wifi/ap_psk` (factory default "0123456789" if unset),
 *     compute SSID from MAC, do NOT start the AP yet.
 *   - Each poll (NET_POLL_MS = 5 s): read NVS `wifi/ap_enable`. On the
 *     0→1 edge call start_ap(); on the 1→0 edge call stop_ap().
 *   - While active: check uptime against `cfg.ap_timeout_min`. On expiry
 *     call stop_ap() AND clear `wifi/ap_enable` so a stale flag in NVS
 *     doesn't restart the AP on the next tick.
 *
 * The operator enables/disables via T8 (LCD menu) or T11 (web GUI), both of
 * which write the flag through Q4 → T4 → NVS. T10 polls NVS directly here
 * to keep the state machine self-contained — no cross-task notify needed.
 *
 * AP_PSK_DEFAULT is identical to 1.20.3 (`0123456789`) so the operator
 * manual instructions still apply. NEVER an open AP — WPA2 requires the
 * raw key, and an unsecured AP would expose the GUI's admin functions to
 * anyone in radio range.
 * ============================================================ */

#define AP_CHANNEL          1u
#define AP_MAX_CONN         4u
#define AP_PSK_DEFAULT      "0123456789"

static esp_netif_t  *s_ap_netif      = NULL;
/* s_ap_active is forward-declared near the top of the file (snapshot_state
 * needs it). */
static int32_t       s_ap_enable_nvs = -1;     /* sentinel: first poll always logs a transition */
static TickType_t    s_ap_started    = 0;
static char          s_ap_ssid[20]   = {};
static char          s_ap_psk[64]    = {};

/**
 * @brief Reload AP SSID + PSK from NVS into the module-static buffers.
 *
 * Called once at task start AND on every `start_ap()` invocation so admin
 * changes to NVS `wifi/ap_ssid` / `wifi/ap_psk` take effect on the next
 * enable cycle without a reboot.
 *
 * Source order:
 *  - SSID: NVS `wifi/ap_ssid` (admin override) → MAC-derived `Greenhouse-XXYY`
 *          default. Every fresh unit thus has a unique SSID without admin setup.
 *  - PSK:  NVS `wifi/ap_psk` (admin override) → `AP_PSK_DEFAULT` ("0123456789").
 *          Never an open AP — WPA2 requires the raw key.
 *
 * @warning Writes to module-static `s_ap_ssid` / `s_ap_psk`. Single-task-owned
 *          by T10 — no locking. Do NOT call from other tasks.
 */
static void load_ap_credentials(void)
{
    /* SSID source order: NVS `wifi/ap_ssid` (admin override) → MAC-derived
     * `Greenhouse-XXYY` default. The MAC-derived default ensures every
     * fresh unit has a unique SSID without the admin needing to set one. */
    char ssid_buf[sizeof(s_ap_ssid)] = {0};
    nvs_cfg_status_t st = nvs_cfg_get_str(NVS_NS_WIFI, "ap_ssid",
                                          ssid_buf, sizeof(ssid_buf));
    if (st == NVS_CFG_OK && ssid_buf[0] != '\0') {
        strncpy(s_ap_ssid, ssid_buf, sizeof(s_ap_ssid) - 1);
    } else {
        uint8_t mac[6] = {};
        if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Greenhouse-%02X%02X",
                     mac[4], mac[5]);
        } else {
            snprintf(s_ap_ssid, sizeof(s_ap_ssid), "Greenhouse-0000");
            ESP_LOGW(TAG, "[T10] esp_read_mac failed — AP SSID fallback");
        }
    }

    /* PSK source order: NVS `wifi/ap_psk` (admin override) → AP_PSK_DEFAULT.
     * Never an open AP (WPA2 requires the raw key; an unsecured AP would
     * expose the admin GUI to anyone in radio range). */
    char psk_buf[sizeof(s_ap_psk)] = {0};
    st = nvs_cfg_get_str(NVS_NS_WIFI, "ap_psk", psk_buf, sizeof(psk_buf));
    if (st == NVS_CFG_OK && psk_buf[0] != '\0') {
        strncpy(s_ap_psk, psk_buf, sizeof(s_ap_psk) - 1);
    } else {
        strncpy(s_ap_psk, AP_PSK_DEFAULT, sizeof(s_ap_psk) - 1);
    }
    s_ap_ssid[sizeof(s_ap_ssid) - 1] = '\0';
    s_ap_psk[sizeof(s_ap_psk)  - 1] = '\0';
}

/**
 * @brief One-shot AP module init — populate SSID/PSK buffers, no radio change.
 *
 * Called once from `task_network_manager()` at task start. The radio stays
 * in whatever mode `nm_wifi_init_blocking()` left it in (typically STA);
 * AP only comes up when the admin flips `wifi/ap_enable=1` in NVS and
 * `poll_ap()` notices the transition.
 *
 * @note Splitting `ap_init()` from `start_ap()` means a fresh boot doesn't
 *       waste radio cycles starting an AP nobody asked for — even though
 *       the credentials are ready to go on demand.
 */
static void ap_init(void)
{
    load_ap_credentials();
    ESP_LOGI(TAG, "[T10] AP config ready: SSID=\"%s\" PSK=(%u chars)",
             s_ap_ssid, (unsigned)strlen(s_ap_psk));
}

/**
 * @brief Start the WPA2-PSK soft-AP. Idempotent.
 *
 * Reloads SSID/PSK from NVS so admin GUI changes take effect on every
 * enable cycle without a reboot. Creates the AP netif if missing, sets
 * WiFi mode to APSTA, configures the AP, and posts a LOG_SYSTEM
 * `value_a=3, value_b=1` audit event on success.
 *
 * AP defaults:
 *  - Channel: `AP_CHANNEL` (1).
 *  - Max clients: `AP_MAX_CONN` (4).
 *  - Authmode: WPA2-PSK (esp_wifi enforces ≥ 8 char key).
 *  - DHCP: auto-started on 192.168.4.0/24 by the IDF default AP netif.
 *
 * @note If `esp_wifi_set_config(WIFI_IF_AP, …)` fails the function rolls
 *       back to `WIFI_MODE_STA` so the radio is left in a clean state.
 * @warning Single-task-owned by T10 — do NOT call from other tasks.
 * @see poll_ap() — only intended caller (gated on NVS `wifi/ap_enable`).
 */
static void start_ap(void)
{
    if (s_ap_active) return;

    /* Reload AP creds from NVS so admin GUI changes take effect on next
     * enable without a reboot. The previous values are kept on a failed
     * NVS read (load_ap_credentials() falls back to MAC + default). */
    load_ap_credentials();

    /* Default AP netif — idempotent: returns the existing handle if a prior
     * start_ap already created it. */
    if (s_ap_netif == NULL) {
        s_ap_netif = esp_netif_create_default_wifi_ap();
        if (s_ap_netif == NULL) {
            ESP_LOGE(TAG, "[T10] esp_netif_create_default_wifi_ap returned NULL");
            return;
        }
    }

    /* Configure AP. Authmode WPA2-PSK; min PSK length is 8 chars (enforced
     * by esp_wifi_set_config). max_connection=4 matches 1.20.3. */
    wifi_config_t ap_cfg = {};
    strncpy((char *)ap_cfg.ap.ssid,     s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    strncpy((char *)ap_cfg.ap.password, s_ap_psk,  sizeof(ap_cfg.ap.password) - 1);
    ap_cfg.ap.ssid_len       = (uint8_t)strlen(s_ap_ssid);
    ap_cfg.ap.channel        = AP_CHANNEL;
    ap_cfg.ap.authmode       = WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.beacon_interval = 100;

    /* Switch mode to APSTA. esp_wifi_set_mode is safe while the radio is
     * running — STA stays connected. */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T10] esp_wifi_set_mode(APSTA) failed: %s", esp_err_to_name(err));
        return;
    }

    err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[T10] esp_wifi_set_config(AP) failed: %s", esp_err_to_name(err));
        /* Roll back to STA-only to leave a clean state. */
        (void)esp_wifi_set_mode(WIFI_MODE_STA);
        return;
    }

    /* esp_netif_create_default_wifi_ap auto-starts DHCP on the AP netif
     * with 192.168.4.0/24 — matches the operator manual instructions
     * ("open http://192.168.4.1"). */
    s_ap_active  = true;
    s_ap_started = xTaskGetTickCount();
    ESP_LOGI(TAG, "[T10] AP started: SSID=\"%s\" (channel %u, max %u clients)",
             s_ap_ssid, (unsigned)AP_CHANNEL, (unsigned)AP_MAX_CONN);
    /* a.6.33 — audit event: value_a=3 (AP), value_b=1 (started). */
    log_sys(3, 1);
}

/**
 * @brief Stop the soft-AP. Returns WiFi mode to STA-only. Idempotent.
 *
 * Posts a LOG_SYSTEM `value_a=3, value_b=0` audit event on success. The
 * AP netif handle is retained for cheap reuse on the next `start_ap()`.
 *
 * @note Switching to `WIFI_MODE_STA` also tears down the AP's DHCP server
 *       and disconnects any associated clients. The STA association
 *       (controller ↔ home WiFi) is NOT affected.
 */
static void stop_ap(void)
{
    if (!s_ap_active) return;

    /* Returning to STA-only also tears down the AP netif's DHCP server.
     * The netif handle is kept for the next start_ap (cheap to reuse). */
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "[T10] esp_wifi_set_mode(STA) on AP stop failed: %s",
                 esp_err_to_name(err));
    }

    s_ap_active = false;
    ESP_LOGI(TAG, "[T10] AP stopped");
    /* a.6.33 — audit event: value_a=3 (AP), value_b=0 (stopped). */
    log_sys(3, 0);
}

/**
 * @brief Per-tick AP-state reconciliation: enable on NVS edge, auto-shutdown on timeout.
 *
 * Called every NET_POLL_MS from the T10 main loop. Two responsibilities:
 *
 *  1. **NVS-edge handling.** Reads `wifi/ap_enable` from NVS. On any
 *     change vs the previous tick (tracked by `s_ap_enable_nvs`), calls
 *     `start_ap()` or `stop_ap()` accordingly. The first tick always
 *     triggers a transition because `s_ap_enable_nvs` is initialised to
 *     the sentinel `-1`.
 *
 *  2. **Auto-shutdown.** When AP is active AND `cfg.ap_timeout_min > 0`,
 *     stops the AP after that many minutes elapsed since `start_ap()`.
 *     Also clears the NVS flag via Q4 so the next poll doesn't restart it.
 *
 * @warning alpha.6.31 security policy: admin-only enable, no auto-enable
 *          on credential loss. Auto-enabling on a failed STA would expose
 *          an unconfigured greenhouse to anyone in radio range. The admin
 *          must explicitly write `wifi/ap_enable=1` via the web GUI.
 * @see start_ap() / stop_ap() — the actual radio-mode toggle.
 */
static void poll_ap(void)
{
    int32_t ap_en = 0;
    nvs_cfg_get_i32_or_default(NVS_NS_WIFI, "ap_enable", 0, &ap_en);

    if (ap_en != s_ap_enable_nvs) {
        s_ap_enable_nvs = ap_en;
        if (ap_en) start_ap();
        else       stop_ap();
    }

    /* Auto-shutdown after cfg.ap_timeout_min minutes. 0 = never shut down
     * (the operator must toggle the flag manually). */
    if (s_ap_active) {
        cfg_shadow_t cfg = {};
        dm_cfg_snapshot(&cfg);
        if (cfg.ap_timeout_min > 0) {
            uint32_t elapsed_ms = (uint32_t)(
                (xTaskGetTickCount() - s_ap_started) * portTICK_PERIOD_MS);
            uint32_t limit_ms = (uint32_t)cfg.ap_timeout_min * 60u * 1000u;
            if (elapsed_ms >= limit_ms) {
                ESP_LOGI(TAG, "[T10] AP auto-shutdown after %ld min",
                         (long)cfg.ap_timeout_min);
                /* Also clear the NVS flag so the next poll doesn't restart it.
                 * Write through Q4 so T4's cfg shadow is also updated. */
                stop_ap();
                post_q4(NVS_NS_WIFI, "ap_enable", 0);
                s_ap_enable_nvs = 0;
            }
        }
    }
}

/* ============================================================
 * Periodic 24 h NTP resync (a.6.33 — Phase T10 maturation)
 *
 * The DS1307 RTC is precise enough for multi-day operation, but slow drift
 * accumulates. The 1.20.3 design called for a once-per-24 h SNTP refresh
 * to keep the wall clock aligned. Called only when STA is connected and
 * the elapsed since last sync exceeds NTP_RESYNC_INTERVAL_S.
 *
 * Implementation notes:
 *   - esp_sntp_* directly, NOT wifi_tickle_run (which is a boot-time helper
 *     that re-initializes the event loop + netif).
 *   - **esp_sntp_setoperatingmode is NOT idempotent on a running client.**
 *     lwIP's sntp.c:748 asserts `sntp_pcb == NULL` (i.e. SNTP must be stopped).
 *     The previous claim that "setoperatingmode / setservername are idempotent"
 *     was wrong and produced the rc.1.3.2 panic — `esp_sntp_init()` on the
 *     first resync allocates `sntp_pcb`; the second resync's setoperatingmode
 *     call then asserts. rc.1.3.3 fixes this by calling `esp_sntp_stop()` at
 *     the top of run_ntp_resync() so `sntp_pcb` is always freed before the
 *     setoperatingmode call. Calling esp_sntp_stop() when already stopped is
 *     a safe no-op in lwIP.
 *   - esp_sntp_init() may be called repeatedly **provided the matching stop
 *     has run**; second-and-later calls reinitialize the SNTP module.
 *   - Wait up to 10 s for a plausible epoch; on timeout, leave
 *     s_last_ntp_sync_us unchanged so the next iteration retries.
 *   - After success, re-apply cfg.tz_str (esp_sntp resets TZ to UTC) and
 *     notify T4 to re-write the DS1307.
 *   - Geo is NOT re-fetched (location is stable).
 * ============================================================ */

/**
 * @brief Periodic 24 h NTP resync — kicks a fresh SNTP query, re-applies TZ.
 *
 * Called from the T10 main loop when `esp_timer_get_time() - s_last_ntp_sync_us`
 * exceeds `NTP_RESYNC_INTERVAL_S` (= 24 h). The DS1307 RTC is precise enough
 * for multi-day operation, but slow drift accumulates — this brings the
 * wall clock back to NTP-grade accuracy.
 *
 * Per-call behaviour:
 *  -# Bail if `esp_sntp_get_sync_status()` reports IN_PROGRESS (avoid
 *     double-kicking).
 *  -# Configure pool.ntp.org + POLL mode; call `esp_sntp_init()` to kick
 *     the query.
 *  -# Wait up to 10 s (20 × 500 ms) for `SNTP_SYNC_STATUS_COMPLETED`.
 *  -# On timeout: log a `value_a=2, value_b=0` audit row and bail (next
 *     T10 cycle retries).
 *  -# On success: re-apply persisted POSIX TZ (`esp_sntp` resets TZ to UTC
 *     implicitly), update `s_last_ntp_sync_us`, notify T4 via
 *     `DM_NOTIFY_NTP_SYNCED` so T4 writes the new time back to the DS1307.
 *
 * @note Blocks the calling task for up to 10 s. Acceptable because T10 is
 *       a low-priority background monitor — climate-control tasks
 *       preempt cleanly.
 * @note Geo is NOT re-fetched during the periodic resync (location is
 *       stable for a stationary controller). Only `do_geo_sync()` at boot
 *       runs the IP-geolocation lookup.
 * @see  nm_sntp_quick_sync() — the boot-time SNTP sync.
 */
static void run_ntp_resync(void)
{
    ESP_LOGI(TAG, "[T10] Starting periodic NTP resync (24 h cadence)");

    /* rc.1.3.3 — defensive stop before reconfiguring. The previous resync's
     * esp_sntp_init() allocated sntp_pcb without a matching stop, which made
     * the next invocation hit lwIP's assert in sntp_setoperatingmode
     * (sntp.c:748, "Operating mode must not be set while SNTP client is
     * running"). esp_sntp_stop() on an already-stopped client is a no-op,
     * so this is safe on every entry — including the very first call after
     * boot, when sntp_pcb is already NULL from nm_sntp_quick_sync()'s
     * esp_netif_sntp_deinit(). */
    esp_sntp_stop();

    /* esp_sntp_get_sync_status returns IN_PROGRESS while a previous sync is
     * still running. Avoid double-kicking. (After the stop above this can
     * never be IN_PROGRESS, but the guard is kept as a belt-and-braces.) */
    sntp_sync_status_t st = esp_sntp_get_sync_status();
    if (st == SNTP_SYNC_STATUS_IN_PROGRESS) {
        ESP_LOGI(TAG, "[T10] NTP resync: another sync already in progress — skipping");
        return;
    }

    /* Setup. esp_sntp_init() is repeatable (given the stop above); calling
     * it kicks a new query against the configured server. */
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();

    /* Wait up to 10 s for ESP-IDF to report SNTP_SYNC_STATUS_COMPLETED.
     * A fresh sync simply updates the clock with sub-second accuracy;
     * the wall-clock value is already trustworthy from the boot sync (or
     * from T4's DS1307 pre-seed if WiFi was absent at boot). The previous
     * comment referenced a time-delta heuristic that the loop never
     * actually used. rc.1.5.3 — kept the (correct) status-query loop and
     * removed the stale prose. */
    const int wait_steps = 20;   /* 20 × 500 ms = 10 s */
    bool ok = false;
    for (int i = 0; i < wait_steps; i++) {
        if (esp_sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
            ok = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    if (!ok) {
        ESP_LOGW(TAG, "[T10] NTP resync: timed out after 10 s — will retry next cycle");
        /* a.6.35.3 — emit the parser-documented NTP timeout audit row. */
        log_sys(2, 0);
        return;
    }

    /* Re-apply persisted POSIX TZ. esp_sntp resets TZ to UTC implicitly;
     * if cfg.tz_str is unset, leave whatever the prior setenv set. */
    cfg_shadow_t cfg = {};
    dm_cfg_snapshot(&cfg);
    if (cfg.tz_str[0] != '\0') {
        const char *cur_tz = getenv("TZ");
        if (cur_tz == NULL || strcmp(cur_tz, cfg.tz_str) != 0) {
            setenv("TZ", cfg.tz_str, 1);
            tzset();
            ESP_LOGI(TAG, "[T10] NTP resync: TZ re-applied (\"%s\")", cfg.tz_str);
        }
    }

    s_last_ntp_sync_us = esp_timer_get_time();
    /* rc.1.5.3 — latch the indicator flag. Idempotent (already true on any
     * subsequent resync); kept here so a unit that came up without WiFi at
     * boot — and therefore with s_sntp_synced still false — flips to "NTP"
     * the moment its first periodic resync completes. */
    s_sntp_synced = true;
    ESP_LOGI(TAG, "[T10] NTP resync complete");

    /* Notify T4 so it re-writes the DS1307 RTC with the freshly-synced time.
     * Idempotent — DS1307 just gets refreshed with the same-to-the-second
     * value. T4 takes MX1 briefly. */
    if (task_t4 != NULL) {
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
    }
}

void task_network_manager(void *pvParameters)
{
    (void)pvParameters;

    ESP_LOGI(TAG, "[T10] task alive (geo sync since alpha.6.28; AP since alpha.6.29)");

    /* Brief settling delay so wifi_tickle's last log lines flush. The
     * esp_wifi + esp_netif state is queryable immediately, but logging
     * order is nicer if T10's first Q5 post comes after wifi_tickle's
     * "SNTP synced after N ms" line. */
    vTaskDelay(pdMS_TO_TICKS(500));

    /* alpha.6.29 — AP one-shot init: compute SSID from MAC, read PSK from
     * NVS. No radio side effects; start_ap() is gated on `wifi/ap_enable`
     * in poll_ap() below. */
    ap_init();

    /* Initial snapshot + Q5 post. */
    net_status_t prev = {};
    snapshot_state(&prev);
    if (xQueueOverwrite(Q5, &prev) == pdTRUE) {
        ESP_LOGI(TAG, "[T10] initial Q5 post: client=%d ap=%d ntp=%d ip=\"%s\"",
                 (int)prev.client_connected, (int)prev.ap_active,
                 (int)prev.ntp_synced, prev.ip_str);
    } else {
        ESP_LOGW(TAG, "[T10] initial Q5 overwrite failed");
    }

    /* If NTP synced at boot (wifi_tickle's job), notify T4 so it can write
     * the post-SNTP system time back to the DS1307 via the TN4 path. T4's
     * dm_get_unix_time accessor is called immediately by event_logger and
     * sensor_poll; we want T4's RTC writeback to happen ASAP. */
    if (prev.ntp_synced && task_t4 != NULL) {
        xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
        ESP_LOGI(TAG, "[T10] TN4 sent to T4 (DM_NOTIFY_NTP_SYNCED)");
        /* a.6.33 — seed the resync timer at boot so the 24 h cadence is
         * measured from the boot-time sync, not from T10 task start. */
        s_last_ntp_sync_us = esp_timer_get_time();
    }
    /* rc.1.5.6 — seed the attempt timestamp regardless of boot SNTP
     * outcome. The retry-cadence branch (below) uses this as the
     * reference when no successful sync has happened yet. Without the
     * seed it would be 0, so the first main-loop iteration's
     * `now - 0 >= NTP_RETRY_INTERVAL` test would fire immediately and
     * SNTP would be re-run back-to-back with the boot quick_sync. */
    s_last_ntp_attempt_us = esp_timer_get_time();

    /* a.6.35.3 — emit boot-time STA / NTP snapshot rows.
     *
     * The main-loop transition handler (below) only fires log_sys on
     * snapshots_equal-detected edges. Because `prev` is seeded from the
     * post-boot state above, the very first iteration sees no edge and
     * never emits the "STA up" / "NTP synced" rows the logparser expects
     * (value_a=1, value_a=2). Emit them once here so every boot's CSV
     * has a definitive record of the network state the firmware came up
     * with — matches the way BOOT (a=5), Unit ID (a=11) and T2 boot-cal
     * (a=10) rows are all stamped once per boot. */
    log_sys(1, prev.client_connected ? 1 : 0);
    log_sys(2, prev.ntp_synced       ? 1 : 0);

    /* alpha.6.28 — one-shot IP geolocation + timezone sync. Gated on NTP
     * being synced (so DNS resolution + HTTP have a working network) and
     * latched after the first successful response. */
    if (prev.client_connected && prev.ntp_synced && !s_geo_done) {
        do_geo_sync();
    }

    /* Main loop — periodic state polling. */
    for (;;) {
        /* 2.0.3 (gh#33) — wait NET_POLL_MS for either a tick or a
         * recovery notification from T14. Notify-driven wakeup short-
         * circuits the delay when T14 has crossed a fail threshold; the
         * normal 5 s polling cadence is unchanged. xTaskNotifyWait
         * clears all bits on read so each notification is consumed
         * exactly once. */
        uint32_t notify = 0;
        (void)xTaskNotifyWait(0, ULONG_MAX, &notify, pdMS_TO_TICKS(NET_POLL_MS));

        if (notify & NM_NOTIFY_RENEW_DHCP) {
            ESP_LOGW(TAG, "[T10] L3 recovery — DHCP renew (T14 fail-threshold A)");
            esp_netif_t *sta = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
            if (sta != NULL) {
                (void)esp_netif_dhcpc_stop(sta);
                vTaskDelay(pdMS_TO_TICKS(100));     /* let lwIP release */
                (void)esp_netif_dhcpc_start(sta);
                log_sys(19, 0);                     /* a=19 b=0: DHCP renew */
            } else {
                ESP_LOGW(TAG, "[T10] DHCP renew skipped — STA netif handle NULL");
            }
        }
        if (notify & NM_NOTIFY_REASSOCIATE) {
            ESP_LOGW(TAG, "[T10] L3 recovery — STA reassociate (T14 fail-threshold B)");
            (void)esp_wifi_disconnect();
            vTaskDelay(pdMS_TO_TICKS(100));         /* let the radio settle */
            (void)esp_wifi_connect();
            log_sys(19, 1);                         /* a=19 b=1: STA reassociate */
        }

        /* alpha.6.29 — operator-toggle AP. Reads NVS wifi/ap_enable; starts
         * or stops the soft-AP on the 0↔1 edge. Also enforces the
         * cfg.ap_timeout_min auto-shutdown. */
        poll_ap();

        /* rc.1.5.6 — periodic NTP resync. Two cadences depending on
         * whether we've ever synced this boot:
         *   - synced (s_last_ntp_sync_us != 0):
         *       retry every NTP_RESYNC_INTERVAL_US (24 h, drift bound).
         *   - never synced (boot SNTP timed out):
         *       retry every NTP_RETRY_INTERVAL_US (5 min) so we catch up
         *       quickly once the network recovers.
         *
         * Before rc.1.5.6 the test was `s_last_ntp_sync_us != 0 &&
         * prev.client_connected`, which meant a unit whose boot SNTP
         * timed out was stranded on RTC for the rest of the boot — the
         * 24 h timer never started counting. The rc.1.5.4 web-GUI fix
         * (using the real s_sntp_synced latch instead of the
         * time-comparison heuristic) made the stuck state observable;
         * this is the actual fix. */
        if (prev.client_connected) {
            int64_t now_us       = esp_timer_get_time();
            bool    synced       = (s_last_ntp_sync_us != 0);
            uint64_t interval_us = synced ? NTP_RESYNC_INTERVAL_US
                                          : NTP_RETRY_INTERVAL_US;
            int64_t reference_us = synced ? s_last_ntp_sync_us
                                          : s_last_ntp_attempt_us;
            if ((uint64_t)(now_us - reference_us) >= interval_us) {
                s_last_ntp_attempt_us = now_us;
                run_ntp_resync();
                /* run_ntp_resync updates s_last_ntp_sync_us on success
                 * (which flips us back to the 24 h branch from the next
                 * iteration on); on failure leaves it unchanged so the
                 * retry branch keeps firing every NTP_RETRY_INTERVAL_S. */
            }
        }

        net_status_t cur = {};
        snapshot_state(&cur);

        if (!snapshots_equal(&prev, &cur)) {
            ESP_LOGI(TAG, "[T10] state changed: client=%d ap=%d ntp=%d ip=\"%s\"",
                     (int)cur.client_connected, (int)cur.ap_active,
                     (int)cur.ntp_synced, cur.ip_str);
            (void)xQueueOverwrite(Q5, &cur);

            /* a.6.35.3 — emit LOG_SYSTEM audit events for STA and NTP state
             * transitions. These were documented in event_logger.h and
             * logparser.md (value_a=1 STA, value_a=2 NTP) but never actually
             * produced by the firmware — the parser would never see them.
             * Edge-triggered (only on transitions) to keep the CSV signal-
             * to-noise high; AP transitions already get the same treatment
             * via start_ap()/stop_ap() so this just fills the remaining
             * two documented slots. */
            if (cur.client_connected != prev.client_connected) {
                log_sys(1, cur.client_connected ? 1 : 0);
            }
            if (cur.ntp_synced != prev.ntp_synced) {
                log_sys(2, cur.ntp_synced ? 1 : 0);
            }

            /* If NTP just transitioned to synced (e.g. after a reconnect
             * that included a fresh SNTP run), notify T4 again. The TN4
             * path in T4 is idempotent — multiple notifies in the same
             * boot just refresh the RTC. */
            if (cur.ntp_synced && !prev.ntp_synced && task_t4 != NULL) {
                xTaskNotify(task_t4, DM_NOTIFY_NTP_SYNCED, eSetBits);
                ESP_LOGI(TAG, "[T10] TN4 re-sent on NTP-synced transition");
                /* a.6.33 — also (re)seed the resync timer on an NTP-up edge,
                 * so the 24 h cadence is measured from when the wall clock
                 * actually became plausible. */
                s_last_ntp_sync_us = esp_timer_get_time();
            }

            prev = cur;
        }

        /* alpha.6.28 — retry geo sync on subsequent ticks if the initial
         * post-boot attempt was skipped (NTP not synced yet) or failed
         * (DNS hiccup, ip-api.com rate-limit etc). The `s_geo_done` latch
         * is set only on a fully parsed response, so failures retry until
         * we get a clean answer. */
        if (!s_geo_done && prev.client_connected && prev.ntp_synced) {
            do_geo_sync();
        }
    }
}
