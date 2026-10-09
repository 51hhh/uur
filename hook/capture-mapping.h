#ifndef UUR_CAPTURE_MAPPING_H
#define UUR_CAPTURE_MAPPING_H

#include <stddef.h>
#include <stdint.h>
#include <limits.h>

typedef struct uur_capture_region {
    int x;
    int y;
    int width;
    int height;
} uur_capture_region;

typedef struct uur_frame_geometry {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
    int available;
} uur_frame_geometry;

#define UUR_POINTER_NORMALIZED_MAX 65535

static inline int32_t uur_clamp_normalized_pointer(int64_t value)
{
    if (value < 0)
        return 0;
    if (value > UUR_POINTER_NORMALIZED_MAX)
        return UUR_POINTER_NORMALIZED_MAX;
    return (int32_t)value;
}

/* SendInput uses two different absolute coordinate spaces. Without
 * MOUSEEVENTF_VIRTUALDESK, dx/dy cover only the primary monitor; with it they
 * cover the complete virtual desktop. Linux injection backends operate on the
 * complete desktop, so convert the primary-monitor form before forwarding it.
 * The primary monitor always starts at Windows desktop coordinate (0,0). */
static inline void uur_absolute_pointer_to_virtual(
    int32_t *x, int32_t *y, int already_virtual, int virtual_x, int virtual_y,
    int virtual_width, int virtual_height, int primary_x, int primary_y,
    int primary_width, int primary_height)
{
    if (x == NULL || y == NULL)
        return;

    int32_t normalized_x = uur_clamp_normalized_pointer(*x);
    int32_t normalized_y = uur_clamp_normalized_pointer(*y);
    if (already_virtual || virtual_width <= 1 || virtual_height <= 1 ||
        primary_width <= 0 || primary_height <= 0) {
        *x = normalized_x;
        *y = normalized_y;
        return;
    }

    int64_t relative_x = ((int64_t)primary_x - virtual_x) *
        UUR_POINTER_NORMALIZED_MAX + (int64_t)normalized_x * (primary_width - 1);
    int64_t relative_y = ((int64_t)primary_y - virtual_y) *
        UUR_POINTER_NORMALIZED_MAX + (int64_t)normalized_y * (primary_height - 1);
    *x = uur_clamp_normalized_pointer(
        (relative_x + (virtual_width - 1) / 2) / (virtual_width - 1));
    *y = uur_clamp_normalized_pointer(
        (relative_y + (virtual_height - 1) / 2) / (virtual_height - 1));
}

/* Keep the fractional desktop position through this conversion. Rounding to
 * a Windows pixel and then a monitor pixel biases every target to top-left,
 * especially when Wine and Portal expose different DPI scales. */
static inline int uur_virtual_pointer_to_monitor(
    int32_t *x, int32_t *y, int virtual_x, int virtual_y, int virtual_width,
    int virtual_height, int monitor_x, int monitor_y, int monitor_width,
    int monitor_height)
{
    if (x == NULL || y == NULL || virtual_width <= 0 || virtual_height <= 0 ||
        monitor_width <= 0 || monitor_height <= 0)
        return 0;
    int64_t relative_x = ((int64_t)virtual_x - monitor_x) *
        UUR_POINTER_NORMALIZED_MAX +
        (int64_t)uur_clamp_normalized_pointer(*x) * (virtual_width - 1);
    int64_t relative_y = ((int64_t)virtual_y - monitor_y) *
        UUR_POINTER_NORMALIZED_MAX +
        (int64_t)uur_clamp_normalized_pointer(*y) * (virtual_height - 1);
    *x = monitor_width > 1 ? uur_clamp_normalized_pointer(
        (relative_x + (monitor_width - 1) / 2) / (monitor_width - 1)) : 0;
    *y = monitor_height > 1 ? uur_clamp_normalized_pointer(
        (relative_y + (monitor_height - 1) / 2) / (monitor_height - 1)) : 0;
    return 1;
}

