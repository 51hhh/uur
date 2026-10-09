#define _GNU_SOURCE
/*
 * uur-pw-capture — PipeWire ScreenCast consumer.
 *
 * Each portal monitor stream is published independently. Stream zero uses
 * UUR_FRAME_PATH and every later stream uses UUR_FRAME_PATH.N. The v2 frame
 * header carries the monitor's logical desktop rectangle so the Windows hook
 * can route UU's per-display GDI capture to the matching PipeWire stream.
 */

#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include "../hook/frame-protocol.h"
#include "layout.h"

#define FRAME_FORMAT_BGRX 1
#define MAX_CAPTURE_STREAMS UURF_MAX_STREAMS

struct frame_header {
    _Atomic uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    _Atomic uint64_t total_frames;
    int32_t logical_x;
    int32_t logical_y;
    uint32_t logical_width;
    uint32_t logical_height;
    uint32_t stream_index;
    uint32_t stream_count;
    _Atomic uint64_t heartbeat;
};

_Static_assert(sizeof(struct frame_header) == UURF_HEADER_BYTES,
               "frame protocol header size changed");

struct frame_slot {
    _Atomic uint64_t seq;
    /* frame bytes follow */
};

struct capture;

struct capture_stream {
    struct capture *capture;
    struct pw_stream *stream;
    struct spa_hook listener;
    uint32_t node_id;
    int32_t logical_x;
    int32_t logical_y;
    uint32_t logical_width;
    uint32_t logical_height;
    int has_geometry;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    enum spa_video_format format;
    int ready;
    uint64_t counter;
    int shm_fd;
    uint8_t *shm;
    size_t shm_size;
    char frame_path[PATH_MAX];
};

struct capture {
    struct pw_main_loop *loop;
    struct capture_stream streams[MAX_CAPTURE_STREAMS];
    size_t stream_count;
    int transports_ready;
    int failed;
    char frame_path[PATH_MAX];
    struct spa_source *heartbeat_timer;
};

static struct capture *active_capture;

/* ScreenCast is damage-driven: unchanged pixels can legitimately produce no
 * frames indefinitely. A separate producer heartbeat distinguishes that from
 * a helper which died while Wine still has its old file mapped. */
static void heartbeat_tick(void *userdata, uint64_t expirations)
{
    struct capture *capture = userdata;
    for (size_t i = 0; i < capture->stream_count; ++i) {
        if (capture->streams[i].shm == NULL)
            continue;
        struct frame_header *header =
            (struct frame_header *)capture->streams[i].shm;
        atomic_fetch_add_explicit(&header->heartbeat, expirations,
                                  memory_order_release);
    }
}

static void stop_capture(int signal_number)
{
    (void)signal_number;
    if (active_capture != NULL && active_capture->loop != NULL)
        pw_main_loop_quit(active_capture->loop);
}

static struct frame_slot *slot_at(struct capture_stream *stream, uint64_t n)
{
    size_t slot_bytes = uurf_slot_bytes(
        (size_t)stream->stride * stream->height);
    uint8_t *base = stream->shm + uurf_slot_offset(n, slot_bytes);
    return (struct frame_slot *)base;
}

static uint8_t *slot_frame(struct frame_slot *slot)
{
    return (uint8_t *)slot + sizeof(struct frame_slot);
}

static int stream_path(const char *base, size_t index, char *path,
                       size_t capacity)
{
    int length = index == 0 ? snprintf(path, capacity, "%s", base)
                            : snprintf(path, capacity, "%s.%zu", base, index);
    return length >= 0 && (size_t)length < capacity ? 0 : -1;
}

