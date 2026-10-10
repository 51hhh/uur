/*
 * uur-hook — input reroute for NetEase UU Remote under Wine.
 *
 * Loaded into GameViewerServer.exe through the wevtapi.dll preload chain
 * (see wevtapi.c).  At PROCESS_ATTACH it rewrites the SendInput entries of
 * every loaded module's import table to point at hook_SendInput, which
 * forwards each input record over an authenticated loopback socket to the
 * uur host daemon.  The daemon injects the record onto the real desktop
 * (XTest on X11, RemoteDesktop portal on Wayland).
 *
 * Failure policy: if the socket is unavailable, hook_SendInput falls back
 * to the original SendInput so the client keeps working with input
 * contained inside the managed Wine prefix.
 *
 * Wire format mirrors src/protocol.rs: 16-byte record
 *   u32 kind (1 keyboard, 2 mouse) u16 code u16 state i32 a i32 b
 */

#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <tlhelp32.h>
#include <dxgi.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "capture-mapping.h"
#include "cursor-overlay.h"
#include "frame-protocol.h"
#include "frame-copy.h"
#include "input-mapping.h"

#define UUR_MAGIC 0x50495555u
#define UUR_VERSION 1u
#define UUR_RECORD_HELLO 1u
#define UUR_RECORD_MOUSE 2u
#define UUR_RECORD_KEYBOARD 3u

static UINT(WINAPI *original_send_input)(UINT, LPINPUT, int);
static HRESULT(WINAPI *original_create_dxgi_factory1)(REFIID, void **);
static FARPROC(WINAPI *original_get_proc_address)(HMODULE, LPCSTR);
UINT WINAPI hook_send_input(UINT count, LPINPUT inputs, int size);

static volatile LONG dxgi_factories_patched = 0;
static volatile LONG dxgi_adapters_patched = 0;
static volatile LONG dxgi_outputs_patched = 0;
static volatile LONG dxgi_duplicate_calls = 0;
static volatile LONG dxgi_dup_acquires = 0;
static volatile LONG dxgi_dup_frames = 0;

static HRESULT __attribute__((unused))(STDMETHODCALLTYPE *original_dxgi_acquire_next_frame)(
    void *duplication, UINT timeout, void *frame_info, void **resource) = NULL;
/* referenced by patch_dxgi_duplication below */

/* ---- DXGI duplication disable ----------------------------------------- */

/* The client's primary capture is IDXGIOutput1::DuplicateOutput.  Wine's
 * implementation succeeds but only delivers the empty XWayland desktop
 * (black video).  Forcing DuplicateOutput to fail makes the client fall
 * back to its GDI capture path, which the frame feed below supplies. */

static const GUID uur_iid_idxgioutput1 = {
    0x00cddea8, 0x939b, 0x4b83,
    {0xa3, 0x40, 0xa6, 0x85, 0x22, 0x66, 0x66, 0xcc}
};

/* Minimal IDXGIOutput1 shape: mingw's C-mode dxgi.h lacks it.  The
 * vtable is IUnknown(3) + IDXGIOutput(12) methods, then DuplicateOutput. */
typedef struct uur_idxgioutput1_vtbl {
    void *methods[15];
    HRESULT(STDMETHODCALLTYPE *DuplicateOutput)(void *, IUnknown *);
} uur_idxgioutput1_vtbl;
typedef struct uur_idxgioutput1 {
    uur_idxgioutput1_vtbl *lpVtbl;
} uur_idxgioutput1;

#ifndef DXGI_ERROR_UNSUPPORTED
#define DXGI_ERROR_UNSUPPORTED ((HRESULT)0x887A0004)
#endif

static HRESULT(STDMETHODCALLTYPE *original_dxgi_enum_adapters1)(
    IDXGIFactory1 *factory, UINT index, IDXGIAdapter1 **adapter) = NULL;
static HRESULT(STDMETHODCALLTYPE *original_dxgi_enum_outputs)(
    IDXGIAdapter *adapter, UINT index, IDXGIOutput **output) = NULL;
static HRESULT STDMETHODCALLTYPE hook_dxgi_enum_adapters1(
    IDXGIFactory1 *factory, UINT index, IDXGIAdapter1 **adapter);
static HRESULT STDMETHODCALLTYPE hook_dxgi_enum_outputs(
    IDXGIAdapter *adapter, UINT index, IDXGIOutput **output);
static HRESULT WINAPI hook_dxgi_duplicate_output(uur_idxgioutput1 *output,
                                                 IUnknown *device);

static int patch_vtable_slot(void **slot, void *hook, void **original)
{
    DWORD old_protect = 0;
    if (*slot == hook)
        return 1;
    if (original != NULL)
        *original = *slot;
    if (!VirtualProtect(slot, sizeof(void *), PAGE_READWRITE, &old_protect))
        return 0;
    *slot = hook;
    VirtualProtect(slot, sizeof(void *), old_protect, &old_protect);
    return 1;
}

static HRESULT(STDMETHODCALLTYPE *original_dxgi_enum_adapters)(
    IDXGIFactory *factory, UINT index, IDXGIAdapter **adapter) = NULL;
static void patch_dxgi_adapter(IDXGIAdapter *adapter);

static void patch_dxgi_output(IDXGIOutput *output)
{
    uur_idxgioutput1 *output1 = NULL;

    if (output == NULL || output->lpVtbl == NULL)
        return;
    if (output->lpVtbl->QueryInterface(
            output, &uur_iid_idxgioutput1, (void **)&output1) != S_OK)
        return;
    if (output1 != NULL && output1->lpVtbl != NULL) {
        if (patch_vtable_slot((void **)&output1->lpVtbl->DuplicateOutput,
                              (void *)hook_dxgi_duplicate_output, NULL)) {
            InterlockedIncrement(&dxgi_outputs_patched);
        }
        /* release through the known-good base interface */
        output->lpVtbl->Release(output);
    }
}

static HRESULT STDMETHODCALLTYPE hook_dxgi_enum_adapters(
    IDXGIFactory *factory, UINT index, IDXGIAdapter **adapter)
{
    HRESULT result = original_dxgi_enum_adapters
        ? original_dxgi_enum_adapters(factory, index, adapter)
        : DXGI_ERROR_INVALID_CALL;

    if (result == S_OK && adapter != NULL && *adapter != NULL)
        patch_dxgi_adapter(*adapter);
    return result;
}

static void patch_dxgi_adapter(IDXGIAdapter *adapter)
{
    if (adapter == NULL || adapter->lpVtbl == NULL)
        return;
    if (patch_vtable_slot((void **)&adapter->lpVtbl->EnumOutputs,
                          (void *)hook_dxgi_enum_outputs,
                          (void **)&original_dxgi_enum_outputs)) {
        InterlockedIncrement(&dxgi_adapters_patched);
    }
}

static HRESULT STDMETHODCALLTYPE hook_dxgi_enum_adapters1(
    IDXGIFactory1 *factory, UINT index, IDXGIAdapter1 **adapter)
{
    HRESULT result = original_dxgi_enum_adapters1
        ? original_dxgi_enum_adapters1(factory, index, adapter)
        : DXGI_ERROR_INVALID_CALL;

    if (result == S_OK && adapter != NULL && *adapter != NULL)
        patch_dxgi_adapter((IDXGIAdapter *)*adapter);
    return result;
}

static HRESULT STDMETHODCALLTYPE hook_dxgi_enum_outputs(
    IDXGIAdapter *adapter, UINT index, IDXGIOutput **output)
{
    HRESULT result = original_dxgi_enum_outputs
        ? original_dxgi_enum_outputs(adapter, index, output)
        : DXGI_ERROR_INVALID_CALL;

    if (result == S_OK && output != NULL && *output != NULL)
        patch_dxgi_output(*output);
    return result;
}

static HRESULT WINAPI hook_dxgi_duplicate_output(uur_idxgioutput1 *output,
                                                 IUnknown *device)
{
    (void)output;
    (void)device;
    InterlockedIncrement(&dxgi_duplicate_calls);
    /* Wine's duplication succeeds but delivers the empty XWayland desktop.
     * Failing it sends the client to its GDI capture path. */
    return DXGI_ERROR_UNSUPPORTED;
}

