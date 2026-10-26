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
    OP_FMUL,
    OP_FDIV,

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

    OP_IN = 0x90,
    OP_OUT,
    OP_RESUME,
    OP_WAIT,

    OP_HALT = 0xff,
};

// An arithmetic opcode plus OP_IMM is its immediate form.
#define OP_IMM 0x10

typedef enum {
    FMT_ILLEGAL,
    FMT_ABC,  // three registers
    FMT_ABI,  // two registers and imm16
    FMT_AB,   // two registers
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
    FAULT_PRIVILEGED,
    FAULT_PORT,
} Fault;

typedef enum {
    STOP_BUDGET,
    STOP_HALT,
    STOP_SYS,
    STOP_FAULT,
    STOP_WAIT,
} Stop;

// Why the kernel's last resume ended.
enum {
    CAUSE_SYS = 1,
    CAUSE_FAULT = 2,
    CAUSE_INTERRUPT = 3,
};

// The CPU's own ports. Devices use the ports from PORT_DEVICES up.
enum {
    PORT_CAUSE = 0x00,
    PORT_VALUE = 0x01,
    PORT_ADDRESS = 0x02,
    PORT_USER_PC = 0x03,
    PORT_USER_CODE_BASE = 0x04,
    PORT_USER_CODE_SIZE = 0x05,
    PORT_USER_DATA_BASE = 0x06,
    PORT_USER_DATA_SIZE = 0x07,
    PORT_USER_STACK_LIMIT = 0x08,
    PORT_PENDING = 0x09,
    PORT_CYCLES = 0x0a,
    PORT_USER_R0 = 0x10, // through PORT_USER_R0 + 8 for sp
    PORT_DEVICES = 0x20,
};

typedef struct Machine Machine;

// Device ports. Each returns false for a port it doesn't have.
typedef bool (*PortRead)(Machine *m, uint32_t port, uint32_t *value);
typedef bool (*PortWrite)(Machine *m, uint32_t port, uint32_t value);

struct Machine {
    uint32_t r[REG_COUNT];
    uint32_t pc;
    AddressSpace space;
    bool kernel;

    // The registers, pc and address space of the mode that isn't running.
    uint32_t saved_r[REG_COUNT];
    uint32_t saved_pc;
    AddressSpace saved_space;

    // With a kernel, system calls, faults and interrupts in user mode return
    // to the kernel. Without one, they stop machine_run.
    bool has_kernel;
    uint32_t cause;
    uint32_t value;
    uint32_t address;
    uint32_t pending;    // interrupt bits waiting for the kernel

    uint64_t cycles;
    uint8_t *ram;

    PortRead port_read;
    PortWrite port_write;
    void *host;

    uint32_t sys;        // number of the last sys instruction
    Fault fault;
    uint32_t fault_pc;
    uint32_t fault_addr;
};

bool machine_init(Machine *m);
void machine_free(Machine *m);
Stop machine_run(Machine *m, uint64_t steps);

uint8_t *machine_data(Machine *m, uint32_t addr, uint32_t len);
bool machine_fetch(Machine *m, uint32_t pc, uint32_t *ins);

Format op_format(uint32_t op);
const char *fault_name(Fault fault);
void disasm(uint32_t ins, uint32_t pc, char *out, size_t size);