static int parse_streams(struct capture *capture, const char *description)
{
    char *copy = strdup(description != NULL ? description : "");
    if (copy == NULL)
        return -1;

    char *save = NULL;
    for (char *entry = strtok_r(copy, ";", &save); entry != NULL;
         entry = strtok_r(NULL, ";", &save)) {
        if (capture->stream_count >= MAX_CAPTURE_STREAMS) {
            fprintf(stderr, "too many capture streams (maximum %u)\n",
                    (unsigned int)MAX_CAPTURE_STREAMS);
            free(copy);
            return -1;
        }
        unsigned int node = 0;
        int x = 0;
        int y = 0;
        int width = 0;
        int height = 0;
        char trailing = '\0';
        if (sscanf(entry, "%u,%d,%d,%d,%d%c", &node, &x, &y, &width,
                   &height, &trailing) != 5 || width < 0 || height < 0) {
            fprintf(stderr, "invalid UUR_PW_STREAMS entry: %s\n", entry);
            free(copy);
            return -1;
        }
        struct capture_stream *stream =
            &capture->streams[capture->stream_count++];
        stream->capture = capture;
        stream->node_id = (uint32_t)node;
        stream->logical_x = (int32_t)x;
        stream->logical_y = (int32_t)y;
        stream->logical_width = (uint32_t)width;
        stream->logical_height = (uint32_t)height;
        stream->has_geometry = width > 0 && height > 0;
        stream->shm_fd = -1;
    }
    free(copy);
    if (capture->stream_count == 0) {
        fprintf(stderr, "UUR_PW_STREAMS contains no streams\n");
        return -1;
    }
    return 0;
}

