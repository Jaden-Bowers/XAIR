#ifndef XAIR_XAIR_H
#define XAIR_XAIR_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XAIR_INVALID_ID UINT32_MAX
#define XAIR_IR_VERSION_MAJOR 0u
#define XAIR_IR_VERSION_MINOR 1u
#define XAIR_IR_VERSION_PATCH 0u

typedef uint32_t xair_value_id;
typedef uint32_t xair_block_id;
typedef uint32_t xair_op_id;

typedef enum {
    XAIR_OK = 0,
    XAIR_ERR_OOM,
    XAIR_ERR_BAD_ARG,
    XAIR_ERR_RANGE,
    XAIR_ERR_VERIFY,
    XAIR_ERR_UNSUPPORTED
} xair_status;

typedef enum {
    XAIR_TYPE_INVALID = 0,
    XAIR_TYPE_VOID,
    XAIR_TYPE_INT,
    XAIR_TYPE_ADDR,
    XAIR_TYPE_FLAGS,
    XAIR_TYPE_MEM,
    XAIR_TYPE_LABEL8,
    XAIR_TYPE_LABELSET
} xair_type_kind;

typedef struct {
    xair_type_kind kind;
    uint16_t bits;
    uint16_t aux;
} xair_type;

typedef enum {
    XAIR_ENDIAN_LE = 0,
    XAIR_ENDIAN_BE = 1
} xair_endian;

typedef enum {
    XAIR_OP_CONST_U64 = 0,
    XAIR_OP_ADD,
    XAIR_OP_SUB,
    XAIR_OP_MUL,
    XAIR_OP_AND,
    XAIR_OP_OR,
    XAIR_OP_XOR,
    XAIR_OP_SHL,
    XAIR_OP_LSHR,
    XAIR_OP_ASHR,
    XAIR_OP_EQ,
    XAIR_OP_NE,
    XAIR_OP_ULT,
    XAIR_OP_ULE,
    XAIR_OP_SLT,
    XAIR_OP_SLE,
    XAIR_OP_ZEXT,
    XAIR_OP_SEXT,
    XAIR_OP_TRUNC,
    XAIR_OP_CONCAT,
    XAIR_OP_ADDR_ADD,
    XAIR_OP_ADDR_SUB,
    XAIR_OP_INT_TO_ADDR,
    XAIR_OP_ADDR_TO_INT,
    XAIR_OP_FLAGS_ADD,
    XAIR_OP_FLAGS_SUB,
    XAIR_OP_FLAGS_LOGIC,
    XAIR_OP_FLAGS_SHL,
    XAIR_OP_FLAG_ZF,
    XAIR_OP_FLAG_CF,
    XAIR_OP_FLAG_OF,
    XAIR_OP_FLAG_SF,
    XAIR_OP_FLAG_PF,
    XAIR_OP_FLAG_AF,
    XAIR_OP_LOAD,
    XAIR_OP_STORE
} xair_opcode;

typedef struct {
    xair_status status;
    xair_block_id block;
    xair_value_id value;
    char message[192];
} xair_error;

typedef struct {
    size_t blocks;
    size_t values;
    size_t operations;
    size_t block_parameters;
    size_t terminator_arguments;
} xair_module_metrics;

typedef struct {
    size_t values_before;
    size_t values_after;
    size_t operations_before;
    size_t operations_after;
    size_t value_number_entries_before;
    size_t value_number_entries_after;
    size_t operands_reordered;
    size_t constants_folded;
    size_t dead_values_removed;
    size_t dead_operations_removed;
} xair_canonicalize_stats;

typedef struct {
    size_t entries;
    size_t created;
    size_t reused;
    size_t collisions;
} xair_value_numbering_stats;

typedef struct xair_module xair_module;
typedef struct xair_builder xair_builder;
typedef struct xair_exec_state xair_exec_state;

#define XAIR_EXEC_MAX_RETURNS 8

typedef enum {
    XAIR_EXEC_HALTED_RETURN = 0,
    XAIR_EXEC_HALTED_TRAP,
    XAIR_EXEC_HALTED_FAULT,
    XAIR_EXEC_HALTED_STEP_LIMIT,
    XAIR_EXEC_HALTED_UNSUPPORTED
} xair_exec_halt_kind;

typedef struct {
    xair_type type;
    uint64_t lo;
    uint64_t hi;
} xair_exec_value;

typedef struct {
    xair_exec_halt_kind kind;
    xair_block_id block;
    uint32_t code;
    size_t return_count;
    xair_exec_value returns[XAIR_EXEC_MAX_RETURNS];
} xair_exec_result;

xair_type xair_type_void(void);
xair_type xair_type_i(uint16_t bits);
xair_type xair_type_addr(uint16_t bits);
xair_type xair_type_flags(uint16_t count);
xair_type xair_type_mem(uint16_t space, uint16_t addr_bits);
xair_type xair_type_label8(void);
xair_type xair_type_labelset(void);
int xair_type_equal(xair_type lhs, xair_type rhs);
int xair_type_is_valid(xair_type type);

