/**
 * @file pin_auth.cpp
 * @brief PIN authentication implementation — see pin_auth.h for full description.
 *
 * Implements the salted SHA-256 / NVS-backed PIN scheme described in
 * TSDS §5.4. All persistent state lives in NVS namespace `access` (see
 * NVS_NS_ACCESS in nvs_config.h); module RAM holds only the 16-byte salt
 * and an init flag.
 *
 * ## Sources of randomness
 *   `esp_fill_random()` (Espressif hardware TRNG) generates the per-device
 *   salt the first time pin_auth_init() runs. The salt is written once and
 *   never regenerated, so PIN hashes remain comparable across reboots.
 *
 * ## Constant-time comparison
 *   hash_equal() XORs corresponding bytes and OR-folds into a single byte
 *   to prevent input-dependent early exit. The intent is to avoid leaking
 *   PIN-prefix correctness via timing — overkill for a four-digit local
 *   keypad, but the same code is reused for the eight-digit admin PIN and
 *   the web /api/login path.
 *
 * @author Greenhouse Controller project
 */

#include "pin_auth.h"
#include "nvs_config.h"        /* LIB-7 — NVS get/set wrappers              */

#include <mbedtls/sha256.h>    /* SHA-256 via ESP-IDF bundled mbedTLS        */
#include <esp_random.h>        /* esp_fill_random() — hardware TRNG          */
#include <string.h>
#include <time.h>              /* time() — Unix timestamp from system clock  */

/* ---------------------------------------------------------------------------
 * NVS key names (namespace NVS_NS_ACCESS = "access", defined in nvs_config.h)
 * --------------------------------------------------------------------------- */

/** @brief NVS key for the per-device random salt (blob[16]). */
#define KEY_SALT           "pin_salt"
/** @brief NVS key for the farmer PIN's SHA-256 hash (blob[32]). */
#define KEY_HASH_FARMER    "pin_farmer_hash"
/** @brief NVS key for the administrator PIN's SHA-256 hash (blob[32]). */
#define KEY_HASH_ADMIN     "pin_admin_hash"
/** @brief NVS key for consecutive farmer-PIN failure counter (int32). */
#define KEY_FAIL_FARMER    "fail_cnt_f"
/** @brief NVS key for consecutive admin-PIN failure counter (int32). */
#define KEY_FAIL_ADMIN     "fail_cnt_a"
/** @brief NVS key for farmer-role lockout expiry (Unix timestamp; 0 = none). */
#define KEY_LOCKOUT_FARMER "lockout_f"
/** @brief NVS key for admin-role lockout expiry (Unix timestamp; 0 = none). */
#define KEY_LOCKOUT_ADMIN  "lockout_a"
/** @brief NVS key for configurable max consecutive failures before lockout. */
#define KEY_LOCKOUT_MAX    "lockout_max"
/** @brief NVS key for configurable lockout duration in seconds. */
#define KEY_LOCKOUT_SECS   "lockout_secs"

/* ---------------------------------------------------------------------------
 * Module state — loaded once at init, read-only thereafter
 * --------------------------------------------------------------------------- */
static uint8_t s_salt[PIN_SALT_LEN];
static bool    s_initialized = false;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/**
 * @brief Compute SHA-256(salt || pin) into hash_out[PIN_HASH_LEN].
 *
 * Uses ESP-IDF's bundled mbedTLS. The PIN is hashed as raw ASCII digit
 * bytes (no NUL terminator), matching what pin_auth_verify() does with
 * caller-supplied PIN strings.
 *
 * @param salt      16-byte salt buffer (PIN_SALT_LEN bytes).
 * @param pin       Null-terminated ASCII PIN string; strlen() is used.
 * @param hash_out  Caller-allocated PIN_HASH_LEN-byte output buffer.
 */
static void compute_hash(const uint8_t *salt, const char *pin, uint8_t *hash_out)
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);                              /* 0 = SHA-256 */
    mbedtls_sha256_update(&ctx, salt, PIN_SALT_LEN);
    mbedtls_sha256_update(&ctx, (const uint8_t *)pin, strlen(pin));
    mbedtls_sha256_finish(&ctx, hash_out);
    mbedtls_sha256_free(&ctx);
}

