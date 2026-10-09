#ifndef UUR_INPUT_MAPPING_H
#define UUR_INPUT_MAPPING_H

#include <stdint.h>

/* Keep this invariant Windows virtual-key subset in sync with
 * src/input/uinput.rs::vk_to_linux_key. The PE hook may suppress Wine's
 * SendInput only when the host bridge can represent the key. */
static inline int uur_virtual_key_supported(uint16_t vkey)
{
    uint16_t base = vkey & 0xffu;
    if ((base >= 0x30u && base <= 0x39u) ||
        (base >= 0x41u && base <= 0x5au) ||
        (base >= 0x60u && base <= 0x69u) ||
        (base >= 0x70u && base <= 0x7bu))
        return 1;
    switch (base) {
    case 0x08: /* backspace */
    case 0x09: /* tab */
    case 0x0d: /* enter / keypad enter */
    case 0x10: /* shift */
    case 0x11: /* control */
    case 0x12: /* alt */
    case 0x13: /* pause */
    case 0x14: /* caps lock */
    case 0x1b: /* escape */
    case 0x20: /* space */
    case 0x21: /* page up */
    case 0x22: /* page down */
    case 0x23: /* end */
    case 0x24: /* home */
    case 0x25: /* left */
    case 0x26: /* up */
    case 0x27: /* right */
    case 0x28: /* down */
    case 0x2d: /* insert */
    case 0x2e: /* delete */
    case 0x5b: /* left meta */
    case 0x5c: /* right meta */
    case 0x6a: /* keypad multiply */
    case 0x6b: /* keypad add */
    case 0x6d: /* keypad subtract */
    case 0x6e: /* keypad decimal */
    case 0x6f: /* keypad divide */
    case 0xa0: /* left shift */
    case 0xa1: /* right shift */
    case 0xa2: /* left control */
    case 0xa3: /* right control */
    case 0xa4: /* left alt */
    case 0xa5: /* right alt */
    case 0xba: /* semicolon */
    case 0xbb: /* equal */
    case 0xbc: /* comma */
    case 0xbd: /* minus */
    case 0xbe: /* period */
    case 0xbf: /* slash */
    case 0xc0: /* grave */
    case 0xdb: /* left bracket */
    case 0xdc: /* backslash */
    case 0xdd: /* right bracket */
    case 0xde: /* apostrophe */
        return 1;
    default:
        return 0;
    }
}

#endif
