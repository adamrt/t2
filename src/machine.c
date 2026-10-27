#include "machine.h"
#include "bytes.h"

#include <stdlib.h>
#include <string.h>

bool
machine_init(Machine *machine)
{
    memset(machine, 0, sizeof(*machine));
    machine->ram = calloc(RAM_SIZE, 1);
    return machine->ram != NULL;
}

void
machine_free(Machine *machine)
{
    free(machine->ram);
    machine->ram = NULL;
}

Format
instruction_format(uint32_t opcode)
{
    switch (opcode) {
    case OP_LI:
    case OP_LIH:
    case OP_BZ:
    case OP_BNZ:
        return FMT_AI;
    case OP_LD:
    case OP_LD16:
    case OP_LD8:
    case OP_ST:
    case OP_ST16:
    case OP_ST8:
    case OP_BEQ:
    case OP_BNE:
    case OP_BLT:
    case OP_BGE:
    case OP_BLTS:
    case OP_BGES:
        return FMT_ABI;
    case OP_JMP:
    case OP_CALL:
        return FMT_J;
    case OP_CALLR:
    case OP_PUSH:
    case OP_POP:
        return FMT_A;
    case OP_RET:
    case OP_RESUME:
    case OP_WAIT:
    case OP_HALT:
        return FMT_NONE;
    case OP_IN:
    case OP_OUT:
        return FMT_AB;
    case OP_COPY:
    case OP_FILL:
        return FMT_ABC;
    case OP_SYS:
        return FMT_I;
    }
    if (opcode >= OP_ADD && opcode <= OP_FDIV)
        return FMT_ABC;
    if (opcode >= OP_ADD + OP_IMM && opcode <= OP_FDIV + OP_IMM)
        return FMT_ABI;
    return FMT_ILLEGAL;
}

const char *
fault_name(Fault fault)
{
    switch (fault) {
    case FAULT_NONE:
        return "none";
    case FAULT_ILLEGAL:
        return "illegal instruction";
    case FAULT_CODE_ADDRESS:
        return "code address";
    case FAULT_DATA_ADDRESS:
        return "data address";
    case FAULT_ALIGNMENT:
        return "alignment";
    case FAULT_DIVIDE:
        return "divide by zero";
    case FAULT_STACK:
        return "stack overflow";
    case FAULT_PRIVILEGED:
        return "privileged instruction";
    case FAULT_PORT:
        return "bad port";
    }
    return "unknown";
}

static bool
instruction_valid(uint32_t instruction)
{
    uint32_t a = instruction >> 20 & 15;
    uint32_t b = instruction >> 16 & 15;
    uint32_t c = instruction >> 12 & 15;

    switch (instruction_format(instruction >> 24)) {
    case FMT_ABC:
        return a < REG_COUNT && b < REG_COUNT && c < REG_COUNT && (instruction & 0xfff) == 0;
    case FMT_ABI:
        return a < REG_COUNT && b < REG_COUNT;
    case FMT_AB:
        return a < REG_COUNT && b < REG_COUNT && (instruction & 0xffff) == 0;
    case FMT_AI:
        return a < REG_COUNT && b == 0;
    case FMT_A:
        return a < REG_COUNT && (instruction & 0xfffff) == 0;
    case FMT_I:
        return (instruction & 0xff0000) == 0;
    case FMT_J:
        return true;
    case FMT_NONE:
        return (instruction & 0xffffff) == 0;
    case FMT_ILLEGAL:
        break;
    }
    return false;
}

static uint8_t *
segment_at(Machine *machine, const Segment *s, uint32_t addr, uint32_t len)
{
    if (addr < NULL_GUARD)
        return NULL;
    uint32_t off = addr - NULL_GUARD;
    if (off > s->size || len > s->size - off)
        return NULL;
    return machine->ram + s->base + off;
}

uint8_t *
machine_data(Machine *machine, uint32_t addr, uint32_t len)
{
    return segment_at(machine, &machine->space.data, addr, len);
}

bool
machine_fetch(Machine *machine, uint32_t pc, uint32_t *instruction)
{
    const uint8_t *p = (pc & 3) ? NULL : segment_at(machine, &machine->space.code, pc, 4);
    if (p == NULL)
        return false;
    *instruction = read_u32_le(p);
    return true;
}