/* Convert normalized coordinates inside one monitor into normalized
 * coordinates for Portal's complete logical desktop. This deliberately uses
 * the selected monitor's own dimensions: Wine and the compositor can place
 * the same mixed-DPI monitor at different global offsets. */
static inline int uur_monitor_pointer_to_portal(
    int32_t *x, int32_t *y, int32_t monitor_x, int32_t monitor_y,
    uint32_t monitor_width, uint32_t monitor_height, int32_t desktop_x,
    int32_t desktop_y, uint32_t desktop_width, uint32_t desktop_height)
{
    if (x == NULL || y == NULL || monitor_width == 0 || monitor_height == 0 ||
        desktop_width == 0 || desktop_height == 0)
        return 0;

    int32_t normalized_x = uur_clamp_normalized_pointer(*x);
    int32_t normalized_y = uur_clamp_normalized_pointer(*y);
    int64_t global_x = ((int64_t)monitor_x - desktop_x) *
        UUR_POINTER_NORMALIZED_MAX + (int64_t)normalized_x * (monitor_width - 1);
    int64_t global_y = ((int64_t)monitor_y - desktop_y) *
        UUR_POINTER_NORMALIZED_MAX + (int64_t)normalized_y * (monitor_height - 1);
    *x = desktop_width > 1
        ? uur_clamp_normalized_pointer(
              (global_x + (desktop_width - 1) / 2) / (desktop_width - 1))
        : 0;
    *y = desktop_height > 1
        ? uur_clamp_normalized_pointer(
              (global_y + (desktop_height - 1) / 2) / (desktop_height - 1))
        : 0;
    return 1;
}

static inline int64_t uur_max_i64(int64_t left, int64_t right)
{
    return left > right ? left : right;
}

static inline int64_t uur_min_i64(int64_t left, int64_t right)
{
    return left < right ? left : right;
}

/* Portal monitor positions are often normalized so the top-left output starts
 * at (0,0), while Windows preserves a negative virtual-screen origin. Map the
 * complete portal bounding box onto the coordinate system returned by
 * GetSystemMetrics in the hooked process. This also handles Wine's global DPI
 * scaling because the source blit and these metrics use the same units. */
static inline int uur_align_frame_geometries(uur_frame_geometry *streams,
                                             size_t stream_count,
                                             int virtual_x, int virtual_y,
                                             int virtual_width,
                                             int virtual_height)
{
    if (streams == NULL || stream_count == 0 || virtual_width <= 0 ||
        virtual_height <= 0)
        return 0;
    int64_t min_x = INT64_MAX;
    int64_t min_y = INT64_MAX;
    int64_t max_x = INT64_MIN;
    int64_t max_y = INT64_MIN;
    int found = 0;
    for (size_t i = 0; i < stream_count; ++i) {
        if (!streams[i].available || streams[i].width == 0 ||
            streams[i].height == 0)
            continue;
        int64_t right = (int64_t)streams[i].x + streams[i].width;
        int64_t bottom = (int64_t)streams[i].y + streams[i].height;
        found = 1;
        min_x = uur_min_i64(min_x, streams[i].x);
        min_y = uur_min_i64(min_y, streams[i].y);
        max_x = uur_max_i64(max_x, right);
        max_y = uur_max_i64(max_y, bottom);
    }
    if (!found)
        return 0;
    int64_t portal_width = max_x - min_x;
    int64_t portal_height = max_y - min_y;
    if (portal_width <= 0 || portal_height <= 0)
        return 0;

    for (size_t i = 0; i < stream_count; ++i) {
        if (!streams[i].available)
            continue;
        int64_t mapped_x = virtual_x +
            ((int64_t)streams[i].x - min_x) * virtual_width / portal_width;
        int64_t mapped_y = virtual_y +
            ((int64_t)streams[i].y - min_y) * virtual_height / portal_height;
        uint64_t mapped_width =
            (uint64_t)streams[i].width * (uint64_t)virtual_width /
            (uint64_t)portal_width;
        uint64_t mapped_height =
            (uint64_t)streams[i].height * (uint64_t)virtual_height /
            (uint64_t)portal_height;
        if (mapped_x < INT32_MIN || mapped_x > INT32_MAX ||
            mapped_y < INT32_MIN || mapped_y > INT32_MAX ||
            mapped_width == 0 || mapped_width > UINT32_MAX ||
            mapped_height == 0 || mapped_height > UINT32_MAX)
            return 0;
        streams[i].x = (int32_t)mapped_x;
        streams[i].y = (int32_t)mapped_y;
        streams[i].width = (uint32_t)mapped_width;
        streams[i].height = (uint32_t)mapped_height;
    }
    return 1;
}

