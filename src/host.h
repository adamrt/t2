#pragma once

#include "machine.h"

#include <stdio.h>

#define HOST_FAILED 0xffffffffu

// A TRX2 executable's header is 32 bytes; see docs/executable.md.
#define EXECUTABLE_HEADER_SIZE 32

typedef struct {
    uint32_t entry_pc;
    uint32_t code_size;
    uint32_t data_size;
    uint32_t bss_size;
    uint32_t stack_size;
    uint32_t memory_size;
    uint32_t permissions;
    const uint8_t *code_bytes;
    const uint8_t *data_bytes;
} Executable;

uint32_t align4(uint32_t n);
uint8_t *read_file(const char *path, size_t *size);

// Returns NULL, or what's wrong with the file.
const char *parse_executable(const uint8_t *file, size_t size, Executable *exe);

typedef struct Symbol Symbol;
typedef struct {
    Symbol *items;
    size_t count;
} SymbolTable;

void symbols_load(SymbolTable *table, const char *program);
void symbols_free(SymbolTable *table);
void print_location(const SymbolTable *table, uint32_t address);
void report_fault(Machine *machine, const SymbolTable *symbols);

uint64_t host_monotonic_us(void);
// A zero deadline means no deadline; -1 means an unlimited timeout.
int host_deadline_ms(uint64_t deadline_us, uint64_t now_us);
int host_min_timeout(int first_ms, int second_ms);
