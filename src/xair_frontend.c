#include "xair/xair_frontend.h"

#include "xair_x86_decode.h"

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
        result->input_regs[i].value = XAIR_INVALID_ID;
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
    if (state->result->input_reg_count >= XAIR_LIFT_MAX_REG_OUTPUTS) {
        return XAIR_ERR_RANGE;
    }
    state->result->input_regs[state->result->input_reg_count].reg = reg;
    state->result->input_regs[state->result->input_reg_count].value = state->regs[reg];
    ++state->result->input_reg_count;
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

static xair_status x86_set_zf_from_value(x86_lift_state *state, xair_value_id value) {
    xair_value_id zero;
    xair_status status;

    status = x86_build_const_u64(state, xair_type_i(64), 0, "zero", &zero);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_EQ,
        xair_type_i(1),
        value,
        zero,
        "zf",
        &state->zf);
    if (status != XAIR_OK) {
        return status;
    }
    state->has_zf = 1;
    return XAIR_OK;
}

static xair_status x86_const_i64(x86_lift_state *state, int64_t value, const char *name, xair_value_id *out_value) {
    return x86_build_const_u64(state, xair_type_i(64), (uint64_t)value, name, out_value);
}

static uint64_t x86_add_i64(uint64_t base, int64_t offset) {
    if (offset < 0) {
        return base - (uint64_t)(-offset);
    }
    return base + (uint64_t)offset;
}

