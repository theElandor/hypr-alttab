// hypr-alttab-capture: grab downscaled snapshots of Hyprland windows.
//
// Usage: hypr-alttab-capture <max-dim> <window-address>...
//
// Uses the hyprland-toplevel-export-v1 protocol, which can capture windows
// regardless of the workspace they're on. For every window that was captured
// successfully, writes a record to stdout:
//
//   uint64 address | uint32 width | uint32 height | width*height*4 bytes
//
// Pixels are native-endian premultiplied ARGB32 (cairo FORMAT_ARGB32), rows
// tightly packed. Windows that fail to capture are simply omitted.

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "hyprland-toplevel-export-v1-client-protocol.h"

#define TIMEOUT_MS 1500

// Referenced by capture_toplevel_with_wlr_toplevel_handle, which we never use.
const struct wl_interface zwlr_foreign_toplevel_handle_v1_interface = {
    "zwlr_foreign_toplevel_handle_v1", 3, 0, NULL, 0, NULL,
};

struct capture {
    uint64_t address;
    struct hyprland_toplevel_export_frame_v1 *frame;
    struct wl_buffer *buffer;
    void *data;
    size_t size;
    uint32_t format, width, height, stride;
    bool have_shm_format;
    bool y_invert;
    bool done;
    bool ok;
};

static struct wl_shm *shm;
static struct hyprland_toplevel_export_manager_v1 *manager;
static uint32_t manager_version;
static struct capture *captures;
static int ncaptures;
static int remaining;
static int max_dim;

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version) {
    (void)data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, hyprland_toplevel_export_manager_v1_interface.name) == 0) {
        manager_version = version < 2 ? version : 2;
        manager = wl_registry_bind(registry, name, &hyprland_toplevel_export_manager_v1_interface,
                                   manager_version);
    }
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name) {
    (void)data; (void)registry; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

static void finish(struct capture *c, bool ok) {
    if (c->done)
        return;
    c->done = true;
    c->ok = ok;
    remaining--;
}

static bool supported_format(uint32_t format) {
    return format == WL_SHM_FORMAT_ARGB8888 || format == WL_SHM_FORMAT_XRGB8888 ||
           format == WL_SHM_FORMAT_ABGR8888 || format == WL_SHM_FORMAT_XBGR8888;
}

static void start_copy(struct capture *c) {
    if (!c->have_shm_format) {
        finish(c, false);
        return;
    }
    c->size = (size_t)c->stride * c->height;
    int fd = memfd_create("hypr-alttab-capture", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)c->size) < 0) {
        if (fd >= 0)
            close(fd);
        finish(c, false);
        return;
    }
    c->data = mmap(NULL, c->size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (c->data == MAP_FAILED) {
        c->data = NULL;
        close(fd);
        finish(c, false);
        return;
    }
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)c->size);
    c->buffer = wl_shm_pool_create_buffer(pool, 0, (int32_t)c->width, (int32_t)c->height,
                                          (int32_t)c->stride, c->format);
    wl_shm_pool_destroy(pool);
    close(fd);
    hyprland_toplevel_export_frame_v1_copy(c->frame, c->buffer, 1);
}

static void frame_buffer(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    (void)frame;
    struct capture *c = data;
    if (c->have_shm_format || !supported_format(format))
        return;
    c->have_shm_format = true;
    c->format = format;
    c->width = width;
    c->height = height;
    c->stride = stride;
    // Version 1 has no buffer_done event: the buffer event is all we get.
    if (manager_version < 2)
        start_copy(c);
}

static void frame_damage(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
                         uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    (void)data; (void)frame; (void)x; (void)y; (void)w; (void)h;
}

