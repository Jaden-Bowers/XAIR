#ifndef XAIR_XAIR_BINARY_H
#define XAIR_XAIR_BINARY_H

#include "xair/xair_frontend.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    XAIR_BINARY_FORMAT_UNKNOWN = 0,
    XAIR_BINARY_FORMAT_ELF,
    XAIR_BINARY_FORMAT_PE
} xair_binary_format;

typedef struct {
    xair_binary_format format;
    xair_arch arch;
    uint64_t entry;
    uint64_t image_base;
    uint64_t segment_base;
    uint8_t *segment_bytes;
    size_t segment_size;
} xair_binary_image;

const char *xair_binary_format_name(xair_binary_format format);
xair_status xair_binary_load_path(const char *path, xair_binary_image *out_image);
void xair_binary_image_destroy(xair_binary_image *image);

#ifdef __cplusplus
}
#endif

#endif
