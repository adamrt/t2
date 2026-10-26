#include "machine.h"

#include <stdio.h>

static const char *reg_names[16] = {
    "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sp",
    "r9?", "r10?", "r11?", "r12?", "r13?", "r14?", "r15?",
};

static const char *alu_ops[] = {
    "+", "-", "*", "/", "/s", "%", "%s", "&", "|", "^", "<<", ">>", ">>s", "*f", "/f",
};

static const char *branch_ops[] = {
    "==", "!=", "<", ">=", "<s", ">=s",
};

static const char *sizes[] = { "", "u16 ", "u8 " };

static void
format_address(char *out, size_t size, const char *base, int32_t offset)
{
    if (offset == 0)
        snprintf(out, size, "[%s]", base);
    else if (offset < 0)
        snprintf(out, size, "[%s - %d]", base, -offset);
    else
        snprintf(out, size, "[%s + %d]", base, offset);
}

void
disasm(uint32_t ins, uint32_t pc, char *out, size_t size)
{
    uint32_t op = ins >> 24;
    const char *a = reg_names[ins >> 20 & 15];
    const char *b = reg_names[ins >> 16 & 15];
    const char *c = reg_names[ins >> 12 & 15];
    int32_t imm = (int16_t)(ins & 0xffff);
    uint32_t target = pc + 4 + (uint32_t)imm * 4;
    char addr[64];

    switch (op_format(op) == FMT_ILLEGAL ? 0 : op) {
    case 0:
        snprintf(out, size, ".word 0x%08x", ins);
        return;
    case OP_LI:
        snprintf(out, size, "%s = %d", a, imm);
        return;
    case OP_LIH:
        snprintf(out, size, "%s = (%s & 0xffff) | 0x%04x0000", a, a, ins & 0xffff);
        return;
    case OP_LD:
    case OP_LD16:
    case OP_LD8:
        format_address(addr, sizeof(addr), b, imm);
        snprintf(out, size, "%s = %s%s", a, sizes[op - OP_LD], addr);
        return;
    case OP_ST:
    case OP_ST16:
    case OP_ST8:
        format_address(addr, sizeof(addr), b, imm);
        snprintf(out, size, "%s%s = %s", sizes[op - OP_ST], addr, a);
        return;
    case OP_BZ:
    case OP_BNZ:
        snprintf(out, size, "if %s %s 0 goto 0x%x", a, op == OP_BZ ? "==" : "!=", target);
        return;
    case OP_JMP:
        snprintf(out, size, "goto 0x%x", (ins & 0xffffff) * 4);
        return;
    case OP_CALL:
        snprintf(out, size, "call 0x%x", (ins & 0xffffff) * 4);
        return;
    case OP_CALLR:
        snprintf(out, size, "call %s", a);
        return;
    case OP_RET:
        snprintf(out, size, "return");
        return;
    case OP_PUSH:
        snprintf(out, size, "push %s", a);
        return;
    case OP_POP:
        snprintf(out, size, "pop %s", a);
        return;
    case OP_COPY:
        snprintf(out, size, "copy %s, %s, %s", a, b, c);
        return;
    case OP_FILL:
        snprintf(out, size, "fill %s, %s, %s", a, b, c);
        return;
    case OP_SYS:
        snprintf(out, size, "sys %u", ins & 0xffff);
        return;
    case OP_HALT:
        snprintf(out, size, "halt");
        return;
    case OP_IN:
        snprintf(out, size, "%s = io %s", a, b);
        return;
    case OP_OUT:
        snprintf(out, size, "io %s = %s", b, a);
        return;
    case OP_RESUME:
        snprintf(out, size, "resume");
        return;
    case OP_WAIT:
        snprintf(out, size, "wait");
        return;
    }

    if (op >= OP_BEQ && op <= OP_BGES) {
        snprintf(out, size, "if %s %s %s goto 0x%x", a, branch_ops[op - OP_BEQ], b, target);
    } else if (op >= OP_ADD && op <= OP_FDIV) {
        snprintf(out, size, "%s = %s %s %s", a, b, alu_ops[op - OP_ADD], c);
    } else if (op == OP_ADD + OP_IMM && imm == 0) {
        snprintf(out, size, "%s = %s", a, b);
    } else {
        snprintf(out, size, "%s = %s %s %d", a, b, alu_ops[op - OP_ADD - OP_IMM], imm);
    }
}
