# Video and display backends

## Frame sources

- X11 sessions expose the XRandR monitor topology directly to Wine.
- Wayland monitor and window sources use ScreenCast/PipeWire.
- `uur display source virtual` requests the portal's Virtual source type and
  stores its restore token.
- A multi-stream compositor exposes each selected PipeWire node through a
  separate frame transport. The Windows adapter matches those transports to
  UU's native display topology for independent Wayland screen switching.

Portal input prefers its EIS connection through the optional libei runtime.
Screen mapping IDs pair each stream with the compositor's absolute-input
region, preserving fractional scaling and monitor-local positions. Normalized
Windows conversions retain fractional precision until the final rounding.
Input wheel values retain Windows units (120 per notch): vertical positive
means up and horizontal positive means right. Portal/EIS converts only the
vertical sign, and EIS preserves fractional notches. Whole-notch backends
accumulate fractional values per axis instead of expanding each update.
Legacy absolute D-Bus input is converted from compositor layout coordinates into the
negotiated stream pixel dimensions. GNOME's monitor stream transform divides
that position by the monitor scale; using the Portal layout size as the input
extent would apply the fractional scale twice and move the cursor toward the
top-left corner. Forwarded mouse and virtual-key events are injected once through the host
backend, and combined motion/button events deliver their position first.
Unicode, scan-code, and unmapped virtual-key events retain Wine's original
injection path; the native bridge receives only its invariant mapped subset.

The frame consumer takes the newest queued PipeWire buffer. Compatible 32-bit
DIB capture destinations receive a direct CPU copy or scale; other GDI
destinations reuse a stable snapshot until the stream supplies a newer image.
Direct copies retain the DIB orientation recorded at creation; `GetObject`
does not preserve the original height sign. Untracked bitmaps and transformed
or mirrored drawing contexts use GDI's normal orientation handling.
The per-process `uur-capture-status-PID.txt` file reports capture time, direct
copies, snapshot copies, and unavailable frames for performance diagnosis.

The combined Portal worker monitors the capture helper. A failed stream or
expired input session closes the old session and negotiates a replacement
using the saved Portal grant. Queued commands from the failed session are
discarded; release events for inputs not held in the replacement session are
ignored. The PE hook reconnects its loopback input bridge after a lost socket.
When a desktop lock closes RemoteDesktop/EIS, uur waits for the screen shield
to become inactive before spending the single-use restore token. Transient
Portal errors never erase a persistent grant; an invalid token is handled by
the Portal as required by its API. Remote input also requests monitor power-on
through the desktop display service. The default policy wakes a blanked display
while leaving the lock screen and its authentication intact. On GNOME, users
can explicitly run `uur remote-unlock enable`; after the loopback bridge token
is authenticated, the first remote input dismisses the screen shield so the
portal capture and input sessions can reconnect. This policy skips local
lock-screen authentication and remains disabled by default.
When uur itself starts under an already locked graphical session, the bridge
begins listening before Portal restoration. This removes the wake deadlock:
the first authenticated remote input can power the monitors on even though
capture and desktop injection remain paused until the screen is unlocked.
Closing a capture session removes all its transports. A dead heartbeat remains
rejected until it changes, including after the file is reopened. Wayland
capture emits empty frames while the Portal is unavailable, rather than
falling back to an obsolete XWayland desktop; native X11 capture is preserved.

## Codec capabilities

`uur doctor` inventories the native acceleration APIs independently:

- DRM render nodes are the common device boundary;
- VA-API covers Intel, AMD, and supported third-party drivers;
- Vulkan Video is recognized through `VK_KHR_video_queue`;
- NVENC and NVDEC are recognized through NVIDIA's encode/decode libraries;
- software H.264 remains available through UU's current Windows path.

The native codec data path will consume the same BGRx frame transport used by
the current capture adapter. A backend receives frame descriptors, negotiated
color metadata, dimensions, and target latency; it returns encoded access units
through one common interface. Codec selection can then prefer a verified
hardware route and fall back to software without changing capture, display, or
UU protocol code.

HDR requires a 10-bit frame format, color primaries, transfer function, and
mastering metadata across the portal, PipeWire, encoder, and UU transport. The
current BGRx transport is SDR.
