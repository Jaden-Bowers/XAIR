#include "xair/xair.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XAIR_NAME_LEN 32

typedef enum {
    XAIR_TERM_NONE = 0,
    XAIR_TERM_JUMP,
    XAIR_TERM_CBRANCH,
    XAIR_TERM_RETURN,
    XAIR_TERM_TRAP,
    XAIR_TERM_FAULT
} xair_term_kind;

typedef struct {
    xair_type type;
    xair_block_id block;
    xair_op_id op;
    char name[XAIR_NAME_LEN];
} xair_value_rec;

typedef struct {
    xair_opcode opcode;
    xair_value_id dst;
    xair_value_id src[3];
    uint8_t src_count;
    uint8_t endian;
    uint64_t imm;
} xair_op_rec;

typedef struct {
    xair_term_kind kind;
    xair_value_id condition;
    xair_block_id true_target;
    xair_block_id false_target;
    xair_value_id *true_args;
    size_t true_arg_count;
    xair_value_id *false_args;
    size_t false_arg_count;
    uint32_t code;
} xair_term_rec;

typedef struct {
    char name[XAIR_NAME_LEN];
    xair_value_id *params;
    size_t param_count;
    size_t param_cap;
    xair_op_id *ops;
    size_t op_count;
    size_t op_cap;
    xair_term_rec term;
} xair_block_rec;

struct xair_module {
    xair_value_rec *values;
    size_t value_count;
    size_t value_cap;
    xair_op_rec *ops;
    size_t op_count;
    size_t op_cap;
    xair_block_rec *blocks;
    size_t block_count;
    size_t block_cap;
};

typedef struct {
    char *data;
    size_t size;
    size_t used;
    int truncated;
} xair_printer;

