// Assembling: Terrarium assembly in, a program out. The port of
// system/lib/asm.tr; docs/assembly.md describes the language.
//
// The sources are read twice. The first pass learns where every label is
// and how big each section is; the second encodes every instruction.

#include "tc.h"

#include <stdlib.h>
#include <string.h>

enum {
    BASE = 0x10000,                 // where code and data start in memory
    HEADER_SIZE = 32,
    DEFAULT_STACK = 0x10000,
    SLOTS = 0x8000,                 // in the symbol table, a power of two
    MAX_SYMBOLS = 0x6000,
    TOKENS = 64,                    // at most 63 a line, then the end
    STRINGS_SIZE = 1024,            // for the decoded strings of a line
};

// The sections, which number the offsets, and a symbol's kind besides.
enum { CODE, DATA, BSS, CONSTANT };

// Opcodes, named as in docs/isa.md.
enum {
    LI = 0x01, LIH = 0x02, ADDI = 0x20, LD = 0x30, ST = 0x34, BZ = 0x46, BNZ = 0x47,
    JMP = 0x50, CALL = 0x51, CALLR = 0x52, RET = 0x53, PUSH = 0x60, POP = 0x61,
    COPY = 0x70, FILL = 0x71, SYS = 0x80, IN = 0x90, OUT = 0x91, RESUME = 0x92,
    WAIT = 0x93, HALT = 0xff,
};

typedef enum {
    A_END, A_NAME, A_REGISTER, A_NUMBER, A_STRING,

    A_IF, A_GOTO, A_CALL, A_RETURN, A_PUSH, A_POP, A_COPY, A_FILL, A_SYS, A_HALT, A_IO,
    A_RESUME, A_WAIT, A_U8, A_U16,

    A_DOT_CODE, A_DOT_DATA, A_DOT_BSS, A_DOT_BYTE, A_DOT_ZERO, A_DOT_MEMORY, A_DOT_STACK,
    A_DOT_ALLOW,

    A_PLUS, A_MINUS, A_TIMES, A_DIVIDE, A_DIVIDE_SIGNED, A_REMAINDER, A_REMAINDER_SIGNED,
    A_AND, A_OR, A_XOR, A_SHIFT_LEFT, A_SHIFT_RIGHT, A_SHIFT_RIGHT_SIGNED, A_TIMES_FIXED,
    A_DIVIDE_FIXED,
    A_EQUAL, A_NOT_EQUAL, A_LESS, A_LESS_EQUAL, A_GREATER, A_GREATER_EQUAL,
    A_LESS_SIGNED, A_LESS_EQUAL_SIGNED, A_GREATER_SIGNED, A_GREATER_EQUAL_SIGNED,
    A_ASSIGN, A_LBRACKET, A_RBRACKET, A_COMMA, A_COLON,
} Kind;

typedef struct {
    Kind kind;
    uint32_t value;     // a number's value, or a register's number
    Str text;           // a name's text, or a string's decoded bytes
} AsmToken;

typedef struct {
    Str name;           // empty in a free slot
    uint32_t kind;      // CODE, DATA or BSS for a label, or CONSTANT
    uint32_t value;     // a constant's value, or a label's offset in its section
    uint32_t line;      // where it's defined, counting every line of every source
} Symbol;

// .memory, .stack and .allow, which may each be given once.
typedef struct {
    uint32_t value;
    bool given;
} Setting;

static AsmSource *sources;
static uint32_t source_count;
static uint32_t source;             // the one being read
static Str input;                   // its text
static uint32_t pos;
static uint32_t line;               // in that source
static uint32_t all_lines;          // in every source so far
static uint32_t pass;
static uint32_t section;
static uint32_t offsets[3];         // in code, data and bss
static uint32_t bss_base;
static uint8_t *code;
static uint8_t *data;
static Symbol *symbols;
static uint32_t symbol_count;
static AsmToken tokens[TOKENS];
static uint32_t cursor;
static uint8_t strings[STRINGS_SIZE];
static Setting memory_size;
static Setting stack_size;
static Setting allowed;

