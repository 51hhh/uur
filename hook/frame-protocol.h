#ifndef UUR_FRAME_PROTOCOL_H
#define UUR_FRAME_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>

#define UURF_MAGIC 0x46525555u
#define UURF_VERSION 2u
#define UURF_SLOT_COUNT 3u
#define UURF_HEADER_BYTES 64u
#define UURF_MAX_STREAMS 16u

/* v2 keeps each selected monitor in its own transport. Stream zero uses the
 * configured base path; later streams append ".N". These offsets preserve the
 * compositor's logical monitor geometry; the hook aligns it with Wine's
 * virtual desktop before routing a Windows capture request. */
#define UURF_LOGICAL_X_OFFSET 32u
#define UURF_LOGICAL_Y_OFFSET 36u
#define UURF_LOGICAL_WIDTH_OFFSET 40u
#define UURF_LOGICAL_HEIGHT_OFFSET 44u
#define UURF_STREAM_INDEX_OFFSET 48u
#define UURF_STREAM_COUNT_OFFSET 52u
#define UURF_HEARTBEAT_OFFSET 56u

/* Every slot starts with an atomic u64 sequence. Keep later slots aligned even
 * when a BGRx frame contains an odd number of pixels (frame bytes are then
 * only four-byte aligned). Callers must reject frame sizes larger than
 * SIZE_MAX - 15 before using this helper. */
static inline size_t uurf_slot_bytes(size_t frame_bytes)
{
    return (8u + frame_bytes + 7u) & ~(size_t)7u;
}

static inline size_t uurf_slot_offset(uint64_t sequence, size_t slot_bytes)
{
    return UURF_HEADER_BYTES +
           (size_t)(sequence % UURF_SLOT_COUNT) * slot_bytes;
}

#endif
