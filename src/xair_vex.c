#include "xair/xair_vex.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    uint32_t key;
    xair_value_id value;
} xair_vex_map_entry;

struct xair_vex_adapter {
    xair_module *module;
    xair_block_id current;
    xair_value_id memory;
    int has_memory;
    xair_vex_map_entry *tmps;
    size_t tmp_count;
    size_t tmp_cap;
    xair_vex_map_entry *regs;
    size_t reg_count;
    size_t reg_cap;
    uint32_t continuation_index;
};

static xair_status vex_grow_array(void **ptr, size_t elem_size, size_t *cap, size_t need) {
    size_t next_cap;
    void *next;

    if (need <= *cap) {
        return XAIR_OK;
    }
    next_cap = *cap ? *cap : 8;
    while (next_cap < need) {
        if (next_cap > SIZE_MAX / 2u) {
            return XAIR_ERR_OOM;
        }
        next_cap *= 2u;
    }
    if (elem_size != 0 && next_cap > SIZE_MAX / elem_size) {
        return XAIR_ERR_OOM;
    }
    next = realloc(*ptr, next_cap * elem_size);
    if (next == NULL) {
        return XAIR_ERR_OOM;
    }
    *ptr = next;
    *cap = next_cap;
    return XAIR_OK;
}

static xair_vex_map_entry *vex_find_entry(xair_vex_map_entry *entries, size_t count, uint32_t key) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (entries[i].key == key) {
            return &entries[i];
        }
    }
    return NULL;
}

static const xair_vex_map_entry *vex_find_entry_const(
    const xair_vex_map_entry *entries,
    size_t count,
    uint32_t key) {
    size_t i;

    for (i = 0; i < count; ++i) {
        if (entries[i].key == key) {
            return &entries[i];
        }
    }
    return NULL;
}

static xair_status vex_set_entry(
    xair_vex_map_entry **entries,
    size_t *count,
    size_t *cap,
    uint32_t key,
    xair_value_id value) {
    xair_vex_map_entry *entry = vex_find_entry(*entries, *count, key);
    xair_status status;

    if (entry != NULL) {
        entry->value = value;
        return XAIR_OK;
    }
    status = vex_grow_array((void **)entries, sizeof(**entries), cap, *count + 1u);
    if (status != XAIR_OK) {
        return status;
    }
    (*entries)[*count].key = key;
    (*entries)[*count].value = value;
    ++*count;
    return XAIR_OK;
}

static int vex_valid_value(const xair_module *module, xair_value_id value) {
    xair_type type;

    if (module == NULL || value == XAIR_INVALID_ID) {
        return 0;
    }
    type = xair_value_type(module, value);
    return type.kind != XAIR_TYPE_INVALID && xair_type_is_valid(type);
}

static int vex_starts_with(const char *text, const char *prefix) {
    size_t prefix_len;

    if (text == NULL || prefix == NULL) {
        return 0;
    }
    prefix_len = strlen(prefix);
    return strncmp(text, prefix, prefix_len) == 0;
}

static int vex_parse_u16(const char *text, size_t *index, uint16_t *out_value) {
    unsigned value = 0;
    size_t i;
    int saw_digit = 0;

    if (text == NULL || index == NULL || out_value == NULL) {
        return 0;
    }
    i = *index;
    while (text[i] >= '0' && text[i] <= '9') {
        saw_digit = 1;
        value = value * 10u + (unsigned)(text[i] - '0');
        if (value > UINT16_MAX) {
            return 0;
        }
        ++i;
    }
    if (!saw_digit) {
        return 0;
    }
    *index = i;
    *out_value = (uint16_t)value;
    return 1;
}

static int vex_match_width(const char *irop, const char *prefix, uint16_t *out_bits) {
    size_t index;

    if (!vex_starts_with(irop, prefix)) {
        return 0;
    }
    index = strlen(prefix);
    if (!vex_parse_u16(irop, &index, out_bits)) {
        return 0;
    }
    return irop[index] == '\0';
}

static int vex_match_cmp(
    const char *irop,
    const char *prefix,
    uint16_t *out_bits,
    char *out_signedness) {
    size_t index;

    if (!vex_starts_with(irop, prefix)) {
        return 0;
    }
    index = strlen(prefix);
    if (!vex_parse_u16(irop, &index, out_bits)) {
        return 0;
    }
    if ((irop[index] != 'U' && irop[index] != 'S') || irop[index + 1u] != '\0') {
        return 0;
    }
    *out_signedness = irop[index];
    return 1;
}

