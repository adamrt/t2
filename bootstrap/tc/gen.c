// Generating assembly from the checked syntax tree. The port of
// system/tc/gen.tr.
//
// Every expression leaves its value in r0, or a slice in r0 (pointer) and r1
// (length), or a struct or array as its address in r0. Temporaries go on the
// stack, and r7 points at the current function's frame.
//
// Only functions that main can reach are generated: starting from main, each
// call or use of a function as a value puts it in the queue. Likewise only
// the globals those functions use get space.

#include "tc.h"

typedef struct StringData StringData;

struct StringData {
    Node *node;
    StringData *next;
};

enum {
    MAX_LOOPS = 64,
};

Writer out;

static uint32_t labels;
static uint32_t return_label;
static uint32_t bounds_label;
static uint32_t break_labels[MAX_LOOPS];
static uint32_t continue_labels[MAX_LOOPS];
static uint32_t loop_depth;
static StringData *first_string;
static StringData *last_string;
static uint32_t string_count;
static int32_t immediate;
static Sym **queue;
static uint32_t queue_start;
static uint32_t queue_end;

static void gen_block(Node *b);
static void gen_statement(Node *st);
static void gen_expr(Node *e);
static void gen_address(Node *e);
static void gen_cond(Node *e, uint32_t label, bool when);

//
// Writing lines
//

static uint32_t
new_label(void)
{
    labels++;
    return labels;
}

static uint32_t
bounds(void)
{
    if (bounds_label == 0)
        bounds_label = new_label();
    return bounds_label;
}

static void
emit(const char *text)
{
    put_c(&out, "    ");
    put_c(&out, text);
    put_byte(&out, '\n');
}

static void
start_line(void)
{
    put_c(&out, "    ");
}

static void
end_line(void)
{
    put_byte(&out, '\n');
}

static void
emit_value(const char *text, uint32_t value)
{
    start_line();
    put_c(&out, text);
    put_u32(&out, value);
    end_line();
}

static void
place(uint32_t label)
{
    put_byte(&out, 'L');
    put_u32(&out, label);
    put_c(&out, ":\n");
}

static void
emit_goto(uint32_t label)
{
    start_line();
    put_c(&out, "goto L");
    put_u32(&out, label);
    end_line();
}

static void
put_label(uint32_t label)
{
    put_byte(&out, 'L');
    put_u32(&out, label);
}

static const char *
size_prefix(Type *t)
{
    if (t->size == 1)
        return "u8 ";
    if (t->size == 2)
        return "u16 ";
    return "";
}

// "[r7 - 12]" or "[r7 + 8]"
static void
put_frame(int32_t offset)
{
    if (offset < 0) {
        put_c(&out, "[r7 - ");
        put_u32(&out, 0u - (uint32_t)offset);
    } else {
        put_c(&out, "[r7 + ");
        put_u32(&out, (uint32_t)offset);
    }
    put_byte(&out, ']');
}

// "reg = r7 - 12"
static void
emit_frame_address(const char *reg, int32_t offset)
{
    start_line();
    put_c(&out, reg);
    if (offset < 0) {
        put_c(&out, " = r7 - ");
        put_u32(&out, 0u - (uint32_t)offset);
    } else {
        put_c(&out, " = r7 + ");
        put_u32(&out, (uint32_t)offset);
    }
    end_line();
}

// A name's label: write is _write, and write in the module io is _io.write.
static void
put_symbol(Sym *s)
{
    put_byte(&out, '_');
    if (s->module != 0) {
        put(&out, modules[s->module]);
        put_byte(&out, '.');
    }
    put(&out, s->name);
}

static void
use_function(Sym *s)
{
    if (!s->used) {
        s->used = true;
        queue[queue_end] = s;
        queue_end++;
    }
}

// Puts the address of a global or function in r0, and marks it used.
static void
emit_symbol(Sym *s)
{
    if (s->kind == SYM_FN)
        use_function(s);
    else
        s->used = true;
    start_line();
    put_c(&out, "r0 = ");
    put_symbol(s);
    end_line();
}

