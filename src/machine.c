#include "machine.h"

#include <stdlib.h>
#include <string.h>

bool
machine_init(Machine *m)
{
    memset(m, 0, sizeof(*m));
    m->ram = calloc(RAM_SIZE, 1);
    return m->ram != NULL;
}

void
machine_free(Machine *m)
{
    free(m->ram);
    m->ram = NULL;
}

Format
op_format(uint32_t op)
{
    switch (op) {
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
    case OP_HALT:
        return FMT_NONE;
    case OP_COPY:
    case OP_FILL:
        return FMT_ABC;
    case OP_SYS:
        return FMT_I;
    }
    if (op >= OP_ADD && op <= OP_SAR)
        return FMT_ABC;
    if (op >= OP_ADD + OP_IMM && op <= OP_SAR + OP_IMM)
        return FMT_ABI;
    return FMT_ILLEGAL;
}

const char *
fault_name(Fault fault)
{
    switch (fault) {
    case FAULT_NONE:         return "none";
    case FAULT_ILLEGAL:      return "illegal instruction";
    case FAULT_CODE_ADDRESS: return "code address";
    case FAULT_DATA_ADDRESS: return "data address";
    case FAULT_ALIGNMENT:    return "alignment";
    case FAULT_DIVIDE:       return "divide by zero";
    case FAULT_STACK:        return "stack overflow";
    }
    return "unknown";
}

static bool
legal(uint32_t ins)
{
    uint32_t a = ins >> 20 & 15;
    uint32_t b = ins >> 16 & 15;
    uint32_t c = ins >> 12 & 15;

    switch (op_format(ins >> 24)) {
    case FMT_ABC:  return a < REG_COUNT && b < REG_COUNT && c < REG_COUNT && (ins & 0xfff) == 0;
    case FMT_ABI:  return a < REG_COUNT && b < REG_COUNT;
    case FMT_AI:   return a < REG_COUNT && b == 0;
    case FMT_A:    return a < REG_COUNT && (ins & 0xfffff) == 0;
    case FMT_I:    return (ins & 0xff0000) == 0;
    case FMT_J:    return true;
    case FMT_NONE: return (ins & 0xffffff) == 0;
    case FMT_ILLEGAL: break;
    }
    return false;
}

static uint32_t
load32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}

static void
store32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint8_t *
segment_at(Machine *m, const Segment *s, uint32_t addr, uint32_t len)
{
    if (addr < NULL_GUARD)
        return NULL;
    uint32_t off = addr - NULL_GUARD;
    if (off > s->size || len > s->size - off)
        return NULL;
    return m->ram + s->base + off;
}

uint8_t *
machine_data(Machine *m, uint32_t addr, uint32_t len)
{
    return segment_at(m, &m->space.data, addr, len);
}

bool
machine_fetch(Machine *m, uint32_t pc, uint32_t *ins)
{
    const uint8_t *p = (pc & 3) ? NULL : segment_at(m, &m->space.code, pc, 4);
    if (p == NULL)
        return false;
    *ins = load32(p);
    return true;
}

static Stop
fault(Machine *m, Fault f, uint32_t addr)
{
    m->fault = f;
    m->fault_pc = m->pc;
    m->fault_addr = addr;
    return STOP_FAULT;
}

static uint32_t
shift_right_arith(uint32_t x, uint32_t n)
{
    uint32_t fill = (x & 0x80000000u) ? ~(0xffffffffu >> n) : 0;
    return x >> n | fill;
}

// Returns false on division by zero.
static bool
alu(uint32_t op, uint32_t x, uint32_t y, uint32_t *out)
{
    bool overflow = x == 0x80000000u && y == 0xffffffffu;

    switch (op) {
    case OP_ADD: *out = x + y; break;
    case OP_SUB: *out = x - y; break;
    case OP_MUL: *out = x * y; break;
    case OP_AND: *out = x & y; break;
    case OP_OR:  *out = x | y; break;
    case OP_XOR: *out = x ^ y; break;
    case OP_SHL: *out = x << (y & 31); break;
    case OP_SHR: *out = x >> (y & 31); break;
    case OP_SAR: *out = shift_right_arith(x, y & 31); break;
    case OP_DIV:
    case OP_DIVS:
    case OP_REM:
    case OP_REMS:
        if (y == 0)
            return false;
        if (op == OP_DIV)
            *out = x / y;
        else if (op == OP_REM)
            *out = x % y;
        else if (op == OP_DIVS)
            *out = overflow ? x : (uint32_t)((int32_t)x / (int32_t)y);
        else
            *out = overflow ? 0 : (uint32_t)((int32_t)x % (int32_t)y);
        break;
    }
    return true;
}

static bool
branch_taken(uint32_t op, uint32_t x, uint32_t y)
{
    switch (op) {
    case OP_BEQ:  return x == y;
    case OP_BNE:  return x != y;
    case OP_BLT:  return x < y;
    case OP_BGE:  return x >= y;
    case OP_BLTS: return (int32_t)x < (int32_t)y;
    case OP_BGES: return (int32_t)x >= (int32_t)y;
    case OP_BZ:   return x == 0;
    case OP_BNZ:  return x != 0;
    }
    return false;
}