static xair_status grow_array(void **ptr, size_t elem_size, size_t *cap, size_t need) {
    size_t next_cap;
    void *next;

    if (need <= *cap) {
        return XAIR_OK;
    }
    next_cap = *cap ? *cap : 8;
    while (next_cap < need) {
        if (next_cap > SIZE_MAX / 2) {
            return XAIR_ERR_OOM;
        }
        next_cap *= 2;
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

static void copy_name(char dst[XAIR_NAME_LEN], const char *src, const char *prefix, uint32_t id) {
    if (src != NULL && src[0] != '\0') {
        (void)snprintf(dst, XAIR_NAME_LEN, "%s", src);
    } else {
        (void)snprintf(dst, XAIR_NAME_LEN, "%s%u", prefix, id);
    }
}

static int valid_block(const xair_module *module, xair_block_id block) {
    return module != NULL && block < module->block_count;
}

static int valid_value(const xair_module *module, xair_value_id value) {
    return module != NULL && value < module->value_count;
}

static void clear_term(xair_term_rec *term) {
    if (term == NULL) {
        return;
    }
    free(term->true_args);
    free(term->false_args);
    memset(term, 0, sizeof(*term));
    term->condition = XAIR_INVALID_ID;
    term->true_target = XAIR_INVALID_ID;
    term->false_target = XAIR_INVALID_ID;
}

static xair_status copy_args(const xair_value_id *args, size_t arg_count, xair_value_id **out_args) {
    xair_value_id *copy;

    *out_args = NULL;
    if (arg_count == 0) {
        return XAIR_OK;
    }
    if (args == NULL || arg_count > SIZE_MAX / sizeof(*copy)) {
        return args == NULL ? XAIR_ERR_BAD_ARG : XAIR_ERR_OOM;
    }
    copy = (xair_value_id *)malloc(arg_count * sizeof(*copy));
    if (copy == NULL) {
        return XAIR_ERR_OOM;
    }
    memcpy(copy, args, arg_count * sizeof(*copy));
    *out_args = copy;
    return XAIR_OK;
}

static xair_status append_param(xair_block_rec *block, xair_value_id value) {
    xair_status status = grow_array(
        (void **)&block->params,
        sizeof(*block->params),
        &block->param_cap,
        block->param_count + 1);
    if (status == XAIR_OK) {
        block->params[block->param_count++] = value;
    }
    return status;
}

static xair_status append_op(xair_block_rec *block, xair_op_id op) {
    xair_status status = grow_array(
        (void **)&block->ops,
        sizeof(*block->ops),
        &block->op_cap,
        block->op_count + 1);
    if (status == XAIR_OK) {
        block->ops[block->op_count++] = op;
    }
    return status;
}

static int is_int(xair_type type) {
    return type.kind == XAIR_TYPE_INT;
}

static int is_addr(xair_type type) {
    return type.kind == XAIR_TYPE_ADDR;
}

static int is_mem(xair_type type) {
    return type.kind == XAIR_TYPE_MEM;
}

static int is_flags(xair_type type) {
    return type.kind == XAIR_TYPE_FLAGS;
}

static int is_i1(xair_type type) {
    return type.kind == XAIR_TYPE_INT && type.bits == 1;
}

static int is_value_type(xair_type type) {
    return xair_type_is_valid(type) && type.kind != XAIR_TYPE_VOID && type.kind != XAIR_TYPE_INVALID;
}

static int valid_int_bits(uint16_t bits) {
    return bits == 1 || bits == 8 || bits == 16 || bits == 32 || bits == 64 || bits == 128;
}

static int valid_addr_bits(uint16_t bits) {
    return bits == 32 || bits == 64;
}

static xair_status add_value(
    xair_module *module,
    xair_block_id block,
    xair_type type,
    const char *name,
    xair_value_id *out_value) {
    xair_status status;
    xair_value_id id;
    xair_value_rec *value;

    if (module == NULL || out_value == NULL || !valid_block(module, block) || !is_value_type(type)) {
        return XAIR_ERR_BAD_ARG;
    }
    if (module->value_count >= UINT32_MAX) {
        return XAIR_ERR_RANGE;
    }
    status = grow_array(
        (void **)&module->values,
        sizeof(*module->values),
        &module->value_cap,
        module->value_count + 1);
    if (status != XAIR_OK) {
        return status;
    }
    id = (xair_value_id)module->value_count++;
    value = &module->values[id];
    value->type = type;
    value->block = block;
    value->op = XAIR_INVALID_ID;
    copy_name(value->name, name, "v", id);
    *out_value = id;
    return XAIR_OK;
}

static xair_status add_op(
    xair_module *module,
    xair_block_id block_id,
    xair_opcode opcode,
    xair_type type,
    const xair_value_id *srcs,
    uint8_t src_count,
    uint64_t imm,
    xair_endian endian,
    const char *name,
    xair_value_id *out_value) {
    xair_status status;
    xair_value_id value_id;
    xair_op_id op_id;
    xair_op_rec *op;
    xair_block_rec *block;

    if (module == NULL || out_value == NULL || !valid_block(module, block_id) || src_count > 3) {
        return XAIR_ERR_BAD_ARG;
    }
    if (src_count != 0 && srcs == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    block = &module->blocks[block_id];
    if (block->term.kind != XAIR_TERM_NONE) {
        return XAIR_ERR_BAD_ARG;
    }
    if (module->op_count >= UINT32_MAX) {
        return XAIR_ERR_RANGE;
    }

    status = add_value(module, block_id, type, name, &value_id);
    if (status != XAIR_OK) {
        return status;
    }
    status = grow_array((void **)&module->ops, sizeof(*module->ops), &module->op_cap, module->op_count + 1);
    if (status != XAIR_OK) {
        module->value_count--;
        return status;
    }

    op_id = (xair_op_id)module->op_count++;
    op = &module->ops[op_id];
    memset(op, 0, sizeof(*op));
    op->opcode = opcode;
    op->dst = value_id;
    op->src_count = src_count;
    op->endian = (uint8_t)endian;
    op->imm = imm;
    if (src_count != 0) {
        memcpy(op->src, srcs, src_count * sizeof(*srcs));
    }
    module->values[value_id].op = op_id;

    status = append_op(block, op_id);
    if (status != XAIR_OK) {
        module->values[value_id].op = XAIR_INVALID_ID;
        module->op_count--;
        module->value_count--;
        return status;
    }
    *out_value = value_id;
    return XAIR_OK;
}

static xair_status set_error(
    xair_error *error,
    xair_block_id block,
    xair_value_id value,
    const char *fmt,
    ...) {
    va_list ap;

    if (error != NULL) {
        error->status = XAIR_ERR_VERIFY;
        error->block = block;
        error->value = value;
        va_start(ap, fmt);
        (void)vsnprintf(error->message, sizeof(error->message), fmt, ap);
        va_end(ap);
    }
    return XAIR_ERR_VERIFY;
}

xair_type xair_type_void(void) {
    xair_type type = {XAIR_TYPE_VOID, 0, 0};
    return type;
}

xair_type xair_type_i(uint16_t bits) {
    xair_type type = {XAIR_TYPE_INT, bits, 0};
    return type;
}

xair_type xair_type_addr(uint16_t bits) {
    xair_type type = {XAIR_TYPE_ADDR, bits, 0};
    return type;
}

xair_type xair_type_flags(uint16_t count) {
    xair_type type = {XAIR_TYPE_FLAGS, count, 0};
    return type;
}

xair_type xair_type_mem(uint16_t space, uint16_t addr_bits) {
    xair_type type = {XAIR_TYPE_MEM, addr_bits, space};
    return type;
}

xair_type xair_type_label8(void) {
    xair_type type = {XAIR_TYPE_LABEL8, 8, 0};
    return type;
}

xair_type xair_type_labelset(void) {
    xair_type type = {XAIR_TYPE_LABELSET, 0, 0};
    return type;
}

int xair_type_equal(xair_type lhs, xair_type rhs) {
    return lhs.kind == rhs.kind && lhs.bits == rhs.bits && lhs.aux == rhs.aux;
}

int xair_type_is_valid(xair_type type) {
    switch (type.kind) {
    case XAIR_TYPE_VOID:
        return type.bits == 0 && type.aux == 0;
    case XAIR_TYPE_INT:
        return valid_int_bits(type.bits) && type.aux == 0;
    case XAIR_TYPE_ADDR:
        return valid_addr_bits(type.bits) && type.aux == 0;
    case XAIR_TYPE_FLAGS:
        return type.bits > 0 && type.bits <= 32 && type.aux == 0;
    case XAIR_TYPE_MEM:
        return valid_addr_bits(type.bits);
    case XAIR_TYPE_LABEL8:
        return type.bits == 8 && type.aux == 0;
    case XAIR_TYPE_LABELSET:
        return type.bits == 0 && type.aux == 0;
    default:
        return 0;
    }
}

const char *xair_status_name(xair_status status) {
    switch (status) {
    case XAIR_OK:
        return "ok";
    case XAIR_ERR_OOM:
        return "out of memory";
    case XAIR_ERR_BAD_ARG:
        return "bad argument";
    case XAIR_ERR_RANGE:
        return "range error";
    case XAIR_ERR_VERIFY:
        return "verification failed";
    default:
        return "unknown status";
    }
}

const char *xair_opcode_name(xair_opcode opcode) {
    switch (opcode) {
    case XAIR_OP_CONST_U64: return "const";
    case XAIR_OP_ADD: return "add";
    case XAIR_OP_SUB: return "sub";
    case XAIR_OP_MUL: return "mul";
    case XAIR_OP_AND: return "and";
    case XAIR_OP_OR: return "or";
    case XAIR_OP_XOR: return "xor";
    case XAIR_OP_SHL: return "shl";
    case XAIR_OP_LSHR: return "lshr";
    case XAIR_OP_ASHR: return "ashr";
    case XAIR_OP_EQ: return "eq";
    case XAIR_OP_NE: return "ne";
    case XAIR_OP_ULT: return "ult";
    case XAIR_OP_ULE: return "ule";
    case XAIR_OP_SLT: return "slt";
    case XAIR_OP_SLE: return "sle";
    case XAIR_OP_ZEXT: return "zext";
    case XAIR_OP_SEXT: return "sext";
    case XAIR_OP_TRUNC: return "trunc";
    case XAIR_OP_CONCAT: return "concat";
    case XAIR_OP_ADDR_ADD: return "addr_add";
    case XAIR_OP_ADDR_SUB: return "addr_sub";
    case XAIR_OP_FLAGS_ADD: return "flags_add";
    case XAIR_OP_FLAGS_SUB: return "flags_sub";
    case XAIR_OP_FLAG_ZF: return "flag_zf";
    case XAIR_OP_FLAG_CF: return "flag_cf";
    case XAIR_OP_FLAG_OF: return "flag_of";
    case XAIR_OP_FLAG_SF: return "flag_sf";
    case XAIR_OP_LOAD: return "load";
    case XAIR_OP_STORE: return "store";
    default: return "unknown";
    }
}

xair_status xair_module_create(xair_module **out_module) {
    xair_module *module;

    if (out_module == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    module = (xair_module *)calloc(1, sizeof(*module));
    if (module == NULL) {
        return XAIR_ERR_OOM;
    }
    *out_module = module;
    return XAIR_OK;
}

void xair_module_destroy(xair_module *module) {
    size_t i;

    if (module == NULL) {
        return;
    }
    for (i = 0; i < module->block_count; ++i) {
        free(module->blocks[i].params);
        free(module->blocks[i].ops);
        clear_term(&module->blocks[i].term);
    }
    free(module->blocks);
    free(module->ops);
    free(module->values);
    free(module);
}

size_t xair_module_block_count(const xair_module *module) {
    return module == NULL ? 0 : module->block_count;
}

size_t xair_module_value_count(const xair_module *module) {
    return module == NULL ? 0 : module->value_count;
}

size_t xair_module_op_count(const xair_module *module) {
    return module == NULL ? 0 : module->op_count;
}

xair_status xair_block_create(xair_module *module, const char *name, xair_block_id *out_block) {
    xair_status status;
    xair_block_id id;
    xair_block_rec *block;

    if (module == NULL || out_block == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (module->block_count >= UINT32_MAX) {
        return XAIR_ERR_RANGE;
    }
    status = grow_array((void **)&module->blocks, sizeof(*module->blocks), &module->block_cap, module->block_count + 1);
    if (status != XAIR_OK) {
        return status;
    }
    id = (xair_block_id)module->block_count++;
    block = &module->blocks[id];
    memset(block, 0, sizeof(*block));
    copy_name(block->name, name, "bb", id);
    block->term.condition = XAIR_INVALID_ID;
    block->term.true_target = XAIR_INVALID_ID;
    block->term.false_target = XAIR_INVALID_ID;
    *out_block = id;
    return XAIR_OK;
}

xair_status xair_block_add_param(
    xair_module *module,
    xair_block_id block,
    xair_type type,
    const char *name,
    xair_value_id *out_value) {
    xair_status status;
    xair_value_id value;
    xair_block_rec *bb;

    if (module == NULL || out_value == NULL || !valid_block(module, block) || !is_value_type(type)) {
        return XAIR_ERR_BAD_ARG;
    }
    bb = &module->blocks[block];
    if (bb->op_count != 0 || bb->term.kind != XAIR_TERM_NONE) {
        return XAIR_ERR_BAD_ARG;
    }
    status = add_value(module, block, type, name, &value);
    if (status != XAIR_OK) {
        return status;
    }
    status = append_param(bb, value);
    if (status != XAIR_OK) {
        module->value_count--;
        return status;
    }
    *out_value = value;
    return XAIR_OK;
}

xair_status xair_build_const_u64(
    xair_module *module,
    xair_block_id block,
    xair_type type,
    uint64_t value,
    const char *name,
    xair_value_id *out_value) {
    if (!(type.kind == XAIR_TYPE_INT || type.kind == XAIR_TYPE_ADDR) || type.bits > 64) {
        return XAIR_ERR_BAD_ARG;
    }
    return add_op(module, block, XAIR_OP_CONST_U64, type, NULL, 0, value, XAIR_ENDIAN_LE, name, out_value);
}

xair_status xair_build_unary(
    xair_module *module,
    xair_block_id block,
    xair_opcode opcode,
    xair_type result_type,
    xair_value_id src,
    const char *name,
    xair_value_id *out_value) {
    xair_value_id srcs[1];

    if (!valid_value(module, src)) {
        return XAIR_ERR_BAD_ARG;
    }
    srcs[0] = src;
    return add_op(module, block, opcode, result_type, srcs, 1, 0, XAIR_ENDIAN_LE, name, out_value);
}

xair_status xair_build_binary(
    xair_module *module,
    xair_block_id block,
    xair_opcode opcode,
    xair_type result_type,
    xair_value_id lhs,
    xair_value_id rhs,
    const char *name,
    xair_value_id *out_value) {
    xair_value_id srcs[2];

    if (!valid_value(module, lhs) || !valid_value(module, rhs)) {
        return XAIR_ERR_BAD_ARG;
    }
    srcs[0] = lhs;
    srcs[1] = rhs;
    return add_op(module, block, opcode, result_type, srcs, 2, 0, XAIR_ENDIAN_LE, name, out_value);
}

xair_status xair_build_load(
    xair_module *module,
    xair_block_id block,
    xair_type result_type,
    xair_value_id memory,
    xair_value_id address,
    xair_endian endian,
    const char *name,
    xair_value_id *out_value) {
    xair_value_id srcs[2];

    if (!valid_value(module, memory) || !valid_value(module, address) ||
        (endian != XAIR_ENDIAN_LE && endian != XAIR_ENDIAN_BE)) {
        return XAIR_ERR_BAD_ARG;
    }
    srcs[0] = memory;
    srcs[1] = address;
    return add_op(module, block, XAIR_OP_LOAD, result_type, srcs, 2, 0, endian, name, out_value);
}

xair_status xair_build_store(
    xair_module *module,
    xair_block_id block,
    xair_value_id memory,
    xair_value_id address,
    xair_value_id data,
    xair_endian endian,
    const char *name,
    xair_value_id *out_memory) {
    xair_value_id srcs[3];

    if (!valid_value(module, memory) || !valid_value(module, address) || !valid_value(module, data) ||
        (endian != XAIR_ENDIAN_LE && endian != XAIR_ENDIAN_BE)) {
        return XAIR_ERR_BAD_ARG;
    }
    srcs[0] = memory;
    srcs[1] = address;
    srcs[2] = data;
    return add_op(module, block, XAIR_OP_STORE, module->values[memory].type, srcs, 3, 0, endian, name, out_memory);
}

static xair_status set_term(xair_module *module, xair_block_id block, xair_term_rec *term) {
    if (!valid_block(module, block) || term == NULL || module->blocks[block].term.kind != XAIR_TERM_NONE) {
        clear_term(term);
        return XAIR_ERR_BAD_ARG;
    }
    module->blocks[block].term = *term;
    return XAIR_OK;
}

xair_status xair_set_jump(
    xair_module *module,
    xair_block_id block,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count) {
    xair_term_rec term;
    xair_status status;

    if (!valid_block(module, target)) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(&term, 0, sizeof(term));
    term.kind = XAIR_TERM_JUMP;
    term.condition = XAIR_INVALID_ID;
    term.true_target = target;
    term.false_target = XAIR_INVALID_ID;
    status = copy_args(args, arg_count, &term.true_args);
    if (status != XAIR_OK) {
        return status;
    }
    term.true_arg_count = arg_count;
    return set_term(module, block, &term);
}

xair_status xair_set_cbranch(
    xair_module *module,
    xair_block_id block,
    xair_value_id condition,
    xair_block_id true_target,
    const xair_value_id *true_args,
    size_t true_arg_count,
    xair_block_id false_target,
    const xair_value_id *false_args,
    size_t false_arg_count) {
    xair_term_rec term;
    xair_status status;

    if (!valid_value(module, condition) || !valid_block(module, true_target) || !valid_block(module, false_target)) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(&term, 0, sizeof(term));
    term.kind = XAIR_TERM_CBRANCH;
    term.condition = condition;
    term.true_target = true_target;
    term.false_target = false_target;
    status = copy_args(true_args, true_arg_count, &term.true_args);
    if (status != XAIR_OK) {
        return status;
    }
    term.true_arg_count = true_arg_count;
    status = copy_args(false_args, false_arg_count, &term.false_args);
    if (status != XAIR_OK) {
        clear_term(&term);
        return status;
    }
    term.false_arg_count = false_arg_count;
    return set_term(module, block, &term);
}

xair_status xair_set_return(
    xair_module *module,
    xair_block_id block,
    const xair_value_id *values,
    size_t value_count) {
    xair_term_rec term;
    xair_status status;

    memset(&term, 0, sizeof(term));
    term.kind = XAIR_TERM_RETURN;
    term.condition = XAIR_INVALID_ID;
    term.true_target = XAIR_INVALID_ID;
    term.false_target = XAIR_INVALID_ID;
    status = copy_args(values, value_count, &term.true_args);
    if (status != XAIR_OK) {
        return status;
    }
    term.true_arg_count = value_count;
    return set_term(module, block, &term);
}

xair_status xair_set_trap(xair_module *module, xair_block_id block, uint32_t code) {
    xair_term_rec term;
    memset(&term, 0, sizeof(term));
    term.kind = XAIR_TERM_TRAP;
    term.condition = XAIR_INVALID_ID;
    term.true_target = XAIR_INVALID_ID;
    term.false_target = XAIR_INVALID_ID;
    term.code = code;
    return set_term(module, block, &term);
}

xair_status xair_set_fault(xair_module *module, xair_block_id block, uint32_t code) {
    xair_term_rec term;
    memset(&term, 0, sizeof(term));
    term.kind = XAIR_TERM_FAULT;
    term.condition = XAIR_INVALID_ID;
    term.true_target = XAIR_INVALID_ID;
    term.false_target = XAIR_INVALID_ID;
    term.code = code;
    return set_term(module, block, &term);
}

xair_type xair_value_type(const xair_module *module, xair_value_id value) {
    xair_type invalid = {XAIR_TYPE_INVALID, 0, 0};
    return valid_value(module, value) ? module->values[value].type : invalid;
}

static xair_status verify_binary_op(const xair_module *module, const xair_op_rec *op, xair_block_id block, xair_error *error) {
    xair_type out = module->values[op->dst].type;
    xair_type lhs = module->values[op->src[0]].type;
    xair_type rhs = module->values[op->src[1]].type;

    switch (op->opcode) {
    case XAIR_OP_ADD:
    case XAIR_OP_SUB:
        if (!xair_type_equal(lhs, rhs) || !(is_int(lhs) || is_addr(lhs)) || !xair_type_equal(out, lhs)) {
            return set_error(error, block, op->dst, "%s requires matching int or addr operands", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_MUL:
    case XAIR_OP_AND:
    case XAIR_OP_OR:
    case XAIR_OP_XOR:
        if (!xair_type_equal(lhs, rhs) || !is_int(lhs) || !xair_type_equal(out, lhs)) {
            return set_error(error, block, op->dst, "%s requires matching integer operands", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_SHL:
    case XAIR_OP_LSHR:
    case XAIR_OP_ASHR:
        if (!is_int(lhs) || !is_int(rhs) || !xair_type_equal(out, lhs)) {
            return set_error(error, block, op->dst, "%s requires integer operands and lhs result type", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_EQ:
    case XAIR_OP_NE:
    case XAIR_OP_ULT:
    case XAIR_OP_ULE:
        if (!xair_type_equal(lhs, rhs) || !(is_int(lhs) || is_addr(lhs)) || !is_i1(out)) {
            return set_error(error, block, op->dst, "%s requires matching int or addr operands and i1 result", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_SLT:
    case XAIR_OP_SLE:
        if (!xair_type_equal(lhs, rhs) || !is_int(lhs) || !is_i1(out)) {
            return set_error(error, block, op->dst, "%s requires matching integer operands and i1 result", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_CONCAT:
        if (!is_int(lhs) || !is_int(rhs) || !is_int(out) || out.bits != (uint16_t)(lhs.bits + rhs.bits)) {
            return set_error(error, block, op->dst, "concat result width must equal input width sum");
        }
        return XAIR_OK;
    case XAIR_OP_ADDR_ADD:
    case XAIR_OP_ADDR_SUB:
        if (!is_addr(lhs) || !is_int(rhs) || lhs.bits != rhs.bits || !xair_type_equal(out, lhs)) {
            return set_error(error, block, op->dst, "%s requires addrN, iN -> addrN", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_FLAGS_ADD:
    case XAIR_OP_FLAGS_SUB:
        if (!xair_type_equal(lhs, rhs) || !is_int(lhs) || !is_flags(out)) {
            return set_error(error, block, op->dst, "%s requires matching integer operands and flags result", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    default:
        return set_error(error, block, op->dst, "opcode is not a binary operation");
    }
}

static xair_status verify_unary_op(const xair_module *module, const xair_op_rec *op, xair_block_id block, xair_error *error) {
    xair_type out = module->values[op->dst].type;
    xair_type src = module->values[op->src[0]].type;

    switch (op->opcode) {
    case XAIR_OP_ZEXT:
    case XAIR_OP_SEXT:
        if (!is_int(src) || !is_int(out) || out.bits <= src.bits) {
            return set_error(error, block, op->dst, "%s requires widening integer result", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    case XAIR_OP_TRUNC:
        if (!is_int(src) || !is_int(out) || out.bits >= src.bits) {
            return set_error(error, block, op->dst, "trunc requires narrowing integer result");
        }
        return XAIR_OK;
    case XAIR_OP_FLAG_ZF:
    case XAIR_OP_FLAG_CF:
    case XAIR_OP_FLAG_OF:
    case XAIR_OP_FLAG_SF:
        if (!is_flags(src) || !is_i1(out)) {
            return set_error(error, block, op->dst, "%s requires flags input and i1 result", xair_opcode_name(op->opcode));
        }
        return XAIR_OK;
    default:
        return set_error(error, block, op->dst, "opcode is not a unary operation");
    }
}

static xair_status verify_memory_op(const xair_module *module, const xair_op_rec *op, xair_block_id block, xair_error *error) {
    xair_type out = module->values[op->dst].type;
    xair_type mem = module->values[op->src[0]].type;
    xair_type addr = module->values[op->src[1]].type;

    if (op->endian != XAIR_ENDIAN_LE && op->endian != XAIR_ENDIAN_BE) {
        return set_error(error, block, op->dst, "invalid memory endianness");
    }
    if (!is_mem(mem) || !is_addr(addr) || mem.bits != addr.bits) {
        return set_error(error, block, op->dst, "memory operation requires mem<space,N> and addrN");
    }
    if (op->opcode == XAIR_OP_LOAD) {
        if (!is_value_type(out) || is_mem(out)) {
            return set_error(error, block, op->dst, "load result must be a non-memory value type");
        }
        return XAIR_OK;
    }
    if (op->opcode == XAIR_OP_STORE) {
        xair_type data = module->values[op->src[2]].type;
        if (!xair_type_equal(out, mem) || !is_value_type(data) || is_mem(data)) {
            return set_error(error, block, op->dst, "store result must be the input memory type and data must be non-memory");
        }
        return XAIR_OK;
    }
    return set_error(error, block, op->dst, "opcode is not a memory operation");
}

static xair_status verify_op(const xair_module *module, const xair_op_rec *op, xair_block_id block, xair_error *error) {
    xair_type out;

    if (op->dst >= module->value_count) {
        return set_error(error, block, XAIR_INVALID_ID, "operation has invalid destination");
    }
    out = module->values[op->dst].type;
    if (!is_value_type(out)) {
        return set_error(error, block, op->dst, "operation destination has invalid type");
    }
    switch (op->opcode) {
    case XAIR_OP_CONST_U64:
        if (op->src_count != 0 || !((is_int(out) || is_addr(out)) && out.bits <= 64)) {
            return set_error(error, block, op->dst, "const requires iN/addrN result up to 64 bits");
        }
        return XAIR_OK;
    case XAIR_OP_ZEXT:
    case XAIR_OP_SEXT:
    case XAIR_OP_TRUNC:
    case XAIR_OP_FLAG_ZF:
    case XAIR_OP_FLAG_CF:
    case XAIR_OP_FLAG_OF:
    case XAIR_OP_FLAG_SF:
        if (op->src_count != 1) {
            return set_error(error, block, op->dst, "unary operation has wrong source count");
        }
        return verify_unary_op(module, op, block, error);
    case XAIR_OP_LOAD:
        if (op->src_count != 2) {
            return set_error(error, block, op->dst, "load has wrong source count");
        }
        return verify_memory_op(module, op, block, error);
    case XAIR_OP_STORE:
        if (op->src_count != 3) {
            return set_error(error, block, op->dst, "store has wrong source count");
        }
        return verify_memory_op(module, op, block, error);
    default:
        if (op->src_count != 2) {
            return set_error(error, block, op->dst, "binary operation has wrong source count");
        }
        return verify_binary_op(module, op, block, error);
    }
}

static xair_status verify_target_args(
    const xair_module *module,
    xair_block_id src_block,
    const unsigned char *available,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count,
    xair_error *error) {
    const xair_block_rec *target_block;
    size_t i;

    if (!valid_block(module, target)) {
        return set_error(error, src_block, XAIR_INVALID_ID, "terminator target is invalid");
    }
    target_block = &module->blocks[target];
    if (target_block->param_count != arg_count) {
        return set_error(error, src_block, XAIR_INVALID_ID, "terminator argument count does not match target parameters");
    }
    for (i = 0; i < arg_count; ++i) {
        xair_value_id arg = args[i];
        xair_value_id param = target_block->params[i];
        if (!valid_value(module, arg) || !available[arg]) {
            return set_error(error, src_block, arg, "terminator argument is not available in source block");
        }
        if (!xair_type_equal(module->values[arg].type, module->values[param].type)) {
            return set_error(error, src_block, arg, "terminator argument type does not match target parameter");
        }
    }
    return XAIR_OK;
}

static xair_status verify_term(
    const xair_module *module,
    xair_block_id block_id,
    const unsigned char *available,
    xair_error *error) {
    const xair_block_rec *block = &module->blocks[block_id];
    size_t i;

    switch (block->term.kind) {
    case XAIR_TERM_NONE:
        return set_error(error, block_id, XAIR_INVALID_ID, "block has no terminator");
    case XAIR_TERM_JUMP:
        return verify_target_args(module, block_id, available, block->term.true_target, block->term.true_args, block->term.true_arg_count, error);
    case XAIR_TERM_CBRANCH:
        if (!valid_value(module, block->term.condition) ||
            !available[block->term.condition] ||
            !is_i1(module->values[block->term.condition].type)) {
            return set_error(error, block_id, block->term.condition, "conditional branch requires available i1 condition");
        }
        if (verify_target_args(module, block_id, available, block->term.true_target, block->term.true_args, block->term.true_arg_count, error) != XAIR_OK) {
            return XAIR_ERR_VERIFY;
        }
        return verify_target_args(module, block_id, available, block->term.false_target, block->term.false_args, block->term.false_arg_count, error);
    case XAIR_TERM_RETURN:
        for (i = 0; i < block->term.true_arg_count; ++i) {
            xair_value_id value = block->term.true_args[i];
            if (!valid_value(module, value) || !available[value]) {
                return set_error(error, block_id, value, "return value is not available in source block");
            }
        }
        return XAIR_OK;
    case XAIR_TERM_TRAP:
    case XAIR_TERM_FAULT:
        return XAIR_OK;
    default:
        return set_error(error, block_id, XAIR_INVALID_ID, "invalid terminator kind");
    }
}

xair_status xair_verify_module(const xair_module *module, xair_error *error) {
    size_t block_id;

    if (error != NULL) {
        memset(error, 0, sizeof(*error));
        error->status = XAIR_OK;
        error->block = XAIR_INVALID_ID;
        error->value = XAIR_INVALID_ID;
    }
    if (module == NULL) {
        return set_error(error, XAIR_INVALID_ID, XAIR_INVALID_ID, "module is null");
    }
    for (block_id = 0; block_id < module->block_count; ++block_id) {
        const xair_block_rec *block = &module->blocks[block_id];
        unsigned char *available = (unsigned char *)calloc(module->value_count ? module->value_count : 1, sizeof(*available));
        size_t i;

        if (available == NULL) {
            return XAIR_ERR_OOM;
        }
        for (i = 0; i < block->param_count; ++i) {
            xair_value_id value = block->params[i];
            if (!valid_value(module, value) || module->values[value].block != block_id || !is_value_type(module->values[value].type)) {
                free(available);
                return set_error(error, (xair_block_id)block_id, value, "block parameter is invalid");
            }
            available[value] = 1;
        }
        for (i = 0; i < block->op_count; ++i) {
            xair_op_id op_id = block->ops[i];
            const xair_op_rec *op;
            size_t src_i;

            if (op_id >= module->op_count) {
                free(available);
                return set_error(error, (xair_block_id)block_id, XAIR_INVALID_ID, "block operation id is invalid");
            }
            op = &module->ops[op_id];
            if (!valid_value(module, op->dst) || module->values[op->dst].block != block_id || module->values[op->dst].op != op_id) {
                free(available);
                return set_error(error, (xair_block_id)block_id, op->dst, "operation destination is inconsistent");
            }
            for (src_i = 0; src_i < op->src_count; ++src_i) {
                xair_value_id src = op->src[src_i];
                if (!valid_value(module, src) || !available[src]) {
                    free(available);
                    return set_error(error, (xair_block_id)block_id, src, "operation source is not available in block");
                }
            }
            if (verify_op(module, op, (xair_block_id)block_id, error) != XAIR_OK) {
                free(available);
                return XAIR_ERR_VERIFY;
            }
            available[op->dst] = 1;
        }
        if (verify_term(module, (xair_block_id)block_id, available, error) != XAIR_OK) {
            free(available);
            return XAIR_ERR_VERIFY;
        }
        free(available);
    }
    return XAIR_OK;
}

static void print_append(xair_printer *printer, const char *fmt, ...) {
    va_list ap;
    int written;
    size_t left;

    if (printer->truncated || printer->size == 0) {
        return;
    }
    left = printer->size - printer->used;
    va_start(ap, fmt);
    written = vsnprintf(printer->data + printer->used, left, fmt, ap);
    va_end(ap);
    if (written < 0 || (size_t)written >= left) {
        printer->used = printer->size - 1;
        printer->data[printer->used] = '\0';
        printer->truncated = 1;
        return;
    }
    printer->used += (size_t)written;
}

static void print_type(xair_printer *printer, xair_type type) {
    switch (type.kind) {
    case XAIR_TYPE_VOID: print_append(printer, "void"); break;
    case XAIR_TYPE_INT: print_append(printer, "i%u", (unsigned)type.bits); break;
    case XAIR_TYPE_ADDR: print_append(printer, "addr%u", (unsigned)type.bits); break;
    case XAIR_TYPE_FLAGS: print_append(printer, "flags%u", (unsigned)type.bits); break;
    case XAIR_TYPE_MEM: print_append(printer, "mem<%u,%u>", (unsigned)type.aux, (unsigned)type.bits); break;
    case XAIR_TYPE_LABEL8: print_append(printer, "label8"); break;
    case XAIR_TYPE_LABELSET: print_append(printer, "labelset"); break;
    default: print_append(printer, "invalid"); break;
    }
}

static void print_value_ref(const xair_module *module, xair_printer *printer, xair_value_id value) {
    if (!valid_value(module, value)) {
        print_append(printer, "%%bad");
    } else {
        print_append(printer, "%%%s", module->values[value].name);
    }
}

static void print_block_ref(const xair_module *module, xair_printer *printer, xair_block_id block) {
    print_append(printer, "%s", valid_block(module, block) ? module->blocks[block].name : "bad_block");
}

static void print_args(const xair_module *module, xair_printer *printer, const xair_value_id *args, size_t arg_count) {
    size_t i;

    print_append(printer, "(");
    for (i = 0; i < arg_count; ++i) {
        if (i != 0) {
            print_append(printer, ", ");
        }
        print_value_ref(module, printer, args[i]);
    }
    print_append(printer, ")");
}

static void print_op(const xair_module *module, xair_printer *printer, const xair_op_rec *op) {
    xair_type dst_type = module->values[op->dst].type;

    print_append(printer, "  ");
    print_value_ref(module, printer, op->dst);
    print_append(printer, ":");
    print_type(printer, dst_type);
    print_append(printer, " = ");

    if (op->opcode == XAIR_OP_CONST_U64) {
        print_append(printer, "const.");
        print_type(printer, dst_type);
        print_append(printer, " 0x%llx\n", (unsigned long long)op->imm);
        return;
    }
    if (op->opcode == XAIR_OP_LOAD) {
        print_append(printer, "load.");
        print_type(printer, dst_type);
        print_append(printer, " %s ", op->endian == XAIR_ENDIAN_LE ? "le" : "be");
        print_value_ref(module, printer, op->src[0]);
        print_append(printer, ", ");
        print_value_ref(module, printer, op->src[1]);
        print_append(printer, "\n");
        return;
    }
    if (op->opcode == XAIR_OP_STORE) {
        print_append(printer, "store %s ", op->endian == XAIR_ENDIAN_LE ? "le" : "be");
        print_value_ref(module, printer, op->src[0]);
        print_append(printer, ", ");
        print_value_ref(module, printer, op->src[1]);
        print_append(printer, ", ");
        print_value_ref(module, printer, op->src[2]);
        print_append(printer, "\n");
        return;
    }

    print_append(printer, "%s", xair_opcode_name(op->opcode));
    if (op->src_count != 0) {
        print_append(printer, ".");
        print_type(printer, module->values[op->src[0]].type);
        print_append(printer, " ");
    }
    if (op->src_count >= 1) {
        print_value_ref(module, printer, op->src[0]);
    }
    if (op->src_count >= 2) {
        print_append(printer, ", ");
        print_value_ref(module, printer, op->src[1]);
    }
    print_append(printer, "\n");
}

static void print_term(const xair_module *module, xair_printer *printer, const xair_block_rec *block) {
    switch (block->term.kind) {
    case XAIR_TERM_JUMP:
        print_append(printer, "  jump ");
        print_block_ref(module, printer, block->term.true_target);
        print_args(module, printer, block->term.true_args, block->term.true_arg_count);
        print_append(printer, "\n");
        break;
    case XAIR_TERM_CBRANCH:
        print_append(printer, "  cbranch ");
        print_value_ref(module, printer, block->term.condition);
        print_append(printer, ", ");
        print_block_ref(module, printer, block->term.true_target);
        print_args(module, printer, block->term.true_args, block->term.true_arg_count);
        print_append(printer, ", ");
        print_block_ref(module, printer, block->term.false_target);
        print_args(module, printer, block->term.false_args, block->term.false_arg_count);
        print_append(printer, "\n");
        break;
    case XAIR_TERM_RETURN:
        print_append(printer, "  return");
        if (block->term.true_arg_count != 0) {
            print_append(printer, " ");
            print_args(module, printer, block->term.true_args, block->term.true_arg_count);
        }
        print_append(printer, "\n");
        break;
    case XAIR_TERM_TRAP:
        print_append(printer, "  trap %u\n", (unsigned)block->term.code);
        break;
    case XAIR_TERM_FAULT:
        print_append(printer, "  fault %u\n", (unsigned)block->term.code);
        break;
    default:
        print_append(printer, "  <unterminated>\n");
        break;
    }
}

xair_status xair_format_module(const xair_module *module, char *buffer, size_t buffer_size) {
    xair_printer printer;
    size_t block_i;

    if (module == NULL || buffer == NULL || buffer_size == 0) {
        return XAIR_ERR_BAD_ARG;
    }
    printer.data = buffer;
    printer.size = buffer_size;
    printer.used = 0;
    printer.truncated = 0;
    buffer[0] = '\0';

    for (block_i = 0; block_i < module->block_count; ++block_i) {
        const xair_block_rec *block = &module->blocks[block_i];
        size_t i;

        print_append(&printer, "%s(", block->name);
        for (i = 0; i < block->param_count; ++i) {
            xair_value_id param = block->params[i];
            if (i != 0) {
                print_append(&printer, ", ");
            }
            print_value_ref(module, &printer, param);
            print_append(&printer, ":");
            print_type(&printer, module->values[param].type);
        }
        print_append(&printer, "):\n");

        for (i = 0; i < block->op_count; ++i) {
            print_op(module, &printer, &module->ops[block->ops[i]]);
        }
        print_term(module, &printer, block);
        print_append(&printer, "\n");
    }

    return printer.truncated ? XAIR_ERR_RANGE : XAIR_OK;
}
