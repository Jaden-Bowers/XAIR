#include "xair/xair_binary.h"

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

static void print_usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--json] <pe-or-elf> [entry-va] [max-instructions]\n", argv0);
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

static void print_json_lift(
    const xair_binary_image *binary,
    const xair_lift_result *result,
    const xair_module_metrics *metrics,
    uint64_t fingerprint) {
    printf("{\n");
    printf("  \"status\": \"ok\",\n");
    printf("  \"format\": \"%s\",\n", xair_binary_format_name(binary->format));
    printf("  \"arch\": \"%s\",\n", xair_arch_name(binary->arch));
    printf("  \"entry\": %llu,\n", (unsigned long long)binary->entry);
    printf("  \"segment_base\": %llu,\n", (unsigned long long)binary->segment_base);
    printf("  \"segment_size\": %llu,\n", (unsigned long long)binary->segment_size);
    printf("  \"ir_version\": \"%s\",\n", xair_ir_version_string());
    printf("  \"decoder\": \"%s\",\n", result->decoder_name == NULL ? "unknown" : result->decoder_name);
    printf("  \"end\": \"%s\",\n", xair_lift_end_kind_name(result->end_kind));
    printf("  \"start\": %llu,\n", (unsigned long long)result->start);
    printf("  \"next\": %llu,\n", (unsigned long long)result->next);
    printf("  \"bytes\": %llu,\n", (unsigned long long)result->bytes_read);
    printf("  \"instructions\": %llu,\n", (unsigned long long)result->instructions);
    printf("  \"unsupported_address\": %llu,\n", (unsigned long long)result->unsupported_address);
    printf("  \"unsupported_opcode\": %u,\n", (unsigned)result->unsupported_opcode);
    printf("  \"metrics\": {\"blocks\": %llu, \"values\": %llu, \"operations\": %llu, \"block_parameters\": %llu, \"terminator_arguments\": %llu},\n",
        (unsigned long long)metrics->blocks,
        (unsigned long long)metrics->values,
        (unsigned long long)metrics->operations,
        (unsigned long long)metrics->block_parameters,
        (unsigned long long)metrics->terminator_arguments);
    printf("  \"fingerprint\": \"0x%016llx\"\n", (unsigned long long)fingerprint);
    printf("}\n");
}

int main(int argc, char **argv) {
    xair_binary_image binary;
    xair_image image;
    xair_lift_options options;
    xair_lift_result result;
    xair_module_metrics metrics;
    xair_error error;
    xair_module *module = NULL;
    uint64_t entry = 0;
    uint64_t fingerprint = 0;
    size_t max_instructions = 32;
    xair_status status;
    int json = 0;
    int argi = 1;
    int exit_code = 1;

    memset(&binary, 0, sizeof(binary));
    if (argc > 1 && strcmp(argv[1], "--json") == 0) {
        json = 1;
        argi = 2;
    }
    if (argc - argi < 1 || argc - argi > 3) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc - argi >= 2 && !parse_u64(argv[argi + 1], &entry)) {
        print_usage(argv[0]);
        return 1;
    }
    if (argc - argi == 3 && !parse_size(argv[argi + 2], &max_instructions)) {
        print_usage(argv[0]);
        return 1;
    }

    status = xair_binary_load_path(argv[argi], &binary);
    if (status != XAIR_OK) {
        fprintf(stderr, "binary load failed: %s\n", xair_status_name(status));
        goto done;
    }
    if (entry == 0) {
        entry = binary.entry;
    }
    status = xair_module_create(&module);
    if (status != XAIR_OK) {
        fprintf(stderr, "module create failed: %s\n", xair_status_name(status));
        goto done;
    }
    status = xair_image_init(&image, binary.segment_bytes, binary.segment_size, binary.segment_base);
    if (status != XAIR_OK) {
        fprintf(stderr, "image init failed: %s\n", xair_status_name(status));
        goto done;
    }
    memset(&options, 0, sizeof(options));
    options.arch = binary.arch;
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
    status = xair_module_freeze(module);
    if (status != XAIR_OK) {
        fprintf(stderr, "freeze failed: %s\n", xair_status_name(status));
        goto done;
    }
    (void)xair_get_module_metrics(module, &metrics);
    (void)xair_module_fingerprint(module, &fingerprint);
    if (json) {
        print_json_lift(&binary, &result, &metrics, fingerprint);
    } else {
        printf("binary.format=%s\n", xair_binary_format_name(binary.format));
        printf("binary.arch=%s\n", xair_arch_name(binary.arch));
        printf("binary.entry=0x%llx\n", (unsigned long long)binary.entry);
        printf("binary.segment_base=0x%llx\n", (unsigned long long)binary.segment_base);
        printf("binary.segment_size=%llu\n\n", (unsigned long long)binary.segment_size);
        if (!format_and_print_module(module)) {
            fprintf(stderr, "format failed\n");
            goto done;
        }
    }
    exit_code = 0;

done:
    xair_module_destroy(module);
    xair_binary_image_destroy(&binary);
    return exit_code;
}
