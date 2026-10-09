/* Real Wine/GDI orientation regression: compare the capture adapter with GDI
 * using asymmetric images, both DIB directions, and destination transforms. */
#include <windows.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "frame-protocol.h"

void *__stdcall EvtOpenPublisherMetadata(void *, void *, void *, unsigned long, unsigned long);
typedef HBITMAP(WINAPI *create_dib_fn)(HDC, const BITMAPINFO *, UINT, void **, HANDLE, DWORD);

static const uint32_t patterns[2][6] = {
    {0xff0000, 0xff8000, 0x00ff00, 0x0000ff, 0xff00ff, 0xffff00},
    {0x204080, 0x804020, 0x40c060, 0xa040c0, 0xc0b020, 0x20b0c0},
};

static int write_frame(const char *path, unsigned int index)
{
    const size_t slot_bytes = uurf_slot_bytes(sizeof(patterns[0]));
    unsigned char data[UURF_HEADER_BYTES +
                       UURF_SLOT_COUNT * ((8 + sizeof(patterns[0]) + 7) & ~7)];
    memset(data, 0, sizeof(data));
    uint32_t words[6] = {UURF_MAGIC, UURF_VERSION, 3, 2, 12, 1};
    memcpy(data, words, sizeof(words));
    uint64_t one = 1, sequence = 2;
    int32_t geometry[6] = {index ? 0 : 3, 0, 3, 2, (int32_t)index, 2};
    memcpy(data + 24, &one, sizeof(one));
    memcpy(data + UURF_LOGICAL_X_OFFSET, geometry, sizeof(geometry));
    memcpy(data + UURF_HEARTBEAT_OFFSET, &one, sizeof(one));
    size_t offset = uurf_slot_offset(1, slot_bytes);
    memcpy(data + offset, &sequence, sizeof(sequence));
    memcpy(data + offset + 8, patterns[index], sizeof(patterns[index]));
    HANDLE file = CreateFileA(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               NULL, CREATE_ALWAYS, 0, NULL);
    if (file == INVALID_HANDLE_VALUE)
        return 0;
    DWORD written = 0;
    int ok = WriteFile(file, data, sizeof(data), &written, NULL) && written == sizeof(data);
    CloseHandle(file);
    return ok;
}

static int compare_case(HDC screen, create_dib_fn native_create,
                       create_dib_fn dynamic_create, int stream, int top_down,
                       int scale, int mode)
{
    int width = 3 * scale, height = 2 * scale;
    BITMAPINFO info;
    memset(&info, 0, sizeof(info));
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = top_down ? -height : height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    HDC dc[2] = {CreateCompatibleDC(screen), CreateCompatibleDC(screen)};
    HBITMAP bitmap[2] = {NULL, NULL};
    HGDIOBJ old[2] = {NULL, NULL};
    uint32_t *pixels[2] = {NULL, NULL};
    int passed = 0;
    for (int i = 0; i < 2; ++i) {
        if (i == 1 || mode == 2)
            bitmap[i] = native_create(dc[i], &info, DIB_RGB_COLORS, (void **)&pixels[i], NULL, 0);
        else if (mode == 1)
            bitmap[i] = dynamic_create(dc[i], &info, DIB_RGB_COLORS, (void **)&pixels[i], NULL, 0);
        else
            bitmap[i] = CreateDIBSection(dc[i], &info, DIB_RGB_COLORS, (void **)&pixels[i], NULL, 0);
        if (bitmap[i] == NULL || pixels[i] == NULL)
            goto cleanup;
        old[i] = SelectObject(dc[i], bitmap[i]);
        memset(pixels[i], 0x11, (size_t)width * height * 4);
        SetStretchBltMode(dc[i], COLORONCOLOR);
        if (mode == 3 && SetLayout(dc[i], LAYOUT_RTL) == GDI_ERROR)
            goto cleanup;
        if (mode == 4) {
            XFORM transform = {-1, 0, 0, -1, (FLOAT)width, (FLOAT)height};
            if (!SetGraphicsMode(dc[i], GM_ADVANCED) || !SetWorldTransform(dc[i], &transform))
                goto cleanup;
        }
    }
    int x = mode >= 5 ? width : 0;
    int y = mode == 6 ? height : 0;
    int destination_width = mode >= 5 ? -width : width;
    int destination_height = mode == 6 ? -height : height;
    int virtual_x = GetSystemMetrics(SM_XVIRTUALSCREEN);
    int virtual_y = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int source_width = GetSystemMetrics(SM_CXVIRTUALSCREEN) / 2;
    int source_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (!StretchBlt(dc[0], x, y, destination_width, destination_height, screen,
                     virtual_x + (stream ? 0 : source_width), virtual_y,
                     source_width, source_height, SRCCOPY))
        goto cleanup;
    BITMAPINFO source = info;
    source.bmiHeader.biWidth = 3;
    source.bmiHeader.biHeight = -2;
    if (!StretchDIBits(dc[1], x, y, destination_width, destination_height,
                       0, 0, 3, 2, patterns[stream], &source, DIB_RGB_COLORS, SRCCOPY))
        goto cleanup;
    GdiFlush();
    passed = 1;
    for (int i = 0; i < width * height; ++i) {
        if ((pixels[0][i] & 0xffffff) != (pixels[1][i] & 0xffffff)) {
            fprintf(stderr, "orientation mismatch: stream=%d top_down=%d scale=%d mode=%d pixel=%d actual=%06lx expected=%06lx\n",
                    stream, top_down, scale, mode, i,
                    (unsigned long)(pixels[0][i] & 0xffffff),
                    (unsigned long)(pixels[1][i] & 0xffffff));
            passed = 0;
            break;
        }
    }
cleanup:
    for (int i = 0; i < 2; ++i) {
        if (old[i] != NULL) SelectObject(dc[i], old[i]);
        if (bitmap[i] != NULL) DeleteObject(bitmap[i]);
        if (dc[i] != NULL) DeleteDC(dc[i]);
    }
    return passed;
}

