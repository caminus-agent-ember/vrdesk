// Minimal screencopy test: capture one output once, report what events arrive.
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client.h"

static struct zwlr_screencopy_manager_v1 *sc;
static struct wl_output *out;
static int got_buffer = 0, got_ready = 0, got_failed = 0;
static int W = 0, H = 0, S = 0;
static void *data = NULL;
static size_t dsize = 0;

static struct wl_shm *shm;
static struct wl_buffer *buf;

static void on_buffer(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt,
                      uint32_t w, uint32_t h, uint32_t stride) {
    (void)d;
    W = (int)w; H = (int)h; S = (int)stride;
    got_buffer = 1;
    printf("buffer event: %ux%u stride=%u fmt=0x%x\n", w, h, stride, fmt);
    if (!shm) { printf("no wl_shm!\n"); return; }
    size_t size = (size_t)stride * h;
    char name[64];
    snprintf(name, sizeof name, "/sctest-%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) { printf("shm_open failed: %s\n", strerror(errno)); return; }
    shm_unlink(name);
    if (ftruncate(fd, (off_t)size) < 0) { printf("ftruncate failed\n"); close(fd); return; }
    data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { printf("mmap failed\n"); close(fd); return; }
    dsize = size;
    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    buf = wl_shm_pool_create_buffer(pool, 0, (int32_t)w, (int32_t)h, (int32_t)stride, fmt);
    wl_shm_pool_destroy(pool);
    close(fd);
    // This is the step the bridge was missing: hand the buffer to the frame.
    zwlr_screencopy_frame_v1_copy(f, buf);
    printf("attached shm buffer (%zu bytes)\n", size);
}
static void on_flags(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fl) {
    (void)d; (void)f; (void)fl;
}
static void on_ready(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t a, uint32_t b, uint32_t c) {
    (void)d; (void)f; (void)a; (void)b; (void)c;
    got_ready = 1;
    printf("ready event\n");
}
static void on_failed(void *d, struct zwlr_screencopy_frame_v1 *f) {
    (void)d; (void)f;
    got_failed = 1;
    printf("failed event\n");
}
static void on_dmabuf(void *d, struct zwlr_screencopy_frame_v1 *f, uint32_t fmt, uint32_t w, uint32_t h) {
    (void)d; (void)f; (void)fmt; (void)w; (void)h;
}
static void on_buffer_done(void *d, struct zwlr_screencopy_frame_v1 *f) { (void)d; (void)f; }

static const struct zwlr_screencopy_frame_v1_listener L = {
    .buffer = on_buffer, .flags = on_flags, .ready = on_ready, .failed = on_failed,
    .linux_dmabuf = on_dmabuf, .buffer_done = on_buffer_done,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name, const char *iface, uint32_t ver) {
    (void)d;
    if (!strcmp(iface, wl_shm_interface.name) && !shm)
        shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (!strcmp(iface, wl_output_interface.name) && !out)
        out = wl_registry_bind(r, name, &wl_output_interface, ver >= 4 ? 4 : ver);
    else if (!strcmp(iface, zwlr_screencopy_manager_v1_interface.name))
        sc = wl_registry_bind(r, name, &zwlr_screencopy_manager_v1_interface, 3);
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t n) { (void)d; (void)r; (void)n; }
static const struct wl_registry_listener RL = { .global = reg_global, .global_remove = reg_remove };

int main(void) {
    struct wl_display *dpy = wl_display_connect(NULL);
    if (!dpy) { printf("no display\n"); return 1; }
    struct wl_registry *reg = wl_display_get_registry(dpy);
    wl_registry_add_listener(reg, &RL, NULL);
    wl_display_roundtrip(dpy);
    wl_display_roundtrip(dpy);
    if (!sc || !out) { printf("missing sc=%p out=%p\n", (void*)sc, (void*)out); return 1; }

    struct zwlr_screencopy_frame_v1 *fr = zwlr_screencopy_manager_v1_capture_output(sc, 0, out);
    zwlr_screencopy_frame_v1_add_listener(fr, &L, NULL);

    // Plain blocking roundtrip: the compositor must answer with buffer + ready.
    for (int i = 0; i < 5 && !got_ready && !got_failed; i++) {
        if (wl_display_roundtrip(dpy) < 0) { printf("roundtrip failed\n"); break; }
        printf("roundtrip %d: buffer=%d ready=%d failed=%d\n", i, got_buffer, got_ready, got_failed);
    }
    printf("RESULT buffer=%d ready=%d failed=%d %dx%d stride=%d\n", got_buffer, got_ready, got_failed, W, H, S);
    return (got_ready ? 0 : 1);
}
