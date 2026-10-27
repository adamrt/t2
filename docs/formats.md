# File Formats

Terrarium's own formats are small and built so programs can read them a
piece at a time, without holding a whole file in memory. Tools in
`system/tools/` bring files in from the outside world.

| Format | Extension | Described in                                   |
| ------ | --------- | ---------------------------------------------- |
| TRX2   | none      | [executable.md](executable.md): programs       |
| TFS1   | `.img`    | [filesystem.md](filesystem.md): disks          |
| TFN1   | `.tfn`    | [devices.md](devices.md#gpu): bitmap fonts     |
| TIM1   | `.tim`    | below: images                                  |
| TSN1   | `.tsn`    | below: sounds                                  |

## TIM1 images

An image is cut into 64×64-pixel tiles, row by row; tiles on the right and
bottom edges are smaller when the image doesn't divide evenly. Each tile is
compressed on its own, so a program can decode just the tiles it shows.

| Offset | Field      | Meaning                                          |
| ------ | ---------- | ------------------------------------------------ |
| 0      | magic      | the bytes `TIM1`                                 |
| 4      | width      | in pixels                                        |
| 8      | height     | in pixels                                        |
| 12     | tile size  | 64                                               |
| 16     | table      | one word per tile: where its bytes start, from the start of the file; then one more word, where the last tile ends |

All words are little-endian.

A tile's pixels go left to right, top to bottom, as a stream of operations.
Each operation makes one or more pixels from the one before it, which starts
as opaque black, and from 64 remembered pixels, which start as zeros. Every
pixel made is remembered in slot `(r * 3 + g * 5 + b * 7 + a * 11) % 64`.

| Byte          | Operation                                                    |
| ------------- | ------------------------------------------------------------ |
| `00`–`3f`     | the remembered pixel in this slot                            |
| `40`–`7f`     | the previous pixel plus small differences: bits 5–4 red, 3–2 green, 1–0 blue, each minus 2 |
| `80`–`bf`     | green changes by the low six bits minus 32; the next byte's high and low halves, minus 8, are how much more red and blue change than green |
| `c0`–`fd`     | the previous pixel again, the low six bits plus one times    |
| `fe`          | red, green and blue follow; alpha stays                      |
| `ff`          | red, green, blue and alpha follow                            |

Channels wrap around within a byte. The operations are the ones the QOI
format uses; the tiles and table are Terrarium's.

## TSN1 sounds

A sound is 16-bit mono samples, each squeezed to 4 bits, in blocks of 256
bytes that each decode on their own, so a player can start anywhere and
needs only a block at a time.

| Offset | Field      | Meaning                                          |
| ------ | ---------- | ------------------------------------------------ |
| 0      | magic      | the bytes `TSN1`                                 |
| 4      | rate       | samples per second, at most 192000               |
| 8      | samples    | how many samples there are                       |
| 12     | block size | 256                                              |
| 16     | blocks     | one after another                                |

A full block holds 505 samples; the last block holds the rest and is
shorter. A block starts with its first sample as a signed 16-bit word, then
a byte saying which step size comes next (0–88), then a zero byte. Every
following byte holds the codes for two more samples, the low four bits
first, so a block of `n` samples takes `4 + n / 2` bytes.

Each code moves the sample by part of the current step size, and makes the
next step bigger or smaller:

```
diff = step / 8
if code & 1: diff += step / 4
if code & 2: diff += step / 2
if code & 4: diff += step
sample = sample - diff if code & 8 else sample + diff   (kept within -32768..32767)
step number -= 1 if code & 7 < 4 else step number += 2 * (code & 7 - 3)   (kept within 0..88)
```

The 89 step sizes start at 7, and each is the one before times 1.1, rounded
down, but at least one more. This is IMA ADPCM, with Terrarium's own step
sizes and file; `play` plays a sound at any rate, and `mksound` makes one
from a WAV file.
