#include "xair/xair_frontend.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    xair_module *module;
    const xair_image *image;
    xair_lift_result *result;
    xair_block_id block;
    uint64_t pc;
    uint16_t memory_space;
    size_t instructions;
    xair_value_id regs[XAIR_X86_REG_COUNT];
    uint8_t written_regs[XAIR_X86_REG_COUNT];
    xair_value_id memory;
    int has_memory;
    xair_value_id zf;
    int has_zf;
} x86_lift_state;

static const char *const x86_reg_names[XAIR_X86_REG_COUNT] = {
    "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
    "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15"
};

static void lift_result_init(xair_lift_result *result) {
    size_t i;

    memset(result, 0, sizeof(*result));
    result->block = XAIR_INVALID_ID;
    result->memory_in = XAIR_INVALID_ID;
    result->memory_out = XAIR_INVALID_ID;
    result->branch_condition = XAIR_INVALID_ID;
    for (i = 0; i < XAIR_LIFT_MAX_REG_OUTPUTS; ++i) {
        result->output_regs[i].value = XAIR_INVALID_ID;
    }
    for (i = 0; i < XAIR_LIFT_MAX_RETURNS; ++i) {
        result->return_values[i] = XAIR_INVALID_ID;
    }
}

xair_status xair_image_init(
    xair_image *image,
    const uint8_t *bytes,
    size_t size,
    uint64_t base) {
    if (image == NULL || (bytes == NULL && size != 0)) {
        return XAIR_ERR_BAD_ARG;
    }
    image->bytes = bytes;
    image->size = size;
    image->base = base;
    return XAIR_OK;
}

const char *xair_arch_name(xair_arch arch) {
    switch (arch) {
    case XAIR_ARCH_X86_64:
        return "x86_64";
    default:
        return "unknown";
    }
}

const char *xair_lift_end_kind_name(xair_lift_end_kind kind) {
    switch (kind) {
    case XAIR_LIFT_END_FALLTHROUGH:
        return "fallthrough";
    case XAIR_LIFT_END_DIRECT_JUMP:
        return "direct_jump";
    case XAIR_LIFT_END_DIRECT_CBRANCH:
        return "direct_cbranch";
    case XAIR_LIFT_END_RETURN:
        return "return";
    case XAIR_LIFT_END_UNSUPPORTED:
        return "unsupported";
    default:
        return "unknown";
    }
}

const char *xair_x86_reg_name(xair_x86_reg reg) {
    if ((unsigned)reg >= (unsigned)XAIR_X86_REG_COUNT) {
        return "invalid";
    }
    return x86_reg_names[reg];
}

static int image_offset(const xair_image *image, uint64_t address, size_t *out_offset) {
    uint64_t delta;

    if (image == NULL || out_offset == NULL || address < image->base) {
        return 0;
    }
    delta = address - image->base;
    if (delta > (uint64_t)SIZE_MAX || (size_t)delta >= image->size) {
        return 0;
    }
    *out_offset = (size_t)delta;
    return 1;
}

static int image_read_u8(const xair_image *image, uint64_t address, uint8_t *out_value) {
    size_t offset;

    if (out_value == NULL || !image_offset(image, address, &offset)) {
        return 0;
    }
    *out_value = image->bytes[offset];
    return 1;
}

static int image_read_le32(const xair_image *image, uint64_t address, uint32_t *out_value) {
    size_t offset;
    const uint8_t *p;

    if (out_value == NULL || !image_offset(image, address, &offset) || image->size - offset < 4u) {
        return 0;
    }
    p = image->bytes + offset;
    *out_value = ((uint32_t)p[0]) |
        ((uint32_t)p[1] << 8u) |
        ((uint32_t)p[2] << 16u) |
        ((uint32_t)p[3] << 24u);
    return 1;
}

static int image_read_le64(const xair_image *image, uint64_t address, uint64_t *out_value) {
    uint32_t lo;
    uint32_t hi;

    if (out_value == NULL || !image_read_le32(image, address, &lo) ||
        !image_read_le32(image, address + 4u, &hi)) {
        return 0;
    }
    *out_value = ((uint64_t)lo) | ((uint64_t)hi << 32u);
    return 1;
}

