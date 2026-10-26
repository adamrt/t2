# Time: a plan

Not built yet. Today `os.time()` counts milliseconds since the machine
started, and `os.date()` gives the host's local time in seconds since 1970.
This is what time should become.

## Two clocks

| Clock     | For                                   | Value                                   |
| --------- | ------------------------------------- | --------------------------------------- |
| Wall      | file times, messages, dates           | `u32` seconds since 2000-01-01 00:00 UTC |
| Monotonic | timers, animation, measuring how long | `u32` milliseconds since the machine started |

Keeping them apart matters more than the epoch. The wall clock can jump
when the host's clock is set; the monotonic clock never goes back, so
durations come from it.

Both are 32-bit because the machine is: the CPU and the language have no
64-bit integers, and adding them would cost far more than it buys. Seconds
since 2000 last until 2136. Milliseconds wrap after 49.7 days, but the
difference of two readings is still right for anything shorter, since the
subtraction wraps too.

## Timestamps and dates

A timestamp says when something happened on this machine, so it never needs
to be before 2000. A date like 1 April 1980, such as a birthday or a date
in a document, is a calendar date: a `DateTime` with its year, month, day,
hour, minute and second, which reaches any year. Turning a `DateTime`
outside 2000–2136 into a timestamp fails.

## UTC

Everything is UTC inside. A program that shows local time adds an offset
for display; daylight saving and time-zone rules can come later without
changing a timestamp. Time as text is ISO 8601: `2026-10-26T18:30:00Z`.
Leap seconds are ignored, as Unix does: every day has 86,400 seconds.

## The module

```
import time

var start = time.monotonic()
do_work()
var took = time.monotonic() - start        // milliseconds

var now = time.now()                        // seconds since 2000, UTC
var d time.DateTime
time.parts(now, &d)
time.put(&w, now)                           // 2026-10-26T18:30:00Z
time.sleep(500)
```

The host turns its own clock into seconds since 2000 at the device: Unix
time minus 946,684,800.

## A clock for tests

Since we own the machine, the monotonic clock can follow the CPU's cycle
counter instead of the host's clock, so a program sees the same times every
run: replayable games, timeouts that test the same way each time. The wall
clock can already be stopped at one moment with `--date`.
