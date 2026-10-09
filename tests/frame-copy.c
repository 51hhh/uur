#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "../hook/frame-copy.h"

int main(void)
{
    const uint32_t source[4] = {1, 2, 3, 4};
    uint32_t destination[24];
    memset(destination, 0, sizeof(destination));
    assert(uur_copy_frame_to_dib((const unsigned char *)source, 2, 2, 8,
                                 (unsigned char *)destination, 4, 4, 24, 1,
                                 0, 0, 4, 4));
    assert(destination[0] == 1 && destination[1] == 1);
    assert(destination[2] == 2 && destination[3] == 2);
    assert(destination[6] == 1 && destination[9] == 2);
    assert(destination[12] == 3 && destination[15] == 4);
    assert(destination[4] == 0 && destination[5] == 0);

    memset(destination, 0, sizeof(destination));
    assert(uur_copy_frame_to_dib((const unsigned char *)source, 2, 2, 8,
                                 (unsigned char *)destination, 4, 4, 16, 0,
                                 1, 1, 2, 2));
    assert(destination[9] == 1 && destination[10] == 2);
    assert(destination[5] == 3 && destination[6] == 4);
    assert(destination[0] == 0 && destination[15] == 0);

    memset(destination, 0, sizeof(destination));
    assert(uur_copy_frame_to_dib((const unsigned char *)source, 2, 2, 8,
                                 (unsigned char *)destination, 2, 2, 8, 1,
                                 -1, -1, 2, 2));
    assert(destination[0] == 4 && destination[1] == 0);

    const uint32_t uneven_source[6] = {1, 2, 3, 4, 5, 6};
    memset(destination, 0, sizeof(destination));
    assert(uur_copy_frame_to_dib((const unsigned char *)uneven_source, 3, 2, 12,
                                 (unsigned char *)destination, 5, 3, 20, 1,
                                 0, 0, 5, 3));
    for (int dy = 0; dy < 3; ++dy)
        for (int dx = 0; dx < 5; ++dx)
            assert(destination[dy * 5 + dx] ==
                    uneven_source[(dy * 2 / 3) * 3 + dx * 3 / 5]);

    assert(!uur_copy_frame_to_dib(NULL, 2, 2, 8,
                                  (unsigned char *)destination, 2, 2, 8, 1,
                                  0, 0, 2, 2));
    assert(!uur_copy_frame_to_dib((const unsigned char *)source, 2, 2, 4,
                                  (unsigned char *)destination, 2, 2, 8, 1,
                                  0, 0, 2, 2));
    return 0;
}
