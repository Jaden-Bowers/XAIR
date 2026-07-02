#include "xair/xair_binary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XAIR_PE_MACHINE_I386 0x014cu
#define XAIR_PE_MACHINE_AMD64 0x8664u
#define XAIR_ELF_MACHINE_386 3u
#define XAIR_ELF_MACHINE_X86_64 62u

#define XAIR_PE_SCN_MEM_EXECUTE 0x20000000u
#define XAIR_PE_SCN_MEM_READ 0x40000000u
#define XAIR_PE_SCN_MEM_WRITE 0x80000000u

static uint16_t read_le16(const uint8_t *bytes, size_t offset) {
    return (uint16_t)(((uint16_t)bytes[offset]) | ((uint16_t)bytes[offset + 1u] << 8u));
}

static uint32_t read_le32(const uint8_t *bytes, size_t offset) {
    return ((uint32_t)bytes[offset]) |
        ((uint32_t)bytes[offset + 1u] << 8u) |
        ((uint32_t)bytes[offset + 2u] << 16u) |
        ((uint32_t)bytes[offset + 3u] << 24u);
}

static uint64_t read_le64(const uint8_t *bytes, size_t offset) {
    return ((uint64_t)read_le32(bytes, offset)) | ((uint64_t)read_le32(bytes, offset + 4u) << 32u);
}

static int range_ok(size_t size, size_t offset, size_t need) {
    return offset <= size && need <= size - offset;
}

static uint64_t max_u64(uint64_t lhs, uint64_t rhs) {
    return lhs > rhs ? lhs : rhs;
}

