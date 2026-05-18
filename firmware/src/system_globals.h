/**
 * @file system_globals.h
 * @brief Phase-6.1 FreeRTOS-infrastructure bootstrap.
 *
 * `app_types.h` declares every queue (Q1..Q6), mutex (MX1..MX5), event group
 * (EG1) and task handle (task_t1..task_t15) as `extern`. The arduino-era
 * `main.cpp` provided the matching definitions and `xQueueCreate` /
 * `xSemaphoreCreateMutex` / `xEventGroupCreate` calls in its `setup()`.
 *
 * v2.0.0's task activation happens incrementally (Phase 6.1..6.13). To let
 * each task's .cpp compile and link without dragging in the full main.cpp,
 * we decouple "FreeRTOS infrastructure exists" from "tasks are running".
 * This file defines all the global symbols and provides a single init call
 * that creates the underlying objects.
 *
 * Task handles are initialised to NULL. Each subsequent Phase-6.N alpha
 * adds an `xTaskCreatePinnedToCore` for one task and assigns its handle.
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
 * Idempotent: safe to call multiple times (subsequent calls log a warning
 * and no-op). Should be called ONCE from app_main, before any task that
 * uses these primitives is spawned.
 *
 * Heap cost on success: roughly 1.2 KB internal heap (queue storage +
 * mutex semaphore structures + event group).
 *
 * @return 0 on success, negative on error (any creation returned NULL).
 */
int system_globals_init(void);

#ifdef __cplusplus
}
#endif
