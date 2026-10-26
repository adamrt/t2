# Time: a plan

Not built yet. Today `os.time()` counts milliseconds since the machine
started, and `os.date()` gives the host's local time in seconds since 1970.
This is what time should become: two kinds of time, each in two 32-bit
words, so nothing needs 64-bit arithmetic.

## Two types

```
struct Time {           // when something happened: the wall clock
    day u32             // days since 0001-01-01
    milli u32           // milliseconds since midnight, 0 to 86,399,999
}

struct Instant {        // for measuring: the monotonic clock
    second u32          // seconds since the machine started
    nano u32            // 0 to 999,999,999
}
```

**Time** labels moments: file changes, messages, dates. It is UTC, on the
Gregorian calendar carried back to 1 January of year 1, and it reaches 11.7
million years. A date without a time of day, like a birthday, is just a day
number: 1 April 1980 is day 722,905. Day numbers work as plain numbers:
`b - a` is the days between, `d + 7` a week later, `a < b` the earlier.

Day 0 is the first day of year 1, for three reasons. It was a Monday, so
the day of the week is `day % 7` with 0 for Monday, as ISO weeks start.
There's no year 0 to explain: the calendar goes from 1 BC to AD 1, and
nothing here needs dates before that. And days 0 to 146,096 are exactly
the years 1 to 400, one whole cycle of the Gregorian calendar's leap years,
so turning a day into a date needs no correction for where it starts.

**Instant** measures: timeouts, animation, benchmarks, how long something
took. It never jumps when the host's clock is set, which the wall clock can.
Durations always come from it, never from subtracting two Times. Sound and
video keep time by counting samples and frames, never by the wall clock.

## Leap years and leap seconds

Leap years only matter when a day number becomes a year, month and day:
divisible by 4 is a leap year, by 100 isn't, by 400 is. That's a few lines
of 32-bit arithmetic.

Every day has exactly 86,400 seconds, as in Unix time: leap seconds aren't
represented. They can't be predicted from a formula, and they're being
abandoned: none will be added after 2035. Around one, the host's clock may
repeat a second or stretch it, and Time follows the host. That is harmless
for labelling moments and would be wrong for keeping time, which is why
nothing keeps time with Time. RFC 7164 reached the same conclusion for
streaming media: RTP's media clock runs steadily, and only its mapping to
wall-clock time is disturbed by leap seconds.

## UTC

Everything is UTC inside. A program that shows local time adds an offset
for display; daylight saving and time-zone rules can come later without
changing a stored Time. Time as text is ISO 8601:
`2026-10-26T18:30:00.250Z`.

## The modules

Since a struct can't be returned in the language, the calls fill one in.

```
import time
import clock

var now time.Time
time.now(&now)
var d time.Calendar                  // year, month, day, hour, minute, second, milli
time.to_calendar(&now, &d)
time.put(&w, &now)                   // 2026-10-26T18:30:00.250Z
var birthday = time.day(1980, 4, 1)  // 722,905

var start clock.Instant
clock.now(&start)
do_work()
var took = clock.millis_since(&start)
clock.sleep(500)                     // milliseconds
```

The host turns its own clock into a Time at the device. 1970-01-01 is day
719,162, so a Unix time `t` is day `719162 + t / 86400`, at
`t % 86400 * 1000` milliseconds.

## A clock for tests

Since we own the machine, the monotonic clock can follow the CPU's cycle
counter instead of the host's clock, so a program sees the same Instants
every run: replayable games, timeouts that test the same way each time. The
wall clock can already be stopped at one moment with `--date`.