/* UU enumerates Windows displays separately and captures each display with a
 * GDI source rectangle. Pick the independent portal stream whose logical
 * desktop rectangle overlaps that request most. This remains correct for a
 * negative virtual-desktop origin and for source rectangles expressed at a
 * higher DPI scale because their monitor region still overlaps the same
 * logical display. */
static inline int uur_select_capture_stream(const uur_frame_geometry *streams,
                                            size_t stream_count, int source_x,
                                            int source_y, int source_width,
                                            int source_height)
{
    if (streams == NULL || stream_count == 0 || source_width <= 0 ||
        source_height <= 0)
        return -1;

    int selected = -1;
    uint64_t selected_area = 0;
    int64_t source_right = (int64_t)source_x + source_width;
    int64_t source_bottom = (int64_t)source_y + source_height;
    for (size_t i = 0; i < stream_count; ++i) {
        if (!streams[i].available || streams[i].width == 0 ||
            streams[i].height == 0)
            continue;
        int64_t right = (int64_t)streams[i].x + streams[i].width;
        int64_t bottom = (int64_t)streams[i].y + streams[i].height;
        int64_t overlap_width =
            uur_min_i64(source_right, right) -
            uur_max_i64(source_x, streams[i].x);
        int64_t overlap_height =
            uur_min_i64(source_bottom, bottom) -
            uur_max_i64(source_y, streams[i].y);
        uint64_t area = overlap_width > 0 && overlap_height > 0
                            ? (uint64_t)overlap_width * overlap_height
                            : 0;
        if (area > selected_area) {
            selected = (int)i;
            selected_area = area;
        }
    }
    if (selected >= 0)
        return selected;

    /* A compositor and Wine can expose different DPI coordinate scales. If
     * there is no direct overlap, choose the stream nearest the request's
     * center so an independently scaled secondary monitor still routes. */
    uint64_t best_distance = UINT64_MAX;
    int64_t source_center_x = (int64_t)source_x * 2 + source_width;
    int64_t source_center_y = (int64_t)source_y * 2 + source_height;
    for (size_t i = 0; i < stream_count; ++i) {
        if (!streams[i].available || streams[i].width == 0 ||
            streams[i].height == 0)
            continue;
        int64_t center_x = (int64_t)streams[i].x * 2 + streams[i].width;
        int64_t center_y = (int64_t)streams[i].y * 2 + streams[i].height;
        uint64_t dx = (uint64_t)(source_center_x > center_x
                                     ? source_center_x - center_x
                                     : center_x - source_center_x);
        uint64_t dy = (uint64_t)(source_center_y > center_y
                                     ? source_center_y - center_y
                                     : center_y - source_center_y);
        uint64_t distance = dx > UINT32_MAX || dy > UINT32_MAX
                                ? UINT64_MAX
                                : dx * dx + dy * dy;
        if (distance < best_distance) {
            selected = (int)i;
            best_distance = distance;
        }
    }
    return selected;
}

static inline int uur_portal_frame_region(int frame_width, int frame_height,
                                          uur_capture_region *region)
{
    if (region == NULL || frame_width <= 0 || frame_height <= 0)
        return 0;
    region->x = 0;
    region->y = 0;
    region->width = frame_width;
    region->height = frame_height;
    return 1;
}

#endif
