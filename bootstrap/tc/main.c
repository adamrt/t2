// tc0 [-L library] -o program.asm file.tr...
//
// Reads every file, and the modules they import from the library (/lib
// unless -L names another), checks the whole program, and writes assembly
// for the Terrarium assembler. The port of system/tc/main.tr.

#include "tc.h"

#include <stdlib.h>
#include <string.h>

SourceFile files[MAX_FILES];
uint32_t file_count;
Str modules[MAX_MODULES];       // their names; module 0 is the program
uint32_t module_count;
Writer errors;

static Str library;

// Helpers standing in for the io, mem and text modules -------------------------

bool
str_equal(Str a, Str b)
{
    return a.n == b.n && (a.n == 0 || memcmp(a.p, b.p, a.n) == 0);
}

Str
str_slice(Str s, uint32_t from, uint32_t to)
{
    return (Str){ s.p + from, to - from };
}

void *
alloc(size_t size)
{
    void *p = calloc(1, size ? size : 1);
    if (p == NULL) {
        fprintf(stderr, "tc0: out of memory\n");
        exit(1);
    }
    return p;
}

void
put(Writer *w, Str text)
{
    fwrite(text.p, 1, text.n, w->file);
}

void
put_c(Writer *w, const char *text)
{
    fputs(text, w->file);
}

void
put_byte(Writer *w, uint8_t c)
{
    fputc(c, w->file);
}

void
put_u32(Writer *w, uint32_t value)
{
    fprintf(w->file, "%u", value);
}

void
put_i32(Writer *w, int32_t value)
{
    if (value < 0) {
        put_byte(w, '-');
        put_u32(w, 0u - (uint32_t)value);
        return;
    }
    put_u32(w, (uint32_t)value);
}

// A NUL-terminated copy, for the C library.
static char *
c_string(Str s)
{
    char *c = alloc(s.n + 1);
    memcpy(c, s.p, s.n);
    return c;
}

// Files ----------------------------------------------------------------------

static Str
read_source(Str path)
{
    FILE *f = fopen(c_string(path), "rb");
    if (f == NULL)
        fail_path("can't read", path);
    size_t cap = 1 << 16, n = 0;
    uint8_t *text = alloc(cap);
    size_t got;
    while ((got = fread(text + n, 1, cap - n, f)) > 0) {
        n += got;
        if (n == cap) {
            cap *= 2;
            text = realloc(text, cap);
            if (text == NULL)
                fail_path("out of memory reading", path);
        }
    }
    fclose(f);
    return (Str){ text, (uint32_t)n };
}

static void
add_file(Str path, uint32_t module)
{
    if (file_count == MAX_FILES)
        fail_plain("too many files");
    files[file_count].name = path;
    files[file_count].text = read_source(path);
    files[file_count].module = module;
    file_count++;
}

// The path of a module's file in the library, or nothing if it isn't there.
static Str
module_path(Str name)
{
    uint32_t n = library.n + name.n + 4;
    uint8_t *path = alloc(n);
    memcpy(path, library.p, library.n);
    path[library.n] = '/';
    memcpy(path + library.n + 1, name.p, name.n);
    memcpy(path + library.n + 1 + name.n, ".tr", 3);
    Str s = { path, n };
    FILE *f = fopen(c_string(s), "rb");
    if (f == NULL)
        return (Str){ path, 0 };
    fclose(f);
    return s;
}

// The module with this name. The first time, its file is found in the
// library and added to those to parse.
uint32_t
load_module(Str name)
{
    uint32_t m = module_named(name);
    if (m != 0)
        return m;
    if (module_count == MAX_MODULES)
        parse_fail("too many modules");
    Str path = module_path(name);
    if (path.n == 0) {
        error_start(tok.file, tok.line);
        put_c(&errors, "no module '");
        put(&errors, name);
        put_c(&errors, "' in ");
        put(&errors, library);
        error_end();
    }
    m = module_count;
    modules[m] = name;
    module_count++;
    add_file(path, m);
    return m;
}

// The loaded module with this name, or 0.
uint32_t
module_named(Str name)
{
    for (uint32_t m = 1; m < module_count; m++) {
        if (str_equal(modules[m], name))
            return m;
    }
    return 0;
}

// The module a file imports under this name, or 0.
uint32_t
imported(uint32_t file, Str name)
{
    SourceFile *f = &files[file];
    for (uint32_t i = 0; i < f->import_count; i++) {
        if (str_equal(modules[f->imports[i]], name))
            return f->imports[i];
    }
    return 0;
}

// Errors ---------------------------------------------------------------------

// Starts an error message at a place in the source. Finish it with error_end().
void
error_start(uint32_t file, uint32_t line)
{
    put(&errors, files[file].name);
    put_byte(&errors, ':');
    put_u32(&errors, line);
    put_c(&errors, ": ");
}

void
error_end(void)
{
    put_byte(&errors, '\n');
    exit(1);
}

void
fail(Node *n, const char *message)
{
    error_start(n->file, n->line);
    put_c(&errors, message);
    error_end();
}

void
fail_name(Node *n, const char *message, Str name)
{
    error_start(n->file, n->line);
    put_c(&errors, message);
    put_c(&errors, " '");
    put(&errors, name);
    put_c(&errors, "'");
    error_end();
}

void
fail_plain(const char *message)
{
    put_c(&errors, "tc: ");
    put_c(&errors, message);
    error_end();
}

void
fail_path(const char *message, Str path)
{
    put_c(&errors, "tc: ");
    put_c(&errors, message);
    put_c(&errors, " ");
    put(&errors, path);
    error_end();
}

static NORETURN void
usage(void)
{
    fail_plain("usage: tc [-L library] -o program.asm file.tr...");
}

// The arguments are copied as tc copies them, at most 256 bytes each.
static Str
argument(char **argv, int index)
{
    size_t n = strlen(argv[index + 1]);
    if (n > 256)
        fail_plain("argument is too long");
    return (Str){ (const uint8_t *)argv[index + 1], (uint32_t)n };
}

int
main(int argc, char **argv)
{
    errors.file = stderr;
    int count = argc - 1;
    Str output = { NULL, 0 };
    library = S("/lib");
    module_count = 1;
    int i = 0;
    while (i < count) {
        Str word = argument(argv, i);
        i++;
        if (str_equal(word, S("-o")) || str_equal(word, S("-L"))) {
            if (i == count)
                usage();
            if (str_equal(word, S("-o")))
                output = argument(argv, i);
            else
                library = argument(argv, i);
            i++;
        } else {
            add_file(word, 0);
        }
    }
    if (output.n == 0 || file_count == 0)
        usage();

    // Parsing a file can load the modules it imports, which adds files.
    for (uint32_t f = 0; f < file_count; f++)
        parse_file(f);
    check_program();

    out.file = fopen(c_string(output), "wb");
    if (out.file == NULL)
        fail_path("can't write", output);
    generate();
    if (fclose(out.file) != 0)
        fail_path("can't write", output);
    return 0;
}