static void
emit_call(Sym *s)
{
    use_function(s);
    start_line();
    put_c(&out, "call ");
    put_symbol(s);
    end_line();
}

// Multiplies reg by size, which is a constant.
static void
scale(const char *reg, uint32_t size)
{
    if (size == 1)
        return;
    start_line();
    put_c(&out, reg);
    if (size == 0) {
        put_c(&out, " = 0");
    } else if ((size & (size - 1)) == 0) {
        put_c(&out, " <<= ");
        uint32_t shift = 0;
        uint32_t bit = 1;
        while (bit != size) {
            bit <<= 1;
            shift++;
        }
        put_u32(&out, shift);
    } else {
        put_c(&out, " *= ");
        put_u32(&out, size);
    }
    end_line();
}

//
// Loading and storing
//

static void
load_local(Local *local, const char *reg)
{
    Type *t = local->ty;
    if (is_aggregate(t)) {
        emit_frame_address(reg, local->offset);
    } else if (t->kind == TY_SLICE) {
        start_line();
        put_c(&out, "r1 = ");
        put_frame(local->offset + 4);
        end_line();
        start_line();
        put_c(&out, "r0 = ");
        put_frame(local->offset);
        end_line();
    } else {
        start_line();
        put_c(&out, reg);
        put_c(&out, " = ");
        put_c(&out, size_prefix(t));
        put_frame(local->offset);
        end_line();
    }
}

static void
store_local(Local *local, Type *t)
{
    if (is_aggregate(t)) {
        emit("r1 = r0");
        emit_frame_address("r0", local->offset);
        emit_value("r2 = ", t->size);
        emit("copy r0, r1, r2");
    } else if (t->kind == TY_SLICE) {
        start_line();
        put_frame(local->offset);
        put_c(&out, " = r0");
        end_line();
        start_line();
        put_frame(local->offset + 4);
        put_c(&out, " = r1");
        end_line();
    } else {
        start_line();
        put_c(&out, size_prefix(t));
        put_frame(local->offset);
        put_c(&out, " = r0");
        end_line();
    }
}

static void
zero_local(Local *local)
{
    Type *t = local->ty;
    if (is_aggregate(t)) {
        emit_frame_address("r0", local->offset);
        emit("r1 = 0");
        emit_value("r2 = ", (t->size + 3) & ~3u);
        emit("fill r0, r1, r2");
        return;
    }
    emit("r0 = 0");
    if (t->kind == TY_SLICE)
        emit("r1 = 0");
    store_local(local, t);
}

// Loads a value of type t from the address in reg into r0 (and r1).
static void
load(const char *reg, Type *t)
{
    if (is_aggregate(t)) {
        if (reg[0] != 'r' || reg[1] != '0' || reg[2] != '\0') {
            start_line();
            put_c(&out, "r0 = ");
            put_c(&out, reg);
            end_line();
        }
    } else if (t->kind == TY_SLICE) {
        start_line();
        put_c(&out, "r1 = [");
        put_c(&out, reg);
        put_c(&out, " + 4]");
        end_line();
        start_line();
        put_c(&out, "r0 = [");
        put_c(&out, reg);
        put_byte(&out, ']');
        end_line();
    } else {
        start_line();
        put_c(&out, "r0 = ");
        put_c(&out, size_prefix(t));
        put_byte(&out, '[');
        put_c(&out, reg);
        put_byte(&out, ']');
        end_line();
    }
}

// Stores r0 (and r1) at the address in reg.
static void
store(const char *reg, Type *t)
{
    if (t->kind == TY_SLICE) {
        start_line();
        put_byte(&out, '[');
        put_c(&out, reg);
        put_c(&out, "] = r0");
        end_line();
        start_line();
        put_byte(&out, '[');
        put_c(&out, reg);
        put_c(&out, " + 4] = r1");
        end_line();
    } else {
        start_line();
        put_c(&out, size_prefix(t));
        put_byte(&out, '[');
        put_c(&out, reg);
        put_c(&out, "] = r0");
        end_line();
    }
}

