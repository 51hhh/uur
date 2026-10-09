#ifndef UUR_CAPTURE_LAYOUT_H
#define UUR_CAPTURE_LAYOUT_H

#include <stddef.h>
#include <stdint.h>

#define UUR_CAPTURE_MAX_DIMENSION 32768

typedef struct uur_capture_layout_stream {
    int32_t logical_x;
    int32_t logical_y;
    uint32_t logical_width;
    uint32_t logical_height;
    int has_geometry;
    uint32_t pixel_width;
    uint32_t pixel_height;
    uint32_t destination_x;
    uint32_t destination_y;
    uint32_t destination_width;
    uint32_t destination_height;
} uur_capture_layout_stream;

/* Build one canvas from Portal monitor metadata. A single stream keeps its
 * physical resolution. Multiple streams use the compositor's logical space so
 * mixed-scale monitors share one stable coordinate system. When a backend
 * omits positions, arrange the native frames from left to right. */
static inline int uur_capture_layout(uur_capture_layout_stream *streams,
                                     size_t stream_count, uint32_t *width,
                                     uint32_t *height)
{
    if (streams == NULL || stream_count == 0 || width == NULL || height == NULL)
        return 0;

    if (stream_count == 1) {
        if (streams[0].pixel_width == 0 || streams[0].pixel_height == 0)
            return 0;
        *width = streams[0].pixel_width;
        *height = streams[0].pixel_height;
        streams[0].destination_x = 0;
        streams[0].destination_y = 0;
        streams[0].destination_width = streams[0].pixel_width;
        streams[0].destination_height = streams[0].pixel_height;
        return *width <= UUR_CAPTURE_MAX_DIMENSION &&
               *height <= UUR_CAPTURE_MAX_DIMENSION;
    }

    int has_geometry = 1;
    for (size_t i = 0; i < stream_count; ++i)
        has_geometry = has_geometry && streams[i].has_geometry &&
                       streams[i].logical_width > 0 &&
                       streams[i].logical_height > 0;

    if (has_geometry) {
        int64_t min_x = INT64_MAX;
        int64_t min_y = INT64_MAX;
        int64_t max_x = INT64_MIN;
        int64_t max_y = INT64_MIN;
        for (size_t i = 0; i < stream_count; ++i) {
            int64_t right =
                (int64_t)streams[i].logical_x + streams[i].logical_width;
            int64_t bottom =
                (int64_t)streams[i].logical_y + streams[i].logical_height;
            if (streams[i].logical_x < min_x)
                min_x = streams[i].logical_x;
            if (streams[i].logical_y < min_y)
                min_y = streams[i].logical_y;
            if (right > max_x)
                max_x = right;
            if (bottom > max_y)
                max_y = bottom;
        }
        int64_t canvas_width = max_x - min_x;
        int64_t canvas_height = max_y - min_y;
        if (canvas_width <= 0 || canvas_height <= 0 ||
            canvas_width > UUR_CAPTURE_MAX_DIMENSION ||
            canvas_height > UUR_CAPTURE_MAX_DIMENSION)
            return 0;
        *width = (uint32_t)canvas_width;
        *height = (uint32_t)canvas_height;
        for (size_t i = 0; i < stream_count; ++i) {
            streams[i].destination_x =
                (uint32_t)((int64_t)streams[i].logical_x - min_x);
            streams[i].destination_y =
                (uint32_t)((int64_t)streams[i].logical_y - min_y);
            streams[i].destination_width = streams[i].logical_width;
            streams[i].destination_height = streams[i].logical_height;
        }
        return 1;
    }

    uint64_t canvas_width = 0;
    uint32_t canvas_height = 0;
    for (size_t i = 0; i < stream_count; ++i) {
        if (streams[i].pixel_width == 0 || streams[i].pixel_height == 0)
            return 0;
        streams[i].destination_x = (uint32_t)canvas_width;
        streams[i].destination_y = 0;
        streams[i].destination_width = streams[i].pixel_width;
        streams[i].destination_height = streams[i].pixel_height;
        canvas_width += streams[i].pixel_width;
        if (streams[i].pixel_height > canvas_height)
            canvas_height = streams[i].pixel_height;
    }
    if (canvas_width > UUR_CAPTURE_MAX_DIMENSION ||
        canvas_height > UUR_CAPTURE_MAX_DIMENSION)
        return 0;
    *width = (uint32_t)canvas_width;
    *height = canvas_height;
    return 1;
}

#endif
