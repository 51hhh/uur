# Capture frame protocol

Wayland monitor frames travel from PipeWire into UU's existing per-display
Windows capture path. Each selected monitor remains an independent stream.

```text
xdg-desktop-portal ScreenCast
        │ (one PipeWire node per selected monitor)
        ▼
uur-pw-capture (C) ── BGRx frame transport per monitor
        │ writes one triple-buffer file for each stream
        ▼
$XDG_RUNTIME_DIR/uur/frames.v1      (stream 0 and manifest)
$XDG_RUNTIME_DIR/uur/frames.v1.1    (stream 1)
...
        │ mapped read-only through Wine's configured Z: path
        ▼
uur-hook.dll ── routes each gdi32!BitBlt/StretchBlt request by its
                 Windows virtual-desktop source rectangle
```

Wine already exposes the host monitor topology to the official UU server as
independent `DISPLAY1`, `DISPLAY2`, and DXGI outputs. The hook preserves that
enumeration. It only replaces the pixels read for each display after UU has
selected and scheduled that display through its normal Windows path.

## Shared file layout

All integers are little-endian. Let `slot_bytes` be
`align_up(8 + stride × height, 8)`. Total size of each file is
`64 + 3 × slot_bytes`.

| offset | type | field |
| --- | --- | --- |
| 0 | u32 | magic `0x46525555` (`UURF`) |
| 4 | u32 | protocol version = 2 |
| 8 | u32 | physical frame width (px) |
| 12 | u32 | physical frame height (px) |
| 16 | u32 | stride (bytes per row) |
| 20 | u32 | format: 1 = BGRx (32 bits per pixel) |
| 24 | u64 | total frames written (monotonic counter) |
| 32 | i32 | monitor logical x in the compositor desktop |
| 36 | i32 | monitor logical y in the compositor desktop |
| 40 | u32 | monitor logical width |
| 44 | u32 | monitor logical height |
| 48 | u32 | zero-based stream index |
| 52 | u32 | number of independent streams in the session |
| 56 | u64 | producer heartbeat, advanced every 250 ms even on a static desktop |

Stream zero uses the configured base path. Stream *N* uses the same path with
`.N` appended. The base file is created last, after all secondary files are
ready, and acts as the manifest for the set.

Slot *i* (i = 0, 1, 2) starts at `64 + i × slot_bytes`:

| offset | type | field |
| --- | --- | --- |
| 0 | u64 | sequence: `2×n − 1` while writing, `2×n` when stable |
| 8 | bytes | frame, `stride × height`, top-to-bottom BGRx rows |

The writer selects slots round-robin, publishes an odd sequence, copies the
frame, publishes the even sequence, and finally updates the total counter. The
reader copies only when both sequence reads match and are even. Each transport
therefore progresses independently if another monitor pauses or disconnects.

The transport paths are per-user files with mode 0600. A new session deletes
all prior stream files before Wine starts. The hook detects stopped producers
using the heartbeat, rather than image changes: a static desktop remains a
valid image. It copies a stable frame directly into compatible 32-bit DIB
destinations, with a cached snapshot and GDI fallback for other destinations.