static void patch_dxgi_factory(IDXGIFactory1 *factory)
{
    if (factory == NULL || factory->lpVtbl == NULL)
        return;
    patch_vtable_slot((void **)&factory->lpVtbl->EnumAdapters1,
                      (void *)hook_dxgi_enum_adapters1,
                      (void **)&original_dxgi_enum_adapters1);
    patch_vtable_slot((void **)&factory->lpVtbl->EnumAdapters,
                      (void *)hook_dxgi_enum_adapters,
                      (void **)&original_dxgi_enum_adapters);
}

static HRESULT WINAPI hook_create_dxgi_factory1(REFIID interface_id,
                                                void **factory)
{
    HRESULT result = original_create_dxgi_factory1 != NULL
        ? original_create_dxgi_factory1(interface_id, factory)
        : DXGI_ERROR_INVALID_CALL;

    if (result == S_OK && factory != NULL && *factory != NULL) {
        IDXGIFactory1 *f = (IDXGIFactory1 *)*factory;
        if (f->lpVtbl != NULL) {
            patch_dxgi_factory(f);
            InterlockedIncrement(&dxgi_factories_patched);
        }
    }
    return result;
}
static BOOL(WINAPI *original_bit_blt)(HDC, int, int, int, int, HDC, int, int,
                                      DWORD);
static BOOL(WINAPI *original_stretch_blt)(HDC, int, int, int, int, HDC, int,
                                          int, int, int, DWORD);
static HBITMAP(WINAPI *original_create_dib_section)(HDC, const BITMAPINFO *, UINT,
                                                   void **, HANDLE, DWORD);
static BOOL(WINAPI *original_delete_object)(HGDIOBJ);
static volatile LONG capture_calls = 0;
static volatile LONG capture_rendered = 0;
static volatile LONG capture_fallbacks = 0;
static volatile LONG capture_snapshot_copies = 0;
static volatile LONG capture_direct_copies = 0;
static volatile LONG capture_unavailable = 0;
static volatile LONG64 capture_microseconds = 0;
static volatile LONG iat_hooks_installed = 0;

static SOCKET hook_socket = INVALID_SOCKET;
static SRWLOCK hook_input_lock = SRWLOCK_INIT;
static volatile LONG bridge_retry_running = 0;
static DWORD WINAPI bridge_retry_thread(LPVOID parameter);

static void start_bridge_retry(void)
{
    if (InterlockedCompareExchange(&bridge_retry_running, 1, 0) != 0)
        return;
    HANDLE thread = CreateThread(NULL, 0, bridge_retry_thread, NULL, 0, NULL);
    if (thread != NULL)
        CloseHandle(thread);
    else
        InterlockedExchange(&bridge_retry_running, 0);
}

/* ---- capture frame feed (docs/capture-protocol.md) -------------------- */

struct frame_source {
    void *map;
    HANDLE map_handle;
    size_t map_size;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint64_t slot_bytes;
    int32_t logical_x;
    int32_t logical_y;
    uint32_t logical_width;
    uint32_t logical_height;
    uint64_t progress_total;
    DWORD progress_tick;
    DWORD path_check_tick;
    uint64_t rejected_heartbeat;
    int heartbeat_rejected;
    unsigned char *snapshot;
    size_t snapshot_capacity;
    uint64_t snapshot_total;
};

static struct frame_source frame_sources[UURF_MAX_STREAMS];
static uint32_t frame_source_count;
static SRWLOCK frame_sources_lock = SRWLOCK_INIT;

static char *read_ini_value(const char *path, const char *key);

static int resolve_frame_path(WCHAR *path, int capacity)
{
    DWORD count = GetEnvironmentVariableW(L"UUR_FRAME_PATH_WIN", path,
                                          (DWORD)capacity);
    if (count > 0 && count < (DWORD)capacity)
        return 1;

    char *utf8 = read_ini_value("C:\\uur-bridge.ini", "frame_path");
    if (utf8 == NULL)
        return 0;
    int converted = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8,
                                        -1, path, capacity);
    free(utf8);
    return converted > 0;
}

static uint32_t load_u32(const unsigned char *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint64_t load_u64(const unsigned char *p)
{
    return load_u32(p) | ((uint64_t)load_u32(p + 4) << 32);
}

static void reset_frame_source(struct frame_source *source)
{
    if (source->map != NULL)
        UnmapViewOfFile(source->map);
    if (source->map_handle != NULL)
        CloseHandle(source->map_handle);
    source->map = NULL;
    source->map_handle = NULL;
    source->map_size = 0;
    source->width = 0;
    source->height = 0;
    source->stride = 0;
    source->slot_bytes = 0;
    source->logical_x = 0;
    source->logical_y = 0;
    source->logical_width = 0;
    source->logical_height = 0;
    source->progress_total = 0;
    source->progress_tick = 0;
    source->path_check_tick = 0;
    source->snapshot_total = 0;
}

static void reset_frame_sources(void)
{
    for (uint32_t i = 0; i < UURF_MAX_STREAMS; ++i)
        reset_frame_source(&frame_sources[i]);
    frame_source_count = 0;
}

static int indexed_frame_path(WCHAR *path, int capacity, uint32_t index)
{
    if (!resolve_frame_path(path, capacity))
        return 0;
    if (index == 0)
        return 1;
    int used = lstrlenW(path);
    int written = _snwprintf(path + used, (size_t)(capacity - used), L".%u",
                             index);
    return written > 0 && used + written < capacity;
}

/* Open one monitor transport. Stream zero is the manifest and declares how
 * many additional UUR_FRAME_PATH.N transports belong to this session. */
static int ensure_frame_source(uint32_t index)
{
    HANDLE file;
    HANDLE mapping;
    void *view;
    const unsigned char *header;
    LARGE_INTEGER size;
    WCHAR path[32768];
    struct frame_source *source;
    uint32_t width, height, stride, stream_index, stream_count;
    uint64_t slot_bytes, expected;

    if (index >= UURF_MAX_STREAMS)
        return 0;
    source = &frame_sources[index];
    if (source->map != NULL) {
        DWORD now = GetTickCount();
        if ((DWORD)(now - source->path_check_tick) >= 250u) {
            source->path_check_tick = now;
            if (!indexed_frame_path(path, (int)(sizeof(path) / sizeof(path[0])), index) ||
                GetFileAttributesW(path) == INVALID_FILE_ATTRIBUTES) {
                if (index == 0)
                    reset_frame_sources();
                else
                    reset_frame_source(source);
                return 0;
            }
        }
        return 1;
    }
    if (!indexed_frame_path(path, (int)(sizeof(path) / sizeof(path[0])), index))
        return 0;
    file = CreateFileW(path, GENERIC_READ,
                       FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                       NULL, OPEN_EXISTING,
                       0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    if (!GetFileSizeEx(file, &size) || size.QuadPart < UURF_HEADER_BYTES ||
        (uint64_t)size.QuadPart > SIZE_MAX) {
        CloseHandle(file);
        return 0;
    }
    mapping = CreateFileMappingW(file, NULL, PAGE_READONLY, 0, 0, NULL);
    CloseHandle(file);
    if (mapping == NULL)
        return 0;
    view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == NULL) {
        CloseHandle(mapping);
        return 0;
    }

    header = (const unsigned char *)view;
    if (load_u32(header) != UURF_MAGIC ||
        load_u32(header + 4) != UURF_VERSION || load_u32(header + 20) != 1u) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 0;
    }
    /* Reopening the same dead transport must not grant its old heartbeat a
     * fresh timeout on every request. A restarted/live producer advances it. */
    if (source->heartbeat_rejected &&
        load_u64(header + UURF_HEARTBEAT_OFFSET) == source->rejected_heartbeat) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 0;
    }
    width = load_u32(header + 8);
    height = load_u32(header + 12);
    stride = load_u32(header + 16);
    stream_index = load_u32(header + UURF_STREAM_INDEX_OFFSET);
    stream_count = load_u32(header + UURF_STREAM_COUNT_OFFSET);
    uint64_t frame_bytes = (uint64_t)stride * height;
    if (frame_bytes > SIZE_MAX - 15u) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 0;
    }
    slot_bytes = uurf_slot_bytes((size_t)frame_bytes);
    expected = UURF_HEADER_BYTES + UURF_SLOT_COUNT * slot_bytes;
    if (width == 0 || height == 0 || stride < width * 4 ||
        stream_index != index || stream_count == 0 ||
        stream_count > UURF_MAX_STREAMS ||
        load_u32(header + UURF_LOGICAL_WIDTH_OFFSET) == 0 ||
        load_u32(header + UURF_LOGICAL_HEIGHT_OFFSET) == 0 ||
        expected > (uint64_t)size.QuadPart) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        return 0;
    }
    source->map = view;
    source->map_handle = mapping;
    source->map_size = (size_t)size.QuadPart;
    source->width = width;
    source->height = height;
    source->stride = stride;
    source->slot_bytes = slot_bytes;
    source->logical_x = (int32_t)load_u32(
        header + UURF_LOGICAL_X_OFFSET);
    source->logical_y = (int32_t)load_u32(
        header + UURF_LOGICAL_Y_OFFSET);
    source->logical_width = load_u32(header + UURF_LOGICAL_WIDTH_OFFSET);
    source->logical_height = load_u32(header + UURF_LOGICAL_HEIGHT_OFFSET);
    source->progress_total = load_u64(header + UURF_HEARTBEAT_OFFSET);
    source->progress_tick = GetTickCount();
    source->path_check_tick = source->progress_tick;
    source->heartbeat_rejected = 0;
    if (index == 0)
        frame_source_count = stream_count;
    return 1;
}

