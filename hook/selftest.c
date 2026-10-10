/*
 * selftest — end-to-end probe without NetEase software.
 *
 * Statically imports wevtapi!EvtOpenPublisherMetadata so the Windows
 * loader resolves the preload chain (wevtapi.dll -> uur-hook.dll) at
 * process start, and USER32!SendInput so the hook has something to
 * rewrite.  Then it issues a few absolute pointer motions and one key
 * press/release pair.  The uur host bridge should log the connection and
 * inject the motions onto the real desktop; `uur pointer` before/after
 * observes the effect.
 */

#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frame-protocol.h"

/* Static import: forces wevtapi.dll (the shim) to load at startup. */
void *__stdcall EvtOpenPublisherMetadata(void *session, void *publisher_id,
                                         void *log_file_path,
                                         unsigned long locale,
                                         unsigned long flags);

static void move_absolute(int x, int y)
{
    INPUT input;
    ZeroMemory(&input, sizeof(input));
    input.type = INPUT_MOUSE;
    input.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE;
    input.mi.dx = (DWORD)((int64_t)x * 65535 / 1920);
    input.mi.dy = (DWORD)((int64_t)y * 65535 / 1080);
    SendInput(1, &input, sizeof(input));
}

static void put_u32(unsigned char *target, uint32_t value)
{
    target[0] = (unsigned char)value;
    target[1] = (unsigned char)(value >> 8);
    target[2] = (unsigned char)(value >> 16);
    target[3] = (unsigned char)(value >> 24);
}

static void put_u64(unsigned char *target, uint64_t value)
{
    put_u32(target, (uint32_t)value);
    put_u32(target + 4, (uint32_t)(value >> 32));
}

