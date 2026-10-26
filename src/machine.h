#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define MiB (1024u * 1024u)

#define RAM_SIZE   (52u * MiB)
#define VRAM_SIZE  (12u * MiB)
#define NULL_GUARD 0x10000u

enum {
    REG_SP = 8,
    REG_COUNT = 9,
};

enum {
    OP_LI = 0x01,
    OP_LIH = 0x02,

    OP_ADD = 0x10,
    OP_SUB,
    OP_MUL,
    OP_DIV,
    OP_DIVS,
    OP_REM,
    OP_REMS,
    OP_AND,
    OP_OR,
    OP_XOR,
    OP_SHL,
    OP_SHR,
    OP_SAR,

    OP_LD = 0x30,
    OP_LD16,
    OP_LD8,
    OP_ST = 0x34,
    OP_ST16,
    OP_ST8,

    OP_BEQ = 0x40,
    OP_BNE,
    OP_BLT,
    OP_BGE,
    OP_BLTS,
    OP_BGES,
    OP_BZ,
    OP_BNZ,

    OP_JMP = 0x50,
    OP_CALL,
    OP_CALLR,
    OP_RET,

    OP_PUSH = 0x60,
    OP_POP,

    OP_COPY = 0x70,
    OP_FILL,

    OP_SYS = 0x80,
    OP_HALT = 0xff,
};

// An arithmetic opcode plus OP_IMM is its immediate form.
#define OP_IMM 0x10

typedef enum {
    FMT_ILLEGAL,
    FMT_ABC,  // three registers
    FMT_ABI,  // two registers and imm16
    FMT_AI,   // one register and imm16
    FMT_A,    // one register
    FMT_I,    // imm16
    FMT_J,    // imm24
    FMT_NONE, // no operands
} Format;

typedef struct {
    uint32_t base; // offset in RAM
    uint32_t size;
} Segment;

typedef struct {
    Segment code;
    Segment data;
    uint32_t stack_limit;
} AddressSpace;

typedef enum {
    FAULT_NONE,
    FAULT_ILLEGAL,
    FAULT_CODE_ADDRESS,
    FAULT_DATA_ADDRESS,
    FAULT_ALIGNMENT,
    FAULT_DIVIDE,
    FAULT_STACK,
} Fault;

typedef enum {
    STOP_BUDGET,
    STOP_HALT,
    STOP_SYS,
    STOP_FAULT,
} Stop;

typedef struct {
    uint32_t r[REG_COUNT];
    uint32_t pc;
    uint64_t cycles;

    uint8_t *ram;
    AddressSpace space;

    uint32_t sys;        // number of the last sys instruction
    Fault fault;
    uint32_t fault_pc;
    uint32_t fault_addr;
} Machine;

bool machine_init(Machine *m);
void machine_free(Machine *m);
Stop machine_run(Machine *m, uint64_t steps);

uint8_t *machine_data(Machine *m, uint32_t addr, uint32_t len);
bool machine_fetch(Machine *m, uint32_t pc, uint32_t *ins);

Format op_format(uint32_t op);
const char *fault_name(Fault fault);
void disasm(uint32_t ins, uint32_t pc, char *out, size_t size);
