#ifndef UUR_FRAME_COPY_H
#define UUR_FRAME_COPY_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>

/* The capture destination is often already a writable 32-bit DIB. Copy or
 * scale into it directly instead of passing the image through Wine's GDI
 * conversion and scaling path. Only the requested destination region changes.
 * Source frames are top-down; destination DIBs can have either orientation. */
static inline int uur_copy_frame_to_dib(
    const unsigned char *source, uint32_t source_width,
    uint32_t source_height, size_t source_stride, unsigned char *destination,
    uint32_t bitmap_width, uint32_t bitmap_height, size_t bitmap_stride,
    int top_down, int x, int y, int width, int height)
{
    if (source == NULL || destination == NULL || source_width == 0 ||
        source_height == 0 || bitmap_width == 0 || bitmap_height == 0 ||
        width <= 0 || height <= 0 || source_stride < (size_t)source_width * 4 ||
        bitmap_stride < (size_t)bitmap_width * 4)
        return 0;

    int64_t right = (int64_t)x + width;
    int64_t bottom = (int64_t)y + height;
    int64_t left = x > 0 ? x : 0;
    int64_t top = y > 0 ? y : 0;
    if (right > bitmap_width)
        right = bitmap_width;
    if (bottom > bitmap_height)
        bottom = bitmap_height;
    if (right <= left || bottom <= top)
        return 0;

    size_t row_bytes = (size_t)(right - left) * 4;
    uint64_t horizontal_start = (uint64_t)(left - x) * source_width;
    uint64_t horizontal_step = source_width / (uint32_t)width;
    uint64_t horizontal_remainder = source_width % (uint32_t)width;
    uint32_t previous_source_y = UINT32_MAX;
    unsigned char *previous_row = NULL;
    for (int64_t dy = top; dy < bottom; ++dy) {
        uint32_t sy = (uint32_t)((uint64_t)(dy - y) * source_height /
                                  (uint32_t)height);
        uint32_t bitmap_y = top_down ? (uint32_t)dy
                                    : bitmap_height - 1 - (uint32_t)dy;
        unsigned char *row = destination + (size_t)bitmap_y * bitmap_stride +
                             (size_t)left * 4;
        if (sy == previous_source_y) {
            memcpy(row, previous_row, row_bytes);
        } else if (source_width == (uint32_t)width) {
            memcpy(row, source + (size_t)sy * source_stride +
                            (size_t)(left - x) * 4, row_bytes);
        } else {
            const unsigned char *source_row = source + (size_t)sy * source_stride;
            uint64_t sx = horizontal_start / (uint32_t)width;
            uint64_t remainder = horizontal_start % (uint32_t)width;
            for (int64_t dx = left; dx < right; ++dx) {
                memcpy(row + (size_t)(dx - left) * 4, source_row + sx * 4, 4);
                sx += horizontal_step;
                remainder += horizontal_remainder;
                if (remainder >= (uint32_t)width) {
                    ++sx;
                    remainder -= (uint32_t)width;
                }
            }
        }
        previous_source_y = sy;
        previous_row = row;
    }
    return 1;
}

#endif
