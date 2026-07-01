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

int main(void) {
    test_flags_and_branch();
    test_memory_versions();
    test_type_mismatch_is_rejected();
    return 0;
}
