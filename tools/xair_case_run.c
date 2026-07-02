#include "xair/xair_frontend.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CASE_MAX_MEMORY 64

typedef struct {
    uint16_t space;
    uint64_t address;
    uint8_t *bytes;
    size_t size;
} memory_entry;

typedef struct {
    uint8_t *bytes;
    size_t byte_count;
    uint64_t base;
    uint64_t entry;
    size_t max_instructions;
    xair_arch arch;
    int have_arch;
    int have_bytes;
    int have_base;
    int have_entry;
    int run;
    uint64_t regs[XAIR_X86_REG_COUNT];
    unsigned char have_reg[XAIR_X86_REG_COUNT];
    memory_entry memory[CASE_MAX_MEMORY];
    size_t memory_count;
} case_config;

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

static char *trim(char *text) {
    char *end;

    while (*text != '\0' && isspace((unsigned char)*text)) {
        ++text;
    }
    end = text + strlen(text);
    while (end > text && isspace((unsigned char)end[-1])) {
        --end;
    }
    *end = '\0';
    return text;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static int parse_hex_bytes(const char *text, uint8_t **out_bytes, size_t *out_size) {
    size_t len;
    size_t i;
    uint8_t *bytes;

    if (text == NULL || out_bytes == NULL || out_size == NULL) {
        return 0;
    }
    len = strlen(text);
    if ((len & 1u) != 0) {
        return 0;
    }
    bytes = (uint8_t *)malloc(len == 0 ? 1u : len / 2u);
    if (bytes == NULL) {
        return 0;
    }
    for (i = 0; i < len / 2u; ++i) {
        int hi = hex_digit(text[i * 2u]);
        int lo = hex_digit(text[i * 2u + 1u]);
        if (hi < 0 || lo < 0) {
            free(bytes);
            return 0;
        }
        bytes[i] = (uint8_t)((hi << 4u) | lo);
    }
    *out_bytes = bytes;
    *out_size = len / 2u;
    return 1;
}

static int reg_from_name(const char *name, xair_x86_reg *out_reg) {
    xair_x86_reg reg;

    if (name == NULL || out_reg == NULL) {
        return 0;
    }
    for (reg = XAIR_X86_RAX; reg < XAIR_X86_REG_COUNT; reg = (xair_x86_reg)(reg + 1)) {
        if (strcmp(name, xair_x86_reg_name(reg)) == 0) {
            *out_reg = reg;
            return 1;
        }
    }
    return 0;
}

static int parse_memory_key(const char *key, uint16_t *out_space, uint64_t *out_address) {
    char space_text[32];
    const char *p;
    char *dot;
    uint64_t space;

    if (strncmp(key, "mem.", 4) != 0) {
        return 0;
    }
    p = key + 4;
    dot = strchr(p, '.');
    if (dot == NULL || (size_t)(dot - p) >= sizeof(space_text)) {
        return 0;
    }
    memcpy(space_text, p, (size_t)(dot - p));
    space_text[dot - p] = '\0';
    if (!parse_u64(space_text, &space) || space > UINT16_MAX) {
        return 0;
    }
    if (!parse_u64(dot + 1, out_address)) {
        return 0;
    }
    *out_space = (uint16_t)space;
    return 1;
}

static void case_config_init(case_config *config) {
    memset(config, 0, sizeof(*config));
    config->arch = XAIR_ARCH_X86_64;
    config->max_instructions = 32;
    config->run = 1;
}

static void case_config_destroy(case_config *config) {
    size_t i;

    free(config->bytes);
    for (i = 0; i < config->memory_count; ++i) {
        free(config->memory[i].bytes);
    }
}

static int parse_case_line(case_config *config, char *line) {
    char *key;
    char *value;
    uint64_t parsed;

    key = trim(line);
    if (key[0] == '\0' || key[0] == '#') {
        return 1;
    }
    value = strchr(key, '=');
    if (value == NULL) {
        return 0;
    }
    *value++ = '\0';
    key = trim(key);
    value = trim(value);

    if (strcmp(key, "arch") == 0) {
        if (strcmp(value, "x86_64") == 0) {
            config->arch = XAIR_ARCH_X86_64;
            config->have_arch = 1;
        } else if (strcmp(value, "x86_32") == 0 || strcmp(value, "x86") == 0 || strcmp(value, "i386") == 0) {
            config->arch = XAIR_ARCH_X86_32;
            config->have_arch = 1;
        } else {
            config->have_arch = 0;
        }
        return config->have_arch;
    }
    if (strcmp(key, "base") == 0) {
        config->have_base = parse_u64(value, &config->base);
        return config->have_base;
    }
    if (strcmp(key, "entry") == 0) {
        config->have_entry = parse_u64(value, &config->entry);
        return config->have_entry;
    }
    if (strcmp(key, "max_instructions") == 0) {
        if (!parse_u64(value, &parsed) || parsed > (uint64_t)SIZE_MAX) {
            return 0;
        }
        config->max_instructions = (size_t)parsed;
        return 1;
    }
    if (strcmp(key, "run") == 0) {
        config->run = strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
        return 1;
    }
    if (strcmp(key, "bytes") == 0) {
        free(config->bytes);
        config->bytes = NULL;
        config->byte_count = 0;
        config->have_bytes = parse_hex_bytes(value, &config->bytes, &config->byte_count);
        return config->have_bytes;
    }
    if (strncmp(key, "reg.", 4) == 0) {
        xair_x86_reg reg;

        if (!reg_from_name(key + 4, &reg) || !parse_u64(value, &parsed)) {
            return 0;
        }
        config->regs[reg] = parsed;
        config->have_reg[reg] = 1u;
        return 1;
    }
    if (strncmp(key, "mem.", 4) == 0) {
        memory_entry *entry;

        if (config->memory_count >= CASE_MAX_MEMORY) {
            return 0;
        }
        entry = &config->memory[config->memory_count];
        if (!parse_memory_key(key, &entry->space, &entry->address) ||
            !parse_hex_bytes(value, &entry->bytes, &entry->size)) {
            return 0;
        }
        ++config->memory_count;
        return 1;
    }
    return 0;
}

static int read_case_file(const char *path, case_config *config) {
    FILE *file;
    char line[8192];

    file = fopen(path, "r");
    if (file == NULL) {
        return 0;
    }
    while (fgets(line, sizeof(line), file) != NULL) {
        if (!parse_case_line(config, line)) {
            fclose(file);
            return 0;
        }
    }
    fclose(file);
    return config->have_arch && config->have_base && config->have_entry && config->have_bytes;
}

static const char *exec_halt_name(xair_exec_halt_kind kind) {
    switch (kind) {
    case XAIR_EXEC_HALTED_RETURN: return "return";
    case XAIR_EXEC_HALTED_TRAP: return "trap";
    case XAIR_EXEC_HALTED_FAULT: return "fault";
    case XAIR_EXEC_HALTED_STEP_LIMIT: return "step_limit";
    case XAIR_EXEC_HALTED_UNSUPPORTED: return "unsupported";
    default: return "unknown";
    }
}

static void print_json_string(const char *text) {
    const unsigned char *p = (const unsigned char *)(text == NULL ? "" : text);

    putchar('"');
    while (*p != '\0') {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar((int)*p);
        } else if (*p == '\n') {
            fputs("\\n", stdout);
        } else if (*p == '\r') {
            fputs("\\r", stdout);
        } else if (*p == '\t') {
            fputs("\\t", stdout);
        } else if (*p < 0x20u) {
            printf("\\u%04x", (unsigned)*p);
        } else {
            putchar((int)*p);
        }
        ++p;
    }
    putchar('"');
}

