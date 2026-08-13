"""Verify Atrinik.GetTime() publishes the corrected one-based calendar."""

import Atrinik


expected = {
    "year": 1,
    "month": 1,
    "month_name": "Month of the Winter",
    "day": 28,
    "hour": 23,
    "minute": 0,
    "dayofweek": 7,
    "dayofweek_name": "Day of the Sun",
    "season": 1,
    "season_name": "Season of the Blizzard",
    "periodofday": 10,
    "periodofday_name": "night",
}
actual = Atrinik.GetTime()
if actual != expected:
    raise RuntimeError(
        "Atrinik.GetTime() calendar mismatch: expected {!r}, got {!r}".format(
            expected, actual
        )
    )
