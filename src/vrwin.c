/* vrwin.c — a tiny Wayland client that maps a solid-colour window and logs every
 * pointer event it receives. Used by run_e2e.sh to prove that a click injected by
 * the bridge actually reaches a window surface (sway's headless seat has no
 * keyboard capability, so keyboard focus is not a valid assertion).
 *
 * usage: vrwin <event-log-path>
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#define WIDTH 400
#define HEIGHT 300

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_surface *surface;
static struct wl_buffer *buffer;
static FILE *evlog;

static void logev(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(evlog, fmt, ap);
    fputc('\n', evlog);
    fflush(evlog);
    va_end(ap);
}

static int create_shm_file(size_t size) {
    char tmpl[] = "/tmp/vrwin-XXXXXX";
    int fd = mkstemp(tmpl);
    if (fd < 0) return -1;
    unlink(tmpl);
    if (ftruncate(fd, (off_t)size) < 0) { close(fd); return -1; }
    return fd;
}

/* ---- pointer ---- */
static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
                      struct wl_surface *s, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)serial; (void)s;
    logev("enter %d,%d", (int)wl_fixed_to_double(sx), (int)wl_fixed_to_double(sy));
}
static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s) {
    (void)d; (void)p; (void)serial; (void)s;
    logev("leave");
}
static void ptr_motion(void *d, struct wl_pointer *p, uint32_t t, wl_fixed_t sx, wl_fixed_t sy) {
    (void)d; (void)p; (void)t;
    logev("motion %d,%d", (int)wl_fixed_to_double(sx), (int)wl_fixed_to_double(sy));
}
static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t t,
                       uint32_t button, uint32_t state) {
    (void)d; (void)p; (void)serial; (void)t;
    logev("button %u %s", button, state == WL_POINTER_BUTTON_STATE_PRESSED ? "press" : "release");
}
static void ptr_axis(void *d, struct wl_pointer *p, uint32_t t, uint32_t axis, wl_fixed_t v) {
    (void)d; (void)p; (void)t; (void)axis; (void)v;
}
static void ptr_frame(void *d, struct wl_pointer *p) { (void)d; (void)p; }
static void ptr_axis_source(void *d, struct wl_pointer *p, uint32_t s) { (void)d; (void)p; (void)s; }
static void ptr_axis_stop(void *d, struct wl_pointer *p, uint32_t t, uint32_t a) { (void)d;(void)p;(void)t;(void)a; }
static void ptr_axis_discrete(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }
static void ptr_axis_value120(void *d, struct wl_pointer *p, uint32_t a, int32_t v) { (void)d;(void)p;(void)a;(void)v; }
static void ptr_axis_dir(void *d, struct wl_pointer *p, uint32_t a, uint32_t dir) { (void)d;(void)p;(void)a;(void)dir; }

static const struct wl_pointer_listener pointer_listener = {
    .enter = ptr_enter, .leave = ptr_leave, .motion = ptr_motion, .button = ptr_button,
    .axis = ptr_axis, .frame = ptr_frame, .axis_source = ptr_axis_source,
    .axis_stop = ptr_axis_stop, .axis_discrete = ptr_axis_discrete,
    .axis_value120 = ptr_axis_value120, .axis_relative_direction = ptr_axis_dir,
};

/* ---- seat ---- */
static void seat_caps(void *d, struct wl_seat *s, uint32_t caps) {
    (void)d;
    logev("seat caps=%u", caps);
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
        pointer = wl_seat_get_pointer(s);
        wl_pointer_add_listener(pointer, &pointer_listener, NULL);
        wl_display_flush(display);
    }
}
static void seat_name(void *d, struct wl_seat *s, const char *name) { (void)d;(void)s;(void)name; }

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_caps, .name = seat_name,
};

/* ---- registry ---- */
static void reg_global(void *d, struct wl_registry *reg, uint32_t name,
                       const char *iface, uint32_t version) {
    (void)d;
    if (!strcmp(iface, wl_compositor_interface.name)) {
        compositor = wl_registry_bind(reg, name, &wl_compositor_interface, version < 4 ? version : 4);
    } else if (!strcmp(iface, wl_shm_interface.name)) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        seat = wl_registry_bind(reg, name, &wl_seat_interface, version < 5 ? version : 5);
        wl_seat_add_listener(seat, &seat_listener, NULL);
    }
}
static void reg_remove(void *d, struct wl_registry *reg, uint32_t name) { (void)d;(void)reg;(void)name; }

static const struct wl_registry_listener registry_listener = {
    .global = reg_global, .global_remove = reg_remove,
};

int main(int argc, char **argv) {
    const char *logpath = argc > 1 ? argv[1] : "/tmp/vrwin-events.log";
    evlog = fopen(logpath, "w");
    if (!evlog) { perror("fopen log"); return 1; }

    display = wl_display_connect(NULL);
    if (!display) { logev("cannot connect to Wayland display"); return 1; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);
    if (!compositor || !shm) { logev("missing wl_compositor or wl_shm"); return 1; }

    int stride = WIDTH * 4;
    size_t size = (size_t)stride * HEIGHT;
    int fd = create_shm_file(size);
    if (fd < 0) { logev("shm file failed"); return 1; }
    uint32_t *pix = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (pix == MAP_FAILED) { logev("mmap failed"); return 1; }
    for (size_t i = 0; i < size / 4; i++) pix[i] = 0xff403020u; /* XRGB8888 */

    struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, WIDTH, HEIGHT, stride, WL_SHM_FORMAT_XRGB8888);
    surface = wl_compositor_create_surface(compositor);
    wl_surface_attach(surface, buffer, 0, 0);
    wl_surface_damage_buffer(surface, 0, 0, WIDTH, HEIGHT);
    wl_surface_commit(surface);
    wl_display_flush(display);
    logev("READY");

    while (wl_display_dispatch(display) >= 0) { }
    logev("dispatch failed");
    return 0;
}
