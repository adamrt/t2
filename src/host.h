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

typedef struct {
    const char *disk;
    const char *symbols;
    bool window;
    bool fullscreen;
    uint32_t width;         // a screen without a window, for testing
    uint32_t height;
    uint32_t run_for;       // milliseconds before stopping, or 0
    const char *screenshot; // where to save the screen when stopping
    const char *type;       // keys to press once the system has started
} BootOptions;

int boot(const BootOptions *options);
