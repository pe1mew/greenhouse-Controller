/**
 * @file relay_controller_stub.cpp
 * @brief Phase-6.7 stub for T2 (relay_controller) functions called by
 *        T4 (data_manager) before T2 itself is activated.
 *
 * T4's `dm_status_snapshot()` calls `t2_get_window_states(out->win)` to
 * include the live window/relay positions in the status snapshot. Until
 * the real T2 ports (Phase 6.8+), this stub returns `WIN_UNKNOWN` for
 * all three channels — same "we haven't calibrated yet" state T2 itself
 * uses before its first `CLOSE_ALL` calibration sweep.
 *
 * The stub is C++-linked (matching relay_controller.h's declaration).
 * When the real `relay_controller.cpp` activates, the linker will refuse
 * two definitions of `t2_get_window_states` — forcing-removal of this
 * stub. Same pattern as `data_manager_stub.cpp` (which was removed in
 * alpha.6.7 along with this file's creation).
 *
 * @author Greenhouse Controller project
 */

#include "../types/app_types.h"     /* window_state_t */
#include "relay_controller.h"        /* t2_get_window_states declaration */

void t2_get_window_states(window_state_t out[3])
{
    out[0] = WIN_UNKNOWN;
    out[1] = WIN_UNKNOWN;
    out[2] = WIN_UNKNOWN;
}