static int ensure_frame_sources(void)
{
    if (!ensure_frame_source(0))
        return 0;
    for (uint32_t i = 1; i < frame_source_count; ++i) {
        /* A partial layout changes the bounding box and can route the live
         * monitor's image into the disconnected monitor's capture request. */
        if (!ensure_frame_source(i))
            return 0;
    }
    return 1;
}

static int aligned_frame_geometries(uur_frame_geometry *geometry)
{
    if (geometry == NULL || frame_source_count == 0)
        return 0;
    ZeroMemory(geometry, sizeof(*geometry) * UURF_MAX_STREAMS);
    for (uint32_t i = 0; i < frame_source_count; ++i) {
        struct frame_source *source = &frame_sources[i];
        geometry[i].x = source->logical_x;
        geometry[i].y = source->logical_y;
        geometry[i].width = source->logical_width;
        geometry[i].height = source->logical_height;
        geometry[i].available = source->map != NULL;
    }
    return uur_align_frame_geometries(
        geometry, frame_source_count, GetSystemMetrics(SM_XVIRTUALSCREEN),
        GetSystemMetrics(SM_YVIRTUALSCREEN),
        GetSystemMetrics(SM_CXVIRTUALSCREEN),
        GetSystemMetrics(SM_CYVIRTUALSCREEN));
}

static struct frame_source *select_frame_source(int source_x, int source_y,
                                                int source_width,
                                                int source_height)
{
    uur_frame_geometry geometry[UURF_MAX_STREAMS];
    if (!aligned_frame_geometries(geometry))
        return NULL;
    int selected = uur_select_capture_stream(
        geometry, frame_source_count, source_x, source_y, source_width,
        source_height);
    return selected >= 0 ? &frame_sources[selected] : NULL;
}

static int primary_monitor_rect(RECT *rect)
{
    POINT origin = {0, 0};
    MONITORINFO info;
    ZeroMemory(&info, sizeof(info));
    info.cbSize = sizeof(info);
    HMONITOR monitor = MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
    if (monitor == NULL || !GetMonitorInfoW(monitor, &info))
        return 0;
    *rect = info.rcMonitor;
    return rect->right > rect->left && rect->bottom > rect->top;
}

/* The Portal and Wine topologies can have different global offsets under
 * mixed DPI scaling. Associate the Windows target monitor with its frame
 * stream, then preserve the pointer's position inside that monitor. */
static int map_absolute_pointer_to_portal_locked(int32_t *x, int32_t *y,
                                          int virtual_desktop)
{
    uur_frame_geometry geometry[UURF_MAX_STREAMS];
    RECT monitor_rect;
    int32_t local_x = uur_clamp_normalized_pointer(*x);
    int32_t local_y = uur_clamp_normalized_pointer(*y);

    if (!ensure_frame_sources() || !aligned_frame_geometries(geometry))
        return 0;

    if (virtual_desktop) {
        int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
        int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
        int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
        int virtual_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
        if (virtual_width <= 0 || virtual_height <= 0)
            return 0;
        POINT point = {
            virtual_x + (LONG)((int64_t)local_x * (virtual_width - 1) /
                               UUR_POINTER_NORMALIZED_MAX),
            virtual_y + (LONG)((int64_t)local_y * (virtual_height - 1) /
                               UUR_POINTER_NORMALIZED_MAX),
        };
        MONITORINFO info;
        ZeroMemory(&info, sizeof(info));
        info.cbSize = sizeof(info);
        HMONITOR monitor = MonitorFromPoint(point, MONITOR_DEFAULTTONEAREST);
        if (monitor == NULL || !GetMonitorInfoW(monitor, &info))
            return 0;
        monitor_rect = info.rcMonitor;
        int monitor_width = monitor_rect.right - monitor_rect.left;
        int monitor_height = monitor_rect.bottom - monitor_rect.top;
        if (monitor_width <= 0 || monitor_height <= 0)
            return 0;
        if (!uur_virtual_pointer_to_monitor(
                &local_x, &local_y, virtual_x, virtual_y, virtual_width,
                virtual_height, monitor_rect.left, monitor_rect.top,
                monitor_width, monitor_height))
            return 0;
    } else if (!primary_monitor_rect(&monitor_rect)) {
        return 0;
    }

    int monitor_width = monitor_rect.right - monitor_rect.left;
    int monitor_height = monitor_rect.bottom - monitor_rect.top;
    int selected = uur_select_capture_stream(
        geometry, frame_source_count, monitor_rect.left, monitor_rect.top,
        monitor_width, monitor_height);
    if (selected < 0)
        return 0;

    int64_t min_x = INT64_MAX;
    int64_t min_y = INT64_MAX;
    int64_t max_x = INT64_MIN;
    int64_t max_y = INT64_MIN;
    for (uint32_t i = 0; i < frame_source_count; ++i) {
        struct frame_source *source = &frame_sources[i];
        if (source->map == NULL || source->logical_width == 0 ||
            source->logical_height == 0)
            continue;
        min_x = uur_min_i64(min_x, source->logical_x);
        min_y = uur_min_i64(min_y, source->logical_y);
        max_x = uur_max_i64(max_x,
                            (int64_t)source->logical_x + source->logical_width);
        max_y = uur_max_i64(max_y,
                            (int64_t)source->logical_y + source->logical_height);
    }
    if (min_x == INT64_MAX || max_x - min_x <= 0 || max_y - min_y <= 0 ||
        min_x < INT32_MIN || min_x > INT32_MAX || min_y < INT32_MIN ||
        min_y > INT32_MAX || max_x - min_x > UINT32_MAX ||
        max_y - min_y > UINT32_MAX)
        return 0;

    struct frame_source *source = &frame_sources[selected];
    if (!uur_monitor_pointer_to_portal(
            &local_x, &local_y, source->logical_x, source->logical_y,
            source->logical_width, source->logical_height, (int32_t)min_x,
            (int32_t)min_y, (uint32_t)(max_x - min_x),
            (uint32_t)(max_y - min_y)))
        return 0;
    *x = local_x;
    *y = local_y;
    return 1;
}

