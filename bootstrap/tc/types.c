// Types, the symbol table, and top-level declarations. The port of
// system/tc/types.tr.

#include "tc.h"

enum {
    BUCKETS = 4096,
};

static Sym *buckets[BUCKETS];
Sym *first_sym;
static Sym *last_sym;
Node *manifest_memory;
Node *manifest_stack;
Node *manifest_allow;

Type *t_u8, *t_u16, *t_u32, *t_i32, *t_fixed, *t_bool, *t_untyped, *t_null, *t_void;
static Type *func_types;

static Type *resolve_ref(Node *tn);

static Type *
make_type(TypeKind kind, Str name, uint32_t size)
{
    Type *t = alloc(sizeof(Type));
    t->kind = kind;
    t->name = name;
    t->size = size;
    t->align = size;
    if (size > 4 || size == 0)
        t->align = 4;
    return t;
}

static void
init_types(void)
{
    t_u8 = make_type(TY_INT, S("u8"), 1);
    t_u16 = make_type(TY_INT, S("u16"), 2);
    t_u32 = make_type(TY_INT, S("u32"), 4);
    t_i32 = make_type(TY_INT, S("i32"), 4);
    t_i32->is_signed = true;
    t_fixed = make_type(TY_FIXED, S("fixed"), 4);
    t_fixed->is_signed = true;
    t_bool = make_type(TY_BOOL, S("bool"), 1);
    t_untyped = make_type(TY_UNTYPED, S("constant"), 4);
    t_null = make_type(TY_NULL, S("null"), 4);
    t_void = make_type(TY_VOID, S("nothing"), 0);
}

static Type *
builtin_type(Str name)
{
    if (str_equal(name, S("u8"))) return t_u8;
    if (str_equal(name, S("u16"))) return t_u16;
    if (str_equal(name, S("u32"))) return t_u32;
    if (str_equal(name, S("i32"))) return t_i32;
    if (str_equal(name, S("fixed"))) return t_fixed;
    if (str_equal(name, S("bool"))) return t_bool;
    return NULL;
}

Type *
ptr_to(Type *t)
{
    if (t->ptr == NULL) {
        t->ptr = make_type(TY_PTR, S(""), 4);
        t->ptr->elem = t;
    }
    return t->ptr;
}

Type *
slice_of(Type *t)
{
    if (t->slice == NULL) {
        t->slice = make_type(TY_SLICE, S(""), 8);
        t->slice->elem = t;
    }
    return t->slice;
}

static Type *
array_of(Type *t, uint32_t length)
{
    Type *a = t->arrays;
    while (a != NULL) {
        if (a->length == length)
            return a;
        a = a->next;
    }
    a = make_type(TY_ARRAY, S(""), t->size * length);
    a->align = t->align;
    a->elem = t;
    a->length = length;
    a->next = t->arrays;
    t->arrays = a;
    return a;
}

// The function type with these parameters and result. Equal signatures
// share one type, so types can be compared as pointers.
Type *
func_type(Node *params, Type *result)
{
    Type *t = func_types;
    while (t != NULL) {
        Node *a = t->params;
        Node *b = params;
        while (a != NULL && b != NULL && a->ty == b->ty) {
            a = a->next;
            b = b->next;
        }
        if (a == NULL && b == NULL && t->elem == result)
            return t;
        t = t->next;
    }
    t = make_type(TY_FUNC, S(""), 4);
    t->params = params;
    t->elem = result;
    t->next = func_types;
    func_types = t;
    return t;
}

bool
is_int(Type *t)
{
    return t->kind == TY_INT;
}

bool
is_scalar(Type *t)
{
    return t->kind == TY_INT || t->kind == TY_FIXED || t->kind == TY_BOOL || t->kind == TY_PTR ||
           t->kind == TY_ENUM || t->kind == TY_FUNC;
}

bool
is_fixed(Type *t)
{
    return t->kind == TY_FIXED;
}

bool
is_aggregate(Type *t)
{
    return t->kind == TY_STRUCT || t->kind == TY_ARRAY;
}

// The type a value has when it takes part in arithmetic.
Type *
arith(Type *t)
{
    if (t == t_u8 || t == t_u16)
        return t_u32;
    return t;
}

void
put_type(Writer *w, Type *t)
{
    switch (t->kind) {
    case TY_PTR:
        put_byte(w, '*');
        put_type(w, t->elem);
        break;
    case TY_SLICE:
        put_c(w, "[]");
        put_type(w, t->elem);
        break;
    case TY_ARRAY:
        put_byte(w, '[');
        put_u32(w, t->length);
        put_byte(w, ']');
        put_type(w, t->elem);
        break;
    case TY_STRUCT:
    case TY_ENUM:
        if (t->sym->module != 0) {
            put(w, modules[t->sym->module]);
            put_byte(w, '.');
        }
        put(w, t->sym->name);
        break;
    case TY_FUNC:
        put_c(w, "fn(");
        for (Node *p = t->params; p != NULL; p = p->next) {
            put_type(w, p->ty);
            if (p->next != NULL)
                put_c(w, ", ");
        }
        put_byte(w, ')');
        if (t->elem != t_void) {
            put_byte(w, ' ');
            put_type(w, t->elem);
        }
        break;
    default:
        put(w, t->name);
        break;
    }
}

