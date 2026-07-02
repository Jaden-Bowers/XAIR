#ifndef XAIR_XAIR_FRONTEND_H
#define XAIR_XAIR_FRONTEND_H

#include "xair/xair.h"

#ifdef __cplusplus
extern "C" {
#endif

#define XAIR_LIFT_MAX_REG_OUTPUTS 16
#define XAIR_LIFT_MAX_RETURNS 24

typedef enum {
    XAIR_ARCH_X86_32 = 1,
    XAIR_ARCH_X86_64 = 2
} xair_arch;

typedef enum {
    XAIR_LIFT_END_FALLTHROUGH = 0,
    XAIR_LIFT_END_DIRECT_JUMP,
    XAIR_LIFT_END_DIRECT_CBRANCH,
    XAIR_LIFT_END_RETURN,
    XAIR_LIFT_END_UNSUPPORTED
} xair_lift_end_kind;

typedef enum {
    XAIR_X86_RAX = 0,
    XAIR_X86_RCX,
    XAIR_X86_RDX,
    XAIR_X86_RBX,
    XAIR_X86_RSP,
    XAIR_X86_RBP,
    XAIR_X86_RSI,
    XAIR_X86_RDI,
    XAIR_X86_R8,
    XAIR_X86_R9,
    XAIR_X86_R10,
    XAIR_X86_R11,
    XAIR_X86_R12,
    XAIR_X86_R13,
    XAIR_X86_R14,
    XAIR_X86_R15,
    XAIR_X86_REG_COUNT
} xair_x86_reg;

typedef struct {
    const uint8_t *bytes;
    size_t size;
    uint64_t base;
} xair_image;

typedef struct xair_x86_decoder_backend xair_x86_decoder_backend;

typedef struct {
    xair_arch arch;
    uint64_t address;
    uint16_t memory_space;
    size_t max_instructions;
    const char *block_name;
    const xair_x86_decoder_backend *decoder;
} xair_lift_options;

typedef struct {
    xair_x86_reg reg;
    xair_value_id value;
} xair_lift_reg_value;

typedef struct {
    xair_lift_end_kind end_kind;
    xair_block_id block;
    uint64_t start;
    uint64_t next;
    uint64_t target;
    uint64_t fallthrough;
    uint64_t unsupported_address;
    uint8_t unsupported_opcode;
    const char *decoder_name;
    size_t bytes_read;
    size_t instructions;
    xair_value_id memory_in;
    xair_value_id memory_out;
    xair_value_id branch_condition;
    size_t input_reg_count;
    xair_lift_reg_value input_regs[XAIR_LIFT_MAX_REG_OUTPUTS];
    size_t output_reg_count;
    xair_lift_reg_value output_regs[XAIR_LIFT_MAX_REG_OUTPUTS];
    size_t return_count;
    xair_value_id return_values[XAIR_LIFT_MAX_RETURNS];
} xair_lift_result;

xair_status xair_image_init(
    xair_image *image,
    const uint8_t *bytes,
    size_t size,
    uint64_t base);

const char *xair_arch_name(xair_arch arch);
const char *xair_lift_end_kind_name(xair_lift_end_kind kind);
const char *xair_x86_reg_name(xair_x86_reg reg);
const xair_x86_decoder_backend *xair_x86_default_decoder(void);
const char *xair_x86_decoder_name(const xair_x86_decoder_backend *decoder);

xair_status xair_lift_basic_block(
    xair_module *module,
    const xair_image *image,
    const xair_lift_options *options,
    xair_lift_result *out_result);

#ifdef __cplusplus
}
#endif

#endif
