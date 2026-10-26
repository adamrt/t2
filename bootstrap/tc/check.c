// Checking function bodies: every expression gets a type, every name finds
// what it refers to, and every local gets a place in the stack frame. The
// port of system/tc/check.tr.

#include "tc.h"

static Local *scope;            // the innermost local first
static Local *block_start;      // the first local outside the current block
static uint32_t frame;
static uint32_t loops;
static Sym *current;

static void check_block(Node *b);
static void check_statement(Node *st);
static bool is_local(Str name);
static Type *check_expr(Node *e, Type *want);

static Local *
add_local(Node *n, Str name, Type *ty, int32_t offset)
{
    if (name.n > 0) {
        for (Local *other = scope; other != block_start; other = other->next) {
            if (str_equal(other->name, name))
                fail_name(n, "already defined:", name);
        }
    }
    Local *local = alloc(sizeof(Local));
    local->name = name;
    local->ty = ty;
    local->offset = offset;
    local->next = scope;
    scope = local;
    return local;
}

static Local *
new_local(Node *n, Str name, Type *ty)
{
    frame += (ty->size + 3) & ~3u;
    return add_local(n, name, ty, -(int32_t)frame);
}

void
check_function(Sym *s)
{
    current = s;
    scope = NULL;
    block_start = NULL;
    frame = 0;
    loops = 0;
    int32_t offset = 8;
    for (Node *p = s->node->list; p != NULL; p = p->next) {
        add_local(p, p->name, p->ty, offset);
        offset += p->ty->kind == TY_SLICE ? 8 : 4;
    }
    check_block(s->node->b);
    s->node->value = frame;
}

// Statements -----------------------------------------------------------------

static void
check_block(Node *b)
{
    Local *saved_scope = scope;
    Local *saved_start = block_start;
    block_start = scope;
    for (Node *st = b->list; st != NULL; st = st->next)
        check_statement(st);
    scope = saved_scope;
    block_start = saved_start;
}

static void
check_condition(Node *e)
{
    if (check(e, NULL) != t_bool)
        fail(e, "a condition must be a bool");
}

// Whether e names a place in memory that can be assigned to.
static bool
is_place(Node *e)
{
    switch (e->kind) {
    case N_NAME:
        return e->local != NULL || (e->sym != NULL && e->sym->kind == SYM_GLOBAL);
    case N_MEMBER:
        return e->a->ty->kind == TY_PTR || is_place(e->a);
    case N_INDEX:
        return e->a->ty->kind == TY_SLICE || is_place(e->a);
    case N_UNARY:
        return e->op == T_STAR;
    default:
        return false;
    }
}

