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

static xair_x86_reg decode_reg(uint8_t field, uint8_t rex_bit, uint16_t mode_bits) {
    return (xair_x86_reg)(field + (mode_bits == 64 && rex_bit != 0 ? 8u : 0u));
}

static void decode_modrm(uint8_t byte, uint8_t *mod, uint8_t *reg, uint8_t *rm) {
    *mod = (uint8_t)(byte >> 6u);
    *reg = (uint8_t)((byte >> 3u) & 7u);
    *rm = (uint8_t)(byte & 7u);
}

static void decode_set_reg(xair_x86_operand *operand, xair_x86_reg reg, uint16_t bits) {
    memset(operand, 0, sizeof(*operand));
    operand->kind = XAIR_X86_OPERAND_REGISTER;
    operand->size_bits = bits;
    operand->value.reg = reg;
}

static void decode_set_imm(xair_x86_operand *operand, uint16_t bits, int64_t value) {
    memset(operand, 0, sizeof(*operand));
    operand->kind = XAIR_X86_OPERAND_IMMEDIATE;
    operand->size_bits = bits;
    operand->value.imm = value;
}

static void decode_set_rip_mem(xair_x86_operand *operand, uint16_t bits, int64_t displacement) {
    memset(operand, 0, sizeof(*operand));
    operand->kind = XAIR_X86_OPERAND_MEMORY;
    operand->size_bits = bits;
    operand->value.mem.kind = XAIR_X86_MEM_RIP_REL;
    operand->value.mem.scale = 1;
    operand->value.mem.displacement = displacement;
}

