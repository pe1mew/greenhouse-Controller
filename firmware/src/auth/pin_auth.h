/**
 * @file pin_auth.h
 * @brief PIN authentication — salted SHA-256 hashing, NVS persistence, lockout.
 *
 * Implements the three-state access model (Normal / Farmer / Administrator)
 * described in TSDS §5.4.
 *
 * ## Hashing scheme
 *   hash = SHA-256(salt || pin_digits_as_ascii)
 *   - salt : 16 random bytes generated once at first boot via esp_fill_random();
 *             stored in NVS "access/pin_salt" as a binary blob.
 *   - hash : 32-byte SHA-256 digest stored in NVS "access/pin_farmer_hash" or
 *             "access/pin_admin_hash" as binary blobs.
 *   - Plain-text PINs are never stored or logged (FR-AC06).
 *   - mbedTLS SHA-256 is used (mbedtls/sha256.h, bundled with ESP-IDF — no
 *     additional lib_deps entry required).
 *
 * ## NVS keys (namespace "access")
 *   pin_salt         blob[16]  — shared salt for both PINs
 *   pin_farmer_hash  blob[32]  — SHA-256 hash of the farmer PIN
 *   pin_admin_hash   blob[32]  — SHA-256 hash of the admin PIN
 *   fail_cnt_f       int32     — consecutive farmer failures since last success
 *   fail_cnt_a       int32     — consecutive admin failures since last success
 *   lockout_f        int32     — farmer lockout expiry (Unix timestamp; 0 = none)
 *   lockout_a        int32     — admin lockout expiry (Unix timestamp; 0 = none)
 *   lockout_max      int32     — max failures before lockout (default 5)
 *   lockout_secs     int32     — lockout duration in seconds (default 300)
 *
 * ## Thread safety
 *   pin_auth_init() must be called once from setup() before tasks start.
 *   pin_auth_verify() / pin_auth_set() are safe to call from T8 or T11;
 *   concurrent calls from both cores simultaneously could produce a rare
 *   double-decrement of the lockout counter, but this is operationally
 *   inconsequential and no additional mutex is introduced.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#include <stdint.h>

/* ---------------------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------------------- */

/** @brief Length of the random salt in bytes. */
#define PIN_SALT_LEN           16

/** @brief Length of the SHA-256 digest in bytes. */
#define PIN_HASH_LEN           32

/** @brief Required digit count for the farmer PIN. */
#define PIN_FARMER_DIGITS       4

/** @brief Required digit count for the administrator PIN. */
#define PIN_ADMIN_DIGITS        8

/** @brief Factory-default farmer PIN (set at first boot; farmer should change). */
#define PIN_DEFAULT_FARMER     "1234"

/** @brief Factory-default administrator PIN (set at first boot; admin must change). */
#define PIN_DEFAULT_ADMIN      "12345678"

/** @brief Default maximum consecutive failures before lockout. */
#define PIN_LOCKOUT_MAX_DEFAULT    5

/** @brief Default lockout duration in seconds (5 minutes). */
#define PIN_LOCKOUT_SECS_DEFAULT  300

/* ---------------------------------------------------------------------------
 * Types
 * --------------------------------------------------------------------------- */

/** @brief Role selector used by verify / set functions. */
typedef enum {
    PIN_ROLE_FARMER = 0,  /**< 4-digit farmer PIN. */
    PIN_ROLE_ADMIN  = 1,  /**< 8-digit administrator PIN. */
} pin_role_t;

/** @brief Return codes for all pin_auth_* functions. */
typedef enum {
    PIN_AUTH_OK          =  0,  /**< Operation succeeded / PIN matched. */
    PIN_AUTH_WRONG       = -1,  /**< PIN did not match stored hash. */
    PIN_AUTH_LOCKED_OUT  = -2,  /**< Too many failures; input locked. */
    PIN_AUTH_ERR_NVS     = -3,  /**< NVS read or write failure. */
    PIN_AUTH_ERR_PARAM   = -4,  /**< Null pointer or wrong PIN length. */
    PIN_AUTH_ERR_INIT    = -5,  /**< Module not initialised; call pin_auth_init() first. */
} pin_auth_result_t;

/* ---------------------------------------------------------------------------
 * API
 * --------------------------------------------------------------------------- */

/**
 * @brief Initialise the PIN authentication module.
 *
 * Must be called once during startup, after nvs_cfg_init(), before any task
 * that performs PIN verification or management.
 *
 * Behaviour:
 *  - Reads "access/pin_salt" from NVS.
 *  - First boot (salt absent): generates a 16-byte random salt with
 *    esp_fill_random(), stores it, then writes SHA-256 hashes for
 *    PIN_DEFAULT_FARMER and PIN_DEFAULT_ADMIN using that salt.
 *  - Partial-write recovery (salt present but a hash is missing): rewrites
 *    both default hashes using the existing salt.
 *  - Normal boot (salt and hashes present): loads the salt into RAM; no writes.
 *
 * @return PIN_AUTH_OK on success, PIN_AUTH_ERR_NVS on NVS failure.
 */
pin_auth_result_t pin_auth_init(void);

/**
 * @brief Verify an entered PIN against the stored hash.
 *
 * Checks the lockout state before hashing. On a correct PIN the failure
 * counter is reset to zero. On an incorrect PIN the failure counter is
 * incremented; when it reaches the configured maximum the role is locked
 * for the configured duration.
 *
 * @param role  PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @param pin   Null-terminated ASCII digit string (exactly PIN_FARMER_DIGITS
 *              or PIN_ADMIN_DIGITS characters depending on role).
 * @return      PIN_AUTH_OK, PIN_AUTH_WRONG, PIN_AUTH_LOCKED_OUT, or error.
 */
pin_auth_result_t pin_auth_verify(pin_role_t role, const char *pin);

/**
 * @brief Replace the stored PIN hash for the given role.
 *
 * Computes SHA-256(salt || new_pin) and writes the result to NVS.
 * The caller must enforce role permissions (admin may change either PIN;
 * farmer may only change the farmer PIN).
 *
 * @param role     PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @param new_pin  Null-terminated ASCII digit string of the correct length.
 * @return         PIN_AUTH_OK or error code.
 */
pin_auth_result_t pin_auth_set(pin_role_t role, const char *new_pin);

/**
 * @brief Reset the administrator PIN to the factory default.
 *
 * Intended for the hardware recovery procedure (physical jumper + key combo
 * at power-on, TSDS §5.4). Rewrites the admin hash using the existing salt
 * and PIN_DEFAULT_ADMIN; does not disturb the farmer PIN or the salt.
 *
 * @return PIN_AUTH_OK or PIN_AUTH_ERR_NVS.
 */
pin_auth_result_t pin_auth_reset_admin(void);

/**
 * @brief Return the seconds remaining in the lockout period for a role.
 *
 * @param role  PIN_ROLE_FARMER or PIN_ROLE_ADMIN.
 * @return      Seconds until lockout expires, or 0 if not currently locked.
 */
uint32_t pin_auth_lockout_remaining_secs(pin_role_t role);