static void
check_statement(Node *st)
{
    switch (st->kind) {
    case N_BLOCK:
        check_block(st);
        break;
    case N_VAR: {
        Type *ty;
        if (st->a != NULL) {
            ty = resolve(st->a);
            if (st->b != NULL)
                coerce(st->b, check(st->b, ty), ty);
        } else {
            ty = check(st->b, NULL);
            if (ty == t_untyped)
                fail(st, "give the variable a type");
            if (ty == t_null || ty == t_void)
                fail(st, "this value has no type");
        }
        if (ty == t_void)
            fail(st, "a variable can't have no type");
        st->local = new_local(st, st->name, ty);
        break;
    }
    case N_ASSIGN: {
        Type *target = check(st->a, NULL);
        if (!is_place(st->a))
            fail(st, "can't assign to this");
        if (st->op == T_ASSIGN) {
            coerce(st->b, check(st->b, target), target);
        } else {
            bool fixed_ok = st->op == T_PLUS_ASSIGN || st->op == T_MINUS_ASSIGN || st->op == T_STAR_ASSIGN ||
                            st->op == T_SLASH_ASSIGN;
            if (!is_int(target) && !(is_fixed(target) && fixed_ok))
                fail(st, "this assignment needs an integer");
            Type *operand = arith(target);
            Type *vt = check(st->b, operand);
            if (st->op == T_SHL_ASSIGN || st->op == T_SHR_ASSIGN) {
                if (vt == t_untyped)
                    coerce(st->b, vt, t_u32);
                else if (!is_int(vt))
                    fail(st, "shift by an integer");
            } else {
                coerce(st->b, vt, operand);
            }
        }
        break;
    }
    case N_EXPR_STMT:
        if (st->a->kind != N_CALL)
            fail(st, "only calls can be statements");
        check(st->a, NULL);
        break;
    case N_IF:
        check_condition(st->a);
        check_block(st->b);
        if (st->c != NULL)
            check_statement(st->c);
        break;
    case N_WHILE:
        check_condition(st->a);
        loops++;
        check_block(st->b);
        loops--;
        break;
    case N_FOR: {
        Type *from = check(st->a, NULL);
        Type *to = check(st->b, NULL);
        Type *ty = t_u32;
        if (from != t_untyped)
            ty = arith(from);
        else if (to != t_untyped)
            ty = arith(to);
        if (!is_int(ty))
            fail(st, "a range needs integers");
        coerce(st->a, from, ty);
        coerce(st->b, to, ty);
        Local *saved_scope = scope;
        Local *saved_start = block_start;
        block_start = scope;
        st->local = new_local(st, st->name, ty);
        st->local2 = new_local(st, S(""), ty);
        loops++;
        check_block(st->c);
        loops--;
        scope = saved_scope;
        block_start = saved_start;
        break;
    }
    case N_MATCH: {
        Type *ty = check(st->a, NULL);
        if (ty == t_untyped || (!is_int(ty) && ty->kind != TY_ENUM))
            fail(st, "match needs an integer or enum");
        ty = arith(ty);
        st->local = new_local(st, S(""), ty);
        for (Node *arm = st->list; arm != NULL; arm = arm->next) {
            for (Node *v = arm->list; v != NULL; v = v->next)
                const_value(v, ty);
            check_block(arm->a);
        }
        if (st->b != NULL)
            check_block(st->b);
        break;
    }
    case N_BREAK:
    case N_CONTINUE:
        if (loops == 0)
            fail(st, "'break' and 'continue' only work in a loop");
        break;
    case N_RETURN: {
        Type *result = current->ty;
        if (st->a == NULL) {
            if (result != t_void)
                fail(st, "return needs a value");
        } else {
            if (result == t_void)
                fail(st, "this function returns nothing");
            coerce(st->a, check(st->a, result), result);
        }
        break;
    }
    default:
        break;
    }
}

// Converts expression e of type t to type want, or fails.
void
coerce(Node *e, Type *t, Type *want)
{
    if (t == want)
        return;
    if (t == t_untyped) {
        if (is_fixed(want)) {
            e->value = e->value << 16;
            e->ty = want;
            return;
        }
        if (!is_int(want)) {
            error_start(e->file, e->line);
            put_c(&errors, "a number can't be a ");
            put_type(&errors, want);
            error_end();
        }
        if ((want->size == 1 && e->value > 255) || (want->size == 2 && e->value > 65535)) {
            error_start(e->file, e->line);
            put_u32(&errors, e->value);
            put_c(&errors, " doesn't fit in ");
            put_type(&errors, want);
            error_end();
        }
        e->ty = want;
        return;
    }
    if (t == t_null && (want->kind == TY_PTR || want->kind == TY_FUNC)) {
        e->ty = want;
        return;
    }
    if (is_int(t) && is_int(want) && t->size < want->size && !t->is_signed)
        return;
    if (t->kind == TY_ARRAY && want->kind == TY_SLICE && t->elem == want->elem && is_place(e)) {
        e->flags |= TO_SLICE;
        return;
    }
    fail_types(e, want, t);
}

// Expressions ----------------------------------------------------------------

static void
set_const(Node *e, uint32_t value)
{
    e->flags |= IS_CONST;
    e->value = value;
}

Type *
check(Node *e, Type *want)
{
    Type *t = check_expr(e, want);
    e->ty = t;
    return t;
}

static bool
is_local(Str name)
{
    for (Local *local = scope; local != NULL; local = local->next) {
        if (str_equal(local->name, name))
            return true;
    }
    return false;
}