static Stop
step(Machine *m)
{
    uint32_t *r = m->r;
    uint32_t ins;

    if (!machine_fetch(m, m->pc, &ins))
        return fault(m, FAULT_CODE_ADDRESS, m->pc);
    if (!legal(ins))
        return fault(m, FAULT_ILLEGAL, m->pc);

    uint32_t op = ins >> 24;
    uint32_t a = ins >> 20 & 15;
    uint32_t b = ins >> 16 & 15;
    uint32_t c = ins >> 12 & 15;
    uint32_t imm = (uint32_t)(int32_t)(int16_t)(ins & 0xffff);
    uint32_t next = m->pc + 4;

    // The value written to register a, if any. Writes to sp are checked
    // against the stack limit before anything changes.
    uint32_t value;
    bool write = true;

    switch (op) {
    case OP_LI:
        value = imm;
        break;

    case OP_LIH:
        value = (ins & 0xffff) << 16 | (r[a] & 0xffff);
        break;

    case OP_LD:
    case OP_LD16:
    case OP_LD8: {
        uint32_t addr = r[b] + imm;
        uint32_t size = op == OP_LD ? 4 : op == OP_LD16 ? 2 : 1;
        if (addr & (size - 1))
            return fault(m, FAULT_ALIGNMENT, addr);
        const uint8_t *p = machine_data(m, addr, size);
        if (p == NULL)
            return fault(m, FAULT_DATA_ADDRESS, addr);
        value = size == 4 ? load32(p) : size == 2 ? (uint32_t)(p[0] | p[1] << 8) : p[0];
        break;
    }

    case OP_ST:
    case OP_ST16:
    case OP_ST8: {
        uint32_t addr = r[b] + imm;
        uint32_t size = op == OP_ST ? 4 : op == OP_ST16 ? 2 : 1;
        if (addr & (size - 1))
            return fault(m, FAULT_ALIGNMENT, addr);
        uint8_t *p = machine_data(m, addr, size);
        if (p == NULL)
            return fault(m, FAULT_DATA_ADDRESS, addr);
        if (size == 4) {
            store32(p, r[a]);
        } else {
            p[0] = (uint8_t)r[a];
            if (size == 2)
                p[1] = (uint8_t)(r[a] >> 8);
        }
        write = false;
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
        if (branch_taken(op, r[a], r[b]))
            next += imm * 4;
        write = false;
        break;

    case OP_JMP:
        next = (ins & 0xffffff) * 4;
        write = false;
        break;

    case OP_CALL:
    case OP_CALLR:
    case OP_PUSH: {
        uint32_t sp = r[REG_SP] - 4;
        if (sp < m->space.stack_limit)
            return fault(m, FAULT_STACK, sp);
        uint8_t *p = (sp & 3) ? NULL : machine_data(m, sp, 4);
        if (p == NULL)
            return fault(m, FAULT_DATA_ADDRESS, sp);
        store32(p, op == OP_PUSH ? r[a] : next);
        if (op == OP_CALL)
            next = (ins & 0xffffff) * 4;
        else if (op == OP_CALLR)
            next = r[a];
        r[REG_SP] = sp;
        write = false;
        break;
    }

    case OP_RET:
    case OP_POP: {
        uint32_t sp = r[REG_SP];
        const uint8_t *p = (sp & 3) ? NULL : machine_data(m, sp, 4);
        if (p == NULL)
            return fault(m, FAULT_DATA_ADDRESS, sp);
        r[REG_SP] = sp + 4;
        if (op == OP_RET) {
            next = load32(p);
            write = false;
        } else {
            value = load32(p);
        }
        break;
    }

    case OP_COPY:
    case OP_FILL: {
        uint32_t len = r[c];
        if (len > 0) {
            uint8_t *dst = machine_data(m, r[a], len);
            if (dst == NULL)
                return fault(m, FAULT_DATA_ADDRESS, r[a]);
            if (op == OP_COPY) {
                const uint8_t *src = machine_data(m, r[b], len);
                if (src == NULL)
                    return fault(m, FAULT_DATA_ADDRESS, r[b]);
                memmove(dst, src, len);
            } else {
                memset(dst, (int)(r[b] & 0xff), len);
            }
        }
        m->cycles += len / 4;
        write = false;
        break;
    }

    case OP_SYS:
        m->sys = ins & 0xffff;
        m->pc = next;
        m->cycles++;
        return STOP_SYS;

    case OP_HALT:
        m->pc = next;
        m->cycles++;
        return STOP_HALT;

    default: {
        bool immediate = op >= OP_ADD + OP_IMM;
        uint32_t base = immediate ? op - OP_IMM : op;
        if (!alu(base, r[b], immediate ? imm : r[c], &value))
            return fault(m, FAULT_DIVIDE, 0);
        break;
    }
    }

    if (write) {
        if (a == REG_SP && value < m->space.stack_limit)
            return fault(m, FAULT_STACK, value);
        r[a] = value;
    }
    m->pc = next;
    m->cycles++;
    return STOP_BUDGET;
}

Stop
machine_run(Machine *m, uint64_t steps)
{
    while (steps-- > 0) {
        Stop stop = step(m);
        if (stop != STOP_BUDGET)
            return stop;
    }
    return STOP_BUDGET;
}