int main(void)
{
    (void)EvtOpenPublisherMetadata(NULL, NULL, NULL, 0, 0);
    const char *path = "C:\\uur-orientation-selftest.bin";
    const char *secondary = "C:\\uur-orientation-selftest.bin.1";
    if (!write_frame(secondary, 1) || !write_frame(path, 0))
        return 10;
    SetEnvironmentVariableA("UUR_FRAME_PATH_WIN", path);
    SetEnvironmentVariableA("UUR_CAPTURE_BACKEND", "portal");
    union {
        FARPROC generic;
        FARPROC(WINAPI *resolve)(HMODULE, LPCSTR);
        create_dib_fn create;
    } procedure;
    HMODULE gdi = GetModuleHandleA("gdi32.dll");
    procedure.generic = GetProcAddress(GetModuleHandleA("kernel32.dll"), "GetProcAddress");
    FARPROC(WINAPI *native_resolve)(HMODULE, LPCSTR) = procedure.resolve;
    procedure.generic = native_resolve(gdi, "CreateDIBSection");
    create_dib_fn native_create = procedure.create;
    procedure.generic = GetProcAddress(gdi, "CreateDIBSection");
    create_dib_fn dynamic_create = procedure.create;
    HDC screen = GetDC(NULL);
    int passed = 0;
    for (int stream = 0; stream < 2; ++stream)
        for (int direction = 0; direction < 2; ++direction)
            for (int scale = 1; scale <= 2; ++scale)
                for (int mode = 0; mode <= 6; ++mode) {
                    if (!compare_case(screen, native_create, dynamic_create, stream,
                                        direction, scale, mode))
                        goto cleanup;
                    ++passed;
                }
cleanup:
    ReleaseDC(NULL, screen);
    DeleteFileA(path);
    DeleteFileA(secondary);
    printf("orientation cases passed: %d/56\n", passed);
    if (passed != 56)
        return 11;
    /* Ensure correctness did not come from disabling every fast capture. */
    Sleep(2100);
    char status_path[MAX_PATH], status[1024] = {0};
    snprintf(status_path, sizeof(status_path), "C:\\uur-capture-status-%lu.txt",
             (unsigned long)GetCurrentProcessId());
    FILE *file = fopen(status_path, "r");
    if (file == NULL)
        return 12;
    size_t size = fread(status, 1, sizeof(status) - 1, file);
    fclose(file);
    status[size] = '\0';
    const char *direct = strstr(status, "direct=");
    long direct_copies = direct != NULL ? strtol(direct + 7, NULL, 10) : 0;
    printf("orientation fast copies: %ld (expected at least 16)\n", direct_copies);
    return direct_copies >= 16 ? 0 : 13;
}
