#ifndef XAIR_X86_DECODE_H
#define XAIR_X86_DECODE_H

#include "xair/xair_frontend.h"

/*
 * Zydis-inspired bootstrap decoder boundary.
 *
 * This private API mirrors the public concept of a decoded instruction with typed operands, but
 * contains original XAIR subset logic only. It does not vendor Zydis source or generated tables.
 * Zydis is MIT licensed by Florian Bernd, Joel Hoener, and contributors.
 */

#define XAIR_X86_DECODE_MAX_OPERANDS 4

typedef enum {
    XAIR_X86_MNEMONIC_INVALID = 0,
    XAIR_X86_MNEMONIC_NOP,
    XAIR_X86_MNEMONIC_MOV,
    XAIR_X86_MNEMONIC_ADD,
    XAIR_X86_MNEMONIC_SUB,
    XAIR_X86_MNEMONIC_AND,
    XAIR_X86_MNEMONIC_OR,
    XAIR_X86_MNEMONIC_XOR,
    XAIR_X86_MNEMONIC_CMP,
    XAIR_X86_MNEMONIC_TEST,
    XAIR_X86_MNEMONIC_LEA,
    XAIR_X86_MNEMONIC_PUSH,
    XAIR_X86_MNEMONIC_POP,
    XAIR_X86_MNEMONIC_JMP,
    XAIR_X86_MNEMONIC_JZ,
    XAIR_X86_MNEMONIC_JNZ,
    XAIR_X86_MNEMONIC_RET
} xair_x86_mnemonic;

typedef enum {
    XAIR_X86_OPERAND_UNUSED = 0,
    XAIR_X86_OPERAND_REGISTER,
    XAIR_X86_OPERAND_MEMORY,
    XAIR_X86_OPERAND_IMMEDIATE
} xair_x86_operand_kind;

typedef enum {
    XAIR_X86_MEM_INVALID = 0,
    XAIR_X86_MEM_RIP_REL,
    XAIR_X86_MEM_BASE_INDEX
} xair_x86_memory_kind;

typedef struct {
    xair_x86_memory_kind kind;
    xair_x86_reg base;
    xair_x86_reg index;
    uint8_t scale;
    uint8_t has_base;
    uint8_t has_index;
    int64_t displacement;
} xair_x86_memory_operand;

typedef struct {
    xair_x86_operand_kind kind;
    uint16_t size_bits;
    union {
        xair_x86_reg reg;
        xair_x86_memory_operand mem;
        int64_t imm;
    } value;
} xair_x86_operand;

typedef struct {
    xair_x86_mnemonic mnemonic;
    uint8_t length;
    uint8_t raw_opcode;
    uint8_t raw_opcode2;
    uint8_t operand_count;
    xair_x86_operand operands[XAIR_X86_DECODE_MAX_OPERANDS];
} xair_x86_decoded_inst;

xair_status xair_x86_decode64(
    const uint8_t *bytes,
    size_t size,
    xair_x86_decoded_inst *out_inst);

#endif
