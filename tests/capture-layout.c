#include <assert.h>
#include <string.h>

#include "../capture/layout.h"

int main(void)
{
    uint32_t width;
    uint32_t height;
    uur_capture_layout_stream streams[2];
    memset(streams, 0, sizeof(streams));

    streams[0].pixel_width = 2560;
    streams[0].pixel_height = 1600;
    streams[0].logical_width = 1280;
    streams[0].logical_height = 800;
    streams[0].has_geometry = 1;
    assert(uur_capture_layout(streams, 1, &width, &height));
    assert(width == 2560 && height == 1600);

    streams[0].logical_x = -1920;
    streams[0].logical_width = 1920;
    streams[0].logical_height = 1080;
    streams[0].pixel_width = 1920;
    streams[0].pixel_height = 1080;
    streams[1].logical_x = 0;
    streams[1].logical_width = 1280;
    streams[1].logical_height = 800;
    streams[1].pixel_width = 2560;
    streams[1].pixel_height = 1600;
    streams[1].has_geometry = 1;
    assert(uur_capture_layout(streams, 2, &width, &height));
    assert(width == 3200 && height == 1080);
    assert(streams[0].destination_x == 0);
    assert(streams[1].destination_x == 1920);
    assert(streams[1].destination_width == 1280);

    streams[0].has_geometry = 0;
    streams[1].has_geometry = 0;
    assert(uur_capture_layout(streams, 2, &width, &height));
    assert(width == 4480 && height == 1600);
    assert(streams[1].destination_x == 1920);

    streams[0].pixel_width = UUR_CAPTURE_MAX_DIMENSION;
    assert(!uur_capture_layout(streams, 2, &width, &height));
    return 0;
}
