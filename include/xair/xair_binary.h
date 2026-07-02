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

#define XAIR_BINARY_PERM_READ 1u
#define XAIR_BINARY_PERM_WRITE 2u
#define XAIR_BINARY_PERM_EXEC 4u

typedef struct {
    uint64_t va;
    uint64_t mem_size;
    uint64_t file_size;
    uint64_t file_offset;
    uint32_t perms;
    uint32_t section_id;
    uint8_t *bytes;
} xair_binary_segment;

typedef struct {
    xair_binary_format format;
    xair_arch arch;
    uint64_t entry;
    uint64_t image_base;
    xair_binary_segment *segments;
    size_t segment_count;
} xair_binary_view;

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
xair_status xair_binary_view_load_path(const char *path, xair_binary_view *out_view);
void xair_binary_view_destroy(xair_binary_view *view);
const xair_binary_segment *xair_binary_view_find_segment(
    const xair_binary_view *view,
    uint64_t va,
    uint32_t required_perms);
xair_status xair_binary_view_read(
    const xair_binary_view *view,
    uint64_t va,
    void *out_bytes,
    size_t size);
xair_status xair_binary_load_path(const char *path, xair_binary_image *out_image);
void xair_binary_image_destroy(xair_binary_image *image);

#ifdef __cplusplus
}
#endif

#endif
