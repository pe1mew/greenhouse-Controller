# 2.0.0-rc.1.5.5 — release notes

**Date built:** 2026-05-29
**Built on top of:** 2.0.0-rc.1.5.4 (web-GUI ↔ LCD agreement on NTP indicator)
**Closes:** the motor-alarm EMI hardening campaign — adds the third defense layer (silicon-level glitch filter) alongside rc.1.5.3's IRAM rate-limit + Tmr Svc stack bump and the 1.8 kΩ hardware shunt on the alarm-input pad
**Scope:** six lines of code in `relay_controller.cpp`'s T2 init block to install ESP-IDF's IO MUX pin glitch filter on GPIO42. Transparent to the rest of the alarm pipeline.

---

## Why a patch bump (1.5.4 → 1.5.5)

Six-line firmware addition in `task_relay_controller`'s init block. No new task, no API change, no SD-log format change, no web-asset content change. Patch bump.

---

## What changed

### IO MUX pin glitch filter on GPIO42

ESP-IDF exposes `gpio_new_pin_glitch_filter()` for ESP32-S3 — a silicon-level filter at the IO MUX that drops pulses shorter than ~2 IO-MUX clock cycles (~25 ns at 80 MHz APB) **before they reach the GPIO matrix interrupt logic**. Sub-25 ns pulses — the bulk of capacitively-coupled EMI on long unterminated cables — are dropped at silicon, costing zero CPU cycles. Genuine mechanical alarm-contact transitions settle in ≥ 1 ms so they pass through unaffected.

```c
gpio_pin_glitch_filter_config_t glitch_cfg = {
    .clk_src  = GLITCH_FILTER_CLK_SRC_DEFAULT,
    .gpio_num = (gpio_num_t)PIN_OPTO_INPUT,
};
esp_err_t gf_rc = gpio_new_pin_glitch_filter(&glitch_cfg, &s_alarm_glitch_filter);
if (gf_rc == ESP_OK) {
    ESP_ERROR_CHECK(gpio_glitch_filter_enable(s_alarm_glitch_filter));
    ESP_LOGI(TAG, "GPIO42 pin glitch filter enabled (~25 ns IO MUX drop)");
}
```

Failure to create the filter is non-fatal — the rate-limit + 75 ms task debounce remain in place, just less robustly; a warning is logged and operation continues.

### The four-layer defense stack on motor-alarm input

| Layer | Origin | Function |
|---|---|---|
| 1.8 kΩ hardware shunt at J10 | board mod 2026-05-29 | amplitude floor; clamps GPIO node to ≤ 3.21 V; lowers source impedance from internal 45 kΩ → ~643 Ω |
| **IO MUX pin glitch filter (~25 ns)** | **this release** | **duration floor at silicon** |
| IRAM ISR rate-limit (5 ms) | rc.1.5.3 | application-level edge throttle |
| Task debounce (75 ms) | rc.1.4 baseline | live pin re-read to reconcile against EG1 state |

For an EMI event to now produce a spurious alarm onset it must clear *all four* floors simultaneously — amplitude > V_IL/V_IH transition margin via the divider, duration > 25 ns at silicon, sustained > 5 ms inter-edge spacing through IRAM, and *still* be in the asserted state when the task re-reads 75 ms later. Real alarm transitions clear all four by construction.

### Tmr Svc stack still at 4 KB

`CONFIG_FREERTOS_TIMER_TASK_STACK_DEPTH=4096` from rc.1.5.3 carries forward — defense-in-depth against any residual edge storm reaching the kernel despite the three preceding layers.

---

## Verified

Build + OTA verified on unit 5C88 (now back in production hardware after the cross-mount diagnostic):
- `fw_ver = 2.0.0-rc.1.5.5`
- `bank = A, accepted = true`
- Boot log shows `GPIO42 pin glitch filter enabled (~25 ns IO MUX drop)` — confirms `gpio_new_pin_glitch_filter()` returned OK
- No motor-alarm onset rows logged since deployment (EMI floor is now too high to clear)

---

## Scope notes

The pin glitch filter is the *cheapest* hardware filter ESP-IDF exposes for ESP32-S3 — fixed-window, single-pin, no Kconfig surface, no IRAM cost. ESP-IDF also exposes a *flex* glitch filter with programmable window width if the 25 ns window ever proves insufficient; not needed today given the four-layer stack but worth documenting as the next step.

The 1.8 kΩ shunt is hardware-only — applied per-board to the J10 alarm input header on the LOLIN. Not all units have it. Units without the shunt are still protected by the three software layers, but the *amplitude floor* is then driven only by the GPIO's internal 45 kΩ pull-up — significantly weaker source impedance against EMI. Recommended for any deployment in close proximity to motor contactors.
