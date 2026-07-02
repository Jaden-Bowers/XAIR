#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include "xair/xair_frontend.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <time.h>
#endif

typedef struct {
    uint8_t *bytes;
    size_t byte_count;
    uint64_t base;
    uint64_t entry;
    size_t max_instructions;
    int have_arch;
    int have_bytes;
    int have_base;
    int have_entry;
} bench_case;

typedef struct {
    size_t repeat;
    size_t warmup;
    int canonicalize;
} bench_options;

static uint64_t monotonic_ns(void) {
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER counter;

    if (freq.QuadPart == 0) {
        (void)QueryPerformanceFrequency(&freq);
    }
    (void)QueryPerformanceCounter(&counter);
    return (uint64_t)((counter.QuadPart * 1000000000ull) / freq.QuadPart);
#else
    struct timespec ts;

    (void)clock_gettime(CLOCK_MONOTONIC, &ts);
    return ((uint64_t)ts.tv_sec * UINT64_C(1000000000)) + (uint64_t)ts.tv_nsec;
#endif
}

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

static void bench_case_init(bench_case *config) {
    memset(config, 0, sizeof(*config));
    config->max_instructions = 32;
}

static void bench_case_destroy(bench_case *config) {
    free(config->bytes);
}

static int parse_case_line(bench_case *config, char *line) {
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
        config->have_arch = strcmp(value, "x86_64") == 0;
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
    if (strcmp(key, "bytes") == 0) {
        free(config->bytes);
        config->bytes = NULL;
        config->byte_count = 0;
        config->have_bytes = parse_hex_bytes(value, &config->bytes, &config->byte_count);
        return config->have_bytes;
    }
    if (strcmp(key, "run") == 0 || strncmp(key, "reg.", 4) == 0 || strncmp(key, "mem.", 4) == 0) {
        return 1;
    }
    return 0;
}

