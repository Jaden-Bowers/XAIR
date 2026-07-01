#include "xair/xair.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void require_ok(xair_status status) {
    if (status != XAIR_OK) {
        fprintf(stderr, "unexpected status: %s\n", xair_status_name(status));
        assert(status == XAIR_OK);
    }
}

static void test_flags_and_branch(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_block_id taken;
    xair_block_id fall;
    xair_value_id rax;
    xair_value_id rbx;
    xair_value_id mem;
    xair_value_id sum;
    xair_value_id flags;
    xair_value_id zf;
    xair_value_id ignored;
    xair_value_id args[2];
    char text[2048];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_create(module, "taken", &taken));
    require_ok(xair_block_create(module, "fall", &fall));

    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "rax", &rax));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "rbx", &rbx));
    require_ok(xair_block_add_param(module, entry, xair_type_mem(0, 64), "m0", &mem));

    require_ok(xair_block_add_param(module, taken, xair_type_i(64), "taken_rax", &ignored));
    require_ok(xair_block_add_param(module, taken, xair_type_mem(0, 64), "taken_mem", &ignored));
    require_ok(xair_set_return(module, taken, NULL, 0));

    require_ok(xair_block_add_param(module, fall, xair_type_i(64), "fall_rax", &ignored));
    require_ok(xair_block_add_param(module, fall, xair_type_mem(0, 64), "fall_mem", &ignored));
    require_ok(xair_set_return(module, fall, NULL, 0));

    require_ok(xair_build_binary(module, entry, XAIR_OP_ADD, xair_type_i(64), rax, rbx, "sum", &sum));
    require_ok(xair_build_binary(module, entry, XAIR_OP_FLAGS_ADD, xair_type_flags(6), rax, rbx, "cc", &flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), flags, "zf", &zf));

    args[0] = sum;
    args[1] = mem;
    require_ok(xair_set_cbranch(module, entry, zf, taken, args, 2, fall, args, 2));

    require_ok(xair_verify_module(module, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    assert(strstr(text, "flags_add.i64") != NULL);
    assert(strstr(text, "cbranch") != NULL);
    xair_module_destroy(module);
}

static void test_memory_versions(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id mem0;
    xair_value_id addr;
    xair_value_id value;
    xair_value_id mem1;
    xair_value_id loaded;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_mem(0, 64), "m0", &mem0));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "addr", &addr));
    require_ok(xair_block_add_param(module, entry, xair_type_i(32), "value", &value));

    require_ok(xair_build_store(module, entry, mem0, addr, value, XAIR_ENDIAN_LE, "m1", &mem1));
    require_ok(xair_build_load(module, entry, xair_type_i(32), mem1, addr, XAIR_ENDIAN_LE, "loaded", &loaded));
    require_ok(xair_set_return(module, entry, &loaded, 1));

    require_ok(xair_verify_module(module, &error));
    xair_module_destroy(module);
}

static void test_type_mismatch_is_rejected(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id a;
    xair_value_id b;
    xair_value_id bad;
    xair_status status;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_i(32), "a", &a));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "b", &b));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADD, xair_type_i(32), a, b, "bad", &bad));
    require_ok(xair_set_return(module, entry, &bad, 1));

    status = xair_verify_module(module, &error);
    assert(status == XAIR_ERR_VERIFY);
    assert(strstr(error.message, "matching int or addr") != NULL);
    xair_module_destroy(module);
}

static void test_canonicalizes_commutative_order(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_canonicalize_stats stats;
    xair_block_id entry;
    xair_value_id b;
    xair_value_id a;
    xair_value_id sum;
    char text[1024];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "b", &b));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "a", &a));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADD, xair_type_i(64), b, a, "sum", &sum));
    require_ok(xair_set_return(module, entry, &sum, 1));

    require_ok(xair_canonicalize_module(module, &stats, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    assert(strstr(text, "add.i64 %a, %b") != NULL);
    assert(stats.operands_reordered == 1);
    xair_module_destroy(module);
}

static void test_canonicalize_folds_constants_and_is_idempotent(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_canonicalize_stats first;
    xair_canonicalize_stats second;
    xair_block_id entry;
    xair_value_id two;
    xair_value_id three;
    xair_value_id sum;
    char text1[1024];
    char text2[1024];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(32), 2, "two", &two));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(32), 3, "three", &three));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADD, xair_type_i(32), three, two, "sum", &sum));
    require_ok(xair_set_return(module, entry, &sum, 1));

    require_ok(xair_canonicalize_module(module, &first, &error));
    require_ok(xair_format_module(module, text1, sizeof(text1)));
    assert(strstr(text1, "%sum:i32 = const.i32 0x5") != NULL);
    assert(first.constants_folded == 1);
    assert(first.operations_after == 1);

    require_ok(xair_canonicalize_module(module, &second, &error));
    require_ok(xair_format_module(module, text2, sizeof(text2)));
    assert(strcmp(text1, text2) == 0);
    assert(second.constants_folded == 0);
    assert(second.dead_operations_removed == 0);
    xair_module_destroy(module);
}

