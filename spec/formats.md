# File Formats

Terrarium's own formats are small and built so programs can read them a
piece at a time, without holding a whole file in memory. Tools in
`system/tools/` bring files in from the outside world.

| Format | Extension | Described in                                   |
| ------ | --------- | ---------------------------------------------- |
| TRX1   | none      | [executable.md](executable.md): programs       |
| TFS1   | `.img`    | [filesystem.md](filesystem.md): disks          |
| TFN1   | `.tfn`    | [devices.md](devices.md#gpu): bitmap fonts     |
| TIM1   | `.tim`    | below: images                                  |

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
