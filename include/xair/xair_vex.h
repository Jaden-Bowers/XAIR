#ifndef XAIR_XAIR_VEX_H
#define XAIR_XAIR_VEX_H

#include "xair/xair.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xair_vex_adapter xair_vex_adapter;

xair_status xair_vex_adapter_create(
    xair_module *module,
    xair_block_id entry,
    xair_value_id initial_memory,
    xair_vex_adapter **out_adapter);
void xair_vex_adapter_destroy(xair_vex_adapter *adapter);

xair_block_id xair_vex_current_block(const xair_vex_adapter *adapter);

xair_status xair_vex_set_tmp(xair_vex_adapter *adapter, uint32_t tmp, xair_value_id value);
xair_status xair_vex_get_tmp(
    const xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_value_id *out_value);
xair_status xair_vex_get_reg(
    xair_vex_adapter *adapter,
    uint32_t offset,
    xair_type type,
    const char *name,
    xair_value_id *out_value);
xair_status xair_vex_put_reg(xair_vex_adapter *adapter, uint32_t offset, xair_value_id value);
xair_status xair_vex_peek_reg(
    const xair_vex_adapter *adapter,
    uint32_t offset,
    xair_value_id *out_value);

xair_status xair_vex_set_memory(xair_vex_adapter *adapter, xair_value_id memory);
xair_status xair_vex_current_memory(const xair_vex_adapter *adapter, xair_value_id *out_memory);

xair_status xair_vex_emit_const(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_type type,
    uint64_t value,
    const char *name);
xair_status xair_vex_emit_unop(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    const char *irop,
    xair_value_id src,
    const char *name);
xair_status xair_vex_emit_binop(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    const char *irop,
    xair_value_id lhs,
    xair_value_id rhs,
    const char *name);
xair_status xair_vex_emit_load(
    xair_vex_adapter *adapter,
    uint32_t tmp,
    xair_type type,
    xair_value_id address,
    xair_endian endian,
    const char *name);
xair_status xair_vex_emit_store(
    xair_vex_adapter *adapter,
    xair_value_id address,
    xair_value_id data,
    xair_endian endian,
    const char *name);
xair_status xair_vex_emit_exit(
    xair_vex_adapter *adapter,
    xair_value_id condition,
    xair_block_id taken,
    const xair_value_id *taken_args,
    size_t taken_arg_count,
    const char *continuation_name);
xair_status xair_vex_finish_jump(
    xair_vex_adapter *adapter,
    xair_block_id target,
    const xair_value_id *args,
    size_t arg_count);
xair_status xair_vex_finish_return(
    xair_vex_adapter *adapter,
    const xair_value_id *values,
    size_t value_count);

#ifdef __cplusplus
}
#endif

#endif
