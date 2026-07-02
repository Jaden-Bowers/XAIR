#include "xair/xair_frontend.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse_u64(const char *text, uint64_t *out_value) {
    char *end = NULL;
    unsigned long long value;

    if (text == NULL || out_value == NULL || text[0] == '\0') {
        return 0;
    }
    errno = 0;
    value = strtoull(text, &end, 0);
    if (errno != 0 || end == text || *end != '\0') {
        return 0;
    }
    *out_value = (uint64_t)value;
    return 1;
}

static int parse_size(const char *text, size_t *out_value) {
    uint64_t value;

    if (!parse_u64(text, &value) || value > (uint64_t)SIZE_MAX) {
        return 0;
    }
    *out_value = (size_t)value;
    return 1;
}

static int read_file(const char *path, uint8_t **out_bytes, size_t *out_size) {
    FILE *file;
    long file_size;
    uint8_t *bytes;
    size_t read_count;

    if (path == NULL || out_bytes == NULL || out_size == NULL) {
        return 0;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return 0;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 0;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return 0;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 0;
    }
    bytes = (uint8_t *)malloc((size_t)file_size == 0 ? 1u : (size_t)file_size);
    if (bytes == NULL) {
        fclose(file);
        return 0;
    }
    read_count = fread(bytes, 1, (size_t)file_size, file);
    fclose(file);
    if (read_count != (size_t)file_size) {
        free(bytes);
        return 0;
    }
    *out_bytes = bytes;
    *out_size = (size_t)file_size;
    return 1;
}

static void print_usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--json] [--arch x86_64|x86_32] <raw-binary> <base> <entry> [max-instructions]\n", argv0);
}

static int format_and_print_module(const xair_module *module) {
    size_t size = 16384;
    char *buffer;
    xair_status status;

    for (;;) {
        buffer = (char *)malloc(size);
        if (buffer == NULL) {
            return 0;
        }
        status = xair_format_module(module, buffer, size);
        if (status == XAIR_OK) {
            printf("%s", buffer);
            free(buffer);
            return 1;
        }
        free(buffer);
        if (status != XAIR_ERR_RANGE || size > 1024u * 1024u) {
            return 0;
        }
        size *= 2u;
    }
}

static void print_lift_result(const xair_lift_result *result) {
    size_t i;

    printf("lift.end=%s\n", xair_lift_end_kind_name(result->end_kind));
    printf("lift.decoder=%s\n", result->decoder_name == NULL ? "unknown" : result->decoder_name);
    printf("lift.start=0x%llx\n", (unsigned long long)result->start);
    printf("lift.next=0x%llx\n", (unsigned long long)result->next);
    printf("lift.bytes=%llu\n", (unsigned long long)result->bytes_read);
    printf("lift.instructions=%llu\n", (unsigned long long)result->instructions);
    if (result->end_kind == XAIR_LIFT_END_DIRECT_JUMP ||
        result->end_kind == XAIR_LIFT_END_DIRECT_CBRANCH) {
        printf("lift.target=0x%llx\n", (unsigned long long)result->target);
    }
    if (result->end_kind == XAIR_LIFT_END_FALLTHROUGH ||
        result->end_kind == XAIR_LIFT_END_DIRECT_CBRANCH) {
        printf("lift.fallthrough=0x%llx\n", (unsigned long long)result->fallthrough);
    }
    if (result->end_kind == XAIR_LIFT_END_UNSUPPORTED) {
        printf("lift.unsupported_address=0x%llx\n", (unsigned long long)result->unsupported_address);
        printf("lift.unsupported_opcode=0x%02x\n", (unsigned)result->unsupported_opcode);
    }
    if (result->memory_in != XAIR_INVALID_ID) {
        printf("lift.memory_in=%%%u\n", (unsigned)result->memory_in);
    }
    if (result->memory_out != XAIR_INVALID_ID) {
        printf("lift.memory_out=%%%u\n", (unsigned)result->memory_out);
    }
    for (i = 0; i < result->input_reg_count; ++i) {
        printf(
            "lift.input.%s=%%%u\n",
            xair_x86_reg_name(result->input_regs[i].reg),
            (unsigned)result->input_regs[i].value);
    }
    for (i = 0; i < result->output_reg_count; ++i) {
        printf(
            "lift.output.%s=%%%u\n",
            xair_x86_reg_name(result->output_regs[i].reg),
            (unsigned)result->output_regs[i].value);
    }
    if (result->branch_condition != XAIR_INVALID_ID) {
        printf("lift.branch_condition=%%%u\n", (unsigned)result->branch_condition);
    }
    printf("\n");
}

