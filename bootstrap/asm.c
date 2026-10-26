// The Terrarium assembler in C, for building Terrarium from nothing. It does
// what system/asm/asm.asm does, step for step, so both make the same program
// from the same source; the language is described in spec/assembly.md.
//
//     asm0 -o program.bin [-s program.sym] file.asm...

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// C99 has no _Noreturn; the C11 keyword, or GCC's and Clang's attribute,
// tells the compiler a function never returns.
#if __STDC_VERSION__ >= 201112L
#define NORETURN _Noreturn
#elif defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

enum {
    GUARD = 0x10000,
    HEADER_SIZE = 32,
    DEFAULT_STACK = 0x10000,
    SOURCE_SIZE = 0x800000,
    CODE_SIZE = 0x400000,
    DATA_SIZE = 0x200000,
    SYMBOL_SLOTS = 0x10000,
    MAX_SYMBOLS = 0xc000,
    MAX_TOKENS = 63,
    STRINGS_SIZE = 1024,
    MAX_FILES = 64,
    ARGS_SIZE = 0x4000,
};

enum { T_END, T_NAME, T_REG, T_NUM, T_STR, T_OP, T_DIR, T_KEY };

enum {
    K_IF, K_GOTO, K_CALL, K_RETURN, K_PUSH, K_POP, K_COPY, K_FILL, K_SYS, K_HALT,
    K_IO, K_RESUME, K_WAIT, K_U8, K_U16, K_U32,
};

enum {
    D_CODE, D_DATA, D_BSS, D_BYTE, D_WORD, D_ZERO, D_ALIGN, D_MEMORY, D_STACK, D_ALLOW,
};

enum {
    OP_PLUS = 30,
    OP_MINUS = 31,
    OP_ASSIGN = 40,
    OP_LBRACKET = 41,
    OP_RBRACKET = 42,
    OP_COMMA = 43,
    OP_COLON = 44,
};

// Symbol kinds. A label's kind is S_CODE plus its section.
enum { S_CONST = 1, S_CODE = 2, S_DATA = 3, S_BSS = 4 };

// Longest first, so the first match is the longest.
static const char *operators[] = {
    ">>s=", "<<=", ">>=", "/s=", "%s=", ">>s", "<=s", ">=s", "*f=", "/f=",
    "<<", ">>", "==", "!=", "<=", ">=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=",
    "/s", "%s", "<s", ">s", "*f", "/f",
    "+", "-", "*", "/", "%", "&", "|", "^", "<", ">", "=", "[", "]", ",", ":",
};

#define OPERATOR_COUNT (sizeof(operators) / sizeof(operators[0]))

// For each operator: its arithmetic opcode as a binary operator, or 0.
static const uint8_t binary_ops[OPERATOR_COUNT] = {
    [5] = 0x1c, [10] = 0x1a, [11] = 0x1b, [24] = 0x14, [25] = 0x16, [28] = 0x1d, [29] = 0x1e,
    [30] = 0x10, [31] = 0x11, [32] = 0x12, [33] = 0x13, [34] = 0x15, [35] = 0x17, [36] = 0x18,
    [37] = 0x19,
};

// For each operator: its arithmetic opcode as a compound assignment, or 0.
static const uint8_t assign_ops[OPERATOR_COUNT] = {
    [0] = 0x1c, [1] = 0x1a, [2] = 0x1b, [3] = 0x14, [4] = 0x16, [8] = 0x1d, [9] = 0x1e,
    [16] = 0x10, [17] = 0x11, [18] = 0x12, [19] = 0x13, [20] = 0x15, [21] = 0x17, [22] = 0x18,
    [23] = 0x19,
};

// For each operator: 0, or a branch code. The low four bits are the branch
// opcode minus 0x3f; 16 is added when the operands swap.
static const uint8_t compare_ops[OPERATOR_COUNT] = {
    [6] = 0x16, [7] = 0x06, [12] = 0x01, [13] = 0x02, [14] = 0x14, [15] = 0x04,
    [26] = 0x05, [27] = 0x15, [38] = 0x03, [39] = 0x13,
};

