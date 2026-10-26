// tc0, the Terrarium compiler in C, for building Terrarium from nothing but
// a C compiler. It is a port of system/tc/*.tr, file for file, and writes
// the same assembly byte for byte; `make test` checks that they agree.

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

// C99 has no _Noreturn; the C11 keyword, or GCC's and Clang's attribute,
// tells the compiler a function never returns.
#if __STDC_VERSION__ >= 201112L
#define NORETURN _Noreturn
#elif defined(__GNUC__)
#define NORETURN __attribute__((noreturn))
#else
#define NORETURN
#endif

// A slice of bytes, like []u8.
typedef struct {
    const uint8_t *p;
    uint32_t n;
} Str;

#define S(text) ((Str){ (const uint8_t *)(text), (uint32_t)(sizeof(text) - 1) })

bool str_equal(Str a, Str b);
Str str_slice(Str s, uint32_t from, uint32_t to);
void *alloc(size_t size);

typedef struct {
    FILE *file;
} Writer;

void put(Writer *w, Str text);
void put_c(Writer *w, const char *text);
void put_byte(Writer *w, uint8_t c);
void put_u32(Writer *w, uint32_t value);
void put_i32(Writer *w, int32_t value);

// main.c ---------------------------------------------------------------------

enum {
    MAX_FILES = 64,
    MAX_MODULES = 32,
    MAX_IMPORTS = 16,
};

typedef struct {
    Str name;
    Str text;
    uint32_t module;            // 0 for the program's own files
    uint32_t imports[MAX_IMPORTS];
    uint32_t import_count;
} SourceFile;

typedef struct Node Node;
typedef struct Type Type;
typedef struct Field Field;
typedef struct Sym Sym;
typedef struct Local Local;

extern SourceFile files[MAX_FILES];
extern uint32_t file_count;
extern Str modules[MAX_MODULES];
extern uint32_t module_count;
extern Writer errors;

uint32_t load_module(Str name);
uint32_t module_named(Str name);
uint32_t imported(uint32_t file, Str name);
void error_start(uint32_t file, uint32_t line);
NORETURN void error_end(void);
NORETURN void fail(Node *n, const char *message);
NORETURN void fail_name(Node *n, const char *message, Str name);
NORETURN void fail_plain(const char *message);
NORETURN void fail_path(const char *message, Str path);

// lex.c ----------------------------------------------------------------------

typedef enum {
    T_END,
    T_NAME,
    T_INT,
    T_FIXED,
    T_STR,

    K_FN, K_STRUCT, K_ENUM, K_CONST, K_VAR, K_MANIFEST, K_IF, K_ELSE, K_WHILE,
    K_FOR, K_IN, K_MATCH, K_BREAK, K_CONTINUE, K_RETURN, K_TRUE, K_FALSE,
    K_NULL, K_AS, K_NEW, K_SIZE_OF, K_IMPORT,

    T_LPAREN, T_RPAREN, T_LBRACKET, T_RBRACKET, T_LBRACE, T_RBRACE,
    T_COMMA, T_SEMICOLON, T_COLON, T_DOT, T_DOT_DOT, T_ARROW, T_FAT_ARROW,
    T_ASSIGN, T_PLUS, T_MINUS, T_STAR, T_SLASH, T_PERCENT, T_AMP, T_PIPE, T_CARET,
    T_TILDE, T_BANG,
    T_LESS, T_GREATER, T_EQUAL, T_NOT_EQUAL, T_LESS_EQUAL, T_GREATER_EQUAL,
    T_AND_AND, T_OR_OR,
    T_SHL, T_SHR,
    T_PLUS_ASSIGN, T_MINUS_ASSIGN, T_STAR_ASSIGN, T_SLASH_ASSIGN, T_PERCENT_ASSIGN,
    T_AMP_ASSIGN, T_PIPE_ASSIGN, T_CARET_ASSIGN, T_SHL_ASSIGN, T_SHR_ASSIGN,
} Tok;

typedef struct {
    Tok kind;
    Str text;           // the source text, or a string's decoded bytes
    uint32_t value;     // a number's value
    uint32_t file;
    uint32_t line;
} Token;

extern Token tok;

void lex_start(uint32_t file);
void next_token(void);

// parse.c --------------------------------------------------------------------

