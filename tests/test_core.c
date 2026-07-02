#include "xair/xair.h"
#include "xair/xair_vex.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void require_ok(xair_status status) {
    if (status != XAIR_OK) {
        fprintf(stderr, "unexpected status: %s\n", xair_status_name(status));
        assert(status == XAIR_OK);
    }
}

static void require_text_equal(const char *actual, const char *expected) {
    if (strcmp(actual, expected) != 0) {
        fprintf(stderr, "unexpected text:\n%s\nexpected:\n%s\n", actual, expected);
        assert(strcmp(actual, expected) == 0);
    }
}

static void test_ir_version_is_v0(void) {
    assert(XAIR_IR_VERSION_MAJOR == 0u);
    assert(strcmp(xair_ir_version_string(), "0.1.0") == 0);
    assert(xair_ir_version_u32() == 0x00000100u);
}

static void test_v0_golden_addr_and_flags_text(void) {
    static const char expected[] =
        "entry(%base:addr64, %delta:i64, %value:i64):\n"
        "  %addr:addr64 = addr_add.addr64 %base, %delta\n"
        "  %flags:flags6 = flags_logic.i64 %value\n"
        "  %zf:i1 = flag_zf.flags6 %flags\n"
        "  return (%addr, %zf)\n"
        "\n";
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id base;
    xair_value_id delta;
    xair_value_id value;
    xair_value_id addr;
    xair_value_id flags;
    xair_value_id zf;
    xair_value_id returns[2];
    char text[1024];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "base", &base));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "delta", &delta));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "value", &value));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADDR_ADD, xair_type_addr(64), base, delta, "addr", &addr));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAGS_LOGIC, xair_type_flags(6), value, "flags", &flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), flags, "zf", &zf));
    returns[0] = addr;
    returns[1] = zf;
    require_ok(xair_set_return(module, entry, returns, 2));

    require_ok(xair_verify_module(module, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    require_text_equal(text, expected);
    xair_module_destroy(module);
}

static void test_v0_verifier_rejects_bad_memory_token(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id not_memory;
    xair_value_id addr;
    xair_value_id loaded;
    xair_status status;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "not_memory", &not_memory));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "addr", &addr));
    require_ok(xair_build_load(module, entry, xair_type_i(32), not_memory, addr, XAIR_ENDIAN_LE, "loaded", &loaded));
    require_ok(xair_set_return(module, entry, &loaded, 1));

    status = xair_verify_module(module, &error);
    assert(status == XAIR_ERR_VERIFY);
    assert(strstr(error.message, "memory operation requires") != NULL);
    xair_module_destroy(module);
}

static void test_v0_verifier_rejects_bad_branch_condition(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_block_id taken;
    xair_block_id fall;
    xair_value_id condition;
    xair_status status;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_create(module, "taken", &taken));
    require_ok(xair_block_create(module, "fall", &fall));
    require_ok(xair_block_add_param(module, entry, xair_type_i(32), "condition", &condition));
    require_ok(xair_set_return(module, taken, NULL, 0));
    require_ok(xair_set_return(module, fall, NULL, 0));
    require_ok(xair_set_cbranch(module, entry, condition, taken, NULL, 0, fall, NULL, 0));

    status = xair_verify_module(module, &error);
    assert(status == XAIR_ERR_VERIFY);
    assert(strstr(error.message, "conditional branch requires available i1 condition") != NULL);
    xair_module_destroy(module);
}

static void test_v0_verifier_rejects_bad_flag_extract(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id not_flags;
    xair_value_id zf;
    xair_status status;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "not_flags", &not_flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), not_flags, "zf", &zf));
    require_ok(xair_set_return(module, entry, &zf, 1));

    status = xair_verify_module(module, &error);
    assert(status == XAIR_ERR_VERIFY);
    assert(strstr(error.message, "requires flags input") != NULL);
    xair_module_destroy(module);
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
    assert(strstr(error.message, "matching integer") != NULL);
    xair_module_destroy(module);
}

static void test_plain_add_rejects_addresses(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id lhs;
    xair_value_id rhs;
    xair_value_id bad;
    xair_status status;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "lhs", &lhs));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "rhs", &rhs));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADD, xair_type_addr(64), lhs, rhs, "bad", &bad));
    require_ok(xair_set_return(module, entry, &bad, 1));

    status = xair_verify_module(module, &error);
    assert(status == XAIR_ERR_VERIFY);
    assert(strstr(error.message, "matching integer") != NULL);
    xair_module_destroy(module);
}

