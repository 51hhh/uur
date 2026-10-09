#include <assert.h>
#include <stdint.h>

#include "../hook/input-mapping.h"

int main(void)
{
    assert(uur_virtual_key_supported('A'));
    assert(uur_virtual_key_supported(0x111)); /* extended right control */
    assert(uur_virtual_key_supported(0x10d)); /* keypad enter */
    assert(uur_virtual_key_supported(0xde));  /* apostrophe */
    assert(!uur_virtual_key_supported(0x2c)); /* print screen */
    assert(!uur_virtual_key_supported(0x5d)); /* application menu */
    assert(!uur_virtual_key_supported(0x90)); /* num lock */
    assert(!uur_virtual_key_supported(0xad)); /* volume mute */
    return 0;
}