static xair_status vex_check_width(xair_type type, uint16_t bits) {
    if (type.bits != bits) {
        return XAIR_ERR_UNSUPPORTED;
    }
    return XAIR_OK;
}

static xair_status vex_map_binop(
    const char *irop,
    xair_type lhs,
    xair_opcode *out_opcode,
    xair_type *out_type) {
    uint16_t bits;
    char signedness;

    if (irop == NULL || out_opcode == NULL || out_type == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (vex_match_width(irop, "Iop_Add", &bits)) {
        *out_opcode = XAIR_OP_ADD;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Sub", &bits)) {
        *out_opcode = XAIR_OP_SUB;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Mul", &bits)) {
        *out_opcode = XAIR_OP_MUL;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_And", &bits)) {
        *out_opcode = XAIR_OP_AND;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Or", &bits)) {
        *out_opcode = XAIR_OP_OR;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Xor", &bits)) {
        *out_opcode = XAIR_OP_XOR;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Shl", &bits)) {
        *out_opcode = XAIR_OP_SHL;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Shr", &bits)) {
        *out_opcode = XAIR_OP_LSHR;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_Sar", &bits)) {
        *out_opcode = XAIR_OP_ASHR;
        *out_type = lhs;
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_CmpEQ", &bits)) {
        *out_opcode = XAIR_OP_EQ;
        *out_type = xair_type_i(1);
        return vex_check_width(lhs, bits);
    }
    if (vex_match_width(irop, "Iop_CmpNE", &bits)) {
        *out_opcode = XAIR_OP_NE;
        *out_type = xair_type_i(1);
        return vex_check_width(lhs, bits);
    }
    if (vex_match_cmp(irop, "Iop_CmpLT", &bits, &signedness)) {
        *out_opcode = signedness == 'S' ? XAIR_OP_SLT : XAIR_OP_ULT;
        *out_type = xair_type_i(1);
        return vex_check_width(lhs, bits);
    }
    if (vex_match_cmp(irop, "Iop_CmpLE", &bits, &signedness)) {
        *out_opcode = signedness == 'S' ? XAIR_OP_SLE : XAIR_OP_ULE;
        *out_type = xair_type_i(1);
        return vex_check_width(lhs, bits);
    }
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status vex_map_unop(
    const char *irop,
    xair_type src,
    xair_opcode *out_opcode,
    xair_type *out_type) {
    uint16_t from_bits;
    uint16_t to_bits;
    size_t index;

    if (irop == NULL || out_opcode == NULL || out_type == NULL || !vex_starts_with(irop, "Iop_")) {
        return XAIR_ERR_BAD_ARG;
    }
    index = 4u;
    if (!vex_parse_u16(irop, &index, &from_bits)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (src.bits != from_bits || src.kind != XAIR_TYPE_INT) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (irop[index] == 'U' && irop[index + 1u] == 't' && irop[index + 2u] == 'o') {
        index += 3u;
        if (!vex_parse_u16(irop, &index, &to_bits) || irop[index] != '\0' || to_bits <= from_bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        *out_opcode = XAIR_OP_ZEXT;
        *out_type = xair_type_i(to_bits);
        return XAIR_OK;
    }
    if (irop[index] == 'S' && irop[index + 1u] == 't' && irop[index + 2u] == 'o') {
        index += 3u;
        if (!vex_parse_u16(irop, &index, &to_bits) || irop[index] != '\0' || to_bits <= from_bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        *out_opcode = XAIR_OP_SEXT;
        *out_type = xair_type_i(to_bits);
        return XAIR_OK;
    }
    if (irop[index] == 't' && irop[index + 1u] == 'o') {
        index += 2u;
        if (!vex_parse_u16(irop, &index, &to_bits) || irop[index] != '\0' || to_bits >= from_bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        *out_opcode = XAIR_OP_TRUNC;
        *out_type = xair_type_i(to_bits);
        return XAIR_OK;
    }
    return XAIR_ERR_UNSUPPORTED;
}

xair_status xair_vex_adapter_create(
    xair_module *module,
    xair_block_id entry,
    xair_value_id initial_memory,
    xair_vex_adapter **out_adapter) {
    xair_vex_adapter *adapter;

    if (module == NULL || out_adapter == NULL || entry >= xair_module_block_count(module)) {
        return XAIR_ERR_BAD_ARG;
    }
    adapter = (xair_vex_adapter *)calloc(1, sizeof(*adapter));
    if (adapter == NULL) {
        return XAIR_ERR_OOM;
    }
    adapter->module = module;
    adapter->current = entry;
    adapter->memory = XAIR_INVALID_ID;
    if (initial_memory != XAIR_INVALID_ID) {
        xair_type type = xair_value_type(module, initial_memory);
        if (type.kind != XAIR_TYPE_MEM) {
            free(adapter);
            return XAIR_ERR_BAD_ARG;
        }
        adapter->memory = initial_memory;
        adapter->has_memory = 1;
    }
    *out_adapter = adapter;
    return XAIR_OK;
}

void xair_vex_adapter_destroy(xair_vex_adapter *adapter) {
    if (adapter == NULL) {
        return;
    }
    free(adapter->tmps);
    free(adapter->regs);
    free(adapter);
}

xair_block_id xair_vex_current_block(const xair_vex_adapter *adapter) {
    return adapter == NULL ? XAIR_INVALID_ID : adapter->current;
}

xair_status xair_vex_set_tmp(xair_vex_adapter *adapter, uint32_t tmp, xair_value_id value) {
    if (adapter == NULL || !vex_valid_value(adapter->module, value)) {
        return XAIR_ERR_BAD_ARG;
    }
    return vex_set_entry(&adapter->tmps, &adapter->tmp_count, &adapter->tmp_cap, tmp, value);
}

xair_status xair_vex_get_tmp(
    const xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_value_id *out_value) {
    const xair_vex_map_entry *entry;

    if (adapter == NULL || out_value == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    entry = vex_find_entry_const(adapter->tmps, adapter->tmp_count, tmp);
    if (entry == NULL) {
        return XAIR_ERR_RANGE;
    }
    *out_value = entry->value;
    return XAIR_OK;
}

xair_status xair_vex_get_reg(
    xair_vex_adapter *adapter,
    uint32_t offset,
    xair_type type,
    const char *name,
    xair_value_id *out_value) {
    xair_vex_map_entry *entry;
    xair_status status;
    xair_value_id value;

    if (adapter == NULL || out_value == NULL || !xair_type_is_valid(type)) {
        return XAIR_ERR_BAD_ARG;
    }
    entry = vex_find_entry(adapter->regs, adapter->reg_count, offset);
    if (entry != NULL) {
        if (!xair_type_equal(xair_value_type(adapter->module, entry->value), type)) {
            return XAIR_ERR_BAD_ARG;
        }
        *out_value = entry->value;
        return XAIR_OK;
    }
    status = xair_block_add_param(adapter->module, adapter->current, type, name, &value);
    if (status != XAIR_OK) {
        return status;
    }
    status = vex_set_entry(&adapter->regs, &adapter->reg_count, &adapter->reg_cap, offset, value);
    if (status != XAIR_OK) {
        return status;
    }
    *out_value = value;
    return XAIR_OK;
}

xair_status xair_vex_put_reg(xair_vex_adapter *adapter, uint32_t offset, xair_value_id value) {
    if (adapter == NULL || !vex_valid_value(adapter->module, value)) {
        return XAIR_ERR_BAD_ARG;
    }
    return vex_set_entry(&adapter->regs, &adapter->reg_count, &adapter->reg_cap, offset, value);
}

xair_status xair_vex_peek_reg(
    const xair_vex_adapter *adapter,
    uint32_t offset,
    xair_value_id *out_value) {
    const xair_vex_map_entry *entry;

    if (adapter == NULL || out_value == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    entry = vex_find_entry_const(adapter->regs, adapter->reg_count, offset);
    if (entry == NULL) {
        return XAIR_ERR_RANGE;
    }
    *out_value = entry->value;
    return XAIR_OK;
}

xair_status xair_vex_set_memory(xair_vex_adapter *adapter, xair_value_id memory) {
    xair_type type;

    if (adapter == NULL || !vex_valid_value(adapter->module, memory)) {
        return XAIR_ERR_BAD_ARG;
    }
    type = xair_value_type(adapter->module, memory);
    if (type.kind != XAIR_TYPE_MEM) {
        return XAIR_ERR_BAD_ARG;
    }
    adapter->memory = memory;
    adapter->has_memory = 1;
    return XAIR_OK;
}

xair_status xair_vex_current_memory(const xair_vex_adapter *adapter, xair_value_id *out_memory) {
    if (adapter == NULL || out_memory == NULL || !adapter->has_memory) {
        return XAIR_ERR_BAD_ARG;
    }
    *out_memory = adapter->memory;
    return XAIR_OK;
}

xair_status xair_vex_emit_const(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_type type,
    uint64_t value,
    const char *name) {
    xair_value_id out;
    xair_status status;

    if (adapter == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    status = xair_build_const_u64(adapter->module, adapter->current, type, value, name, &out);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_vex_set_tmp(adapter, tmp, out);
}

xair_status xair_vex_emit_unop(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    const char *irop,
    xair_value_id src,
    const char *name) {
    xair_type src_type;
    xair_type out_type;
    xair_opcode opcode;
    xair_value_id out;
    xair_status status;

    if (adapter == NULL || !vex_valid_value(adapter->module, src)) {
        return XAIR_ERR_BAD_ARG;
    }
    src_type = xair_value_type(adapter->module, src);
    status = vex_map_unop(irop, src_type, &opcode, &out_type);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_unary(adapter->module, adapter->current, opcode, out_type, src, name, &out);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_vex_set_tmp(adapter, tmp, out);
}

xair_status xair_vex_emit_binop(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    const char *irop,
    xair_value_id lhs,
    xair_value_id rhs,
    const char *name) {
    xair_type lhs_type;
    xair_type rhs_type;
    xair_type out_type;
    xair_opcode opcode;
    xair_value_id out;
    xair_status status;

    if (adapter == NULL || !vex_valid_value(adapter->module, lhs) ||
        !vex_valid_value(adapter->module, rhs)) {
        return XAIR_ERR_BAD_ARG;
    }
    lhs_type = xair_value_type(adapter->module, lhs);
    rhs_type = xair_value_type(adapter->module, rhs);
    if (!xair_type_equal(lhs_type, rhs_type) && !vex_starts_with(irop, "Iop_Shl") &&
        !vex_starts_with(irop, "Iop_Shr") && !vex_starts_with(irop, "Iop_Sar")) {
        return XAIR_ERR_BAD_ARG;
    }
    status = vex_map_binop(irop, lhs_type, &opcode, &out_type);
    if (status != XAIR_OK) {
        return status;
    }
    status = xair_build_binary(adapter->module, adapter->current, opcode, out_type, lhs, rhs, name, &out);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_vex_set_tmp(adapter, tmp, out);
}

xair_status xair_vex_emit_load(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_type type,
    xair_value_id address,
    xair_endian endian,
    const char *name) {
    xair_value_id out;
    xair_status status;

    if (adapter == NULL || !adapter->has_memory || !vex_valid_value(adapter->module, address)) {
        return XAIR_ERR_BAD_ARG;
    }
    status = xair_build_load(adapter->module, adapter->current, type, adapter->memory, address, endian, name, &out);
    if (status != XAIR_OK) {
        return status;
    }
    return xair_vex_set_tmp(adapter, tmp, out);
}

xair_status xair_vex_emit_store(
    xair_vex_adapter *adapter,
    xair_value_id address,
    xair_value_id data,
    xair_endian endian,
    const char *name) {
    xair_value_id next_memory;
    xair_status status;

    if (adapter == NULL || !adapter->has_memory || !vex_valid_value(adapter->module, address) ||
        !vex_valid_value(adapter->module, data)) {
        return XAIR_ERR_BAD_ARG;
    }
    status = xair_build_store(
        adapter->module,
        adapter->current,
        adapter->memory,
        address,
        data,
        endian,
        name,
        &next_memory);
    if (status != XAIR_OK) {
        return status;
    }
    adapter->memory = next_memory;
    return XAIR_OK;
}

static xair_status vex_append_arg(
    xair_value_id **args,
    size_t *arg_count,
    size_t *arg_cap,
    xair_value_id value) {
    xair_status status;

    status = vex_grow_array((void **)args, sizeof(**args), arg_cap, *arg_count + 1u);
    if (status != XAIR_OK) {
        return status;
    }
    (*args)[*arg_count] = value;
    ++*arg_count;
    return XAIR_OK;
}

static xair_status vex_transfer_value_to_continuation(
    xair_vex_adapter *adapter,
    xair_block_id continuation,
    const char *name,
    xair_value_id old_value,
    xair_value_id *out_new_value,
    xair_value_id **args,
    size_t *arg_count,
    size_t *arg_cap) {
    xair_type type;
    xair_status status;

    type = xair_value_type(adapter->module, old_value);
    status = xair_block_add_param(adapter->module, continuation, type, name, out_new_value);
    if (status != XAIR_OK) {
        return status;
    }
    return vex_append_arg(args, arg_count, arg_cap, old_value);
}

xair_status xair_vex_emit_exit(
    xair_vex_adapter *adapter,
    xair_value_id condition,
    xair_block_id taken,
    const xair_value_id *taken_args,
    size_t taken_arg_count,
    const char *continuation_name) {
    xair_block_id old_current;
    xair_block_id continuation;
    xair_value_id *cont_args = NULL;
    xair_value_id *next_tmps = NULL;
    xair_value_id *next_regs = NULL;
    xair_value_id next_memory = XAIR_INVALID_ID;
    size_t cont_arg_count = 0;
    size_t cont_arg_cap = 0;
    size_t i;
    xair_status status;
    char generated_name[32];

    if (adapter == NULL || !vex_valid_value(adapter->module, condition) ||
        taken >= xair_module_block_count(adapter->module)) {
        return XAIR_ERR_BAD_ARG;
    }
    old_current = adapter->current;
    if (continuation_name == NULL || continuation_name[0] == '\0') {
        (void)snprintf(
            generated_name,
            sizeof(generated_name),
            "vex_cont_%u",
            (unsigned)adapter->continuation_index++);
        continuation_name = generated_name;
    }
    status = xair_block_create(adapter->module, continuation_name, &continuation);
    if (status != XAIR_OK) {
        return status;
    }
    if (adapter->tmp_count != 0) {
        next_tmps = (xair_value_id *)malloc(adapter->tmp_count * sizeof(*next_tmps));
        if (next_tmps == NULL) {
            return XAIR_ERR_OOM;
        }
    }
    if (adapter->reg_count != 0) {
        next_regs = (xair_value_id *)malloc(adapter->reg_count * sizeof(*next_regs));
        if (next_regs == NULL) {
            free(next_tmps);
            return XAIR_ERR_OOM;
        }
    }

    if (adapter->has_memory) {
        status = vex_transfer_value_to_continuation(
            adapter,
            continuation,
            "mem",
            adapter->memory,
            &next_memory,
            &cont_args,
            &cont_arg_count,
            &cont_arg_cap);
        if (status != XAIR_OK) {
            free(cont_args);
            free(next_tmps);
            free(next_regs);
            return status;
        }
    }

    for (i = 0; i < adapter->tmp_count; ++i) {
        char name[32];

        (void)snprintf(name, sizeof(name), "t%u", (unsigned)adapter->tmps[i].key);
        status = vex_transfer_value_to_continuation(
            adapter,
            continuation,
            name,
            adapter->tmps[i].value,
            &next_tmps[i],
            &cont_args,
            &cont_arg_count,
            &cont_arg_cap);
        if (status != XAIR_OK) {
            free(cont_args);
            free(next_tmps);
            free(next_regs);
            return status;
        }
    }

    for (i = 0; i < adapter->reg_count; ++i) {
        char name[32];

        (void)snprintf(name, sizeof(name), "r%u", (unsigned)adapter->regs[i].key);
        status = vex_transfer_value_to_continuation(
            adapter,
            continuation,
            name,
            adapter->regs[i].value,
            &next_regs[i],
            &cont_args,
            &cont_arg_count,
            &cont_arg_cap);
        if (status != XAIR_OK) {
            free(cont_args);
            free(next_tmps);
            free(next_regs);
            return status;
        }
    }

    status = xair_set_cbranch(
        adapter->module,
        old_current,
        condition,
        taken,
        taken_args,
        taken_arg_count,
        continuation,
        cont_args,
        cont_arg_count);
    free(cont_args);
    if (status != XAIR_OK) {
        free(next_tmps);
        free(next_regs);
        return status;
    }
    if (adapter->has_memory) {
        adapter->memory = next_memory;
    }
    for (i = 0; i < adapter->tmp_count; ++i) {
        adapter->tmps[i].value = next_tmps[i];
    }
    for (i = 0; i < adapter->reg_count; ++i) {
        adapter->regs[i].value = next_regs[i];
    }
    free(next_tmps);
    free(next_regs);
    adapter->current = continuation;
    return XAIR_OK;
}

xair_status xair_vex_finish_jump(
    xair_vex_adapter *adapter,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count) {
    if (adapter == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    return xair_set_jump(adapter->module, adapter->current, target, args, arg_count);
}

xair_status xair_vex_finish_return(
    xair_vex_adapter *adapter,
    const xair_value_id *values,
    size_t value_count) {
    if (adapter == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    return xair_set_return(adapter->module, adapter->current, values, value_count);
}