// Fails for a name that isn't defined. If one of the modules the file
// imports has it, the message says so.
void
unknown_name(Node *e, const char *message)
{
    error_start(e->file, e->line);
    if (e->module != 0) {
        put_c(&errors, "no '");
        put(&errors, e->name);
        put_c(&errors, "' in module ");
        put(&errors, modules[e->module]);
        error_end();
    }
    put_c(&errors, message);
    put_c(&errors, " '");
    put(&errors, e->name);
    put_c(&errors, "'");
    SourceFile *f = &files[e->file];
    for (uint32_t i = 0; i < f->import_count; i++) {
        uint32_t m = f->imports[i];
        if (find_sym(m, e->name) != NULL) {
            put_c(&errors, "; did you mean '");
            put(&errors, modules[m]);
            put_byte(&errors, '.');
            put(&errors, e->name);
            put_c(&errors, "'?");
            break;
        }
    }
    error_end();
}

static Type *
lookup(Node *e)
{
    for (Local *local = scope; local != NULL; local = local->next) {
        if (str_equal(local->name, e->name)) {
            e->local = local;
            return local->ty;
        }
    }
    if (e->module == 0 && str_equal(e->name, S("KiB"))) {
        set_const(e, 1024);
        return t_untyped;
    }
    if (e->module == 0 && str_equal(e->name, S("MiB"))) {
        set_const(e, 1048576);
        return t_untyped;
    }
    Sym *s = find_sym(module_of(e), e->name);
    if (s == NULL)
        unknown_name(e, "unknown name");
    declare(s);
    e->sym = s;
    switch (s->kind) {
    case SYM_GLOBAL:
        return s->ty;
    case SYM_CONST:
        set_const(e, s->value);
        return s->ty;
    case SYM_FN:
        return func_type(s->node->list, s->ty);
    default:
        break;
    }
    fail_name(e, "this is a type:", e->name);
}

// Turns io.write, where io is a module the file imports, into the name
// write found in io. A local or a name of the file's own module hides the
// module.
static void
qualify(Node *e)
{
    if (e->kind != N_MEMBER || e->a->kind != N_NAME || e->a->module != 0)
        return;
    Str name = e->a->name;
    if (is_local(name) || find_sym(files[e->a->file].module, name) != NULL)
        return;
    uint32_t m = imported(e->a->file, name);
    if (m == 0)
        return;
    e->kind = N_NAME;
    e->module = m;
    e->a = NULL;
}

// Whether the name refers to a value rather than an enum.
static bool
is_value_name(Node *e)
{
    if (e->module == 0 && is_local(e->name))
        return true;
    Sym *s = find_sym(module_of(e), e->name);
    return s == NULL || s->kind != SYM_ENUM;
}

static void
check_index(Node *e)
{
    Type *t = check(e, NULL);
    if (t == t_untyped)
        coerce(e, t, t_u32);
    else if (!is_int(t))
        fail(e, "an index must be an integer");
}

static Type *
check_member(Node *e)
{
    qualify(e->a);
    if (e->a->kind == N_NAME && !is_value_name(e->a)) {
        Sym *s = find_sym(module_of(e->a), e->a->name);
        declare(s);
        for (Node *item = s->node->list; item != NULL; item = item->next) {
            if (str_equal(item->name, e->name)) {
                set_const(e, item->value);
                return s->ty;
            }
        }
        fail_name(e, "not in the enum:", e->name);
    }
    Type *t = check(e->a, NULL);
    if ((t->kind == TY_SLICE || t->kind == TY_ARRAY) && str_equal(e->name, S("len"))) {
        if (t->kind == TY_ARRAY)
            set_const(e, t->length);
        return t_u32;
    }
    if (t->kind == TY_SLICE && str_equal(e->name, S("ptr")))
        return ptr_to(t->elem);
    Type *st = t;
    if (t->kind == TY_PTR)
        st = t->elem;
    if (st->kind != TY_STRUCT) {
        if (e->a->kind == N_NAME && e->a->module == 0 && imported(e->a->file, e->a->name) != 0)
            fail_name(e->a, "this variable hides the module", e->a->name);
        fail_name(e, "no such field:", e->name);
    }
    complete(st);
    for (Field *f = st->fields; f != NULL; f = f->next) {
        if (str_equal(f->name, e->name)) {
            e->value = f->offset;
            return f->ty;
        }
    }
    fail_name(e, "no such field:", e->name);
}

