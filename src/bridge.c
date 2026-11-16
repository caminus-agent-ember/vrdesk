// vrdesk-bridge: streams headless-sway outputs to the Quest and injects controller input.
//
// Wayland side (client of sway):
//   - wlr_screencopy_manager_v1 : capture each output into a shm buffer
//   - zwlr_virtual_pointer_manager_v1 : move/click/drag the pointer
//
// Network side (TCP, one client at a time = the headset):
//   server -> client : frame messages
//       u8  type = 1
//       u16 output_id
//       u32 width, u32 height, u32 stride
//       u32 payload_len (stride*height), then raw BGRA rows
//   client -> server : input messages
//       u8  type = 2 (pointer)
//       u16 output_id
//       f32 x, f32 y            (normalised 0..1 within that output)
//       u8  buttons             (bit0 = primary)
//       u8  action              (0 = move only, 1 = press, 2 = release)
//       u8  type = 3 (key)      u32 keysym, u8 pressed
//       u8  type = 4 (scroll)   u16 output_id, f32 dx, f32 dy
//
// Build: see build.sh. Run: vrdesk-bridge --port 5000 [--fps 30]

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/uio.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>
#include "wlr-screencopy-unstable-v1-client.h"
#include "wlr-virtual-pointer-unstable-v1-client.h"

#define MAX_OUTPUTS 8
#define MSG_FRAME 1
#define MSG_POINTER 2
#define MSG_KEY 3
#define MSG_SCROLL 4

struct output {
    struct wl_output *wl_output;
    struct zwlr_screencopy_frame_v1 *frame;
    int id;
    int gx, gy;         // global position of this output in the compositor layout
    char name[64];      // wl_output name, e.g. HEADLESS-1
    int width, height, stride, format;
    int ready;          // a captured frame is waiting to be sent
    int capturing;      // a capture is in flight
    void *shm_data;
    size_t shm_size;
    struct wl_buffer *buffer;
    struct wl_shm_pool *pool;
    int shm_fd;
};

static struct wl_display *display;
static struct wl_registry *registry;
static struct wl_shm *shm;
static struct zwlr_screencopy_manager_v1 *screencopy;
static struct zwlr_virtual_pointer_manager_v1 *vpointer_mgr;
static struct zwlr_virtual_pointer_v1 *vpointer;

static struct output outputs[MAX_OUTPUTS];
static int n_outputs = 0;
static long stat_captures = 0, stat_ready = 0, stat_failed = 0, stat_sent = 0;
static int running = 1;
static int client_fd = -1;
static int listen_fd = -1;
static int target_fps = 30;
static int verbose = 0;

static void logmsg(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[bridge] ");
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
    va_end(ap);
}

static int create_shm_file(size_t size) {
    char name[64];
    snprintf(name, sizeof name, "/vrdesk-shm-%d", getpid());
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    shm_unlink(name);
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}

static void output_free_buffer(struct output *o) {
    if (o->buffer) { wl_buffer_destroy(o->buffer); o->buffer = NULL; }
    if (o->pool) { wl_shm_pool_destroy(o->pool); o->pool = NULL; }
    if (o->shm_data) { munmap(o->shm_data, o->shm_size); o->shm_data = NULL; }
    if (o->shm_fd >= 0) { close(o->shm_fd); o->shm_fd = -1; }
}

static void frame_buffer(void *data, struct zwlr_screencopy_frame_v1 *frame,
                         uint32_t format, uint32_t width, uint32_t height, uint32_t stride) {
    struct output *o = data;
    o->format = format; o->width = width; o->height = height; o->stride = stride;
    size_t size = (size_t)stride * height;
    if (!o->shm_data || o->shm_size < size) {
        output_free_buffer(o);
        o->shm_fd = create_shm_file(size);
        if (o->shm_fd < 0) { logmsg("shm create failed: %s", strerror(errno)); return; }
        o->shm_data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, o->shm_fd, 0);
        if (o->shm_data == MAP_FAILED) { o->shm_data = NULL; return; }
        o->shm_size = size;
        o->pool = wl_shm_create_pool(shm, o->shm_fd, (int32_t)size);
    }
    if (!o->buffer)
        o->buffer = wl_shm_pool_create_buffer(o->pool, 0, (int32_t)width, (int32_t)height,
                                              (int32_t)stride, format);
    // THE MISSING STEP: hand the buffer to the frame, or the compositor never sends `ready`.
    zwlr_screencopy_frame_v1_copy(frame, o->buffer);
}