static const char *keywords[] = {
    "if", "goto", "call", "return", "push", "pop", "copy", "fill", "sys", "halt", "io",
    "resume", "wait", "u8", "u16", "u32",
};

static const char *directives[] = {
    "code", "data", "bss", "byte", "word", "zero", "align", "memory", "stack", "allow",
};

static const char undefined_name[] = "undefined name";

typedef struct {
    int kind;
    uint32_t value;         // a number, register, operator, keyword or directive
    const char *text;       // a name's text, or a string's decoded bytes
    uint32_t len;
} Token;

typedef struct {
    const char *name;
    uint32_t len;
    int kind;
    uint32_t value;
    uint32_t line;
} Symbol;

typedef struct {
    const char *name;
    const char *start;
    const char *end;
} File;

static File files[MAX_FILES];
static int file_count;
static const char *output_name;
static const char *symbols_name;
static char *source;
static uint8_t *code;
static uint8_t *data;
static Symbol symbols[SYMBOL_SLOTS];
static uint32_t symbol_count;

static Token tokens[MAX_TOKENS + 1];
static int cursor;
static char strings[STRINGS_SIZE];

static int pass;
static uint32_t section;
static uint32_t offsets[3];         // code, data and bss
static uint32_t bss_base;
static uint32_t line;               // counts every line of every file
static uint32_t file_line;
static const File *current_file;
static bool memory_set, stack_set, allow_set;
static uint32_t memory_value, stack_value, allow_value;

// Errors ---------------------------------------------------------------------

static void
put_stderr(const char *s, size_t n)
{
    fwrite(s, 1, n, stderr);
}

NORETURN static void
finish_message(const char *message, const char *name, uint32_t len)
{
    put_stderr(message, strlen(message));
    if (len > 0) {
        put_stderr(" '", 2);
        put_stderr(name, len);
        put_stderr("'", 1);
        // A compiled name like _io.write that nothing defines usually means
        // the runtime was left out.
        if (message == undefined_name && name[0] == '_')
            fputs(" (programs need /lib/rt.asm first)", stderr);
    }
    put_stderr("\n", 1);
    exit(1);
}

// Prints "file:line: message 'name'" and exits.
NORETURN static void
fail_name(const char *message, const char *name, uint32_t len)
{
    fprintf(stderr, "%s:%u: ", current_file->name, file_line);
    finish_message(message, name, len);
}

NORETURN static void
fail(const char *message)
{
    fail_name(message, NULL, 0);
}

// Prints "asm: message 'name'" and exits.
NORETURN static void
fatal_name(const char *message, const char *name)
{
    fputs("asm: ", stderr);
    finish_message(message, name, name ? (uint32_t)strlen(name) : 0);
}

NORETURN static void
fatal(const char *message)
{
    fatal_name(message, NULL);
}

// Tokens ---------------------------------------------------------------------

// 2 for letters and '_', which start names; 1 for digits and '.', which only
// continue them, as in _io.write; 0 for everything else.
static int
char_class(char c)
{
    if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_')
        return 2;
    if ((c >= '0' && c <= '9') || c == '.')
        return 1;
    return 0;
}