static void print_json_reg_values(const char *name, const xair_lift_reg_value *regs, size_t count) {
    size_t i;

    printf("  \"%s\": [", name);
    for (i = 0; i < count; ++i) {
        if (i != 0) {
            printf(", ");
        }
        printf(
            "{\"reg\":\"%s\",\"value\":%u}",
            xair_x86_reg_name(regs[i].reg),
            (unsigned)regs[i].value);
    }
    printf("],\n");
}

static void print_json_value_numbering(const char *name, const xair_value_numbering_stats *stats, int trailing_comma) {
    printf(
        "  \"%s\": {\"entries\": %llu, \"created\": %llu, \"reused\": %llu, \"collisions\": %llu}%s\n",
        name,
        (unsigned long long)stats->entries,
        (unsigned long long)stats->created,
        (unsigned long long)stats->reused,
        (unsigned long long)stats->collisions,
        trailing_comma ? "," : "");
}

static void print_json_lift_result(
    const xair_module *module,
    const xair_lift_result *result,
    xair_arch arch,
    const xair_value_numbering_stats *construction_value_numbers) {
    xair_module_metrics metrics;
    xair_value_numbering_stats final_value_numbers;
    uint64_t fingerprint = 0;

    (void)xair_get_module_metrics(module, &metrics);
    (void)xair_get_value_numbering_stats(module, &final_value_numbers);
    (void)xair_module_fingerprint(module, &fingerprint);
    printf("{\n");
    printf("  \"status\": \"ok\",\n");
    printf("  \"verified\": true,\n");
    printf("  \"ir_version\": \"%s\",\n", xair_ir_version_string());
    printf("  \"arch\": \"%s\",\n", xair_arch_name(arch));
    printf("  \"decoder\": \"%s\",\n", result->decoder_name == NULL ? "unknown" : result->decoder_name);
    printf("  \"end\": \"%s\",\n", xair_lift_end_kind_name(result->end_kind));
    printf("  \"start\": %llu,\n", (unsigned long long)result->start);
    printf("  \"next\": %llu,\n", (unsigned long long)result->next);
    printf("  \"bytes\": %llu,\n", (unsigned long long)result->bytes_read);
    printf("  \"instructions\": %llu,\n", (unsigned long long)result->instructions);
    printf("  \"target\": %llu,\n", (unsigned long long)result->target);
    printf("  \"fallthrough\": %llu,\n", (unsigned long long)result->fallthrough);
    printf("  \"unsupported_address\": %llu,\n", (unsigned long long)result->unsupported_address);
    printf("  \"unsupported_opcode\": %u,\n", (unsigned)result->unsupported_opcode);
    printf("  \"memory_in\": %d,\n", result->memory_in == XAIR_INVALID_ID ? -1 : (int)result->memory_in);
    printf("  \"memory_out\": %d,\n", result->memory_out == XAIR_INVALID_ID ? -1 : (int)result->memory_out);
    printf("  \"branch_condition\": %d,\n", result->branch_condition == XAIR_INVALID_ID ? -1 : (int)result->branch_condition);
    print_json_reg_values("input_regs", result->input_regs, result->input_reg_count);
    print_json_reg_values("output_regs", result->output_regs, result->output_reg_count);
    printf("  \"metrics\": {\"blocks\": %llu, \"values\": %llu, \"operations\": %llu, \"block_parameters\": %llu, \"terminator_arguments\": %llu},\n",
        (unsigned long long)metrics.blocks,
        (unsigned long long)metrics.values,
        (unsigned long long)metrics.operations,
        (unsigned long long)metrics.block_parameters,
        (unsigned long long)metrics.terminator_arguments);
    print_json_value_numbering("value_numbering_construction", construction_value_numbers, 1);
    print_json_value_numbering("value_numbering_final", &final_value_numbers, 1);
    printf("  \"fingerprint\": \"0x%016llx\"\n", (unsigned long long)fingerprint);
    printf("}\n");
}

