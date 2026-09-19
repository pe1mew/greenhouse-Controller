# Sun and outdoor T/RH sensors: are they worth adding to the controller?

| Field | Value |
|---|---|
| Question | Would a sun sensor and a dedicated outdoor T/RH sensor, wired to the controller, benefit the greenhouse? |
| Status | **Study, 2026-09-19.** Nothing is built and nothing is decided |
| Evidence | Controller 5C88's summer 2026: its SD logs, 2026-06-04 to 09-17, with the LoRa outdoor sensor `lht65-20` (T, RH, lux) converted from UTC. `python model/closedloop/campaign_figures.py --only SENSORS` prints every number on this page |
| Related | [`refactorSensorConfiguration.md`](refactorSensorConfiguration.md) (how a sensor is added); [`ventModelContract.md`](ventModelContract.md) (what a control law may read); [`model/closedloop/modelQuality.md`](../model/closedloop/modelQuality.md) (the simulator that can test a law that uses them) |

## Short answer

**Yes, but only once a control law reads them**, and the three measurements are not equally valuable:
- **Outdoor temperature: the strongest case.** How far the same M3 opening cools the house varies by a factor of six with the difference between inside and outside.
- **Sun: earlier warning.** A sun sensor sees what the controller now notices only 8-10 minutes later.
- **Outdoor RH: little effect in summer.** It matters in cool, damp weather, which this summer's data does not cover.
- **The model gains from all three at once.**

## What the controller knows today

The controller reads:
- **indoor T and RH:** FG6485A, Modbus address 1;
- **wind:** S200, address 44, outdoors on a mast;
- **M3's position:** the encoder at address 40 (observe-only);
- **day or night:** computed from sunrise and sunset at the site's coordinates.

It has no outdoor temperature, humidity or sun, and neither does the control law's input: `vent_in_t` carries no outdoor field. Those three reach only the model, through the LoRa sensor `lht65-20`, once every 10 minutes, via a separate database.

## The evidence

### Outdoor temperature: the strongest case

The same M3 opening cools the house by very different amounts, depending on how much warmer the inside is than the outside when it opens:

| Inside minus outside when M3 opens | Daytime openings | Median drop after 25 min |
|---|---|---|
| 0-4 °C | 30 | −0.8 °C |
| 4-7 °C | 131 | −1.6 °C |
| 7-10 °C | 109 | −3.4 °C |
| over 10 °C | 19 | −5.1 °C |

That is a factor of six over 289 openings (r = −0.61). Today's law opens M3 fully whatever it is outside, so on a cool day each opening overshoots.

**The swing inside a cycle follows it too.** Over 73 cycling days, the day's swing correlates with the daytime inside-outside difference (r = 0.42) and inversely with the outdoor temperature (r = −0.36).

**A law that knows the outdoor temperature can act on this:**
- **with today's binary windows:** open a roof window instead of M3 when the difference is large;
- **with the position sensor (mode 2):** open M3 only as far as the difference calls for.

### Sun: earlier warning

The controller acts 8-10 minutes after the air changes:
- the probe's reading lags the air by 3.5-5.5 min (the sensor stage, NS-10);
- 5C88 averages temperature over 3 minutes before it decides;
- M3 takes about 3 minutes to open (171 + 5 s).

A sun sensor sees the cause (a cloud passing, the sun coming out) minutes before the probe sees its effect, so a law could act before the temperature moves instead of after. Days with more variable sun swing more (r = 0.36), which fits a controller that reacts late to clouds.

**The LoRa sensor is too coarse for this.** Its lux arrives once every 10 minutes; daytime lux has a median of 19 300 and a 90th percentile of 33 800. Clouds pass in minutes. A controller-side sensor read at every 30-s poll would resolve them.

**A pyranometer rather than a lux sensor.** A pyranometer measures the heat input in W/m², which is what drives the greenhouse. Visible light tracks it only roughly.

### Outdoor RH: little effect in summer

Over the summer the controller ventilated for 1 316 hours:
- **humidity raised the ventilation step above temperature's** for 273 of them;
- **the outside air was at least as moist as the inside** for only 6 of those 273;
- **humidity alone drove ventilation** for 4 hours, never while the outside air was the wetter.