static int map_absolute_pointer_to_portal(int32_t *x, int32_t *y,
                                          int virtual_desktop)
{
    AcquireSRWLockExclusive(&frame_sources_lock);
    int mapped = map_absolute_pointer_to_portal_locked(x, y, virtual_desktop);
    ReleaseSRWLockExclusive(&frame_sources_lock);
    return mapped;
}

static BOOL source_is_screen_dc(HDC source)
{
    return GetObjectType(source) == OBJ_DC &&
           GetDeviceCaps(source, TECHNOLOGY) == DT_RASDISPLAY;
}

/* GDI screen captures do not include the X11 hardware cursor.  Keep the
 * cursor visible in the fallback path used on X11 by compositing Wine's
 * current cursor onto the capture destination after the desktop blit. */
static void draw_cursor_overlay(HDC hdc_dest, int x, int y, int width, int height,
                                int source_x, int source_y, int source_width,
                                int source_height)
{
    CURSORINFO cursor;
    ICONINFO icon;
    BITMAP mask;
    POINT position;
    uur_cursor_rect source = {source_x, source_y, source_width, source_height};
    uur_cursor_rect destination = {x, y, width, height};
    uur_cursor_rect overlay;
    uur_cursor_shape shape;

    if (hdc_dest == NULL || width <= 0 || height <= 0 || source_width <= 0 ||
        source_height <= 0 || source_is_screen_dc(hdc_dest))
        return;
    ZeroMemory(&cursor, sizeof(cursor));
    cursor.cbSize = sizeof(cursor);
    if (!GetCursorInfo(&cursor) || !(cursor.flags & CURSOR_SHOWING) ||
        !GetCursorPos(&position))
        return;
    /* Wine can report a visible cursor with a null handle until the first
     * real window has selected a class cursor.  A generic arrow is preferable
     * to dropping the cursor from the remote frame altogether. */
    if (cursor.hCursor == NULL)
        cursor.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (cursor.hCursor == NULL)
        return;

    ZeroMemory(&icon, sizeof(icon));
    if (!GetIconInfo(cursor.hCursor, &icon))
        return;

    /* GetIconInfo's mask holds the cursor's actual bitmap dimensions. For a
     * monochrome cursor the AND/XOR masks are stacked vertically. */
    if (icon.hbmMask != NULL &&
        GetObjectW(icon.hbmMask, sizeof(mask), &mask) == sizeof(mask)) {
        shape.x = position.x;
        shape.y = position.y;
        shape.width = mask.bmWidth;
        shape.height = uur_cursor_bitmap_height(mask.bmHeight,
                                                 icon.hbmColor != NULL);
        shape.hotspot_x = (int)icon.xHotspot;
        shape.hotspot_y = (int)icon.yHotspot;
        if (uur_map_cursor_overlay(&source, &destination, &shape, &overlay))
            DrawIconEx(hdc_dest, overlay.x, overlay.y, cursor.hCursor,
                       overlay.width, overlay.height, 0, NULL, DI_NORMAL);
    }
    if (icon.hbmColor != NULL)
        DeleteObject(icon.hbmColor);
    if (icon.hbmMask != NULL)
        DeleteObject(icon.hbmMask);
}

/* GetObject(DIBSECTION) normalizes biHeight to a positive value, including
 * top-down DIBs. Preserve the original creation direction; never infer it from
 * that normalized descriptor. Untracked bitmaps use GDI's orientation logic. */
struct dib_orientation {
    HBITMAP bitmap;
    void *bits;
    LONG width, height, stride;
    int top_down;
    struct dib_orientation *next;
};

#define DIB_ORIENTATION_BUCKETS 64u
static struct dib_orientation *dib_orientations[DIB_ORIENTATION_BUCKETS];
static SRWLOCK dib_orientations_lock = SRWLOCK_INIT;

static size_t dib_orientation_bucket(HGDIOBJ bitmap)
{
    return ((uintptr_t)bitmap >> 2) % DIB_ORIENTATION_BUCKETS;
}

static void forget_dib_orientation(HGDIOBJ bitmap)
{
    size_t bucket = dib_orientation_bucket(bitmap);
    AcquireSRWLockExclusive(&dib_orientations_lock);
    struct dib_orientation **link = &dib_orientations[bucket];
    while (*link != NULL) {
        struct dib_orientation *entry = *link;
        if ((HGDIOBJ)entry->bitmap == bitmap) {
            *link = entry->next;
            HeapFree(GetProcessHeap(), 0, entry);
            break;
        }
        link = &entry->next;
    }
    ReleaseSRWLockExclusive(&dib_orientations_lock);
}

static HBITMAP WINAPI hook_create_dib_section(HDC dc, const BITMAPINFO *info,
                                              UINT usage, void **bits,
                                              HANDLE section, DWORD offset)
{
    int has_header = info != NULL && info->bmiHeader.biSize >= sizeof(BITMAPINFOHEADER);
    int top_down = has_header && info->bmiHeader.biHeight < 0;
    HBITMAP bitmap = original_create_dib_section(dc, info, usage, bits, section, offset);
    if (bitmap == NULL || !has_header)
        return bitmap;
    DIBSECTION dib;
    if (GetObjectW(bitmap, sizeof(dib), &dib) != sizeof(dib) ||
        dib.dsBm.bmBits == NULL || dib.dsBm.bmBitsPixel != 32 ||
        dib.dsBmih.biCompression != BI_RGB)
        return bitmap;
    struct dib_orientation *entry = HeapAlloc(GetProcessHeap(), 0, sizeof(*entry));
    if (entry == NULL)
        return bitmap;
    entry->bitmap = bitmap;
    entry->bits = dib.dsBm.bmBits;
    entry->width = dib.dsBm.bmWidth;
    entry->height = dib.dsBm.bmHeight;
    entry->stride = dib.dsBm.bmWidthBytes;
    entry->top_down = top_down;
    forget_dib_orientation((HGDIOBJ)bitmap);
    size_t bucket = dib_orientation_bucket((HGDIOBJ)bitmap);
    AcquireSRWLockExclusive(&dib_orientations_lock);
    entry->next = dib_orientations[bucket];
    dib_orientations[bucket] = entry;
    ReleaseSRWLockExclusive(&dib_orientations_lock);
    return bitmap;
}

static BOOL WINAPI hook_delete_object(HGDIOBJ object)
{
    /* Forget before releasing the handle so a later bitmap can safely reuse
     * it. A failed deletion merely makes that bitmap use the GDI path. */
    forget_dib_orientation(object);
    return original_delete_object(object);
}

static int known_dib_orientation(HBITMAP bitmap, const DIBSECTION *dib, int *top_down)
{
    int found = 0;
    size_t bucket = dib_orientation_bucket((HGDIOBJ)bitmap);
    AcquireSRWLockShared(&dib_orientations_lock);
    for (struct dib_orientation *entry = dib_orientations[bucket]; entry != NULL; entry = entry->next) {
        if (entry->bitmap == bitmap && entry->bits == dib->dsBm.bmBits &&
            entry->width == dib->dsBm.bmWidth && entry->height == dib->dsBm.bmHeight &&
            entry->stride == dib->dsBm.bmWidthBytes) {
            *top_down = entry->top_down;
            found = 1;
            break;
        }
    }
    ReleaseSRWLockShared(&dib_orientations_lock);
    return found;
}