static void frame_flags(void *data, struct zwlr_screencopy_frame_v1 *frame, uint32_t flags) {
    (void)data; (void)frame; (void)flags;
}

static void frame_ready(void *data, struct zwlr_screencopy_frame_v1 *frame,
                        uint32_t tv_sec_hi, uint32_t tv_sec_lo, uint32_t tv_nsec) {
    (void)tv_sec_hi; (void)tv_sec_lo; (void)tv_nsec;
    struct output *o = data;
    zwlr_screencopy_frame_v1_destroy(frame);
    o->frame = NULL;
    o->capturing = 0;
    o->ready = 1;
    stat_ready++;
}

static void frame_failed(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    struct output *o = data;
    zwlr_screencopy_frame_v1_destroy(frame);
    o->frame = NULL;
    o->capturing = 0;
    if (verbose) logmsg("capture failed on output %d", o->id);
}

// v3 adds linux_dmabuf; we only use shm, but the listener must not be NULL.
static void frame_linux_dmabuf(void *data, struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t format, uint32_t width, uint32_t height) {
    (void)data; (void)frame; (void)format; (void)width; (void)height;
}

static void frame_buffer_done(void *data, struct zwlr_screencopy_frame_v1 *frame) {
    (void)data; (void)frame;
}

static const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_buffer, .flags = frame_flags,
    .ready = frame_ready, .failed = frame_failed,
    .linux_dmabuf = frame_linux_dmabuf,
    .buffer_done = frame_buffer_done,
};

static void start_capture(struct output *o) {
    if (!screencopy || !o->wl_output || o->capturing || !shm) return;
    o->frame = zwlr_screencopy_manager_v1_capture_output(screencopy, 0, o->wl_output);
    zwlr_screencopy_frame_v1_add_listener(o->frame, &frame_listener, o);
    o->capturing = 1;
    stat_captures++;
}

static void output_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
                            int32_t pw, int32_t ph, int32_t subpixel,
                            const char *make, const char *model, int32_t transform) {
    struct output *out = d;
    (void)o;(void)pw;(void)ph;(void)subpixel;(void)make;(void)model;(void)transform;
    out->gx = x; out->gy = y;
    if (verbose) logmsg("output %d geometry at %d,%d", out->id, x, y);
}
static void output_mode(void *d, struct wl_output *o, uint32_t flags, int32_t w, int32_t h, int32_t refresh) {
    (void)d;(void)o;(void)flags;(void)w;(void)h;(void)refresh;
}
static void output_done(void *d, struct wl_output *o) { (void)d;(void)o; }
static void output_scale(void *d, struct wl_output *o, int32_t f) { (void)d;(void)o;(void)f; }
static void output_name(void *d, struct wl_output *o, const char *name) {
    struct output *out = d;
    snprintf(out->name, sizeof out->name, "%s", name);
    if (verbose) logmsg("output %d name=%s", out->id, name);
}