static int transport_setup(struct capture_stream *stream, size_t stream_index,
                           size_t stream_count)
{
    if (stream->stride == 0 || stream->height == 0 ||
        (size_t)stream->stride > (SIZE_MAX - 15u) / stream->height) {
        fprintf(stderr, "invalid capture geometry for stream %zu\n",
                stream_index);
        return -1;
    }
    size_t frame_bytes = (size_t)stream->stride * stream->height;
    size_t slot_bytes = uurf_slot_bytes(frame_bytes);
    if (slot_bytes > (SIZE_MAX - UURF_HEADER_BYTES) / UURF_SLOT_COUNT) {
        fprintf(stderr, "capture transport is too large for stream %zu\n",
                stream_index);
        return -1;
    }
    size_t total = UURF_HEADER_BYTES + UURF_SLOT_COUNT * slot_bytes;
    stream->shm_fd = open(stream->frame_path,
                          O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (stream->shm_fd < 0) {
        perror("open frame transport");
        return -1;
    }
    if (ftruncate(stream->shm_fd, (off_t)total) < 0) {
        perror("ftruncate");
        return -1;
    }
    stream->shm = mmap(NULL, total, PROT_READ | PROT_WRITE, MAP_SHARED,
                       stream->shm_fd, 0);
    if (stream->shm == MAP_FAILED) {
        perror("mmap");
        stream->shm = NULL;
        return -1;
    }
    stream->shm_size = total;

    struct frame_header *header = (struct frame_header *)stream->shm;
    atomic_init(&header->magic, 0);
    header->version = UURF_VERSION;
    header->width = stream->width;
    header->height = stream->height;
    header->stride = stream->stride;
    header->format = FRAME_FORMAT_BGRX;
    atomic_init(&header->total_frames, 0);
    header->logical_x = stream->logical_x;
    header->logical_y = stream->logical_y;
    header->logical_width = stream->logical_width;
    header->logical_height = stream->logical_height;
    header->stream_index = (uint32_t)stream_index;
    header->stream_count = (uint32_t)stream_count;
    atomic_init(&header->heartbeat, 1);
    for (uint32_t i = 0; i < UURF_SLOT_COUNT; ++i)
        atomic_init(&slot_at(stream, (uint64_t)i)->seq, 0);
    /* Publish the magic last so a consumer never accepts a partially
     * initialized file that has just appeared at the manifest path. */
    atomic_store_explicit(&header->magic, UURF_MAGIC, memory_order_release);
    return 0;
}

static void transports_reset(struct capture *capture)
{
    /* Invalidate the old inode before replacing the manifest. Existing Wine
     * mappings observe this immediately and reopen the new transport instead
     * of serving an old frame until the heartbeat timeout. */
    for (size_t i = 0; i < capture->stream_count; ++i) {
        struct capture_stream *stream = &capture->streams[i];
        if (stream->shm != NULL) {
            struct frame_header *header = (struct frame_header *)stream->shm;
            atomic_store_explicit(&header->total_frames, 0,
                                  memory_order_release);
            atomic_store_explicit(&header->magic, 0, memory_order_release);
            munmap(stream->shm, stream->shm_size);
            stream->shm = NULL;
            stream->shm_size = 0;
        }
        if (stream->shm_fd >= 0) {
            close(stream->shm_fd);
            stream->shm_fd = -1;
        }
        unlink(stream->frame_path);
        stream->counter = 0;
    }
    capture->transports_ready = 0;
}

static int transports_setup(struct capture *capture)
{
    if (capture->transports_ready)
        return 1;
    for (size_t i = 0; i < capture->stream_count; ++i) {
        if (!capture->streams[i].ready)
            return 0;
    }

    uur_capture_layout_stream layout[MAX_CAPTURE_STREAMS];
    memset(layout, 0, sizeof(layout));
    int has_geometry = 1;
    for (size_t i = 0; i < capture->stream_count; ++i) {
        struct capture_stream *stream = &capture->streams[i];
        layout[i].logical_x = stream->logical_x;
        layout[i].logical_y = stream->logical_y;
        layout[i].logical_width = stream->logical_width;
        layout[i].logical_height = stream->logical_height;
        layout[i].has_geometry = stream->has_geometry;
        layout[i].pixel_width = stream->width;
        layout[i].pixel_height = stream->height;
        has_geometry = has_geometry && stream->has_geometry;
    }
    uint32_t desktop_width = 0;
    uint32_t desktop_height = 0;
    if (!uur_capture_layout(layout, capture->stream_count, &desktop_width,
                            &desktop_height)) {
        fprintf(stderr, "invalid capture stream topology\n");
        return -1;
    }
    if (!has_geometry) {
        for (size_t i = 0; i < capture->stream_count; ++i) {
            capture->streams[i].logical_x =
                (int32_t)layout[i].destination_x;
            capture->streams[i].logical_y =
                (int32_t)layout[i].destination_y;
            capture->streams[i].logical_width = layout[i].destination_width;
            capture->streams[i].logical_height = layout[i].destination_height;
        }
    }

    /* Create secondary files first. The base file is also the manifest, so it
     * becomes visible only after every independent stream can be opened. */
    for (size_t offset = 1; offset <= capture->stream_count; ++offset) {
        size_t i = offset % capture->stream_count;
        struct capture_stream *stream = &capture->streams[i];
        if (transport_setup(stream, i, capture->stream_count) < 0)
            return -1;
        fprintf(stderr,
                "capture stream %zu node %u: %ux%u at logical %d,%d %ux%u\n",
                i, stream->node_id, stream->width, stream->height,
                stream->logical_x, stream->logical_y, stream->logical_width,
                stream->logical_height);
    }
    capture->transports_ready = 1;
    return 1;
}

static int format_bytes_per_pixel(enum spa_video_format format)
{
    return format == SPA_VIDEO_FORMAT_BGR || format == SPA_VIDEO_FORMAT_RGB
               ? 3
               : 4;
}

static void publish_frame(struct capture_stream *stream, const uint8_t *pixels,
                          uint32_t source_stride, uint32_t source_rows)
{
    if (stream->shm == NULL)
        return;
    int bytes_per_pixel = format_bytes_per_pixel(stream->format);
    if (source_rows == 0 ||
        source_stride < stream->width * (uint32_t)bytes_per_pixel)
        return;

    stream->counter += 1;
    struct frame_slot *slot = slot_at(stream, stream->counter);
    atomic_store_explicit(&slot->seq, 2 * stream->counter - 1,
                          memory_order_release);
    uint8_t *destination = slot_frame(slot);
    uint32_t rows = source_rows < stream->height ? source_rows : stream->height;
    for (uint32_t y = 0; y < rows; ++y) {
        const uint8_t *source_row = pixels + (size_t)y * source_stride;
        uint8_t *destination_row = destination + (size_t)y * stream->stride;
        if (stream->format == SPA_VIDEO_FORMAT_BGRx ||
            stream->format == SPA_VIDEO_FORMAT_BGRA) {
            memcpy(destination_row, source_row, stream->stride);
            continue;
        }
        for (uint32_t x = 0; x < stream->width; ++x) {
            const uint8_t *source =
                source_row + (size_t)x * (size_t)bytes_per_pixel;
            uint8_t *target = destination_row + (size_t)x * 4;
            if (stream->format == SPA_VIDEO_FORMAT_RGBx ||
                stream->format == SPA_VIDEO_FORMAT_RGBA ||
                stream->format == SPA_VIDEO_FORMAT_RGB) {
                target[0] = source[2];
                target[1] = source[1];
                target[2] = source[0];
            } else {
                target[0] = source[0];
                target[1] = source[1];
                target[2] = source[2];
            }
            target[3] = 0;
        }
    }
    atomic_store_explicit(&slot->seq, 2 * stream->counter,
                          memory_order_release);
    struct frame_header *header = (struct frame_header *)stream->shm;
    atomic_store_explicit(&header->total_frames, stream->counter,
                          memory_order_release);
}

static void on_process(void *userdata)
{
    struct capture_stream *stream = userdata;
    struct pw_buffer *buffer = pw_stream_dequeue_buffer(stream->stream);
    if (buffer == NULL)
        return;
    /* Consume the newest available image. Old queued images increase visible
     * cursor latency without helping UU's latest-frame capture consumer. */
    struct pw_buffer *newer;
    for (unsigned int queued = 0; queued < 32 &&
         (newer = pw_stream_dequeue_buffer(stream->stream)) != NULL; ++queued) {
        pw_stream_queue_buffer(stream->stream, buffer);
        buffer = newer;
    }
    struct spa_buffer *buf = buffer->buffer;

    if (buf->n_datas >= 1 && stream->shm != NULL) {
        struct spa_data *plane = &buf->datas[0];
        uint8_t *pixels = NULL;
        void *mapping = MAP_FAILED;
        size_t mapping_size = 0;
        if (plane->chunk == NULL) {
            pw_stream_queue_buffer(stream->stream, buffer);
            return;
        }
        if (plane->data != NULL) {
            pixels = plane->data + plane->chunk->offset;
        } else if (plane->type == SPA_DATA_MemFd && plane->fd >= 0) {
            long page_size = sysconf(_SC_PAGESIZE);
            uint64_t offset = (uint64_t)plane->mapoffset + plane->chunk->offset;
            uint64_t page_mask =
                (uint64_t)(page_size > 0 ? page_size : 4096) - 1;
            off_t aligned = (off_t)(offset & ~page_mask);
            size_t delta = (size_t)(offset - (uint64_t)aligned);
            mapping_size = delta + plane->chunk->size;
            mapping = mmap(NULL, mapping_size, PROT_READ, MAP_PRIVATE,
                           plane->fd, aligned);
            if (mapping != MAP_FAILED)
                pixels = (uint8_t *)mapping + delta;
        }
        if (pixels != NULL) {
            int32_t signed_stride = plane->chunk->stride;
            uint32_t stride = signed_stride > 0
                                  ? (uint32_t)signed_stride
                                  : stream->width * (uint32_t)
                                        format_bytes_per_pixel(stream->format);
            uint32_t rows = stride > 0 ? plane->chunk->size / stride : 0;
            publish_frame(stream, pixels, stride, rows);
        }
        if (mapping != MAP_FAILED)
            munmap(mapping, mapping_size);
    }
    pw_stream_queue_buffer(stream->stream, buffer);
}

static int supported_format(enum spa_video_format format)
{
    return format == SPA_VIDEO_FORMAT_BGRx ||
           format == SPA_VIDEO_FORMAT_BGRA ||
           format == SPA_VIDEO_FORMAT_RGBx ||
           format == SPA_VIDEO_FORMAT_RGBA ||
           format == SPA_VIDEO_FORMAT_BGR ||
           format == SPA_VIDEO_FORMAT_RGB;
}

static void on_param_changed(void *userdata, uint32_t id,
                             const struct spa_pod *param)
{
    struct capture_stream *stream = userdata;
    struct capture *capture = stream->capture;
    if (param == NULL || id != SPA_PARAM_Format)
        return;

    struct spa_video_info_raw info;
    if (spa_format_video_raw_parse(param, &info) < 0)
        return;
    if (!supported_format(info.format)) {
        fprintf(stderr, "node %u negotiated unexpected format %d\n",
                stream->node_id, info.format);
        return;
    }
    uint32_t width = (uint32_t)info.size.width;
    uint32_t height = (uint32_t)info.size.height;
    int geometry_changed = capture->transports_ready &&
        (stream->width != width || stream->height != height ||
         stream->format != info.format);
    if (geometry_changed) {
        fprintf(stderr,
                "node %u format changed from %ux%u/%d to %ux%u/%d; rebuilding transports\n",
                stream->node_id, stream->width, stream->height, stream->format,
                width, height, info.format);
        transports_reset(capture);
    }
    stream->width = width;
    stream->height = height;
    stream->format = info.format;
    stream->ready = stream->width > 0 && stream->height > 0 &&
                    stream->width <= UINT32_MAX / 4;
    stream->stride = stream->ready ? stream->width * 4 : 0;
    fprintf(stderr, "node %u negotiated: %ux%u\n", stream->node_id,
            stream->width, stream->height);
    int result = transports_setup(capture);
    if (result < 0) {
        capture->failed = 1;
        pw_main_loop_quit(capture->loop);
    }
}

static void on_state_changed(void *userdata, enum pw_stream_state old,
                             enum pw_stream_state state, const char *error)
{
    struct capture_stream *stream = userdata;
    (void)old;
    fprintf(stderr, "node %u state: %s%s%s\n", stream->node_id,
            pw_stream_state_as_string(state),
            error != NULL && error[0] != '\0' ? " - " : "",
            error != NULL ? error : "");
    if (state == PW_STREAM_STATE_ERROR) {
        stream->capture->failed = 1;
        pw_main_loop_quit(stream->capture->loop);
    }
}

static const struct pw_stream_events stream_events = {
    .version = PW_VERSION_STREAM_EVENTS,
    .process = on_process,
    .param_changed = on_param_changed,
    .state_changed = on_state_changed,
};

static int connect_stream(struct capture_stream *stream, struct pw_core *core)
{
    struct pw_properties *properties = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen", NULL);
    char name[64];
    snprintf(name, sizeof(name), "uur-capture-%u", stream->node_id);
    stream->stream = pw_stream_new(core, name, properties);
    if (stream->stream == NULL)
        return -1;
    pw_stream_add_listener(stream->stream, &stream->listener, &stream_events,
                           stream);

    uint8_t pod_buffer[1024];
    struct spa_pod_builder builder =
        SPA_POD_BUILDER_INIT(pod_buffer, sizeof(pod_buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(
        &builder, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format,
        SPA_POD_CHOICE_ENUM_Id(6, SPA_VIDEO_FORMAT_BGRx,
                               SPA_VIDEO_FORMAT_BGRA,
                               SPA_VIDEO_FORMAT_RGBx,
                               SPA_VIDEO_FORMAT_RGBA,
                               SPA_VIDEO_FORMAT_BGR,
                               SPA_VIDEO_FORMAT_RGB));
    return pw_stream_connect(stream->stream, PW_DIRECTION_INPUT,
                             stream->node_id,
                             PW_STREAM_FLAG_MAP_BUFFERS |
                                 PW_STREAM_FLAG_AUTOCONNECT,
                             params, 1);
}

static void cleanup_capture(struct capture *capture)
{
    for (size_t i = 0; i < capture->stream_count; ++i) {
        struct capture_stream *stream = &capture->streams[i];
        if (stream->stream != NULL)
            pw_stream_destroy(stream->stream);
        if (stream->shm != NULL)
            munmap(stream->shm, stream->shm_size);
        if (stream->shm_fd >= 0)
            close(stream->shm_fd);
        if (stream->frame_path[0] != '\0')
            unlink(stream->frame_path);
    }
}

int main(void)
{
    /* A killed supervisor cannot run Child cleanup. Stop its private capture
     * helper as well, so an orphan never keeps a stale frame's heartbeat live. */
    pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) < 0) {
        perror("PR_SET_PDEATHSIG");
        return 1;
    }
    if (getppid() != parent)
        return 1;
    const char *fd_text = getenv("UUR_PW_FD");
    const char *streams_text = getenv("UUR_PW_STREAMS");
    const char *frame_path = getenv("UUR_FRAME_PATH");
    if (fd_text == NULL || streams_text == NULL) {
        fprintf(stderr,
                "UUR_PW_FD and UUR_PW_STREAMS must be set by 'uur capture'\n");
        return 2;
    }
    if (frame_path == NULL || frame_path[0] != '/' ||
        strlen(frame_path) >= PATH_MAX) {
        fprintf(stderr, "UUR_FRAME_PATH must be a short absolute path\n");
        return 2;
    }

    struct capture capture;
    memset(&capture, 0, sizeof(capture));
    memcpy(capture.frame_path, frame_path, strlen(frame_path) + 1);
    if (parse_streams(&capture, streams_text) < 0)
        return 2;
    for (size_t i = 0; i < capture.stream_count; ++i) {
        if (stream_path(capture.frame_path, i, capture.streams[i].frame_path,
                        sizeof(capture.streams[i].frame_path)) < 0) {
            fprintf(stderr, "capture frame path is too long\n");
            return 2;
        }
        unlink(capture.streams[i].frame_path);
    }

    int pw_fd = atoi(fd_text);
    pw_init(NULL, NULL);
    active_capture = &capture;
    signal(SIGINT, stop_capture);
    signal(SIGTERM, stop_capture);
    signal(SIGHUP, stop_capture);

    capture.loop = pw_main_loop_new(NULL);
    if (capture.loop == NULL) {
        fprintf(stderr, "pw_main_loop_new failed\n");
        return 1;
    }
    struct pw_context *context = pw_context_new(
        pw_main_loop_get_loop(capture.loop), NULL, 0);
    if (context == NULL) {
        fprintf(stderr, "pw_context_new failed\n");
        return 1;
    }
    struct pw_core *core = pw_context_connect_fd(context, pw_fd, NULL, 0);
    if (core == NULL) {
        fprintf(stderr, "pw_context_connect_fd failed\n");
        return 1;
    }
    for (size_t i = 0; i < capture.stream_count; ++i) {
        if (connect_stream(&capture.streams[i], core) < 0) {
            fprintf(stderr, "pw_stream_connect failed for node %u\n",
                    capture.streams[i].node_id);
            cleanup_capture(&capture);
            return 1;
        }
    }

    struct pw_loop *loop = pw_main_loop_get_loop(capture.loop);
    capture.heartbeat_timer = pw_loop_add_timer(loop, heartbeat_tick, &capture);
    if (capture.heartbeat_timer == NULL) {
        cleanup_capture(&capture);
        return 1;
    }
    struct timespec interval = {0, 250000000};
    pw_loop_update_timer(loop, capture.heartbeat_timer, &interval, &interval,
                          false);

    pw_main_loop_run(capture.loop);
    active_capture = NULL;
    pw_loop_destroy_source(loop, capture.heartbeat_timer);
    cleanup_capture(&capture);
    pw_context_destroy(context);
    pw_main_loop_destroy(capture.loop);
    pw_deinit();
    return capture.failed ? 1 : 0;
}