//
// Expressions
//

static bool
is_constant(Node *e)
{
    TypeKind k = e->ty->kind;
    return (e->flags & IS_CONST) != 0 && (k == TY_INT || k == TY_FIXED || k == TY_ENUM || k == TY_UNTYPED);
}

// Leaves the left operand in r0 and the right in r1. When allowed, a small
// constant right operand goes in `immediate` instead, and this returns true.
static bool
gen_operands(Node *e, bool allow_immediate)
{
    Node *right = e->b;
    if ((right->flags & IS_CONST) != 0 &&
        (right->ty->kind == TY_INT || right->ty->kind == TY_FIXED || right->ty->kind == TY_ENUM)) {
        gen_expr(e->a);
        int32_t v = (int32_t)right->value;
        if (allow_immediate && v >= -32768 && v <= 32767) {
            immediate = v;
            return true;
        }
        emit_value("r1 = ", right->value);
        return false;
    }
    // A plain local on the right can be loaded straight into r1.
    if (right->kind == N_NAME && right->local != NULL && is_scalar(right->ty) && (right->flags & TO_SLICE) == 0) {
        gen_expr(e->a);
        load_local(right->local, "r1");
        return false;
    }
    gen_expr(e->a);
    emit("push r0");
    gen_expr(right);
    emit("r1 = r0");
    emit("pop r0");
    return false;
}

// Emits r0 = r0 op r1, or with the immediate when use_immediate.
static void
operate(Tok op, Type *ty, bool use_immediate)
{
    bool is_signed = ty->is_signed;
    bool fixed = is_fixed(ty);
    start_line();
    put_c(&out, "r0 = r0 ");
    switch (op) {
    case T_PLUS: put_c(&out, "+"); break;
    case T_MINUS: put_c(&out, "-"); break;
    case T_STAR: put_c(&out, fixed ? "*f" : "*"); break;
    case T_AMP: put_c(&out, "&"); break;
    case T_PIPE: put_c(&out, "|"); break;
    case T_CARET: put_c(&out, "^"); break;
    case T_SHL: put_c(&out, "<<"); break;
    case T_SLASH: put_c(&out, fixed ? "/f" : is_signed ? "/s" : "/"); break;
    case T_PERCENT: put_c(&out, is_signed ? "%s" : "%"); break;
    case T_SHR: put_c(&out, is_signed ? ">>s" : ">>"); break;
    default: break;
    }
    if (use_immediate) {
        put_byte(&out, ' ');
        put_i32(&out, immediate);
    } else {
        put_c(&out, " r1");
    }
    end_line();
}

static void
gen_binary(Node *e)
{
    Tok op = e->op;
    if (op == T_AND_AND || op == T_OR_OR || is_comparison(op)) {
        uint32_t false_label = new_label();
        uint32_t end = new_label();
        gen_cond(e, false_label, false);
        emit("r0 = 1");
        emit_goto(end);
        place(false_label);
        emit("r0 = 0");
        place(end);
        return;
    }
    bool use_immediate = gen_operands(e, true);
    operate(op, e->ty, use_immediate);
}

static Tok
negate(Tok op)
{
    switch (op) {
    case T_EQUAL: return T_NOT_EQUAL;
    case T_NOT_EQUAL: return T_EQUAL;
    case T_LESS: return T_GREATER_EQUAL;
    case T_LESS_EQUAL: return T_GREATER;
    case T_GREATER: return T_LESS_EQUAL;
    default: return T_LESS;
    }
}

static const char *
compare_text(Tok op, bool is_signed)
{
    switch (op) {
    case T_EQUAL: return "==";
    case T_NOT_EQUAL: return "!=";
    case T_LESS: return is_signed ? "<s" : "<";
    case T_LESS_EQUAL: return is_signed ? "<=s" : "<=";
    case T_GREATER: return is_signed ? ">s" : ">";
    default: return is_signed ? ">=s" : ">=";
    }
}

