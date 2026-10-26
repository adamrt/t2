#pragma once

#include "machine.h"

#include <stdio.h>

#define HEADER_SIZE 28
#define FAILED      0xffffffffu

typedef struct {
    uint32_t entry;
    uint32_t code;
    uint32_t data;
    uint32_t bss;
    uint32_t stack;
    uint32_t memory;
    const uint8_t *code_bytes;
    const uint8_t *data_bytes;
} Executable;

uint32_t read32(const uint8_t *p);
uint32_t align4(uint32_t n);
uint8_t *read_file(const char *path, size_t *size);

// Returns NULL, or what's wrong with the file.
const char *parse_executable(const uint8_t *file, size_t size, Executable *exe);

void load_symbols(const char *program);
void print_location(uint32_t addr);
void report_fault(Machine *m);