static int write_transport(const char *path, const unsigned char *data,
                           size_t size)
{
    DWORD written = 0;
    HANDLE file = CreateFileA(path, GENERIC_WRITE,
                              FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    int ok = WriteFile(file, data, (DWORD)size, &written, NULL) &&
             written == size;
    CloseHandle(file);
    return ok;
}

static int update_heartbeat(const char *path, uint64_t value)
{
    unsigned char bytes[8];
    DWORD written;
    put_u64(bytes, value);
    HANDLE file = CreateFileA(path, GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    int ok = SetFilePointer(file, UURF_HEARTBEAT_OFFSET, NULL, FILE_BEGIN) !=
                 INVALID_SET_FILE_POINTER &&
             WriteFile(file, bytes, sizeof(bytes), &written, NULL) &&
             written == sizeof(bytes);
    CloseHandle(file);
    return ok;
}

static int capture_round_trip(int native_removal)
{
    static const char frame_path[] = "C:\\uur-selftest-frames.bin";
    static const char second_frame_path[] = "C:\\uur-selftest-frames.bin.1";
    const uint32_t width = 2;
    const uint32_t height = 2;
    const uint32_t stride = width * 4;
    const size_t slot_bytes = uurf_slot_bytes(stride * height);
    const size_t total_bytes = UURF_HEADER_BYTES + UURF_SLOT_COUNT * slot_bytes;
    unsigned char *transport = calloc(1, total_bytes);
    HDC screen = NULL;
    HDC destination = NULL;
    HBITMAP bitmap = NULL;
    HGDIOBJ previous = NULL;
    HWND fallback_window = NULL;
    const char *stage = "transport setup";
    unsigned char *pixels = NULL;
    int passed = 0;

    if (transport == NULL)
        return 0;
    put_u32(transport, UURF_MAGIC);
    put_u32(transport + 4, UURF_VERSION);
    put_u32(transport + 8, width);
    put_u32(transport + 12, height);
    put_u32(transport + 16, stride);
    put_u32(transport + 20, 1);
    put_u64(transport + 24, 1);
    put_u64(transport + UURF_HEARTBEAT_OFFSET, 1);
    put_u32(transport + UURF_LOGICAL_X_OFFSET, 0);
    put_u32(transport + UURF_LOGICAL_Y_OFFSET, 0);
    put_u32(transport + UURF_LOGICAL_WIDTH_OFFSET, width);
    put_u32(transport + UURF_LOGICAL_HEIGHT_OFFSET, height);
    put_u32(transport + UURF_STREAM_COUNT_OFFSET, 2);
    unsigned char *slot = transport + uurf_slot_offset(1, slot_bytes);
    put_u64(slot, 2);

    /* Secondary display: independent green frame at normalized x=0. Create it
     * before stream zero, matching the helper's manifest publication order. */
    put_u32(transport + UURF_LOGICAL_X_OFFSET, 0);
    put_u32(transport + UURF_STREAM_INDEX_OFFSET, 1);
    for (size_t offset = 8; offset < slot_bytes; offset += 4) {
        slot[offset + 0] = 0x00;
        slot[offset + 1] = 0xff;
        slot[offset + 2] = 0x00;
        slot[offset + 3] = 0x00;
    }
    if (!write_transport(second_frame_path, transport, total_bytes))
        goto cleanup;

    /* Primary display: red frame at normalized x=2 and base manifest path. */
    put_u32(transport + UURF_LOGICAL_X_OFFSET, width);
    put_u32(transport + UURF_STREAM_INDEX_OFFSET, 0);
    for (size_t offset = 8; offset < slot_bytes; offset += 4) {
        slot[offset + 0] = 0x00;
        slot[offset + 1] = 0x00;
        slot[offset + 2] = 0xff;
        slot[offset + 3] = 0x00;
    }

    if (!write_transport(frame_path, transport, total_bytes))
        goto cleanup;
    if (!WritePrivateProfileStringA("bridge", "frame_path", frame_path,
                                    "C:\\uur-bridge.ini"))
        goto cleanup;
    if (!WritePrivateProfileStringA("bridge", "capture_backend", "portal",
                                    "C:\\uur-bridge.ini"))
        goto cleanup;

    BITMAPINFO info;
    ZeroMemory(&info, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = (LONG)width;
    info.bmiHeader.biHeight = -(LONG)height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;

    screen = GetDC(NULL);
    destination = CreateCompatibleDC(screen);
    bitmap = CreateDIBSection(destination, &info, DIB_RGB_COLORS,
                              (void **)&pixels, NULL, 0);
    if (screen == NULL || destination == NULL || bitmap == NULL || pixels == NULL)
        goto cleanup;
    previous = SelectObject(destination, bitmap);
    int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtual_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    stage = "primary frame";
    if (!BitBlt(destination, 0, 0, width, height, screen,
                virtual_x + virtual_width / 2, 0, SRCCOPY))
        goto cleanup;
    if (!(pixels[0] == 0x00 && pixels[1] == 0x00 && pixels[2] == 0xff))
        goto cleanup;
    ZeroMemory(pixels, stride * height);
    stage = "secondary frame";
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0,
                SRCCOPY))
        goto cleanup;
    if (!(pixels[0] == 0x00 && pixels[1] == 0xff && pixels[2] == 0x00))
        goto cleanup;

    /* A static desktop does not publish new frames. Producer liveness still
     * advances, and capture must keep returning its existing valid image. */
    Sleep(3300);
    stage = "static frame heartbeat";
    if (!update_heartbeat(frame_path, 2) ||
        !update_heartbeat(second_frame_path, 2))
        goto cleanup;
    ZeroMemory(pixels, stride * height);
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0,
                SRCCOPY))
        goto cleanup;
    if (!(pixels[0] == 0x00 && pixels[1] == 0xff && pixels[2] == 0x00))
        goto cleanup;

    /* Expired Portal data must not fall back to Wine's stale desktop. A white
     * X11 window makes the native fallback distinguishable from the required
     * black unavailable frame. */
    fallback_window = CreateWindowExA(WS_EX_TOPMOST, "STATIC", "",
        WS_POPUP | WS_VISIBLE | SS_WHITERECT, virtual_x, 0, 20, 20,
        NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (fallback_window == NULL)
        goto cleanup;
    UpdateWindow(fallback_window);
    Sleep(3300);
    stage = "native fallback window";
    if (GetPixel(screen, virtual_x + 1, 1) == RGB(0, 0, 0))
        goto cleanup;
    memset(pixels, 0xff, stride * height);
    stage = "expired frame blanking";
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0, SRCCOPY))
        goto cleanup;
    if (!(pixels[0] == 0 && pixels[1] == 0 && pixels[2] == 0))
        goto cleanup;
    memset(pixels, 0xff, stride * height);
    stage = "expired frame stays rejected";
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0, SRCCOPY) ||
        pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0)
        goto cleanup;
    if (!update_heartbeat(frame_path, 3) ||
        !update_heartbeat(second_frame_path, 3))
        goto cleanup;
    stage = "resumed frame";
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0, SRCCOPY))
        goto cleanup;
    if (!(pixels[0] == 0 && pixels[1] == 0xff && pixels[2] == 0))
        goto cleanup;

    if (!native_removal) {
        passed = 1;
        goto cleanup;
    }
    stage = "removed transport";
    /* A mapped file cannot always be deleted through Windows semantics. The
     * real producer unlinks it from Linux; the native test peer does the same. */
    if (!write_transport("C:\\uur-selftest-remove-request", (const unsigned char *)"", 0))
        goto cleanup;
    DWORD removal_start = GetTickCount();
    while (GetFileAttributesA("C:\\uur-selftest-remove-complete") ==
           INVALID_FILE_ATTRIBUTES && GetTickCount() - removal_start < 3000u)
        Sleep(10);
    if (GetFileAttributesA("C:\\uur-selftest-remove-complete") == INVALID_FILE_ATTRIBUTES)
        goto cleanup;
    Sleep(280);
    memset(pixels, 0xff, stride * height);
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0, SRCCOPY) ||
        pixels[0] != 0 || pixels[1] != 0 || pixels[2] != 0)
        goto cleanup;

    /* The replacement helper creates new inodes and restarts its sequence and
     * heartbeat. The same consumer must read the replacement's blue pixels. */
    stage = "replacement transport";
    put_u64(transport + UURF_HEARTBEAT_OFFSET, 1);
    put_u32(transport + UURF_LOGICAL_X_OFFSET, 0);
    put_u32(transport + UURF_STREAM_INDEX_OFFSET, 1);
    for (size_t offset = 8; offset < slot_bytes; offset += 4) {
        slot[offset] = 0xff;
        slot[offset + 1] = 0;
        slot[offset + 2] = 0;
    }
    if (!write_transport(second_frame_path, transport, total_bytes))
        goto cleanup;
    put_u32(transport + UURF_LOGICAL_X_OFFSET, width);
    put_u32(transport + UURF_STREAM_INDEX_OFFSET, 0);
    if (!write_transport(frame_path, transport, total_bytes))
        goto cleanup;
    if (!BitBlt(destination, 0, 0, width, height, screen, virtual_x, 0, SRCCOPY))
        goto cleanup;
    passed = pixels[0] == 0xff && pixels[1] == 0 && pixels[2] == 0;