static int read_case_file(const char *path, bench_case *config) {
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

static void print_value_numbering(const char *name, const xair_value_numbering_stats *stats) {
    printf(
        "\"%s\":{\"entries\":%llu,\"created\":%llu,\"reused\":%llu,\"collisions\":%llu}",
        name,
        (unsigned long long)stats->entries,
        (unsigned long long)stats->created,
        (unsigned long long)stats->reused,
        (unsigned long long)stats->collisions);
}

static void print_canonicalize_stats(const xair_canonicalize_stats *stats) {
    printf(
        "\"canonicalize_stats\":{\"values_before\":%llu,\"values_after\":%llu,"
        "\"operations_before\":%llu,\"operations_after\":%llu,"
        "\"value_number_entries_before\":%llu,\"value_number_entries_after\":%llu,"
        "\"operands_reordered\":%llu,\"constants_folded\":%llu,"
        "\"dead_values_removed\":%llu,\"dead_operations_removed\":%llu}",
        (unsigned long long)stats->values_before,
        (unsigned long long)stats->values_after,
        (unsigned long long)stats->operations_before,
        (unsigned long long)stats->operations_after,
        (unsigned long long)stats->value_number_entries_before,
        (unsigned long long)stats->value_number_entries_after,
        (unsigned long long)stats->operands_reordered,
        (unsigned long long)stats->constants_folded,
        (unsigned long long)stats->dead_values_removed,
        (unsigned long long)stats->dead_operations_removed);
}

static void print_error_row(
    const char *case_path,
    size_t iteration,
    const char *phase,
    xair_status status) {
    printf("{\"engine\":\"xair-native\",\"case\":");
    print_json_string(case_path);
    printf(
        ",\"iteration\":%llu,\"status\":\"error\",\"phase\":",
        (unsigned long long)iteration);
    print_json_string(phase);
    printf(",\"error\":");
    print_json_string(xair_status_name(status));
    printf("}\n");
}

static xair_status bench_once(
    const char *case_path,
    const bench_case *config,
    const bench_options *options,
    size_t iteration,
    int emit) {
    xair_module *module = NULL;
    xair_image image;
    xair_lift_options lift_options;
    xair_lift_result lift;
    xair_error error;
    xair_module_metrics metrics;
    xair_value_numbering_stats construction_vn;
    xair_value_numbering_stats final_vn;
    xair_canonicalize_stats canon_stats;
    xair_status status;
    uint64_t fingerprint = 0;
    uint64_t total_start;
    uint64_t create_ns = 0;
    uint64_t lift_ns = 0;
    uint64_t verify_ns = 0;
    uint64_t canonicalize_ns = 0;
    uint64_t freeze_ns = 0;
    uint64_t fingerprint_ns = 0;
    uint64_t t0;
    uint64_t t1;

    memset(&construction_vn, 0, sizeof(construction_vn));
    memset(&final_vn, 0, sizeof(final_vn));
    memset(&canon_stats, 0, sizeof(canon_stats));
    memset(&metrics, 0, sizeof(metrics));
    memset(&lift, 0, sizeof(lift));

    total_start = monotonic_ns();
    t0 = monotonic_ns();
    status = xair_module_create(&module);
    t1 = monotonic_ns();
    create_ns = t1 - t0;
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "module_create", status);
        }
        return status;
    }

    status = xair_image_init(&image, config->bytes, config->byte_count, config->base);
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "image_init", status);
        }
        xair_module_destroy(module);
        return status;
    }
    memset(&lift_options, 0, sizeof(lift_options));
    lift_options.arch = XAIR_ARCH_X86_64;
    lift_options.address = config->entry;
    lift_options.max_instructions = config->max_instructions;

    t0 = monotonic_ns();
    status = xair_lift_basic_block(module, &image, &lift_options, &lift);
    t1 = monotonic_ns();
    lift_ns = t1 - t0;
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "lift", status);
        }
        xair_module_destroy(module);
        return status;
    }

    t0 = monotonic_ns();
    status = xair_verify_module(module, &error);
    t1 = monotonic_ns();
    verify_ns = t1 - t0;
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "verify", status);
        }
        xair_module_destroy(module);
        return status;
    }

    (void)xair_get_value_numbering_stats(module, &construction_vn);
    if (options->canonicalize) {
        t0 = monotonic_ns();
        status = xair_canonicalize_module(module, &canon_stats, &error);
        t1 = monotonic_ns();
        canonicalize_ns = t1 - t0;
        if (status != XAIR_OK) {
            if (emit) {
                print_error_row(case_path, iteration, "canonicalize", status);
            }
            xair_module_destroy(module);
            return status;
        }
    }

    t0 = monotonic_ns();
    status = xair_module_freeze(module);
    t1 = monotonic_ns();
    freeze_ns = t1 - t0;
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "freeze", status);
        }
        xair_module_destroy(module);
        return status;
    }

    (void)xair_get_module_metrics(module, &metrics);
    (void)xair_get_value_numbering_stats(module, &final_vn);

    t0 = monotonic_ns();
    status = xair_module_fingerprint(module, &fingerprint);
    t1 = monotonic_ns();
    fingerprint_ns = t1 - t0;
    if (status != XAIR_OK) {
        if (emit) {
            print_error_row(case_path, iteration, "fingerprint", status);
        }
        xair_module_destroy(module);
        return status;
    }

    if (emit) {
        uint64_t total_ns = monotonic_ns() - total_start;

        printf("{\"engine\":\"xair-native\",\"case\":");
        print_json_string(case_path);
        printf(
            ",\"iteration\":%llu,\"status\":\"ok\",\"ir_version\":\"%s\","
            "\"decoder\":",
            (unsigned long long)iteration,
            xair_ir_version_string());
        print_json_string(lift.decoder_name == NULL ? "unknown" : lift.decoder_name);
        printf(
            ",\"end\":\"%s\",\"start\":%llu,\"next\":%llu,\"bytes\":%llu,"
            "\"instructions\":%llu,\"target\":%llu,\"fallthrough\":%llu,"
            "\"unsupported_address\":%llu,\"unsupported_opcode\":%u,"
            "\"canonicalized\":%s,"
            "\"time_ns\":%llu,\"create_ns\":%llu,\"lift_ns\":%llu,"
            "\"verify_ns\":%llu,\"canonicalize_ns\":%llu,\"freeze_ns\":%llu,"
            "\"fingerprint_ns\":%llu,"
            "\"metrics\":{\"blocks\":%llu,\"values\":%llu,\"operations\":%llu,"
            "\"block_parameters\":%llu,\"terminator_arguments\":%llu},",
            xair_lift_end_kind_name(lift.end_kind),
            (unsigned long long)lift.start,
            (unsigned long long)lift.next,
            (unsigned long long)lift.bytes_read,
            (unsigned long long)lift.instructions,
            (unsigned long long)lift.target,
            (unsigned long long)lift.fallthrough,
            (unsigned long long)lift.unsupported_address,
            (unsigned)lift.unsupported_opcode,
            options->canonicalize ? "true" : "false",
            (unsigned long long)total_ns,
            (unsigned long long)create_ns,
            (unsigned long long)lift_ns,
            (unsigned long long)verify_ns,
            (unsigned long long)canonicalize_ns,
            (unsigned long long)freeze_ns,
            (unsigned long long)fingerprint_ns,
            (unsigned long long)metrics.blocks,
            (unsigned long long)metrics.values,
            (unsigned long long)metrics.operations,
            (unsigned long long)metrics.block_parameters,
            (unsigned long long)metrics.terminator_arguments);
        print_value_numbering("value_numbering_construction", &construction_vn);
        printf(",");
        print_value_numbering("value_numbering_final", &final_vn);
        printf(",");
        print_canonicalize_stats(&canon_stats);
        printf(",\"fingerprint\":\"0x%016llx\"}\n", (unsigned long long)fingerprint);
    }

    xair_module_destroy(module);
    return XAIR_OK;
}

