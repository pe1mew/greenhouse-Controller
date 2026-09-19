/**
 * @file ventmodel_ffi.cpp
 * @brief C ABI over drivers/ventModel, for the offline closed-loop simulator.
 *
 * The simulator in model/closedloop/ runs the control law the firmware will
 * run, compiled from the same sources: drivers/ventModel/src/. This file adds
 * nothing to any law. It only gives Python's ctypes a stable, unmangled way
 * in:
 *
 *   - a model table, so the simulator picks a law by its name;
 *   - reset/step wrappers around the vent_model_t function pointers;
 *   - a layout table (every size and member offset, by C name), so the
 *     hand-written ctypes mirror of vent_model.h fails loudly on load,
 *     instead of silently reading the wrong field, when the header changes
 *     and the mirror does not.
 *
 * Adding a model to the library is one row in k_models below (and its
 * accessor in vent_model.h, which the library needs anyway).
 *
 * Built by model/closedloop/ventmodel.py; never compiled into the firmware.
 */

#include "vent_model.h"

#include <stddef.h>

#if defined(_WIN32)
#  define VM_EXPORT extern "C" __declspec(dllexport)
#else
#  define VM_EXPORT extern "C" __attribute__((visibility("default")))
#endif

typedef const vent_model_t *(*vm_accessor_t)(void);

/** Every law the library provides. Index = the id the Python side uses. */
static const vm_accessor_t k_models[] = {
    vent_model_stepped,
    vent_model_graded,
};

static const int k_model_count = (int)(sizeof(k_models) / sizeof(k_models[0]));

static const vent_model_t *model_at(int id)
{
    if (id < 0 || id >= k_model_count) {
        return NULL;
    }
    return k_models[id]();
}

VM_EXPORT int vm_api(void)
{
    return VENT_MODEL_API;
}

VM_EXPORT int vm_count(void)
{
    return k_model_count;
}

VM_EXPORT const char *vm_name(int id)
{
    const vent_model_t *m = model_at(id);
    return (m != NULL) ? m->name : NULL;
}

VM_EXPORT int vm_version(int id)
{
    const vent_model_t *m = model_at(id);
    return (m != NULL) ? (int)m->version : -1;
}

VM_EXPORT int vm_reset(int id, vent_state_t *st)
{
    const vent_model_t *m = model_at(id);
    if (m == NULL || st == NULL) {
        return -1;
    }
    m->reset(st);
    return 0;
}

VM_EXPORT int vm_step(int id, const vent_in_t *in, vent_state_t *st, vent_out_t *out)
{
    const vent_model_t *m = model_at(id);
    if (m == NULL || in == NULL || st == NULL || out == NULL) {
        return -1;
    }
    m->step(in, st, out);
    return 0;
}

/* Every struct size and member offset, keyed by its C name. ventmodel.py
 * compares its ctypes mirror against this table name by name, so a mirror
 * that swaps two same-sized members (step and step_t, say) is caught as
 * surely as one that gets a width wrong. A positional comparison missed
 * exactly that, which is why the names are here. */
typedef struct {
    const char *name;
    int         value;
} vm_layout_entry_t;

#define VM_SIZE(type)          { #type, (int)sizeof(type) }
#define VM_FIELD(type, member) { #type "." #member, (int)offsetof(type, member) }

static const vm_layout_entry_t k_layout[] = {
    VM_SIZE(vent_win_in_t),
    VM_FIELD(vent_win_in_t, state),
    VM_FIELD(vent_win_in_t, cap),
    VM_FIELD(vent_win_in_t, pos_x10),
    VM_FIELD(vent_win_in_t, pos_age_ms),
    VM_FIELD(vent_win_in_t, last_target_x10),
    VM_FIELD(vent_win_in_t, last_result),
    VM_FIELD(vent_win_in_t, ms_since_move),

    VM_SIZE(vent_in_t),
    VM_FIELD(vent_in_t, now_ms),
    VM_FIELD(vent_in_t, unix_time),
    VM_FIELD(vent_in_t, daytime),
    VM_FIELD(vent_in_t, t_c10),
    VM_FIELD(vent_in_t, t_avg_c10),
    VM_FIELD(vent_in_t, t_avg_c),
    VM_FIELD(vent_in_t, rh_pct),
    VM_FIELD(vent_in_t, rh_avg_pct),
    VM_FIELD(vent_in_t, wind_ms10),
    VM_FIELD(vent_in_t, wind_avg_ms10),
    VM_FIELD(vent_in_t, wind_dir_deg),
    VM_FIELD(vent_in_t, wind_dir_avg_deg),
    VM_FIELD(vent_in_t, wind_dir_var_deg),
    VM_FIELD(vent_in_t, t_valid),
    VM_FIELD(vent_in_t, rh_valid),
    VM_FIELD(vent_in_t, wind_valid),
    VM_FIELD(vent_in_t, t_max_c10),
    VM_FIELD(vent_in_t, rh_max_pct),
    VM_FIELD(vent_in_t, rh_min_pct),
    VM_FIELD(vent_in_t, hyst_t_c),
    VM_FIELD(vent_in_t, hyst_rh_pct),
    VM_FIELD(vent_in_t, cr_priority),
    VM_FIELD(vent_in_t, rh_ctrl_en),
    VM_FIELD(vent_in_t, m3_deadzone_x10),
    VM_FIELD(vent_in_t, m3_min_interval_ms),
    VM_FIELD(vent_in_t, win),

    VM_SIZE(vent_win_out_t),
    VM_FIELD(vent_win_out_t, action),
    VM_FIELD(vent_win_out_t, target_x10),

    VM_SIZE(vent_out_t),
    VM_FIELD(vent_out_t, win),
    VM_FIELD(vent_out_t, reason),
    VM_FIELD(vent_out_t, step),
    VM_FIELD(vent_out_t, step_t),
    VM_FIELD(vent_out_t, step_rh),
    VM_FIELD(vent_out_t, demand_t_x10),
    VM_FIELD(vent_out_t, demand_rh_x10),

    VM_SIZE(vent_state_t),
};

static const int k_layout_count = (int)(sizeof(k_layout) / sizeof(k_layout[0]));

VM_EXPORT int vm_layout_count(void)
{
    return k_layout_count;
}

VM_EXPORT const char *vm_layout_name(int i)
{
    return (i >= 0 && i < k_layout_count) ? k_layout[i].name : NULL;
}

VM_EXPORT int vm_layout_value(int i)
{
    return (i >= 0 && i < k_layout_count) ? k_layout[i].value : -1;
}