static size_t return_index_for_value(const xair_lift_result *result, xair_value_id value) {
    size_t i;

    for (i = 0; i < result->return_count; ++i) {
        if (result->return_values[i] == value) {
            return i;
        }
    }
    return SIZE_MAX;
}

static int can_execute(const case_config *config, const xair_lift_result *lift, const char **out_reason) {
    size_t i;

    if (!config->run) {
        *out_reason = "disabled";
        return 0;
    }
    if (lift->end_kind == XAIR_LIFT_END_UNSUPPORTED) {
        *out_reason = "unsupported_lift";
        return 0;
    }
    for (i = 0; i < lift->input_reg_count; ++i) {
        if (!config->have_reg[lift->input_regs[i].reg]) {
            *out_reason = "missing_register";
            return 0;
        }
    }
    *out_reason = "";
    return 1;
}

static void print_json_u64_array(const xair_exec_result *exec) {
    size_t i;

    printf("    \"returns\": [");
    for (i = 0; i < exec->return_count; ++i) {
        if (i != 0) {
            printf(", ");
        }
        printf("{\"type_bits\":%u,\"value\":%llu}",
            (unsigned)exec->returns[i].type.bits,
            (unsigned long long)exec->returns[i].lo);
    }
    printf("],\n");
}

static void print_json_value_numbering(const char *name, const xair_value_numbering_stats *stats) {
    printf(
        "  \"%s\": {\"entries\": %llu, \"created\": %llu, \"reused\": %llu, \"collisions\": %llu},\n",
        name,
        (unsigned long long)stats->entries,
        (unsigned long long)stats->created,
        (unsigned long long)stats->reused,
        (unsigned long long)stats->collisions);
}

