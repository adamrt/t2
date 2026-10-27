# The Terrarium Language

A small language in the spirit of C: explicit types, structs, slices, no
garbage collector and no hidden allocation. Files end in `.tr`.

```
fn sum(values []u32) u32 {
    var total u32 = 0
    for i in 0..values.len {
        total += values[i]
    }
    return total
}
```

A program is a set of files compiled together, along with the
[modules](#modules) they import. Every top-level name of the program is
visible in all of its files, and declarations can appear in any order. Only
the functions `main` can reach, and the globals they use, end up in the
program, so a module costs only what a program uses of it.

## Types

| Type      | Meaning                                                  |
| --------- | -------------------------------------------------------- |
| `u8`      | 8-bit unsigned integer                                   |
| `u16`     | 16-bit unsigned integer                                  |
| `u32`     | 32-bit unsigned integer                                  |
| `i32`     | 32-bit signed integer                                    |
| `fixed`   | 16.16 fixed point: from −32768 to just under 32768       |
| `bool`    | `true` or `false`                                        |
| `*T`      | pointer to a `T`, or `null`                              |
| `[]T`     | slice: a pointer and a length                            |
| `[N]T`    | array of `N` values                                      |
| `fn(T, U) R` | a function taking a `T` and a `U` and giving an `R`, or `null` |
| `Name`    | a struct or enum                                         |

Arithmetic happens on `u32` and `i32`. A `u8` or `u16` becomes a `u32` when
it takes part in arithmetic or a comparison.

`fixed` numbers have 16 bits after the point. They add, subtract, multiply,
divide and compare, but don't mix with integers: `x as fixed` and `f as i32`
convert, the second rounding down. Literals with a point, like `1.5` or
`0.25`, are `fixed`, and a whole-number constant becomes a `fixed` where one
is needed, so `f * 2` doubles `f`.

Values never change type silently, with two exceptions:

- A smaller integer widens to a larger one: `u8` and `u16` to `u32` or `i32`.
- An array becomes a slice of itself.

Everything else needs `as`: `x as u8` keeps the low 8 bits, `p as *Node`
reinterprets a pointer, `n as Color` turns a number into an enum.

### Constants

Number literals (`42`, `0xff`, `0b1010`, `1_000`) and characters (`'a'`)
have no type of their own. They take the type their use needs, and it is an
error if they don't fit. A variable can't take its type from one:

```
var count u32 = 0      // fine
var count = 0          // error: say which type
```

Arithmetic between constants without a type is worked out on 32-bit
values, with division, remainder and right shifts unsigned, so `-9 / 2` is
not `-4`. Give a constant a type, as in `const N i32 = -9`, and arithmetic
on it follows that type.

A constant shifted by an amount that isn't constant is a `u32`.

`KiB` (1024) and `MiB` (1048576) are predefined.

### Structs and enums

```
struct Point {
    x i32
    y i32
}

enum Color {
    Red,
    Green,
    Blue = 10,
}
```

Fields are laid out in order, each aligned to its size (at most 4 bytes).
Enum values count up from 0, or from the last explicit value. They are
written `Color.Red`, stored as 32 bits, and compare only with their own enum.

### Pointers and slices

`&x` takes an address, `*p` follows a pointer, and `p.x` follows a pointer to
reach a field. There is no pointer arithmetic.

Indexing an array or slice is checked: an index past the end stops the
program. `s.len` is the length of a slice or array; `s.ptr` is a slice's
pointer. `a[lo:hi]` makes a slice, with either end optional, and is also
checked. `p[0:n]` makes a slice from a pointer, and is the one unchecked
operation.

Following `null` stops the program, because nothing lives at low addresses.

### Functions as values

A function's name without a call is its value, with a type like
`fn(u32, []u8) bool`, or `fn(u32)` for one that gives nothing back.
Variables, fields, parameters and results can hold functions, and calling
one calls the function it holds:

```
struct Button {
    label []u8
    pressed fn(*Button)
}

fn sort(items []u32, before fn(u32, u32) bool) { ... }

button.pressed(&button)
sort(numbers[:], descending)
```

Functions compare with `==` and `!=`, including against `null`. Calling
`null` stops the program.

Source files are UTF-8. A string literal is a `[]u8` holding its text as
UTF-8, and a character literal like `'a'` or `'é'` is the character's
number. Escapes are `\n`, `\t`, `\r`, `\0`, `\\`, `\'`, `\"` and `\xNN`.

## Declarations

```
const LIMIT = 100
const MASK u32 = 0xff
var counter u32                         // globals start at zero
fn add(a u32, b u32) u32 { return a + b }
fn write(file u32, bytes []u8) u32      // implemented in assembly
```

A function without a body is implemented in assembly, following the calling
convention below. The library's are in `/lib/runtime.asm`, which `tc`
assembles with every program.

Function parameters and results can be integers, `bool`, enums, pointers or
slices. Pass structs and arrays by pointer.

### The manifest

A program declares its memory once:

```
manifest {
    memory = 4 * MiB
    stack = 64 * KiB
    allow = "read write window"
}
```

`allow` lists what the program may do: `read`, `write`, `spawn`, `window`,
`processes`, `recovery`, `audio` and `network` (see
[executable.md](executable.md)). Without it,
a program may do none of those.

`main` receives all of the program's working memory as a slice and returns
its exit status:

```
fn main(memory []u8) u32 { ... }
```

## Modules

A module is one file in the library, named after its file: `/lib/io.tr` is
the module `io`. A file imports the modules it uses at its top, before
anything else:

```
import io
import gfx
```

Outside a module, its names are qualified with the module's name:
`io.print("hi\n")`, `var w: io.Writer;`, `gfx.EVENT_CLOSE`. Inside the
module they're written plainly. Each module has names of its own, so
`text.equal` and a program's own `equal` don't clash.

A local variable, a parameter, or a top-level name of the file's own
module hides a module with the same name. A parameter called `text` works
as usual in a file that imports `text`, but `text.equal` can't be used
where it's visible.

`tc` finds modules in `/lib`, or in the directory `-L` names, and reads
each one once, however many files import it. The library has these:

| Module  | What it has                                                    |
| ------- | -------------------------------------------------------------- |
| `io`    | files and streams: `open`, `read`, `write`, `Writer`, `Reader`, `print` |
| `mem`   | arenas, and copying and filling memory                         |
| `text`  | comparing text, numbers in text, and UTF-8                     |
| `os`    | arguments, `exit`, starting programs, directories, processes, time |
| `net`   | TCP connections                                                |
| `gfx`   | windows, events and drawing                                    |
| `ui`    | buttons, check boxes and text fields                           |
| `math`  | fixed-point vectors and matrices, `sin` and `cos`              |
| `image` | TIM1 images                                                    |
| `sound` | TSN1 sounds, and playing samples                               |
| `asm`   | assembling: Terrarium assembly in, a program out               |

## Statements

```
var x u32 = 1           // a local; without a value it starts at zero
var t = next_token()    // the type comes from the value
x = 2
x += 3                  // also -= *= /= %= &= |= ^= <<= >>=
if x > 3 { ... } else if x == 0 { ... } else { ... }
while x < 10 { x += 1 }
for i in 0..10 { ... }  // i counts from 0 to 9; a range of two constants is u32
match color {
    Color.Red, Color.Green => { ... }
    else => { ... }
}
break
continue
return x
```

A statement ends at the end of its line. A line ends a statement when its
last token is a name, a number, a string, `)`, `]`, `true`, `false`,
`null`, `return`, `break` or `continue`, so a long expression breaks after
an operator or a comma, and a `{` goes on the line it opens. `;` separates
statements on one line, and isn't needed before a `}`. Declarations end the
same way.

Conditions must be `bool`. `match` compares against constants, takes the
first arm that matches, and does nothing if none does and there's no `else`.

## Expressions

From lowest to highest precedence:

| Operators                     |                          |
| ----------------------------- | ------------------------ |
| `\|\|`                        | logical or               |
| `&&`                          | logical and              |
| `== != < <= > >=`             | comparison               |
| `\|`                          | bitwise or               |
| `^`                           | bitwise exclusive or     |
| `&`                           | bitwise and              |
| `<< >>`                       | shifts                   |
| `+ -`                         | addition                 |
| `* / %`                       | multiplication           |
| `as`                          | conversion               |
| `- ! ~ & *`                   | unary                    |
| `f() a[i] a[i:j] a.f`         | calls, indexing, fields  |

`&&` and `||` evaluate their right side only when the left doesn't decide
the answer. Division and `>>` are signed for `i32` and unsigned for `u32`.
Arithmetic wraps. Division by zero stops the program.

`size_of(T)` is the size of a type in bytes.

`new(arena, T)` takes a zeroed `T` from an arena and returns a `*T`;
`new(arena, T, n)` returns a `[]T` of `n` elements. It calls `mem.alloc`,
so a program that uses `new` needs the module `mem`.

## Calling convention

Compiled code is plain assembly, so assembly can call it and be called.
`tc` assembles it after the runtime and any `.asm` files on its command
line; `tc -S` writes it out instead.

- The caller pushes arguments from last to first, calls, then removes them.
  Each argument takes 4 bytes, except slices, which take 8: the pointer at
  the lower address, then the length.
- The result is in r0, or for a slice the pointer in r0 and length in r1.
- A function may change r0–r6. It keeps r7 (the frame pointer) and sp.
- A name `foo` in the language is `_foo` in assembly, and `foo` in the
  module `io` is `_io.foo`. A function value is its code address, and
  calling one is `call` with a register.
