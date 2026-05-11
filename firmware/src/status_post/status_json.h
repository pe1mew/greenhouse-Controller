/**
 * @file status_json.h
 * @brief Canonical controller-status JSON builder used by both surfaces.
 *
 * Produces the spec-shaped JSON payload defined in
 *   design/technical-spec-statusWebsite.md § 9.2
 *
 * Single source of truth for "what does the controller's status JSON look
 * like": both the local web UI (T11 /api/status, WebSocket push) and the
 * remote status-website POST task (T14) call build_canonical_status_json()
 * with the same status_snapshot_t. The expose_mask gates which top-level
 * tile objects appear; the local UI passes STATUS_EXPOSE_ALL, T14 passes
 * cfg.status_expose so the user can hide tiles from the public dashboard.
 *
 * Window-state strings drop the WIN_ prefix to match the spec
 * ("OPEN", "CLOSED", "MOVING_OPEN", "MOVING_CLOSE", "UNKNOWN"). Mode strings
 * follow op_mode_t names without the MODE_ prefix.
 */

#pragma once

#include <stdint.h>
#include <stddef.h>

#include "../types/app_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Convert a window_state_t to its spec string (no WIN_ prefix).
 * Falls back to "UNKNOWN" for out-of-range values.
 */
const char *window_state_str(window_state_t s);

/**
 * @brief Convert an op_mode_t to its public string (no MODE_ prefix).
 * Falls back to "AUTOMATIC".
 */
const char *op_mode_str(op_mode_t m);

/**
 * @brief Format a status snapshot into the canonical JSON payload.
 *
 * @param buf                       Output buffer.
 * @param cap                       Capacity of @p buf in bytes (must include
 *                                  room for the terminating NUL).
 * @param s                         Snapshot, typically filled by
 *                                  dm_status_snapshot().
 * @param expose_mask               Bitmask of STATUS_EXPOSE_* flags. Cleared
 *                                  bits cause the matching top-level object
 *                                  to be omitted entirely.
 * @param include_disabled_setpoints  When true, `rh_max_active` and
 *                                  `rh_min_active` are emitted regardless of
 *                                  `rh_ctrl_enabled` (local-UI behaviour: the
 *                                  GUI dims the rows but still wants the
 *                                  values). When false, the two RH-setpoint
 *                                  fields are omitted when RH control is
 *                                  disabled (T14 → public dashboard).
 *                                  `rh_ctrl_enabled` itself is always emitted
 *                                  so consumers know which mode is active.
 *
 * @return Number of bytes written excluding the terminating NUL, or 0 on
 *         failure (typically buf too small).
 */
size_t build_canonical_status_json(char *buf, size_t cap,
                                   const status_snapshot_t *s,
                                   uint32_t expose_mask,
                                   bool include_disabled_setpoints);

#ifdef __cplusplus
}
#endif