// wl_output.geometry reports 0,0 for every headless output, so the real layout
// must come from sway's IPC. Ask sway for output rects and store them per name.
static void query_sway_outputs(void) {
    static char sockbuf[256];
    const char *sock = getenv("SWAYSOCK");
    if (!sock || !*sock) {
        // fall back to the well-known socket this project's start-sway.sh creates
        const char *xdg = getenv("XDG_RUNTIME_DIR");
        if (!xdg) xdg = "/run/user/1002";
        snprintf(sockbuf, sizeof sockbuf, "%s/sway-ipc.%d.vrdesk.sock", xdg, (int)getuid());
        sock = sockbuf;
    }
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    snprintf(sa.sun_path, sizeof sa.sun_path, "%s", sock);
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        logmsg("sway IPC connect %s: %s", sock, strerror(errno));
        close(fd);
        return;
    }
    const char magic[] = "i3-ipc";
    uint32_t len = 0, type = 3; // GET_OUTPUTS
    struct iovec iov[3] = {
        { (void *)magic, 6 }, { &len, 4 }, { &type, 4 },
    };
    struct msghdr mh = { .msg_iov = iov, .msg_iovlen = 3 };
    if (sendmsg(fd, &mh, 0) < 0) { close(fd); return; }
    // read reply header
    char hdr[14];
    size_t got = 0;
    while (got < sizeof hdr) {
        ssize_t n = recv(fd, hdr + got, sizeof hdr - got, 0);
        if (n <= 0) { close(fd); return; }
        got += (size_t)n;
    }
    uint32_t rlen;
    memcpy(&rlen, hdr + 6, 4);
    if (rlen == 0 || rlen > (1u << 22)) { close(fd); return; }
    char *buf = malloc(rlen + 1);
    if (!buf) { close(fd); return; }
    got = 0;
    while (got < rlen) {
        ssize_t n = recv(fd, buf + got, rlen - got, 0);
        if (n <= 0) { free(buf); close(fd); return; }
        got += (size_t)n;
    }
    buf[rlen] = 0;
    close(fd);
    // minimal parse: find each {"name": "...", ... "rect": {"x": N, "y": N ...}}
    for (struct output *o = outputs; o < outputs + n_outputs; o++) {
        if (!o->name[0]) continue;
        char key[96];
        snprintf(key, sizeof key, "\"name\": \"%s\"", o->name);
        char *p = strstr(buf, key);
        if (!p) continue;
        // "rect" comes BEFORE "name" in sway's JSON, so search backwards from the name.
        char *r = NULL;
        for (char *q = p; q > buf; q--) {
            if (strncmp(q, "\"rect\": {", 9) == 0) { r = q; break; }
        }
        if (!r) continue;
        char *x = strstr(r, "\"x\": ");
        char *y = strstr(r, "\"y\": ");
        if (!x || !y) continue;
        o->gx = atoi(x + 5);
        o->gy = atoi(y + 5);
        if (verbose) logmsg("output %d %s sway rect %d,%d", o->id, o->name, o->gx, o->gy);
    }
    free(buf);
}
static void output_description(void *d, struct wl_output *o, const char *desc) {
    (void)d;(void)o;(void)desc;
}
static const struct wl_output_listener output_listener = {
    .geometry = output_geometry, .mode = output_mode, .done = output_done,
    .scale = output_scale, .name = output_name, .description = output_description,
};

static void registry_global(void *data, struct wl_registry *reg, uint32_t name,
                            const char *iface, uint32_t version) {
    (void)data;
    if (strcmp(iface, wl_shm_interface.name) == 0) {
        shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, wl_output_interface.name) == 0) {
        if (n_outputs >= MAX_OUTPUTS) return;
        struct output *o = &outputs[n_outputs];
        memset(o, 0, sizeof *o);
        o->id = n_outputs;
        o->shm_fd = -1;
        o->wl_output = wl_registry_bind(reg, name, &wl_output_interface, version >= 4 ? 4 : version);
        wl_output_add_listener(o->wl_output, &output_listener, o);
        n_outputs++;
    } else if (strcmp(iface, zwlr_screencopy_manager_v1_interface.name) == 0) {
        screencopy = wl_registry_bind(reg, name, &zwlr_screencopy_manager_v1_interface, 3);
    } else if (strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name) == 0) {
        vpointer_mgr = wl_registry_bind(reg, name, &zwlr_virtual_pointer_manager_v1_interface, 2);
    }
}

static void registry_global_remove(void *data, struct wl_registry *reg, uint32_t name) {
    (void)data; (void)reg; (void)name;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_global, .global_remove = registry_global_remove,
};

// ---- networking -------------------------------------------------------------

static int write_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    while (len) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int send_frame(struct output *o) {
    if (client_fd < 0 || !o->ready || !o->shm_data) return -1;
    uint8_t type = MSG_FRAME;
    uint16_t oid = (uint16_t)o->id;
    uint32_t w = (uint32_t)o->width, h = (uint32_t)o->height, s = (uint32_t)o->stride;
    uint32_t plen = s * h;
    if (write_all(client_fd, &type, 1) || write_all(client_fd, &oid, 2) ||
        write_all(client_fd, &w, 4) || write_all(client_fd, &h, 4) ||
        write_all(client_fd, &s, 4) || write_all(client_fd, &plen, 4) ||
        write_all(client_fd, o->shm_data, plen)) {
        return -1;
    }
    o->ready = 0;
    return 0;
}