// Swaps the running mode's registers, pc and address space with the saved
// ones. The mode being left continues at continue_pc next time.
static void
switch_mode(Machine *machine, uint32_t continue_pc)
{
    uint32_t r[REG_COUNT];
    memcpy(r, machine->r, sizeof(r));
    memcpy(machine->r, machine->saved_r, sizeof(r));
    memcpy(machine->saved_r, r, sizeof(r));
    AddressSpace space = machine->space;
    machine->space = machine->saved_space;
    machine->saved_space = space;
    machine->pc = machine->saved_pc;
    machine->saved_pc = continue_pc;
    machine->kernel = !machine->kernel;
}

// Returns from user mode to the kernel, which continues after its resume.
static Stop
trap(Machine *machine, uint32_t cause, uint32_t value, uint32_t address, uint32_t resume_pc)
{
    machine->cause = cause;
    machine->value = value;
    machine->address = address;
    switch_mode(machine, resume_pc);
    return STOP_BUDGET;
}

static Stop
fault(Machine *machine, Fault f, uint32_t addr)
{
    machine->fault = f;
    machine->fault_pc = machine->pc;
    machine->fault_addr = addr;
    if (!machine->kernel && machine->has_kernel)
        return trap(machine, CAUSE_FAULT, f, addr, machine->pc);
    return STOP_FAULT;
}

static bool
port_read(Machine *machine, uint32_t port, uint32_t *v)
{
    switch (port) {
    case PORT_CAUSE:
        *v = machine->cause;
        return true;
    case PORT_VALUE:
        *v = machine->value;
        return true;
    case PORT_ADDRESS:
        *v = machine->address;
        return true;
    case PORT_USER_PC:
        *v = machine->saved_pc;
        return true;
    case PORT_USER_CODE_BASE:
        *v = machine->saved_space.code.base;
        return true;
    case PORT_USER_CODE_SIZE:
        *v = machine->saved_space.code.size;
        return true;
    case PORT_USER_DATA_BASE:
        *v = machine->saved_space.data.base;
        return true;
    case PORT_USER_DATA_SIZE:
        *v = machine->saved_space.data.size;
        return true;
    case PORT_USER_STACK_LIMIT:
        *v = machine->saved_space.stack_limit;
        return true;
    case PORT_PENDING:
        *v = machine->pending;
        return true;
    case PORT_CYCLES:
        *v = (uint32_t)machine->cycles;
        return true;
    }
    if (port >= PORT_USER_R0 && port < PORT_USER_R0 + REG_COUNT) {
        *v = machine->saved_r[port - PORT_USER_R0];
        return true;
    }
    return port >= PORT_DEVICES && machine->port_read != NULL &&
           machine->port_read(machine, port, v);
}

static bool
port_write(Machine *machine, uint32_t port, uint32_t v)
{
    switch (port) {
    case PORT_USER_PC:
        machine->saved_pc = v;
        return true;
    case PORT_USER_CODE_BASE:
        machine->saved_space.code.base = v;
        return true;
    case PORT_USER_CODE_SIZE:
        machine->saved_space.code.size = v;
        return true;
    case PORT_USER_DATA_BASE:
        machine->saved_space.data.base = v;
        return true;
    case PORT_USER_DATA_SIZE:
        machine->saved_space.data.size = v;
        return true;
    case PORT_USER_STACK_LIMIT:
        machine->saved_space.stack_limit = v;
        return true;
    case PORT_PENDING:
        machine->pending &= ~v;
        return true;
    }
    if (port >= PORT_USER_R0 && port < PORT_USER_R0 + REG_COUNT) {
        machine->saved_r[port - PORT_USER_R0] = v;
        return true;
    }
    return port >= PORT_DEVICES && machine->port_write != NULL &&
           machine->port_write(machine, port, v);
}

// Segments must stay inside RAM, or a bad kernel could reach past it.
static bool
space_valid(const AddressSpace *s)
{
    return s->code.base <= RAM_SIZE && s->code.size <= RAM_SIZE - s->code.base &&
           s->data.base <= RAM_SIZE && s->data.size <= RAM_SIZE - s->data.base;
}

static uint32_t
shift_right_arith(uint32_t x, uint32_t n)
{
    uint32_t fill = (x & 0x80000000u) ? ~(0xffffffffu >> n) : 0;
    return x >> n | fill;
}