// A call to a function by its name is direct. Anything else, like a
// variable or field holding a function, is called through its value.
static Type *
check_call(Node *e)
{
    Node *params;
    Type *result;
    Sym *s = NULL;
    qualify(e->a);
    if (e->a->kind == N_NAME && (e->a->module != 0 || !is_local(e->a->name)))
        s = find_sym(module_of(e->a), e->a->name);
    if (s != NULL && s->kind == SYM_FN) {
        declare(s);
        e->sym = s;
        params = s->node->list;
        result = s->ty;
    } else {
        Type *t = check(e->a, NULL);
        if (t->kind != TY_FUNC)
            fail(e, "only functions can be called");
        params = t->params;
        result = t->elem;
    }
    Node *p = params;
    Node *a = e->list;
    while (p != NULL && a != NULL) {
        Type *at = check(a, p->ty);
        coerce(a, at, p->ty);
        p = p->next;
        a = a->next;
    }
    if (p != NULL || a != NULL) {
        if (s != NULL && s->kind == SYM_FN)
            fail_name(e, "wrong number of arguments for", s->name);
        fail(e, "wrong number of arguments");
    }
    return result;
}

static Type *
check_unary(Node *e, Type *want)
{
    Type *t;
    if (e->op == T_AMP) {
        t = check(e->a, NULL);
        if (!is_place(e->a))
            fail(e, "can't take the address of this");
        return ptr_to(t);
    }
    if (e->op == T_STAR) {
        t = check(e->a, NULL);
        if (t->kind != TY_PTR)
            fail(e, "only pointers can be followed");
        complete(t->elem);
        return t->elem;
    }
    t = check(e->a, want);
    if (e->op == T_BANG) {
        if (t != t_bool)
            fail(e, "'!' needs a bool");
        return t_bool;
    }
    if (t == t_untyped) {
        if (e->op == T_MINUS)
            set_const(e, 0u - e->a->value);
        else
            set_const(e, ~e->a->value);
        return t_untyped;
    }
    if (is_fixed(t) && e->op == T_MINUS) {
        if (e->a->flags & IS_CONST)
            set_const(e, 0u - e->a->value);
        return t;
    }
    if (!is_int(t))
        fail(e, "this operator needs an integer");
    if (e->a->flags & IS_CONST) {
        if (e->op == T_MINUS)
            set_const(e, 0u - e->a->value);
        else
            set_const(e, ~e->a->value);
    }
    return arith(t);
}

bool
is_comparison(Tok op)
{
    return op == T_EQUAL || op == T_NOT_EQUAL || op == T_LESS || op == T_LESS_EQUAL || op == T_GREATER ||
           op == T_GREATER_EQUAL;
}

// Arithmetic as the Terrarium CPU does it: shifts use the low five bits of
// the amount, and dividing the most negative number by -1 gives it back.
static uint32_t
fold(Node *e, Tok op, uint32_t a, uint32_t b)
{
    if ((op == T_SLASH || op == T_PERCENT) && b == 0)
        fail(e, "division by zero");
    switch (op) {
    case T_PLUS: return a + b;
    case T_MINUS: return a - b;
    case T_STAR: return a * b;
    case T_SLASH: return a / b;
    case T_PERCENT: return a % b;
    case T_AMP: return a & b;
    case T_PIPE: return a | b;
    case T_CARET: return a ^ b;
    case T_SHL: return a << (b & 31);
    case T_SHR: return a >> (b & 31);
    default: break;
    }
    fail(e, "this operator needs integers");
}