static int writable_capture_dib(HDC dc, DIBSECTION *dib, int *top_down)
{
    POINT window_origin, viewport_origin;
    if (GetMapMode(dc) != MM_TEXT || GetLayout(dc) != 0 ||
        !GetWindowOrgEx(dc, &window_origin) ||
        !GetViewportOrgEx(dc, &viewport_origin) || window_origin.x != 0 ||
        window_origin.y != 0 || viewport_origin.x != 0 || viewport_origin.y != 0)
        return 0;
    if (GetGraphicsMode(dc) == GM_ADVANCED) {
        XFORM transform;
        if (!GetWorldTransform(dc, &transform) || transform.eM11 != 1.0f ||
            transform.eM12 != 0.0f || transform.eM21 != 0.0f ||
            transform.eM22 != 1.0f || transform.eDx != 0.0f || transform.eDy != 0.0f)
            return 0;
    }
    HGDIOBJ bitmap = GetCurrentObject(dc, OBJ_BITMAP);
    if (bitmap == NULL || GetObjectW(bitmap, sizeof(*dib), dib) != sizeof(*dib) ||
        dib->dsBm.bmBits == NULL || dib->dsBm.bmBitsPixel != 32 ||
        dib->dsBm.bmWidth <= 0 || dib->dsBm.bmHeight <= 0 ||
        dib->dsBm.bmWidthBytes <= 0 || dib->dsBmih.biCompression != BI_RGB)
        return 0;
    RECT clip;
    int kind = GetClipBox(dc, &clip);
    return kind == SIMPLEREGION && clip.left == 0 && clip.top == 0 &&
           clip.right == dib->dsBm.bmWidth &&
           clip.bottom == dib->dsBm.bmHeight &&
           known_dib_orientation((HBITMAP)bitmap, dib, top_down);
}

/* Feed the newest desktop frame into the capture destination by drawing
 * our shared-memory frame with StretchDIBits.  Works regardless of the
 * destination bitmap type (DDB or DIB section). */
static BOOL feed_capture_frame_locked(HDC hdc_dest, int x, int y, int width,
                               int height, int source_x, int source_y,
                               int source_width, int source_height)
{
    const unsigned char *header;
    uint32_t fw, fh, fstride;
    uint64_t total;
    int tries;
    int ok = 0;
    struct frame_source *frame;
    DIBSECTION dib;
    int top_down = 0;
    int direct_dib = width > 0 && height > 0 &&
                     writable_capture_dib(hdc_dest, &dib, &top_down);

    if (!ensure_frame_sources())
        return FALSE;
    frame = select_frame_source(source_x, source_y, source_width,
                                source_height);
    if (frame == NULL)
        return FALSE;
    header = (const unsigned char *)frame->map;
    if (load_u32(header) != UURF_MAGIC ||
        load_u32(header + 4) != UURF_VERSION) {
        /* A format change replaces the whole multi-stream manifest. Never mix
         * an old mapped stream with files from the replacement generation. */
        reset_frame_sources();
        return FALSE;
    }
    fw = frame->width;
    fh = frame->height;
    fstride = frame->stride;
    size_t frame_bytes = (size_t)fstride * fh;
    total = load_u64(header + 24);
    uint64_t heartbeat = load_u64(header + UURF_HEARTBEAT_OFFSET);
    DWORD progress_tick = GetTickCount();
    if (heartbeat != frame->progress_total) {
        frame->progress_total = heartbeat;
        frame->progress_tick = progress_tick;
    } else if (heartbeat != 0 &&
               (DWORD)(progress_tick - frame->progress_tick) >= 3000u) {
        /* The file may belong to a helper from an earlier session. Drop the
         * mapping and resolve the current path on the next capture request. */
        frame->rejected_heartbeat = heartbeat;
        frame->heartbeat_rejected = 1;
        if (frame == &frame_sources[0])
            reset_frame_sources();
        else
            reset_frame_source(frame);
        return FALSE;
    }
    if (fw == 0 || fh == 0 || fstride < fw * 4 || total == 0 ||
        frame_bytes / fstride != fh ||
        UURF_HEADER_BYTES + UURF_SLOT_COUNT * frame->slot_bytes >
            frame->map_size)
        return FALSE;
    if (!direct_dib && frame->snapshot_capacity < frame_bytes) {
        unsigned char *fresh = HeapReAlloc(GetProcessHeap(), 0,
                                           frame->snapshot, frame_bytes);
        if (fresh == NULL && frame->snapshot == NULL)
            fresh = HeapAlloc(GetProcessHeap(), 0, frame_bytes);
        if (fresh == NULL)
            return FALSE;
        frame->snapshot = fresh;
        frame->snapshot_capacity = frame_bytes;
    }
    if (direct_dib)
        GdiFlush();

    for (tries = 0; tries < 16 && !ok; ++tries) {
        const unsigned char *shm_slot =
            (const unsigned char *)frame->map +
            uurf_slot_offset(total, (size_t)frame->slot_bytes);
        uint64_t before = load_u64(shm_slot);
        if ((before & 1) == 0 && before / 2 == total) {
            MemoryBarrier();
            if (direct_dib) {
                ok = uur_copy_frame_to_dib(
                    shm_slot + 8, fw, fh, fstride, dib.dsBm.bmBits,
                    (uint32_t)dib.dsBm.bmWidth, (uint32_t)dib.dsBm.bmHeight,
                    (size_t)dib.dsBm.bmWidthBytes, top_down,
                    x, y, width, height);
                MemoryBarrier();
                if (before != load_u64(shm_slot)) {
                    ok = 0;
                    total = load_u64(header + 24);
                    continue;
                }
                if (ok)
                    InterlockedIncrement(&capture_direct_copies);
                continue;
            }
            if (frame->snapshot_total != total) {
                memcpy(frame->snapshot, shm_slot + 8, frame_bytes);
                InterlockedIncrement(&capture_snapshot_copies);
            }
            MemoryBarrier();
            uint64_t after = load_u64(shm_slot);
            if (before != after) {
                frame->snapshot_total = 0;
                continue;
            }
            frame->snapshot_total = total;
            BITMAPINFOHEADER bmp_header;
            ZeroMemory(&bmp_header, sizeof(bmp_header));
            bmp_header.biSize = sizeof(BITMAPINFOHEADER);
            bmp_header.biWidth = (LONG)fw;
            bmp_header.biHeight = -(LONG)fh; /* top-down */
            bmp_header.biPlanes = 1;
            bmp_header.biBitCount = 32;
            bmp_header.biCompression = BI_RGB;
            bmp_header.biSizeImage = fstride * fh;

            uur_capture_region source;
            if (!uur_portal_frame_region((int)fw, (int)fh, &source))
                return FALSE;
            int result = StretchDIBits(
                hdc_dest, x, y, width, height, source.x, source.y,
                source.width, source.height,
                (const void *)frame->snapshot,
                (const BITMAPINFO *)&bmp_header,
                DIB_RGB_COLORS, SRCCOPY);
            ok = result != 0 && (DWORD)result != GDI_ERROR;
        } else {
            uint64_t now = load_u64(header + 24);
            total = (now < total) ? now : total - 1;
            if (total == 0)
                return FALSE;
        }
    }
    return ok;
}

static BOOL feed_capture_frame(HDC hdc_dest, int x, int y, int width,
                               int height, int source_x, int source_y,
                               int source_width, int source_height)
{
    LARGE_INTEGER start, end, frequency;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    AcquireSRWLockExclusive(&frame_sources_lock);
    BOOL result = feed_capture_frame_locked(hdc_dest, x, y, width, height,
                                            source_x, source_y, source_width,
                                            source_height);
    ReleaseSRWLockExclusive(&frame_sources_lock);
    QueryPerformanceCounter(&end);
    if (frequency.QuadPart > 0)
        InterlockedAdd64(&capture_microseconds,
            (end.QuadPart - start.QuadPart) * 1000000 / frequency.QuadPart);
    return result;
}