/**
 * @brief Constant-time comparison of two fixed-length byte arrays.
 *
 * Avoids early-exit timing leaks by XOR-folding the difference into a
 * single accumulator. Always touches every byte regardless of mismatch
 * position.
 *
 * @param a    First byte array.
 * @param b    Second byte array.
 * @param len  Length in bytes; both arrays must be at least this long.
 * @return     true if the arrays match byte-for-byte, false otherwise.
 */
static bool hash_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/** @brief Return the NVS hash key name for a role. */
static const char *hash_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_HASH_FARMER : KEY_HASH_ADMIN;
}

/** @brief Return the NVS failure-counter key name for a role. */
static const char *fail_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_FAIL_FARMER : KEY_FAIL_ADMIN;
}

/** @brief Return the NVS lockout-expiry key name for a role. */
static const char *lockout_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_LOCKOUT_FARMER : KEY_LOCKOUT_ADMIN;
}

/** @brief Return the required digit count for a role (4 farmer / 8 admin). */
static size_t required_digits(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? PIN_FARMER_DIGITS : PIN_ADMIN_DIGITS;
}

/**
 * @brief Write default hashes for both roles using the current s_salt.
 *
 * Used at first boot (after salt generation) and for partial-write recovery
 * when one of the two hashes is missing from NVS. The salt is read from
 * s_salt; the caller must have populated it first.
 *
 * @return PIN_AUTH_OK on success, PIN_AUTH_ERR_NVS if either NVS blob write
 *         fails. On failure the NVS state may be partially written.
 * @note   Caller pays for two NVS blob writes (~32 KB flash wear budget).
 * @warning Logs of plain-text PINs are never produced; only the hash blobs
 *          are stored (FR-AC06).
 */
static pin_auth_result_t write_default_hashes(void)
{
    uint8_t hash[PIN_HASH_LEN];

    compute_hash(s_salt, PIN_DEFAULT_FARMER, hash);
    if (nvs_cfg_set_blob(NVS_NS_ACCESS, KEY_HASH_FARMER, hash, PIN_HASH_LEN) != NVS_CFG_OK)
        return PIN_AUTH_ERR_NVS;

    compute_hash(s_salt, PIN_DEFAULT_ADMIN, hash);
    if (nvs_cfg_set_blob(NVS_NS_ACCESS, KEY_HASH_ADMIN, hash, PIN_HASH_LEN) != NVS_CFG_OK)
        return PIN_AUTH_ERR_NVS;

    return PIN_AUTH_OK;
}

/* ---------------------------------------------------------------------------
 * Public API
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the PIN authentication module. See pin_auth.h.
 *
 * Three boot paths:
 *   - First boot (salt absent): generate random salt + write both default
 *     hashes. Two blob writes to NVS.
 *   - Partial-write recovery (salt present, hash missing): rewrite both
 *     default hashes with existing salt.
 *   - Normal boot: load salt; no writes.
 *
 * @return PIN_AUTH_OK on success, PIN_AUTH_ERR_NVS on NVS failure.
 * @see    write_default_hashes()
 */
pin_auth_result_t pin_auth_init(void)
{
    size_t salt_len = PIN_SALT_LEN;
    nvs_cfg_status_t err = nvs_cfg_get_blob(NVS_NS_ACCESS, KEY_SALT, s_salt, &salt_len);

    if (err == NVS_CFG_ERR_NOT_FOUND || salt_len != PIN_SALT_LEN) {
        /* First boot: generate random salt and write factory-default hashes. */
        esp_fill_random(s_salt, PIN_SALT_LEN);
        if (nvs_cfg_set_blob(NVS_NS_ACCESS, KEY_SALT, s_salt, PIN_SALT_LEN) != NVS_CFG_OK)
            return PIN_AUTH_ERR_NVS;
        if (write_default_hashes() != PIN_AUTH_OK)
            return PIN_AUTH_ERR_NVS;

    } else if (err != NVS_CFG_OK) {
        return PIN_AUTH_ERR_NVS;

    } else {
        /* Normal boot: salt present. Check for partial-write recovery. */
        uint8_t probe[PIN_HASH_LEN];
        size_t  probe_len = PIN_HASH_LEN;
        if (nvs_cfg_get_blob(NVS_NS_ACCESS, KEY_HASH_FARMER, probe, &probe_len) != NVS_CFG_OK ||
            probe_len != PIN_HASH_LEN) {
            /* Hash missing or corrupt — rewrite defaults with existing salt. */
            if (write_default_hashes() != PIN_AUTH_OK)
                return PIN_AUTH_ERR_NVS;
        }
    }

    s_initialized = true;
    return PIN_AUTH_OK;
}