static xair_status x86_lift_add_sub_values(
    x86_lift_state *state,
    xair_opcode value_opcode,
    xair_opcode flags_opcode,
    xair_x86_reg dst,
    xair_value_id lhs,
    xair_value_id rhs) {
    xair_value_id value;
    xair_value_id flags;
    xair_status status;

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

static xair_status x86_lift_cmp_values(x86_lift_state *state, xair_value_id lhs, xair_value_id rhs) {
    xair_value_id flags;
    xair_status status;

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

static xair_status x86_lift_test_values(x86_lift_state *state, xair_value_id lhs, xair_value_id rhs) {
    xair_value_id and_value;
    xair_status status;

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
    return x86_set_zf_from_value(state, and_value);
}

static xair_status x86_lift_logic_values(
    x86_lift_state *state,
    xair_opcode opcode,
    xair_x86_reg dst,
    xair_value_id lhs,
    xair_value_id rhs) {
    xair_value_id value;
    xair_status status;

    status = xair_build_binary(
        state->module,
        state->block,
        opcode,
        xair_type_i(64),
        lhs,
        rhs,
        xair_opcode_name(opcode),
        &value);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_set_zf_from_value(state, value);
    if (status != XAIR_OK) {
        return status;
    }
    x86_write_reg(state, dst, value);
    return XAIR_OK;
}

static xair_status x86_int_to_addr(x86_lift_state *state, xair_value_id value, xair_value_id *out_addr) {
    return xair_build_unary(
        state->module,
        state->block,
        XAIR_OP_INT_TO_ADDR,
        xair_type_addr(64),
        value,
        "addr",
        out_addr);
}

static xair_status x86_addr_to_int(x86_lift_state *state, xair_value_id value, xair_value_id *out_int) {
    return xair_build_unary(
        state->module,
        state->block,
        XAIR_OP_ADDR_TO_INT,
        xair_type_i(64),
        value,
        "lea",
        out_int);
}

static xair_status x86_addr_add_i64(
    x86_lift_state *state,
    xair_value_id base,
    int64_t offset,
    const char *name,
    xair_value_id *out_addr) {
    xair_value_id delta;
    xair_status status;

    status = x86_const_i64(state, offset, "disp", &delta);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_build_binary(
        state->module,
        state->block,
        XAIR_OP_ADDR_ADD,
        xair_type_addr(64),
        base,
        delta,
        name,
        out_addr);
}

static xair_status x86_build_memory_address(
    x86_lift_state *state,
    const xair_x86_memory_operand *mem,
    uint64_t next_pc,
    xair_value_id *out_addr) {
    xair_value_id addr = XAIR_INVALID_ID;
    xair_status status;

    if (state == NULL || mem == NULL || out_addr == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (mem->kind == XAIR_X86_MEM_RIP_REL) {
        return x86_build_const_u64(
            state,
            xair_type_addr(64),
            x86_add_i64(next_pc, mem->displacement),
            "rip_addr",
            out_addr);
    }
    if (mem->kind != XAIR_X86_MEM_BASE_INDEX) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (mem->has_base) {
        xair_value_id base_value;

        status = x86_ensure_reg(state, mem->base, &base_value);
        if (status != XAIR_OK) {
            return status;
        }
        status = x86_int_to_addr(state, base_value, &addr);
        if (status != XAIR_OK) {
            return status;
        }
    }
    if (mem->has_index) {
        xair_value_id index_value;
        xair_value_id scaled_value = XAIR_INVALID_ID;

        status = x86_ensure_reg(state, mem->index, &index_value);
        if (status != XAIR_OK) {
            return status;
        }
        if (mem->scale != 1u) {
            xair_value_id scale_value;

            status = x86_const_i64(state, (int64_t)mem->scale, "scale", &scale_value);
            if (status != XAIR_OK) {
                return status;
            }
            status = xair_build_binary(
                state->module,
                state->block,
                XAIR_OP_MUL,
                xair_type_i(64),
                index_value,
                scale_value,
                "index",
                &scaled_value);
            if (status != XAIR_OK) {
                return status;
            }
        } else {
            scaled_value = index_value;
        }
        if (addr == XAIR_INVALID_ID) {
            status = x86_int_to_addr(state, scaled_value, &addr);
            if (status != XAIR_OK) {
                return status;
            }
        } else {
            status = xair_build_binary(
                state->module,
                state->block,
                XAIR_OP_ADDR_ADD,
                xair_type_addr(64),
                addr,
                scaled_value,
                "addr_index",
                &addr);
            if (status != XAIR_OK) {
                return status;
            }
        }
    }
    if (addr == XAIR_INVALID_ID) {
        return x86_build_const_u64(
            state,
            xair_type_addr(64),
            (uint64_t)mem->displacement,
            "addr",
            out_addr);
    }
    if (mem->displacement != 0) {
        return x86_addr_add_i64(state, addr, mem->displacement, "addr_disp", out_addr);
    }
    *out_addr = addr;
    return XAIR_OK;
}

static xair_status x86_lift_mem_load(
    x86_lift_state *state,
    xair_x86_reg dst,
    const xair_x86_memory_operand *mem,
    uint64_t next_pc) {
    xair_value_id memory;
    xair_value_id addr_value;
    xair_value_id loaded;
    xair_status status;

    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_build_memory_address(state, mem, next_pc, &addr_value);
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

static xair_status x86_lift_mem_store(
    x86_lift_state *state,
    const xair_x86_memory_operand *mem,
    xair_value_id data,
    uint64_t next_pc) {
    xair_value_id memory;
    xair_value_id addr_value;
    xair_value_id next_memory;
    xair_status status;

    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_build_memory_address(state, mem, next_pc, &addr_value);
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

static int x86_get_reg_operand(
    const xair_x86_decoded_inst *inst,
    size_t index,
    xair_x86_reg *out_reg) {
    if (inst == NULL || out_reg == NULL || index >= inst->operand_count ||
        inst->operands[index].kind != XAIR_X86_OPERAND_REGISTER) {
        return 0;
    }
    *out_reg = inst->operands[index].value.reg;
    return 1;
}

static int x86_get_imm_operand(
    const xair_x86_decoded_inst *inst,
    size_t index,
    int64_t *out_value) {
    if (inst == NULL || out_value == NULL || index >= inst->operand_count ||
        inst->operands[index].kind != XAIR_X86_OPERAND_IMMEDIATE) {
        return 0;
    }
    *out_value = inst->operands[index].value.imm;
    return 1;
}

static int x86_get_mem_operand(
    const xair_x86_decoded_inst *inst,
    size_t index,
    const xair_x86_memory_operand **out_mem) {
    if (inst == NULL || out_mem == NULL || index >= inst->operand_count ||
        inst->operands[index].kind != XAIR_X86_OPERAND_MEMORY) {
        return 0;
    }
    *out_mem = &inst->operands[index].value.mem;
    return 1;
}

static xair_status x86_operand_value(
    x86_lift_state *state,
    const xair_x86_decoded_inst *inst,
    size_t index,
    xair_value_id *out_value) {
    xair_x86_reg reg;
    int64_t imm;

    if (x86_get_reg_operand(inst, index, &reg)) {
        return x86_ensure_reg(state, reg, out_value);
    }
    if (x86_get_imm_operand(inst, index, &imm)) {
        return x86_const_i64(state, imm, "imm", out_value);
    }
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status x86_lift_mov_decoded(
    x86_lift_state *state,
    const xair_x86_decoded_inst *inst,
    uint64_t next_pc) {
    xair_x86_reg dst;
    xair_x86_reg src;
    int64_t imm;
    const xair_x86_memory_operand *mem;
    xair_status status;

    if (x86_get_reg_operand(inst, 0, &dst) && x86_get_imm_operand(inst, 1, &imm)) {
        xair_value_id value;

        status = x86_build_const_u64(state, xair_type_i(64), (uint64_t)imm, xair_x86_reg_name(dst), &value);
        if (status != XAIR_OK) {
            return status;
        }
        x86_write_reg(state, dst, value);
        return XAIR_OK;
    }
    if (x86_get_reg_operand(inst, 0, &dst) && x86_get_reg_operand(inst, 1, &src)) {
        xair_value_id value;

        status = x86_ensure_reg(state, src, &value);
        if (status != XAIR_OK) {
            return status;
        }
        x86_write_reg(state, dst, value);
        return XAIR_OK;
    }
    if (x86_get_reg_operand(inst, 0, &dst) && x86_get_mem_operand(inst, 1, &mem)) {
        return x86_lift_mem_load(state, dst, mem, next_pc);
    }
    if (x86_get_mem_operand(inst, 0, &mem) && x86_get_reg_operand(inst, 1, &src)) {
        xair_value_id data;

        status = x86_ensure_reg(state, src, &data);
        if (status != XAIR_OK) {
            return status;
        }
        return x86_lift_mem_store(state, mem, data, next_pc);
    }
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status x86_lift_lea_decoded(
    x86_lift_state *state,
    const xair_x86_decoded_inst *inst,
    uint64_t next_pc) {
    xair_x86_reg dst;
    const xair_x86_memory_operand *mem;
    xair_value_id addr;
    xair_value_id value;
    xair_status status;

    if (!x86_get_reg_operand(inst, 0, &dst) || !x86_get_mem_operand(inst, 1, &mem)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    status = x86_build_memory_address(state, mem, next_pc, &addr);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_addr_to_int(state, addr, &value);
    if (status != XAIR_OK) {
        return status;
    }
    x86_write_reg(state, dst, value);
    return XAIR_OK;
}

static xair_opcode x86_logic_opcode(xair_x86_mnemonic mnemonic) {
    switch (mnemonic) {
    case XAIR_X86_MNEMONIC_AND:
        return XAIR_OP_AND;
    case XAIR_X86_MNEMONIC_OR:
        return XAIR_OP_OR;
    case XAIR_X86_MNEMONIC_XOR:
        return XAIR_OP_XOR;
    default:
        return XAIR_OP_CONST_U64;
    }
}

static xair_status x86_lift_alu_decoded(x86_lift_state *state, const xair_x86_decoded_inst *inst) {
    xair_x86_reg dst;
    xair_value_id lhs;
    xair_value_id rhs;
    xair_status status;

    if (!x86_get_reg_operand(inst, 0, &dst)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    status = x86_ensure_reg(state, dst, &lhs);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_operand_value(state, inst, 1, &rhs);
    if (status != XAIR_OK) {
        return status;
    }
    if (inst->operands[1].kind == XAIR_X86_OPERAND_MEMORY) {
        return XAIR_ERR_UNSUPPORTED;
    }
    switch (inst->mnemonic) {
    case XAIR_X86_MNEMONIC_ADD:
        return x86_lift_add_sub_values(state, XAIR_OP_ADD, XAIR_OP_FLAGS_ADD, dst, lhs, rhs);
    case XAIR_X86_MNEMONIC_SUB:
        return x86_lift_add_sub_values(state, XAIR_OP_SUB, XAIR_OP_FLAGS_SUB, dst, lhs, rhs);
    case XAIR_X86_MNEMONIC_AND:
    case XAIR_X86_MNEMONIC_OR:
    case XAIR_X86_MNEMONIC_XOR:
        return x86_lift_logic_values(state, x86_logic_opcode(inst->mnemonic), dst, lhs, rhs);
    case XAIR_X86_MNEMONIC_CMP:
        return x86_lift_cmp_values(state, lhs, rhs);
    case XAIR_X86_MNEMONIC_TEST:
        return x86_lift_test_values(state, lhs, rhs);
    default:
        return XAIR_ERR_UNSUPPORTED;
    }
}

static xair_status x86_lift_push_decoded(x86_lift_state *state, const xair_x86_decoded_inst *inst) {
    xair_x86_reg src;
    xair_value_id rsp;
    xair_value_id data;
    xair_value_id eight;
    xair_value_id next_rsp;
    xair_value_id addr;
    xair_value_id memory;
    xair_value_id next_memory;
    xair_status status;

    if (!x86_get_reg_operand(inst, 0, &src)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    status = x86_ensure_reg(state, XAIR_X86_RSP, &rsp);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_reg(state, src, &data);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_const_i64(state, 8, "push_size", &eight);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(state->module, state->block, XAIR_OP_SUB, xair_type_i(64), rsp, eight, "push_rsp", &next_rsp);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_int_to_addr(state, next_rsp, &addr);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_store(
        state->module,
        state->block,
        memory,
        addr,
        data,
        XAIR_ENDIAN_LE,
        "push",
        &next_memory);
    if (status != XAIR_OK) {
        return status;
    }
    state->memory = next_memory;
    state->result->memory_out = next_memory;
    x86_write_reg(state, XAIR_X86_RSP, next_rsp);
    return XAIR_OK;
}

static xair_status x86_lift_pop_decoded(x86_lift_state *state, const xair_x86_decoded_inst *inst) {
    xair_x86_reg dst;
    xair_value_id rsp;
    xair_value_id addr;
    xair_value_id memory;
    xair_value_id loaded;
    xair_value_id eight;
    xair_value_id next_rsp;
    xair_status status;

    if (!x86_get_reg_operand(inst, 0, &dst)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    status = x86_ensure_reg(state, XAIR_X86_RSP, &rsp);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_int_to_addr(state, rsp, &addr);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_ensure_memory(state, &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_load(
        state->module,
        state->block,
        xair_type_i(64),
        memory,
        addr,
        XAIR_ENDIAN_LE,
        "pop",
        &loaded);
    if (status != XAIR_OK) {
        return status;
    }
    status = x86_const_i64(state, 8, "pop_size", &eight);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(state->module, state->block, XAIR_OP_ADD, xair_type_i(64), rsp, eight, "pop_rsp", &next_rsp);
    if (status != XAIR_OK) {
        return status;
    }
    x86_write_reg(state, dst, loaded);
    x86_write_reg(state, XAIR_X86_RSP, next_rsp);
    return XAIR_OK;
}

static xair_status x86_lift_one(x86_lift_state *state, int *out_done) {
    uint64_t inst_start = state->pc;
    uint64_t next_pc;
    size_t offset;
    xair_x86_decoded_inst inst;
    xair_status status;

    *out_done = 0;
    if (!image_offset(state->image, inst_start, &offset)) {
        return XAIR_ERR_RANGE;
    }
    status = xair_x86_decode64(state->image->bytes + offset, state->image->size - offset, &inst);
    if (status == XAIR_ERR_UNSUPPORTED) {
        return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
    }
    if (status != XAIR_OK) {
        return status;
    }
    next_pc = inst_start + inst.length;

    switch (inst.mnemonic) {
    case XAIR_X86_MNEMONIC_NOP:
        status = XAIR_OK;
        break;
    case XAIR_X86_MNEMONIC_MOV:
        status = x86_lift_mov_decoded(state, &inst, next_pc);
        break;
    case XAIR_X86_MNEMONIC_LEA:
        status = x86_lift_lea_decoded(state, &inst, next_pc);
        break;
    case XAIR_X86_MNEMONIC_ADD:
    case XAIR_X86_MNEMONIC_SUB:
    case XAIR_X86_MNEMONIC_AND:
    case XAIR_X86_MNEMONIC_OR:
    case XAIR_X86_MNEMONIC_XOR:
    case XAIR_X86_MNEMONIC_CMP:
    case XAIR_X86_MNEMONIC_TEST:
        status = x86_lift_alu_decoded(state, &inst);
        break;
    case XAIR_X86_MNEMONIC_PUSH:
        status = x86_lift_push_decoded(state, &inst);
        break;
    case XAIR_X86_MNEMONIC_POP:
        status = x86_lift_pop_decoded(state, &inst);
        break;
    case XAIR_X86_MNEMONIC_JMP: {
        int64_t rel;

        if (!x86_get_imm_operand(&inst, 0, &rel)) {
            return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
        }
        state->pc = next_pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_direct_jump(state, x86_add_i64(next_pc, rel));
    }
    case XAIR_X86_MNEMONIC_JZ:
    case XAIR_X86_MNEMONIC_JNZ: {
        int64_t rel;
        xair_value_id condition;

        if (!x86_get_imm_operand(&inst, 0, &rel)) {
            return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
        }
        status = x86_condition_from_zf(state, inst.mnemonic == XAIR_X86_MNEMONIC_JZ, &condition);
        if (status == XAIR_ERR_UNSUPPORTED) {
            return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
        }
        if (status != XAIR_OK) {
            return status;
        }
        state->pc = next_pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_direct_cbranch(state, x86_add_i64(next_pc, rel), next_pc, condition);
    }
    case XAIR_X86_MNEMONIC_RET:
        state->pc = next_pc;
        ++state->instructions;
        *out_done = 1;
        return x86_finish_return(state);
    default:
        return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
    }

    if (status == XAIR_ERR_UNSUPPORTED) {
        return x86_finish_unsupported(state, inst_start, inst.raw_opcode);
    }
    if (status != XAIR_OK) {
        return status;
    }
    state->pc = next_pc;
    ++state->instructions;
    return XAIR_OK;
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