// Jumps to label when e is `when`.
static void
gen_cond(Node *e, uint32_t label, bool when)
{
    if (e->kind == N_BINARY && (e->op == T_AND_AND || e->op == T_OR_OR)) {
        if ((e->op == T_AND_AND) != when) {
            gen_cond(e->a, label, when);
            gen_cond(e->b, label, when);
        } else {
            uint32_t skip = new_label();
            gen_cond(e->a, skip, !when);
            gen_cond(e->b, label, when);
            place(skip);
        }
        return;
    }
    if (e->kind == N_UNARY && e->op == T_BANG) {
        gen_cond(e->a, label, !when);
        return;
    }
    if (e->kind == N_BINARY && is_comparison(e->op)) {
        Tok op = e->op;
        if (!when)
            op = negate(op);
        bool is_signed = e->operand->is_signed;
        gen_operands(e, false);
        start_line();
        put_c(&out, "if r0 ");
        put_c(&out, compare_text(op, is_signed));
        put_c(&out, " r1 goto ");
        put_label(label);
        end_line();
        return;
    }
    gen_expr(e);
    start_line();
    put_c(&out, when ? "if r0 != 0 goto " : "if r0 == 0 goto ");
    put_label(label);
    end_line();
}

// Pushes the arguments from last to first. Returns the bytes they take.
static uint32_t
push_args(Node *a, Node *p)
{
    if (a == NULL)
        return 0;
    uint32_t size = push_args(a->next, p->next);
    gen_expr(a);
    if (p->ty->kind == TY_SLICE) {
        emit("push r1");
        size += 4;
    }
    emit("push r0");
    return size + 4;
}

static void
gen_call(Node *e)
{
    uint32_t size;
    if (e->sym != NULL) {
        size = push_args(e->list, e->sym->node->list);
        emit_call(e->sym);
    } else {
        size = push_args(e->list, e->a->ty->params);
        gen_expr(e->a);
        emit("call r0");
    }
    if (size > 0)
        emit_value("sp += ", size);
}

static void
gen_slice(Node *e)
{
    Type *base = e->a->ty;
    if (base->kind == TY_ARRAY) {
        gen_address(e->a);
        emit("push r0");
        emit_value("r0 = ", base->length);
        emit("push r0");
    } else if (base->kind == TY_SLICE) {
        gen_expr(e->a);
        emit("push r0");
        emit("push r1");
    } else {
        gen_expr(e->a);
        emit("push r0");
    }
    if (e->b != NULL)
        gen_expr(e->b);
    else
        emit("r0 = 0");
    emit("push r0");
    if (e->c != NULL)
        gen_expr(e->c);
    else
        emit("r0 = [sp + 4]");
    emit("r1 = r0");
    emit("pop r2");
    if (base->kind != TY_PTR) {
        emit("pop r3");
        start_line();
        put_c(&out, "if r1 > r3 goto ");
        put_label(bounds());
        end_line();
    }
    emit("pop r0");
    start_line();
    put_c(&out, "if r2 > r1 goto ");
    put_label(bounds());
    end_line();
    emit("r1 -= r2");
    scale("r2", base->elem->size);
    emit("r0 += r2");
}

