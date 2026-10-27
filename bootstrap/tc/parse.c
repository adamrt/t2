// Turning tokens into a syntax tree. The port of system/tc/parse.tr.

#include "tc.h"

List declarations;

static Node *parse_type(void);
static Node *parse_block(void);
static Node *parse_statement(void);
static Node *parse_expr(void);

static Node *
node(NodeKind kind)
{
    Node *n = alloc(sizeof(Node));
    n->kind = kind;
    n->file = tok.file;
    n->line = tok.line;
    return n;
}

static void
append(List *list, Node *n)
{
    if (list->last == NULL)
        list->first = n;
    else
        list->last->next = n;
    list->last = n;
}

void
parse_fail(const char *message)
{
    error_start(tok.file, tok.line);
    put_c(&errors, message);
    error_end();
}

static bool
take(Tok kind)
{
    if (tok.kind == kind) {
        next_token();
        return true;
    }
    return false;
}

static void
expect(Tok kind, const char *text)
{
    if (!take(kind)) {
        error_start(tok.file, tok.line);
        put_c(&errors, "expected '");
        put_c(&errors, text);
        put_c(&errors, "', found '");
        put(&errors, tok.text);
        put_c(&errors, "'");
        error_end();
    }
}

static Str
expect_name(void)
{
    if (tok.kind != T_NAME)
        parse_fail("expected a name");
    Str text = tok.text;
    next_token();
    return text;
}

// A statement or declaration ends with a semicolon, or with the end of its
// line, or just before the } that closes its block.
static void
expect_end(void)
{
    if (tok.kind != T_RBRACE)
        expect(T_SEMICOLON, ";");
}

// Whether a type starts here, as the result after a function's parameters.
static bool
starts_type(Tok kind)
{
    return kind == T_NAME || kind == T_STAR || kind == T_LBRACKET || kind == K_FN;
}

//
// Declarations
//

static Node *
parse_declaration(void)
{
    Node *n;
    if (take(K_FN)) {
        n = node(N_FN);
        n->name = expect_name();
        expect(T_LPAREN, "(");
        List params = { NULL, NULL };
        while (!take(T_RPAREN)) {
            Node *p = node(N_PARAM);
            p->name = expect_name();
            p->a = parse_type();
            append(&params, p);
            if (tok.kind != T_RPAREN)
                expect(T_COMMA, ",");
        }
        n->list = params.first;
        if (starts_type(tok.kind))
            n->a = parse_type();
        if (!take(T_SEMICOLON))
            n->b = parse_block();
        return n;
    }
    if (take(K_STRUCT)) {
        n = node(N_STRUCT);
        n->name = expect_name();
        expect(T_LBRACE, "{");
        List fields = { NULL, NULL };
        while (!take(T_RBRACE)) {
            Node *f = node(N_FIELD);
            f->name = expect_name();
            f->a = parse_type();
            expect_end();
            append(&fields, f);
        }
        n->list = fields.first;
        return n;
    }
    if (take(K_ENUM)) {
        n = node(N_ENUM);
        n->name = expect_name();
        expect(T_LBRACE, "{");
        List items = { NULL, NULL };
        while (!take(T_RBRACE)) {
            Node *item = node(N_ITEM);
            item->name = expect_name();
            if (take(T_ASSIGN))
                item->a = parse_expr();
            append(&items, item);
            if (tok.kind != T_RBRACE && !take(T_SEMICOLON))
                expect(T_COMMA, ",");
        }
        n->list = items.first;
        return n;
    }
    if (take(K_CONST)) {
        n = node(N_CONST);
        n->name = expect_name();
        if (tok.kind != T_ASSIGN)
            n->a = parse_type();
        expect(T_ASSIGN, "=");
        n->b = parse_expr();
        expect_end();
        return n;
    }
    if (take(K_VAR)) {
        n = node(N_GLOBAL);
        n->name = expect_name();
        n->a = parse_type();
        expect_end();
        return n;
    }
    if (take(K_MANIFEST)) {
        n = node(N_MANIFEST);
        expect(T_LBRACE, "{");
        List entries = { NULL, NULL };
        while (!take(T_RBRACE)) {
            Node *entry = node(N_ITEM);
            entry->name = expect_name();
            expect(T_ASSIGN, "=");
            entry->a = parse_expr();
            expect_end();
            append(&entries, entry);
        }
        n->list = entries.first;
        return n;
    }
    if (tok.kind == K_IMPORT)
        parse_fail("imports go at the top of the file");
    parse_fail("expected a declaration");
}

void
parse_file(uint32_t file)
{
    lex_start(file);
    SourceFile *f = &files[file];
    while (take(K_IMPORT)) {
        uint32_t m = load_module(expect_name());
        expect_end();
        if (imported(file, modules[m]) == 0) {
            if (f->import_count == MAX_IMPORTS)
                parse_fail("too many imports");
            f->imports[f->import_count] = m;
            f->import_count++;
        }
    }
    while (tok.kind != T_END)
        append(&declarations, parse_declaration());
}