int main(int argc, char **argv) {
    uint8_t *bytes = NULL;
    size_t size = 0;
    uint64_t base;
    uint64_t entry;
    size_t max_instructions = 32;
    xair_image image;
    xair_lift_options options;
    xair_lift_result result;
    xair_error error;
    xair_module *module = NULL;
    xair_value_numbering_stats construction_value_numbers;
    xair_status status;
    xair_arch arch = XAIR_ARCH_X86_64;
    int exit_code = 1;
    int json = 0;
    int argi = 1;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--json") == 0) {
            json = 1;
            ++argi;
        } else if (strcmp(argv[argi], "--arch") == 0) {
            if (argi + 1 >= argc) {
                print_usage(argv[0]);
                return 1;
            }
            if (strcmp(argv[argi + 1], "x86_64") == 0) {
                arch = XAIR_ARCH_X86_64;
            } else if (strcmp(argv[argi + 1], "x86_32") == 0 || strcmp(argv[argi + 1], "x86") == 0 ||
                strcmp(argv[argi + 1], "i386") == 0) {
                arch = XAIR_ARCH_X86_32;
            } else {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (argc - argi < 3 || argc - argi > 4) {
        print_usage(argv[0]);
        return 1;
    }
    if (!parse_u64(argv[argi + 1], &base) || !parse_u64(argv[argi + 2], &entry) ||
        (argc - argi == 4 && !parse_size(argv[argi + 3], &max_instructions))) {
        print_usage(argv[0]);
        return 1;
    }
    if (!read_file(argv[argi], &bytes, &size)) {
        fprintf(stderr, "failed to read input file\n");
        return 1;
    }

    status = xair_module_create(&module);
    if (status != XAIR_OK) {
        fprintf(stderr, "module create failed: %s\n", xair_status_name(status));
        goto done;
    }
    status = xair_image_init(&image, bytes, size, base);
    if (status != XAIR_OK) {
        fprintf(stderr, "image init failed: %s\n", xair_status_name(status));
        goto done;
    }
    memset(&options, 0, sizeof(options));
    options.arch = arch;
    options.address = entry;
    options.max_instructions = max_instructions;

    status = xair_lift_basic_block(module, &image, &options, &result);
    if (status != XAIR_OK) {
        fprintf(stderr, "lift failed: %s\n", xair_status_name(status));
        goto done;
    }
    status = xair_verify_module(module, &error);
    if (status != XAIR_OK) {
        fprintf(stderr, "verify failed: %s\n", error.message);
        goto done;
    }
    (void)xair_get_value_numbering_stats(module, &construction_value_numbers);
    status = xair_module_freeze(module);
    if (status != XAIR_OK) {
        fprintf(stderr, "freeze failed: %s\n", xair_status_name(status));
        goto done;
    }
    if (json) {
        print_json_lift_result(module, &result, arch, &construction_value_numbers);
    } else {
        print_lift_result(&result);
        if (!format_and_print_module(module)) {
            fprintf(stderr, "format failed\n");
            goto done;
        }
    }
    exit_code = 0;

done:
    xair_module_destroy(module);
    free(bytes);
    return exit_code;
}