static int64_t sign_extend8(uint8_t value) {
    return (int64_t)(int8_t)value;
}

static int64_t sign_extend32(uint32_t value) {
    return (int64_t)(int32_t)value;
}

static xair_status x86_ensure_reg(
    x86_lift_state *state,
    xair_x86_reg reg,
    xair_value_id *out_value) {
    xair_status status;

    if ((unsigned)reg >= (unsigned)XAIR_X86_REG_COUNT || out_value == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (state->regs[reg] != XAIR_INVALID_ID) {
        *out_value = state->regs[reg];
        return XAIR_OK;
    }
    status = xair_block_add_param(
        state->module,
        state->block,
        xair_type_i(64),
        xair_x86_reg_name(reg),
        &state->regs[reg]);
    if (status != XAIR_OK) {
        return status;
    }
    *out_value = state->regs[reg];
    return XAIR_OK;
}

static xair_status x86_ensure_memory(x86_lift_state *state, xair_value_id *out_value) {
    xair_status status;

    if (out_value == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (state->has_memory) {
        *out_value = state->memory;
        return XAIR_OK;
    }
    status = xair_block_add_param(
        state->module,
        state->block,
        xair_type_mem(state->memory_space, 64),
        "mem",
        &state->memory);
    if (status != XAIR_OK) {
        return status;
    }
    state->has_memory = 1;
    state->result->memory_in = state->memory;
    *out_value = state->memory;
    return XAIR_OK;
}

static void x86_write_reg(x86_lift_state *state, xair_x86_reg reg, xair_value_id value) {
    state->regs[reg] = value;
    state->written_regs[reg] = 1;
}

static xair_status x86_build_const_u64(
    x86_lift_state *state,
    xair_type type,
    uint64_t value,
    const char *name,
    xair_value_id *out_value) {
    return xair_build_const_u64(state->module, state->block, type, value, name, out_value);
}

static xair_status x86_set_zf_from_flags(x86_lift_state *state, xair_value_id flags) {
    xair_status status;

    status = xair_build_unary(
        state->module,
        state->block,
        XAIR_OP_FLAG_ZF,
        xair_type_i(1),
        flags,
        "zf",
        &state->zf);
    if (status != XAIR_OK) {
        return status;
    }
    state->has_zf = 1;
    return XAIR_OK;
}

static xair_status x86_lift_add_sub(
    x86_lift_state *state,
    xair_opcode value_opcode,
    xair_opcode flags_opcode,
    xair_x86_reg dst,
    xair_x86_reg src) {
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id value;
    xair_value_id flags;
    xair_status status;

    status = x86_ensure_reg(state, dst, &lhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_reg(state, src, &rhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        value_opcode,
        xair_type_i(64),
        lhs,
        rhs,
        value_opcode == XAIR_OP_ADD ? "add" : "sub",
        &value);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        flags_opcode,
        xair_type_flags(6),
        lhs,
        rhs,
        "flags",
        &flags);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_set_zf_from_flags(state, flags);
    if (status != XAIR_OK) {
        return status;
    }
    x86_write_reg(state, dst, value);
    return XAIR_OK;
}

static xair_status x86_lift_cmp(x86_lift_state *state, xair_x86_reg lhs_reg, xair_x86_reg rhs_reg) {
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id flags;
    xair_status status;

    status = x86_ensure_reg(state, lhs_reg, &lhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_reg(state, rhs_reg, &rhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_FLAGS_SUB,
        xair_type_flags(6),
        lhs,
        rhs,
        "cmp_flags",
        &flags);
    if (status != XAIR_OK) {
        return status;
    }
    return x86_set_zf_from_flags(state, flags);
}

static xair_status x86_lift_test(x86_lift_state *state, xair_x86_reg lhs_reg, xair_x86_reg rhs_reg) {
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id and_value;
    xair_value_id zero;
    xair_status status;

    status = x86_ensure_reg(state, lhs_reg, &lhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_reg(state, rhs_reg, &rhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_AND,
        xair_type_i(64),
        lhs,
        rhs,
        "test",
        &and_value);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_build_const_u64(state, xair_type_i(64), 0, "zero", &zero);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_EQ,
        xair_type_i(1),
        and_value,
        zero,
        "zf",
        &state->zf);
    if (status != XAIR_OK) {
        return status;
    }
    state->has_zf = 1;
    return XAIR_OK;
}

static xair_status x86_lift_rip_load(
    x86_lift_state *state,
    xair_x86_reg dst,
    uint64_t address) {
    xair_value_id memory;
    xair_value_id addr_value;
    xair_value_id loaded;
    xair_status status;

    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_build_const_u64(state, xair_type_addr(64), address, "addr", &addr_value);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_load(
        state->module,
        state->block,
        xair_type_i(64),
        memory,
        addr_value,
        XAIR_ENDIAN_LE,
        "load",
        &loaded);
    if (status != XAIR_OK) {
        return status;
    }
    x86_write_reg(state, dst, loaded);
    return XAIR_OK;
}

static xair_status x86_lift_rip_store(
    x86_lift_state *state,
    xair_x86_reg src,
    uint64_t address) {
    xair_value_id memory;
    xair_value_id addr_value;
    xair_value_id data;
    xair_value_id next_memory;
    xair_status status;

    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_reg(state, src, &data);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_build_const_u64(state, xair_type_addr(64), address, "addr", &addr_value);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_store(
        state->module,
        state->block,
        memory,
        addr_value,
        data,
        XAIR_ENDIAN_LE,
        "store",
        &next_memory);
    if (status != XAIR_OK) {
        return status;
    }
    state->memory = next_memory;
    state->result->memory_out = next_memory;
    return XAIR_OK;
}

static xair_status x86_append_return(x86_lift_state *state, xair_value_id value) {
    if (state->result->return_count >= XAIR_LIFT_MAX_RETURNS) {
        return XAIR_ERR_RANGE;
    }
    state->result->return_values[state->result->return_count++] = value;
    return XAIR_OK;
}

static xair_status x86_finish_block(x86_lift_state *state) {
    size_t i;
    xair_status status;

    state->result->bytes_read = (size_t)(state->pc - state->result->start);
    state->result->next = state->pc;
    state->result->instructions = state->instructions;
    if (state->has_memory) {
        state->result->memory_out = state->memory;
        status = x86_append_return(state, state->memory);
        if (status != XAIR_OK) {
            return status;
        }
    }
    for (i = 0; i < XAIR_X86_REG_COUNT; ++i) {
        if (state->written_regs[i]) {
            if (state->result->output_reg_count >= XAIR_LIFT_MAX_REG_OUTPUTS) {
                return XAIR_ERR_RANGE;
            }
            state->result->output_regs[state->result->output_reg_count].reg = (xair_x86_reg)i;
            state->result->output_regs[state->result->output_reg_count].value = state->regs[i];
            ++state->result->output_reg_count;
            status = x86_append_return(state, state->regs[i]);
            if (status != XAIR_OK) {
                return status;
            }
        }
    }
    if (state->result->end_kind == XAIR_LIFT_END_DIRECT_CBRANCH &&
        state->result->branch_condition != XAIR_INVALID_ID) {
        status = x86_append_return(state, state->result->branch_condition);
        if (status != XAIR_OK) {
            return status;
        }
    }
    return xair_set_return(
        state->module,
        state->block,
        state->result->return_values,
        state->result->return_count);
}

static xair_status x86_finish_unsupported(
    x86_lift_state *state,
    uint64_t address,
    uint8_t opcode) {
    state->result->end_kind = XAIR_LIFT_END_UNSUPPORTED;
    state->result->unsupported_address = address;
    state->result->unsupported_opcode = opcode;
    return x86_finish_block(state);
}

static xair_status x86_finish_fallthrough(x86_lift_state *state) {
    state->result->end_kind = XAIR_LIFT_END_FALLTHROUGH;
    state->result->fallthrough = state->pc;
    return x86_finish_block(state);
}

static xair_status x86_finish_direct_jump(x86_lift_state *state, uint64_t target) {
    state->result->end_kind = XAIR_LIFT_END_DIRECT_JUMP;
    state->result->target = target;
    return x86_finish_block(state);
}

static xair_status x86_finish_return(x86_lift_state *state) {
    state->result->end_kind = XAIR_LIFT_END_RETURN;
    return x86_finish_block(state);
}

static xair_status x86_finish_direct_cbranch(
    x86_lift_state *state,
    uint64_t target,
    uint64_t fallthrough,
    xair_value_id condition) {
    state->result->end_kind = XAIR_LIFT_END_DIRECT_CBRANCH;
    state->result->target = target;
    state->result->fallthrough = fallthrough;
    state->result->branch_condition = condition;
    return x86_finish_block(state);
}

static int x86_decode_modrm(uint8_t byte, uint8_t *mod, uint8_t *reg, uint8_t *rm) {
    if (mod == NULL || reg == NULL || rm == NULL) {
        return 0;
    }
    *mod = (uint8_t)(byte >> 6u);
    *reg = (uint8_t)((byte >> 3u) & 7u);
    *rm = (uint8_t)(byte & 7u);
    return 1;
}

static xair_status x86_lift_modrm_move(
    x86_lift_state *state,
    uint8_t opcode,
    uint8_t rex,
    uint64_t inst_start,
    uint64_t *pc) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_reg reg;
    xair_x86_reg rm;
    xair_status status;

    if ((rex & 8u) == 0) {
        return x86_finish_unsupported(state, inst_start, opcode);
    }
    if (!image_read_u8(state->image, *pc, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++*pc;
    (void)x86_decode_modrm(modrm, &mod, &reg_field, &rm_field);
    reg = (xair_x86_reg)(reg_field + ((rex & 4u) != 0 ? 8u : 0u));
    rm = (xair_x86_reg)(rm_field + ((rex & 1u) != 0 ? 8u : 0u));

    if (mod == 3u) {
        xair_value_id src;

        if (opcode == 0x8bu) {
            status = x86_ensure_reg(state, rm, &src);
            if (status != XAIR_OK) {
                return status;
            }
            x86_write_reg(state, reg, src);
        } else {
            status = x86_ensure_reg(state, reg, &src);
            if (status != XAIR_OK) {
                return status;
            }
            x86_write_reg(state, rm, src);
        }
        state->pc = *pc;
        ++state->instructions;
        return XAIR_OK;
    }

    if (mod == 0u && rm_field == 5u && (rex & 1u) == 0) {
        uint32_t raw_disp;
        uint64_t next_pc;
        uint64_t address;

        if (!image_read_le32(state->image, *pc, &raw_disp)) {
            return XAIR_ERR_RANGE;
        }
        *pc += 4u;
        next_pc = *pc;
        address = (uint64_t)((int64_t)next_pc + sign_extend32(raw_disp));
        if (opcode == 0x8bu) {
            status = x86_lift_rip_load(state, reg, address);
        } else {
            status = x86_lift_rip_store(state, reg, address);
        }
        if (status != XAIR_OK) {
            return status;
        }
        state->pc = *pc;
        ++state->instructions;
        return XAIR_OK;
    }

    return x86_finish_unsupported(state, inst_start, opcode);
}

static xair_status x86_lift_modrm_alu(
    x86_lift_state *state,
    uint8_t opcode,
    uint8_t rex,
    uint64_t inst_start,
    uint64_t *pc) {
    uint8_t modrm;
    uint8_t mod;
    uint8_t reg_field;
    uint8_t rm_field;
    xair_x86_reg reg;
    xair_x86_reg rm;
    xair_x86_reg dst;
    xair_x86_reg src;
    xair_status status;

    if ((rex & 8u) == 0) {
        return x86_finish_unsupported(state, inst_start, opcode);
    }
    if (!image_read_u8(state->image, *pc, &modrm)) {
        return XAIR_ERR_RANGE;
    }
    ++*pc;
    (void)x86_decode_modrm(modrm, &mod, &reg_field, &rm_field);
    if (mod != 3u) {
        return x86_finish_unsupported(state, inst_start, opcode);
    }
    reg = (xair_x86_reg)(reg_field + ((rex & 4u) != 0 ? 8u : 0u));
    rm = (xair_x86_reg)(rm_field + ((rex & 1u) != 0 ? 8u : 0u));

    switch (opcode) {
    case 0x01u:
        dst = rm;
        src = reg;
        status = x86_lift_add_sub(state, XAIR_OP_ADD, XAIR_OP_FLAGS_ADD, dst, src);
        break;
    case 0x03u:
        dst = reg;
        src = rm;
        status = x86_lift_add_sub(state, XAIR_OP_ADD, XAIR_OP_FLAGS_ADD, dst, src);
        break;
    case 0x29u:
        dst = rm;
        src = reg;
        status = x86_lift_add_sub(state, XAIR_OP_SUB, XAIR_OP_FLAGS_SUB, dst, src);
        break;
    case 0x2bu:
        dst = reg;
        src = rm;
        status = x86_lift_add_sub(state, XAIR_OP_SUB, XAIR_OP_FLAGS_SUB, dst, src);
        break;
    case 0x39u:
        status = x86_lift_cmp(state, rm, reg);
        break;
    case 0x3bu:
        status = x86_lift_cmp(state, reg, rm);
        break;
    case 0x85u:
        status = x86_lift_test(state, rm, reg);
        break;
    default:
        status = XAIR_ERR_UNSUPPORTED;
        break;
    }
    if (status == XAIR_ERR_UNSUPPORTED) {
        return x86_finish_unsupported(state, inst_start, opcode);
    }
    if (status != XAIR_OK) {
        return status;
    }
    state->pc = *pc;
    ++state->instructions;
    return XAIR_OK;
}

static xair_status x86_condition_from_zf(
    x86_lift_state *state,
    int want_zero,
    xair_value_id *out_condition) {
    xair_value_id zero;
    xair_status status;

    if (out_condition == NULL || !state->has_zf) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (want_zero) {
        *out_condition = state->zf;
        return XAIR_OK;
    }
    status = x86_build_const_u64(state, xair_type_i(1), 0, "not_zf", &zero);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_EQ,
        xair_type_i(1),
        state->zf,
        zero,
        "jnz",
        out_condition);
}

static xair_status x86_lift_one(x86_lift_state *state, int *out_done) {
    uint64_t inst_start = state->pc;
    uint64_t pc = state->pc;
    uint8_t rex = 0;
    uint8_t opcode;

    *out_done = 0;
    if (!image_read_u8(state->image, pc, &opcode)) {
        return XAIR_ERR_RANGE;
    }
    while (opcode >= 0x40u && opcode <= 0x4fu) {
        rex = opcode;
        ++pc;
        if (!image_read_u8(state->image, pc, &opcode)) {
            return XAIR_ERR_RANGE;
        }
    }
    ++pc;

    if (opcode >= 0xb8u && opcode <= 0xbfu) {
        uint64_t imm;
        xair_x86_reg dst;
        xair_value_id value;
        xair_status status;

        if ((rex & 8u) == 0) {
            return x86_finish_unsupported(state, inst_start, opcode);
        }
        if (!image_read_le64(state->image, pc, &imm)) {
            return XAIR_ERR_RANGE;
        }
        pc += 8u;
        dst = (xair_x86_reg)((opcode - 0xb8u) + ((rex & 1u) != 0 ? 8u : 0u));
        status = x86_build_const_u64(state, xair_type_i(64), imm, xair_x86_reg_name(dst), &value);
        if (status != XAIR_OK) {
            return status;
        }
        x86_write_reg(state, dst, value);
        state->pc = pc;
        ++state->instructions;
        return XAIR_OK;
    }

    switch (opcode) {
    case 0x8bu:
    case 0x89u:
        return x86_lift_modrm_move(state, opcode, rex, inst_start, &pc);
    case 0x01u:
    case 0x03u:
    case 0x29u:
    case 0x2bu:
    case 0x39u:
    case 0x3bu:
    case 0x85u:
        return x86_lift_modrm_alu(state, opcode, rex, inst_start, &pc);
    case 0xebu: {
        uint8_t rel;
        uint64_t target;

        if (!image_read_u8(state->image, pc, &rel)) {
            return XAIR_ERR_RANGE;
        }
        pc += 1u;
        target = (uint64_t)((int64_t)pc + sign_extend8(rel));
        state->pc = pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_direct_jump(state, target);
    }
    case 0xe9u: {
        uint32_t rel;
        uint64_t target;

        if (!image_read_le32(state->image, pc, &rel)) {
            return XAIR_ERR_RANGE;
        }
        pc += 4u;
        target = (uint64_t)((int64_t)pc + sign_extend32(rel));
        state->pc = pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_direct_jump(state, target);
    }
    case 0x74u:
    case 0x75u: {
        uint8_t rel;
        uint64_t target;
        xair_value_id condition;
        xair_status status;

        if (!image_read_u8(state->image, pc, &rel)) {
            return XAIR_ERR_RANGE;
        }
        pc += 1u;
        status = x86_condition_from_zf(state, opcode == 0x74u, &condition);
        if (status == XAIR_ERR_UNSUPPORTED) {
            return x86_finish_unsupported(state, inst_start, opcode);
        }
        if (status != XAIR_OK) {
            return status;
        }
        target = (uint64_t)((int64_t)pc + sign_extend8(rel));
        state->pc = pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_direct_cbranch(state, target, pc, condition);
    }
    case 0x0fu: {
        uint8_t opcode2;

        if (!image_read_u8(state->image, pc, &opcode2)) {
            return XAIR_ERR_RANGE;
        }
        ++pc;
        if (opcode2 == 0x84u || opcode2 == 0x85u) {
            uint32_t rel;
            uint64_t target;
            xair_value_id condition;
            xair_status status;

            if (!image_read_le32(state->image, pc, &rel)) {
                return XAIR_ERR_RANGE;
            }
            pc += 4u;
            status = x86_condition_from_zf(state, opcode2 == 0x84u, &condition);
            if (status == XAIR_ERR_UNSUPPORTED) {
                return x86_finish_unsupported(state, inst_start, opcode);
            }
            if (status != XAIR_OK) {
                return status;
            }
            target = (uint64_t)((int64_t)pc + sign_extend32(rel));
            state->pc = pc;
            ++state->instructions;
            *out_done = 1;
            return x86_finish_direct_cbranch(state, target, pc, condition);
        }
        return x86_finish_unsupported(state, inst_start, opcode);
    }
    case 0xc3u:
        state->pc = pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_return(state);
    default:
        return x86_finish_unsupported(state, inst_start, opcode);
    }
}

static xair_status lift_x86_64_basic_block(
    xair_module *module,
    const xair_image *image,
    const xair_lift_options *options,
    xair_lift_result *out_result) {
    x86_lift_state state;
    char generated_name[40];
    size_t i;
    size_t max_instructions;
    xair_status status;

    memset(&state, 0, sizeof(state));
    for (i = 0; i < XAIR_X86_REG_COUNT; ++i) {
        state.regs[i] = XAIR_INVALID_ID;
    }
    state.module = module;
    state.image = image;
    state.result = out_result;
    state.pc = options->address;
    state.memory_space = options->memory_space;
    state.memory = XAIR_INVALID_ID;
    state.zf = XAIR_INVALID_ID;

    if (options->block_name == NULL || options->block_name[0] == '\0') {
        (void)snprintf(
            generated_name,
            sizeof(generated_name),
            "bb_%llx",
            (unsigned long long)options->address);
        status = xair_block_create(module, generated_name, &state.block);
    } else {
        status = xair_block_create(module, options->block_name, &state.block);
    }
    if (status != XAIR_OK) {
        return status;
    }

    out_result->block = state.block;
    out_result->start = options->address;
    max_instructions = options->max_instructions == 0 ? 32u : options->max_instructions;

    while (state.instructions < max_instructions) {
        int done;

        status = x86_lift_one(&state, &done);
        if (status != XAIR_OK) {
            return status;
        }
        if (done || state.result->end_kind == XAIR_LIFT_END_UNSUPPORTED) {
            return XAIR_OK;
        }
    }
    return x86_finish_fallthrough(&state);
}

xair_status xair_lift_basic_block(
    xair_module *module,
    const xair_image *image,
    const xair_lift_options *options,
    xair_lift_result *out_result) {
    size_t offset;

    if (module == NULL || image == NULL || options == NULL || out_result == NULL ||
        image->bytes == NULL || options->arch != XAIR_ARCH_X86_64) {
        return XAIR_ERR_BAD_ARG;
    }
    if (!image_offset(image, options->address, &offset)) {
        return XAIR_ERR_RANGE;
    }
    (void)offset;
    lift_result_init(out_result);
    return lift_x86_64_basic_block(module, image, options, out_result);
}