static BOOL try_capture_feed(HDC hdc_dest, int x, int y, DWORD rop,
                             HDC hdc_src, int width, int height, int source_x,
                             int source_y, int source_width, int source_height)
{
    InterlockedIncrement(&capture_calls);
    /* The client's desktop capture is an SRCCOPY blit whose source is the
     * screen DC; anything else falls through untouched. */
    if ((rop & 0x00ffffffu) == SRCCOPY && hdc_src != NULL &&
        source_is_screen_dc(hdc_src)) {
        if (feed_capture_frame(hdc_dest, x, y, width, height, source_x,
                               source_y, source_width, source_height)) {
            InterlockedIncrement(&capture_rendered);
            return TRUE;
        }
        /* XWayland cannot provide a current Wayland desktop after Portal
         * disconnects. Publish an empty frame while it reconnects instead of
         * presenting an old XWayland image as live video. X11 sessions retain
         * their ordinary native capture fallback. */
        char backend[32];
        char *configured_backend = NULL;
        int portal_capture = 0;
        DWORD backend_length = GetEnvironmentVariableA("UUR_CAPTURE_BACKEND",
                                                        backend, sizeof(backend));
        if (backend_length > 0 && backend_length < sizeof(backend)) {
            portal_capture = strcmp(backend, "portal") == 0;
        } else {
            configured_backend = read_ini_value("C:\\uur-bridge.ini",
                                                 "capture_backend");
            portal_capture = configured_backend != NULL &&
                             strcmp(configured_backend, "portal") == 0;
        }
        free(configured_backend);
        if (portal_capture && width > 0 && height > 0) {
            InterlockedIncrement(&capture_unavailable);
            BOOL cleared = PatBlt(hdc_dest, x, y, width, height, BLACKNESS);
            GdiFlush();
            return cleared;
        }
    }
    InterlockedIncrement(&capture_fallbacks);
    return FALSE;
}

BOOL WINAPI hook_bit_blt(HDC hdc_dest, int x, int y, int width, int height,
                         HDC hdc_src, int x_src, int y_src, DWORD rop)
{
    BOOL result;
    if (try_capture_feed(hdc_dest, x, y, rop, hdc_src, width, height, x_src,
                         y_src, width, height))
        return TRUE;
    result = original_bit_blt(hdc_dest, x, y, width, height, hdc_src, x_src,
                              y_src, rop);
    if (result && (rop & 0x00ffffffu) == SRCCOPY && hdc_src != NULL &&
        source_is_screen_dc(hdc_src))
        draw_cursor_overlay(hdc_dest, x, y, width, height, x_src, y_src,
                            width, height);
    return result;
}

BOOL WINAPI hook_stretch_blt(HDC hdc_dest, int x, int y, int w, int h,
                             HDC hdc_src, int xs, int ys, int sw, int sh,
                             DWORD rop)
{
    BOOL result;
    if (try_capture_feed(hdc_dest, x, y, rop, hdc_src, w, h, xs, ys, sw,
                         sh))
        return TRUE;
    result = original_stretch_blt(hdc_dest, x, y, w, h, hdc_src, xs, ys, sw,
                                  sh, rop);
    if (result && (rop & 0x00ffffffu) == SRCCOPY && hdc_src != NULL &&
        source_is_screen_dc(hdc_src))
        draw_cursor_overlay(hdc_dest, x, y, w, h, xs, ys, sw, sh);
    return result;
}

/* ---- diagnostics ------------------------------------------------------ */

static void hook_log(const char *text)
{
    char path[MAX_PATH];
    if (GetEnvironmentVariableA("UUR_HOOK_LOG", path, sizeof(path)) == 0 ||
        path[0] == '\0')
        lstrcpynA(path, "C:\\uur-bridge.log", sizeof(path));
    FILE *f = fopen(path, "a");
    if (f == NULL)
        return;
    fprintf(f, "%s\n", text);
    fclose(f);
}

/* ---- wire helpers ---------------------------------------------------- */

static void put_u32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}

static void put_u16(unsigned char *p, uint16_t v)
{
    p[0] = (unsigned char)(v);
    p[1] = (unsigned char)(v >> 8);
}

static void put_i32(unsigned char *p, int32_t v)
{
    put_u32(p, (uint32_t)v);
}

static int send_record(uint32_t kind, uint16_t code, uint16_t state,
                        int32_t a, int32_t b)
{
    unsigned char frame[16 + 16];
    put_u32(frame + 0, UUR_MAGIC);
    put_u32(frame + 4, UUR_VERSION);
    put_u32(frame + 8, kind);
    put_u32(frame + 12, 16);
    put_u32(frame + 16, kind);
    put_u16(frame + 20, code);
    put_u16(frame + 22, state);
    put_i32(frame + 24, a);
    put_i32(frame + 28, b);

    int total = 0;
    while (total < (int)sizeof(frame)) {
        int sent = send(hook_socket, (const char *)frame + total,
                        (int)sizeof(frame) - total, 0);
        if (sent <= 0) {
            closesocket(hook_socket);
            hook_socket = INVALID_SOCKET;
            start_bridge_retry();
            return 0;
        }
        total += sent;
    }
    return 1;
}

static FARPROC WINAPI hook_get_proc_address(HMODULE module, LPCSTR name)
{
    union {
        FARPROC generic;
        UINT(WINAPI *send_input)(UINT, LPINPUT, int);
        BOOL(WINAPI *bit_blt)(HDC, int, int, int, int, HDC, int, int, DWORD);
        BOOL(WINAPI *stretch_blt)(HDC, int, int, int, int, HDC, int, int,
                                  int, int, DWORD);
        HRESULT(WINAPI *create_dxgi_factory1)(REFIID, void **);
        HBITMAP(WINAPI *create_dib_section)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
        BOOL(WINAPI *delete_object)(HGDIOBJ);
    } replacement;
    FARPROC resolved = original_get_proc_address(module, name);
    if (name == NULL || ((uintptr_t)name >> 16) == 0)
        return resolved;
    if (lstrcmpiA(name, "SendInput") == 0) {
        replacement.send_input = hook_send_input;
        return replacement.generic;
    }
    if (lstrcmpiA(name, "BitBlt") == 0) {
        replacement.bit_blt = hook_bit_blt;
        return replacement.generic;
    }
    if (lstrcmpiA(name, "StretchBlt") == 0) {
        replacement.stretch_blt = hook_stretch_blt;
        return replacement.generic;
    }
    if (resolved != NULL && lstrcmpiA(name, "CreateDIBSection") == 0) {
        replacement.create_dib_section = hook_create_dib_section;
        return replacement.generic;
    }
    if (resolved != NULL && lstrcmpiA(name, "DeleteObject") == 0) {
        replacement.delete_object = hook_delete_object;
        return replacement.generic;
    }
    if (lstrcmpiA(name, "CreateDXGIFactory1") == 0) {
        replacement.create_dxgi_factory1 = hook_create_dxgi_factory1;
        return replacement.generic;
    }
    return resolved;
}

/* Minimal ini reader: returns a malloc'd value for `key` or NULL. */
static char *read_ini_value(const char *path, const char *key)
{
    FILE *f = fopen(path, "r");
    char line[512];
    size_t key_len;

    if (f == NULL)
        return NULL;
    key_len = strlen(key);
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, key_len) == 0 && line[key_len] == '=') {
            char *end = line + strlen(line);
            char *value = line + key_len + 1;
            char *out;
            size_t len;
            while (end > value &&
                   (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' '))
                *--end = '\0';
            fclose(f);
            len = (size_t)(end - value);
            out = malloc(len + 1);
            if (out != NULL) {
                memcpy(out, value, len);
                out[len] = '\0';
            }
            return out;
        }
    }
    fclose(f);
    return NULL;
}