static void decode_set_base_index_mem(
    xair_x86_operand *operand,
    int has_base,
    xair_x86_reg base,
    int has_index,
    xair_x86_reg index,
    uint8_t scale,
    uint16_t bits,
    int64_t displacement) {
    memset(operand, 0, sizeof(*operand));
    operand->kind = XAIR_X86_OPERAND_MEMORY;
    operand->size_bits = bits;
    operand->value.mem.kind = XAIR_X86_MEM_BASE_INDEX;
    operand->value.mem.has_base = (uint8_t)(has_base != 0);
    operand->value.mem.base = base;
    operand->value.mem.has_index = (uint8_t)(has_index != 0);
    operand->value.mem.index = index;
    operand->value.mem.scale = scale == 0 ? 1u : scale;
    operand->value.mem.displacement = displacement;
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

static xair_status decode_displacement(
    const uint8_t *bytes,
    size_t size,
    uint8_t mod,
    size_t *offset,
    int64_t *out_disp) {
    uint8_t disp8;
    uint32_t disp32;

    *out_disp = 0;
    if (mod == 1u) {
        if (!decode_read_u8(bytes, size, *offset, &disp8)) {
            return XAIR_ERR_RANGE;
        }
        ++*offset;
        *out_disp = decode_sign_extend8(disp8);
    } else if (mod == 2u) {
        if (!decode_read_le32(bytes, size, *offset, &disp32)) {
            return XAIR_ERR_RANGE;
        }
        *offset += 4u;
        *out_disp = decode_sign_extend32(disp32);
    }
    return XAIR_OK;
}

static xair_status decode_memory_operand(
    const uint8_t *bytes,
    size_t size,
    uint8_t mod,
    uint8_t rm_field,
    uint8_t rex,
    uint16_t mode_bits,
    uint16_t operand_bits,
    size_t *offset,
    xair_x86_operand *out_operand) {
    int64_t displacement;
    uint32_t disp32;

    if (mod == 3u) {
        return XAIR_ERR_BAD_ARG;
    }

    if (rm_field == 4u) {
        uint8_t sib;
        uint8_t scale_bits;
        uint8_t index_field;
        uint8_t base_field;
        int has_base = 1;
        int has_index = 1;
        xair_x86_reg base = XAIR_X86_REG_COUNT;
        xair_x86_reg index = XAIR_X86_REG_COUNT;
        uint8_t scale;

        if (!decode_read_u8(bytes, size, *offset, &sib)) {
            return XAIR_ERR_RANGE;
        }
        ++*offset;
        scale_bits = (uint8_t)(sib >> 6u);
        index_field = (uint8_t)((sib >> 3u) & 7u);
        base_field = (uint8_t)(sib & 7u);
        scale = (uint8_t)(1u << scale_bits);

        if (index_field == 4u && (rex & 2u) == 0) {
            has_index = 0;
        } else {
            index = decode_reg(index_field, (uint8_t)(rex & 2u), mode_bits);
        }

        if (mod == 0u && base_field == 5u) {
            has_base = 0;
            if (!decode_read_le32(bytes, size, *offset, &disp32)) {
                return XAIR_ERR_RANGE;
            }
            *offset += 4u;
            displacement = decode_sign_extend32(disp32);
        } else {
            base = decode_reg(base_field, (uint8_t)(rex & 1u), mode_bits);
            if (decode_displacement(bytes, size, mod, offset, &displacement) != XAIR_OK) {
                return XAIR_ERR_RANGE;
            }
        }
        decode_set_base_index_mem(
            out_operand,
            has_base,
            base,
            has_index,
            index,
            scale,
            operand_bits,
            displacement);
        return XAIR_OK;
    }

    if (mod == 0u && rm_field == 5u) {
        if (!decode_read_le32(bytes, size, *offset, &disp32)) {
            return XAIR_ERR_RANGE;
        }
        *offset += 4u;
        if (mode_bits == 64u) {
            decode_set_rip_mem(out_operand, operand_bits, decode_sign_extend32(disp32));
        } else {
            decode_set_base_index_mem(
                out_operand,
                0,
                XAIR_X86_REG_COUNT,
                0,
                XAIR_X86_REG_COUNT,
                1,
                operand_bits,
                decode_sign_extend32(disp32));
        }
        return XAIR_OK;
    }

    if (decode_displacement(bytes, size, mod, offset, &displacement) != XAIR_OK) {
        return XAIR_ERR_RANGE;
    }
    decode_set_base_index_mem(
        out_operand,
        1,
        decode_reg(rm_field, (uint8_t)(rex & 1u), mode_bits),
        0,
        XAIR_X86_REG_COUNT,
        1,
        operand_bits,
        displacement);
    return XAIR_OK;
}

static xair_status decode_rm_operand(
    const uint8_t *bytes,
    size_t size,
    uint8_t mod,
    uint8_t rm_field,
    uint8_t rex,
    uint16_t mode_bits,
    uint16_t operand_bits,
    size_t *offset,
    xair_x86_operand *out_operand) {
    if (mod == 3u) {
        decode_set_reg(out_operand, decode_reg(rm_field, (uint8_t)(rex & 1u), mode_bits), operand_bits);
        return XAIR_OK;
    }
    return decode_memory_operand(bytes, size, mod, rm_field, rex, mode_bits, operand_bits, offset, out_operand);
}

static xair_status decode_modrm_move_or_lea(
    const uint8_t *bytes,
    size_t size,
    uint8_t opcode,
    uint8_t rex,
    uint16_t mode_bits,
    size_t offset,
    xair_x86_decoded_inst *inst) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_operand rm_operand;

    if (mode_bits == 64u && (rex & 8u) == 0) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (!decode_read_u8(bytes, size, offset, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++offset;
    decode_modrm(modrm, &mod, &reg_field, &rm_field);

    if (opcode == 0x8du) {
        if (mod == 3u) {
            return decode_unsupported(inst, opcode, 0);
        }
        if (decode_memory_operand(bytes, size, mod, rm_field, rex, mode_bits, mode_bits, &offset, &rm_operand) != XAIR_OK) {
            return XAIR_ERR_RANGE;
        }
        inst->mnemonic = XAIR_X86_MNEMONIC_LEA;
        inst->operand_count = 2;
        decode_set_reg(&inst->operands[0], decode_reg(reg_field, (uint8_t)(rex & 4u), mode_bits), mode_bits);
        inst->operands[1] = rm_operand;
        inst->length = (uint8_t)offset;
        return XAIR_OK;
    }

    if (decode_rm_operand(bytes, size, mod, rm_field, rex, mode_bits, mode_bits, &offset, &rm_operand) != XAIR_OK) {
        return XAIR_ERR_RANGE;
    }
    inst->mnemonic = XAIR_X86_MNEMONIC_MOV;
    inst->operand_count = 2;
    if (opcode == 0x8bu) {
        decode_set_reg(&inst->operands[0], decode_reg(reg_field, (uint8_t)(rex & 4u), mode_bits), mode_bits);
        inst->operands[1] = rm_operand;
    } else {
        inst->operands[0] = rm_operand;
        decode_set_reg(&inst->operands[1], decode_reg(reg_field, (uint8_t)(rex & 4u), mode_bits), mode_bits);
    }
    inst->length = (uint8_t)offset;
    return XAIR_OK;
}

static xair_x86_mnemonic decode_binary_mnemonic(uint8_t opcode) {
    switch (opcode) {
    case 0x01u:
    case 0x03u:
        return XAIR_X86_MNEMONIC_ADD;
    case 0x09u:
    case 0x0bu:
        return XAIR_X86_MNEMONIC_OR;
    case 0x21u:
    case 0x23u:
        return XAIR_X86_MNEMONIC_AND;
    case 0x29u:
    case 0x2bu:
        return XAIR_X86_MNEMONIC_SUB;
    case 0x31u:
    case 0x33u:
        return XAIR_X86_MNEMONIC_XOR;
    case 0x39u:
    case 0x3bu:
        return XAIR_X86_MNEMONIC_CMP;
    case 0x85u:
        return XAIR_X86_MNEMONIC_TEST;
    default:
        return XAIR_X86_MNEMONIC_INVALID;
    }
}

static int decode_reg_dest_opcode(uint8_t opcode) {
    switch (opcode) {
    case 0x03u:
    case 0x0bu:
    case 0x23u:
    case 0x2bu:
    case 0x33u:
    case 0x3bu:
        return 1;
    default:
        return 0;
    }
}

static xair_status decode_modrm_binary(
    const uint8_t *bytes,
    size_t size,
    uint8_t opcode,
    uint8_t rex,
    uint16_t mode_bits,
    size_t offset,
    xair_x86_decoded_inst *inst) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_operand rm_operand;
    xair_x86_operand reg_operand;

    if (mode_bits == 64u && (rex & 8u) == 0) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (!decode_read_u8(bytes, size, offset, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++offset;
    decode_modrm(modrm, &mod, &reg_field, &rm_field);
    if (decode_rm_operand(bytes, size, mod, rm_field, rex, mode_bits, mode_bits, &offset, &rm_operand) != XAIR_OK) {
        return XAIR_ERR_RANGE;
    }
    if (rm_operand.kind != XAIR_X86_OPERAND_REGISTER) {
        return decode_unsupported(inst, opcode, 0);
    }
    decode_set_reg(&reg_operand, decode_reg(reg_field, (uint8_t)(rex & 4u), mode_bits), mode_bits);

    inst->mnemonic = decode_binary_mnemonic(opcode);
    if (inst->mnemonic == XAIR_X86_MNEMONIC_INVALID) {
        return decode_unsupported(inst, opcode, 0);
    }
    inst->operand_count = 2;
    if (decode_reg_dest_opcode(opcode)) {
        inst->operands[0] = reg_operand;
        inst->operands[1] = rm_operand;
    } else {
        inst->operands[0] = rm_operand;
        inst->operands[1] = reg_operand;
    }
    inst->length = (uint8_t)offset;
    return XAIR_OK;
}

static xair_x86_mnemonic decode_group1_mnemonic(uint8_t group) {
    switch (group) {
    case 0u: return XAIR_X86_MNEMONIC_ADD;
    case 1u: return XAIR_X86_MNEMONIC_OR;
    case 4u: return XAIR_X86_MNEMONIC_AND;
    case 5u: return XAIR_X86_MNEMONIC_SUB;
    case 6u: return XAIR_X86_MNEMONIC_XOR;
    case 7u: return XAIR_X86_MNEMONIC_CMP;
    default: return XAIR_X86_MNEMONIC_INVALID;
    }
}

static xair_status decode_group1_imm(
    const uint8_t *bytes,
    size_t size,
    uint8_t opcode,
    uint8_t rex,
    uint16_t mode_bits,
    size_t offset,
    xair_x86_decoded_inst *inst) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t group;
    uint8_t rm_field;
    xair_x86_operand rm_operand;
    int64_t imm;

    if (mode_bits == 64u && (rex & 8u) == 0) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (!decode_read_u8(bytes, size, offset, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++offset;
    decode_modrm(modrm, &mod, &group, &rm_field);
    if (decode_rm_operand(bytes, size, mod, rm_field, rex, mode_bits, mode_bits, &offset, &rm_operand) != XAIR_OK) {
        return XAIR_ERR_RANGE;
    }
    if (rm_operand.kind != XAIR_X86_OPERAND_REGISTER) {
        return decode_unsupported(inst, opcode, 0);
    }
    if (opcode == 0x83u) {
        uint8_t raw_imm;

        if (!decode_read_u8(bytes, size, offset, &raw_imm)) {
            return XAIR_ERR_RANGE;
        }
        ++offset;
        imm = decode_sign_extend8(raw_imm);
    } else {
        uint32_t raw_imm;

        if (!decode_read_le32(bytes, size, offset, &raw_imm)) {
            return XAIR_ERR_RANGE;
        }
        offset += 4u;
        imm = decode_sign_extend32(raw_imm);
    }

    inst->mnemonic = decode_group1_mnemonic(group);
    if (inst->mnemonic == XAIR_X86_MNEMONIC_INVALID) {
        return decode_unsupported(inst, opcode, 0);
    }
    inst->operand_count = 2;
    inst->operands[0] = rm_operand;
    decode_set_imm(&inst->operands[1], opcode == 0x83u ? 8 : 32, imm);
    inst->length = (uint8_t)offset;
    return XAIR_OK;
}

static xair_status xair_x86_decode_mode(
    const uint8_t *bytes,
    size_t size,
    uint16_t mode_bits,
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
    while (mode_bits == 64u && opcode >= 0x40u && opcode <= 0x4fu) {
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

        if (mode_bits == 64u && (rex & 8u) == 0) {
            *out_inst = inst;
            return decode_unsupported(out_inst, opcode, 0);
        }
        if (mode_bits == 64u) {
            if (!decode_read_le64(bytes, size, offset, &imm)) {
                return XAIR_ERR_RANGE;
            }
            offset += 8u;
        } else {
            uint32_t imm32;

            if (!decode_read_le32(bytes, size, offset, &imm32)) {
                return XAIR_ERR_RANGE;
            }
            offset += 4u;
            imm = (uint64_t)imm32;
        }
        dst = decode_reg((uint8_t)(opcode - 0xb8u), (uint8_t)(rex & 1u), mode_bits);
        inst.mnemonic = XAIR_X86_MNEMONIC_MOV;
        inst.operand_count = 2;
        inst.length = (uint8_t)offset;
        decode_set_reg(&inst.operands[0], dst, mode_bits);
        decode_set_imm(&inst.operands[1], mode_bits, (int64_t)imm);
        *out_inst = inst;
        return XAIR_OK;
    }

    if ((opcode >= 0x50u && opcode <= 0x57u) || (opcode >= 0x58u && opcode <= 0x5fu)) {
        inst.mnemonic = opcode < 0x58u ? XAIR_X86_MNEMONIC_PUSH : XAIR_X86_MNEMONIC_POP;
        inst.operand_count = 1;
        inst.length = (uint8_t)offset;
        decode_set_reg(&inst.operands[0], decode_reg((uint8_t)(opcode & 7u), (uint8_t)(rex & 1u), mode_bits), mode_bits);
        *out_inst = inst;
        return XAIR_OK;
    }

    switch (opcode) {
    case 0x90u:
        inst.mnemonic = XAIR_X86_MNEMONIC_NOP;
        inst.length = (uint8_t)offset;
        *out_inst = inst;
        return XAIR_OK;
    case 0x8bu:
    case 0x89u:
    case 0x8du:
        *out_inst = inst;
        return decode_modrm_move_or_lea(bytes, size, opcode, rex, mode_bits, offset, out_inst);
    case 0x01u:
    case 0x03u:
    case 0x09u:
    case 0x0bu:
    case 0x21u:
    case 0x23u:
    case 0x29u:
    case 0x2bu:
    case 0x31u:
    case 0x33u:
    case 0x39u:
    case 0x3bu:
    case 0x85u:
        *out_inst = inst;
        return decode_modrm_binary(bytes, size, opcode, rex, mode_bits, offset, out_inst);
    case 0x81u:
    case 0x83u:
        *out_inst = inst;
        return decode_group1_imm(bytes, size, opcode, rex, mode_bits, offset, out_inst);
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

xair_status xair_x86_decode32(
    const uint8_t *bytes,
    size_t size,
    xair_x86_decoded_inst *out_inst) {
    return xair_x86_decode_mode(bytes, size, 32, out_inst);
}

xair_status xair_x86_decode64(
    const uint8_t *bytes,
    size_t size,
    xair_x86_decoded_inst *out_inst) {
    return xair_x86_decode_mode(bytes, size, 64, out_inst);
}

static const xair_x86_decoder_backend xair_x86_stub_decoder_backend = {
    "x86_stub",
    xair_x86_decode32,
    xair_x86_decode64
};

const xair_x86_decoder_backend *xair_x86_stub_decoder(void) {
    return &xair_x86_stub_decoder_backend;
}
