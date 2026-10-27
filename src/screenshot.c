#include "screenshot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PNG_BLOCK_SIZE 65535

static void
write_u32_be(FILE *file, uint32_t value)
{
    uint8_t bytes[4] = { (uint8_t)(value >> 24), (uint8_t)(value >> 16), (uint8_t)(value >> 8),
                         (uint8_t)value };
    fwrite(bytes, 1, sizeof(bytes), file);
}

static uint32_t
update_crc(uint32_t crc, const uint8_t *bytes, size_t size)
{
    for (size_t i = 0; i < size; i++) {
        crc ^= bytes[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return crc;
}

static void
write_chunk(FILE *file, const char *type, const uint8_t *data, size_t size)
{
    write_u32_be(file, (uint32_t)size);
    uint32_t crc = update_crc(0xffffffffu, (const uint8_t *)type, 4);
    crc = update_crc(crc, data, size);
    fwrite(type, 1, 4, file);
    if (size != 0)
        fwrite(data, 1, size, file);
    write_u32_be(file, ~crc);
}

static void
encode_pixels(const Gpu *gpu, uint8_t *pixels)
{
    size_t stride = (size_t)gpu->width * 3 + 1;
    for (uint32_t y = 0; y < gpu->height; y++) {
        uint8_t *row = pixels + y * stride;
        row[0] = 0; // No PNG row filter.
        for (uint32_t x = 0; x < gpu->width; x++) {
            uint32_t pixel = gpu->front[(size_t)y * gpu->width + x];
            row[1 + x * 3] = (uint8_t)(pixel >> 16);
            row[2 + x * 3] = (uint8_t)(pixel >> 8);
            row[3 + x * 3] = (uint8_t)pixel;
        }
    }
}

// zlib with stored blocks, followed by an Adler-32 checksum.
static size_t
encode_blocks(const uint8_t *pixels, size_t size, uint8_t *output)
{
    size_t offset = 0;
    output[offset++] = 0x78;
    output[offset++] = 0x01;
    uint32_t sum = 1, cumulative_sum = 0;
    for (size_t start = 0; start < size; start += PNG_BLOCK_SIZE) {
        size_t length = size - start < PNG_BLOCK_SIZE ? size - start : PNG_BLOCK_SIZE;
        output[offset++] = start + length == size ? 1 : 0;
        output[offset++] = (uint8_t)length;
        output[offset++] = (uint8_t)(length >> 8);
        output[offset++] = (uint8_t)~length;
        output[offset++] = (uint8_t)(~length >> 8);
        memcpy(output + offset, pixels + start, length);
        offset += length;
        for (size_t i = 0; i < length; i++) {
            sum = (sum + pixels[start + i]) % 65521;
            cumulative_sum = (cumulative_sum + sum) % 65521;
        }
    }
    uint32_t adler = cumulative_sum << 16 | sum;
    for (int i = 0; i < 4; i++)
        output[offset++] = (uint8_t)(adler >> (24 - 8 * i));
    return offset;
}

bool
screenshot_write(const Gpu *gpu, const char *path)
{
    if (gpu->width == 0 || gpu->height == 0)
        return false;
    size_t pixel_size = ((size_t)gpu->width * 3 + 1) * gpu->height;
    size_t blocks = (pixel_size + PNG_BLOCK_SIZE - 1) / PNG_BLOCK_SIZE;
    uint8_t *pixels = malloc(pixel_size);
    uint8_t *compressed = malloc(2 + pixel_size + blocks * 5 + 4);
    FILE *file = NULL;
    bool ok = false;
    if (pixels == NULL || compressed == NULL)
        goto cleanup;
    file = fopen(path, "wb");
    if (file == NULL)
        goto cleanup;

    fwrite("\x89PNG\r\n\x1a\n", 1, 8, file);
    uint8_t header[13] = { 0 };
    for (int i = 0; i < 4; i++) {
        header[i] = (uint8_t)(gpu->width >> (24 - 8 * i));
        header[4 + i] = (uint8_t)(gpu->height >> (24 - 8 * i));
    }
    header[8] = 8; // Eight bits per channel.
    header[9] = 2; // RGB.
    write_chunk(file, "IHDR", header, sizeof(header));
    encode_pixels(gpu, pixels);
    size_t compressed_size = encode_blocks(pixels, pixel_size, compressed);
    write_chunk(file, "IDAT", compressed, compressed_size);
    write_chunk(file, "IEND", NULL, 0);
    ok = !ferror(file);

cleanup:
    if (file != NULL && fclose(file) != 0)
        ok = false;
    free(pixels);
    free(compressed);
    return ok;
}