static int connect_bridge(void)
{
    char port_text[16] = "47010";
    char token_hex[80] = "";
    char *value;

    /* Config: environment first (fast path for uur-spawned processes),
     * then C:\uur-bridge.ini — the file form survives any spawn path,
     * including the Wine service manager restarting the service without
     * uur's environment. */
    if ((value = getenv("UUR_BRIDGE_PORT")) != NULL && *value)
        lstrcpynA(port_text, value, sizeof(port_text));
    else {
        char *v = read_ini_value("C:\\uur-bridge.ini", "port");
        if (v != NULL) {
            lstrcpynA(port_text, v, sizeof(port_text));
            free(v);
        }
    }
    if ((value = getenv("UUR_BRIDGE_TOKEN")) != NULL && *value)
        lstrcpynA(token_hex, value, sizeof(token_hex));
    else {
        char *v = read_ini_value("C:\\uur-bridge.ini", "token");
        if (v != NULL) {
            lstrcpynA(token_hex, v, sizeof(token_hex));
            free(v);
        }
    }

    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        hook_log("connect: WSAStartup failed");
        return 0;
    }

    struct addrinfo hints;
    struct addrinfo *result = NULL;
    ZeroMemory(&hints, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    if (getaddrinfo("127.0.0.1", port_text, &hints, &result) != 0 ||
        result == NULL) {
        char buf[96];
        sprintf(buf, "connect: getaddrinfo failed (port=%s)", port_text);
        hook_log(buf);
        WSACleanup();
        return 0;
    }

    SOCKET sock = socket(result->ai_family, result->ai_socktype,
                         result->ai_protocol);
    if (sock == INVALID_SOCKET) {
        freeaddrinfo(result);
        WSACleanup();
        return 0;
    }
    if (connect(sock, result->ai_addr, (int)result->ai_addrlen) != 0) {
        char buf[96];
        sprintf(buf, "connect: failed wsae=%lu port=%s",
                (unsigned long)WSAGetLastError(), port_text);
        hook_log(buf);
        closesocket(sock);
        freeaddrinfo(result);
        WSACleanup();
        return 0;
    }
    freeaddrinfo(result);

    /* HELLO: header only; the daemon then reads 32 raw token bytes. */
    unsigned char hello[16];
    put_u32(hello + 0, UUR_MAGIC);
    put_u32(hello + 4, UUR_VERSION);
    put_u32(hello + 8, UUR_RECORD_HELLO);
    put_u32(hello + 12, 32);
    if (send(sock, (const char *)hello, sizeof(hello), 0) != sizeof(hello)) {
        closesocket(sock);
        WSACleanup();
        return 0;
    }

    unsigned char token[32];
    ZeroMemory(token, sizeof(token));
    for (int i = 0; i < 32; ++i) {
        unsigned byte = 0;
        char hex[3] = {token_hex[2 * i], token_hex[2 * i + 1], 0};
        if (hex[0] && hex[1]) {
            byte = (unsigned)strtoul(hex, NULL, 16);
        }
        token[i] = (unsigned char)byte;
    }
    if (send(sock, (const char *)token, sizeof(token), 0) != sizeof(token)) {
        closesocket(sock);
        WSACleanup();
        return 0;
    }

    /* Nothing to read until the daemon closes; the connection is
     * fire-and-forget from here on. */
    hook_socket = sock;
    return 1;
}

/* ---- SendInput interception ------------------------------------------ */

UINT WINAPI hook_send_input(UINT count, LPINPUT inputs, int size)
{
    if (inputs == NULL || size != (int)sizeof(INPUT))
        return original_send_input(count, inputs, size);

    UINT accepted = 0;
    for (UINT i = 0; i < count; ++i) {
        int input_forwarded = 0;
        AcquireSRWLockExclusive(&hook_input_lock);
        if (hook_socket != INVALID_SOCKET) {
            LPINPUT input = &inputs[i];
            if (input->type == INPUT_KEYBOARD && input->ki.wVk != 0 &&
                !(input->ki.dwFlags & (KEYEVENTF_SCANCODE | KEYEVENTF_UNICODE))) {
                uint16_t code = input->ki.wVk;
                if (input->ki.dwFlags & KEYEVENTF_EXTENDEDKEY)
                    code |= 0x100;
                /* The bridge is fire-and-forget, so it cannot reject a key
                 * after Wine has already been suppressed. Leave keys outside
                 * the shared invariant mapping on Wine's original path. */
                if (uur_virtual_key_supported(code))
                    input_forwarded = send_record(
                        UUR_RECORD_KEYBOARD, code,
                        (input->ki.dwFlags & KEYEVENTF_KEYUP) ? 0 : 1, 0, 0);
            } else if (input->type == INPUT_MOUSE) {
                DWORD flags = input->mi.dwFlags;
                int handled = 0;
                int sent = 1;
                /* MOVE and buttons may share one INPUT. The position must
                 * reach the bridge first so a click lands at its new target. */
                int zero_relative_move =
                    (flags & MOUSEEVENTF_MOVE) &&
                    !(flags & MOUSEEVENTF_ABSOLUTE) && input->mi.dx == 0 &&
                    input->mi.dy == 0;
                if ((flags & MOUSEEVENTF_MOVE) && !zero_relative_move) {
                    handled = 1;
                    int32_t x = input->mi.dx;
                    int32_t y = input->mi.dy;
                    if (flags & MOUSEEVENTF_ABSOLUTE) {
                        int virtual_desktop =
                            (flags & MOUSEEVENTF_VIRTUALDESK) != 0;
                        if (!map_absolute_pointer_to_portal(&x, &y, virtual_desktop)) {
                            RECT primary = {0, 0, GetSystemMetrics(SM_CXSCREEN),
                                            GetSystemMetrics(SM_CYSCREEN)};
                            primary_monitor_rect(&primary);
                            uur_absolute_pointer_to_virtual(
                                &x, &y, virtual_desktop,
                                GetSystemMetrics(SM_XVIRTUALSCREEN),
                                GetSystemMetrics(SM_YVIRTUALSCREEN),
                                GetSystemMetrics(SM_CXVIRTUALSCREEN),
                                GetSystemMetrics(SM_CYVIRTUALSCREEN),
                                primary.left, primary.top,
                                primary.right - primary.left,
                                primary.bottom - primary.top);
                        }
                    }
                    sent &= send_record(UUR_RECORD_MOUSE, 0xffff,
                        (flags & MOUSEEVENTF_ABSOLUTE) != 0, x, y);
                }
                if (flags & MOUSEEVENTF_WHEEL) {
                    handled = 1;
                    /* SendInput carries a signed 32-bit wheel delta. Keep
                     * fractional notches and large deltas intact for the host. */
                    sent &= send_record(UUR_RECORD_MOUSE, 0xfffe /*wheel*/,
                                0, 0,
                                (int32_t)input->mi.mouseData);
                }
                if (flags & MOUSEEVENTF_HWHEEL) {
                    handled = 1;
                    sent &= send_record(UUR_RECORD_MOUSE, 0xfffe,
                        0, 1, (int32_t)input->mi.mouseData);
                }
                if (flags & (MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP)) {
                    handled = 1;
                    /* mouseData is a bitmask; either or both side buttons can
                     * change in this INPUT, with independent press/release. */
                    for (uint16_t side = 0; side < 2; ++side) {
                        if (!(input->mi.mouseData & (XBUTTON1 << side)))
                            continue;
                        if (flags & MOUSEEVENTF_XDOWN)
                            sent &= send_record(UUR_RECORD_MOUSE, side + 8, 1, 0, 0);
                        if (flags & MOUSEEVENTF_XUP)
                            sent &= send_record(UUR_RECORD_MOUSE, side + 8, 0, 0, 0);
                    }
                }
                const DWORD down_flags[3] = {MOUSEEVENTF_LEFTDOWN,
                    MOUSEEVENTF_RIGHTDOWN, MOUSEEVENTF_MIDDLEDOWN};
                const DWORD up_flags[3] = {MOUSEEVENTF_LEFTUP,
                    MOUSEEVENTF_RIGHTUP, MOUSEEVENTF_MIDDLEUP};
                for (uint16_t button = 0; button < 3; ++button) {
                    if (flags & down_flags[button]) {
                        handled = 1;
                        sent &= send_record(UUR_RECORD_MOUSE,
                            button + 1, 1, 0, 0);
                    }
                    if (flags & up_flags[button]) {
                        handled = 1;
                        sent &= send_record(UUR_RECORD_MOUSE,
                            button + 1, 0, 0, 0);
                    }
                }
                /* UU's virtual-mouse path prefixes every absolute position
                 * with a zero relative move. It is a Win32-side state update,
                 * not host motion: leave the pure no-op on Wine's original
                 * path and avoid alternating the Linux mouse/tablet devices. */
                input_forwarded = handled && sent;
            }
        }
        ReleaseSRWLockExclusive(&hook_input_lock);
        /* Forwarded events are already injected by the host backend. Calling
         * Wine's SendInput again duplicates keys/clicks/relative motion and
         * races Portal's correctly scaled absolute position. Unicode and
         * scan-code input is not represented by the virtual-key protocol and
         * must use Wine's original path instead. */
        if (input_forwarded)
            ++accepted;
        else
            accepted += original_send_input(1, &inputs[i], size);
    }
    return accepted;
}