// Leaves the address of e in r0.
static void
gen_address(Node *e)
{
    switch (e->kind) {
    case N_NAME:
        if (e->local != NULL)
            emit_frame_address("r0", e->local->offset);
        else
            emit_symbol(e->sym);
        break;
    case N_MEMBER:
        if (e->a->ty->kind == TY_PTR)
            gen_expr(e->a);
        else
            gen_address(e->a);
        if (e->value != 0)
            emit_value("r0 += ", e->value);
        break;
    case N_INDEX: {
        Type *elem = e->ty;
        Type *base = e->a->ty;
        if (base->kind == TY_ARRAY) {
            gen_address(e->a);
            if ((e->b->flags & IS_CONST) != 0) {
                if (e->b->value >= base->length)
                    fail(e, "index is past the end of the array");
                if (e->b->value != 0)
                    emit_value("r0 += ", e->b->value * elem->size);
                return;
            }
            emit("push r0");
            gen_expr(e->b);
            emit("r1 = r0");
            emit("pop r0");
            emit_value("r2 = ", base->length);
        } else {
            gen_expr(e->a);
            emit("push r1");
            emit("push r0");
            gen_expr(e->b);
            emit("r1 = r0");
            emit("pop r0");
            emit("pop r2");
        }
        start_line();
        put_c(&out, "if r1 >= r2 goto ");
        put_label(bounds());
        end_line();
        scale("r1", elem->size);
        emit("r0 += r1");
        break;
    }
    case N_UNARY:
        gen_expr(e->a);
        break;
    default:
        fail(e, "can't take the address of this");
    }
}

static void
gen_value(Node *e)
{
    Type *t = e->ty;
    switch (e->kind) {
    case N_BOOL:
        emit_value("r0 = ", e->value);
        break;
    case N_NULL:
        emit("r0 = 0");
        break;
    case N_STR: {
        string_count++;
        e->value = string_count;
        StringData *d = alloc(sizeof(StringData));
        d->node = e;
        if (last_string == NULL)
            first_string = d;
        else
            last_string->next = d;
        last_string = d;
        emit_value("r0 = S", e->value);
        emit_value("r1 = ", e->name.n);
        break;
    }
    case N_NAME:
        if (e->local != NULL) {
            load_local(e->local, "r0");
        } else {
            emit_symbol(e->sym);
            if (e->sym->kind != SYM_FN)
                load("r0", t);
        }
        break;
    case N_MEMBER:
        if (e->a->ty->kind == TY_SLICE) {
            gen_expr(e->a);
            if (str_equal(e->name, S("len")))
                emit("r0 = r1");
        } else {
            gen_address(e);
            load("r0", t);
        }
        break;
    case N_INDEX:
        gen_address(e);
        load("r0", t);
        break;
    case N_SLICE:
        gen_slice(e);
        break;
    case N_CALL:
        gen_call(e);
        break;
    case N_UNARY:
        if (e->op == T_AMP) {
            gen_address(e->a);
        } else if (e->op == T_STAR) {
            gen_expr(e->a);
            load("r0", t);
        } else {
            gen_expr(e->a);
            if (e->op == T_MINUS) {
                emit("r0 ^= -1");
                emit("r0 += 1");
            } else if (e->op == T_TILDE) {
                emit("r0 ^= -1");
            } else {
                emit("r0 ^= 1");
            }
        }
        break;
    case N_BINARY:
        gen_binary(e);
        break;
    case N_CAST: {
        gen_expr(e->a);
        Type *from = e->a->ty;
        if (is_fixed(t) && is_int(from))
            emit("r0 <<= 16");
        else if (is_fixed(from) && is_int(t))
            emit("r0 >>s= 16");
        if (is_int(t) && t->size < 4 && !(is_int(from) && from->size <= t->size)) {
            if (t->size == 2) {
                emit("r0 <<= 16");
                emit("r0 >>= 16");
            } else {
                emit("r0 &= 255");
            }
        }
        break;
    }
    case N_NEW: {
        Type *elem = resolve(e->b);
        if (e->c == NULL) {
            emit_value("r0 = ", elem->size);
            emit("push r0");
            gen_expr(e->a);
            emit("push r0");
            emit_call(e->sym);
            emit("sp += 8");
        } else {
            gen_expr(e->c);
            emit("push r0");
            scale("r0", elem->size);
            emit("push r0");
            gen_expr(e->a);
            emit("push r0");
            emit_call(e->sym);
            emit("sp += 8");
            emit("pop r1");
        }
        break;
    }
    default:
        fail(e, "can't generate this expression");
    }
}

