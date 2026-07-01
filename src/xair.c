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
    case XAIR_ERR_UNSUPPORTED:
        return "unsupported operation";
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
    case XAIR_OP_INT_TO_ADDR: return "int_to_addr";
    case XAIR_OP_ADDR_TO_INT: return "addr_to_int";
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
    if (bb->term.kind != XAIR_TERM_NONE) {
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
    case XAIR_OP_INT_TO_ADDR:
        if (!is_int(src) || !is_addr(out) || src.bits != out.bits) {
            return set_error(error, block, op->dst, "int_to_addr requires iN -> addrN");
        }
        return XAIR_OK;
    case XAIR_OP_ADDR_TO_INT:
        if (!is_addr(src) || !is_int(out) || src.bits != out.bits) {
            return set_error(error, block, op->dst, "addr_to_int requires addrN -> iN");
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
    case XAIR_OP_INT_TO_ADDR:
    case XAIR_OP_ADDR_TO_INT:
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

xair_status xair_get_module_metrics(const xair_module *module, xair_module_metrics *out_metrics) {
    size_t i;

    if (module == NULL || out_metrics == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(out_metrics, 0, sizeof(*out_metrics));
    out_metrics->blocks = module->block_count;
    out_metrics->values = module->value_count;
    out_metrics->operations = module->op_count;

    for (i = 0; i < module->block_count; ++i) {
        const xair_block_rec *block = &module->blocks[i];
        out_metrics->block_parameters += block->param_count;
        switch (block->term.kind) {
        case XAIR_TERM_JUMP:
        case XAIR_TERM_RETURN:
            out_metrics->terminator_arguments += block->term.true_arg_count;
            break;
        case XAIR_TERM_CBRANCH:
            out_metrics->terminator_arguments += block->term.true_arg_count + block->term.false_arg_count + 1;
            break;
        default:
            break;
        }
    }
    return XAIR_OK;
}

xair_status xair_ops_per_instruction(
    const xair_module *module,
    size_t machine_instruction_count,
    double *out_ratio) {
    if (module == NULL || out_ratio == NULL || machine_instruction_count == 0) {
        return XAIR_ERR_BAD_ARG;
    }
    *out_ratio = (double)module->op_count / (double)machine_instruction_count;
    return XAIR_OK;
}

static uint64_t mask_for_bits(uint16_t bits) {
    if (bits >= 64) {
        return UINT64_MAX;
    }
    return (UINT64_C(1) << bits) - UINT64_C(1);
}

static uint64_t sign_bit_for_bits(uint16_t bits) {
    return bits == 64 ? (UINT64_C(1) << 63) : (UINT64_C(1) << (bits - 1));
}

static uint64_t truncate_to_type(uint64_t value, xair_type type) {
    if (type.kind != XAIR_TYPE_INT && type.kind != XAIR_TYPE_ADDR) {
        return value;
    }
    return value & mask_for_bits(type.bits);
}

static int const_u64_value(const xair_module *module, xair_value_id value, uint64_t *out_value) {
    xair_op_id op_id;
    const xair_op_rec *op;
    xair_type type;

    if (!valid_value(module, value)) {
        return 0;
    }
    type = module->values[value].type;
    if ((type.kind != XAIR_TYPE_INT && type.kind != XAIR_TYPE_ADDR) || type.bits > 64) {
        return 0;
    }
    op_id = module->values[value].op;
    if (op_id == XAIR_INVALID_ID || op_id >= module->op_count) {
        return 0;
    }
    op = &module->ops[op_id];
    if (op->opcode != XAIR_OP_CONST_U64) {
        return 0;
    }
    *out_value = truncate_to_type(op->imm, type);
    return 1;
}

static int signed_less_u64(uint64_t lhs, uint64_t rhs, uint16_t bits) {
    uint64_t sign = sign_bit_for_bits(bits);
    uint64_t mask = mask_for_bits(bits);
    int lhs_negative;
    int rhs_negative;

    lhs &= mask;
    rhs &= mask;
    lhs_negative = (lhs & sign) != 0;
    rhs_negative = (rhs & sign) != 0;
    if (lhs_negative != rhs_negative) {
        return lhs_negative;
    }
    return lhs < rhs;
}

static void fold_to_const(xair_op_rec *op, uint64_t value, xair_type type) {
    op->opcode = XAIR_OP_CONST_U64;
    op->src_count = 0;
    op->src[0] = XAIR_INVALID_ID;
    op->src[1] = XAIR_INVALID_ID;
    op->src[2] = XAIR_INVALID_ID;
    op->endian = XAIR_ENDIAN_LE;
    op->imm = truncate_to_type(value, type);
}

static int compute_flag_extract(
    const xair_module *module,
    xair_opcode extract_opcode,
    xair_value_id flags_value,
    uint64_t *out_value) {
    xair_op_id flags_op_id;
    const xair_op_rec *flags_op;
    xair_type operand_type;
    uint16_t bits;
    uint64_t lhs;
    uint64_t rhs;
    uint64_t result;
    uint64_t mask;
    uint64_t sign;
    int lhs_sign;
    int rhs_sign;
    int result_sign;

    if (!valid_value(module, flags_value)) {
        return 0;
    }
    flags_op_id = module->values[flags_value].op;
    if (flags_op_id == XAIR_INVALID_ID || flags_op_id >= module->op_count) {
        return 0;
    }
    flags_op = &module->ops[flags_op_id];
    if (flags_op->opcode != XAIR_OP_FLAGS_ADD && flags_op->opcode != XAIR_OP_FLAGS_SUB) {
        return 0;
    }
    if (!const_u64_value(module, flags_op->src[0], &lhs) || !const_u64_value(module, flags_op->src[1], &rhs)) {
        return 0;
    }

    operand_type = module->values[flags_op->src[0]].type;
    if (!is_int(operand_type) || operand_type.bits > 64) {
        return 0;
    }

    bits = operand_type.bits;
    mask = mask_for_bits(bits);
    sign = sign_bit_for_bits(bits);
    lhs &= mask;
    rhs &= mask;
    result = flags_op->opcode == XAIR_OP_FLAGS_ADD ? (lhs + rhs) & mask : (lhs - rhs) & mask;

    lhs_sign = (lhs & sign) != 0;
    rhs_sign = (rhs & sign) != 0;
    result_sign = (result & sign) != 0;

    switch (extract_opcode) {
    case XAIR_OP_FLAG_ZF:
        *out_value = result == 0;
        return 1;
    case XAIR_OP_FLAG_CF:
        *out_value = flags_op->opcode == XAIR_OP_FLAGS_ADD ? result < lhs : lhs < rhs;
        return 1;
    case XAIR_OP_FLAG_OF:
        if (flags_op->opcode == XAIR_OP_FLAGS_ADD) {
            *out_value = (lhs_sign == rhs_sign) && (result_sign != lhs_sign);
        } else {
            *out_value = (lhs_sign != rhs_sign) && (result_sign != lhs_sign);
        }
        return 1;
    case XAIR_OP_FLAG_SF:
        *out_value = result_sign;
        return 1;
    default:
        return 0;
    }
}

static int fold_constant_op(xair_module *module, xair_op_rec *op) {
    xair_type out;
    xair_type lhs_type;
    uint64_t lhs;
    uint64_t rhs;
    uint64_t folded;
    uint64_t shift;

    out = module->values[op->dst].type;
    switch (op->opcode) {
    case XAIR_OP_ADD:
    case XAIR_OP_SUB:
    case XAIR_OP_MUL:
    case XAIR_OP_AND:
    case XAIR_OP_OR:
    case XAIR_OP_XOR:
        if (!is_int(out) || out.bits > 64 ||
            !const_u64_value(module, op->src[0], &lhs) ||
            !const_u64_value(module, op->src[1], &rhs)) {
            return 0;
        }
        switch (op->opcode) {
        case XAIR_OP_ADD: folded = lhs + rhs; break;
        case XAIR_OP_SUB: folded = lhs - rhs; break;
        case XAIR_OP_MUL: folded = lhs * rhs; break;
        case XAIR_OP_AND: folded = lhs & rhs; break;
        case XAIR_OP_OR: folded = lhs | rhs; break;
        default: folded = lhs ^ rhs; break;
        }
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_SHL:
    case XAIR_OP_LSHR:
    case XAIR_OP_ASHR:
        if (!is_int(out) || out.bits > 64 ||
            !const_u64_value(module, op->src[0], &lhs) ||
            !const_u64_value(module, op->src[1], &rhs)) {
            return 0;
        }
        shift = rhs;
        if (shift >= out.bits) {
            return 0;
        }
        lhs &= mask_for_bits(out.bits);
        if (op->opcode == XAIR_OP_SHL) {
            folded = lhs << shift;
        } else if (op->opcode == XAIR_OP_LSHR) {
            folded = lhs >> shift;
        } else if (shift == 0) {
            folded = lhs;
        } else if ((lhs & sign_bit_for_bits(out.bits)) != 0) {
            folded = (lhs >> shift) | (mask_for_bits(out.bits) << (out.bits - shift));
        } else {
            folded = lhs >> shift;
        }
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_EQ:
    case XAIR_OP_NE:
    case XAIR_OP_ULT:
    case XAIR_OP_ULE:
    case XAIR_OP_SLT:
    case XAIR_OP_SLE:
        if (!const_u64_value(module, op->src[0], &lhs) || !const_u64_value(module, op->src[1], &rhs)) {
            return 0;
        }
        lhs_type = module->values[op->src[0]].type;
        if (lhs_type.bits > 64) {
            return 0;
        }
        lhs &= mask_for_bits(lhs_type.bits);
        rhs &= mask_for_bits(lhs_type.bits);
        switch (op->opcode) {
        case XAIR_OP_EQ: folded = lhs == rhs; break;
        case XAIR_OP_NE: folded = lhs != rhs; break;
        case XAIR_OP_ULT: folded = lhs < rhs; break;
        case XAIR_OP_ULE: folded = lhs <= rhs; break;
        case XAIR_OP_SLT: folded = signed_less_u64(lhs, rhs, lhs_type.bits); break;
        default: folded = signed_less_u64(lhs, rhs, lhs_type.bits) || lhs == rhs; break;
        }
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_ZEXT:
    case XAIR_OP_SEXT:
    case XAIR_OP_TRUNC:
        if (!is_int(out) || out.bits > 64 || !const_u64_value(module, op->src[0], &lhs)) {
            return 0;
        }
        lhs_type = module->values[op->src[0]].type;
        if (op->opcode == XAIR_OP_SEXT && (lhs & sign_bit_for_bits(lhs_type.bits)) != 0) {
            folded = lhs | ~mask_for_bits(lhs_type.bits);
        } else {
            folded = lhs;
        }
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_INT_TO_ADDR:
    case XAIR_OP_ADDR_TO_INT:
        if (!(is_int(out) || is_addr(out)) || out.bits > 64 || !const_u64_value(module, op->src[0], &lhs)) {
            return 0;
        }
        fold_to_const(op, lhs, out);
        return 1;

    case XAIR_OP_CONCAT:
        if (!is_int(out) || out.bits > 64 ||
            !const_u64_value(module, op->src[0], &lhs) ||
            !const_u64_value(module, op->src[1], &rhs)) {
            return 0;
        }
        lhs_type = module->values[op->src[0]].type;
        folded = (lhs << module->values[op->src[1]].type.bits) | rhs;
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_ADDR_ADD:
    case XAIR_OP_ADDR_SUB:
        if (!is_addr(out) || out.bits > 64 ||
            !const_u64_value(module, op->src[0], &lhs) ||
            !const_u64_value(module, op->src[1], &rhs)) {
            return 0;
        }
        folded = op->opcode == XAIR_OP_ADDR_ADD ? lhs + rhs : lhs - rhs;
        fold_to_const(op, folded, out);
        return 1;

    case XAIR_OP_FLAG_ZF:
    case XAIR_OP_FLAG_CF:
    case XAIR_OP_FLAG_OF:
    case XAIR_OP_FLAG_SF:
        if (!compute_flag_extract(module, op->opcode, op->src[0], &folded)) {
            return 0;
        }
        fold_to_const(op, folded, out);
        return 1;

    default:
        return 0;
    }
}

static int op_is_commutative(xair_opcode opcode) {
    switch (opcode) {
    case XAIR_OP_ADD:
    case XAIR_OP_MUL:
    case XAIR_OP_AND:
    case XAIR_OP_OR:
    case XAIR_OP_XOR:
    case XAIR_OP_EQ:
    case XAIR_OP_NE:
    case XAIR_OP_FLAGS_ADD:
        return 1;
    default:
        return 0;
    }
}

static int compare_value_order(const xair_module *module, xair_value_id lhs, xair_value_id rhs) {
    uint64_t lhs_const;
    uint64_t rhs_const;
    int lhs_is_const;
    int rhs_is_const;
    xair_type lhs_type;
    xair_type rhs_type;
    int name_cmp;

    lhs_is_const = const_u64_value(module, lhs, &lhs_const);
    rhs_is_const = const_u64_value(module, rhs, &rhs_const);
    if (lhs_is_const != rhs_is_const) {
        return lhs_is_const ? 1 : -1;
    }
    lhs_type = module->values[lhs].type;
    rhs_type = module->values[rhs].type;
    if (lhs_type.kind != rhs_type.kind) {
        return lhs_type.kind < rhs_type.kind ? -1 : 1;
    }
    if (lhs_type.bits != rhs_type.bits) {
        return lhs_type.bits < rhs_type.bits ? -1 : 1;
    }
    if (lhs_type.aux != rhs_type.aux) {
        return lhs_type.aux < rhs_type.aux ? -1 : 1;
    }
    if (lhs_is_const && lhs_const != rhs_const) {
        return lhs_const < rhs_const ? -1 : 1;
    }
    name_cmp = strcmp(module->values[lhs].name, module->values[rhs].name);
    if (name_cmp != 0) {
        return name_cmp;
    }
    if (lhs == rhs) {
        return 0;
    }
    return lhs < rhs ? -1 : 1;
}

static void canonicalize_local_ops(xair_module *module, xair_canonicalize_stats *stats) {
    size_t i;

    for (i = 0; i < module->op_count; ++i) {
        xair_op_rec *op = &module->ops[i];
        if (fold_constant_op(module, op)) {
            stats->constants_folded++;
        }
        if (op->src_count == 2 && op_is_commutative(op->opcode) &&
            compare_value_order(module, op->src[0], op->src[1]) > 0) {
            xair_value_id tmp = op->src[0];
            op->src[0] = op->src[1];
            op->src[1] = tmp;
            stats->operands_reordered++;
        }
    }
}

static void mark_live_value(const xair_module *module, unsigned char *live, xair_value_id value) {
    xair_op_id op_id;
    const xair_op_rec *op;
    size_t i;

    if (!valid_value(module, value) || live[value]) {
        return;
    }
    live[value] = 1;
    op_id = module->values[value].op;
    if (op_id == XAIR_INVALID_ID || op_id >= module->op_count) {
        return;
    }
    op = &module->ops[op_id];
    for (i = 0; i < op->src_count; ++i) {
        mark_live_value(module, live, op->src[i]);
    }
}

static void mark_term_live_values(const xair_module *module, const xair_block_rec *block, unsigned char *live) {
    size_t i;

    switch (block->term.kind) {
    case XAIR_TERM_JUMP:
    case XAIR_TERM_RETURN:
        for (i = 0; i < block->term.true_arg_count; ++i) {
            mark_live_value(module, live, block->term.true_args[i]);
        }
        break;
    case XAIR_TERM_CBRANCH:
        mark_live_value(module, live, block->term.condition);
        for (i = 0; i < block->term.true_arg_count; ++i) {
            mark_live_value(module, live, block->term.true_args[i]);
        }
        for (i = 0; i < block->term.false_arg_count; ++i) {
            mark_live_value(module, live, block->term.false_args[i]);
        }
        break;
    default:
        break;
    }
}

static void remap_args(const xair_value_id *value_map, xair_value_id *args, size_t arg_count) {
    size_t i;

    for (i = 0; i < arg_count; ++i) {
        args[i] = value_map[args[i]];
    }
}

static xair_status compact_live_values(
    xair_module *module,
    const unsigned char *live,
    xair_canonicalize_stats *stats) {
    xair_value_id *value_map;
    xair_op_id *op_map;
    xair_value_rec *new_values;
    xair_op_rec *new_ops;
    size_t old_value_count = module->value_count;
    size_t old_op_count = module->op_count;
    size_t new_value_count = 0;
    size_t new_op_count = 0;
    size_t block_i;
    size_t i;

    value_map = (xair_value_id *)malloc((old_value_count ? old_value_count : 1) * sizeof(*value_map));
    op_map = (xair_op_id *)malloc((old_op_count ? old_op_count : 1) * sizeof(*op_map));
    if (value_map == NULL || op_map == NULL) {
        free(value_map);
        free(op_map);
        return XAIR_ERR_OOM;
    }
    for (i = 0; i < old_value_count; ++i) {
        value_map[i] = XAIR_INVALID_ID;
    }
    for (i = 0; i < old_op_count; ++i) {
        op_map[i] = XAIR_INVALID_ID;
    }

    for (block_i = 0; block_i < module->block_count; ++block_i) {
        const xair_block_rec *block = &module->blocks[block_i];
        for (i = 0; i < block->param_count; ++i) {
            value_map[block->params[i]] = (xair_value_id)new_value_count++;
        }
        for (i = 0; i < block->op_count; ++i) {
            xair_op_id op_id = block->ops[i];
            xair_value_id dst = module->ops[op_id].dst;
            if (live[dst]) {
                value_map[dst] = (xair_value_id)new_value_count++;
                op_map[op_id] = (xair_op_id)new_op_count++;
            }
        }
    }

    if (new_value_count == old_value_count && new_op_count == old_op_count) {
        free(value_map);
        free(op_map);
        return XAIR_OK;
    }

    new_values = (xair_value_rec *)calloc(new_value_count ? new_value_count : 1, sizeof(*new_values));
    new_ops = (xair_op_rec *)calloc(new_op_count ? new_op_count : 1, sizeof(*new_ops));
    if (new_values == NULL || new_ops == NULL) {
        free(value_map);
        free(op_map);
        free(new_values);
        free(new_ops);
        return XAIR_ERR_OOM;
    }

    for (i = 0; i < old_value_count; ++i) {
        if (value_map[i] != XAIR_INVALID_ID) {
            new_values[value_map[i]] = module->values[i];
            new_values[value_map[i]].op = XAIR_INVALID_ID;
        }
    }

    for (block_i = 0; block_i < module->block_count; ++block_i) {
        xair_block_rec *block = &module->blocks[block_i];
        size_t kept_ops = 0;

        for (i = 0; i < block->param_count; ++i) {
            block->params[i] = value_map[block->params[i]];
        }
        for (i = 0; i < block->op_count; ++i) {
            xair_op_id old_op_id = block->ops[i];
            xair_op_id new_op_id = op_map[old_op_id];
            if (new_op_id != XAIR_INVALID_ID) {
                xair_op_rec new_op = module->ops[old_op_id];
                size_t src_i;

                new_op.dst = value_map[new_op.dst];
                for (src_i = 0; src_i < new_op.src_count; ++src_i) {
                    new_op.src[src_i] = value_map[new_op.src[src_i]];
                }
                new_ops[new_op_id] = new_op;
                new_values[new_op.dst].op = new_op_id;
                block->ops[kept_ops++] = new_op_id;
            }
        }
        block->op_count = kept_ops;

        switch (block->term.kind) {
        case XAIR_TERM_JUMP:
        case XAIR_TERM_RETURN:
            remap_args(value_map, block->term.true_args, block->term.true_arg_count);
            break;
        case XAIR_TERM_CBRANCH:
            block->term.condition = value_map[block->term.condition];
            remap_args(value_map, block->term.true_args, block->term.true_arg_count);
            remap_args(value_map, block->term.false_args, block->term.false_arg_count);
            break;
        default:
            break;
        }
    }

    stats->dead_values_removed += old_value_count - new_value_count;
    stats->dead_operations_removed += old_op_count - new_op_count;

    free(module->values);
    free(module->ops);
    module->values = new_values;
    module->value_count = new_value_count;
    module->value_cap = new_value_count;
    module->ops = new_ops;
    module->op_count = new_op_count;
    module->op_cap = new_op_count;

    free(value_map);
    free(op_map);
    return XAIR_OK;
}

static xair_status eliminate_dead_values(xair_module *module, xair_canonicalize_stats *stats) {
    unsigned char *live;
    size_t block_i;
    size_t i;
    xair_status status;

    live = (unsigned char *)calloc(module->value_count ? module->value_count : 1, sizeof(*live));
    if (live == NULL) {
        return XAIR_ERR_OOM;
    }

    for (block_i = 0; block_i < module->block_count; ++block_i) {
        const xair_block_rec *block = &module->blocks[block_i];
        for (i = 0; i < block->param_count; ++i) {
            mark_live_value(module, live, block->params[i]);
        }
        mark_term_live_values(module, block, live);
    }

    status = compact_live_values(module, live, stats);
    free(live);
    return status;
}

xair_status xair_canonicalize_module(
    xair_module *module,
    xair_canonicalize_stats *out_stats,
    xair_error *error) {
    xair_canonicalize_stats stats;
    xair_status status;

    if (module == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(&stats, 0, sizeof(stats));
    stats.values_before = module->value_count;
    stats.operations_before = module->op_count;

    status = xair_verify_module(module, error);
    if (status != XAIR_OK) {
        return status;
    }

    canonicalize_local_ops(module, &stats);
    status = eliminate_dead_values(module, &stats);
    if (status != XAIR_OK) {
        return status;
    }

    status = xair_verify_module(module, error);
    if (status != XAIR_OK) {
        return status;
    }

    stats.values_after = module->value_count;
    stats.operations_after = module->op_count;
    if (out_stats != NULL) {
        *out_stats = stats;
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

#define XAIR_EXEC_PAGE_BITS 12
#define XAIR_EXEC_PAGE_SIZE ((size_t)1u << XAIR_EXEC_PAGE_BITS)
#define XAIR_EXEC_PAGE_MASK ((uint64_t)(XAIR_EXEC_PAGE_SIZE - 1u))
#define XAIR_EXEC_FLAG_ZF UINT64_C(1)
#define XAIR_EXEC_FLAG_CF UINT64_C(2)
#define XAIR_EXEC_FLAG_OF UINT64_C(4)
#define XAIR_EXEC_FLAG_SF UINT64_C(8)

typedef struct {
    uint16_t space;
    uint64_t number;
    uint8_t bytes[XAIR_EXEC_PAGE_SIZE];
    uint8_t defined[XAIR_EXEC_PAGE_SIZE];
} xair_exec_page;

struct xair_exec_state {
    const xair_module *module;
    xair_exec_value *values;
    uint8_t *defined;
    size_t value_count;
    xair_exec_page *pages;
    size_t page_count;
    size_t page_cap;
    uint64_t mem_generation;
};

static xair_exec_value exec_value_make(xair_type type, uint64_t lo, uint64_t hi) {
    xair_exec_value value;

    value.type = type;
    value.lo = truncate_to_type(lo, type);
    value.hi = hi;
    return value;
}

xair_exec_value xair_exec_i(uint16_t bits, uint64_t value) {
    return exec_value_make(xair_type_i(bits), value, 0);
}

xair_exec_value xair_exec_addr(uint16_t bits, uint64_t value) {
    return exec_value_make(xair_type_addr(bits), value, 0);
}

xair_exec_value xair_exec_mem(uint16_t space, uint16_t addr_bits) {
    return exec_value_make(xair_type_mem(space, addr_bits), 0, 0);
}

static int exec_scalar_u64_type(xair_type type) {
    return (is_int(type) || is_addr(type)) && type.bits <= 64;
}

static int exec_byte_sized_scalar(xair_type type, size_t *out_size) {
    if (!exec_scalar_u64_type(type) || type.bits == 1 || (type.bits % 8u) != 0u) {
        return 0;
    }
    *out_size = (size_t)(type.bits / 8u);
    return 1;
}

static xair_status exec_check_value_slot(const xair_exec_state *state, xair_value_id value) {
    if (state == NULL || value >= state->value_count || state->module == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (value >= state->module->value_count) {
        return XAIR_ERR_BAD_ARG;
    }
    return XAIR_OK;
}

static xair_status exec_define_value(
    xair_exec_state *state,
    xair_value_id value,
    xair_exec_value concrete) {
    xair_status status = exec_check_value_slot(state, value);
    xair_type expected;

    if (status != XAIR_OK) {
        return status;
    }
    expected = state->module->values[value].type;
    if (!xair_type_equal(expected, concrete.type)) {
        return XAIR_ERR_BAD_ARG;
    }
    state->values[value] = concrete;
    state->defined[value] = 1;
    return XAIR_OK;
}

static xair_status exec_read_value(
    const xair_exec_state *state,
    xair_value_id value,
    xair_exec_value *out_value) {
    xair_status status;

    if (out_value == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    status = exec_check_value_slot(state, value);
    if (status != XAIR_OK) {
        return status;
    }
    if (!state->defined[value]) {
        return XAIR_ERR_BAD_ARG;
    }
    *out_value = state->values[value];
    return XAIR_OK;
}

static xair_exec_page *exec_find_page_mut(xair_exec_state *state, uint16_t space, uint64_t number) {
    size_t i;

    for (i = 0; i < state->page_count; ++i) {
        xair_exec_page *page = &state->pages[i];
        if (page->space == space && page->number == number) {
            return page;
        }
    }
    return NULL;
}

static const xair_exec_page *exec_find_page_const(
    const xair_exec_state *state,
    uint16_t space,
    uint64_t number) {
    size_t i;

    for (i = 0; i < state->page_count; ++i) {
        const xair_exec_page *page = &state->pages[i];
        if (page->space == space && page->number == number) {
            return page;
        }
    }
    return NULL;
}

static xair_status exec_get_or_create_page(
    xair_exec_state *state,
    uint16_t space,
    uint64_t number,
    xair_exec_page **out_page) {
    xair_exec_page *page;
    xair_status status;

    page = exec_find_page_mut(state, space, number);
    if (page != NULL) {
        *out_page = page;
        return XAIR_OK;
    }
    status = grow_array(
        (void **)&state->pages,
        sizeof(*state->pages),
        &state->page_cap,
        state->page_count + 1);
    if (status != XAIR_OK) {
        return status;
    }
    page = &state->pages[state->page_count++];
    memset(page, 0, sizeof(*page));
    page->space = space;
    page->number = number;
    *out_page = page;
    return XAIR_OK;
}

static int exec_range_wraps(uint64_t address, size_t size) {
    return size != 0 && (uint64_t)(size - 1u) > UINT64_MAX - address;
}

xair_status xair_exec_store_bytes(
    xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    const uint8_t *data,
    size_t size) {
    size_t i;

    if (state == NULL || (data == NULL && size != 0) || exec_range_wraps(address, size)) {
        return XAIR_ERR_BAD_ARG;
    }
    for (i = 0; i < size; ++i) {
        uint64_t current = address + (uint64_t)i;
        uint64_t page_number = current >> XAIR_EXEC_PAGE_BITS;
        size_t offset = (size_t)(current & XAIR_EXEC_PAGE_MASK);
        xair_exec_page *page;
        xair_status status = exec_get_or_create_page(state, space, page_number, &page);

        if (status != XAIR_OK) {
            return status;
        }
        page->bytes[offset] = data[i];
        page->defined[offset] = 1;
    }
    return XAIR_OK;
}

xair_status xair_exec_load_bytes(
    const xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    uint8_t *out_data,
    size_t size) {
    size_t i;

    if (state == NULL || (out_data == NULL && size != 0) || exec_range_wraps(address, size)) {
        return XAIR_ERR_BAD_ARG;
    }
    for (i = 0; i < size; ++i) {
        uint64_t current = address + (uint64_t)i;
        uint64_t page_number = current >> XAIR_EXEC_PAGE_BITS;
        size_t offset = (size_t)(current & XAIR_EXEC_PAGE_MASK);
        const xair_exec_page *page = exec_find_page_const(state, space, page_number);

        if (page == NULL || !page->defined[offset]) {
            return XAIR_ERR_RANGE;
        }
        out_data[i] = page->bytes[offset];
    }
    return XAIR_OK;
}

static xair_status exec_store_u64(
    xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    uint64_t value,
    size_t size,
    xair_endian endian) {
    uint8_t bytes[8];
    size_t i;

    if (size > sizeof(bytes)) {
        return XAIR_ERR_UNSUPPORTED;
    }
    for (i = 0; i < size; ++i) {
        size_t shift_index = endian == XAIR_ENDIAN_LE ? i : (size - 1u - i);
        bytes[i] = (uint8_t)((value >> (shift_index * 8u)) & 0xffu);
    }
    return xair_exec_store_bytes(state, space, address, bytes, size);
}

static xair_status exec_load_u64(
    const xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    size_t size,
    xair_endian endian,
    uint64_t *out_value) {
    uint8_t bytes[8];
    size_t i;
    uint64_t value = 0;
    xair_status status;

    if (out_value == NULL || size > sizeof(bytes)) {
        return XAIR_ERR_BAD_ARG;
    }
    status = xair_exec_load_bytes(state, space, address, bytes, size);
    if (status != XAIR_OK) {
        return status;
    }
    for (i = 0; i < size; ++i) {
        size_t shift_index = endian == XAIR_ENDIAN_LE ? i : (size - 1u - i);
        value |= ((uint64_t)bytes[i]) << (shift_index * 8u);
    }
    *out_value = value;
    return XAIR_OK;
}

xair_status xair_exec_state_create(const xair_module *module, xair_exec_state **out_state) {
    xair_exec_state *state;
    xair_status status;

    if (module == NULL || out_state == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    status = xair_verify_module(module, NULL);
    if (status != XAIR_OK) {
        return status;
    }
    state = (xair_exec_state *)calloc(1, sizeof(*state));
    if (state == NULL) {
        return XAIR_ERR_OOM;
    }
    state->module = module;
    state->value_count = module->value_count;
    if (state->value_count != 0) {
        state->values = (xair_exec_value *)calloc(state->value_count, sizeof(*state->values));
        state->defined = (uint8_t *)calloc(state->value_count, sizeof(*state->defined));
        if (state->values == NULL || state->defined == NULL) {
            xair_exec_state_destroy(state);
            return XAIR_ERR_OOM;
        }
    }
    *out_state = state;
    return XAIR_OK;
}

void xair_exec_state_destroy(xair_exec_state *state) {
    if (state == NULL) {
        return;
    }
    free(state->values);
    free(state->defined);
    free(state->pages);
    free(state);
}

xair_status xair_exec_set_param(
    xair_exec_state *state,
    xair_value_id value,
    xair_exec_value concrete) {
    xair_status status;

    status = exec_check_value_slot(state, value);
    if (status != XAIR_OK) {
        return status;
    }
    if (state->module->values[value].op != XAIR_INVALID_ID) {
        return XAIR_ERR_BAD_ARG;
    }
    return exec_define_value(state, value, concrete);
}

xair_status xair_exec_get_value(
    const xair_exec_state *state,
    xair_value_id value,
    xair_exec_value *out_value) {
    return exec_read_value(state, value, out_value);
}

static uint64_t exec_flags(xair_opcode opcode, uint64_t lhs, uint64_t rhs, uint16_t bits) {
    uint64_t mask = mask_for_bits(bits);
    uint64_t sign = sign_bit_for_bits(bits);
    uint64_t result;
    uint64_t packed = 0;
    int lhs_sign;
    int rhs_sign;
    int result_sign;

    lhs &= mask;
    rhs &= mask;
    result = opcode == XAIR_OP_FLAGS_ADD ? (lhs + rhs) & mask : (lhs - rhs) & mask;
    lhs_sign = (lhs & sign) != 0;
    rhs_sign = (rhs & sign) != 0;
    result_sign = (result & sign) != 0;

    if (result == 0) {
        packed |= XAIR_EXEC_FLAG_ZF;
    }
    if (opcode == XAIR_OP_FLAGS_ADD ? result < lhs : lhs < rhs) {
        packed |= XAIR_EXEC_FLAG_CF;
    }
    if (opcode == XAIR_OP_FLAGS_ADD) {
        if ((lhs_sign == rhs_sign) && (result_sign != lhs_sign)) {
            packed |= XAIR_EXEC_FLAG_OF;
        }
    } else if ((lhs_sign != rhs_sign) && (result_sign != lhs_sign)) {
        packed |= XAIR_EXEC_FLAG_OF;
    }
    if (result_sign) {
        packed |= XAIR_EXEC_FLAG_SF;
    }
    return packed;
}

static xair_status exec_binary_op(
    xair_exec_state *state,
    const xair_op_rec *op,
    xair_type out,
    xair_exec_value lhs,
    xair_exec_value rhs) {
    uint64_t mask;
    uint64_t result;
    uint64_t shift;

    if (!exec_scalar_u64_type(lhs.type) || lhs.type.bits > 64 || rhs.type.bits > 64) {
        return XAIR_ERR_UNSUPPORTED;
    }
    mask = mask_for_bits(lhs.type.bits);
    lhs.lo &= mask;
    rhs.lo &= mask_for_bits(rhs.type.bits);

    switch (op->opcode) {
    case XAIR_OP_ADD:
        result = lhs.lo + rhs.lo;
        break;
    case XAIR_OP_SUB:
        result = lhs.lo - rhs.lo;
        break;
    case XAIR_OP_MUL:
        result = lhs.lo * rhs.lo;
        break;
    case XAIR_OP_AND:
        result = lhs.lo & rhs.lo;
        break;
    case XAIR_OP_OR:
        result = lhs.lo | rhs.lo;
        break;
    case XAIR_OP_XOR:
        result = lhs.lo ^ rhs.lo;
        break;
    case XAIR_OP_SHL:
        shift = rhs.lo;
        if (shift >= lhs.type.bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        result = lhs.lo << shift;
        break;
    case XAIR_OP_LSHR:
        shift = rhs.lo;
        if (shift >= lhs.type.bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        result = lhs.lo >> shift;
        break;
    case XAIR_OP_ASHR:
        shift = rhs.lo;
        if (shift >= lhs.type.bits) {
            return XAIR_ERR_UNSUPPORTED;
        }
        if (shift == 0) {
            result = lhs.lo;
        } else if ((lhs.lo & sign_bit_for_bits(lhs.type.bits)) != 0) {
            result = (lhs.lo >> shift) | (mask << (lhs.type.bits - shift));
        } else {
            result = lhs.lo >> shift;
        }
        break;
    case XAIR_OP_EQ:
        result = lhs.lo == rhs.lo;
        break;
    case XAIR_OP_NE:
        result = lhs.lo != rhs.lo;
        break;
    case XAIR_OP_ULT:
        result = lhs.lo < rhs.lo;
        break;
    case XAIR_OP_ULE:
        result = lhs.lo <= rhs.lo;
        break;
    case XAIR_OP_SLT:
        result = signed_less_u64(lhs.lo, rhs.lo, lhs.type.bits);
        break;
    case XAIR_OP_SLE:
        result = signed_less_u64(lhs.lo, rhs.lo, lhs.type.bits) || lhs.lo == rhs.lo;
        break;
    case XAIR_OP_CONCAT:
        if (!is_int(out) || out.bits > 64 || rhs.type.bits >= 64) {
            return XAIR_ERR_UNSUPPORTED;
        }
        result = (lhs.lo << rhs.type.bits) | rhs.lo;
        break;
    case XAIR_OP_ADDR_ADD:
        result = lhs.lo + rhs.lo;
        break;
    case XAIR_OP_ADDR_SUB:
        result = lhs.lo - rhs.lo;
        break;
    case XAIR_OP_FLAGS_ADD:
    case XAIR_OP_FLAGS_SUB:
        if (!is_int(lhs.type)) {
            return XAIR_ERR_UNSUPPORTED;
        }
        return exec_define_value(
            state,
            op->dst,
            exec_value_make(out, exec_flags(op->opcode, lhs.lo, rhs.lo, lhs.type.bits), 0));
    default:
        return XAIR_ERR_UNSUPPORTED;
    }
    return exec_define_value(state, op->dst, exec_value_make(out, result, 0));
}

static xair_status exec_unary_op(
    xair_exec_state *state,
    const xair_op_rec *op,
    xair_type out,
    xair_exec_value src) {
    uint64_t result;
    uint64_t flag;

    switch (op->opcode) {
    case XAIR_OP_ZEXT:
    case XAIR_OP_TRUNC:
    case XAIR_OP_INT_TO_ADDR:
    case XAIR_OP_ADDR_TO_INT:
        if (!exec_scalar_u64_type(out) || !exec_scalar_u64_type(src.type)) {
            return XAIR_ERR_UNSUPPORTED;
        }
        result = src.lo;
        break;
    case XAIR_OP_SEXT:
        if (!exec_scalar_u64_type(out) || !exec_scalar_u64_type(src.type)) {
            return XAIR_ERR_UNSUPPORTED;
        }
        if ((src.lo & sign_bit_for_bits(src.type.bits)) != 0) {
            result = src.lo | ~mask_for_bits(src.type.bits);
        } else {
            result = src.lo;
        }
        break;
    case XAIR_OP_FLAG_ZF:
        flag = XAIR_EXEC_FLAG_ZF;
        result = (src.lo & flag) != 0;
        break;
    case XAIR_OP_FLAG_CF:
        flag = XAIR_EXEC_FLAG_CF;
        result = (src.lo & flag) != 0;
        break;
    case XAIR_OP_FLAG_OF:
        flag = XAIR_EXEC_FLAG_OF;
        result = (src.lo & flag) != 0;
        break;
    case XAIR_OP_FLAG_SF:
        flag = XAIR_EXEC_FLAG_SF;
        result = (src.lo & flag) != 0;
        break;
    default:
        return XAIR_ERR_UNSUPPORTED;
    }
    return exec_define_value(state, op->dst, exec_value_make(out, result, 0));
}

static xair_status exec_memory_op(xair_exec_state *state, const xair_op_rec *op, xair_type out) {
    xair_exec_value memory;
    xair_exec_value address;
    xair_status status;
    size_t size;

    status = exec_read_value(state, op->src[0], &memory);
    if (status != XAIR_OK) {
        return status;
    }
    status = exec_read_value(state, op->src[1], &address);
    if (status != XAIR_OK) {
        return status;
    }
    if (!is_mem(memory.type) || !is_addr(address.type)) {
        return XAIR_ERR_BAD_ARG;
    }

    if (op->opcode == XAIR_OP_LOAD) {
        uint64_t loaded;

        if (!exec_byte_sized_scalar(out, &size)) {
            return XAIR_ERR_UNSUPPORTED;
        }
        status = exec_load_u64(state, memory.type.aux, address.lo, size, (xair_endian)op->endian, &loaded);
        if (status != XAIR_OK) {
            return status;
        }
        return exec_define_value(state, op->dst, exec_value_make(out, loaded, 0));
    }
    if (op->opcode == XAIR_OP_STORE) {
        xair_exec_value data;

        status = exec_read_value(state, op->src[2], &data);
        if (status != XAIR_OK) {
            return status;
        }
        if (!exec_byte_sized_scalar(data.type, &size)) {
            return XAIR_ERR_UNSUPPORTED;
        }
        status = exec_store_u64(state, memory.type.aux, address.lo, data.lo, size, (xair_endian)op->endian);
        if (status != XAIR_OK) {
            return status;
        }
        ++state->mem_generation;
        return exec_define_value(state, op->dst, exec_value_make(out, state->mem_generation, 0));
    }
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status exec_op(xair_exec_state *state, const xair_op_rec *op) {
    xair_type out = state->module->values[op->dst].type;
    xair_status status;

    if (op->opcode == XAIR_OP_CONST_U64) {
        return exec_define_value(state, op->dst, exec_value_make(out, op->imm, 0));
    }
    if (op->opcode == XAIR_OP_LOAD || op->opcode == XAIR_OP_STORE) {
        return exec_memory_op(state, op, out);
    }
    if (op->src_count == 1) {
        xair_exec_value src;

        status = exec_read_value(state, op->src[0], &src);
        if (status != XAIR_OK) {
            return status;
        }
        return exec_unary_op(state, op, out, src);
    }
    if (op->src_count == 2) {
        xair_exec_value lhs;
        xair_exec_value rhs;

        status = exec_read_value(state, op->src[0], &lhs);
        if (status != XAIR_OK) {
            return status;
        }
        status = exec_read_value(state, op->src[1], &rhs);
        if (status != XAIR_OK) {
            return status;
        }
        return exec_binary_op(state, op, out, lhs, rhs);
    }
    return XAIR_ERR_UNSUPPORTED;
}

static xair_status exec_transfer_args(
    xair_exec_state *state,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count) {
    const xair_block_rec *block;
    xair_exec_value *copies;
    size_t i;

    if (!valid_block(state->module, target)) {
        return XAIR_ERR_BAD_ARG;
    }
    block = &state->module->blocks[target];
    if (block->param_count != arg_count) {
        return XAIR_ERR_BAD_ARG;
    }
    if (arg_count == 0) {
        return XAIR_OK;
    }
    copies = (xair_exec_value *)malloc(arg_count * sizeof(*copies));
    if (copies == NULL) {
        return XAIR_ERR_OOM;
    }
    for (i = 0; i < arg_count; ++i) {
        xair_status status = exec_read_value(state, args[i], &copies[i]);
        if (status != XAIR_OK) {
            free(copies);
            return status;
        }
    }
    for (i = 0; i < arg_count; ++i) {
        xair_status status = exec_define_value(state, block->params[i], copies[i]);
        if (status != XAIR_OK) {
            free(copies);
            return status;
        }
    }
    free(copies);
    return XAIR_OK;
}

static void exec_set_result(
    xair_exec_result *result,
    xair_exec_halt_kind kind,
    xair_block_id block,
    uint32_t code) {
    if (result == NULL) {
        return;
    }
    memset(result, 0, sizeof(*result));
    result->kind = kind;
    result->block = block;
    result->code = code;
}

xair_status xair_exec_run(
    const xair_module *module,
    xair_block_id entry,
    xair_exec_state *state,
    size_t step_limit,
    xair_exec_result *out_result) {
    xair_block_id current = entry;
    size_t steps = 0;

    if (module == NULL || state == NULL || out_result == NULL || state->module != module ||
        state->value_count != module->value_count || !valid_block(module, entry)) {
        return XAIR_ERR_BAD_ARG;
    }

    for (;;) {
        const xair_block_rec *block;
        size_t i;
        xair_status status;

        if (steps >= step_limit) {
            exec_set_result(out_result, XAIR_EXEC_HALTED_STEP_LIMIT, current, 0);
            return XAIR_OK;
        }
        ++steps;

        block = &module->blocks[current];
        for (i = 0; i < block->op_count; ++i) {
            const xair_op_rec *op = &module->ops[block->ops[i]];

            status = exec_op(state, op);
            if (status == XAIR_ERR_UNSUPPORTED) {
                exec_set_result(out_result, XAIR_EXEC_HALTED_UNSUPPORTED, current, (uint32_t)op->opcode);
                return XAIR_OK;
            }
            if (status != XAIR_OK) {
                return status;
            }
        }

        switch (block->term.kind) {
        case XAIR_TERM_JUMP:
            status = exec_transfer_args(
                state,
                block->term.true_target,
                block->term.true_args,
                block->term.true_arg_count);
            if (status != XAIR_OK) {
                return status;
            }
            current = block->term.true_target;
            break;
        case XAIR_TERM_CBRANCH: {
            xair_exec_value condition;
            int take_true;

            status = exec_read_value(state, block->term.condition, &condition);
            if (status != XAIR_OK) {
                return status;
            }
            if (!is_i1(condition.type)) {
                return XAIR_ERR_BAD_ARG;
            }
            take_true = (condition.lo & 1u) != 0;
            status = exec_transfer_args(
                state,
                take_true ? block->term.true_target : block->term.false_target,
                take_true ? block->term.true_args : block->term.false_args,
                take_true ? block->term.true_arg_count : block->term.false_arg_count);
            if (status != XAIR_OK) {
                return status;
            }
            current = take_true ? block->term.true_target : block->term.false_target;
            break;
        }
        case XAIR_TERM_RETURN:
            if (block->term.true_arg_count > XAIR_EXEC_MAX_RETURNS) {
                return XAIR_ERR_RANGE;
            }
            exec_set_result(out_result, XAIR_EXEC_HALTED_RETURN, current, 0);
            out_result->return_count = block->term.true_arg_count;
            for (i = 0; i < block->term.true_arg_count; ++i) {
                status = exec_read_value(state, block->term.true_args[i], &out_result->returns[i]);
                if (status != XAIR_OK) {
                    return status;
                }
            }
            return XAIR_OK;
        case XAIR_TERM_TRAP:
            exec_set_result(out_result, XAIR_EXEC_HALTED_TRAP, current, block->term.code);
            return XAIR_OK;
        case XAIR_TERM_FAULT:
            exec_set_result(out_result, XAIR_EXEC_HALTED_FAULT, current, block->term.code);
            return XAIR_OK;
        default:
            return XAIR_ERR_BAD_ARG;
        }
    }
}