typedef enum {
    // Declarations
    N_FN,               // name, list: params, a: result type, b: body
    N_PARAM,            // name, a: type
    N_STRUCT,           // name, list: fields
    N_FIELD,            // name, a: type
    N_ENUM,             // name, list: items
    N_ITEM,             // name, a: value (enum items and manifest entries)
    N_CONST,            // name, a: type, b: value
    N_GLOBAL,           // name, a: type
    N_MANIFEST,         // list: items

    // Types
    N_TYPE_NAME,        // name, module if it has one
    N_TYPE_PTR,         // a: element
    N_TYPE_SLICE,       // a: element
    N_TYPE_ARRAY,       // a: length, b: element
    N_TYPE_FN,          // list: parameter types, a: result type

    // Statements
    N_BLOCK,            // list: statements
    N_VAR,              // name, a: type, b: value, local
    N_IF,               // a: condition, b: then, c: else
    N_WHILE,            // a: condition, b: body
    N_FOR,              // name, a: from, b: to, c: body, local: counter, local2: end
    N_MATCH,            // a: value, list: arms, b: else, local: the value
    N_ARM,              // list: values, a: body
    N_BREAK,
    N_CONTINUE,
    N_RETURN,           // a: value
    N_ASSIGN,           // op, a: target, b: value
    N_EXPR_STMT,        // a: expression

    // Expressions
    N_INT,              // value
    N_FIXED,            // value, in 16.16
    N_STR,              // name: bytes, value: label number
    N_NAME,             // name, module once io.write becomes one, then local or sym
    N_BOOL,             // value
    N_NULL,
    N_SIZE_OF,          // a: type
    N_NEW,              // a: arena, b: type, c: count, sym: alloc
    N_CALL,             // a: function name, list: arguments, sym: function
    N_INDEX,            // a: base, b: index
    N_SLICE,            // a: base, b: from, c: to
    N_MEMBER,           // a: base, name, offset
    N_UNARY,            // op, a
    N_BINARY,           // op, a, b, operand: the type compared
    N_CAST,             // a: value, b: type
} NodeKind;

enum {
    IS_CONST = 1,       // value holds the expression's constant value
    TO_SLICE = 2,       // an array used as a slice
};

struct Node {
    NodeKind kind;
    Tok op;
    uint32_t file;
    uint32_t line;
    uint32_t flags;
    Str name;
    uint32_t value;
    Node *a;
    Node *b;
    Node *c;
    Node *list;
    Node *next;
    Type *ty;
    Type *operand;
    Sym *sym;
    Local *local;
    Local *local2;
    uint32_t module;    // the module a name was found in, or 0 for the file's own
};

typedef struct {
    Node *first;
    Node *last;
} List;

extern List declarations;

NORETURN void parse_fail(const char *message);
void parse_file(uint32_t file);

// types.c --------------------------------------------------------------------

typedef enum {
    TY_INT,
    TY_FIXED,           // 16.16 fixed point
    TY_BOOL,
    TY_PTR,
    TY_SLICE,
    TY_ARRAY,
    TY_STRUCT,
    TY_ENUM,
    TY_FUNC,            // a function's address, called through
    TY_UNTYPED,         // a number that hasn't taken a type yet
    TY_NULL,
    TY_VOID,
} TypeKind;

struct Type {
    TypeKind kind;
    uint32_t size;
    uint32_t align;
    bool is_signed;
    Str name;           // for builtin types
    Type *elem;         // what a pointer, slice or array holds, or a function's result
    uint32_t length;    // an array's length
    Sym *sym;           // the struct or enum that declares it
    Field *fields;
    Node *params;       // a function type's parameters, each with its type
    Type *ptr;          // the pointer to this type, once made
    Type *slice;        // the slice of this type, once made
    Type *arrays;       // arrays of this type, linked by next
    Type *next;         // also links the function types
};

struct Field {
    Str name;
    uint32_t offset;
    Type *ty;
    Field *next;
};

typedef enum {
    SYM_FN,
    SYM_STRUCT,
    SYM_ENUM,
    SYM_CONST,
    SYM_GLOBAL,
} SymKind;

struct Sym {
    SymKind kind;
    Str name;
    uint32_t module;    // 0 for the program's own names
    Node *node;
    Type *ty;           // the struct or enum, the constant's or global's type, or the result
    uint32_t value;     // a constant's value
    uint32_t state;     // 0 new, 1 being declared, 2 declared
    bool used;          // a function main can reach, or a global one uses
    Sym *next;          // next in the same bucket
    Sym *after;         // next in declaration order
};

extern Sym *first_sym;
extern Node *manifest_memory;
extern Node *manifest_stack;
extern Node *manifest_allow;
extern Type *t_u8, *t_u16, *t_u32, *t_i32, *t_fixed, *t_bool, *t_untyped, *t_null, *t_void;

Type *ptr_to(Type *t);
Type *slice_of(Type *t);
Type *func_type(Node *params, Type *result);
bool is_int(Type *t);
bool is_scalar(Type *t);
bool is_fixed(Type *t);
bool is_aggregate(Type *t);
Type *arith(Type *t);
void put_type(Writer *w, Type *t);
NORETURN void fail_types(Node *n, Type *want, Type *found);
Sym *find_sym(uint32_t module, Str name);
uint32_t module_of(Node *n);
void check_program(void);
void declare(Sym *s);
Type *resolve(Node *tn);
void complete(Type *t);
uint32_t const_value(Node *e, Type *want);

// check.c --------------------------------------------------------------------

struct Local {
    Str name;           // empty for hidden locals
    Type *ty;
    int32_t offset;     // from r7: parameters above, locals below
    Local *next;
};

void check_function(Sym *s);
void coerce(Node *e, Type *t, Type *want);
Type *check(Node *e, Type *want);
NORETURN void unknown_name(Node *e, const char *message);
bool is_comparison(Tok op);

// gen.c ----------------------------------------------------------------------

extern Writer out;

void generate(void);