static void handle_pointer(struct output *o, float nx, float ny, uint8_t buttons, uint8_t action) {
    if (!vpointer) return;
    // Map normalised coords into this output's pixels, then into the compositor's
    // global layout: zwlr_virtual_pointer_v1_motion_absolute is global, so add the
    // output's position as reported by wl_output.geometry.
    int px = (int)(nx * (float)o->width);
    int py = (int)(ny * (float)o->height);
    if (px < 0) px = 0;
    if (py < 0) py = 0;
    if (px >= o->width) px = o->width - 1;
    if (py >= o->height) py = o->height - 1;
    int gx = o->gx + px;
    int gy = o->gy + py;
    zwlr_virtual_pointer_v1_motion_absolute(vpointer, 0, (uint32_t)gx, (uint32_t)gy, o->width, o->height);
    zwlr_virtual_pointer_v1_frame(vpointer);
    if (action == 1) {
        zwlr_virtual_pointer_v1_button(vpointer, 0, 272, WL_POINTER_BUTTON_STATE_PRESSED);
        zwlr_virtual_pointer_v1_frame(vpointer);
    } else if (action == 2) {
        zwlr_virtual_pointer_v1_button(vpointer, 0, 272, WL_POINTER_BUTTON_STATE_RELEASED);
        zwlr_virtual_pointer_v1_frame(vpointer);
    }
    wl_display_flush(display);
    if (verbose) logmsg("pointer out=%d %d,%d action=%d", o->id, px, py, action);
}

static void handle_scroll(struct output *o, float dx, float dy) {
    (void)o;
    if (!vpointer) return;
    zwlr_virtual_pointer_v1_axis_source(vpointer, WL_POINTER_AXIS_SOURCE_WHEEL);
    if (dy != 0.0f)
        zwlr_virtual_pointer_v1_axis_discrete(vpointer, 0, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                              wl_fixed_from_double(dy), (int32_t)(dy > 0 ? 1 : -1));
    if (dx != 0.0f)
        zwlr_virtual_pointer_v1_axis_discrete(vpointer, 0, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                              wl_fixed_from_double(dx), (int32_t)(dx > 0 ? 1 : -1));
    zwlr_virtual_pointer_v1_frame(vpointer);
    wl_display_flush(display);
}

// Read exactly len bytes; returns 0 ok, -1 closed/error.
static int read_all(int fd, void *buf, size_t len) {
    char *p = buf;
    while (len) {
        ssize_t n = recv(fd, p, len, 0);
        if (n == 0) return -1;
        if (n < 0) { if (errno == EINTR) continue; return -1; }
        p += n; len -= (size_t)n;
    }
    return 0;
}

static int handle_client_input(void) {
    uint8_t type;
    if (read_all(client_fd, &type, 1) < 0) return -1;
    if (type == MSG_POINTER) {
        uint16_t oid; float x, y; uint8_t buttons, action;
        if (read_all(client_fd, &oid, 2) || read_all(client_fd, &x, 4) ||
            read_all(client_fd, &y, 4) || read_all(client_fd, &buttons, 1) ||
            read_all(client_fd, &action, 1)) return -1;
        if (oid < n_outputs) handle_pointer(&outputs[oid], x, y, buttons, action);
    } else if (type == MSG_SCROLL) {
        uint16_t oid; float dx, dy;
        if (read_all(client_fd, &oid, 2) || read_all(client_fd, &dx, 4) || read_all(client_fd, &dy, 4)) return -1;
        if (oid < n_outputs) handle_scroll(&outputs[oid], dx, dy);
    } else if (type == MSG_KEY) {
        uint32_t keysym; uint8_t pressed;
        if (read_all(client_fd, &keysym, 4) || read_all(client_fd, &pressed, 1)) return -1;
        // Keyboard injection needs virtual-keyboard protocol; not wired yet.
        if (verbose) logmsg("key 0x%x pressed=%d (ignored: no vkbd)", keysym, pressed);
    } else {
        logmsg("unknown message type %u", type);
        return -1;
    }
    return 0;
}

static void accept_client(void) {
    struct sockaddr_in addr; socklen_t len = sizeof addr;
    int fd = accept(listen_fd, (struct sockaddr *)&addr, &len);
    if (fd < 0) return;
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (client_fd >= 0) close(client_fd);
    client_fd = fd;
    logmsg("headset connected");
}

static void on_signal(int sig) { (void)sig; running = 0; }