/**
 * @brief Verify an entered PIN against the stored hash. See pin_auth.h.
 *
 * Sequence:
 *   1. Validate args (init, non-null pin, correct digit length).
 *   2. Lockout check: read lockout expiry; if still active, return
 *      PIN_AUTH_LOCKED_OUT. If expired, clear lockout + counter and proceed.
 *   3. Hash the entered PIN with s_salt; constant-time compare against the
 *      stored hash blob from NVS.
 *   4. On match: zero the failure counter and return PIN_AUTH_OK.
 *   5. On miss: increment failure counter; arm lockout if the threshold is
 *      reached. Return PIN_AUTH_WRONG (or PIN_AUTH_LOCKED_OUT on threshold).
 *
 * @param role  PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @param pin   ASCII digit string; length must match `role`.
 * @return      PIN_AUTH_OK, PIN_AUTH_WRONG, PIN_AUTH_LOCKED_OUT, or error.
 * @note   Every call writes at least one int32 to NVS (counter reset or
 *         increment); callers that probe speculatively will wear flash.
 */
pin_auth_result_t pin_auth_verify(pin_role_t role, const char *pin)
{
    if (!s_initialized)         return PIN_AUTH_ERR_INIT;
    if (pin == NULL)            return PIN_AUTH_ERR_PARAM;
    if (strlen(pin) != required_digits(role)) return PIN_AUTH_ERR_PARAM;

    /* --- Lockout check ---------------------------------------------------- */
    int32_t lockout_until = 0;
    nvs_cfg_get_i32(NVS_NS_ACCESS, lockout_key(role), &lockout_until);
    if (lockout_until != 0) {
        int32_t now = (int32_t)time(NULL);
        if (now < lockout_until)
            return PIN_AUTH_LOCKED_OUT;
        /* Lockout expired — reset counter. */
        nvs_cfg_set_i32(NVS_NS_ACCESS, lockout_key(role), 0);
        nvs_cfg_set_i32(NVS_NS_ACCESS, fail_key(role), 0);
    }

    /* --- Hash the entered PIN and compare --------------------------------- */
    uint8_t entered_hash[PIN_HASH_LEN];
    compute_hash(s_salt, pin, entered_hash);

    uint8_t stored_hash[PIN_HASH_LEN];
    size_t  stored_len = PIN_HASH_LEN;
    if (nvs_cfg_get_blob(NVS_NS_ACCESS, hash_key(role), stored_hash, &stored_len) != NVS_CFG_OK ||
        stored_len != PIN_HASH_LEN)
        return PIN_AUTH_ERR_NVS;

    if (hash_equal(entered_hash, stored_hash, PIN_HASH_LEN)) {
        /* Correct PIN — reset failure counter. */
        nvs_cfg_set_i32(NVS_NS_ACCESS, fail_key(role), 0);
        return PIN_AUTH_OK;
    }

    /* --- Wrong PIN — increment failure counter, trigger lockout if needed - */
    int32_t lockout_max  = PIN_LOCKOUT_MAX_DEFAULT;
    int32_t lockout_secs = PIN_LOCKOUT_SECS_DEFAULT;
    nvs_cfg_get_i32(NVS_NS_ACCESS, KEY_LOCKOUT_MAX,  &lockout_max);
    nvs_cfg_get_i32(NVS_NS_ACCESS, KEY_LOCKOUT_SECS, &lockout_secs);

    int32_t fail_count = 0;
    nvs_cfg_get_i32(NVS_NS_ACCESS, fail_key(role), &fail_count);
    fail_count++;
    nvs_cfg_set_i32(NVS_NS_ACCESS, fail_key(role), fail_count);

    if (fail_count >= lockout_max) {
        int32_t expiry = (int32_t)time(NULL) + lockout_secs;
        nvs_cfg_set_i32(NVS_NS_ACCESS, lockout_key(role), expiry);
        return PIN_AUTH_LOCKED_OUT;
    }

    return PIN_AUTH_WRONG;
}

