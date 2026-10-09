#include <assert.h>
#include <math.h>
#include <stdlib.h>

#include "../hook/capture-mapping.h"

int main(void)
{
    uur_capture_region region;
    uur_frame_geometry streams[2] = {
        {.x = 0, .y = 0, .width = 1920, .height = 1200, .available = 1},
        {.x = -2560,
         .y = -408,
         .width = 2560,
         .height = 1440,
         .available = 1},
    };

    /* A 200% logical desktop still consumes the complete physical frame. */
    assert(uur_portal_frame_region(2560, 1600, &region));
    assert(region.x == 0 && region.y == 0);
    assert(region.width == 2560 && region.height == 1600);

    /* A selected monitor is independent of Wine's wider virtual desktop and
     * of negative monitor origins. */
    assert(uur_portal_frame_region(1920, 1080, &region));
    assert(region.x == 0 && region.y == 0);
    assert(region.width == 1920 && region.height == 1080);

    assert(!uur_portal_frame_region(0, 1080, &region));
    assert(!uur_portal_frame_region(1920, -1, &region));
    assert(!uur_portal_frame_region(1920, 1080, 0));

    assert(uur_select_capture_stream(streams, 2, 0, 0, 1920, 1200) == 0);
    assert(uur_select_capture_stream(streams, 2, -2560, -408, 2560, 1440) ==
           1);
    /* Wine can expose the same monitor rectangle in physical DPI units. */
    assert(uur_select_capture_stream(streams, 2, -5120, -816, 5120, 2880) ==
           1);

    /* GNOME's Portal normalizes the left monitor to (0,0), while Wine keeps
     * that monitor at a negative virtual-screen origin. */
    uur_frame_geometry normalized[2] = {
        {.x = 0, .y = 0, .width = 2560, .height = 1440, .available = 1},
        {.x = 2560,
         .y = 408,
         .width = 1920,
         .height = 1200,
         .available = 1},
    };
    assert(uur_align_frame_geometries(normalized, 2, -2560, -408, 4480,
                                      1608));
    assert(normalized[0].x == -2560 && normalized[0].y == -408);
    assert(normalized[0].width == 2560 && normalized[0].height == 1440);
    assert(normalized[1].x == 0 && normalized[1].y == 0);
    assert(normalized[1].width == 1920 && normalized[1].height == 1200);
    assert(uur_select_capture_stream(normalized, 2, 0, 0, 1920, 1200) == 1);
    assert(uur_select_capture_stream(normalized, 2, -2560, -408, 2560, 1440) ==
           0);

    uur_frame_geometry physical[2] = {
        {.x = 0, .y = 0, .width = 2560, .height = 1440, .available = 1},
        {.x = 2560,
         .y = 408,
         .width = 1920,
         .height = 1200,
         .available = 1},
    };
    assert(uur_align_frame_geometries(physical, 2, -5120, -816, 8960, 3216));
    assert(physical[0].x == -5120 && physical[0].y == -816);
    assert(physical[0].width == 5120 && physical[0].height == 2880);
    assert(physical[1].x == 0 && physical[1].y == 0);
    assert(physical[1].width == 3840 && physical[1].height == 2400);

    streams[1].available = 0;
    assert(uur_select_capture_stream(streams, 2, -2560, -408, 2560, 1440) ==
           0);
    assert(uur_select_capture_stream(streams, 0, 0, 0, 1, 1) == -1);

    /* Absolute SendInput without VIRTUALDESK is relative to the primary
     * monitor. Convert it to the complete virtual desktop used by Linux. */
    int32_t pointer_x = 0;
    int32_t pointer_y = 0;
    uur_absolute_pointer_to_virtual(&pointer_x, &pointer_y, 0, -2560, -408,
                                    4480, 1608, 0, 0, 1920, 1200);
    assert(pointer_x > 37450 && pointer_x < 37470);
    assert(pointer_y > 16630 && pointer_y < 16650);

    pointer_x = 65535;
    pointer_y = 65535;
    uur_absolute_pointer_to_virtual(&pointer_x, &pointer_y, 0, -2560, -408,
                                    4480, 1608, 0, 0, 1920, 1200);
    assert(pointer_x == 65535);
    assert(pointer_y == 65535);

    pointer_x = 12345;
    pointer_y = 54321;
    uur_absolute_pointer_to_virtual(&pointer_x, &pointer_y, 1, -2560, -408,
                                    4480, 1608, 0, 0, 1920, 1200);
    assert(pointer_x == 12345 && pointer_y == 54321);

    /* Wine can vertically offset its primary display differently from Portal.
     * Monitor-local mapping keeps the center at the center of Portal's stream. */
    pointer_x = 32768;
    pointer_y = 32768;
    assert(uur_monitor_pointer_to_portal(&pointer_x, &pointer_y, 2560, 408,
                                         1920, 1200, 0, 0, 4480, 1608));
    assert(pointer_x > 51470 && pointer_x < 51510);
    assert(pointer_y == 41087);

    /* A repeated normalized conversion must retain subpixel precision instead
     * of losing one pixel at each intermediate DPI/monitor coordinate space. */
    for (int32_t position = 6554; position < 65535; position += 6553) {
        pointer_x = position;
        pointer_y = position;
        assert(uur_monitor_pointer_to_portal(&pointer_x, &pointer_y, 2560, 408,
                                             1920, 1200, 0, 0, 4480, 1608));
        double expected_x = 2560.0 + position * 1919.0 / 65535.0;
        double expected_y = 408.0 + position * 1199.0 / 65535.0;
        assert(fabs(pointer_x * 4479.0 / 65535.0 - expected_x) < 0.04);
        assert(fabs(pointer_y * 1607.0 / 65535.0 - expected_y) < 0.02);

        assert(uur_virtual_pointer_to_monitor(&pointer_x, &pointer_y,
                    -2560, -408, 4480, 1608, 0, 0, 1920, 1200));
        assert(abs(pointer_x - position) <= 2);
        assert(abs(pointer_y - position) <= 2);
    }
    return 0;
}