// Works out an operation on two typed integer constants.
static void
fold_typed(Node *e, Type *ty)
{
    if ((e->a->flags & IS_CONST) == 0 || (e->b->flags & IS_CONST) == 0)
        return;
    uint32_t a = e->a->value;
    uint32_t b = e->b->value;
    if (ty->is_signed && (e->op == T_SLASH || e->op == T_PERCENT || e->op == T_SHR)) {
        if (e->op != T_SHR && b == 0)
            fail(e, "division by zero");
        int32_t sa = (int32_t)a;
        int32_t sb = (int32_t)b;
        bool overflow = a == 0x80000000u && b == 0xffffffffu;
        if (e->op == T_SLASH) {
            set_const(e, overflow ? a : (uint32_t)(sa / sb));
        } else if (e->op == T_PERCENT) {
            set_const(e, overflow ? 0 : (uint32_t)(sa % sb));
        } else {
            uint32_t n = b & 31;
            uint32_t fill = (a & 0x80000000u) && n ? ~(0xffffffffu >> n) : 0;
            set_const(e, a >> n | fill);
        }
        return;
    }
    set_const(e, fold(e, e->op, a, b));
}

static Type *
check_binary(Node *e)
{
    Tok op = e->op;
    if (op == T_AND_AND || op == T_OR_OR) {
        if (check(e->a, NULL) != t_bool)
            fail(e->a, "'&&' and '||' need bools");
        if (check(e->b, NULL) != t_bool)
            fail(e->b, "'&&' and '||' need bools");
        return t_bool;
    }
    Type *lt = check(e->a, NULL);
    Type *rt = check(e->b, NULL);
    bool comparison = is_comparison(op);
    if (lt == t_untyped && rt == t_untyped) {
        if (comparison) {
            coerce(e->a, lt, t_u32);
            coerce(e->b, rt, t_u32);
            e->operand = t_u32;
            return t_bool;
        }
        set_const(e, fold(e, op, e->a->value, e->b->value));
        return t_untyped;
    }
    if (op == T_SHL || op == T_SHR) {
        if (lt == t_untyped) {
            coerce(e->a, lt, t_u32);
            lt = t_u32;
        }
        if (!is_int(lt))
            fail(e, "only integers can be shifted");
        if (rt == t_untyped)
            coerce(e->b, rt, t_u32);
        else if (!is_int(rt))
            fail(e, "shift by an integer");
        fold_typed(e, arith(lt));
        return arith(lt);
    }
    Type *ty;
    if (lt == t_untyped) {
        ty = arith(rt);
    } else if (rt == t_untyped) {
        ty = arith(lt);
    } else {
        ty = arith(lt);
        if (arith(rt) != ty) {
            if ((lt->kind == TY_PTR || lt->kind == TY_FUNC) && rt == t_null) {
                ty = lt;
            } else if (lt == t_null && (rt->kind == TY_PTR || rt->kind == TY_FUNC)) {
                ty = rt;
            } else {
                error_start(e->file, e->line);
                put_c(&errors, "can't combine ");
                put_type(&errors, lt);
                put_c(&errors, " and ");
                put_type(&errors, rt);
                error_end();
            }
        }
    }
    if (lt == t_null && rt == t_null)
        fail(e, "comparing null with null");
    if (lt != ty && !(is_int(lt) && arith(lt) == ty))
        coerce(e->a, lt, ty);
    if (rt != ty && !(is_int(rt) && arith(rt) == ty))
        coerce(e->b, rt, ty);
    e->operand = ty;
    if (comparison) {
        if ((ty->kind == TY_PTR || ty->kind == TY_BOOL || ty->kind == TY_FUNC) && op != T_EQUAL && op != T_NOT_EQUAL)
            fail(e, "pointers, functions and bools can only be compared with == and !=");
        if (ty->kind != TY_INT && ty->kind != TY_FIXED && ty->kind != TY_ENUM && ty->kind != TY_PTR &&
            ty->kind != TY_BOOL && ty->kind != TY_FUNC)
            fail(e, "these values can't be compared");
        return t_bool;
    }
    if (is_fixed(ty) && (op == T_PLUS || op == T_MINUS || op == T_STAR || op == T_SLASH))
        return ty;
    if (!is_int(ty))
        fail(e, "this operator needs integers");
    fold_typed(e, ty);
    return ty;
}

