/**
 * @file status_post_stub.cpp
 * @brief Phase-6.12 stub for status_post.cpp functions called by
 *        T8 (ui_display) before the full status_post.cpp is activated.
 *
 * T8 (ui_display.cpp:764) calls status_post_backoff_active() to decide
 * whether to suffix the LCD network status display with "BK" (back-off
 * active). Until the full status_post task ports in Phase 6.N (where it
 * replaces the alpha.4 https_tickle), this stub always returns false —
 * matching the production semantics during the dormant pre-soak period
 * when no status server traffic is yet being driven.
 *
 * Designed for forcing-removal: when the real status_post.cpp activates
 * (likely Phase 6.N final), the linker will refuse two definitions of
 * status_post_backoff_active() — same pattern as data_manager_stub.cpp
 * (Phase 6.6→6.7) and relay_controller_stub.cpp (Phase 6.7→6.9).
 *
 * @author Greenhouse Controller project
 */

#include "status_post.h"   /* status_post_backoff_active declaration */

bool status_post_backoff_active(void)
{
    return false;
}