static void test_addr_add_accepts_address_and_integer(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_block_id entry;
    xair_value_id base;
    xair_value_id delta;
    xair_value_id next;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_addr(64), "base", &base));
    require_ok(xair_block_add_param(module, entry, xair_type_i(64), "delta", &delta));
    require_ok(xair_build_binary(module, entry, XAIR_OP_ADDR_ADD, xair_type_addr(64), base, delta, "next", &next));
    require_ok(xair_set_return(module, entry, &next, 1));

    require_ok(xair_verify_module(module, &error));
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

static void test_canonicalize_folds_logic_flag_extracts(void) {
    xair_module *module = NULL;
    xair_error error;
    xair_canonicalize_stats stats;
    xair_block_id entry;
    xair_value_id zero;
    xair_value_id sign;
    xair_value_id zero_flags;
    xair_value_id sign_flags;
    xair_value_id zf;
    xair_value_id cf;
    xair_value_id of;
    xair_value_id sf;
    xair_value_id returns[4];
    char text[1536];

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(8), 0, "zero", &zero));
    require_ok(xair_build_const_u64(module, entry, xair_type_i(8), 0x80, "sign", &sign));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAGS_LOGIC, xair_type_flags(6), zero, "zero_flags", &zero_flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAGS_LOGIC, xair_type_flags(6), sign, "sign_flags", &sign_flags));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_ZF, xair_type_i(1), zero_flags, "zf", &zf));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_CF, xair_type_i(1), sign_flags, "cf", &cf));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_OF, xair_type_i(1), sign_flags, "of", &of));
    require_ok(xair_build_unary(module, entry, XAIR_OP_FLAG_SF, xair_type_i(1), sign_flags, "sf", &sf));
    returns[0] = zf;
    returns[1] = cf;
    returns[2] = of;
    returns[3] = sf;
    require_ok(xair_set_return(module, entry, returns, 4));

    require_ok(xair_canonicalize_module(module, &stats, &error));
    require_ok(xair_format_module(module, text, sizeof(text)));
    assert(strstr(text, "%zf:i1 = const.i1 0x1") != NULL);
    assert(strstr(text, "%cf:i1 = const.i1 0x0") != NULL);
    assert(strstr(text, "%of:i1 = const.i1 0x0") != NULL);
    assert(strstr(text, "%sf:i1 = const.i1 0x1") != NULL);
    assert(strstr(text, "flags_logic") == NULL);
    assert(stats.constants_folded == 4);
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

static void test_vex_adapter_register_memory_sequence(void) {
    xair_module *module = NULL;
    xair_vex_adapter *adapter = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result result;
    xair_block_id entry;
    xair_value_id mem0;
    xair_value_id reg;
    xair_value_id one;
    xair_value_id sum;
    xair_value_id addr;
    xair_value_id loaded;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_add_param(module, entry, xair_type_mem(0, 64), "m0", &mem0));
    require_ok(xair_vex_adapter_create(module, entry, mem0, &adapter));

    require_ok(xair_vex_get_reg(adapter, 16, xair_type_i(64), "r16", &reg));
    require_ok(xair_vex_emit_const(adapter, 1, xair_type_i(64), 1, "one"));
    require_ok(xair_vex_get_tmp(adapter, 1, &one));
    require_ok(xair_vex_emit_binop(adapter, 2, "Iop_Add64", reg, one, "sum"));
    require_ok(xair_vex_get_tmp(adapter, 2, &sum));
    require_ok(xair_vex_put_reg(adapter, 16, sum));

    require_ok(xair_vex_emit_const(adapter, 3, xair_type_addr(64), 0x2000, "addr"));
    require_ok(xair_vex_get_tmp(adapter, 3, &addr));
    require_ok(xair_vex_emit_store(adapter, addr, sum, XAIR_ENDIAN_LE, "m1"));
    require_ok(xair_vex_emit_load(adapter, 4, xair_type_i(64), addr, XAIR_ENDIAN_LE, "loaded"));
    require_ok(xair_vex_get_tmp(adapter, 4, &loaded));
    require_ok(xair_vex_finish_return(adapter, &loaded, 1));

    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_set_param(state, mem0, xair_exec_mem(0, 64)));
    require_ok(xair_exec_set_param(state, reg, xair_exec_i(64, 41)));
    require_ok(xair_exec_run(module, entry, state, 16, &result));
    assert(result.kind == XAIR_EXEC_HALTED_RETURN);
    assert(result.return_count == 1);
    assert(result.returns[0].lo == 42);

    xair_exec_state_destroy(state);
    xair_vex_adapter_destroy(adapter);
    xair_module_destroy(module);
}

