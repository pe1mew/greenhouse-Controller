
## Climate control parameters — current defaults and suggested values for general crops

The firmware has three namespaces relevant to climate control: **climate**, **wind**, and **motor** (plus `poll_interval` from **system**).

---

### `climate` namespace

| # | Key | Default | Suggested | Motivation |
|---|---|---|---|---|
| C1 | `t_max_day` | **26 °C** | **28 °C** | The default is conservative. For tomatoes, peppers, cucumbers, and lettuce the productive daytime range is up to ~28 °C; opening at 26 °C is slightly premature on sunny spring days. 28 °C avoids unnecessary ventilation when the crop is still warming up. |
| C2 | `t_max_ngt` | **22 °C** | **20 °C** | At night, most general crops prefer 16–18 °C. 22 °C allows too much heat retention. Lowering to 20 °C enables night ventilation on warm summer nights and reduces disease pressure. |
| C3 | `t_min_day` | **15 °C** | **16 °C** | Currently informational (not used by T6). Stored for future heating setpoint. 16 °C is a typical lower bound for warm-season crops; fine to raise one degree to match real-world practice. |
| C4 | `t_min_ngt` | **12 °C** | **14 °C** | Also informational/future heating. 12 °C is acceptable only for cold-hardy crops; 14 °C is more appropriate for typical greenhouse vegetables (tomato, pepper, cucumber). |
| C5 | `rh_max_day` | **80 %** | **75 %** | 80 % is at the upper edge of acceptable for fungal disease management. At 75 % RH the controller will begin ventilating sooner, reducing risk of Botrytis and powdery mildew on general crops. |
| C6 | `rh_max_ngt` | **85 %** | **80 %** | Night condensation is the primary disease risk vector. Keeping night RH below 80 % (rather than 85 %) is the recommended threshold for most vegetable crops to prevent leaf wetness and fungal infection. |
| C7 | `rh_min_day` | **40 %** | **50 %** | 40 % is too dry for most greenhouse crops; stomata close, transpiration drops, and fruit quality suffers (e.g. blossom end rot in tomato). 50 % is a well-established minimum for productive daytime conditions. |
| C8 | `rh_min_ngt` | **50 %** | **55 %** | Night humidity above 55 % is preferable over the current 50 % minimum to prevent excessive water loss through open stomata during warm nights. The controller closes windows if RH drops below this. |
| C9 | `hyst_t` | **2 °C** | **3 °C** | With 3 motors and `NUM_VENT_STEPS=3`, the step width is `hyst/3 = 1 °C` at default. That means windows open at t_max+1 °C, t_max+2 °C, t_max+3 °C. This gives more gradual ventilation and reduces overcorrection. A wider band also prevents relay cycling on slightly fluctuating temperatures near the setpoint. |
| C10 | `hyst_rh` | **5 %** | **5 %** | **No change.** 5 % is appropriate for RH; a narrower band would cause rapid oscillation in humid conditions. |
| C11 | `rh_ctrl_en` | **1** (enabled) | **1** (enabled) | **No change.** For general crops, active RH control is always beneficial. Disabling it risks fungal problems from unchecked humidity spikes. |
| C12 | `cr_priority` | **0** (T first) | **0** (T first) | **No change.** Temperature-first conflict resolution is correct for general crops: overheating is more immediately damaging than a humidity exceedance. The RH controller's close demand (too dry) will be overridden by a temperature open demand, which is the safe choice. |
| C13 | `avg_win_t` | **1 min** | **3 min** | A 1-minute window equals ~2 samples at 30 s poll interval — essentially the instantaneous reading. For general crops a 3-minute window (6 samples) smooths transient temperature spikes from door openings or direct sun, preventing unnecessary window actuation. |
| C14 | `avg_win_rh` | **1 min** | **5 min** | RH fluctuates faster than temperature and is more prone to sensor noise. A 5-minute window (10 samples at 30 s) provides stable averaged RH without lag that would matter agronomically, while filtering brief dips and spikes. |

---

### `wind` namespace

| # | Key | Default | Suggested | Motivation |
|---|---|---|---|---|
| C15 | `v_max` | **7 m/s** | **6 m/s** | Beaufort 4 (moderate breeze) begins at ~5.5 m/s. Most greenhouse window mechanisms are rated to open below Beaufort 4; 6 m/s provides a slight extra margin over the default 7 m/s without being overly conservative. For lightweight single-span structures the lower threshold protects against lateral frame stress. |
| C16 | `dir_excl_low` | **0°** (disabled) | *site-specific* | If the greenhouse has a dominant prevailing wind direction that is structurally problematic (e.g. west-facing ridge windows in Atlantic climates), set an exclusion zone. No universal value can be given — this requires on-site wind rose data. |
| C17 | `dir_excl_high` | **0°** (disabled) | *site-specific* | Same as above. |
| C18 | `wind_prot_en` | **1** (enabled) | **1** (enabled) | **No change.** Wind protection must stay enabled for all general-crop greenhouses. Disabling it risks structural damage. |

---

### `motor` namespace

| # | Key | Default | Suggested | Motivation |
|---|---|---|---|---|
| — | `travel_m1` | **21 s** | *measured* | Motor travel times are hardware-specific and must always be measured on the actual installation. The 21 s default is a placeholder. |
| — | `travel_m2` | **21 s** | *measured* | As above. |
| — | `travel_m3` | **171 s** | *measured* | Ridge vent is typically slower; 171 s is a reasonable starting point but must be verified. |
| C19 | `dwell_open_m1/m2/m3` | **0 min** | **2 min** | A short post-open dwell prevents the controller from immediately closing a window that just opened due to a fast-reading RH rise followed by a temperature drop. 2 minutes gives the air exchange time to actually affect the greenhouse before a re-evaluation closes the window. |
| C20 | `dwell_close_m1/m2/m3` | **0 min** | **0 min** | **No change.** Closing should be reactive (wind, temperature drop, rain — none of which benefit from a delay). Immediate close response is the safe default. |

---

### `system` namespace

| # | Key | Default | Suggested | Motivation |
|---|---|---|---|---|
| C21 | `poll_interval` | **30 s** | **60 s** | 30 s is fast enough for testing but in production it causes twice as many relay actuations and Modbus bus transactions as necessary. Sensor conditions in a greenhouse change on a timescale of minutes. 60 s is the standard for commercial systems and extends motor/relay service life. The averaging windows (`avg_win_t`, `avg_win_rh`) automatically recalculate sample counts when this changes. |