/* ---- IAT rewrite ------------------------------------------------------ */

static int patch_module_iat(HMODULE base)
{
    if (base == NULL)
        return 0;

    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)((char *)base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
        return 0;

    IMAGE_DATA_DIRECTORY *dir =
        &nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (dir->VirtualAddress == 0)
        return 0;

    int patched = 0;
    IMAGE_IMPORT_DESCRIPTOR *descriptor = (IMAGE_IMPORT_DESCRIPTOR *)
        ((char *)base + dir->VirtualAddress);
    for (; descriptor->Name != 0; ++descriptor) {
        const char *module_name = (const char *)base + descriptor->Name;
        if (_stricmp(module_name, "USER32.dll") != 0 &&
            _stricmp(module_name, "GDI32.dll") != 0 &&
            _stricmp(module_name, "dxgi.dll") != 0 &&
            _stricmp(module_name, "KERNEL32.dll") != 0 &&
            _stricmp(module_name, "KERNELBASE.dll") != 0)
            continue;

        IMAGE_THUNK_DATA *thunk = (IMAGE_THUNK_DATA *)
            ((char *)base + descriptor->FirstThunk);
        IMAGE_THUNK_DATA *original = thunk;
        if (descriptor->OriginalFirstThunk != 0)
            original = (IMAGE_THUNK_DATA *)
                ((char *)base + descriptor->OriginalFirstThunk);

        for (; original->u1.AddressOfData != 0; ++thunk, ++original) {
            if (original->u1.Ordinal & IMAGE_ORDINAL_FLAG)
                continue;
            IMAGE_IMPORT_BY_NAME *name = (IMAGE_IMPORT_BY_NAME *)
                ((char *)base + original->u1.AddressOfData);
            void *hook_fn = NULL;
            void **original_slot = NULL;

            if (lstrcmpiA(name->Name, "SendInput") == 0) {
                hook_fn = (void *)hook_send_input;
                original_slot = (void **)&original_send_input;
            } else if (lstrcmpiA(name->Name, "BitBlt") == 0) {
                hook_fn = (void *)hook_bit_blt;
                original_slot = (void **)&original_bit_blt;
            } else if (lstrcmpiA(name->Name, "StretchBlt") == 0) {
                hook_fn = (void *)hook_stretch_blt;
                original_slot = (void **)&original_stretch_blt;
            } else if (lstrcmpiA(name->Name, "CreateDIBSection") == 0) {
                hook_fn = (void *)hook_create_dib_section;
                original_slot = (void **)&original_create_dib_section;
            } else if (lstrcmpiA(name->Name, "DeleteObject") == 0) {
                hook_fn = (void *)hook_delete_object;
                original_slot = (void **)&original_delete_object;
            } else if (lstrcmpiA(name->Name, "CreateDXGIFactory1") == 0) {
                hook_fn = (void *)hook_create_dxgi_factory1;
                original_slot = (void **)&original_create_dxgi_factory1;
            } else if (lstrcmpiA(name->Name, "GetProcAddress") == 0) {
                hook_fn = (void *)hook_get_proc_address;
                original_slot = (void **)&original_get_proc_address;
            }
            if (hook_fn == NULL)
                continue;
            if (thunk->u1.Function == (uintptr_t)hook_fn)
                continue;
            *original_slot = (void *)thunk->u1.Function;
            DWORD old_protect = 0;
            if (!VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                                PAGE_READWRITE, &old_protect))
                continue;
            thunk->u1.Function = (uintptr_t)hook_fn;
            VirtualProtect(&thunk->u1.Function, sizeof(thunk->u1.Function),
                           old_protect, &old_protect);
            ++patched;
            InterlockedIncrement(&iat_hooks_installed);
        }
    }
    return patched;
}

static int patch_loaded_modules(void)
{
    int patched = 0;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE,
                                               GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    MODULEENTRY32W entry;
    entry.dwSize = sizeof(entry);
    if (Module32FirstW(snapshot, &entry)) {
        do {
            patched += patch_module_iat(entry.hModule);
        } while (Module32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return patched;
}

/* ---- late-module watchdog --------------------------------------------- */

static DWORD WINAPI patch_watchdog(LPVOID parameter)
{
    (void)parameter;
    for (;;) {
        Sleep(2000);
        patch_loaded_modules();
        char status_path[MAX_PATH];
        sprintf(status_path, "C:\\uur-capture-status-%lu.txt",
                (unsigned long)GetCurrentProcessId());
        FILE *f = fopen(status_path, "w");
        if (f != NULL) {
            fprintf(f,
                    "pid=%lu iat=%ld dxgi_f=%ld dxgi_a=%ld dxgi_o=%ld dup=%ld\n\
                     calls=%ld rendered=%ld fallbacks=%ld\n\
                     capture_us=%lld direct=%ld snapshot_copies=%ld unavailable=%ld\n",
                    GetCurrentProcessId(), iat_hooks_installed,
                    dxgi_factories_patched, dxgi_adapters_patched,
                    dxgi_outputs_patched, dxgi_duplicate_calls,
                    capture_calls, capture_rendered, capture_fallbacks,
                    (long long)capture_microseconds, capture_direct_copies,
                    capture_snapshot_copies, capture_unavailable);
            fclose(f);
        }
    }
    return 0;
}

static DWORD WINAPI bridge_retry_thread(LPVOID parameter)
{
    (void)parameter;
    for (int attempt = 0; attempt < 120; ++attempt) {
        Sleep(2000);
        AcquireSRWLockExclusive(&hook_input_lock);
        int connected = hook_socket != INVALID_SOCKET || connect_bridge();
        ReleaseSRWLockExclusive(&hook_input_lock);
        if (connected) {
            InterlockedExchange(&bridge_retry_running, 0);
            return 0;
        }
        char buf[96];
        sprintf(buf, "retry %d failed wsae=%lu", attempt,
                (unsigned long)WSAGetLastError());
        hook_log(buf);
    }
    InterlockedExchange(&bridge_retry_running, 0);
    return 0;
}

/* ---- entry ------------------------------------------------------------ */

/* Marker export: the wevtapi shim references this symbol purely so the
 * linker emits the uur-hook.dll dependency that carries the preload chain. */
void uur_hook_version(void) {}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved)
{
    (void)instance;
    (void)reserved;
    if (reason != DLL_PROCESS_ATTACH)
        return TRUE;

    DisableThreadLibraryCalls(instance);
    /* Dynamic lookups also need originals when no module imported these APIs. */
    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    union {
        FARPROC generic;
        HBITMAP(WINAPI *create_dib_section)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);
        BOOL(WINAPI *delete_object)(HGDIOBJ);
    } procedure;
    procedure.generic = GetProcAddress(gdi, "CreateDIBSection");
    original_create_dib_section = procedure.create_dib_section;
    procedure.generic = GetProcAddress(gdi, "DeleteObject");
    original_delete_object = procedure.delete_object;
    if (connect_bridge()) {
        hook_log("attach: bridge connected");
    } else {
        /* The daemon may simply not be bound yet: the service spawns the
         * server seconds before uur binds the bridge.  Retry in the
         * background instead of staying idle forever. */
        hook_log("attach: bridge unavailable, retrying");
        start_bridge_retry();
    }
    if (patch_loaded_modules() > 0) {
        hook_log("attach: SendInput/BitBlt imports rewritten");
    } else {
        hook_log("attach: no SendInput import found");
    }

    /* streamer.dll and friends can load after us; keep scanning so late
     * modules get their capture imports rewritten too. */
    HANDLE watchdog = CreateThread(NULL, 0, patch_watchdog, NULL, 0, NULL);
    if (watchdog != NULL)
        CloseHandle(watchdog);
    return TRUE;
}