static void test_vex_adapter_exit_continuation_carries_state(void) {
    xair_module *module = NULL;
    xair_vex_adapter *adapter = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result result;
    xair_error error;
    xair_block_id entry;
    xair_block_id side;
    xair_value_id false_value;
    xair_value_id seven;
    xair_value_id five;
    xair_value_id direct_reg;
    xair_value_id sparse_reg;
    xair_value_id tmp_sum;
    xair_value_id reg_sum;
    xair_value_id sum;
    xair_value_id side_value;

    require_ok(xair_module_create(&module));
    require_ok(xair_block_create(module, "entry", &entry));
    require_ok(xair_block_create(module, "side", &side));
    require_ok(xair_build_const_u64(module, side, xair_type_i(64), 99, "side_value", &side_value));
    require_ok(xair_set_return(module, side, &side_value, 1));

    require_ok(xair_vex_adapter_create(module, entry, XAIR_INVALID_ID, &adapter));
    require_ok(xair_vex_emit_const(adapter, 0, xair_type_i(1), 0, "cond"));
    require_ok(xair_vex_get_tmp(adapter, 0, &false_value));
    require_ok(xair_vex_emit_const(adapter, 1, xair_type_i(64), 7, "seven"));
    require_ok(xair_vex_get_tmp(adapter, 1, &seven));
    require_ok(xair_vex_put_reg(adapter, 16, seven));
    require_ok(xair_vex_put_reg(adapter, 5000, seven));
    require_ok(xair_vex_emit_exit(adapter, false_value, side, NULL, 0, "cont"));

    require_ok(xair_vex_get_tmp(adapter, 1, &seven));
    require_ok(xair_vex_peek_reg(adapter, 16, &direct_reg));
    require_ok(xair_vex_peek_reg(adapter, 5000, &sparse_reg));
    require_ok(xair_vex_emit_const(adapter, 2, xair_type_i(64), 5, "five"));
    require_ok(xair_vex_get_tmp(adapter, 2, &five));
    require_ok(xair_vex_emit_binop(adapter, 3, "Iop_Add64", seven, five, "tmp_sum"));
    require_ok(xair_vex_get_tmp(adapter, 3, &tmp_sum));
    require_ok(xair_vex_emit_binop(adapter, 4, "Iop_Add64", direct_reg, sparse_reg, "reg_sum"));
    require_ok(xair_vex_get_tmp(adapter, 4, &reg_sum));
    require_ok(xair_vex_emit_binop(adapter, 5, "Iop_Add64", tmp_sum, reg_sum, "sum"));
    require_ok(xair_vex_get_tmp(adapter, 5, &sum));
    require_ok(xair_vex_finish_return(adapter, &sum, 1));

    require_ok(xair_verify_module(module, &error));
    require_ok(xair_exec_state_create(module, &state));
    require_ok(xair_exec_run(module, entry, state, 16, &result));
    assert(result.kind == XAIR_EXEC_HALTED_RETURN);
    assert(result.return_count == 1);
    assert(result.returns[0].lo == 26);

    xair_exec_state_destroy(state);
    xair_vex_adapter_destroy(adapter);
    xair_module_destroy(module);
}

int main(void) {
    test_ir_version_is_v0();
    test_v0_golden_addr_and_flags_text();
    test_v0_verifier_rejects_bad_memory_token();
    test_v0_verifier_rejects_bad_branch_condition();
    test_v0_verifier_rejects_bad_flag_extract();
    test_flags_and_branch();
    test_memory_versions();
    test_type_mismatch_is_rejected();
    test_plain_add_rejects_addresses();
    test_addr_add_accepts_address_and_integer();
    test_canonicalizes_commutative_order();
    test_canonicalize_folds_constants_and_is_idempotent();
    test_canonicalize_folds_flag_extract_and_removes_flag_summary();
    test_canonicalize_folds_logic_flag_extracts();
    test_canonicalize_removes_unused_store_and_measures_inflation();
    test_exec_flags_branch();
    test_exec_memory_roundtrip();
    test_exec_step_limit();
    test_vex_adapter_register_memory_sequence();
    test_vex_adapter_exit_continuation_carries_state();
    return 0;
}
