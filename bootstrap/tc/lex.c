// Turning source text into tokens, one at a time. The port of
// system/tc/lex.tr.

#include "tc.h"

#include <string.h>

Token tok;

static uint32_t lex_file;
static Str lex_text;
static uint32_t lex_pos;
static uint32_t lex_line;
static Tok lex_last;        // the token before, which decides how a line ends

static bool
is_letter(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool
is_digit(uint8_t c)
{
    return c >= '0' && c <= '9';
}

void
lex_start(uint32_t file)
{
    lex_file = file;
    lex_text = files[file].text;
    lex_pos = 0;
    lex_line = 1;
    lex_last = T_SEMICOLON;
    next_token();
}

static NORETURN void
lex_fail(const char *message)
{
    error_start(lex_file, lex_line);
    put_c(&errors, message);
    error_end();
}

static uint8_t
peek_char(uint32_t offset)
{
    if (lex_pos + offset >= lex_text.n)
        return 0;
    return lex_text.p[lex_pos + offset];
}

// A line that ends with one of these ends its statement too, as if it ended
// with a semicolon.
static bool
ends_statement(Tok kind)
{
    switch (kind) {
    case T_NAME: case T_INT: case T_FIXED: case T_STR: case T_RPAREN: case T_RBRACKET:
    case K_RETURN: case K_BREAK: case K_CONTINUE: case K_TRUE: case K_FALSE: case K_NULL:
        return true;
    default:
        return false;
    }
}

// Skips spaces and comments. Returns true, without going past it, at the end
// of a line that ends a statement.
static bool
skip_space(void)
{
    while (lex_pos < lex_text.n) {
        uint8_t c = lex_text.p[lex_pos];
        if (c == '\n') {
            if (ends_statement(lex_last))
                return true;
            lex_line++;
            lex_pos++;
        } else if (c == ' ' || c == '\t' || c == '\r') {
            lex_pos++;
        } else if (c == '/' && peek_char(1) == '/') {
            while (lex_pos < lex_text.n && lex_text.p[lex_pos] != '\n')
                lex_pos++;
        } else {
            return false;
        }
    }
    return ends_statement(lex_last);
}

static Tok
keyword(Str word)
{
    static const struct {
        const char *text;
        Tok kind;
    } words[] = {
        { "as", K_AS }, { "break", K_BREAK }, { "const", K_CONST }, { "continue", K_CONTINUE },
        { "else", K_ELSE }, { "enum", K_ENUM }, { "fn", K_FN }, { "for", K_FOR },
        { "false", K_FALSE }, { "if", K_IF }, { "in", K_IN }, { "import", K_IMPORT },
        { "match", K_MATCH }, { "manifest", K_MANIFEST }, { "new", K_NEW }, { "null", K_NULL },
        { "return", K_RETURN }, { "struct", K_STRUCT }, { "size_of", K_SIZE_OF },
        { "true", K_TRUE }, { "var", K_VAR }, { "while", K_WHILE },
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        Str w = { (const uint8_t *)words[i].text, (uint32_t)strlen(words[i].text) };
        if (str_equal(word, w))
            return words[i].kind;
    }
    return T_NAME;
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

// Reads the digits after the point of a fixed-point number like 1.25.
static void
lex_fraction(void)
{
    lex_pos++;
    uint32_t start = lex_pos;
    while (lex_pos < lex_text.n && is_digit(lex_text.p[lex_pos]))
        lex_pos++;
    // Work from the last digit to the first, dividing by ten each time.
    uint32_t fraction = 0;
    uint32_t i = lex_pos;
    while (i > start) {
        i--;
        fraction = (fraction + (uint32_t)(lex_text.p[i] - '0') * 65536) / 10;
    }
    if (tok.value > 32767)
        lex_fail("fixed-point numbers must be below 32768");
    tok.kind = T_FIXED;
    tok.value = (tok.value << 16) + fraction;
}

static void
lex_number(void)
{
    uint32_t start = lex_pos;
    while (lex_pos < lex_text.n && (is_letter(lex_text.p[lex_pos]) || is_digit(lex_text.p[lex_pos])))
        lex_pos++;
    Str number = str_slice(lex_text, start, lex_pos);
    tok.kind = T_INT;
    tok.text = number;

    uint32_t base = 10;
    uint32_t i = 0;
    if (number.n > 2 && number.p[0] == '0' && (number.p[1] == 'x' || number.p[1] == 'X')) {
        base = 16;
        i = 2;
    } else if (number.n > 2 && number.p[0] == '0' && (number.p[1] == 'b' || number.p[1] == 'B')) {
        base = 2;
        i = 2;
    } else if (number.n > 1 && number.p[0] == '0') {
        lex_fail("bad number");
    }
    uint32_t value = 0;
    uint32_t digits = 0;
    while (i < number.n) {
        uint8_t c = number.p[i];
        i++;
        if (c == '_')
            continue;
        uint32_t d = digit_value(c);
        if (d >= base)
            lex_fail("bad number");
        if (value > (0xffffffffu - d) / base)
            lex_fail("number is too large");
        value = value * base + d;
        digits++;
    }
    if (digits == 0)
        lex_fail("bad number");
    tok.value = value;
    if (base == 10 && peek_char(0) == '.' && is_digit(peek_char(1)))
        lex_fraction();
}

// Reads one character of a string or character literal, decoding escapes.
static uint32_t
lex_escape(void)
{
    uint8_t c = peek_char(0);
    if (lex_pos >= lex_text.n || c == '\n')
        lex_fail("unterminated text");
    lex_pos++;
    if (c != '\\')
        return c;
    uint8_t e = peek_char(0);
    lex_pos++;
    switch (e) {
    case 'n': return 10;
    case 't': return 9;
    case 'r': return 13;
    case '0': return 0;
    case '\\': return 92;
    case '\'': return 39;
    case '"': return 34;
    case 'x': {
        uint32_t high = digit_value(peek_char(0));
        uint32_t low = digit_value(peek_char(1));
        if (high > 15 || low > 15)
            lex_fail("bad escape");
        lex_pos += 2;
        return high * 16 + low;
    }
    }
    lex_fail("bad escape");
}

// Reads a character written in UTF-8, giving its number.
static uint32_t
lex_utf8(void)
{
    uint32_t c = peek_char(0);
    uint32_t n = 1;
    uint32_t value = c & 0x1f;
    if (c >= 0xf0) {
        n = 3;
        value = c & 0x07;
    } else if (c >= 0xe0) {
        n = 2;
        value = c & 0x0f;
    } else if (c < 0xc0) {
        lex_fail("bad UTF-8");
    }
    lex_pos++;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t b = peek_char(0);
        if ((b & 0xc0) != 0x80)
            lex_fail("bad UTF-8");
        value = value << 6 | (uint32_t)(b & 0x3f);
        lex_pos++;
    }
    return value;
}

static void
lex_string(void)
{
    lex_pos++;
    // Decoded text is never longer than its source.
    uint32_t end = lex_pos;
    while (end < lex_text.n && lex_text.p[end] != '"' && lex_text.p[end] != '\n') {
        if (lex_text.p[end] == '\\')
            end++;
        end++;
    }
    uint8_t *bytes = alloc(end - lex_pos);
    uint32_t n = 0;
    while (peek_char(0) != '"') {
        bytes[n] = (uint8_t)lex_escape();
        n++;
    }
    lex_pos++;
    tok.kind = T_STR;
    tok.text = (Str){ bytes, n };
}

static Tok
lex_operator(void)
{
    uint8_t c = lex_text.p[lex_pos];
    uint8_t c1 = peek_char(1);
    uint8_t c2 = peek_char(2);
    lex_pos++;
    switch (c) {
    case '(': return T_LPAREN;
    case ')': return T_RPAREN;
    case '[': return T_LBRACKET;
    case ']': return T_RBRACKET;
    case '{': return T_LBRACE;
    case '}': return T_RBRACE;
    case ',': return T_COMMA;
    case ';': return T_SEMICOLON;
    case ':': return T_COLON;
    case '~': return T_TILDE;
    case '.':
        if (c1 == '.') { lex_pos++; return T_DOT_DOT; }
        return T_DOT;
    case '=':
        if (c1 == '>') { lex_pos++; return T_FAT_ARROW; }
        if (c1 == '=') { lex_pos++; return T_EQUAL; }
        return T_ASSIGN;
    case '!':
        if (c1 == '=') { lex_pos++; return T_NOT_EQUAL; }
        return T_BANG;
    case '<':
        if (c1 == '<' && c2 == '=') { lex_pos += 2; return T_SHL_ASSIGN; }
        if (c1 == '<') { lex_pos++; return T_SHL; }
        if (c1 == '=') { lex_pos++; return T_LESS_EQUAL; }
        return T_LESS;
    case '>':
        if (c1 == '>' && c2 == '=') { lex_pos += 2; return T_SHR_ASSIGN; }
        if (c1 == '>') { lex_pos++; return T_SHR; }
        if (c1 == '=') { lex_pos++; return T_GREATER_EQUAL; }
        return T_GREATER;
    case '&':
        if (c1 == '&') { lex_pos++; return T_AND_AND; }
        if (c1 == '=') { lex_pos++; return T_AMP_ASSIGN; }
        return T_AMP;
    case '|':
        if (c1 == '|') { lex_pos++; return T_OR_OR; }
        if (c1 == '=') { lex_pos++; return T_PIPE_ASSIGN; }
        return T_PIPE;
    case '-':
        if (c1 == '>') { lex_pos++; return T_ARROW; }
        if (c1 == '=') { lex_pos++; return T_MINUS_ASSIGN; }
        return T_MINUS;
    case '+':
        if (c1 == '=') { lex_pos++; return T_PLUS_ASSIGN; }
        return T_PLUS;
    case '*':
        if (c1 == '=') { lex_pos++; return T_STAR_ASSIGN; }
        return T_STAR;
    case '/':
        if (c1 == '=') { lex_pos++; return T_SLASH_ASSIGN; }
        return T_SLASH;
    case '%':
        if (c1 == '=') { lex_pos++; return T_PERCENT_ASSIGN; }
        return T_PERCENT;
    case '^':
        if (c1 == '=') { lex_pos++; return T_CARET_ASSIGN; }
        return T_CARET;
    }
    lex_pos--;
    lex_fail("unexpected character");
}

static void
read_token(void)
{
    bool line_ends = skip_space();
    tok.file = lex_file;
    tok.line = lex_line;
    tok.value = 0;
    if (line_ends) {
        tok.kind = T_SEMICOLON;
        tok.text = S("end of line");
        return;
    }
    if (lex_pos >= lex_text.n) {
        tok.kind = T_END;
        tok.text = S("end of file");
        return;
    }
    uint32_t start = lex_pos;
    uint8_t c = lex_text.p[lex_pos];
    if (is_letter(c)) {
        while (lex_pos < lex_text.n && (is_letter(lex_text.p[lex_pos]) || is_digit(lex_text.p[lex_pos])))
            lex_pos++;
        tok.text = str_slice(lex_text, start, lex_pos);
        tok.kind = keyword(tok.text);
    } else if (is_digit(c)) {
        lex_number();
    } else if (c == '\'') {
        lex_pos++;
        tok.kind = T_INT;
        if (peek_char(0) > 127)
            tok.value = lex_utf8();
        else
            tok.value = lex_escape();
        if (peek_char(0) != '\'')
            lex_fail("unterminated character");
        lex_pos++;
        tok.text = str_slice(lex_text, start, lex_pos);
    } else if (c == '"') {
        lex_string();
    } else {
        tok.kind = lex_operator();
        tok.text = str_slice(lex_text, start, lex_pos);
    }
}

// Reads the next token into tok.
void
next_token(void)
{
    read_token();
    lex_last = tok.kind;
}