static xair_status read_file(const char *path, uint8_t **out_bytes, size_t *out_size) {
    FILE *file;
    long file_size;
    uint8_t *bytes;
    size_t read_count;

    if (path == NULL || out_bytes == NULL || out_size == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    file = fopen(path, "rb");
    if (file == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return XAIR_ERR_BAD_ARG;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return XAIR_ERR_BAD_ARG;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return XAIR_ERR_BAD_ARG;
    }
    bytes = (uint8_t *)malloc((size_t)file_size == 0 ? 1u : (size_t)file_size);
    if (bytes == NULL) {
        fclose(file);
        return XAIR_ERR_OOM;
    }
    read_count = fread(bytes, 1, (size_t)file_size, file);
    fclose(file);
    if (read_count != (size_t)file_size) {
        free(bytes);
        return XAIR_ERR_BAD_ARG;
    }
    *out_bytes = bytes;
    *out_size = (size_t)file_size;
    return XAIR_OK;
}

static xair_status append_segment(
    xair_binary_view *view,
    const uint8_t *file_bytes,
    size_t file_size,
    uint64_t va,
    uint64_t mem_size,
    uint64_t file_offset,
    uint64_t segment_file_size,
    uint32_t perms,
    uint32_t section_id) {
    xair_binary_segment *next_segments;
    xair_binary_segment *segment;
    uint8_t *segment_bytes;
    size_t segment_size;

    if (mem_size == 0) {
        return XAIR_OK;
    }
    if (file_offset > (uint64_t)SIZE_MAX || segment_file_size > (uint64_t)SIZE_MAX) {
        return XAIR_ERR_RANGE;
    }
    segment_size = (size_t)segment_file_size;
    if (!range_ok(file_size, (size_t)file_offset, segment_size)) {
        return XAIR_ERR_RANGE;
    }
    if (view->segment_count == (size_t)UINT32_MAX || view->segment_count == SIZE_MAX) {
        return XAIR_ERR_RANGE;
    }
    next_segments = (xair_binary_segment *)realloc(
        view->segments,
        (view->segment_count + 1u) * sizeof(*view->segments));
    if (next_segments == NULL) {
        return XAIR_ERR_OOM;
    }
    view->segments = next_segments;
    segment_bytes = (uint8_t *)malloc(segment_size == 0 ? 1u : segment_size);
    if (segment_bytes == NULL) {
        return XAIR_ERR_OOM;
    }
    if (segment_size != 0) {
        memcpy(segment_bytes, file_bytes + (size_t)file_offset, segment_size);
    }
    segment = &view->segments[view->segment_count++];
    memset(segment, 0, sizeof(*segment));
    segment->va = va;
    segment->mem_size = mem_size;
    segment->file_size = segment_file_size;
    segment->file_offset = file_offset;
    segment->perms = perms;
    segment->section_id = section_id;
    segment->bytes = segment_bytes;
    return XAIR_OK;
}

static uint32_t pe_section_perms(uint32_t characteristics) {
    uint32_t perms = 0;

    if ((characteristics & XAIR_PE_SCN_MEM_READ) != 0) {
        perms |= XAIR_BINARY_PERM_READ;
    }
    if ((characteristics & XAIR_PE_SCN_MEM_WRITE) != 0) {
        perms |= XAIR_BINARY_PERM_WRITE;
    }
    if ((characteristics & XAIR_PE_SCN_MEM_EXECUTE) != 0) {
        perms |= XAIR_BINARY_PERM_EXEC;
    }
    return perms;
}

static uint32_t elf_segment_perms(uint32_t flags) {
    uint32_t perms = 0;

    if ((flags & 4u) != 0) {
        perms |= XAIR_BINARY_PERM_READ;
    }
    if ((flags & 2u) != 0) {
        perms |= XAIR_BINARY_PERM_WRITE;
    }
    if ((flags & 1u) != 0) {
        perms |= XAIR_BINARY_PERM_EXEC;
    }
    return perms;
}

static xair_status parse_pe_view(const uint8_t *bytes, size_t size, xair_binary_view *out_view) {
    uint32_t pe_offset;
    uint16_t machine;
    uint16_t section_count;
    uint16_t optional_size;
    uint16_t magic;
    uint32_t entry_rva;
    uint64_t image_base;
    size_t coff_offset;
    size_t optional_offset;
    size_t section_offset;
    size_t i;
    xair_status status;

    if (!range_ok(size, 0, 0x40u) || bytes[0] != 'M' || bytes[1] != 'Z') {
        return XAIR_ERR_UNSUPPORTED;
    }
    pe_offset = read_le32(bytes, 0x3cu);
    #if SIZE_MAX < UINT32_MAX
    if (pe_offset > (uint32_t)SIZE_MAX) {
        return XAIR_ERR_UNSUPPORTED;
    }
    #endif
    if (!range_ok(size, (size_t)pe_offset, 24u) || memcmp(bytes + (size_t)pe_offset, "PE\0\0", 4u) != 0) {
        return XAIR_ERR_UNSUPPORTED;
    }
    coff_offset = (size_t)pe_offset + 4u;
    machine = read_le16(bytes, coff_offset);
    section_count = read_le16(bytes, coff_offset + 2u);
    optional_size = read_le16(bytes, coff_offset + 16u);
    optional_offset = coff_offset + 20u;
    if (!range_ok(size, optional_offset, optional_size) || optional_size < 32u) {
        return XAIR_ERR_RANGE;
    }
    magic = read_le16(bytes, optional_offset);
    entry_rva = read_le32(bytes, optional_offset + 16u);
    if (machine == XAIR_PE_MACHINE_AMD64 && magic == 0x20bu) {
        out_view->arch = XAIR_ARCH_X86_64;
        image_base = read_le64(bytes, optional_offset + 24u);
    } else if (machine == XAIR_PE_MACHINE_I386 && magic == 0x10bu) {
        out_view->arch = XAIR_ARCH_X86_32;
        image_base = read_le32(bytes, optional_offset + 28u);
    } else {
        return XAIR_ERR_UNSUPPORTED;
    }
    out_view->format = XAIR_BINARY_FORMAT_PE;
    out_view->image_base = image_base;
    out_view->entry = image_base + entry_rva;
    section_offset = optional_offset + optional_size;
    if (!range_ok(size, section_offset, (size_t)section_count * 40u)) {
        return XAIR_ERR_RANGE;
    }
    for (i = 0; i < section_count; ++i) {
        size_t off = section_offset + i * 40u;
        uint32_t virtual_size = read_le32(bytes, off + 8u);
        uint32_t virtual_address = read_le32(bytes, off + 12u);
        uint32_t raw_size = read_le32(bytes, off + 16u);
        uint32_t raw_offset = read_le32(bytes, off + 20u);
        uint32_t characteristics = read_le32(bytes, off + 36u);

        status = append_segment(
            out_view,
            bytes,
            size,
            image_base + virtual_address,
            max_u64(virtual_size, raw_size),
            raw_offset,
            raw_size,
            pe_section_perms(characteristics),
            (uint32_t)i);
        if (status != XAIR_OK) {
            return status;
        }
    }
    return out_view->segment_count == 0 ? XAIR_ERR_RANGE : XAIR_OK;
}

static xair_status parse_elf64_view(const uint8_t *bytes, size_t size, xair_binary_view *out_view) {
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    size_t i;
    xair_status status;

    if (!range_ok(size, 0, 64u)) {
        return XAIR_ERR_RANGE;
    }
    machine = read_le16(bytes, 18u);
    if (machine != XAIR_ELF_MACHINE_X86_64) {
        return XAIR_ERR_UNSUPPORTED;
    }
    entry = read_le64(bytes, 24u);
    phoff = read_le64(bytes, 32u);
    phentsize = read_le16(bytes, 54u);
    phnum = read_le16(bytes, 56u);
    if (phoff > (uint64_t)SIZE_MAX || phentsize < 56u ||
        (size_t)phnum > SIZE_MAX / (size_t)phentsize ||
        !range_ok(size, (size_t)phoff, (size_t)phentsize * (size_t)phnum)) {
        return XAIR_ERR_RANGE;
    }
    out_view->format = XAIR_BINARY_FORMAT_ELF;
    out_view->arch = XAIR_ARCH_X86_64;
    out_view->entry = entry;
    out_view->image_base = 0;
    for (i = 0; i < phnum; ++i) {
        size_t off = (size_t)phoff + i * (size_t)phentsize;
        uint32_t type = read_le32(bytes, off);
        uint32_t flags = read_le32(bytes, off + 4u);
        uint64_t file_offset = read_le64(bytes, off + 8u);
        uint64_t vaddr = read_le64(bytes, off + 16u);
        uint64_t filesz = read_le64(bytes, off + 32u);
        uint64_t memsz = read_le64(bytes, off + 40u);

        if (type == 1u) {
            status = append_segment(
                out_view,
                bytes,
                size,
                vaddr,
                memsz,
                file_offset,
                filesz,
                elf_segment_perms(flags),
                (uint32_t)i);
            if (status != XAIR_OK) {
                return status;
            }
        }
    }
    return out_view->segment_count == 0 ? XAIR_ERR_RANGE : XAIR_OK;
}

static xair_status parse_elf32_view(const uint8_t *bytes, size_t size, xair_binary_view *out_view) {
    uint16_t machine;
    uint32_t entry;
    uint32_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    size_t i;
    xair_status status;

    if (!range_ok(size, 0, 52u)) {
        return XAIR_ERR_RANGE;
    }
    machine = read_le16(bytes, 18u);
    if (machine != XAIR_ELF_MACHINE_386) {
        return XAIR_ERR_UNSUPPORTED;
    }
    entry = read_le32(bytes, 24u);
    phoff = read_le32(bytes, 28u);
    phentsize = read_le16(bytes, 42u);
    phnum = read_le16(bytes, 44u);
    if (phentsize < 32u || (size_t)phnum > SIZE_MAX / (size_t)phentsize ||
        !range_ok(size, phoff, (size_t)phentsize * (size_t)phnum)) {
        return XAIR_ERR_RANGE;
    }
    out_view->format = XAIR_BINARY_FORMAT_ELF;
    out_view->arch = XAIR_ARCH_X86_32;
    out_view->entry = entry;
    out_view->image_base = 0;
    for (i = 0; i < phnum; ++i) {
        size_t off = (size_t)phoff + i * (size_t)phentsize;
        uint32_t type = read_le32(bytes, off);
        uint32_t file_offset = read_le32(bytes, off + 4u);
        uint32_t vaddr = read_le32(bytes, off + 8u);
        uint32_t filesz = read_le32(bytes, off + 16u);
        uint32_t memsz = read_le32(bytes, off + 20u);
        uint32_t flags = read_le32(bytes, off + 24u);

        if (type == 1u) {
            status = append_segment(
                out_view,
                bytes,
                size,
                vaddr,
                memsz,
                file_offset,
                filesz,
                elf_segment_perms(flags),
                (uint32_t)i);
            if (status != XAIR_OK) {
                return status;
            }
        }
    }
    return out_view->segment_count == 0 ? XAIR_ERR_RANGE : XAIR_OK;
}

static xair_status parse_elf_view(const uint8_t *bytes, size_t size, xair_binary_view *out_view) {
    if (!range_ok(size, 0, 16u) || bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' || bytes[5] != 1u) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (bytes[4] == 1u) {
        return parse_elf32_view(bytes, size, out_view);
    }
    if (bytes[4] == 2u) {
        return parse_elf64_view(bytes, size, out_view);
    }
    return XAIR_ERR_UNSUPPORTED;
}

const char *xair_binary_format_name(xair_binary_format format) {
    switch (format) {
    case XAIR_BINARY_FORMAT_ELF:
        return "elf";
    case XAIR_BINARY_FORMAT_PE:
        return "pe";
    default:
        return "unknown";
    }
}

xair_status xair_binary_view_load_path(const char *path, xair_binary_view *out_view) {
    uint8_t *file_bytes = NULL;
    size_t file_size = 0;
    xair_status status;

    if (path == NULL || out_view == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(out_view, 0, sizeof(*out_view));
    out_view->format = XAIR_BINARY_FORMAT_UNKNOWN;
    status = read_file(path, &file_bytes, &file_size);
    if (status != XAIR_OK) {
        return status;
    }
    status = parse_elf_view(file_bytes, file_size, out_view);
    if (status == XAIR_ERR_UNSUPPORTED) {
        status = parse_pe_view(file_bytes, file_size, out_view);
    }
    free(file_bytes);
    if (status != XAIR_OK) {
        xair_binary_view_destroy(out_view);
    }
    return status;
}

void xair_binary_view_destroy(xair_binary_view *view) {
    size_t i;

    if (view == NULL) {
        return;
    }
    for (i = 0; i < view->segment_count; ++i) {
        free(view->segments[i].bytes);
    }
    free(view->segments);
    memset(view, 0, sizeof(*view));
}

const xair_binary_segment *xair_binary_view_find_segment(
    const xair_binary_view *view,
    uint64_t va,
    uint32_t required_perms) {
    size_t i;

    if (view == NULL) {
        return NULL;
    }
    for (i = 0; i < view->segment_count; ++i) {
        const xair_binary_segment *segment = &view->segments[i];

        if ((segment->perms & required_perms) == required_perms &&
            va >= segment->va && va - segment->va < segment->mem_size) {
            return segment;
        }
    }
    return NULL;
}

xair_status xair_binary_view_read(
    const xair_binary_view *view,
    uint64_t va,
    void *out_bytes,
    size_t size) {
    const xair_binary_segment *segment;
    uint64_t offset;

    if (view == NULL || out_bytes == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    segment = xair_binary_view_find_segment(view, va, 0);
    if (segment == NULL) {
        return XAIR_ERR_RANGE;
    }
    offset = va - segment->va;
    if (offset > segment->file_size || size > (size_t)(segment->file_size - offset)) {
        return XAIR_ERR_RANGE;
    }
    memcpy(out_bytes, segment->bytes + (size_t)offset, size);
    return XAIR_OK;
}

xair_status xair_binary_load_path(const char *path, xair_binary_image *out_image) {
    xair_binary_view view;
    const xair_binary_segment *segment;
    uint8_t *bytes;
    xair_status status;

    if (path == NULL || out_image == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(out_image, 0, sizeof(*out_image));
    status = xair_binary_view_load_path(path, &view);
    if (status != XAIR_OK) {
        return status;
    }
    segment = xair_binary_view_find_segment(&view, view.entry, XAIR_BINARY_PERM_EXEC);
    if (segment == NULL) {
        segment = xair_binary_view_find_segment(&view, view.entry, 0);
    }
    if (segment == NULL || segment->file_size > (uint64_t)SIZE_MAX) {
        xair_binary_view_destroy(&view);
        return XAIR_ERR_RANGE;
    }
    bytes = (uint8_t *)malloc((size_t)segment->file_size == 0 ? 1u : (size_t)segment->file_size);
    if (bytes == NULL) {
        xair_binary_view_destroy(&view);
        return XAIR_ERR_OOM;
    }
    if (segment->file_size != 0) {
        memcpy(bytes, segment->bytes, (size_t)segment->file_size);
    }
    out_image->format = view.format;
    out_image->arch = view.arch;
    out_image->entry = view.entry;
    out_image->image_base = view.image_base;
    out_image->segment_base = segment->va;
    out_image->segment_bytes = bytes;
    out_image->segment_size = (size_t)segment->file_size;
    xair_binary_view_destroy(&view);
    return XAIR_OK;
}

void xair_binary_image_destroy(xair_binary_image *image) {
    if (image == NULL) {
        return;
    }
    free(image->segment_bytes);
    memset(image, 0, sizeof(*image));
}