/**
 * @brief Replace the stored PIN hash for the given role. See pin_auth.h.
 *
 * Computes SHA-256(s_salt || new_pin) and overwrites the role's hash blob
 * in NVS. Does not touch lockout state or the failure counter.
 *
 * @param role     PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @param new_pin  ASCII digit string of the correct length for `role`.
 * @return         PIN_AUTH_OK or PIN_AUTH_ERR_PARAM / PIN_AUTH_ERR_NVS / PIN_AUTH_ERR_INIT.
 * @warning Caller MUST enforce role permissions before invoking — farmer
 *          must not be allowed to call this with PIN_ROLE_ADMIN.
 */
pin_auth_result_t pin_auth_set(pin_role_t role, const char *new_pin)
{
    if (!s_initialized)                      return PIN_AUTH_ERR_INIT;
    if (new_pin == NULL)                     return PIN_AUTH_ERR_PARAM;
    if (strlen(new_pin) != required_digits(role)) return PIN_AUTH_ERR_PARAM;

    uint8_t hash[PIN_HASH_LEN];
    compute_hash(s_salt, new_pin, hash);

    if (nvs_cfg_set_blob(NVS_NS_ACCESS, hash_key(role), hash, PIN_HASH_LEN) != NVS_CFG_OK)
        return PIN_AUTH_ERR_NVS;

    return PIN_AUTH_OK;
}

/**
 * @brief Reset the administrator PIN to the factory default. See pin_auth.h.
 *
 * Recovery path used after the hardware jumper procedure (TSDS §5.4).
 * Rewrites the admin hash with PIN_DEFAULT_ADMIN under the existing salt,
 * then clears both the admin failure counter and the admin lockout expiry.
 * Does not touch the farmer PIN, the salt, or the farmer counters.
 *
 * @return PIN_AUTH_OK on success, PIN_AUTH_ERR_NVS on NVS failure,
 *         PIN_AUTH_ERR_INIT if pin_auth_init() has not been called.
 * @warning Should only be reachable through the hardware recovery procedure;
 *          a software-only path would compromise the access model.
 */
pin_auth_result_t pin_auth_reset_admin(void)
{
    if (!s_initialized) return PIN_AUTH_ERR_INIT;

    uint8_t hash[PIN_HASH_LEN];
    compute_hash(s_salt, PIN_DEFAULT_ADMIN, hash);

    if (nvs_cfg_set_blob(NVS_NS_ACCESS, KEY_HASH_ADMIN, hash, PIN_HASH_LEN) != NVS_CFG_OK)
        return PIN_AUTH_ERR_NVS;

    /* Also clear any active lockout and failure counter for admin. */
    nvs_cfg_set_i32(NVS_NS_ACCESS, KEY_FAIL_ADMIN,    0);
    nvs_cfg_set_i32(NVS_NS_ACCESS, KEY_LOCKOUT_ADMIN, 0);

    return PIN_AUTH_OK;
}

/**
 * @brief Return seconds remaining in the lockout period for a role.
 *        See pin_auth.h.
 *
 * Reads the per-role lockout expiry from NVS and subtracts the current
 * Unix timestamp. Returns 0 if no lockout is active (either never set or
 * already expired in wall-clock time).
 *
 * @param role  PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @return      Seconds until lockout expires, or 0 if not currently locked.
 * @note   This is purely informational; the next pin_auth_verify() call
 *         will detect the expiry and clear the lockout itself.
 */
uint32_t pin_auth_lockout_remaining_secs(pin_role_t role)
{
    if (!s_initialized) return 0;

    int32_t lockout_until = 0;
    nvs_cfg_get_i32(NVS_NS_ACCESS, lockout_key(role), &lockout_until);
    if (lockout_until == 0) return 0;

    int32_t remaining = lockout_until - (int32_t)time(NULL);
    return (remaining > 0) ? (uint32_t)remaining : 0;
}
