#include "xair/xair_binary.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XAIR_PE_MACHINE_I386 0x014cu
#define XAIR_PE_MACHINE_AMD64 0x8664u
#define XAIR_ELF_MACHINE_386 3u
#define XAIR_ELF_MACHINE_X86_64 62u

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

static xair_status copy_segment(
    const uint8_t *file_bytes,
    size_t file_size,
    uint64_t file_offset,
    uint64_t virtual_address,
    uint64_t file_bytes_size,
    xair_binary_image *out_image) {
    uint8_t *segment;

    if (file_offset > (uint64_t)SIZE_MAX || file_bytes_size > (uint64_t)SIZE_MAX ||
        !range_ok(file_size, (size_t)file_offset, (size_t)file_bytes_size)) {
        return XAIR_ERR_RANGE;
    }
    segment = (uint8_t *)malloc((size_t)file_bytes_size == 0 ? 1u : (size_t)file_bytes_size);
    if (segment == NULL) {
        return XAIR_ERR_OOM;
    }
    memcpy(segment, file_bytes + (size_t)file_offset, (size_t)file_bytes_size);
    out_image->segment_base = virtual_address;
    out_image->segment_bytes = segment;
    out_image->segment_size = (size_t)file_bytes_size;
    return XAIR_OK;
}

static int rva_in_range(uint64_t rva, uint64_t start, uint64_t virtual_size, uint64_t raw_size) {
    uint64_t size = virtual_size > raw_size ? virtual_size : raw_size;

    return rva >= start && rva - start < size;
}

static xair_status parse_pe(const uint8_t *bytes, size_t size, xair_binary_image *out_image) {
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
        out_image->arch = XAIR_ARCH_X86_64;
        image_base = read_le64(bytes, optional_offset + 24u);
    } else if (machine == XAIR_PE_MACHINE_I386 && magic == 0x10bu) {
        out_image->arch = XAIR_ARCH_X86_32;
        image_base = read_le32(bytes, optional_offset + 28u);
    } else {
        return XAIR_ERR_UNSUPPORTED;
    }
    out_image->format = XAIR_BINARY_FORMAT_PE;
    out_image->image_base = image_base;
    out_image->entry = image_base + entry_rva;
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

        if (rva_in_range(entry_rva, virtual_address, virtual_size, raw_size)) {
            return copy_segment(
                bytes,
                size,
                raw_offset,
                image_base + virtual_address,
                raw_size,
                out_image);
        }
    }
    return XAIR_ERR_RANGE;
}

static xair_status parse_elf64(const uint8_t *bytes, size_t size, xair_binary_image *out_image) {
    uint16_t machine;
    uint64_t entry;
    uint64_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    size_t i;

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
        !range_ok(size, (size_t)phoff, (size_t)phentsize * phnum)) {
        return XAIR_ERR_RANGE;
    }
    out_image->format = XAIR_BINARY_FORMAT_ELF;
    out_image->arch = XAIR_ARCH_X86_64;
    out_image->entry = entry;
    out_image->image_base = 0;
    for (i = 0; i < phnum; ++i) {
        size_t off = (size_t)phoff + i * phentsize;
        uint32_t type = read_le32(bytes, off);
        uint32_t flags = read_le32(bytes, off + 4u);
        uint64_t file_offset = read_le64(bytes, off + 8u);
        uint64_t vaddr = read_le64(bytes, off + 16u);
        uint64_t filesz = read_le64(bytes, off + 32u);
        uint64_t memsz = read_le64(bytes, off + 40u);

        if (type == 1u && (flags & 1u) != 0 && entry >= vaddr && entry - vaddr < memsz) {
            return copy_segment(bytes, size, file_offset, vaddr, filesz, out_image);
        }
    }
    return XAIR_ERR_RANGE;
}

static xair_status parse_elf32(const uint8_t *bytes, size_t size, xair_binary_image *out_image) {
    uint16_t machine;
    uint32_t entry;
    uint32_t phoff;
    uint16_t phentsize;
    uint16_t phnum;
    size_t i;

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
    if (phentsize < 32u || !range_ok(size, phoff, (size_t)phentsize * phnum)) {
        return XAIR_ERR_RANGE;
    }
    out_image->format = XAIR_BINARY_FORMAT_ELF;
    out_image->arch = XAIR_ARCH_X86_32;
    out_image->entry = entry;
    out_image->image_base = 0;
    for (i = 0; i < phnum; ++i) {
        size_t off = phoff + i * phentsize;
        uint32_t type = read_le32(bytes, off);
        uint32_t file_offset = read_le32(bytes, off + 4u);
        uint32_t vaddr = read_le32(bytes, off + 8u);
        uint32_t filesz = read_le32(bytes, off + 16u);
        uint32_t memsz = read_le32(bytes, off + 20u);
        uint32_t flags = read_le32(bytes, off + 24u);

        if (type == 1u && (flags & 1u) != 0 && entry >= vaddr && entry - vaddr < memsz) {
            return copy_segment(bytes, size, file_offset, vaddr, filesz, out_image);
        }
    }
    return XAIR_ERR_RANGE;
}

static xair_status parse_elf(const uint8_t *bytes, size_t size, xair_binary_image *out_image) {
    if (!range_ok(size, 0, 16u) || bytes[0] != 0x7fu || bytes[1] != 'E' ||
        bytes[2] != 'L' || bytes[3] != 'F' || bytes[5] != 1u) {
        return XAIR_ERR_UNSUPPORTED;
    }
    if (bytes[4] == 1u) {
        return parse_elf32(bytes, size, out_image);
    }
    if (bytes[4] == 2u) {
        return parse_elf64(bytes, size, out_image);
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

xair_status xair_binary_load_path(const char *path, xair_binary_image *out_image) {
    uint8_t *file_bytes = NULL;
    size_t file_size = 0;
    xair_status status;

    if (path == NULL || out_image == NULL) {
        return XAIR_ERR_BAD_ARG;
    }
    memset(out_image, 0, sizeof(*out_image));
    out_image->format = XAIR_BINARY_FORMAT_UNKNOWN;
    status = read_file(path, &file_bytes, &file_size);
    if (status != XAIR_OK) {
        return status;
    }
    status = parse_elf(file_bytes, file_size, out_image);
    if (status == XAIR_ERR_UNSUPPORTED) {
        status = parse_pe(file_bytes, file_size, out_image);
    }
    free(file_bytes);
    if (status != XAIR_OK) {
        xair_binary_image_destroy(out_image);
    }
    return status;
}

void xair_binary_image_destroy(xair_binary_image *image) {
    if (image == NULL) {
        return;
    }
    free(image->segment_bytes);
    memset(image, 0, sizeof(*image));
}