static void
gen_expr(Node *e)
{
    if (is_constant(e))
        emit_value("r0 = ", e->value);
    else
        gen_value(e);
    if (e->flags & TO_SLICE)
        emit_value("r1 = ", e->ty->length);
}

//
// Statements
//

static void
gen_block(Node *b)
{
    for (Node *st = b->list; st != NULL; st = st->next)
        gen_statement(st);
}

static void
push_loop(uint32_t break_label, uint32_t continue_label, Node *n)
{
    if (loop_depth == MAX_LOOPS)
        fail(n, "loops are nested too deeply");
    break_labels[loop_depth] = break_label;
    continue_labels[loop_depth] = continue_label;
    loop_depth++;
}

// The binary operator inside a compound assignment like "+=".
static Tok
assign_operator(Tok op)
{
    switch (op) {
    case T_PLUS_ASSIGN: return T_PLUS;
    case T_MINUS_ASSIGN: return T_MINUS;
    case T_STAR_ASSIGN: return T_STAR;
    case T_SLASH_ASSIGN: return T_SLASH;
    case T_PERCENT_ASSIGN: return T_PERCENT;
    case T_AMP_ASSIGN: return T_AMP;
    case T_PIPE_ASSIGN: return T_PIPE;
    case T_CARET_ASSIGN: return T_CARET;
    case T_SHL_ASSIGN: return T_SHL;
    default: return T_SHR;
    }
}

static void
gen_assign(Node *st)
{
    Type *t = st->a->ty;
    if (st->op == T_ASSIGN) {
        if (is_aggregate(t)) {
            gen_address(st->a);
            emit("push r0");
            gen_expr(st->b);
            emit("r1 = r0");
            emit("pop r0");
            emit_value("r2 = ", t->size);
            emit("copy r0, r1, r2");
            return;
        }
        if (st->a->kind == N_NAME && st->a->local != NULL) {
            gen_expr(st->b);
            store_local(st->a->local, t);
            return;
        }
        gen_address(st->a);
        emit("push r0");
        gen_expr(st->b);
        emit("pop r2");
        store("r2", t);
        return;
    }
    gen_address(st->a);
    emit("push r0");
    gen_expr(st->b);
    emit("r1 = r0");
    emit("r0 = [sp]");
    load("r0", t);
    operate(assign_operator(st->op), arith(t), false);
    emit("pop r2");
    store("r2", t);
}

