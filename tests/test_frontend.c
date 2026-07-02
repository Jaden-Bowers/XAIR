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

static xair_value_id input_reg_value(const xair_lift_result *result, xair_x86_reg reg) {
    size_t i;

    for (i = 0; i < result->input_reg_count; ++i) {
        if (result->input_regs[i].reg == reg) {
            return result->input_regs[i].value;
        }
    }
    assert(!"input register not found");
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
    options.decoder = xair_x86_default_decoder();
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_RETURN);
    assert(strcmp(xair_x86_decoder_name(options.decoder), "x86_stub") == 0);
    assert(strcmp(lift.decoder_name, "x86_stub") == 0);
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

static void test_lift_direct_jump_metadata(void) {
    static const uint8_t bytes[] = {0xeb, 0xfe};
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_error error;
    xair_module *module = NULL;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x4000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x4000;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_DIRECT_JUMP);
    assert(lift.next == 0x4002);
    assert(lift.target == 0x4000);
    require_ok(xair_verify_module(module, &error));

    xair_module_destroy(module);
}

static void test_lift_logic_immediate_branch_executes(void) {
    static const uint8_t bytes[] = {
        0x48, 0xb8, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x48, 0x83, 0xe0, 0x0f,
        0x48, 0x83, 0xf8, 0x0f,
        0x74, 0x03,
        0xc3
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    size_t condition_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x6000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x6000;
    options.max_instructions = 8;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_DIRECT_CBRANCH);
    assert(lift.branch_condition != XAIR_INVALID_ID);
    condition_index = return_index_for_value(&lift, lift.branch_condition);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, lift.block, state, 16, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.returns[condition_index].lo == 1);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_lift_stack_frame_memory_executes(void) {
    static const uint8_t bytes[] = {
        0x55,
        0x48, 0x89, 0xe5,
        0x48, 0x83, 0xec, 0x20,
        0x48, 0x8d, 0x45, 0xf0,
        0x48, 0x89, 0x45, 0xf8,
        0x48, 0x8b, 0x5d, 0xf8,
        0x48, 0x83, 0xc4, 0x20,
        0x5d,
        0xc3
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    xair_value_id mem;
    xair_value_id rsp_in;
    xair_value_id rbp_in;
    xair_value_id rax_out;
    xair_value_id rbx_out;
    xair_value_id rsp_out;
    xair_value_id rbp_out;
    size_t rax_index;
    size_t rbx_index;
    size_t rsp_index;
    size_t rbp_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x7000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x7000;
    options.max_instructions = 16;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_RETURN);
    assert(lift.memory_in != XAIR_INVALID_ID);
    assert(lift.memory_out != XAIR_INVALID_ID);

    mem = lift.memory_in;
    rsp_in = input_reg_value(&lift, XAIR_X86_RSP);
    rbp_in = input_reg_value(&lift, XAIR_X86_RBP);
    rax_out = output_reg_value(&lift, XAIR_X86_RAX);
    rbx_out = output_reg_value(&lift, XAIR_X86_RBX);
    rsp_out = output_reg_value(&lift, XAIR_X86_RSP);
    rbp_out = output_reg_value(&lift, XAIR_X86_RBP);
    rax_index = return_index_for_value(&lift, rax_out);
    rbx_index = return_index_for_value(&lift, rbx_out);
    rsp_index = return_index_for_value(&lift, rsp_out);
    rbp_index = return_index_for_value(&lift, rbp_out);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_set_param(state, mem, xair_exec_mem(0, 64)));
    require_ok(xair_exec_set_param(state, rsp_in, xair_exec_i(64, 0x8000)));
    require_ok(xair_exec_set_param(state, rbp_in, xair_exec_i(64, 0x9000)));
    require_ok(xair_exec_run(module, lift.block, state, 64, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.returns[rax_index].lo == 0x7fe8);
    assert(exec.returns[rbx_index].lo == 0x7fe8);
    assert(exec.returns[rsp_index].lo == 0x8000);
    assert(exec.returns[rbp_index].lo == 0x9000);

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

static void test_lift_non_rex64_form_is_unsupported(void) {
    static const uint8_t bytes[] = {0xb8, 0x01, 0x00, 0x00, 0x00};
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_error error;
    xair_module *module = NULL;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x5000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_64;
    options.address = 0x5000;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_UNSUPPORTED);
    assert(lift.unsupported_address == 0x5000);
    assert(lift.unsupported_opcode == 0xb8);
    require_ok(xair_verify_module(module, &error));

    xair_module_destroy(module);
}

static void test_lift_x86_32_mov_add_ret_executes(void) {
    static const uint8_t bytes[] = {
        0xb8, 0x28, 0x00, 0x00, 0x00,
        0xbb, 0x02, 0x00, 0x00, 0x00,
        0x01, 0xd8,
        0xc3
    };
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    xair_value_id eax;
    size_t eax_index;

    require_ok(xair_image_init(&image, bytes, sizeof(bytes), 0x1000));
    memset(&options, 0, sizeof(options));
    options.arch = XAIR_ARCH_X86_32;
    options.address = 0x1000;
    options.max_instructions = 8;

    require_ok(xair_module_create(&module));
    require_ok(xair_lift_basic_block(module, &image, &options, &lift));
    assert(lift.end_kind == XAIR_LIFT_END_RETURN);
    assert(lift.instructions == 4);
    assert(lift.bytes_read == sizeof(bytes));
    eax = output_reg_value(&lift, XAIR_X86_RAX);
    assert(xair_value_type(module, eax).bits == 32);
    eax_index = return_index_for_value(&lift, eax);

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, lift.block, state, 8, &exec));
    assert(exec.kind == XAIR_EXEC_HALTED_RETURN);
    assert(exec.returns[eax_index].type.bits == 32);
    assert(exec.returns[eax_index].lo == 42);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

int main(void) {
    test_lift_mov_add_ret_executes();
    test_lift_jz_metadata_and_condition();
    test_lift_rip_relative_load_executes();
    test_lift_direct_jump_metadata();
    test_lift_logic_immediate_branch_executes();
    test_lift_stack_frame_memory_executes();
    test_lift_unsupported_is_explicit();
    test_lift_non_rex64_form_is_unsupported();
    test_lift_x86_32_mov_add_ret_executes();
    return 0;
}
