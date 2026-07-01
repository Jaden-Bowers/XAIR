#include "xair/xair_frontend.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void require_ok(xair_status status) {
    if (status != XAIR_OK) {
        fprintf(stderr, "unexpected status: %s\n", xair_status_name(status));
        assert(status == XAIR_OK);
    }
}

static size_t return_index_for_value(const xair_lift_result *result, xair_value_id value) {
    size_t i;

    for (i = 0; i < result->return_count; ++i) {
        if (result->return_values[i] == value) {
            return i;
        }
    }
    assert(!"return value not found");
    return 0;
}

static xair_value_id output_reg_value(const xair_lift_result *result, xair_x86_reg reg) {
    size_t i;

    for (i = 0; i < result->output_reg_count; ++i) {
        if (result->output_regs[i].reg == reg) {
            return result->output_regs[i].value;
        }
    }
    assert(!"output register not found");
    return XAIR_INVALID_ID;
}

static void test_lift_mov_add_ret_executes(void) {
    static const uint8_t bytes[] = {
        0x48, 0xb8, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0xbb, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x01, 0xd8,
        0xc3
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    xair_value_id rax;
    size_t rax_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x1000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x1000;
    options.max_instructions = 8;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_RETURN);
    assert(lift.instructions == 4);
    assert(lift.bytes_read == sizeof(bytes));
    rax = output_reg_value(&lift, XAIR_X86_RAX);
    rax_index = return_index_for_value(&lift, rax);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, lift.block, state, 8, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.return_count == lift.return_count);
    assert(exec.returns[rax_index].lo == 42);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_lift_jz_metadata_and_condition(void) {
    static const uint8_t bytes[] = {
        0x48, 0xb8, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x85, 0xc0,
        0x74, 0x05,
        0xc3
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    size_t condition_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x1000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x1000;
    options.max_instructions = 8;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_DIRECT_CBRANCH);
    assert(lift.fallthrough == 0x100f);
    assert(lift.target == 0x1014);
    assert(lift.branch_condition != XAIR_INVALID_ID);
    condition_index = return_index_for_value(&lift, lift.branch_condition);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, lift.block, state, 8, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.returns[condition_index].lo == 1);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_lift_rip_relative_load_executes(void) {
    static const uint8_t bytes[] = {
        0x48, 0x8b, 0x05, 0x00, 0x00, 0x00, 0x00,
        0xc3
    };
    static const uint8_t memory_value[] = {
        0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    xair_value_id rax;
    size_t rax_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x2000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x2000;
    options.max_instructions = 4;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_RETURN);
    assert(lift.memory_in != XAIR_INVALID_ID);
    rax = output_reg_value(&lift, XAIR_X86_RAX);
    rax_index = return_index_for_value(&lift, rax);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_set_param(state, lift.memory_in, xair_exec_mem(0, 64)));
    require_ok(xair_exec_store_bytes(state, 0, 0x2007, memory_value, sizeof(memory_value)));
    require_ok(xair_exec_run(module, lift.block, state, 8, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.returns[rax_index].lo == 0x1122334455667788ULL);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_lift_unsupported_is_explicit(void) {
    static const uint8_t bytes[] = {0x0f, 0xa2};
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_error error;
    xair_module *module = NULL;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x3000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x3000;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_UNSUPPORTED);
    assert(lift.unsupported_address == 0x3000);
    assert(lift.unsupported_opcode == 0x0f);
    require_ok(xair_verify_module(module, &error));

    xair_module_destroy(module);
}

int main(void) {
    test_lift_mov_add_ret_executes();
    test_lift_jz_metadata_and_condition();
    test_lift_rip_relative_load_executes();
    test_lift_unsupported_is_explicit();
    return 0;
}
