"""
lora_time.py -- the Wenumseveld LoRa database stamps its rows in UTC.

Every `dateTime` the database returns (lht65-20, lds01-5/6, LHT65-02/03, ...)
is UTC, not Europe/Amsterdam local time. The controller's SD logs are local
time. So a LoRa row must be converted before it is joined to an SD sample,
or the join pairs each indoor reading with outdoor data from two hours later
in summer (one hour in winter).

Verified 2026-09-18, two independent ways, over the summer of 2026:
  * lht65-20's daylight (lux >= 1000) is centred 124 min before the day the
    controller's logged sunrise and sunset define (median of 91 days, IQR
    -130..-118 min); after converting, -4 min (IQR -10..+2).
  * The indoor LHT65-02 and LHT65-03 temperature changes match the
    controller's FG6485A best at a +120 min lag (r = 0.86 and 0.88), against
    r = 0.16 and 0.19 at zero lag.

Until that date fetch_lora_data.py documented the stamps as local time and
prepare_calibration_input.py joined them unconverted, so every
calibration_input_*.csv built before 2026-09-18 carries outdoor and door
data two hours late -- the summer-2026 calibration included.

utc_to_local() applies the EU summer-time rule directly (from the last Sunday
of March 01:00 UTC to the last Sunday of October 01:00 UTC), because Windows
Python has no time-zone database without the tzdata package.
"""

from datetime import datetime, timedelta


def _last_sunday(year, month):
    """The last Sunday of a month, as a date-time at midnight."""
    first_of_next = datetime(year + (month == 12), month % 12 + 1, 1)
    last = first_of_next - timedelta(days=1)
    return last - timedelta(days=(last.weekday() + 1) % 7)


def utc_to_local(u):
    """Naive UTC date-time -> naive Europe/Amsterdam local date-time."""
    start = _last_sunday(u.year, 3).replace(hour=1)
    end = _last_sunday(u.year, 10).replace(hour=1)
    return u + timedelta(hours=2 if start <= u < end else 1)