static void
gen_statement(Node *st)
{
    switch (st->kind) {
    case N_BLOCK:
        gen_block(st);
        break;
    case N_VAR:
        if (st->b != NULL) {
            gen_expr(st->b);
            store_local(st->local, st->local->ty);
        } else {
            zero_local(st->local);
        }
        break;
    case N_ASSIGN:
        gen_assign(st);
        break;
    case N_EXPR_STMT:
        gen_expr(st->a);
        break;
    case N_IF: {
        uint32_t other = new_label();
        gen_cond(st->a, other, false);
        gen_block(st->b);
        if (st->c == NULL) {
            place(other);
        } else {
            uint32_t end = new_label();
            emit_goto(end);
            place(other);
            gen_statement(st->c);
            place(end);
        }
        break;
    }
    case N_WHILE: {
        uint32_t top = new_label();
        uint32_t end = new_label();
        place(top);
        gen_cond(st->a, end, false);
        push_loop(end, top, st);
        gen_block(st->b);
        loop_depth--;
        emit_goto(top);
        place(end);
        break;
    }
    case N_FOR: {
        uint32_t top = new_label();
        uint32_t step = new_label();
        uint32_t end = new_label();
        gen_expr(st->a);
        store_local(st->local, st->local->ty);
        gen_expr(st->b);
        store_local(st->local2, st->local2->ty);
        place(top);
        load_local(st->local, "r0");
        load_local(st->local2, "r1");
        start_line();
        put_c(&out, st->local->ty->is_signed ? "if r0 >=s r1 goto " : "if r0 >= r1 goto ");
        put_label(end);
        end_line();
        push_loop(end, step, st);
        gen_block(st->c);
        loop_depth--;
        place(step);
        load_local(st->local, "r0");
        emit("r0 += 1");
        store_local(st->local, st->local->ty);
        emit_goto(top);
        place(end);
        break;
    }
    case N_MATCH: {
        uint32_t end = new_label();
        gen_expr(st->a);
        store_local(st->local, st->local->ty);
        for (Node *arm = st->list; arm != NULL; arm = arm->next) {
            uint32_t body = new_label();
            uint32_t skip = new_label();
            for (Node *v = arm->list; v != NULL; v = v->next) {
                load_local(st->local, "r0");
                emit_value("r1 = ", v->value);
                start_line();
                put_c(&out, "if r0 == r1 goto ");
                put_label(body);
                end_line();
            }
            emit_goto(skip);
            place(body);
            gen_block(arm->a);
            emit_goto(end);
            place(skip);
        }
        if (st->b != NULL)
            gen_block(st->b);
        place(end);
        break;
    }
    case N_BREAK:
        emit_goto(break_labels[loop_depth - 1]);
        break;
    case N_CONTINUE:
        emit_goto(continue_labels[loop_depth - 1]);
        break;
    case N_RETURN:
        if (st->a != NULL)
            gen_expr(st->a);
        emit_goto(return_label);
        break;
    default:
        break;
    }
}

static void
gen_function(Sym *s)
{
    Node *n = s->node;
    return_label = new_label();
    bounds_label = 0;
    loop_depth = 0;
    put_byte(&out, '\n');
    put_symbol(s);
    put_c(&out, ":\n");
    emit("push r7");
    emit("r7 = sp");
    if (n->value > 0) {
        if (n->value > 32767)
            fail(n, "too many local variables");
        emit_value("sp -= ", n->value);
    }
    gen_block(n->b);
    place(return_label);
    emit("sp = r7");
    emit("pop r7");
    emit("return");
    if (bounds_label != 0) {
        place(bounds_label);
        emit("call _bounds_failed");
    }
}

void
generate(void)
{
    put_c(&out, ".code\n");
    uint32_t functions = 0;
    for (Sym *s = first_sym; s != NULL; s = s->after) {
        if (s->kind == SYM_FN)
            functions++;
    }
    queue = alloc(functions * sizeof(Sym *));
    use_function(find_sym(0, S("main")));
    while (queue_start < queue_end) {
        Sym *f = queue[queue_start];
        queue_start++;
        if (f->node->b != NULL)
            gen_function(f);
    }

    put_c(&out, "\n.data\n");
    for (StringData *d = first_string; d != NULL; d = d->next) {
        put_byte(&out, 'S');
        put_u32(&out, d->node->value);
        put_c(&out, ":\n");
        Str bytes = d->node->name;
        for (uint32_t i = 0; i < bytes.n; i++) {
            put_c(&out, i % 16 == 0 ? "    .byte " : ", ");
            put_u32(&out, bytes.p[i]);
            if (i % 16 == 15 || i == bytes.n - 1)
                put_byte(&out, '\n');
        }
    }

    put_c(&out, "\n.bss\n");
    for (Sym *s = first_sym; s != NULL; s = s->after) {
        if (s->kind == SYM_GLOBAL && s->used) {
            put_symbol(s);
            put_c(&out, ":\n");
            uint32_t size = (s->ty->size + 3) & ~3u;
            if (size < 4)
                size = 4;
            emit_value(".zero ", size);
        }
    }
    put_byte(&out, '\n');
    if (manifest_memory != NULL)
        emit_value(".memory ", manifest_memory->value);
    if (manifest_stack != NULL)
        emit_value(".stack ", manifest_stack->value);
    if (manifest_allow != NULL)
        emit_value(".allow ", manifest_allow->value);
}