static void frame_flags(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
                        uint32_t flags) {
    (void)frame;
    struct capture *c = data;
    c->y_invert = flags & HYPRLAND_TOPLEVEL_EXPORT_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_ready(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    (void)frame; (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    finish(data, true);
}

static void frame_failed(void *data, struct hyprland_toplevel_export_frame_v1 *frame) {
    (void)frame;
    finish(data, false);
}

static void frame_linux_dmabuf(void *data, struct hyprland_toplevel_export_frame_v1 *frame,
                               uint32_t format, uint32_t width, uint32_t height) {
    (void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_buffer_done(void *data, struct hyprland_toplevel_export_frame_v1 *frame) {
    (void)frame;
    start_copy(data);
}

static const struct hyprland_toplevel_export_frame_v1_listener frame_listener = {
    .buffer = frame_buffer,
    .damage = frame_damage,
    .flags = frame_flags,
    .ready = frame_ready,
    .failed = frame_failed,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

static bool write_all(const void *buf, size_t len) {
    const char *p = buf;
    while (len > 0) {
        ssize_t n = write(STDOUT_FILENO, p, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        p += n;
        len -= (size_t)n;
    }
    return true;
}

// Box-filter downscale into a tightly packed premultiplied ARGB32 image and
// write it out.
static bool emit(struct capture *c) {
    double scale = 1.0;
    uint32_t longest = c->width > c->height ? c->width : c->height;
    if (max_dim > 0 && longest > (uint32_t)max_dim)
        scale = (double)max_dim / longest;
    uint32_t ow = (uint32_t)(c->width * scale + 0.5), oh = (uint32_t)(c->height * scale + 0.5);
    if (ow == 0) ow = 1;
    if (oh == 0) oh = 1;

    bool swap_rb = c->format == WL_SHM_FORMAT_ABGR8888 || c->format == WL_SHM_FORMAT_XBGR8888;
    bool opaque = c->format == WL_SHM_FORMAT_XRGB8888 || c->format == WL_SHM_FORMAT_XBGR8888;

    uint32_t *out = malloc((size_t)ow * oh * 4);
    if (!out)
        return false;

    for (uint32_t oy = 0; oy < oh; oy++) {
        uint32_t sy0 = (uint64_t)oy * c->height / oh;
        uint32_t sy1 = (uint64_t)(oy + 1) * c->height / oh;
        if (sy1 <= sy0) sy1 = sy0 + 1;
        for (uint32_t ox = 0; ox < ow; ox++) {
            uint32_t sx0 = (uint64_t)ox * c->width / ow;
            uint32_t sx1 = (uint64_t)(ox + 1) * c->width / ow;
            if (sx1 <= sx0) sx1 = sx0 + 1;
            uint64_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t sy = sy0; sy < sy1; sy++) {
                uint32_t row = c->y_invert ? c->height - 1 - sy : sy;
                const uint32_t *src = (const uint32_t *)((const uint8_t *)c->data + (size_t)row * c->stride);
                for (uint32_t sx = sx0; sx < sx1; sx++) {
                    uint32_t px = src[sx];
                    sum[0] += (px >> 24) & 0xff;
                    sum[1] += (px >> 16) & 0xff;
                    sum[2] += (px >> 8) & 0xff;
                    sum[3] += px & 0xff;
                    n++;
                }
            }
            uint32_t a = opaque ? 0xff : (uint32_t)(sum[0] / n);
            uint32_t hi = (uint32_t)(sum[1] / n), lo = (uint32_t)(sum[3] / n);
            uint32_t g = (uint32_t)(sum[2] / n);
            uint32_t r = swap_rb ? lo : hi, b = swap_rb ? hi : lo;
            out[(size_t)oy * ow + ox] = (a << 24) | (r << 16) | (g << 8) | b;
        }
    }

    uint32_t dims[2] = {ow, oh};
    bool ok = write_all(&c->address, sizeof c->address) && write_all(dims, sizeof dims) &&
              write_all(out, (size_t)ow * oh * 4);
    free(out);
    return ok;
}

static int64_t now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <max-dim> <window-address>...\n", argv[0]);
        return 2;
    }
    max_dim = atoi(argv[1]);

    struct wl_display *display = wl_display_connect(NULL);
    if (!display) {
        fprintf(stderr, "hypr-alttab-capture: cannot connect to wayland display\n");
        return 1;
    }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!shm || !manager) {
        fprintf(stderr, "hypr-alttab-capture: compositor lacks wl_shm or hyprland_toplevel_export_manager_v1\n");
        return 1;
    }

    ncaptures = argc - 2;
    captures = calloc((size_t)ncaptures, sizeof *captures);
    for (int i = 0; i < ncaptures; i++) {
        struct capture *c = &captures[i];
        c->address = strtoull(argv[i + 2], NULL, 16);
        c->frame = hyprland_toplevel_export_manager_v1_capture_toplevel(
            manager, 0, (uint32_t)(c->address & 0xffffffff));
        hyprland_toplevel_export_frame_v1_add_listener(c->frame, &frame_listener, c);
        remaining++;
    }

    int64_t deadline = now_ms() + TIMEOUT_MS;
    while (remaining > 0) {
        while (wl_display_prepare_read(display) != 0)
            wl_display_dispatch_pending(display);
        wl_display_flush(display);
        int64_t left = deadline - now_ms();
        if (left <= 0) {
            wl_display_cancel_read(display);
            break;
        }
        struct pollfd pfd = {.fd = wl_display_get_fd(display), .events = POLLIN};
        int r = poll(&pfd, 1, (int)left);
        if (r <= 0) {
            wl_display_cancel_read(display);
            if (r < 0 && errno == EINTR)
                continue;
            break;
        }
        if (wl_display_read_events(display) < 0)
            break;
        if (wl_display_dispatch_pending(display) < 0)
            break;
    }

    int status = 0;
    for (int i = 0; i < ncaptures; i++) {
        struct capture *c = &captures[i];
        if (c->ok && c->data && !emit(c))
            status = 1;
        if (c->data)
            munmap(c->data, c->size);
        if (c->buffer)
            wl_buffer_destroy(c->buffer);
        hyprland_toplevel_export_frame_v1_destroy(c->frame);
    }
    hyprland_toplevel_export_manager_v1_destroy(manager);
    wl_display_flush(display);
    wl_display_disconnect(display);
    return status;
}