static void test_canonicalize_folds_flag_extract_and_removes_flag_summary(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_canonicalize_stats stats;
    xair_block_id entry;
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id flags;
    xair_value_id zf;
    char text[1024];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(8), 0xff, "lhs", &lhs));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(8), 1, "rhs", &rhs));
    require_ok(xair_build_binary(module, entry, XAIR_OP_FLAGS_ADD, xair_type_flags(6), lhs, rhs, "flags", &flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), flags, "zf", &zf));
    require_ok(xair_set_return(module, entry, &zf, 1));

    require_ok(xair_canonicalize_module(module, &stats, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    assert(strstr(text, "%zf:i1 = const.i1 0x1") != NULL);
    assert(strstr(text, "flags_add") == NULL);
    assert(stats.dead_operations_removed == 3);
    xair_module_destroy(module);
}

static void test_canonicalize_removes_unused_store_and_measures_inflation(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_canonicalize_stats stats;
    xair_module_metrics metrics;
    xair_block_id entry;
    xair_value_id mem0;
    xair_value_id addr;
    xair_value_id value;
    xair_value_id mem1;
    double ratio;
    char text[1024];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_mem(0, 64), "m0", &mem0));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "addr", &addr));
    require_ok(xair_block_add_param(module, entry, xair_type_i(32), "value", &value));
    require_ok(xair_build_store(module, entry, mem0, addr, value, XAIR_ENDIAN_LE, "m1", &mem1));
    require_ok(xair_set_return(module, entry, &value, 1));

    require_ok(xair_canonicalize_module(module, &stats, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    assert(strstr(text, "store") == NULL);
    assert(stats.dead_operations_removed == 1);

    require_ok(xair_get_module_metrics(module, &metrics));
    assert(metrics.operations == 0);
    require_ok(xair_ops_per_instruction(module, 2, &ratio));
    assert(ratio == 0.0);
    (void)mem1;
    xair_module_destroy(module);
}

static void test_exec_flags_branch(void) {
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result result;
    xair_block_id entry;
    xair_block_id taken;
    xair_block_id fall;
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id flags;
    xair_value_id zf;
    xair_value_id one;
    xair_value_id zero;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_create(module, "taken", &taken));
    require_ok(xair_block_create(module, "fall", &fall));

    require_ok(xair_block_add_param(module, entry, xair_type_i(8), "lhs", &lhs));
    require_ok(xair_block_add_param(module, entry, xair_type_i(8), "rhs", &rhs));
    require_ok(xair_build_binary(module, entry, XAIR_OP_FLAGS_ADD, xair_type_flags(6), lhs, rhs, "flags", &flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), flags, "zf", &zf));
    require_ok(xair_set_cbranch(module, entry, zf, taken, NULL, 0, fall, NULL, 0));

    require_ok(xair_build_const_u64(module, taken, xair_type_i(32), 1, "one", &one));
    require_ok(xair_set_return(module, taken, &one, 1));
    require_ok(xair_build_const_u64(module, fall, xair_type_i(32), 0, "zero", &zero));
    require_ok(xair_set_return(module, fall, &zero, 1));

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_set_param(state, lhs, xair_exec_i(8, 0xff)));
    require_ok(xair_exec_set_param(state, rhs, xair_exec_i(8, 1)));
    require_ok(xair_exec_run(module, entry, state, 8, &result));
    assert(result.kind == XAIR_EXEC_HALTED_RETURN);
    assert(result.return_count == 1);
    assert(result.returns[0].lo == 1);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_exec_memory_roundtrip(void) {
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result result;
    xair_block_id entry;
    xair_value_id mem0;
    xair_value_id addr;
    xair_value_id value;
    xair_value_id mem1;
    xair_value_id loaded;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_mem(0, 64), "m0", &mem0));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "addr", &addr));
    require_ok(xair_block_add_param(module, entry, xair_type_i(32), "value", &value));
    require_ok(xair_build_store(module, entry, mem0, addr, value, XAIR_ENDIAN_LE, "m1", &mem1));
    require_ok(xair_build_load(module, entry, xair_type_i(32), mem1, addr, XAIR_ENDIAN_LE, "loaded", &loaded));
    require_ok(xair_set_return(module, entry, &loaded, 1));

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_set_param(state, mem0, xair_exec_mem(0, 64)));
    require_ok(xair_exec_set_param(state, addr, xair_exec_addr(64, 0x1000)));
    require_ok(xair_exec_set_param(state, value, xair_exec_i(32, 0x11223344)));
    require_ok(xair_exec_run(module, entry, state, 8, &result));
    assert(result.kind == XAIR_EXEC_HALTED_RETURN);
    assert(result.return_count == 1);
    assert(result.returns[0].lo == 0x11223344);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

static void test_exec_step_limit(void) {
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result result;
    xair_block_id entry;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_set_jump(module, entry, entry, NULL, 0));

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, entry, state, 3, &result));
    assert(result.kind == XAIR_EXEC_HALTED_STEP_LIMIT);

    xair_exec_state_destroy(state);
    xair_module_destroy(module);
}

int main(void) {
    test_flags_and_branch();
    test_memory_versions();
    test_type_mismatch_is_rejected();
    test_canonicalizes_commutative_order();
    test_canonicalize_folds_constants_and_is_idempotent();
    test_canonicalize_folds_flag_extract_and_removes_flag_summary();
    test_canonicalize_removes_unused_store_and_measures_inflation();
    test_exec_flags_branch();
    test_exec_memory_roundtrip();
    test_exec_step_limit();
    return 0;
}