static Type *
check_cast(Node *e)
{
    Type *target = resolve(e->b);
    Type *want = NULL;
    if (is_int(target))
        want = target;
    Type *t = check(e->a, want);
    if (t == t_untyped) {
        if (is_int(target)) {
            e->a->ty = t_u32;
            uint32_t value = e->a->value;
            if (target->size == 1)
                value &= 255;
            else if (target->size == 2)
                value &= 0xffff;
            set_const(e, value);
            return target;
        }
        if (target->kind == TY_ENUM) {
            set_const(e, e->a->value);
            return target;
        }
        if (is_fixed(target)) {
            e->a->ty = t_u32;
            set_const(e, e->a->value << 16);
            return target;
        }
        error_start(e->file, e->line);
        put_c(&errors, "can't convert a number to ");
        put_type(&errors, target);
        error_end();
    }
    bool ok = t == target;
    if (is_int(t) && (is_int(target) || target->kind == TY_ENUM || target->kind == TY_PTR))
        ok = true;
    if (t->kind == TY_ENUM && is_int(target))
        ok = true;
    if (t->kind == TY_PTR && (target->kind == TY_PTR || target == t_u32))
        ok = true;
    if (t == t_bool && is_int(target))
        ok = true;
    if ((is_int(t) && is_fixed(target)) || (is_fixed(t) && is_int(target)))
        ok = true;
    if (!ok) {
        error_start(e->file, e->line);
        put_c(&errors, "can't convert ");
        put_type(&errors, t);
        put_c(&errors, " to ");
        put_type(&errors, target);
        error_end();
    }
    return target;
}

static Type *
check_expr(Node *e, Type *want)
{
    Type *t;
    switch (e->kind) {
    case N_INT:
        set_const(e, e->value);
        return t_untyped;
    case N_FIXED:
        set_const(e, e->value);
        return t_fixed;
    case N_BOOL:
        return t_bool;
    case N_NULL:
        return t_null;
    case N_STR:
        return slice_of(t_u8);
    case N_SIZE_OF:
        set_const(e, resolve(e->a)->size);
        return t_untyped;
    case N_NAME:
        return lookup(e);
    case N_MEMBER:
        qualify(e);
        if (e->kind == N_NAME)
            return lookup(e);
        return check_member(e);
    case N_INDEX:
        t = check(e->a, NULL);
        if (t->kind != TY_SLICE && t->kind != TY_ARRAY)
            fail(e, "only arrays and slices can be indexed");
        if (e->b == NULL)
            fail(e, "expected an index");
        check_index(e->b);
        return t->elem;
    case N_SLICE:
        t = check(e->a, NULL);
        if (t->kind != TY_SLICE && t->kind != TY_ARRAY && t->kind != TY_PTR)
            fail(e, "only arrays, slices and pointers can be sliced");
        if (t->kind == TY_ARRAY && !is_place(e->a))
            fail(e, "can only slice an array in a variable");
        if (t->kind == TY_PTR && e->c == NULL)
            fail(e, "slicing a pointer needs an end");
        if (e->b != NULL)
            check_index(e->b);
        if (e->c != NULL)
            check_index(e->c);
        return slice_of(t->elem);
    case N_CALL:
        return check_call(e);
    case N_UNARY:
        return check_unary(e, want);
    case N_BINARY:
        return check_binary(e);
    case N_CAST:
        return check_cast(e);
    case N_NEW: {
        Type *arena_type = check(e->a, NULL);
        uint32_t mem_module = module_named(S("mem"));
        Sym *alloc_sym = find_sym(mem_module, S("alloc"));
        if (mem_module == 0 || alloc_sym == NULL || alloc_sym->kind != SYM_FN)
            fail(e, "'new' needs mem.alloc, from the module mem");
        declare(alloc_sym);
        Node *p = alloc_sym->node->list;
        if (p == NULL || p->next == NULL || p->next->next != NULL || p->next->ty != t_u32 ||
            alloc_sym->ty != ptr_to(t_u8))
            fail(e, "'alloc' must be 'fn alloc(arena *Arena, size u32) *u8'");
        coerce(e->a, arena_type, p->ty);
        e->sym = alloc_sym;
        t = resolve(e->b);
        if (e->c == NULL)
            return ptr_to(t);
        Type *ct = check(e->c, t_u32);
        coerce(e->c, ct, t_u32);
        return slice_of(t);
    }
    default:
        break;
    }
    fail(e, "unknown expression");
}