static uint32_t
digit_value(char c)
{
    if (c >= '0' && c <= '9')
        return (uint32_t)(c - '0');
    if (c >= 'a' && c <= 'f')
        return (uint32_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F')
        return (uint32_t)(c - 'A' + 10);
    return 255;
}

static const char *
scan_word(const char *p, const char *end)
{
    while (p < end && char_class(*p) != 0)
        p++;
    return p;
}

static int
find_word(const char *p, size_t n, const char **table, int count)
{
    for (int i = 0; i < count; i++)
        if (strlen(table[i]) == n && memcmp(table[i], p, n) == 0)
            return i;
    return -1;
}

static int
register_number(const char *p, size_t n)
{
    if (n != 2)
        return -1;
    if (p[0] == 's')
        return p[1] == 'p' ? 8 : -1;
    if (p[0] == 'r' && p[1] >= '0' && p[1] < '8')
        return p[1] - '0';
    return -1;
}

static uint32_t
parse_number(const char *p, const char *end)
{
    uint32_t value = 0;
    uint32_t base = 10;
    if (end - p >= 2 && p[0] == '0') {
        if ((p[1] | 0x20) != 'x')
            fail("bad number");
        p += 2;
        base = 16;
        if (p == end)
            fail("bad number");
    }
    for (; p < end; p++) {
        uint32_t digit = digit_value(*p);
        if (digit >= base || value > (0xffffffffu - digit) / base)
            fail("bad number");
        value = value * base + digit;
    }
    return value;
}

// Reads one character or escape. Returns its value, and sets *next past it.
static uint32_t
read_char(const char *p, const char *end, const char **next)
{
    static const char escapes[] = "n\nt\tr\r0\0\\\\''\"\"";
    if (p >= end || *p == '\n')
        fail("unterminated character");
    if (*p != '\\') {
        *next = p + 1;
        return (uint8_t)*p;
    }
    p++;
    if (p >= end)
        fail("bad escape");
    for (size_t i = 0; i < sizeof(escapes) - 1; i += 2) {
        if (escapes[i] == *p) {
            *next = p + 1;
            return (uint8_t)escapes[i + 1];
        }
    }
    fail("bad escape");
}

// The longest operator at p, or -1. An operator ending in a letter can't run
// into a name: "/size" is "/" then "size".
static int
match_operator(const char *p, const char *end, size_t *len)
{
    for (size_t i = 0; i < OPERATOR_COUNT; i++) {
        size_t n = strlen(operators[i]);
        if ((size_t)(end - p) < n || memcmp(p, operators[i], n) != 0)
            continue;
        if (char_class(operators[i][n - 1]) == 2 && p + n < end && char_class(p[n]) != 0)
            continue;
        *len = n;
        return (int)i;
    }
    return -1;
}

// Splits the line at p into tokens. Returns the start of the next line.
static const char *
tokenize(const char *p, const char *end)
{
    int n = 0;
    size_t used = 0;
    cursor = 0;
    while (p < end) {
        char c = *p;
        if (c == '\n') {
            p++;
            break;
        }
        if (c == ' ' || c == '\t' || c == '\r') {
            p++;
            continue;
        }
        if (c == ';') {
            while (p < end && *p != '\n')
                p++;
            continue;
        }
        if (n >= MAX_TOKENS)
            fail("line is too long");
        Token *t = &tokens[n++];
        memset(t, 0, sizeof(*t));
        if (c == '.' && p + 1 < end && char_class(p[1]) == 2) {
            const char *word = p + 1;
            p = scan_word(word, end);
            int d = find_word(word, (size_t)(p - word), directives, D_ALLOW + 1);
            if (d < 0)
                fail("unknown directive");
            t->kind = T_DIR;
            t->value = (uint32_t)d;
        } else if (c != '.' && char_class(c) == 1) {
            const char *start = p;
            p = scan_word(p, end);
            t->kind = T_NUM;
            t->value = parse_number(start, p);
        } else if (char_class(c) == 2) {
            const char *start = p;
            p = scan_word(p, end);
            size_t len = (size_t)(p - start);
            int r = register_number(start, len);
            int k = r < 0 ? find_word(start, len, keywords, K_U32 + 1) : -1;
            if (r >= 0) {
                t->kind = T_REG;
                t->value = (uint32_t)r;
            } else if (k >= 0) {
                t->kind = T_KEY;
                t->value = (uint32_t)k;
            } else {
                t->kind = T_NAME;
                t->text = start;
                t->len = (uint32_t)len;
            }
        } else if (c == '\'') {
            t->kind = T_NUM;
            t->value = read_char(p + 1, end, &p);
            if (p >= end || *p != '\'')
                fail("unterminated character");
            p++;
        } else if (c == '"') {
            p++;
            t->kind = T_STR;
            t->text = strings + used;
            for (;;) {
                if (p >= end)
                    fail("unterminated string");
                if (*p == '"')
                    break;
                uint32_t value = read_char(p, end, &p);
                if (used >= STRINGS_SIZE)
                    fail("line is too long");
                strings[used++] = (char)value;
            }
            p++;
            t->len = (uint32_t)(strings + used - t->text);
        } else {
            size_t len;
            int op = match_operator(p, end, &len);
            if (op < 0)
                fail("unexpected character");
            p += len;
            t->kind = T_OP;
            t->value = (uint32_t)op;
        }
    }
    tokens[n].kind = T_END;
    return p;
}

// Reading tokens -------------------------------------------------------------

static Token *
peek(void)
{
    return &tokens[cursor];
}

static void
advance(void)
{
    cursor++;
}

static bool
take_op(uint32_t op)
{
    if (peek()->kind == T_OP && peek()->value == op) {
        advance();
        return true;
    }
    return false;
}

static void
expect_op(uint32_t op)
{
    if (!take_op(op))
        fail_name("expected", operators[op], (uint32_t)strlen(operators[op]));
}

static uint32_t
expect_reg(void)
{
    if (peek()->kind != T_REG)
        fail("expected a register");
    uint32_t r = peek()->value;
    advance();
    return r;
}

static void
expect_end(void)
{
    if (peek()->kind != T_END)
        fail("unexpected text at end of line");
}

static bool
take_keyword(uint32_t k)
{
    if (peek()->kind == T_KEY && peek()->value == k) {
        advance();
        return true;
    }
    return false;
}

// An optional u8, u16 or u32. Returns 0 for 32 bits, 1 for 16 and 2 for 8.
static uint32_t
size_prefix(bool *had)
{
    *had = true;
    if (take_keyword(K_U32))
        return 0;
    if (take_keyword(K_U16))
        return 1;
    if (take_keyword(K_U8))
        return 2;
    *had = false;
    return 0;
}

// Symbols --------------------------------------------------------------------

// The symbol with this name, or the empty slot where it belongs.
static Symbol *
find_symbol(const char *name, uint32_t len, bool *found)
{
    uint32_t h = 0x811c9dc5;
    for (uint32_t i = 0; i < len; i++)
        h = (h ^ (uint8_t)name[i]) * 0x01000193;
    for (;;) {
        Symbol *s = &symbols[h & (SYMBOL_SLOTS - 1)];
        if (s->name == NULL) {
            *found = false;
            return s;
        }
        if (s->len == len && memcmp(s->name, name, len) == 0) {
            *found = true;
            return s;
        }
        h++;
    }
}

static void
define(const char *name, uint32_t len, int kind, uint32_t value)
{
    bool found;
    Symbol *s = find_symbol(name, len, &found);
    if (pass != 1) {
        if (s->kind != kind || s->value != value)
            fail_name("internal error, symbol moved between passes:", name, len);
        return;
    }
    if (found)
        fail_name("already defined:", name, len);
    *s = (Symbol){ name, len, kind, value, line };
    if (++symbol_count >= MAX_SYMBOLS)
        fail("too many symbols");
}

static uint32_t
label_address(const Symbol *s)
{
    return s->kind == S_BSS ? bss_base + s->value : GUARD + s->value;
}

// Values ---------------------------------------------------------------------

// An optional '-' and a number or name. *kind is 0 for a number, 1 for a name
// with a known value, or 2 for one whose value isn't known yet (in pass 1);
// *sym is the name's symbol, or NULL.
static uint32_t
atom(int *kind, const Symbol **sym)
{
    bool negate = take_op(OP_MINUS);
    Token *t = peek();
    uint32_t value;
    *sym = NULL;
    if (t->kind == T_NUM) {
        advance();
        *kind = 0;
        value = t->value;
    } else if (t->kind == T_NAME) {
        advance();
        bool found;
        Symbol *s = find_symbol(t->text, t->len, &found);
        if (!found) {
            if (pass != 1)
                fail_name(undefined_name, t->text, t->len);
            *kind = 2;
            return 0;
        }
        if (s->kind == S_CONST) {
            if (s->line >= line)
                fail_name("constant used before its definition:", t->text, t->len);
            value = s->value;
        } else {
            if (negate)
                fail("a label can't be negated");
            if (pass == 1) {
                *kind = 2;
                return 0;
            }
            value = label_address(s);
        }
        *kind = 1;
        *sym = s;
    } else {
        fail("expected a value");
    }
    return negate ? 0 - value : value;
}

// A number or constant.
static uint32_t
constant(void)
{
    int kind;
    const Symbol *sym;
    uint32_t value = atom(&kind, &sym);
    if (kind != 0 && (sym == NULL || sym->kind != S_CONST))
        fail("expected a number or constant");
    return value;
}

static bool
fits16(uint32_t value)
{
    return value + 0x8000 <= 0xffff;
}

// A constant that fits in 16 signed bits, as its low 16 bits.
static uint32_t
immediate(void)
{
    uint32_t value = constant();
    if (!fits16(value))
        fail("value doesn't fit");
    return value & 0xffff;
}

// A label in .code: its address, or 0 in pass 1.
static uint32_t
code_label(void)
{
    Token *t = peek();
    if (t->kind != T_NAME)
        fail("expected a label");
    advance();
    if (pass == 1)
        return 0;
    bool found;
    Symbol *s = find_symbol(t->text, t->len, &found);
    if (!found)
        fail_name(undefined_name, t->text, t->len);
    if (s->kind != S_CODE)
        fail_name("not a label in .code:", t->text, t->len);
    return label_address(s);
}

// Output ---------------------------------------------------------------------

static void
put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void
emit(uint32_t word)
{
    if (section != 0)
        fail("instructions belong in .code");
    if (offsets[0] + 4 > CODE_SIZE)
        fail("too much code");
    if (pass == 2)
        put32(code + offsets[0], word);
    offsets[0] += 4;
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
emit_data(const void *bytes, uint32_t n)
{
    if (section != 1)
        fail("data belongs in .data");
    if (n > DATA_SIZE - offsets[1])
        fail("too much data");
    if (pass == 2) {
        if (bytes)
            memcpy(data + offsets[1], bytes, n);
        else
            memset(data + offsets[1], 0, n);
    }
    offsets[1] += n;
}

static void
zeros(uint32_t n)
{
    if (section == 0)
        fail("can't reserve space in .code");
    if (section == 1) {
        emit_data(NULL, n);
        return;
    }
    if (offsets[2] + n < n)
        fail("value doesn't fit");
    offsets[2] += n;
}

// Statements -----------------------------------------------------------------

// NAME = value
static void
constant_definition(void)
{
    Token *t = peek();
    advance();
    if (!take_op(OP_ASSIGN))
        fail("expected '=' after a name");
    uint32_t value = constant();
    expect_end();
    define(t->text, t->len, S_CONST, value);
}

// Arithmetic operation op into rd from rs, with a register or constant from
// the cursor.
static void
alu(uint32_t op, uint32_t rd, uint32_t rs)
{
    if (peek()->kind == T_REG) {
        uint32_t rt = peek()->value;
        advance();
        expect_end();
        emit_fields(op, rd, rs, rt << 12);
        return;
    }
    uint32_t imm = immediate();
    expect_end();
    emit_fields(op + 0x10, rd, rs, imm);
}

// "[reg]", "[reg + value]" or "[reg - value]". Returns the base register and
// sets *offset to the offset's low 16 bits.
static uint32_t
memory_operand(uint32_t *offset)
{
    expect_op(OP_LBRACKET);
    uint32_t base = expect_reg();
    uint32_t value = 0;
    if (take_op(OP_PLUS)) {
        value = constant();
    } else if (take_op(OP_MINUS)) {
        value = 0 - constant();
    }
    if (!fits16(value))
        fail("value doesn't fit");
    expect_op(OP_RBRACKET);
    *offset = value & 0xffff;
    return base;
}

// Statements that start with a register.
static void
assign(void)
{
    uint32_t rd = peek()->value;
    advance();
    Token *t = peek();
    if (t->kind != T_OP)
        fail("expected an operator");
    advance();
    if (t->value != OP_ASSIGN) {
        if (assign_ops[t->value] == 0)
            fail("unknown operator");
        alu(assign_ops[t->value], rd, rd);
        return;
    }
    if (take_keyword(K_IO)) {
        uint32_t port = expect_reg();
        expect_end();
        emit_fields(0x90, rd, port, 0);
        return;
    }
    bool had;
    uint32_t size = size_prefix(&had);
    t = peek();
    if (t->kind == T_OP && t->value == OP_LBRACKET) {
        uint32_t offset;
        uint32_t base = memory_operand(&offset);
        expect_end();
        emit_fields(0x30 + size, rd, base, offset);
        return;
    }
    if (had)
        fail_name("expected", "[", 1);
    if (t->kind == T_REG) {
        uint32_t rs = t->value;
        advance();
        t = peek();
        if (t->kind == T_END) {
            emit_fields(0x20, rd, rs, 0);
            return;
        }
        if (t->kind != T_OP || binary_ops[t->value] == 0)
            fail("expected an operator");
        advance();
        alu(binary_ops[t->value], rd, rs);
        return;
    }
    // A number or constant takes one instruction if it fits in 16 bits; a
    // label always takes two.
    int kind;
    const Symbol *sym;
    uint32_t value = atom(&kind, &sym);
    bool fixed = kind == 0 || (sym != NULL && sym->kind == S_CONST);
    expect_end();
    emit_fields(0x01, rd, 0, value);
    if (!fixed || !fits16(value))
        emit_fields(0x02, rd, 0, value >> 16);
}

// [reg + offset] = reg, with an optional size prefix.
static void
store(void)
{
    bool had;
    uint32_t size = size_prefix(&had);
    uint32_t offset;
    uint32_t base = memory_operand(&offset);
    expect_op(OP_ASSIGN);
    uint32_t value = expect_reg();
    expect_end();
    emit_fields(0x34 + size, value, base, offset);
}

// if a CMP b goto label
static void
if_statement(void)
{
    uint32_t a = expect_reg();
    uint32_t b = 0;
    Token *t = peek();
    uint32_t op = t->kind == T_OP ? compare_ops[t->value] : 0;
    if (op == 0)
        fail("expected a comparison");
    advance();
    t = peek();
    if (t->kind == T_NUM) {
        if (t->value != 0 || (op != 1 && op != 2))
            fail("only '== 0' and '!= 0' compare with a number");
        op += 0x45;     // == gives 0x46, != gives 0x47
        advance();
    } else {
        b = expect_reg();
        if (op >> 4) {
            uint32_t swap = a;
            a = b;
            b = swap;
        }
        op = (op & 15) + 0x3f;
    }
    if (!take_keyword(K_GOTO))
        fail("expected 'goto'");
    uint32_t target = code_label();
    expect_end();
    uint32_t offset = 0;
    if (pass != 1) {
        // The distance in words from the next instruction, rounded down.
        uint32_t distance = target - (offsets[0] + GUARD + 4);
        offset = distance >> 2 | (distance & 0x80000000u ? 0xc0000000u : 0);
        if (!fits16(offset))
            fail("branch target is too far away");
    }
    emit_fields(op, a, b, offset);
}

// Statements that start with a keyword.
static void
keyword(void)
{
    uint32_t k = peek()->value;
    advance();
    switch (k) {
    case K_IO: {
        uint32_t port = expect_reg();
        expect_op(OP_ASSIGN);
        uint32_t value = expect_reg();
        expect_end();
        emit_fields(0x91, value, port, 0);
        break;
    }
    case K_GOTO: {
        uint32_t target = code_label();
        expect_end();
        emit_jump(0x50, target);
        break;
    }
    case K_CALL:
        if (peek()->kind == T_REG) {
            uint32_t r = peek()->value;
            advance();
            expect_end();
            emit_fields(0x52, r, 0, 0);
        } else {
            uint32_t target = code_label();
            expect_end();
            emit_jump(0x51, target);
        }
        break;
    case K_RETURN:
    case K_RESUME:
    case K_WAIT:
    case K_HALT:
        expect_end();
        emit_fields(k == K_RETURN ? 0x53 : k == K_RESUME ? 0x92 : k == K_WAIT ? 0x93 : 0xff, 0, 0, 0);
        break;
    case K_SYS: {
        uint32_t n = constant();
        expect_end();
        if (n > 0xffff)
            fail("system call number out of range");
        emit_fields(0x80, 0, 0, n);
        break;
    }
    case K_PUSH:
    case K_POP:
        for (;;) {
            emit_fields(k == K_PUSH ? 0x60 : 0x61, expect_reg(), 0, 0);
            if (peek()->kind == T_END)
                break;
            expect_op(OP_COMMA);
        }
        break;
    case K_COPY:
    case K_FILL: {
        uint32_t a = expect_reg();
        expect_op(OP_COMMA);
        uint32_t b = expect_reg();
        expect_op(OP_COMMA);
        uint32_t c = expect_reg();
        expect_end();
        emit_fields(k == K_COPY ? 0x70 : 0x71, a, b, c << 12);
        break;
    }
    case K_IF:
        if_statement();
        break;
    default:
        fail("expected a statement");
    }
}

// Sets a directive that may appear once.
static void
set_once(bool *set, uint32_t *value, uint32_t v)
{
    if (*set)
        fail("directive is already set");
    *set = true;
    *value = v;
}

// Statements that start with a directive.
static void
directive(void)
{
    uint32_t d = peek()->value;
    advance();
    if (d <= D_BSS) {
        expect_end();
        section = d;
        return;
    }
    if (d == D_BYTE) {
        for (;;) {
            Token *t = peek();
            if (t->kind == T_STR) {
                emit_data(t->text, t->len);
                advance();
            } else {
                uint32_t value = constant();
                if (value + 128 >= 384)
                    fail("value doesn't fit in a byte");
                uint8_t byte = (uint8_t)value;
                emit_data(&byte, 1);
            }
            if (peek()->kind == T_END)
                return;
            expect_op(OP_COMMA);
        }
    }
    if (d == D_WORD) {
        for (;;) {
            if (section == 1 && (offsets[1] & 3) != 0)
                fail(".word needs 4-byte alignment (use .align 4)");
            int kind;
            const Symbol *sym;
            uint8_t word[4];
            put32(word, atom(&kind, &sym));
            emit_data(word, 4);
            if (peek()->kind == T_END)
                return;
            expect_op(OP_COMMA);
        }
    }
    uint32_t value = constant();
    expect_end();
    switch (d) {
    case D_ZERO:
        zeros(value);
        break;
    case D_ALIGN: {
        if (value == 0)
            fail("bad size");
        uint32_t pad = offsets[section] % value;
        zeros(pad == 0 ? 0 : value - pad);
        break;
    }
    case D_MEMORY:
        set_once(&memory_set, &memory_value, value);
        break;
    case D_ALLOW:
        set_once(&allow_set, &allow_value, value);
        break;
    default:
        set_once(&stack_set, &stack_value, value);
        break;
    }
}

static void
statement(void)
{
    Token *t = peek();
    if (t->kind == T_NAME && tokens[cursor + 1].kind == T_OP && tokens[cursor + 1].value == OP_COLON) {
        define(t->text, t->len, (int)(S_CODE + section), offsets[section]);
        advance();
        advance();
        t = peek();
    }
    switch (t->kind) {
    case T_END:
        break;
    case T_DIR:
        directive();
        break;
    case T_NAME:
        constant_definition();
        break;
    case T_REG:
        assign();
        break;
    case T_OP:
        if (t->value != OP_LBRACKET)
            fail("expected a statement");
        store();
        break;
    case T_KEY:
        if (t->value >= K_U8)
            store();
        else
            keyword();
        break;
    default:
        fail("expected a statement");
    }
}

// Passes ---------------------------------------------------------------------

// The first pass finds every label's address, the second encodes.
static void
assemble(int which)
{
    pass = which;
    section = 0;
    memset(offsets, 0, sizeof(offsets));
    line = 0;
    memory_set = stack_set = allow_set = false;
    for (int i = 0; i < file_count; i++) {
        current_file = &files[i];
        file_line = 0;
        const char *p = files[i].start;
        while (p < files[i].end) {
            file_line++;
            line++;
            p = tokenize(p, files[i].end);
            statement();
        }
    }
}

static uint32_t
align4(uint32_t n)
{
    return (n + 3) & ~3u;
}

static void
write_all(FILE *f, const void *p, size_t n)
{
    if (n > 0 && fwrite(p, 1, n, f) != n)
        exit(1);
}

static void
write_executable(void)
{
    bool found;
    Symbol *start = find_symbol("start", 5, &found);
    if (!found || start->kind != S_CODE)
        fatal("no 'start' label in .code");
    uint32_t stack = stack_set ? stack_value : DEFAULT_STACK;
    uint32_t need = offsets[0] + align4(offsets[1]) + align4(offsets[2]) + stack;
    uint32_t memory = align4(memory_set ? memory_value : need);
    if (memory < need)
        fatal(".memory is smaller than the program");
    uint8_t header[HEADER_SIZE];
    memcpy(header, "TRX2", 4);
    put32(header + 4, label_address(start));
    put32(header + 8, offsets[0]);
    put32(header + 12, offsets[1]);
    put32(header + 16, offsets[2]);
    put32(header + 20, stack);
    put32(header + 24, memory);
    put32(header + 28, allow_value);
    FILE *f = fopen(output_name, "wb");
    if (f == NULL)
        fatal_name("can't write", output_name);
    write_all(f, header, HEADER_SIZE);
    write_all(f, code, offsets[0]);
    write_all(f, data, offsets[1]);
    fclose(f);
}

// Writes "c 00010000 name" for every label, in the symbol table's order.
static void
write_symbols(void)
{
    if (symbols_name == NULL)
        return;
    FILE *f = fopen(symbols_name, "wb");
    if (f == NULL)
        fatal_name("can't write", symbols_name);
    for (int i = 0; i < SYMBOL_SLOTS; i++) {
        const Symbol *s = &symbols[i];
        if (s->name == NULL || s->kind == S_CONST || s->len > 200)
            continue;
        fprintf(f, "%c %08x %.*s\n", s->kind == S_CODE ? 'c' : 'd', label_address(s), (int)s->len, s->name);
    }
    fclose(f);
}

// Main -----------------------------------------------------------------------

static void
read_arguments(int argc, char **argv)
{
    size_t total = 0;
    int after = 0;      // 1 after "-o", 2 after "-s"
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        total += strlen(arg);
        if (total > ARGS_SIZE)
            fatal("arguments are too long");
        if (after == 1) {
            output_name = arg;
            after = 0;
        } else if (after == 2) {
            symbols_name = arg;
            after = 0;
        } else if (strcmp(arg, "-o") == 0) {
            after = 1;
        } else if (strcmp(arg, "-s") == 0) {
            after = 2;
        } else {
            if (file_count >= MAX_FILES)
                fatal("too many input files");
            files[file_count++].name = arg;
        }
    }
    if (after != 0 || output_name == NULL || file_count == 0)
        fatal("usage: asm -o program.bin [-s program.sym] file.asm...");
}

// Reads every input file into the one source buffer.
static void
read_sources(void)
{
    source = malloc(SOURCE_SIZE);
    code = calloc(CODE_SIZE, 1);
    data = calloc(DATA_SIZE, 1);
    if (source == NULL || code == NULL || data == NULL)
        fatal("out of memory");
    size_t used = 0;
    for (int i = 0; i < file_count; i++) {
        FILE *f = fopen(files[i].name, "rb");
        if (f == NULL)
            fatal_name("can't read", files[i].name);
        files[i].start = source + used;
        for (;;) {
            if (used == SOURCE_SIZE)
                fatal("too much source");
            size_t n = fread(source + used, 1, SOURCE_SIZE - used, f);
            if (n == 0) {
                if (ferror(f))
                    fatal_name("can't read", files[i].name);
                break;
            }
            used += n;
        }
        fclose(f);
        files[i].end = source + used;
    }
}

int
main(int argc, char **argv)
{
    read_arguments(argc, argv);
    read_sources();
    assemble(1);
    bss_base = align4(offsets[1]) + GUARD;
    assemble(2);
    write_executable();
    write_symbols();
    return 0;
}
