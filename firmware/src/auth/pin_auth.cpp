/**
 * @file pin_auth.cpp
 * @brief PIN authentication implementation — see pin_auth.h for full description.
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
#define KEY_SALT           "pin_salt"
#define KEY_HASH_FARMER    "pin_farmer_hash"
#define KEY_HASH_ADMIN     "pin_admin_hash"
#define KEY_FAIL_FARMER    "fail_cnt_f"
#define KEY_FAIL_ADMIN     "fail_cnt_a"
#define KEY_LOCKOUT_FARMER "lockout_f"
#define KEY_LOCKOUT_ADMIN  "lockout_a"
#define KEY_LOCKOUT_MAX    "lockout_max"
#define KEY_LOCKOUT_SECS   "lockout_secs"

/* ---------------------------------------------------------------------------
 * Module state — loaded once at init, read-only thereafter
 * --------------------------------------------------------------------------- */
static uint8_t s_salt[PIN_SALT_LEN];
static bool    s_initialized = false;

/* ---------------------------------------------------------------------------
 * Internal helpers
 * --------------------------------------------------------------------------- */

/** Compute SHA-256(salt || pin) into hash_out[PIN_HASH_LEN]. */
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

/** Constant-time comparison of two fixed-length byte arrays. */
static bool hash_equal(const uint8_t *a, const uint8_t *b, size_t len)
{
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff |= a[i] ^ b[i];
    return diff == 0;
}

/** Return the NVS hash key name for a role. */
static const char *hash_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_HASH_FARMER : KEY_HASH_ADMIN;
}

/** Return the NVS failure-counter key name for a role. */
static const char *fail_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_FAIL_FARMER : KEY_FAIL_ADMIN;
}

/** Return the NVS lockout-expiry key name for a role. */
static const char *lockout_key(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? KEY_LOCKOUT_FARMER : KEY_LOCKOUT_ADMIN;
}

/** Return the required digit count for a role. */
static size_t required_digits(pin_role_t role)
{
    return (role == PIN_ROLE_FARMER) ? PIN_FARMER_DIGITS : PIN_ADMIN_DIGITS;
}

/** Write default hashes for both roles using the current s_salt. */
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

uint32_t pin_auth_lockout_remaining_secs(pin_role_t role)
{
    if (!s_initialized) return 0;

    int32_t lockout_until = 0;
    nvs_cfg_get_i32(NVS_NS_ACCESS, lockout_key(role), &lockout_until);
    if (lockout_until == 0) return 0;

    int32_t remaining = lockout_until - (int32_t)time(NULL);
    return (remaining > 0) ? (uint32_t)remaining : 0;
}