// "expected X, found Y"
void
fail_types(Node *n, Type *want, Type *found)
{
    error_start(n->file, n->line);
    put_c(&errors, "expected ");
    put_type(&errors, want);
    put_c(&errors, ", found ");
    put_type(&errors, found);
    error_end();
}

//
// Symbols
//

static uint32_t
hash(Str name)
{
    uint32_t h = 0x811c9dc5;
    for (uint32_t i = 0; i < name.n; i++)
        h = (h ^ name.p[i]) * 0x01000193;
    return h & (BUCKETS - 1);
}

// Each module has its own names, so the same name can be in several.
Sym *
find_sym(uint32_t module, Str name)
{
    for (Sym *s = buckets[hash(name)]; s != NULL; s = s->next) {
        if (s->module == module && str_equal(s->name, name))
            return s;
    }
    return NULL;
}

// The module a name in the source refers to: the one it was qualified
// with, or else the one its file belongs to.
uint32_t
module_of(Node *n)
{
    if (n->module != 0)
        return n->module;
    return files[n->file].module;
}

static Sym *
add_sym(SymKind kind, Node *n)
{
    uint32_t module = files[n->file].module;
    if (find_sym(module, n->name) != NULL || builtin_type(n->name) != NULL || str_equal(n->name, S("KiB")) ||
        str_equal(n->name, S("MiB")))
        fail_name(n, "already defined:", n->name);
    Sym *s = alloc(sizeof(Sym));
    s->kind = kind;
    s->name = n->name;
    s->module = module;
    s->node = n;
    uint32_t h = hash(n->name);
    s->next = buckets[h];
    buckets[h] = s;
    if (last_sym == NULL)
        first_sym = s;
    else
        last_sym->after = s;
    last_sym = s;
    if (kind == SYM_STRUCT || kind == SYM_ENUM) {
        s->ty = make_type(TY_STRUCT, S(""), 0);
        s->ty->align = 1;
        s->ty->sym = s;
        if (kind == SYM_ENUM) {
            s->ty->kind = TY_ENUM;
            s->ty->size = 4;
            s->ty->align = 4;
        }
    }
    return s;
}

//
// Declarations
//

// Turns a list of permission names, like "read write", into their bits.
static uint32_t
permissions(Node *e)
{
    static const char *names[] = { "read", "write", "spawn", "window", "processes", "recovery", "audio", "network" };
    if (e->kind != N_STR)
        fail(e, "allow takes a list of permissions, like \"read write\"");
    uint32_t bits = 0;
    Str rest = e->name;
    while (rest.n > 0) {
        if (rest.p[0] == ' ') {
            rest = str_slice(rest, 1, rest.n);
            continue;
        }
        uint32_t end = 0;
        while (end < rest.n && rest.p[end] != ' ')
            end++;
        Str word = str_slice(rest, 0, end);
        rest = str_slice(rest, end, rest.n);
        uint32_t i = 0;
        while (i < 8) {
            Str name = { (const uint8_t *)names[i], 0 };
            while (names[i][name.n] != '\0')
                name.n++;
            if (str_equal(word, name))
                break;
            i++;
        }
        if (i == 8)
            fail_name(e, "unknown permission", word);
        bits |= 1u << i;
    }
    return bits;
}

// Gives each parameter its type. A parameter's type node is in a, or for a
// function type, the parameter is the type node itself.
static void
resolve_params(Node *p)
{
    while (p != NULL) {
        Node *tn = p->a;
        if (p->kind != N_PARAM)
            tn = p;
        p->ty = resolve(tn);
        if (!is_scalar(p->ty) && p->ty->kind != TY_SLICE)
            fail(p, "parameters must be integers, bool, enums, pointers, functions or slices");
        p = p->next;
    }
}

static Type *
resolve_result(Node *tn)
{
    if (tn == NULL)
        return t_void;
    Type *t = resolve(tn);
    if (!is_scalar(t) && t->kind != TY_SLICE)
        fail(tn, "results must be integers, bool, enums, pointers, functions or slices");
    return t;
}