// Returns false on division by zero.
static bool
execute_alu(uint32_t opcode, uint32_t x, uint32_t y, uint32_t *out)
{
    bool overflow = x == 0x80000000u && y == 0xffffffffu;

    switch (opcode) {
    case OP_ADD:
        *out = x + y;
        break;
    case OP_SUB:
        *out = x - y;
        break;
    case OP_MUL:
        *out = x * y;
        break;
    case OP_AND:
        *out = x & y;
        break;
    case OP_OR:
        *out = x | y;
        break;
    case OP_XOR:
        *out = x ^ y;
        break;
    case OP_SHL:
        *out = x << (y & 31);
        break;
    case OP_SHR:
        *out = x >> (y & 31);
        break;
    case OP_SAR:
        *out = shift_right_arith(x, y & 31);
        break;
    case OP_FMUL: {
        // 16.16 fixed point: the product's middle 32 bits, rounded down.
        int64_t p = (int64_t)(int32_t)x * (int32_t)y;
        *out = (uint32_t)(uint64_t)((p - (p < 0 ? 65535 : 0)) / 65536);
        break;
    }
    case OP_FDIV:
        if (y == 0)
            return false;
        *out = (uint32_t)(uint64_t)((int64_t)(int32_t)x * 65536 / (int32_t)y);
        break;
    case OP_DIV:
    case OP_DIVS:
    case OP_REM:
    case OP_REMS:
        if (y == 0)
            return false;
        if (opcode == OP_DIV)
            *out = x / y;
        else if (opcode == OP_REM)
            *out = x % y;
        else if (opcode == OP_DIVS)
            *out = overflow ? x : (uint32_t)((int32_t)x / (int32_t)y);
        else
            *out = overflow ? 0 : (uint32_t)((int32_t)x % (int32_t)y);
        break;
    }
    return true;
}

static bool
branch_taken(uint32_t opcode, uint32_t x, uint32_t y)
{
    switch (opcode) {
    case OP_BEQ:
        return x == y;
    case OP_BNE:
        return x != y;
    case OP_BLT:
        return x < y;
    case OP_BGE:
        return x >= y;
    case OP_BLTS:
        return (int32_t)x < (int32_t)y;
    case OP_BGES:
        return (int32_t)x >= (int32_t)y;
    case OP_BZ:
        return x == 0;
    case OP_BNZ:
        return x != 0;
    }
    return false;
}

