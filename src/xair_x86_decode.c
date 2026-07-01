#include "xair_x86_decode.h"

#include <string.h>

static int decode_read_u8(const uint8_t *bytes, size_t size, size_t offset, uint8_t *out_value) {
    if (bytes == NULL || out_value == NULL || offset >= size) {
        return 0;
    }
    *out_value = bytes[offset];
    return 1;
}

static int decode_read_le32(const uint8_t *bytes, size_t size, size_t offset, uint32_t *out_value) {
    const uint8_t *p;

    if (bytes == NULL || out_value == NULL || offset > size || size - offset < 4u) {
        return 0;
    }
    p = bytes + offset;
    *out_value = ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8u) |
        ((uint32_t)p[2] << 16u) |
        ((uint32_t)p[3] << 24u);
    return 1;
}

static int decode_read_le64(const uint8_t *bytes, size_t size, size_t offset, uint64_t *out_value) {
    uint32_t lo;
    uint32_t hi;

    if (out_value == NULL || !decode_read_le32(bytes, size, offset, &lo) ||
        !decode_read_le32(bytes, size, offset + 4u, &hi)) {
        return 0;
    }
    *out_value = ((uint64_t)lo) | ((uint64_t)hi << 32u);
    return 1;
}

static int64_t decode_sign_extend8(uint8_t value) {
    return (int64_t)(int8_t)value;
}

static int64_t decode_sign_extend32(uint32_t value) {
    return (int64_t)(int32_t)value;
}

static void decode_set_reg(xair_x86_operand *operand, xair_x86_reg reg) {
    operand->kind = XAIR_X86_OPERAND_REGISTER;
    operand->size_bits = 64;
    operand->value.reg = reg;
}

static void decode_set_rip_mem(xair_x86_operand *operand, int64_t displacement) {
    operand->kind = XAIR_X86_OPERAND_MEMORY;
    operand->size_bits = 64;
    operand->value.mem.kind = XAIR_X86_MEM_RIP_REL;
    operand->value.mem.displacement = displacement;
}

static void decode_set_imm(xair_x86_operand *operand, uint16_t bits, int64_t value) {
    operand->kind = XAIR_X86_OPERAND_IMMEDIATE;
    operand->size_bits = bits;
    operand->value.imm = value;
}

static void decode_modrm(uint8_t byte, uint8_t *mod, uint8_t *reg, uint8_t *rm) {
    *mod = (uint8_t)(byte >> 6u);
    *reg = (uint8_t)((byte >> 3u) & 7u);
    *rm = (uint8_t)(byte & 7u);
}

static xair_x86_reg decode_reg(uint8_t field, uint8_t rex_bit) {
    return (xair_x86_reg)(field + (rex_bit != 0 ? 8u : 0u));
}