static void print_execution_json(const xair_lift_result *lift, const xair_exec_result *exec) {
    size_t i;

    printf("  \"execution\": {\n");
    printf("    \"ran\": true,\n");
    printf("    \"halt\": \"%s\",\n", exec_halt_name(exec->kind));
    printf("    \"code\": %u,\n", (unsigned)exec->code);
    print_json_u64_array(exec);
    printf("    \"output_regs\": [");
    for (i = 0; i < lift->output_reg_count; ++i) {
        size_t index = return_index_for_value(lift, lift->output_regs[i].value);

        if (i != 0) {
            printf(", ");
        }
        printf("{\"reg\":\"%s\",\"value\":",
            xair_x86_reg_name(lift->output_regs[i].reg));
        if (index == SIZE_MAX || index >= exec->return_count) {
            printf("null}");
        } else {
            printf("%llu}", (unsigned long long)exec->returns[index].lo);
        }
    }
    printf("]\n");
    printf("  }\n");
}

static void print_skip_json(const char *reason) {
    printf("  \"execution\": {\"ran\": false, \"reason\": ");
    print_json_string(reason);
    printf("}\n");
}

int main(int argc, char **argv) {
    case_config config;
    xair_image image;
    xair_lift_options options;
    xair_lift_result lift;
    xair_module *module = NULL;
    xair_exec_state *state = NULL;
    xair_exec_result exec;
    xair_module_metrics metrics;
    xair_value_numbering_stats construction_value_numbers;
    xair_value_numbering_stats final_value_numbers;
    xair_error error;
    uint64_t fingerprint = 0;
    xair_status status;
    const char *skip_reason = "";
    int do_execute;
    int exit_code = 1;
    size_t i;

    if (argc != 2) {
        fprintf(stderr, "usage: %s <case-file>\n", argv[0]);
        return 1;
    }
    case_config_init(&config);
    if (!read_case_file(argv[1], &config)) {
        fprintf(stderr, "failed to parse case file\n");
        goto done;
    }
    status = xair_module_create(&module);
    if (status != XAIR_OK) {
        fprintf(stderr, "module create failed: %s\n", xair_status_name(status));
        goto done;
    }
    status = xair_image_init(&image, config.bytes, config.byte_count, config.base);
    if (status != XAIR_OK) {
        fprintf(stderr, "image init failed: %s\n", xair_status_name(status));
        goto done;
    }
    memset(&options, 0, sizeof(options));
    options.arch = config.arch;
    options.address = config.entry;
    options.max_instructions = config.max_instructions;
    status = xair_lift_basic_block(module, &image, &options, &lift);
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
    (void)xair_get_module_metrics(module, &metrics);
    (void)xair_get_value_numbering_stats(module, &final_value_numbers);
    (void)xair_module_fingerprint(module, &fingerprint);

    do_execute = can_execute(&config, &lift, &skip_reason);
    if (do_execute) {
        status = xair_exec_state_create(module, &state);
        if (status != XAIR_OK) {
            fprintf(stderr, "exec state create failed: %s\n", xair_status_name(status));
            goto done;
        }
        if (lift.memory_in != XAIR_INVALID_ID) {
            status = xair_exec_set_param(state, lift.memory_in, xair_exec_mem(0, 64));
            if (status != XAIR_OK) {
                fprintf(stderr, "memory param failed: %s\n", xair_status_name(status));
                goto done;
            }
        }
        for (i = 0; i < config.memory_count; ++i) {
            status = xair_exec_store_bytes(
                state,
                config.memory[i].space,
                config.memory[i].address,
                config.memory[i].bytes,
                config.memory[i].size);
            if (status != XAIR_OK) {
                fprintf(stderr, "memory store failed: %s\n", xair_status_name(status));
                goto done;
            }
        }
        for (i = 0; i < lift.input_reg_count; ++i) {
            xair_x86_reg reg = lift.input_regs[i].reg;

            status = xair_exec_set_param(state, lift.input_regs[i].value, xair_exec_i(64, config.regs[reg]));
            if (status != XAIR_OK) {
                fprintf(stderr, "register param failed: %s\n", xair_status_name(status));
                goto done;
            }
        }
        status = xair_exec_run(module, lift.block, state, 256, &exec);
        if (status != XAIR_OK) {
            fprintf(stderr, "exec failed: %s\n", xair_status_name(status));
            goto done;
        }
    }

    printf("{\n");
    printf("  \"status\": \"ok\",\n");
    printf("  \"case\": ");
    print_json_string(argv[1]);
    printf(",\n");
    printf("  \"ir_version\": \"%s\",\n", xair_ir_version_string());
    printf("  \"decoder\": ");
    print_json_string(lift.decoder_name == NULL ? "unknown" : lift.decoder_name);
    printf(",\n");
    printf("  \"end\": \"%s\",\n", xair_lift_end_kind_name(lift.end_kind));
    printf("  \"start\": %llu,\n", (unsigned long long)lift.start);
    printf("  \"next\": %llu,\n", (unsigned long long)lift.next);
    printf("  \"bytes\": %llu,\n", (unsigned long long)lift.bytes_read);
    printf("  \"instructions\": %llu,\n", (unsigned long long)lift.instructions);
    printf("  \"target\": %llu,\n", (unsigned long long)lift.target);
    printf("  \"fallthrough\": %llu,\n", (unsigned long long)lift.fallthrough);
    printf("  \"unsupported_address\": %llu,\n", (unsigned long long)lift.unsupported_address);
    printf("  \"unsupported_opcode\": %u,\n", (unsigned)lift.unsupported_opcode);
    printf("  \"metrics\": {\"blocks\": %llu, \"values\": %llu, \"operations\": %llu, \"block_parameters\": %llu, \"terminator_arguments\": %llu},\n",
        (unsigned long long)metrics.blocks,
        (unsigned long long)metrics.values,
        (unsigned long long)metrics.operations,
        (unsigned long long)metrics.block_parameters,
        (unsigned long long)metrics.terminator_arguments);
    print_json_value_numbering("value_numbering_construction", &construction_value_numbers);
    print_json_value_numbering("value_numbering_final", &final_value_numbers);
    printf("  \"fingerprint\": \"0x%016llx\",\n", (unsigned long long)fingerprint);
    if (do_execute) {
        print_execution_json(&lift, &exec);
    } else {
        print_skip_json(skip_reason);
    }
    printf("}\n");
    exit_code = 0;

done:
    xair_exec_state_destroy(state);
    xair_module_destroy(module);
    case_config_destroy(&config);
    return exit_code;
}
