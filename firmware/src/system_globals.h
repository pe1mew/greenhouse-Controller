/**
 * @file system_globals.h
 * @brief Phase-6.1 FreeRTOS-infrastructure bootstrap — single init call.
 *
 * `types/app_types.h` declares every queue (Q1..Q6), mutex (MX1..MX5), event
 * group (EG1) and task handle (task_t1..task_t15) as `extern`. The arduino-era
 * `main.cpp` provided the matching definitions and `xQueueCreate` /
 * `xSemaphoreCreateMutex` / `xEventGroupCreate` calls in its `setup()`.
 *
 * v2.0.0's task activation happens incrementally (Phase 6.1..6.13). To let
 * each task's .cpp compile and link without dragging in the full main.cpp,
 * we decouple "FreeRTOS infrastructure exists" from "tasks are running".
 * This file declares the bootstrap entry point; `system_globals.cpp`
 * defines all the global symbols and `system_globals_init()` creates the
 * underlying queue / mutex / event-group objects at boot.
 *
 * ## Lifecycle
 *  - `system_globals_init()` must be called ONCE from `app_main()` before any
 *    task that uses Q1..Q6 / MX1..MX5 / EG1 is spawned.
 *  - The init call is idempotent; a second invocation logs a warning and
 *    returns 0 without recreating any object.
 *  - Task handles (`task_t1`..`task_t15`) are initialised to NULL by static
 *    linkage. Each subsequent Phase-6.N alpha adds an
 *    `xTaskCreatePinnedToCore` for one task and assigns its handle.
 *
 * @see system_globals.cpp for queue depths and mutex semantics.
 * @see types/app_types.h for the `extern` declarations this module backs.
 *
 * @author Greenhouse Controller project
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Create all FreeRTOS queues, mutexes, and the system event group.
 *
 * Allocates Q1..Q6, MX1..MX5 and EG1 from internal heap and assigns the
 * resulting handles to the matching globals declared in `app_types.h`.
 *
 * Idempotent: safe to call multiple times. The second and subsequent calls
 * detect that Q1 is already set, log a warning, and return 0 without
 * recreating any object. Should be called ONCE from `app_main()`, before
 * any task that uses these primitives is spawned.
 *
 * Heap cost on success: roughly 1.2 KB internal heap (queue storage +
 * mutex semaphore structures + event group).
 *
 * @return 0 on success.
 * @return -1 if any of Q1..Q6 creation returned NULL (out of heap).
 * @return -2 if any of MX1..MX5 creation returned NULL (out of heap).
 * @return -3 if EG1 creation returned NULL (out of heap).
 * @note  An error return leaves partially-initialised globals; callers
 *        should treat this as a fatal boot condition.
 * @see   system_globals.cpp for the queue depth + mutex semantics tables.
 */
int system_globals_init(void);

#ifdef __cplusplus
}
#endif