static xair_status decode_unsupported(
    xair_x86_decoded_inst *inst,
    uint8_t opcode,
    uint8_t opcode2) {
    inst->mnemonic = XAIR_X86_MNEMONIC_INVALID;
    inst->raw_opcode = opcode;
    inst->raw_opcode2 = opcode2;
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status decode_modrm_move(
    const uint8_t *bytes,
    size_t size,
    uint8_t opcode,
    uint8_t rex,
    size_t offset,
    xair_x86_decoded_inst *inst) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_reg reg;
    xair_x86_reg rm;
    uint32_t disp;

    if ((rex & 8u) == 0) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (!decode_read_u8(bytes, size, offset, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++offset;
    decode_modrm(modrm, &mod, &reg_field, &rm_field);
    reg = decode_reg(reg_field, (uint8_t)(rex & 4u));
    rm = decode_reg(rm_field, (uint8_t)(rex & 1u));

    inst->mnemonic = XAIR_X86_MNEMONIC_MOV;
    inst->operand_count = 2;
    if (mod == 3u) {
        if (opcode == 0x8bu) {
            decode_set_reg(&inst->operands[0], reg);
            decode_set_reg(&inst->operands[1], rm);
        } else {
            decode_set_reg(&inst->operands[0], rm);
            decode_set_reg(&inst->operands[1], reg);
        }
        inst->length = (uint8_t)offset;
        return XAIR_OK;
    }
    if (mod == 0u && rm_field == 5u && (rex & 1u) == 0) {
        if (!decode_read_le32(bytes, size, offset, &disp)) {
            return XAIR_ERR_RANGE;
        }
        offset += 4u;
        if (opcode == 0x8bu) {
            decode_set_reg(&inst->operands[0], reg);
            decode_set_rip_mem(&inst->operands[1], decode_sign_extend32(disp));
        } else {
            decode_set_rip_mem(&inst->operands[0], decode_sign_extend32(disp));
            decode_set_reg(&inst->operands[1], reg);
        }
        inst->length = (uint8_t)offset;
        return XAIR_OK;
    }
    return decode_unsupported(inst, opcode, 0);
}

static xair_status decode_modrm_alu(
    const uint8_t *bytes,
    size_t size,
    uint8_t opcode,
    uint8_t rex,
    size_t offset,
    xair_x86_decoded_inst *inst) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_reg reg;
    xair_x86_reg rm;

    if ((rex & 8u) == 0) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (!decode_read_u8(bytes, size, offset, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++offset;
    decode_modrm(modrm, &mod, &reg_field, &rm_field);
    if (mod != 3u) {
        return decode_unsupported(inst, opcode, 0);
    }
    reg = decode_reg(reg_field, (uint8_t)(rex & 4u));
    rm = decode_reg(rm_field, (uint8_t)(rex & 1u));

    inst->operand_count = 2;
    switch (opcode) {
    case 0x01u:
        inst->mnemonic = XAIR_X86_MNEMONIC_ADD;
        decode_set_reg(&inst->operands[0], rm);
        decode_set_reg(&inst->operands[1], reg);
        break;
    case 0x03u:
        inst->mnemonic = XAIR_X86_MNEMONIC_ADD;
        decode_set_reg(&inst->operands[0], reg);
        decode_set_reg(&inst->operands[1], rm);
        break;
    case 0x29u:
        inst->mnemonic = XAIR_X86_MNEMONIC_SUB;
        decode_set_reg(&inst->operands[0], rm);
        decode_set_reg(&inst->operands[1], reg);
        break;
    case 0x2bu:
        inst->mnemonic = XAIR_X86_MNEMONIC_SUB;
        decode_set_reg(&inst->operands[0], reg);
        decode_set_reg(&inst->operands[1], rm);
        break;
    case 0x39u:
        inst->mnemonic = XAIR_X86_MNEMONIC_CMP;
        decode_set_reg(&inst->operands[0], rm);
        decode_set_reg(&inst->operands[1], reg);
        break;
    case 0x3bu:
        inst->mnemonic = XAIR_X86_MNEMONIC_CMP;
        decode_set_reg(&inst->operands[0], reg);
        decode_set_reg(&inst->operands[1], rm);
        break;
    case 0x85u:
        inst->mnemonic = XAIR_X86_MNEMONIC_TEST;
        decode_set_reg(&inst->operands[0], rm);
        decode_set_reg(&inst->operands[1], reg);
        break;
    default:
        return decode_unsupported(inst, opcode, 0);
    }
    inst->length = (uint8_t)offset;
    return XAIR_OK;
}

xair_status xair_x86_decode64(
    const uint8_t *bytes,
    size_t size,
    xair_x86_decoded_inst *out_inst) {
    xair_x86_decoded_inst inst;
    size_t offset = 0;
    uint8_t rex = 0;
    uint8_t opcode;

    if (bytes == NULL || out_inst == NULL || size == 0) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(&inst, 0, sizeof(inst));
    if (!decode_read_u8(bytes, size, offset, &opcode)) {
        return XAIR_ERR_RANGE;
    }
    while (opcode >= 0x40u && opcode <= 0x4fu) {
        rex = opcode;
        ++offset;
        if (!decode_read_u8(bytes, size, offset, &opcode)) {
            return XAIR_ERR_RANGE;
        }
    }
    ++offset;
    inst.raw_opcode = opcode;

    if (opcode >= 0xb8u && opcode <= 0xbfu) {
        uint64_t imm;
        xair_x86_reg dst;

        if ((rex & 8u) == 0) {
            *out_inst = inst;
            return decode_unsupported(out_inst, opcode, 0);
        }
        if (!decode_read_le64(bytes, size, offset, &imm)) {
            return XAIR_ERR_RANGE;
        }
        offset += 8u;
        dst = decode_reg((uint8_t)(opcode - 0xb8u), (uint8_t)(rex & 1u));
        inst.mnemonic = XAIR_X86_MNEMONIC_MOV;
        inst.operand_count = 2;
        inst.length = (uint8_t)offset;
        decode_set_reg(&inst.operands[0], dst);
        decode_set_imm(&inst.operands[1], 64, (int64_t)imm);
        *out_inst = inst;
        return XAIR_OK;
    }

    switch (opcode) {
    case 0x8bu:
    case 0x89u:
        *out_inst = inst;
        return decode_modrm_move(bytes, size, opcode, rex, offset, out_inst);
    case 0x01u:
    case 0x03u:
    case 0x29u:
    case 0x2bu:
    case 0x39u:
    case 0x3bu:
    case 0x85u:
        *out_inst = inst;
        return decode_modrm_alu(bytes, size, opcode, rex, offset, out_inst);
    case 0xebu: {
        uint8_t rel;

        if (!decode_read_u8(bytes, size, offset, &rel)) {
            return XAIR_ERR_RANGE;
        }
        ++offset;
        inst.mnemonic = XAIR_X86_MNEMONIC_JMP;
        inst.operand_count = 1;
        inst.length = (uint8_t)offset;
        decode_set_imm(&inst.operands[0], 8, decode_sign_extend8(rel));
        *out_inst = inst;
        return XAIR_OK;
    }
    case 0xe9u: {
        uint32_t rel;

        if (!decode_read_le32(bytes, size, offset, &rel)) {
            return XAIR_ERR_RANGE;
        }
        offset += 4u;
        inst.mnemonic = XAIR_X86_MNEMONIC_JMP;
        inst.operand_count = 1;
        inst.length = (uint8_t)offset;
        decode_set_imm(&inst.operands[0], 32, decode_sign_extend32(rel));
        *out_inst = inst;
        return XAIR_OK;
    }
    case 0x74u:
    case 0x75u: {
        uint8_t rel;

        if (!decode_read_u8(bytes, size, offset, &rel)) {
            return XAIR_ERR_RANGE;
        }
        ++offset;
        inst.mnemonic = opcode == 0x74u ? XAIR_X86_MNEMONIC_JZ : XAIR_X86_MNEMONIC_JNZ;
        inst.operand_count = 1;
        inst.length = (uint8_t)offset;
        decode_set_imm(&inst.operands[0], 8, decode_sign_extend8(rel));
        *out_inst = inst;
        return XAIR_OK;
    }
    case 0x0fu: {
        uint8_t opcode2;

        if (!decode_read_u8(bytes, size, offset, &opcode2)) {
            return XAIR_ERR_RANGE;
        }
        ++offset;
        inst.raw_opcode2 = opcode2;
        if (opcode2 == 0x84u || opcode2 == 0x85u) {
            uint32_t rel;

            if (!decode_read_le32(bytes, size, offset, &rel)) {
                return XAIR_ERR_RANGE;
            }
            offset += 4u;
            inst.mnemonic = opcode2 == 0x84u ? XAIR_X86_MNEMONIC_JZ : XAIR_X86_MNEMONIC_JNZ;
            inst.operand_count = 1;
            inst.length = (uint8_t)offset;
            decode_set_imm(&inst.operands[0], 32, decode_sign_extend32(rel));
            *out_inst = inst;
            return XAIR_OK;
        }
        *out_inst = inst;
        return decode_unsupported(out_inst, opcode, opcode2);
    }
    case 0xc3u:
        inst.mnemonic = XAIR_X86_MNEMONIC_RET;
        inst.length = (uint8_t)offset;
        *out_inst = inst;
        return XAIR_OK;
    default:
        *out_inst = inst;
        return decode_unsupported(out_inst, opcode, 0);
    }
}