static Node *
parse_type(void)
{
    Node *n;
    if (tok.kind == K_FN) {
        n = node(N_TYPE_FN);
        next_token();
        expect(T_LPAREN, "(");
        List params = { NULL, NULL };
        while (!take(T_RPAREN)) {
            append(&params, parse_type());
            if (tok.kind != T_RPAREN)
                expect(T_COMMA, ",");
        }
        n->list = params.first;
        if (starts_type(tok.kind))
            n->a = parse_type();
        return n;
    }
    if (tok.kind == T_STAR) {
        n = node(N_TYPE_PTR);
        next_token();
        n->a = parse_type();
        return n;
    }
    if (tok.kind == T_LBRACKET) {
        n = node(N_TYPE_SLICE);
        next_token();
        if (!take(T_RBRACKET)) {
            n->kind = N_TYPE_ARRAY;
            n->a = parse_expr();
            expect(T_RBRACKET, "]");
            n->b = parse_type();
            return n;
        }
        n->a = parse_type();
        return n;
    }
    n = node(N_TYPE_NAME);
    if (tok.kind != T_NAME) {
        error_start(tok.file, tok.line);
        put_c(&errors, "expected a type, found '");
        put(&errors, tok.text);
        put_c(&errors, "'");
        error_end();
    }
    n->name = expect_name();
    if (take(T_DOT)) {
        n->module = imported(n->file, n->name);
        if (n->module == 0) {
            error_start(n->file, n->line);
            put_c(&errors, "no module '");
            put(&errors, n->name);
            put_c(&errors, "' is imported here");
            error_end();
        }
        n->name = expect_name();
    }
    return n;
}

//
// Statements
//

static Node *
parse_block(void)
{
    Node *n = node(N_BLOCK);
    expect(T_LBRACE, "{");
    List statements = { NULL, NULL };
    while (!take(T_RBRACE))
        append(&statements, parse_statement());
    n->list = statements.first;
    return n;
}

static bool
is_assignment(Tok kind)
{
    switch (kind) {
    case T_ASSIGN: case T_PLUS_ASSIGN: case T_MINUS_ASSIGN: case T_STAR_ASSIGN: case T_SLASH_ASSIGN:
    case T_PERCENT_ASSIGN: case T_AMP_ASSIGN: case T_PIPE_ASSIGN: case T_CARET_ASSIGN:
    case T_SHL_ASSIGN: case T_SHR_ASSIGN:
        return true;
    default:
        return false;
    }
}

static Node *
parse_statement(void)
{
    Node *n;
    if (tok.kind == T_LBRACE)
        return parse_block();
    if (tok.kind == K_VAR) {
        n = node(N_VAR);
        next_token();
        n->name = expect_name();
        if (tok.kind != T_ASSIGN && tok.kind != T_SEMICOLON)
            n->a = parse_type();
        if (take(T_ASSIGN))
            n->b = parse_expr();
        if (n->a == NULL && n->b == NULL)
            parse_fail("a variable needs a type or a value");
        expect_end();
        return n;
    }
    if (tok.kind == K_IF) {
        n = node(N_IF);
        next_token();
        n->a = parse_expr();
        n->b = parse_block();
        if (take(K_ELSE)) {
            if (tok.kind == K_IF)
                n->c = parse_statement();
            else
                n->c = parse_block();
        }
        return n;
    }
    if (tok.kind == K_WHILE) {
        n = node(N_WHILE);
        next_token();
        n->a = parse_expr();
        n->b = parse_block();
        return n;
    }
    if (tok.kind == K_FOR) {
        n = node(N_FOR);
        next_token();
        n->name = expect_name();
        expect(K_IN, "in");
        n->a = parse_expr();
        expect(T_DOT_DOT, "..");
        n->b = parse_expr();
        n->c = parse_block();
        return n;
    }
    if (tok.kind == K_MATCH) {
        n = node(N_MATCH);
        next_token();
        n->a = parse_expr();
        expect(T_LBRACE, "{");
        List arms = { NULL, NULL };
        while (!take(T_RBRACE)) {
            if (take(K_ELSE)) {
                expect(T_FAT_ARROW, "=>");
                n->b = parse_block();
                continue;
            }
            Node *arm = node(N_ARM);
            List values = { NULL, NULL };
            append(&values, parse_expr());
            while (take(T_COMMA))
                append(&values, parse_expr());
            arm->list = values.first;
            expect(T_FAT_ARROW, "=>");
            arm->a = parse_block();
            append(&arms, arm);
        }
        n->list = arms.first;
        return n;
    }
    if (tok.kind == K_BREAK || tok.kind == K_CONTINUE) {
        n = node(N_BREAK);
        if (tok.kind == K_CONTINUE)
            n->kind = N_CONTINUE;
        next_token();
        expect_end();
        return n;
    }
    if (tok.kind == K_RETURN) {
        n = node(N_RETURN);
        next_token();
        if (tok.kind != T_SEMICOLON)
            n->a = parse_expr();
        expect_end();
        return n;
    }
    n = node(N_EXPR_STMT);
    Node *e = parse_expr();
    if (is_assignment(tok.kind)) {
        n->kind = N_ASSIGN;
        n->op = tok.kind;
        next_token();
        n->a = e;
        n->b = parse_expr();
    } else {
        n->a = e;
    }
    expect_end();
    return n;
}