static void read_all(uint32_t which);
static void tokenize(void);
static void statement(void);
static Symbol *find_symbol(Str name);
static uint32_t label_address(const Symbol *s);
NORETURN static void asm_fail(const char *message);

static uint32_t
align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

static void
write_word(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

// Assembles the sources, in order, into a program, and returns it. A
// mistake stops with a message.
Str
assemble(AsmSource *all, uint32_t count)
{
    sources = all;
    source_count = count;
    symbols = alloc(SLOTS * sizeof(Symbol));
    symbol_count = 0;
    read_all(1);
    bss_base = BASE + align4(offsets[DATA]);
    uint8_t *program = alloc(HEADER_SIZE + offsets[CODE] + offsets[DATA]);
    code = program + HEADER_SIZE;
    data = program + HEADER_SIZE + offsets[CODE];
    read_all(2);

    Symbol *start = find_symbol(S("start"));
    if (start->name.n == 0 || start->kind != CODE)
        asm_fail("no 'start' label in .code");
    uint32_t stack = DEFAULT_STACK;
    if (stack_size.given)
        stack = stack_size.value;
    uint32_t need = offsets[CODE] + align4(offsets[DATA]) + align4(offsets[BSS]) + stack;
    uint32_t memory = need;
    if (memory_size.given)
        memory = memory_size.value;
    memory = align4(memory);
    if (memory < need)
        asm_fail(".memory is smaller than the program");
    memcpy(program, "TRX2", 4);
    write_word(program + 4, label_address(start));
    write_word(program + 8, offsets[CODE]);
    write_word(program + 12, offsets[DATA]);
    write_word(program + 16, offsets[BSS]);
    write_word(program + 20, stack);
    write_word(program + 24, memory);
    write_word(program + 28, allowed.value);
    return (Str){ program, HEADER_SIZE + offsets[CODE] + offsets[DATA] };
}

// Writes a line like "c 00010000 start" for each label of the program just
// assembled, c for code and d for data, in the symbol table's order.
void
put_symbols(Writer *w)
{
    for (uint32_t i = 0; i < SLOTS; i++) {
        const Symbol *s = &symbols[i];
        if (s->name.n > 0 && s->kind != CONSTANT) {
            put_c(w, s->kind == CODE ? "c " : "d ");
            put_hex(w, label_address(s));
            put_byte(w, ' ');
            put(w, s->name);
            put_byte(w, '\n');
        }
    }
}

// One pass over every source, a line at a time.
static void
read_all(uint32_t which)
{
    pass = which;
    section = CODE;
    offsets[CODE] = 0;
    offsets[DATA] = 0;
    offsets[BSS] = 0;
    all_lines = 0;
    memory_size.given = false;
    stack_size.given = false;
    allowed.given = false;
    for (source = 0; source < source_count; source++) {
        input = sources[source].text;
        pos = 0;
        line = 0;
        while (pos < input.n) {
            line++;
            all_lines++;
            tokenize();
            statement();
        }
    }
}

//
// Errors
//

// Stops with "source:line: message 'name'", or with "asm: message" once the
// passes are over.
NORETURN static void
asm_fail_name(const char *message, Str name)
{
    if (source < source_count) {
        put(&errors, sources[source].name);
        put_byte(&errors, ':');
        put_u32(&errors, line);
        put_c(&errors, ": ");
    } else {
        put_c(&errors, "asm: ");
    }
    put_c(&errors, message);
    if (name.n > 0) {
        put_c(&errors, " '");
        put(&errors, name);
        put_byte(&errors, '\'');
        // Compiled names start with '_', and one nothing defines is usually
        // a function declared without a body.
        if (name.p[0] == '_' && strcmp(message, "undefined name") == 0)
            put_c(&errors, " (functions without a body are defined in assembly, as in /lib/runtime.asm)");
    }
    put_byte(&errors, '\n');
    exit(1);
}

static void
asm_fail(const char *message)
{
    asm_fail_name(message, (Str){ NULL, 0 });
}

//
// Tokens
//

static uint8_t
char_at(uint32_t offset)
{
    if (pos + offset >= input.n)
        return 0;
    return input.p[pos + offset];
}

static bool
is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

static bool
is_letter(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

// Names are letters, digits, '_' and '.', as in _io.write, and start with a
// letter or '_'.
static bool
is_name_char(uint8_t c)
{
    return is_letter(c) || is_digit(c) || c == '.';
}

static Str
read_word(void)
{
    uint32_t start = pos;
    while (pos < input.n && is_name_char(input.p[pos]))
        pos++;
    return str_slice(input, start, pos);
}

// A register's number, sp being 8, or 99 if the word isn't one.
static uint32_t
register_number(Str word)
{
    if (word.n != 2)
        return 99;
    if (word.p[0] == 's' && word.p[1] == 'p')
        return 8;
    if (word.p[0] == 'r' && word.p[1] >= '0' && word.p[1] < '8')
        return (uint32_t)(word.p[1] - '0');
    return 99;
}

static Kind
keyword(Str word)
{
    switch (word.p[0]) {
    case 'c':
        if (str_equal(word, S("call"))) return A_CALL;
        if (str_equal(word, S("copy"))) return A_COPY;
        break;
    case 'f':
        if (str_equal(word, S("fill"))) return A_FILL;
        break;
    case 'g':
        if (str_equal(word, S("goto"))) return A_GOTO;
        break;
    case 'h':
        if (str_equal(word, S("halt"))) return A_HALT;
        break;
    case 'i':
        if (str_equal(word, S("if"))) return A_IF;
        if (str_equal(word, S("io"))) return A_IO;
        break;
    case 'p':
        if (str_equal(word, S("push"))) return A_PUSH;
        if (str_equal(word, S("pop"))) return A_POP;
        break;
    case 'r':
        if (str_equal(word, S("return"))) return A_RETURN;
        if (str_equal(word, S("resume"))) return A_RESUME;
        break;
    case 's':
        if (str_equal(word, S("sys"))) return A_SYS;
        break;
    case 'u':
        if (str_equal(word, S("u8"))) return A_U8;
        if (str_equal(word, S("u16"))) return A_U16;
        break;
    case 'w':
        if (str_equal(word, S("wait"))) return A_WAIT;
        break;
    }
    return A_NAME;
}

static Kind
directive(Str word)
{
    if (str_equal(word, S("code"))) return A_DOT_CODE;
    if (str_equal(word, S("data"))) return A_DOT_DATA;
    if (str_equal(word, S("bss"))) return A_DOT_BSS;
    if (str_equal(word, S("byte"))) return A_DOT_BYTE;
    if (str_equal(word, S("zero"))) return A_DOT_ZERO;
    if (str_equal(word, S("memory"))) return A_DOT_MEMORY;
    if (str_equal(word, S("stack"))) return A_DOT_STACK;
    if (str_equal(word, S("allow"))) return A_DOT_ALLOW;
    asm_fail("unknown directive");
}

static uint32_t
digit_value(uint8_t c)
{
    if (c >= '0' && c <= '9')
        return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (uint32_t)(c - 'A' + 10);
    return 99;
}

// A decimal number, or hexadecimal after 0x.
static uint32_t
parse_number(Str word)
{
    uint32_t base = 10;
    uint32_t i = 0;
    if (word.n >= 2 && word.p[0] == '0') {
        if ((word.p[1] != 'x' && word.p[1] != 'X') || word.n == 2)
            asm_fail("bad number");
        base = 16;
        i = 2;
    }
    uint32_t value = 0;
    for (; i < word.n; i++) {
        uint32_t digit = digit_value(word.p[i]);
        if (digit >= base || value > (0xffffffffu - digit) / base)
            asm_fail("bad number");
        value = value * base + digit;
    }
    return value;
}

// Reads a character or an escape, and moves past it.
static uint32_t
read_char(void)
{
    if (pos >= input.n || input.p[pos] == '\n')
        asm_fail("unterminated character");
    uint8_t c = input.p[pos];
    pos++;
    if (c != '\\')
        return c;
    uint8_t e = char_at(0);
    pos++;
    switch (e) {
    case 'n': return '\n';
    case 't': return '\t';
    case 'r': return '\r';
    case '0': return 0;
    case '\\': case '\'': case '"': return e;
    }
    asm_fail("bad escape");
}

// Takes c after an operator, as in /s, unless a name follows: "/size" is
// '/' and then "size".
static bool
suffix(uint8_t c)
{
    if (char_at(0) == c && !is_name_char(char_at(1))) {
        pos++;
        return true;
    }
    return false;
}

// Reads an operator. A compound assignment like += is two: + and then =.
static Kind
read_operator(void)
{
    uint8_t c = input.p[pos];
    pos++;
    switch (c) {
    case '+': return A_PLUS;
    case '-': return A_MINUS;
    case '&': return A_AND;
    case '|': return A_OR;
    case '^': return A_XOR;
    case '[': return A_LBRACKET;
    case ']': return A_RBRACKET;
    case ',': return A_COMMA;
    case ':': return A_COLON;
    case '*':
        if (suffix('f')) return A_TIMES_FIXED;
        return A_TIMES;
    case '/':
        if (suffix('s')) return A_DIVIDE_SIGNED;
        if (suffix('f')) return A_DIVIDE_FIXED;
        return A_DIVIDE;
    case '%':
        if (suffix('s')) return A_REMAINDER_SIGNED;
        return A_REMAINDER;
    case '=':
        if (char_at(0) == '=') { pos++; return A_EQUAL; }
        return A_ASSIGN;
    case '!':
        if (char_at(0) == '=') { pos++; return A_NOT_EQUAL; }
        break;
    case '<':
        if (char_at(0) == '<') { pos++; return A_SHIFT_LEFT; }
        if (char_at(0) == '=') {
            pos++;
            if (suffix('s')) return A_LESS_EQUAL_SIGNED;
            return A_LESS_EQUAL;
        }
        if (suffix('s')) return A_LESS_SIGNED;
        return A_LESS;
    case '>':
        if (char_at(0) == '>') {
            pos++;
            if (suffix('s')) return A_SHIFT_RIGHT_SIGNED;
            return A_SHIFT_RIGHT;
        }
        if (char_at(0) == '=') {
            pos++;
            if (suffix('s')) return A_GREATER_EQUAL_SIGNED;
            return A_GREATER_EQUAL;
        }
        if (suffix('s')) return A_GREATER_SIGNED;
        return A_GREATER;
    }
    asm_fail("unexpected character");
}

// Splits the line at pos into tokens, and moves pos to the next line.
static void
tokenize(void)
{
    uint32_t n = 0;
    uint32_t used = 0;
    cursor = 0;
    while (pos < input.n) {
        uint8_t c = input.p[pos];
        if (c == '\n') {
            pos++;
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            pos++;
            continue;
        }
        if (c == ';') {
            while (pos < input.n && input.p[pos] != '\n')
                pos++;
            continue;
        }
        if (n == TOKENS - 1)
            asm_fail("line is too long");
        AsmToken *t = &tokens[n];
        n++;
        t->value = 0;
        if (c == '.' && is_letter(char_at(1))) {
            pos++;
            t->kind = directive(read_word());
        } else if (is_digit(c)) {
            t->kind = A_NUMBER;
            t->value = parse_number(read_word());
        } else if (is_letter(c)) {
            t->text = read_word();
            t->value = register_number(t->text);
            if (t->value < 9)
                t->kind = A_REGISTER;
            else
                t->kind = keyword(t->text);
        } else if (c == '\'') {
            pos++;
            t->kind = A_NUMBER;
            t->value = read_char();
            if (char_at(0) != '\'')
                asm_fail("unterminated character");
            pos++;
        } else if (c == '"') {
            pos++;
            uint32_t start = used;
            while (char_at(0) != '"') {
                if (pos >= input.n || input.p[pos] == '\n')
                    asm_fail("unterminated string");
                if (used == STRINGS_SIZE)
                    asm_fail("line is too long");
                strings[used] = (uint8_t)read_char();
                used++;
            }
            pos++;
            t->kind = A_STRING;
            t->text = (Str){ strings + start, used - start };
        } else {
            t->kind = read_operator();
        }
    }
    tokens[n].kind = A_END;
}

// The arithmetic instruction an operator stands for, or 0. Adding 0x10
// gives the one that takes a constant.
static uint32_t
alu_opcode(Kind kind)
{
    switch (kind) {
    case A_PLUS: return 0x10;
    case A_MINUS: return 0x11;
    case A_TIMES: return 0x12;
    case A_DIVIDE: return 0x13;
    case A_DIVIDE_SIGNED: return 0x14;
    case A_REMAINDER: return 0x15;
    case A_REMAINDER_SIGNED: return 0x16;
    case A_AND: return 0x17;
    case A_OR: return 0x18;
    case A_XOR: return 0x19;
    case A_SHIFT_LEFT: return 0x1a;
    case A_SHIFT_RIGHT: return 0x1b;
    case A_SHIFT_RIGHT_SIGNED: return 0x1c;
    case A_TIMES_FIXED: return 0x1d;
    case A_DIVIDE_FIXED: return 0x1e;
    default: return 0;
    }
}

// The branch instruction a comparison stands for, or 0. Those without one
// of their own swap the operands: a > b is b < a.
static uint32_t
branch_opcode(Kind kind)
{
    switch (kind) {
    case A_EQUAL: return 0x40;
    case A_NOT_EQUAL: return 0x41;
    case A_LESS: case A_GREATER: return 0x42;
    case A_GREATER_EQUAL: case A_LESS_EQUAL: return 0x43;
    case A_LESS_SIGNED: case A_GREATER_SIGNED: return 0x44;
    case A_GREATER_EQUAL_SIGNED: case A_LESS_EQUAL_SIGNED: return 0x45;
    default: return 0;
    }
}

static bool
swaps(Kind kind)
{
    return kind == A_GREATER || kind == A_LESS_EQUAL || kind == A_GREATER_SIGNED ||
        kind == A_LESS_EQUAL_SIGNED;
}

//
// Reading tokens
//

static AsmToken *
peek(void)
{
    return &tokens[cursor];
}

static AsmToken *
next(void)
{
    cursor++;
    return &tokens[cursor - 1];
}

static bool
take(Kind kind)
{
    if (tokens[cursor].kind == kind) {
        cursor++;
        return true;
    }
    return false;
}

static void
expect(Kind kind, const char *what)
{
    if (!take(kind))
        asm_fail_name("expected", (Str){ (const uint8_t *)what, (uint32_t)strlen(what) });
}

static uint32_t
expect_register(void)
{
    if (peek()->kind != A_REGISTER)
        asm_fail("expected a register");
    return next()->value;
}

static void
expect_end(void)
{
    if (peek()->kind != A_END)
        asm_fail("unexpected text at end of line");
}

// An optional u8 or u16 before a memory operand: what to add to the opcode
// that loads or stores 32 bits.
static uint32_t
size_prefix(void)
{
    if (take(A_U16))
        return 1;
    if (take(A_U8))
        return 2;
    return 0;
}

//
// Symbols
//

// The symbol with this name, or the free slot where it belongs.
static Symbol *
find_symbol(Str name)
{
    uint32_t h = 0x811c9dc5;
    for (uint32_t i = 0; i < name.n; i++)
        h = (h ^ name.p[i]) * 0x01000193;
    for (;;) {
        Symbol *s = &symbols[h & (SLOTS - 1)];
        if (s->name.n == 0 || str_equal(s->name, name))
            return s;
        h++;
    }
}

static void
define(Str name, uint32_t kind, uint32_t value)
{
    Symbol *s = find_symbol(name);
    if (pass == 2) {
        if (s->kind != kind || s->value != value)
            asm_fail_name("internal error, symbol moved between passes:", name);
        return;
    }
    if (s->name.n > 0)
        asm_fail_name("already defined:", name);
    s->name = name;
    s->kind = kind;
    s->value = value;
    s->line = all_lines;
    symbol_count++;
    if (symbol_count == MAX_SYMBOLS)
        asm_fail("too many symbols");
}

static uint32_t
label_address(const Symbol *s)
{
    if (s->kind == BSS)
        return bss_base + s->value;
    return BASE + s->value;
}

//
// Values
//

// A number, or the name of a constant or a label, with an optional '-'
// before it. Sets *label when it's a label, whose address is 0 in the first
// pass.
static uint32_t
value(bool *label)
{
    bool negate = take(A_MINUS);
    AsmToken *t = next();
    uint32_t v = 0;
    *label = false;
    if (t->kind == A_NUMBER) {
        v = t->value;
    } else if (t->kind == A_NAME) {
        Symbol *s = find_symbol(t->text);
        if (s->name.n > 0 && s->kind == CONSTANT) {
            if (s->line >= all_lines)
                asm_fail_name("constant used before its definition:", t->text);
            v = s->value;
        } else {
            *label = true;
            if (pass == 1)
                return 0;
            if (s->name.n == 0)
                asm_fail_name("undefined name", t->text);
            if (negate)
                asm_fail("a label can't be negated");
            return label_address(s);
        }
    } else {
        asm_fail("expected a value");
    }
    if (negate)
        return 0 - v;
    return v;
}

// A number or a constant.
static uint32_t
constant(void)
{
    bool label;
    uint32_t v = value(&label);
    if (label)
        asm_fail("expected a number or constant");
    return v;
}

static bool
fits16(uint32_t v)
{
    return v + 0x8000 <= 0xffff;
}

// A constant that fits in 16 signed bits, as its low 16 bits.
static uint32_t
immediate(void)
{
    uint32_t v = constant();
    if (!fits16(v))
        asm_fail("value doesn't fit");
    return v & 0xffff;
}

// A label in .code: its address, or 0 in the first pass.
static uint32_t
code_label(void)
{
    AsmToken *t = next();
    if (t->kind != A_NAME)
        asm_fail("expected a label");
    if (pass == 1)
        return 0;
    Symbol *s = find_symbol(t->text);
    if (s->name.n == 0)
        asm_fail_name("undefined name", t->text);
    if (s->kind != CODE)
        asm_fail_name("not a label in .code:", t->text);
    return label_address(s);
}

//
// Output
//

static void
emit(uint32_t word)
{
    if (section != CODE)
        asm_fail("instructions belong in .code");
    if (pass == 2)
        write_word(code + offsets[CODE], word);
    offsets[CODE] += 4;
}

static void
emit_fields(uint32_t op, uint32_t a, uint32_t b, uint32_t low)
{
    emit(op << 24 | a << 20 | b << 16 | (low & 0xffff));
}

static void
emit_jump(uint32_t op, uint32_t target)
{
    emit(op << 24 | ((target >> 2) & 0xffffff));
}

static void
emit_byte(uint8_t b)
{
    if (section != DATA)
        asm_fail("data belongs in .data");
    if (pass == 2)
        data[offsets[DATA]] = b;
    offsets[DATA]++;
}

// Space in .data or .bss, which is zero to begin with.
static void
zeros(uint32_t n)
{
    if (section == CODE)
        asm_fail("can't reserve space in .code");
    if (offsets[section] + n < n)
        asm_fail("value doesn't fit");
    offsets[section] += n;
}

//
// Statements
//

// NAME = value
static void
constant_definition(void)
{
    AsmToken *t = next();
    if (!take(A_ASSIGN))
        asm_fail("expected '=' after a name");
    uint32_t v = constant();
    expect_end();
    define(t->text, CONSTANT, v);
}

// An arithmetic operation into rd from rs and a register or a constant.
static void
arithmetic(uint32_t op, uint32_t rd, uint32_t rs)
{
    if (peek()->kind == A_REGISTER) {
        uint32_t rt = next()->value;
        expect_end();
        emit_fields(op, rd, rs, rt << 12);
        return;
    }
    uint32_t imm = immediate();
    expect_end();
    emit_fields(op + 0x10, rd, rs, imm);
}

// [reg], [reg + value] or [reg - value]. Returns the register, and sets
// *offset to the offset's low 16 bits.
static uint32_t
memory_operand(uint32_t *offset)
{
    expect(A_LBRACKET, "[");
    uint32_t base = expect_register();
    uint32_t v = 0;
    if (take(A_PLUS))
        v = constant();
    else if (take(A_MINUS))
        v = 0 - constant();
    if (!fits16(v))
        asm_fail("value doesn't fit");
    expect(A_RBRACKET, "]");
    *offset = v & 0xffff;
    return base;
}

// Statements that start with a register.
static void
assign(void)
{
    uint32_t rd = next()->value;
    Kind op = next()->kind;
    if (op != A_ASSIGN) {
        // rd += value, rd >>s= value, ...
        uint32_t opcode = alu_opcode(op);
        if (opcode == 0)
            asm_fail("expected an operator");
        expect(A_ASSIGN, "=");
        arithmetic(opcode, rd, rd);
        return;
    }
    if (take(A_IO)) {
        uint32_t port = expect_register();
        expect_end();
        emit_fields(IN, rd, port, 0);
        return;
    }
    uint32_t size = size_prefix();
    if (peek()->kind == A_LBRACKET) {
        uint32_t offset;
        uint32_t base = memory_operand(&offset);
        expect_end();
        emit_fields(LD + size, rd, base, offset);
        return;
    }
    if (size != 0)
        asm_fail_name("expected", S("["));
    if (peek()->kind == A_REGISTER) {
        uint32_t rs = next()->value;
        if (peek()->kind == A_END) {
            emit_fields(ADDI, rd, rs, 0);
            return;
        }
        uint32_t opcode = alu_opcode(next()->kind);
        if (opcode == 0)
            asm_fail("expected an operator");
        arithmetic(opcode, rd, rs);
        return;
    }
    // A number or constant takes one instruction if it fits in 16 bits; a
    // label always takes two.
    bool label;
    uint32_t v = value(&label);
    expect_end();
    emit_fields(LI, rd, 0, v);
    if (label || !fits16(v))
        emit_fields(LIH, rd, 0, v >> 16);
}

// [reg + offset] = reg, with u8 or u16 before it for a smaller store.
static void
store(void)
{
    uint32_t size = size_prefix();
    uint32_t offset;
    uint32_t base = memory_operand(&offset);
    expect(A_ASSIGN, "=");
    uint32_t v = expect_register();
    expect_end();
    emit_fields(ST + size, v, base, offset);
}

// if a CMP b goto label, where b can also be 0 with == and !=.
static void
if_statement(void)
{
    uint32_t a = expect_register();
    uint32_t b = 0;
    Kind compare = next()->kind;
    uint32_t op = branch_opcode(compare);
    if (op == 0)
        asm_fail("expected a comparison");
    if (peek()->kind == A_NUMBER) {
        if (next()->value != 0 || (compare != A_EQUAL && compare != A_NOT_EQUAL))
            asm_fail("only '== 0' and '!= 0' compare with a number");
        op = BZ;
        if (compare == A_NOT_EQUAL)
            op = BNZ;
    } else {
        b = expect_register();
        if (swaps(compare)) {
            uint32_t first = a;
            a = b;
            b = first;
        }
    }
    if (!take(A_GOTO))
        asm_fail("expected 'goto'");
    uint32_t target = code_label();
    expect_end();
    uint32_t offset = 0;
    if (pass == 2) {
        // The distance in words from the next instruction, shifted as a
        // signed number.
        uint32_t distance = target - (BASE + offsets[CODE] + 4);
        offset = distance >> 2 | (distance & 0x80000000u ? 0xc0000000u : 0);
        if (!fits16(offset))
            asm_fail("branch target is too far away");
    }
    emit_fields(op, a, b, offset);
}

// .byte 1, 'x', "text"
static void
bytes(void)
{
    for (;;) {
        AsmToken *t = peek();
        if (t->kind == A_STRING) {
            cursor++;
            for (uint32_t i = 0; i < t->text.n; i++)
                emit_byte(t->text.p[i]);
        } else {
            uint32_t v = constant();
            if (v + 128 >= 384)
                asm_fail("value doesn't fit in a byte");
            emit_byte((uint8_t)v);
        }
        if (peek()->kind == A_END)
            return;
        expect(A_COMMA, ",");
    }
}

static void
set_once(Setting *s)
{
    uint32_t v = constant();
    expect_end();
    if (s->given)
        asm_fail("directive is already set");
    s->given = true;
    s->value = v;
}

static void
statement(void)
{
    AsmToken *t = peek();
    if (t->kind == A_NAME && tokens[cursor + 1].kind == A_COLON) {
        define(t->text, section, offsets[section]);
        cursor += 2;
        t = peek();
    }
    switch (t->kind) {
    case A_END:
        return;
    case A_NAME:
        constant_definition();
        break;
    case A_REGISTER:
        assign();
        break;
    case A_LBRACKET:
    case A_U8:
    case A_U16:
        store();
        break;
    case A_IF:
        cursor++;
        if_statement();
        break;
    case A_GOTO: {
        cursor++;
        uint32_t target = code_label();
        expect_end();
        emit_jump(JMP, target);
        break;
    }
    case A_CALL:
        cursor++;
        if (peek()->kind == A_REGISTER) {
            uint32_t r = next()->value;
            expect_end();
            emit_fields(CALLR, r, 0, 0);
        } else {
            uint32_t target = code_label();
            expect_end();
            emit_jump(CALL, target);
        }
        break;
    case A_RETURN:
    case A_RESUME:
    case A_WAIT:
    case A_HALT: {
        cursor++;
        expect_end();
        uint32_t op = RET;
        if (t->kind == A_RESUME)
            op = RESUME;
        else if (t->kind == A_WAIT)
            op = WAIT;
        else if (t->kind == A_HALT)
            op = HALT;
        emit_fields(op, 0, 0, 0);
        break;
    }
    case A_PUSH:
    case A_POP: {
        cursor++;
        uint32_t op = PUSH;
        if (t->kind == A_POP)
            op = POP;
        for (;;) {
            emit_fields(op, expect_register(), 0, 0);
            if (peek()->kind == A_END)
                break;
            expect(A_COMMA, ",");
        }
        break;
    }
    case A_COPY:
    case A_FILL: {
        cursor++;
        uint32_t a = expect_register();
        expect(A_COMMA, ",");
        uint32_t b = expect_register();
        expect(A_COMMA, ",");
        uint32_t c = expect_register();
        expect_end();
        uint32_t op = COPY;
        if (t->kind == A_FILL)
            op = FILL;
        emit_fields(op, a, b, c << 12);
        break;
    }
    case A_SYS: {
        cursor++;
        uint32_t n = constant();
        expect_end();
        if (n > 0xffff)
            asm_fail("system call number out of range");
        emit_fields(SYS, 0, 0, n);
        break;
    }
    case A_IO: {
        cursor++;
        uint32_t port = expect_register();
        expect(A_ASSIGN, "=");
        uint32_t v = expect_register();
        expect_end();
        emit_fields(OUT, v, port, 0);
        break;
    }
    case A_DOT_CODE:
    case A_DOT_DATA:
    case A_DOT_BSS:
        cursor++;
        expect_end();
        section = CODE;
        if (t->kind == A_DOT_DATA)
            section = DATA;
        else if (t->kind == A_DOT_BSS)
            section = BSS;
        break;
    case A_DOT_BYTE:
        cursor++;
        bytes();
        break;
    case A_DOT_ZERO: {
        cursor++;
        uint32_t n = constant();
        expect_end();
        zeros(n);
        break;
    }
    case A_DOT_MEMORY:
        cursor++;
        set_once(&memory_size);
        break;
    case A_DOT_STACK:
        cursor++;
        set_once(&stack_size);
        break;
    case A_DOT_ALLOW:
        cursor++;
        set_once(&allowed);
        break;
    default:
        asm_fail("expected a statement");
    }
}