static double now_sec(void) {
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int main(int argc, char **argv) {
    int port = 5000;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--port") && i + 1 < argc) port = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fps") && i + 1 < argc) target_fps = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-v")) verbose = 1;
        else { fprintf(stderr, "usage: %s [--port N] [--fps N] [-v]\n", argv[0]); return 2; }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    display = wl_display_connect(NULL);
    if (!display) { logmsg("cannot connect to Wayland display"); return 1; }
    registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    wl_display_roundtrip(display);

    if (!screencopy) { logmsg("compositor lacks wlr_screencopy_manager_v1"); return 1; }
    if (!vpointer_mgr) { logmsg("compositor lacks zwlr_virtual_pointer_manager_v1"); return 1; }
    vpointer = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(vpointer_mgr, NULL);
    query_sway_outputs();
    logmsg("connected: %d outputs, screencopy+vpointer ready", n_outputs);

    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa = { .sin_family = AF_INET, .sin_port = htons((uint16_t)port),
                              .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) < 0) {
        logmsg("bind %d: %s", port, strerror(errno)); return 1;
    }
    listen(listen_fd, 4);
    logmsg("listening on 0.0.0.0:%d", port);

    double next_frame = now_sec();
    double next_stats = next_frame + 2.0;
    double frame_interval = 1.0 / (double)(target_fps > 0 ? target_fps : 30);

    while (running) {
        // Correct non-blocking Wayland pattern: prepare_read, poll, then either
        // read_events (fd ready) or cancel_read (timeout), then dispatch_pending.
        wl_display_dispatch_pending(display);
        wl_display_flush(display);

        struct pollfd pfds[3];
        int nf = 0;
        pfds[nf].fd = wl_display_get_fd(display); pfds[nf].events = POLLIN; nf++;
        pfds[nf].fd = listen_fd; pfds[nf].events = POLLIN; nf++;
        int cin = -1;
        if (client_fd >= 0) { cin = nf; pfds[nf].fd = client_fd; pfds[nf].events = POLLIN; nf++; }

        int timeout_ms = (int)((next_frame - now_sec()) * 1000.0);
        if (timeout_ms < 0) timeout_ms = 0;
        if (timeout_ms > 50) timeout_ms = 50;

        if (wl_display_prepare_read(display) != 0) {
            // Events already queued; just dispatch them.
            wl_display_dispatch_pending(display);
        }
        int pr = poll(pfds, (nfds_t)nf, timeout_ms);
        if (pr < 0 && errno != EINTR) { wl_display_cancel_read(display); break; }
        if (pr > 0 && (pfds[0].revents & POLLIN)) {
            if (wl_display_read_events(display) < 0) { logmsg("display read failed"); break; }
            if (wl_display_dispatch_pending(display) < 0) { logmsg("display dispatch failed"); break; }
        } else {
            wl_display_cancel_read(display);
        }

        if (pr > 0) {
            if (pfds[1].revents & POLLIN) accept_client();
            if (cin >= 0 && (pfds[cin].revents & (POLLIN | POLLHUP | POLLERR))) {
                if (handle_client_input() < 0) {
                    /* A headset that drops (app restart, USB hiccup, sleep) must
                       not take the bridge with it: the Quest reconnects every
                       second, and the whole point is to still be listening. */
                    logmsg("headset disconnected");
                    close(client_fd); client_fd = -1;
                }
            }
        }

        // Frame pacing: start a capture on every output, then send whatever is ready.
        double t = now_sec();
        if (verbose && t >= next_stats) {
            next_stats = t + 2.0;
            logmsg("stats: captures=%ld ready=%ld failed=%ld sent=%ld client=%d",
                   stat_captures, stat_ready, stat_failed, stat_sent, client_fd >= 0);
        }
        if (t >= next_frame) {
            next_frame = t + frame_interval;
            for (int i = 0; i < n_outputs; i++) start_capture(&outputs[i]);
            wl_display_flush(display);
        }
        if (client_fd >= 0) {
            for (int i = 0; i < n_outputs; i++) {
                if (outputs[i].ready) {
                    if (send_frame(&outputs[i]) < 0) {
                        logmsg("send failed, dropping headset");
                        close(client_fd); client_fd = -1;
                        break;
                    }
                    stat_sent++;
                }
            }
        }
    }

    logmsg("shutting down");
    if (client_fd >= 0) close(client_fd);
    close(listen_fd);
    for (int i = 0; i < n_outputs; i++) output_free_buffer(&outputs[i]);
    if (vpointer) zwlr_virtual_pointer_v1_destroy(vpointer);
    wl_display_disconnect(display);
    return 0;
}