//
// Expressions
//

// Binary operators bind tighter as the level rises.
static uint32_t
binary_level(Tok kind)
{
    switch (kind) {
    case T_OR_OR: return 1;
    case T_AND_AND: return 2;
    case T_EQUAL: case T_NOT_EQUAL: case T_LESS: case T_LESS_EQUAL: case T_GREATER: case T_GREATER_EQUAL:
        return 3;
    case T_PIPE: return 4;
    case T_CARET: return 5;
    case T_AMP: return 6;
    case T_SHL: case T_SHR: return 7;
    case T_PLUS: case T_MINUS: return 8;
    case T_STAR: case T_SLASH: case T_PERCENT: return 9;
    default: return 0;
    }
}

static Node *
parse_primary(void)
{
    Node *n;
    switch (tok.kind) {
    case T_INT:
        n = node(N_INT);
        n->value = tok.value;
        next_token();
        return n;
    case T_FIXED:
        n = node(N_FIXED);
        n->value = tok.value;
        next_token();
        return n;
    case T_STR:
        n = node(N_STR);
        n->name = tok.text;
        next_token();
        return n;
    case T_NAME:
        n = node(N_NAME);
        n->name = tok.text;
        next_token();
        return n;
    case K_TRUE:
    case K_FALSE:
        n = node(N_BOOL);
        if (tok.kind == K_TRUE)
            n->value = 1;
        next_token();
        return n;
    case K_NULL:
        n = node(N_NULL);
        next_token();
        return n;
    case K_SIZE_OF:
        n = node(N_SIZE_OF);
        next_token();
        expect(T_LPAREN, "(");
        n->a = parse_type();
        expect(T_RPAREN, ")");
        return n;
    case K_NEW:
        n = node(N_NEW);
        next_token();
        expect(T_LPAREN, "(");
        n->a = parse_expr();
        expect(T_COMMA, ",");
        n->b = parse_type();
        if (take(T_COMMA))
            n->c = parse_expr();
        expect(T_RPAREN, ")");
        return n;
    case T_LPAREN:
        next_token();
        n = parse_expr();
        expect(T_RPAREN, ")");
        return n;
    default:
        break;
    }
    parse_fail("expected an expression");
}

static Node *
parse_postfix(Node *base)
{
    Node *e = base;
    for (;;) {
        Node *n;
        if (tok.kind == T_LPAREN) {
            n = node(N_CALL);
            next_token();
            List args = { NULL, NULL };
            while (!take(T_RPAREN)) {
                append(&args, parse_expr());
                if (tok.kind != T_RPAREN)
                    expect(T_COMMA, ",");
            }
            n->a = e;
            n->list = args.first;
        } else if (tok.kind == T_LBRACKET) {
            n = node(N_INDEX);
            next_token();
            n->a = e;
            if (tok.kind != T_COLON)
                n->b = parse_expr();
            if (take(T_COLON)) {
                n->kind = N_SLICE;
                if (tok.kind != T_RBRACKET)
                    n->c = parse_expr();
            }
            expect(T_RBRACKET, "]");
        } else if (tok.kind == T_DOT) {
            n = node(N_MEMBER);
            next_token();
            n->a = e;
            n->name = expect_name();
        } else {
            return e;
        }
        e = n;
    }
}

static Node *
parse_unary(void)
{
    switch (tok.kind) {
    case T_MINUS: case T_BANG: case T_TILDE: case T_AMP: case T_STAR: {
        Node *n = node(N_UNARY);
        n->op = tok.kind;
        next_token();
        n->a = parse_unary();
        return n;
    }
    default:
        return parse_postfix(parse_primary());
    }
}

static Node *
parse_cast(void)
{
    Node *e = parse_unary();
    while (tok.kind == K_AS) {
        Node *n = node(N_CAST);
        next_token();
        n->a = e;
        n->b = parse_type();
        e = n;
    }
    return e;
}

static Node *
parse_binary(uint32_t level)
{
    if (level > 9)
        return parse_cast();
    Node *left = parse_binary(level + 1);
    while (binary_level(tok.kind) == level) {
        Node *n = node(N_BINARY);
        n->op = tok.kind;
        next_token();
        n->a = left;
        n->b = parse_binary(level + 1);
        left = n;
        if (level == 3 && binary_level(tok.kind) == 3)
            parse_fail("comparisons can't be chained");
    }
    return left;
}

static Node *
parse_expr(void)
{
    return parse_binary(1);
}