const char *xair_status_name(xair_status status);
const char *xair_opcode_name(xair_opcode opcode);
const char *xair_ir_version_string(void);
uint32_t xair_ir_version_u32(void);

xair_status xair_module_create(xair_module **out_module);
void xair_module_destroy(xair_module *module);
xair_status xair_module_freeze(xair_module *module);
int xair_module_is_frozen(const xair_module *module);

xair_status xair_builder_create(xair_builder **out_builder);
void xair_builder_destroy(xair_builder *builder);
xair_module *xair_builder_module(xair_builder *builder);
xair_status xair_builder_freeze(xair_builder *builder, xair_module **out_module);

size_t xair_module_block_count(const xair_module *module);
size_t xair_module_value_count(const xair_module *module);
size_t xair_module_op_count(const xair_module *module);

xair_status xair_block_create(
    xair_module *module,
    const char *name,
    xair_block_id *out_block);

xair_status xair_block_add_param(
    xair_module *module,
    xair_block_id block,
    xair_type type,
    const char *name,
    xair_value_id *out_value);

xair_status xair_build_const_u64(
    xair_module *module,
    xair_block_id block,
    xair_type type,
    uint64_t value,
    const char *name,
    xair_value_id *out_value);

xair_status xair_build_unary(
    xair_module *module,
    xair_block_id block,
    xair_opcode opcode,
    xair_type result_type,
    xair_value_id src,
    const char *name,
    xair_value_id *out_value);

xair_status xair_build_binary(
    xair_module *module,
    xair_block_id block,
    xair_opcode opcode,
    xair_type result_type,
    xair_value_id lhs,
    xair_value_id rhs,
    const char *name,
    xair_value_id *out_value);

xair_status xair_build_load(
    xair_module *module,
    xair_block_id block,
    xair_type result_type,
    xair_value_id memory,
    xair_value_id address,
    xair_endian endian,
    const char *name,
    xair_value_id *out_value);

xair_status xair_build_store(
    xair_module *module,
    xair_block_id block,
    xair_value_id memory,
    xair_value_id address,
    xair_value_id data,
    xair_endian endian,
    const char *name,
    xair_value_id *out_memory);

xair_status xair_set_jump(
    xair_module *module,
    xair_block_id block,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count);

xair_status xair_set_cbranch(
    xair_module *module,
    xair_block_id block,
    xair_value_id condition,
    xair_block_id true_target,
    const xair_value_id *true_args,
    size_t true_arg_count,
    xair_block_id false_target,
    const xair_value_id *false_args,
    size_t false_arg_count);

xair_status xair_set_return(
    xair_module *module,
    xair_block_id block,
    const xair_value_id *values,
    size_t value_count);

xair_status xair_set_trap(xair_module *module, xair_block_id block, uint32_t code);
xair_status xair_set_fault(xair_module *module, xair_block_id block, uint32_t code);

xair_type xair_value_type(const xair_module *module, xair_value_id value);

xair_status xair_get_module_metrics(const xair_module *module, xair_module_metrics *out_metrics);
xair_status xair_get_value_numbering_stats(
    const xair_module *module,
    xair_value_numbering_stats *out_stats);
xair_status xair_module_fingerprint(const xair_module *module, uint64_t *out_hash);
xair_status xair_ops_per_instruction(
    const xair_module *module,
    size_t machine_instruction_count,
    double *out_ratio);
xair_status xair_canonicalize_module(
    xair_module *module,
    xair_canonicalize_stats *out_stats,
    xair_error *error);

xair_status xair_verify_module(const xair_module *module, xair_error *error);
xair_status xair_format_module(const xair_module *module, char *buffer, size_t buffer_size);

xair_exec_value xair_exec_i(uint16_t bits, uint64_t value);
xair_exec_value xair_exec_addr(uint16_t bits, uint64_t value);
xair_exec_value xair_exec_mem(uint16_t space, uint16_t addr_bits);

xair_status xair_exec_state_create(const xair_module *module, xair_exec_state **out_state);
void xair_exec_state_destroy(xair_exec_state *state);
xair_status xair_exec_set_param(
    xair_exec_state *state,
    xair_value_id value,
    xair_exec_value concrete);
xair_status xair_exec_get_value(
    const xair_exec_state *state,
    xair_value_id value,
    xair_exec_value *out_value);
xair_status xair_exec_store_bytes(
    xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    const uint8_t *data,
    size_t size);
xair_status xair_exec_load_bytes(
    const xair_exec_state *state,
    uint16_t space,
    uint64_t address,
    uint8_t *out_data,
    size_t size);
xair_status xair_exec_run(
    const xair_module *module,
    xair_block_id entry,
    xair_exec_state *state,
    size_t step_limit,
    xair_exec_result *out_result);

#ifdef __cplusplus
}
#endif

#endif