So the rule an outdoor RH sensor enables would have changed almost nothing this summer: do not ventilate for humidity when the outside air is wetter. In summer the outside air is nearly always drier than the inside in absolute terms. The rule matters in cool, damp weather: fog, rain, spring and autumn mornings. The spring campaign's scenario S4 (T 5-8 °C, RH 95-100 %) is such a case. This summer's data cannot show it.

### The model gains from all three, at once

Today these readings reach the model through a LoRa sensor and a separate database. This summer that route cost a lot:
- **the database stamps in UTC,** and every join made before 2026-09-18 was two hours out ([`model/lora_time.py`](../model/lora_time.py));
- **a LoRa sensor on the same network,** door 1's, was silent for 34 days;
- **the lux arrives every 10 minutes.**

On the controller's own bus, all three would be logged in the SD log every 30 s, on the same clock as everything else:
- **sharper plant fits,** above all of the solar gain;
- **forced tests that can tell the sun's effect from the wind's.** The NS-9 tests on the north-wind effect have to be run at matched or low sun ([`model/thermalProfileCampaign.md`](../model/thermalProfileCampaign.md) §9.12.6), and the two forced-test days so far differed in sun ([`model/campaignResults_summer2026.md`](../model/campaignResults_summer2026.md), NS-9). A sun reading at the controller, every 30 s, would show whether a test's sun really was matched.

## What it would take

- **Outdoor T/RH hardware.** The same sensor model as the indoor one (FG6485A), at its own Modbus address, so the firmware reuses the existing driver. It needs a radiation shield, and must sit away from the vents' exhaust air.
- **Sun hardware.** A Modbus pyranometer: level, unshaded, and clear of the greenhouse structure.
- **Mounting.** Both can go on the existing S200 wind mast and share its RS485 cable run.
- **The bus.** Two more slaves in T5's poll. This year's lessons apply to each new caller and slave: the 20-ms inter-frame gap, busy handling, and the out-of-spec slaves of gh#66 and gh#68 ([`addModbusMutex.md`](addModbusMutex.md)). [`refactorSensorConfiguration.md`](refactorSensorConfiguration.md) is the plan for adding sensors: roles, `dm_measure()`, and a static sensor table.
- **Failure behaviour.** Unlike the wind sensor, a failed outdoor sensor must never close the greenhouse. The law falls back to today's behaviour, and the contract's validity flags carry the fault.
- **Firmware and tools:**
  - T5 polling;
  - new SENSOR_HR channels, which `log/logparser.py`, `plot_daily.py` and the model's loaders must learn;
  - the web GUI's display;
  - new `vent_in_t` fields with their validity flags, added to [`ventModelContract.md`](ventModelContract.md) (§3a says how an input is added).
- **None of it changes control on its own.** The stepped law up to 2.10.0 ignores anything it is not given. The benefit to the greenhouse arrives only with a law that uses the new inputs.

## Recommendation

1. **Fit outdoor T/RH first:** the strongest evidence, and the cheapest hardware.
2. **Add the pyranometer second.**
3. **Log only, at first.** The model gains at once, and the next forced tests become cleaner.
4. **Measure the benefit in the simulator before changing the law.** For example, run a law that opens a roof window instead of M3 when the inside-outside difference is large. Run it against both adopted plants over this summer, with the LoRa outdoor data as a stand-in, and compare the swing, the openings and the hours above 31 °C. Treat a predicted swing reduction with caution: the model already under-states the swing on north-wind days ([`modelQuality.md`](../model/closedloop/modelQuality.md)).

## Regenerating the numbers

```bash
python model/closedloop/campaign_figures.py --only SENSORS
```

The 8-10 minute delay is not computed there. It adds up figures documented elsewhere: the sensor stage (NS-10, [`thermalProfileCampaign.md`](../model/thermalProfileCampaign.md) §9.13.2), 5C88's `avg_win_t` of 3 min ([`model/closedloop/README.md`](../model/closedloop/README.md), "Settings"), and M3's travel of 171 s plus the 5-s margin.