static int parse_size_arg(const char *text, size_t *out_value) {
    uint64_t value;

    if (!parse_u64(text, &value) || value > (uint64_t)SIZE_MAX) {
        return 0;
    }
    *out_value = (size_t)value;
    return 1;
}

static void print_usage(const char *argv0) {
    fprintf(stderr, "usage: %s [--repeat N] [--warmup N] [--canonicalize] <case-file>...\n", argv0);
}

int main(int argc, char **argv) {
    bench_options options;
    int argi = 1;
    int exit_code = 0;

    options.repeat = 1000;
    options.warmup = 100;
    options.canonicalize = 0;

    while (argi < argc && strncmp(argv[argi], "--", 2) == 0) {
        if (strcmp(argv[argi], "--repeat") == 0) {
            if (argi + 1 >= argc || !parse_size_arg(argv[argi + 1], &options.repeat) || options.repeat == 0) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (strcmp(argv[argi], "--warmup") == 0) {
            if (argi + 1 >= argc || !parse_size_arg(argv[argi + 1], &options.warmup)) {
                print_usage(argv[0]);
                return 1;
            }
            argi += 2;
        } else if (strcmp(argv[argi], "--canonicalize") == 0) {
            options.canonicalize = 1;
            ++argi;
        } else {
            print_usage(argv[0]);
            return 1;
        }
    }
    if (argi >= argc) {
        print_usage(argv[0]);
        return 1;
    }

    for (; argi < argc; ++argi) {
        bench_case config;
        size_t i;

        bench_case_init(&config);
        if (!read_case_file(argv[argi], &config)) {
            fprintf(stderr, "failed to parse case file: %s\n", argv[argi]);
            bench_case_destroy(&config);
            return 1;
        }
        for (i = 0; i < options.warmup; ++i) {
            if (bench_once(argv[argi], &config, &options, i, 0) != XAIR_OK) {
                exit_code = 1;
                break;
            }
        }
        if (exit_code == 0) {
            for (i = 0; i < options.repeat; ++i) {
                if (bench_once(argv[argi], &config, &options, i, 1) != XAIR_OK) {
                    exit_code = 1;
                    break;
                }
            }
        }
        bench_case_destroy(&config);
        if (exit_code != 0) {
            break;
        }
    }
    return exit_code;
}