void
declare(Sym *s)
{
    if (s->state == 2)
        return;
    if (s->state == 1)
        fail_name(s->node, "depends on itself:", s->name);
    s->state = 1;
    Node *n = s->node;
    switch (s->kind) {
    case SYM_STRUCT: {
        uint32_t offset = 0;
        uint32_t align = 1;
        Field *last = NULL;
        for (Node *f = n->list; f != NULL; f = f->next) {
            Type *ft = resolve(f->a);
            for (Field *other = s->ty->fields; other != NULL; other = other->next) {
                if (str_equal(other->name, f->name))
                    fail_name(f, "field is already defined:", f->name);
            }
            offset = (offset + ft->align - 1) / ft->align * ft->align;
            Field *field = alloc(sizeof(Field));
            field->name = f->name;
            field->offset = offset;
            field->ty = ft;
            if (last == NULL)
                s->ty->fields = field;
            else
                last->next = field;
            last = field;
            offset += ft->size;
            if (ft->align > align)
                align = ft->align;
        }
        s->ty->size = (offset + align - 1) / align * align;
        s->ty->align = align;
        break;
    }
    case SYM_ENUM: {
        uint32_t v = 0;
        for (Node *item = n->list; item != NULL; item = item->next) {
            if (item->a != NULL)
                v = const_value(item->a, t_u32);
            for (Node *other = n->list; other != item; other = other->next) {
                if (str_equal(other->name, item->name))
                    fail_name(item, "already in the enum:", item->name);
            }
            item->value = v;
            v++;
        }
        break;
    }
    case SYM_CONST:
        if (n->a != NULL) {
            s->ty = resolve(n->a);
            if (!is_int(s->ty))
                fail(n, "constants must be integers");
            s->value = const_value(n->b, s->ty);
        } else {
            s->ty = check(n->b, NULL);
            if (s->ty != t_untyped && !is_int(s->ty) && s->ty->kind != TY_ENUM)
                fail(n, "constants must be integers");
            s->value = const_value(n->b, NULL);
        }
        break;
    case SYM_GLOBAL:
        s->ty = resolve(n->a);
        break;
    case SYM_FN:
        resolve_params(n->list);
        s->ty = resolve_result(n->a);
        break;
    }
    s->state = 2;
}

void
check_program(void)
{
    init_types();
    for (Node *d = declarations.first; d != NULL; d = d->next) {
        switch (d->kind) {
        case N_FN: add_sym(SYM_FN, d); break;
        case N_STRUCT: add_sym(SYM_STRUCT, d); break;
        case N_ENUM: add_sym(SYM_ENUM, d); break;
        case N_CONST: add_sym(SYM_CONST, d); break;
        case N_GLOBAL: add_sym(SYM_GLOBAL, d); break;
        case N_MANIFEST:
            for (Node *entry = d->list; entry != NULL; entry = entry->next) {
                if (str_equal(entry->name, S("memory")))
                    manifest_memory = entry;
                else if (str_equal(entry->name, S("stack")))
                    manifest_stack = entry;
                else if (str_equal(entry->name, S("allow")))
                    manifest_allow = entry;
                else
                    fail_name(entry, "unknown manifest entry", entry->name);
            }
            break;
        default:
            break;
        }
    }

    for (Sym *s = first_sym; s != NULL; s = s->after)
        declare(s);
    if (manifest_memory != NULL)
        manifest_memory->value = const_value(manifest_memory->a, t_u32);
    if (manifest_stack != NULL)
        manifest_stack->value = const_value(manifest_stack->a, t_u32);
    if (manifest_allow != NULL)
        manifest_allow->value = permissions(manifest_allow->a);

    for (Sym *s = first_sym; s != NULL; s = s->after) {
        if (s->kind == SYM_FN && s->node->b != NULL)
            check_function(s);
    }

    Sym *main_sym = find_sym(0, S("main"));
    if (main_sym == NULL || main_sym->kind != SYM_FN)
        fail_plain("no 'main' function");
    Node *p = main_sym->node->list;
    if (p == NULL || p->next != NULL || p->ty != slice_of(t_u8) || main_sym->ty != t_u32)
        fail(main_sym->node, "main must be 'fn main(memory []u8) u32'");
}

// The type a type node names, complete enough to know its size.
Type *
resolve(Node *tn)
{
    switch (tn->kind) {
    case N_TYPE_FN:
        resolve_params(tn->list);
        return func_type(tn->list, resolve_result(tn->a));
    case N_TYPE_PTR:
        return ptr_to(resolve_ref(tn->a));
    case N_TYPE_SLICE:
        return slice_of(resolve_ref(tn->a));
    case N_TYPE_ARRAY: {
        Type *elem = resolve(tn->b);
        return array_of(elem, const_value(tn->a, t_u32));
    }
    default:
        break;
    }
    Type *t = resolve_ref(tn);
    complete(t);
    return t;
}

// Like resolve, but a struct may still be incomplete, as behind a pointer.
static Type *
resolve_ref(Node *tn)
{
    if (tn->kind != N_TYPE_NAME)
        return resolve(tn);
    Type *t = NULL;
    if (tn->module == 0)
        t = builtin_type(tn->name);
    if (t != NULL)
        return t;
    Sym *s = find_sym(module_of(tn), tn->name);
    if (s == NULL || (s->kind != SYM_STRUCT && s->kind != SYM_ENUM))
        unknown_name(tn, "unknown type");
    return s->ty;
}

void
complete(Type *t)
{
    if (t->kind == TY_STRUCT || t->kind == TY_ENUM)
        declare(t->sym);
}

// Checks e as a constant and returns its value.
uint32_t
const_value(Node *e, Type *want)
{
    Type *t = check(e, want);
    if ((e->flags & IS_CONST) == 0)
        fail(e, "expected a constant");
    if (want != NULL)
        coerce(e, t, want);
    return e->value;
}