static Stop
machine_step(Machine *machine)
{
    uint32_t *r = machine->r;
    uint32_t instruction;

    if (machine->pending != 0 && !machine->kernel && machine->has_kernel)
        return trap(machine, CAUSE_INTERRUPT, machine->pending, 0, machine->pc);
    if (!machine_fetch(machine, machine->pc, &instruction))
        return fault(machine, FAULT_CODE_ADDRESS, machine->pc);
    if (!instruction_valid(instruction))
        return fault(machine, FAULT_ILLEGAL, machine->pc);

    uint32_t opcode = instruction >> 24;
    uint32_t a = instruction >> 20 & 15;
    uint32_t b = instruction >> 16 & 15;
    uint32_t c = instruction >> 12 & 15;
    uint32_t immediate = (uint32_t)(int32_t)(int16_t)(instruction & 0xffff);
    uint32_t next_pc = machine->pc + 4;

    // The value written to register a, if any. Writes to sp are checked
    // against the stack limit before anything changes.
    uint32_t value = 0;
    bool write_register = true;

    switch (opcode) {
    case OP_LI:
        value = immediate;
        break;

    case OP_LIH:
        value = (instruction & 0xffff) << 16 | (r[a] & 0xffff);
        break;

    case OP_LD:
    case OP_LD16:
    case OP_LD8: {
        uint32_t addr = r[b] + immediate;
        uint32_t size = opcode == OP_LD ? 4 : opcode == OP_LD16 ? 2 : 1;
        if (addr & (size - 1))
            return fault(machine, FAULT_ALIGNMENT, addr);
        const uint8_t *p = machine_data(machine, addr, size);
        if (p == NULL)
            return fault(machine, FAULT_DATA_ADDRESS, addr);
        value = size == 4 ? read_u32_le(p) : size == 2 ? (uint32_t)(p[0] | p[1] << 8) : p[0];
        break;
    }

    case OP_ST:
    case OP_ST16:
    case OP_ST8: {
        uint32_t addr = r[b] + immediate;
        uint32_t size = opcode == OP_ST ? 4 : opcode == OP_ST16 ? 2 : 1;
        if (addr & (size - 1))
            return fault(machine, FAULT_ALIGNMENT, addr);
        uint8_t *p = machine_data(machine, addr, size);
        if (p == NULL)
            return fault(machine, FAULT_DATA_ADDRESS, addr);
        if (size == 4) {
            write_u32_le(p, r[a]);
        } else {
            p[0] = (uint8_t)r[a];
            if (size == 2)
                p[1] = (uint8_t)(r[a] >> 8);
        }
        write_register = false;
        break;
    }

    case OP_BEQ:
    case OP_BNE:
    case OP_BLT:
    case OP_BGE:
    case OP_BLTS:
    case OP_BGES:
    case OP_BZ:
    case OP_BNZ:
        if (branch_taken(opcode, r[a], r[b]))
            next_pc += immediate * 4;
        write_register = false;
        break;

    case OP_JMP:
        next_pc = (instruction & 0xffffff) * 4;
        write_register = false;
        break;

    case OP_CALL:
    case OP_CALLR:
    case OP_PUSH: {
        uint32_t sp = r[REG_SP] - 4;
        if (sp < machine->space.stack_limit)
            return fault(machine, FAULT_STACK, sp);
        uint8_t *p = (sp & 3) ? NULL : machine_data(machine, sp, 4);
        if (p == NULL)
            return fault(machine, FAULT_DATA_ADDRESS, sp);
        write_u32_le(p, opcode == OP_PUSH ? r[a] : next_pc);
        if (opcode == OP_CALL)
            next_pc = (instruction & 0xffffff) * 4;
        else if (opcode == OP_CALLR)
            next_pc = r[a];
        r[REG_SP] = sp;
        write_register = false;
        break;
    }

    case OP_RET:
    case OP_POP: {
        uint32_t sp = r[REG_SP];
        const uint8_t *p = (sp & 3) ? NULL : machine_data(machine, sp, 4);
        if (p == NULL)
            return fault(machine, FAULT_DATA_ADDRESS, sp);
        // The stack pointer moves up, unless the pop is into sp itself.
        if (opcode == OP_RET) {
            r[REG_SP] = sp + 4;
            next_pc = read_u32_le(p);
            write_register = false;
        } else {
            value = read_u32_le(p);
            if (a != REG_SP)
                r[REG_SP] = sp + 4;
        }
        break;
    }

    case OP_COPY:
    case OP_FILL: {
        uint32_t len = r[c];
        if (len > 0) {
            uint8_t *dst = machine_data(machine, r[a], len);
            if (dst == NULL)
                return fault(machine, FAULT_DATA_ADDRESS, r[a]);
            if (opcode == OP_COPY) {
                const uint8_t *src = machine_data(machine, r[b], len);
                if (src == NULL)
                    return fault(machine, FAULT_DATA_ADDRESS, r[b]);
                memmove(dst, src, len);
            } else {
                memset(dst, (int)(r[b] & 0xff), len);
            }
        }
        machine->cycles += len / 4;
        write_register = false;
        break;
    }

    case OP_SYS:
        if (machine->kernel)
            return fault(machine, FAULT_ILLEGAL, machine->pc);
        machine->sys = instruction & 0xffff;
        machine->cycles++;
        if (machine->has_kernel)
            return trap(machine, CAUSE_SYS, machine->sys, 0, next_pc);
        machine->pc = next_pc;
        return STOP_SYS;

    case OP_HALT:
    case OP_IN:
    case OP_OUT:
    case OP_RESUME:
    case OP_WAIT:
        if (!machine->kernel)
            return fault(machine, FAULT_PRIVILEGED, machine->pc);
        if (opcode == OP_IN) {
            if (!port_read(machine, r[b], &value))
                return fault(machine, FAULT_PORT, r[b]);
            break;
        }
        if (opcode == OP_OUT && !port_write(machine, r[b], r[a]))
            return fault(machine, FAULT_PORT, r[b]);
        if (opcode == OP_WAIT && machine->pending == 0)
            return STOP_WAIT;
        if (opcode == OP_RESUME && !space_valid(&machine->saved_space))
            return fault(machine, FAULT_DATA_ADDRESS, 0);
        machine->cycles++;
        if (opcode == OP_RESUME) {
            switch_mode(machine, next_pc);
            return STOP_BUDGET;
        }
        machine->pc = next_pc;
        return opcode == OP_HALT ? STOP_HALT : STOP_BUDGET;

    default: {
        bool use_immediate = opcode >= OP_ADD + OP_IMM;
        uint32_t base = use_immediate ? opcode - OP_IMM : opcode;
        if (!execute_alu(base, r[b], use_immediate ? immediate : r[c], &value))
            return fault(machine, FAULT_DIVIDE, 0);
        break;
    }
    }

    if (write_register) {
        if (a == REG_SP && value < machine->space.stack_limit)
            return fault(machine, FAULT_STACK, value);
        r[a] = value;
    }
    machine->pc = next_pc;
    machine->cycles++;
    return STOP_BUDGET;
}

Stop
machine_run(Machine *machine, uint64_t steps)
{
    while (steps-- > 0) {
        Stop stop = machine_step(machine);
        if (stop != STOP_BUDGET)
            return stop;
    }
    return STOP_BUDGET;
}