cleanup:
    if (!passed)
        fprintf(stderr, "capture round-trip failed at %s\n", stage);
    if (fallback_window != NULL)
        DestroyWindow(fallback_window);
    if (previous != NULL)
        SelectObject(destination, previous);
    if (bitmap != NULL)
        DeleteObject(bitmap);
    if (destination != NULL)
        DeleteDC(destination);
    if (screen != NULL)
        ReleaseDC(NULL, screen);
    DeleteFileA(frame_path);
    DeleteFileA(second_frame_path);
    DeleteFileA("C:\\uur-selftest-remove-request");
    DeleteFileA("C:\\uur-selftest-remove-complete");
    free(transport);
    return passed;
}

static void dispatch_pending_messages(int *key_messages, int *b_down, int *b_up,
                                      int *unicode_chars, int *f13_down,
                                      int *f13_up)
{
    MSG message;
    while (PeekMessageW(&message, NULL, 0, 0, PM_REMOVE)) {
        if (message.message == WM_KEYDOWN || message.message == WM_KEYUP) {
            if (message.wParam == 'A' || message.wParam == VK_CONTROL)
                ++*key_messages;
            if (message.wParam == 'B') {
                if (message.message == WM_KEYDOWN)
                    ++*b_down;
                else
                    ++*b_up;
            }
            if (message.wParam == VK_F13) {
                if (message.message == WM_KEYDOWN)
                    ++*f13_down;
                else
                    ++*f13_up;
            }
        }
        if (message.message == WM_CHAR && message.wParam == 0x4e2d)
            ++*unicode_chars;
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
}

static int keyboard_forwarding_round_trip(void)
{
    HWND window = CreateWindowExW(WS_EX_TOPMOST, L"STATIC", L"Input test",
        WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 300, 200,
        NULL, NULL, GetModuleHandleW(NULL), NULL);
    if (window == NULL)
        return 25;
    SetForegroundWindow(window);
    SetFocus(window);
    Sleep(100);
    int keys = 0, b_down = 0, b_up = 0, unicode_chars = 0;
    int f13_down = 0, f13_up = 0;
    dispatch_pending_messages(&keys, &b_down, &b_up, &unicode_chars,
                              &f13_down, &f13_up);
    keys = 0;

    INPUT input[4];
    ZeroMemory(input, sizeof(input));
    for (int i = 0; i < 4; ++i)
        input[i].type = INPUT_KEYBOARD;
    input[0].ki.wVk = input[1].ki.wVk = 'A';
    input[1].ki.dwFlags = KEYEVENTF_KEYUP;
    input[2].ki.wVk = input[3].ki.wVk = VK_CONTROL;
    input[2].ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    input[3].ki.dwFlags = KEYEVENTF_EXTENDEDKEY | KEYEVENTF_KEYUP;
    int result = SendInput(4, input, sizeof(INPUT)) == 4 ? 0 : 26;
    Sleep(100);
    dispatch_pending_messages(&keys, &b_down, &b_up, &unicode_chars,
                              &f13_down, &f13_up);
    /* The mock bridge never injects. A local key here proves Wine received
     * a second injection after the hook had already forwarded the event. */
    if (keys != 0)
        result = 27;

    ZeroMemory(input, sizeof(input));
    for (int i = 0; i < 4; ++i)
        input[i].type = INPUT_KEYBOARD;
    input[0].ki.wScan = input[1].ki.wScan = 0x30; /* scan code for B */
    input[0].ki.dwFlags = KEYEVENTF_SCANCODE;
    input[1].ki.dwFlags = KEYEVENTF_SCANCODE | KEYEVENTF_KEYUP;
    input[2].ki.wScan = input[3].ki.wScan = 0x4e2d;
    input[2].ki.dwFlags = KEYEVENTF_UNICODE;
    input[3].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
    if (SendInput(4, input, sizeof(INPUT)) != 4)
        result = 28;
    Sleep(100);
    dispatch_pending_messages(&keys, &b_down, &b_up, &unicode_chars,
                              &f13_down, &f13_up);
    if (b_down != 1 || b_up != 1 || unicode_chars != 1)
        result = 29;

    /* F13 is intentionally outside the native bridge mapping. It must remain
     * on Wine's original SendInput path instead of being sent and then rejected
     * asynchronously by the host backend. */
    INPUT unsupported[2];
    ZeroMemory(unsupported, sizeof(unsupported));
    unsupported[0].type = unsupported[1].type = INPUT_KEYBOARD;
    unsupported[0].ki.wVk = unsupported[1].ki.wVk = VK_F13;
    unsupported[1].ki.dwFlags = KEYEVENTF_KEYUP;
    if (SendInput(2, unsupported, sizeof(INPUT)) != 2)
        result = 30;
    Sleep(100);
    dispatch_pending_messages(&keys, &b_down, &b_up, &unicode_chars,
                              &f13_down, &f13_up);
    if (f13_down != 1 || f13_up != 1)
        result = 31;
    DestroyWindow(window);
    if (result != 0)
        fprintf(stderr,
                "keyboard forwarding failed: code=%d local=%d B=%d/%d Unicode=%d F13=%d/%d\n",
                result, keys, b_down, b_up, unicode_chars, f13_down, f13_up);
    return result;
}

static int input_forwarding_round_trip(void)
{
    POINT before, after;
    if (!GetCursorPos(&before))
        return 20;

    INPUT events[3];
    ZeroMemory(events, sizeof(events));
    events[0].type = INPUT_MOUSE;
    events[0].mi.dwFlags = MOUSEEVENTF_MOVE;
    events[0].mi.dx = 111;
    events[0].mi.dy = 77;
    events[1].type = INPUT_MOUSE;
    events[1].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE |
                           MOUSEEVENTF_LEFTDOWN;
    events[1].mi.dx = 16384;
    events[1].mi.dy = 16384;
    events[2].type = INPUT_MOUSE;
    events[2].mi.dwFlags = MOUSEEVENTF_LEFTUP;
    if (SendInput(3, events, sizeof(INPUT)) != 3)
        return 21;
    const DWORD wheel_flags[] = {MOUSEEVENTF_WHEEL, MOUSEEVENTF_WHEEL,
                                MOUSEEVENTF_HWHEEL, MOUSEEVENTF_HWHEEL};
    const int32_t wheel_deltas[] = {120, -120, 60, 65536};
    INPUT wheel[4];
    ZeroMemory(wheel, sizeof(wheel));
    for (int i = 0; i < 4; ++i) {
        wheel[i].type = INPUT_MOUSE;
        wheel[i].mi.dwFlags = wheel_flags[i];
        wheel[i].mi.mouseData = (DWORD)wheel_deltas[i];
    }
    if (SendInput(4, wheel, sizeof(INPUT)) != 4)
        return 23;
    INPUT sides;
    ZeroMemory(&sides, sizeof(sides));
    sides.type = INPUT_MOUSE;
    sides.mi.dwFlags = MOUSEEVENTF_XDOWN | MOUSEEVENTF_XUP;
    sides.mi.mouseData = XBUTTON1 | XBUTTON2;
    if (SendInput(1, &sides, sizeof(INPUT)) != 1)
        return 24;
    Sleep(100);
    if (!GetCursorPos(&after) || before.x != after.x || before.y != after.y) {
        fprintf(stderr, "forwarded mouse input was injected again by Wine\n");
        return 22;
    }
    INPUT no_op;
    ZeroMemory(&no_op, sizeof(no_op));
    no_op.type = INPUT_MOUSE;
    no_op.mi.dwFlags = MOUSEEVENTF_MOVE;
    /* UU virtual mouse sends this Win32-side state update before every
     * absolute position. It must stay on Wine's path and out of the bridge. */
    if (SendInput(1, &no_op, sizeof(INPUT)) != 1)
        return 20;
    int keyboard_result = keyboard_forwarding_round_trip();
    if (keyboard_result != 0)
        return keyboard_result;
    puts("hook input forwarding, click order, and keyboard fallback test passed");
    return 0;
}

int main(int argc, char **argv)
{
    (void)EvtOpenPublisherMetadata(NULL, NULL, NULL, 0, 0);
    if (argc > 1 && strcmp(argv[1], "--input-forwarding") == 0)
        return input_forwarding_round_trip();

    int native_removal = argc > 1 && strcmp(argv[1], "--native-removal") == 0;
    if (!capture_round_trip(native_removal)) {
        fprintf(stderr, "capture adapter round-trip failed\n");
        return 10;
    }

    move_absolute(200, 200);
    Sleep(400);
    move_absolute(700, 500);
    Sleep(400);
    move_absolute(960, 540);
    Sleep(400);

    INPUT key;
    ZeroMemory(&key, sizeof(key));
    key.type = INPUT_KEYBOARD;
    key.ki.wVk = 0x41; /* 'A' */
    SendInput(1, &key, sizeof(key));
    key.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &key, sizeof(key));


    Sleep(200);
    printf("hook capture and input smoke test passed\n");
    return 0;
}
