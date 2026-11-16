/*
 * vrdesk_xr.c - Quest-side OpenXR client for the vrdesk virtual desktop.
 *
 * Renders every virtual monitor streamed by the Linux bridge (bin/vrdesk-bridge)
 * as a textured quad in 3D space, and turns controller ray + trigger into
 * MSG_POINTER events back to the bridge.
 *
 * Wire format (must match src/bridge.c):
 *   server -> client  MSG_FRAME  = 1: u8 type, u16 oid, u32 w, u32 h, u32 stride,
 *                                  u8 flags, u32 len, payload
 *                     flags bit0 = payload is zlib-deflated (inflates to stride*h BGRA bytes)
 *   client -> server  MSG_POINTER= 2: u16 oid, f32 x, f32 y (normalised), u8 buttons, u8 action
 *                     MSG_SCROLL = 4: u16 oid, f32 dx, f32 dy
 *   action: 0 = move, 1 = press, 2 = release
 *
 * Networking runs on its own pthread so the XR frame loop never blocks.
 */
#define LOG_TAG "vrdesk_xr"
#include <android/log.h>
#include <android/native_window.h>
#include <android_native_app_glue.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES3/gl3ext.h>

#define XR_USE_GRAPHICS_API_OPENGL_ES
#define XR_USE_PLATFORM_ANDROID
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <errno.h>
#include <math.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>
#include <zlib.h>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

#define MAX_OUTPUTS 8
#define MAX_TEX     4          /* texture ring per monitor */
/* Fallback only: the real address comes from /sdcard/vrdesk.conf (pushed by
   the desktop's start-bridge.sh) or from the adb reverse tunnel. A hardcoded
   LAN address goes stale the moment the host's DHCP lease changes, and the
   app then loops forever on "connecting to ..." with no clue why. */
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT 5000

/* ------------------------------------------------------------------ wire */

#define MSG_FRAME   1
#define MSG_POINTER 2
#define MSG_KEY     3
#define MSG_SCROLL  4
#define FRAME_FLAG_ZLIB 0x01

static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t rd32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* ----------------------------------------------------------------- state */

typedef struct {
    int      used;
    uint16_t oid;
    uint32_t w, h;
    /* CPU staging for the newest frame, guarded by net_mu */
    uint8_t *pending;
    size_t   pending_cap;
    int      dirty;
    /* GL side, owned by the render thread */
    GLuint   tex[MAX_TEX];
    int      tex_w[MAX_TEX], tex_h[MAX_TEX];
    int      tex_next;
    int      tex_count;
} monitor_t;

static monitor_t        g_mon[MAX_OUTPUTS];
static pthread_mutex_t  g_net_mu = PTHREAD_MUTEX_INITIALIZER;

static int  g_sock = -1;
static char g_host[128] = DEFAULT_HOST;
static int  g_port = DEFAULT_PORT;
static volatile int g_net_running = 0;
static volatile int g_connected = 0;
static volatile int g_bad_frames = 0;   /* frames that failed to inflate */
static volatile int g_quit = 0;

/* outbound queue: controller events produced by the render thread */
typedef struct { uint8_t buf[16]; int len; } outmsg_t;
#define OUTQ 256
static outmsg_t         g_outq[OUTQ];
static int              g_out_head = 0, g_out_tail = 0;
static pthread_mutex_t  g_out_mu = PTHREAD_MUTEX_INITIALIZER;

static void out_push(const uint8_t *b, int len) {
    if (len <= 0 || len > 16) return;
    pthread_mutex_lock(&g_out_mu);
    int nt = (g_out_tail + 1) % OUTQ;
    if (nt != g_out_head) {
        memcpy(g_outq[g_out_tail].buf, b, len);
        g_outq[g_out_tail].len = len;
        g_out_tail = nt;
    }
    pthread_mutex_unlock(&g_out_mu);
}

/* ------------------------------------------------------------ networking */

static int send_all(int fd, const uint8_t *b, int len) {
    int off = 0;
    while (off < len) {
        ssize_t n = send(fd, b + off, len - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR)) continue;
            return -1;
        }
        off += (int)n;
    }
    return 0;
}

static int recv_all(int fd, uint8_t *b, int len) {
    int off = 0;
    while (off < len) {
        ssize_t n = recv(fd, b + off, len - off, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            return -1;
        }
        off += (int)n;
    }
    return 0;
}

static monitor_t *mon_for(uint16_t oid) {
    for (int i = 0; i < MAX_OUTPUTS; i++)
        if (g_mon[i].used && g_mon[i].oid == oid) return &g_mon[i];
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        if (!g_mon[i].used) {
            memset(&g_mon[i], 0, sizeof(g_mon[i]));
            g_mon[i].used = 1;
            g_mon[i].oid = oid;
            LOGI("new monitor oid=%u", oid);
            return &g_mon[i];
        }
    }
    return NULL;
}

static void *net_thread(void *arg) {
    (void)arg;
    while (!g_quit) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) { usleep(500000); continue; }
        struct sockaddr_in sa;
        memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = htons((uint16_t)g_port);
        if (inet_pton(AF_INET, g_host, &sa.sin_addr) != 1) {
            LOGE("bad host '%s'", g_host);
            close(fd);
            sleep(2);
            continue;
        }
        LOGI("connecting to %s:%d", g_host, g_port);
        if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
            close(fd);
            sleep(2);
            continue;
        }
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        g_sock = fd;
        g_connected = 1;
        LOGI("connected to bridge %s:%d", g_host, g_port);

        uint8_t hdr[17];
        while (!g_quit) {
            /* drain outbound controller events first */
            for (;;) {
                uint8_t tmp[16];
                int len = 0;
                pthread_mutex_lock(&g_out_mu);
                if (g_out_head != g_out_tail) {
                    len = g_outq[g_out_head].len;
                    memcpy(tmp, g_outq[g_out_head].buf, len);
                    g_out_head = (g_out_head + 1) % OUTQ;
                }
                pthread_mutex_unlock(&g_out_mu);
                if (!len) break;
                if (send_all(fd, tmp, len) != 0) goto drop;
            }
            /* then one frame header (blocking, but the bridge streams at ~30 fps).
               The bridge writes type(1) + oid(2) + w(4) + h(4) + stride(4) + flags(1)
               + len(4) = 23 bytes. Reading only 17 (an earlier bug) left 2 bytes of
               every header in the stream, so each subsequent header was parsed 2 bytes
               off and the length field came out as garbage. */
            if (recv_all(fd, hdr, 23) != 0) goto drop;
            if (hdr[0] != MSG_FRAME) {
                LOGE("unexpected message type %u", hdr[0]);
                goto drop;
            }
            uint16_t oid    = rd16(hdr + 1);
            uint32_t w      = rd32(hdr + 3);
            uint32_t h      = rd32(hdr + 7);
            uint32_t stride = rd32(hdr + 11);
            uint8_t  flags  = hdr[15];
            uint32_t len    = rd32(hdr + 16);
            uint32_t raw    = stride * h;
            if (w == 0 || h == 0 || stride < w * 4 || len == 0 || len > (64u << 20) ||
                (!(flags & FRAME_FLAG_ZLIB) && len != raw)) {
                LOGE("bad frame oid=%u %ux%u stride=%u flags=%u len=%u", oid, w, h, stride, flags, len);
                goto drop;
            }
            monitor_t *m = mon_for(oid);
            if (!m) { LOGE("too many monitors"); goto drop; }
            pthread_mutex_lock(&g_net_mu);
            /* always stage a full raw-size buffer: a compressed frame inflates into it */
            if (m->pending_cap < raw) {
                uint8_t *nb = realloc(m->pending, raw);
                if (!nb) {
                    pthread_mutex_unlock(&g_net_mu);
                    LOGE("oom for frame %u bytes", raw);
                    goto drop;
                }
                m->pending = nb;
                m->pending_cap = raw;
            }
            m->w = w;
            m->h = h;
            pthread_mutex_unlock(&g_net_mu);
            if (recv_all(fd, m->pending, (int)len) != 0) goto drop;
            if (flags & FRAME_FLAG_ZLIB) {
                /* zlib stream: inflate in place into a scratch buffer, then swap it in.
                   uLongf is the zlib contract; the frame is at most a few MB. */
                static uint8_t *zbuf;
                static size_t zcap;
                if (zcap < raw) {
                    uint8_t *nb = realloc(zbuf, raw);
                    if (!nb) { LOGE("oom for inflate %u bytes", raw); goto drop; }
                    zbuf = nb; zcap = raw;
                }
                uLongf out = (uLongf)raw;
                int zr = uncompress(zbuf, &out, m->pending, (uLong)len);
                if (zr != Z_OK || out != (uLongf)raw) {
                    LOGE("inflate failed zr=%d out=%lu want=%u", zr, (unsigned long)out, raw);
                    g_bad_frames++;
                    goto drop;
                }
                pthread_mutex_lock(&g_net_mu);
                uint8_t *tmp = m->pending;
                m->pending = zbuf;
                m->pending_cap = raw;
                zbuf = tmp; zcap = raw;
                pthread_mutex_unlock(&g_net_mu);
            }
            pthread_mutex_lock(&g_net_mu);
            m->dirty = 1;
            pthread_mutex_unlock(&g_net_mu);
        }
    drop:
        g_connected = 0;
        if (g_sock >= 0) { close(g_sock); g_sock = -1; }
        LOGI("disconnected from bridge");
        sleep(1);
    }
    g_net_running = 0;
    return NULL;
}

/* ---------------------------------------------------------------- OpenXR */

static XrInstance     g_instance = XR_NULL_HANDLE;
static XrSession      g_session  = XR_NULL_HANDLE;
static XrSystemId     g_system   = XR_NULL_SYSTEM_ID;
static XrSpace        g_local    = XR_NULL_HANDLE;
static XrSpace        g_view     = XR_NULL_HANDLE;
static XrSessionState g_state    = XR_SESSION_STATE_UNKNOWN;
static int            g_session_ready = 0;

static XrActionSet    g_actionset = XR_NULL_HANDLE;
static XrAction       g_pose_action = XR_NULL_HANDLE;
static XrAction       g_trigger_action = XR_NULL_HANDLE;
static XrAction       g_click_action = XR_NULL_HANDLE;
static XrPath         g_hand_path[2] = {XR_NULL_PATH, XR_NULL_PATH};
static XrSpace        g_hand_space[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
static int            g_prev_trigger[2] = {0, 0};
static int            g_prev_click[2] = {0, 0};

/* swapchain: one 2D array layer per eye */
typedef struct { XrSwapchain handle; int32_t w, h; uint32_t n; XrSwapchainImageOpenGLESKHR *imgs; } swapchain_t;
static swapchain_t g_sc[2];
static uint32_t    g_sc_count = 0;

static EGLDisplay g_egl_display = EGL_NO_DISPLAY;
static EGLConfig  g_egl_config  = NULL;
static EGLContext g_egl_context = EGL_NO_CONTEXT;
static EGLSurface g_egl_surface = EGL_NO_SURFACE;

static const char *VERT_SRC =
    "#version 300 es\n"
    "layout(location=0) in vec3 a_pos;\n"
    "layout(location=1) in vec2 a_uv;\n"
    "uniform mat4 u_mvp;\n"
    "out vec2 v_uv;\n"
    "void main(){ v_uv = a_uv; gl_Position = u_mvp * vec4(a_pos,1.0); }\n";

static const char *FRAG_SRC =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 v_uv;\n"
    "uniform sampler2D u_tex;\n"
    "uniform int u_has_tex;\n"
    "uniform vec4 u_tint;\n"
    "out vec4 o_color;\n"
    "void main(){\n"
    "  if (u_has_tex == 1) o_color = texture(u_tex, v_uv);\n"
    "  else o_color = u_tint;\n"
    "}\n";

static GLuint g_prog = 0, g_vbo = 0, g_vao = 0;
static GLint  g_u_mvp = -1, g_u_tex = -1, g_u_has_tex = -1, g_u_tint = -1;

static GLuint compile(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        LOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static void gl_init(void) {
    GLuint vs = compile(GL_VERTEX_SHADER, VERT_SRC);
    GLuint fs = compile(GL_FRAGMENT_SHADER, FRAG_SRC);
    g_prog = glCreateProgram();
    glAttachShader(g_prog, vs);
    glAttachShader(g_prog, fs);
    glLinkProgram(g_prog);
    GLint ok = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[1024];
        glGetProgramInfoLog(g_prog, sizeof(log), NULL, log);
        LOGE("program link failed: %s", log);
    }
    glDeleteShader(vs);
    glDeleteShader(fs);
    g_u_mvp     = glGetUniformLocation(g_prog, "u_mvp");
    g_u_tex     = glGetUniformLocation(g_prog, "u_tex");
    g_u_has_tex = glGetUniformLocation(g_prog, "u_has_tex");
    g_u_tint    = glGetUniformLocation(g_prog, "u_tint");

    /* unit quad in XY plane, UV origin top-left to match BGRA rows */
    static const float verts[] = {
        /* x     y     z    u    v */
        -0.5f, -0.5f, 0.f, 0.f, 1.f,
         0.5f, -0.5f, 0.f, 1.f, 1.f,
         0.5f,  0.5f, 0.f, 1.f, 0.f,
        -0.5f, -0.5f, 0.f, 0.f, 1.f,
         0.5f,  0.5f, 0.f, 1.f, 0.f,
        -0.5f,  0.5f, 0.f, 0.f, 0.f,
    };
    glGenVertexArrays(1, &g_vao);
    glBindVertexArray(g_vao);
    glGenBuffers(1, &g_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
    LOGI("GL initialised, program=%u", g_prog);
}

/* ------------------------------------------------------------ small math */

typedef struct { float m[16]; } mat4_t;

static mat4_t mat_identity(void) {
    mat4_t r;
    memset(&r, 0, sizeof(r));
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

static mat4_t mat_mul(mat4_t a, mat4_t b) { /* a * b, column-major */
    mat4_t r;
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float s = 0.f;
            for (int k = 0; k < 4; k++) s += a.m[k * 4 + row] * b.m[c * 4 + k];
            r.m[c * 4 + row] = s;
        }
    return r;
}

static mat4_t mat_from_xr(XrPosef p) {
    float x = p.orientation.x, y = p.orientation.y, z = p.orientation.z, w = p.orientation.w;
    mat4_t r;
    r.m[0]  = 1 - 2 * (y * y + z * z);
    r.m[1]  = 2 * (x * y + z * w);
    r.m[2]  = 2 * (x * z - y * w);
    r.m[3]  = 0;
    r.m[4]  = 2 * (x * y - z * w);
    r.m[5]  = 1 - 2 * (x * x + z * z);
    r.m[6]  = 2 * (y * z + x * w);
    r.m[7]  = 0;
    r.m[8]  = 2 * (x * z + y * w);
    r.m[9]  = 2 * (y * z - x * w);
    r.m[10] = 1 - 2 * (x * x + y * y);
    r.m[11] = 0;
    r.m[12] = p.position.x;
    r.m[13] = p.position.y;
    r.m[14] = p.position.z;
    r.m[15] = 1;
    return r;
}

static mat4_t mat_projection(XrFovf fov, float near_z, float far_z) {
    float l = tanf(fov.angleLeft), r = tanf(fov.angleRight);
    float d = tanf(fov.angleDown), u = tanf(fov.angleUp);
    mat4_t m;
    memset(&m, 0, sizeof(m));
    m.m[0]  = 2.f / (r - l);
    m.m[5]  = 2.f / (u - d);
    m.m[8]  = (r + l) / (r - l);
    m.m[9]  = (u + d) / (u - d);
    m.m[10] = -(far_z + near_z) / (far_z - near_z);
    m.m[11] = -1.f;
    m.m[14] = -(2.f * far_z * near_z) / (far_z - near_z);
    return m;
}

static mat4_t mat_view(XrPosef p) {
    mat4_t t = mat_from_xr(p);
    /* invert rigid transform */
    mat4_t r;
    for (int c = 0; c < 3; c++)
        for (int row = 0; row < 3; row++) r.m[c * 4 + row] = t.m[row * 4 + c];
    r.m[3] = r.m[7] = r.m[11] = 0.f;
    r.m[12] = -(r.m[0] * t.m[12] + r.m[4] * t.m[13] + r.m[8] * t.m[14]);
    r.m[13] = -(r.m[1] * t.m[12] + r.m[5] * t.m[13] + r.m[9] * t.m[14]);
    r.m[14] = -(r.m[2] * t.m[12] + r.m[6] * t.m[13] + r.m[10] * t.m[14]);
    r.m[15] = 1.f;
    return r;
}

/* ------------------------------------------------------------------ XR */

static void xr_check(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return;
    char buf[XR_MAX_RESULT_STRING_SIZE] = {0};
    if (g_instance != XR_NULL_HANDLE) xrResultToString(g_instance, r, buf);
    LOGE("%s failed: %d %s", what, (int)r, buf);
}

static XrResult android_thread_init(struct android_app *app) {
    PFN_xrInitializeLoaderKHR init_loader = NULL;
    XrResult r = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR",
                                       (PFN_xrVoidFunction *)&init_loader);
    if (XR_FAILED(r) || !init_loader) {
        LOGE("xrInitializeLoaderKHR unavailable (%d)", (int)r);
        return r;
    }
    XrLoaderInitInfoAndroidKHR info = {XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM   = app->activity->vm;
    info.applicationContext = app->activity->clazz;
    return init_loader((const XrLoaderInitInfoBaseHeaderKHR *)&info);
}

static int xr_init(struct android_app *app) {
    (void)app;
    xr_check(android_thread_init(app), "loader init");

    const char *exts[] = {
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
    };
    XrInstanceCreateInfoAndroidKHR aci = {XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    aci.applicationVM = app->activity->vm;
    aci.applicationActivity = app->activity->clazz;

    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &aci;
    ci.enabledExtensionCount = 2;
    ci.enabledExtensionNames = exts;
    snprintf(ci.applicationInfo.applicationName, XR_MAX_APPLICATION_NAME_SIZE, "vrdesk");
    ci.applicationInfo.applicationVersion = 1;
    snprintf(ci.applicationInfo.engineName, XR_MAX_ENGINE_NAME_SIZE, "vrdesk-gl");
    ci.applicationInfo.engineVersion = 1;
    ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
    XrResult r = xrCreateInstance(&ci, &g_instance);
    if (XR_FAILED(r)) { LOGE("xrCreateInstance failed %d", (int)r); return 0; }

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_instance, &props);
    LOGI("OpenXR runtime: %s %u.%u.%u", props.runtimeName,
         XR_VERSION_MAJOR(props.runtimeVersion), XR_VERSION_MINOR(props.runtimeVersion),
         XR_VERSION_PATCH(props.runtimeVersion));

    XrSystemGetInfo sgi = {XR_TYPE_SYSTEM_GET_INFO};
    sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    r = xrGetSystem(g_instance, &sgi, &g_system);
    if (XR_FAILED(r)) { LOGE("xrGetSystem failed %d", (int)r); return 0; }

    /* actions */
    XrActionSetCreateInfo asci = {XR_TYPE_ACTION_SET_CREATE_INFO};
    snprintf(asci.actionSetName, XR_MAX_ACTION_SET_NAME_SIZE, "vrdesk");
    snprintf(asci.localizedActionSetName, XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE, "vrdesk");
    asci.priority = 0;
    xr_check(xrCreateActionSet(g_instance, &asci, &g_actionset), "createActionSet");
    xrStringToPath(g_instance, "/user/hand/left",  &g_hand_path[0]);
    xrStringToPath(g_instance, "/user/hand/right", &g_hand_path[1]);

    XrActionCreateInfo act = {XR_TYPE_ACTION_CREATE_INFO};
    act.countSubactionPaths = 2;
    act.subactionPaths = g_hand_path;
    snprintf(act.actionName, XR_MAX_ACTION_NAME_SIZE, "hand_pose");
    snprintf(act.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Hand pose");
    act.actionType = XR_ACTION_TYPE_POSE_INPUT;
    xr_check(xrCreateAction(g_actionset, &act, &g_pose_action), "createAction pose");

    memset(&act, 0, sizeof(act));
    act.type = XR_TYPE_ACTION_CREATE_INFO;
    act.countSubactionPaths = 2;
    act.subactionPaths = g_hand_path;
    snprintf(act.actionName, XR_MAX_ACTION_NAME_SIZE, "trigger");
    snprintf(act.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Trigger");
    act.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    xr_check(xrCreateAction(g_actionset, &act, &g_trigger_action), "createAction trigger");

    memset(&act, 0, sizeof(act));
    act.type = XR_TYPE_ACTION_CREATE_INFO;
    act.countSubactionPaths = 2;
    act.subactionPaths = g_hand_path;
    snprintf(act.actionName, XR_MAX_ACTION_NAME_SIZE, "click");
    snprintf(act.localizedActionName, XR_MAX_LOCALIZED_ACTION_NAME_SIZE, "Click");
    act.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    xr_check(xrCreateAction(g_actionset, &act, &g_click_action), "createAction click");

    /* bindings: aim pose, trigger float, thumbstick click as alternate click */
    XrPath aim[2], trig[2], stick[2], click[2];
    xrStringToPath(g_instance, "/user/hand/left/input/aim/pose",   &aim[0]);
    xrStringToPath(g_instance, "/user/hand/right/input/aim/pose",  &aim[1]);
    xrStringToPath(g_instance, "/user/hand/left/input/trigger/value",  &trig[0]);
    xrStringToPath(g_instance, "/user/hand/right/input/trigger/value", &trig[1]);
    xrStringToPath(g_instance, "/user/hand/left/input/thumbstick/click",  &stick[0]);
    xrStringToPath(g_instance, "/user/hand/right/input/thumbstick/click", &stick[1]);
    xrStringToPath(g_instance, "/user/hand/left/input/squeeze/click",  &click[0]);
    xrStringToPath(g_instance, "/user/hand/right/input/squeeze/click", &click[1]);

    XrActionSuggestedBinding binds[6];
    int nb = 0;
    for (int i = 0; i < 2; i++) {
        binds[nb].action = g_pose_action;    binds[nb++].binding = aim[i];
        binds[nb].action = g_trigger_action; binds[nb++].binding = trig[i];
        binds[nb].action = g_click_action;   binds[nb++].binding = stick[i];
    }
    XrPath profile;
    xrStringToPath(g_instance, "/interaction_profiles/oculus/touch_controller", &profile);
    XrInteractionProfileSuggestedBinding sb = {XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = profile;
    sb.countSuggestedBindings = nb;
    sb.suggestedBindings = binds;
    xr_check(xrSuggestInteractionProfileBindings(g_instance, &sb), "suggestBindings");

    /* NOTE: reference spaces and action spaces are created in session_create(),
       not here: they need g_session, which does not exist until xrCreateSession.
       Creating them here used to fail with XR_ERROR_HANDLE_INVALID (-12). */
    LOGI("OpenXR initialised, system=%llu", (unsigned long long)g_system);
    return 1;
}

/* --------------------------------------------------------------- EGL/GL */

static int egl_init(void) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR get_req = NULL;
    xrGetInstanceProcAddr(g_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          (PFN_xrVoidFunction *)&get_req);
    if (get_req) {
        XrGraphicsRequirementsOpenGLESKHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
        xr_check(get_req(g_instance, g_system, &req), "getGLESRequirements");
        LOGI("GLES requirements: %u.%u .. %u.%u",
             XR_VERSION_MAJOR(req.minApiVersionSupported), XR_VERSION_MINOR(req.minApiVersionSupported),
             XR_VERSION_MAJOR(req.maxApiVersionSupported), XR_VERSION_MINOR(req.maxApiVersionSupported));
    }

    g_egl_display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g_egl_display == EGL_NO_DISPLAY) { LOGE("no EGL display"); return 0; }
    EGLint major = 0, minor = 0;
    if (!eglInitialize(g_egl_display, &major, &minor)) { LOGE("eglInitialize failed"); return 0; }
    LOGI("EGL %d.%d", major, minor);

    const EGLint cfg_attr[] = {
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 16,
        EGL_NONE
    };
    EGLint ncfg = 0;
    if (!eglChooseConfig(g_egl_display, cfg_attr, &g_egl_config, 1, &ncfg) || ncfg < 1) {
        LOGE("eglChooseConfig failed");
        return 0;
    }
    const EGLint ctx_attr[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};
    g_egl_context = eglCreateContext(g_egl_display, g_egl_config, EGL_NO_CONTEXT, ctx_attr);
    if (g_egl_context == EGL_NO_CONTEXT) { LOGE("eglCreateContext failed"); return 0; }
    const EGLint pb_attr[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
    g_egl_surface = eglCreatePbufferSurface(g_egl_display, g_egl_config, pb_attr);
    if (g_egl_surface == EGL_NO_SURFACE) { LOGE("pbuffer failed"); return 0; }
    if (!eglMakeCurrent(g_egl_display, g_egl_surface, g_egl_surface, g_egl_context)) {
        LOGE("eglMakeCurrent failed");
        return 0;
    }
    LOGI("GL_VERSION=%s", (const char *)glGetString(GL_VERSION));
    gl_init();
    return 1;
}

static int session_create(void) {
    XrGraphicsBindingOpenGLESAndroidKHR gb = {XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = g_egl_display;
    gb.config  = g_egl_config;
    gb.context = g_egl_context;
    XrSessionCreateInfo sci = {XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = g_system;
    XrResult r = xrCreateSession(g_instance, &sci, &g_session);
    if (XR_FAILED(r)) { LOGE("xrCreateSession failed %d", (int)r); return 0; }

    XrSessionActionSetsAttachInfo attach = {XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g_actionset;
    xr_check(xrAttachSessionActionSets(g_session, &attach), "attachActionSets");

    /* reference + action spaces: these need g_session, so they belong here and
       not in xr_init (which runs before xrCreateSession). */
    XrPosef ident;
    memset(&ident, 0, sizeof(ident));
    ident.orientation.w = 1.f;
    XrReferenceSpaceCreateInfo rsci = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rsci.poseInReferenceSpace = ident;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &rsci, &g_local))) {
        LOGE("createReferenceSpace local failed");
        return 0;
    }
    rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(g_session, &rsci, &g_view))) {
        LOGE("createReferenceSpace view failed");
        return 0;
    }
    for (int i = 0; i < 2; i++) {
        XrActionSpaceCreateInfo asci2 = {XR_TYPE_ACTION_SPACE_CREATE_INFO};
        asci2.action = g_pose_action;
        asci2.subactionPath = g_hand_path[i];
        asci2.poseInActionSpace = ident;
        if (XR_FAILED(xrCreateActionSpace(g_session, &asci2, &g_hand_space[i]))) {
            LOGE("createActionSpace %d failed", i);
            return 0;
        }
    }
    LOGI("session created, spaces ready");
    return 1;
}

/* ------------------------------------------------------------ rendering */

static void upload_pending(monitor_t *m) {
    pthread_mutex_lock(&g_net_mu);
    if (!m->dirty || !m->pending) {
        pthread_mutex_unlock(&g_net_mu);
        return;
    }
    uint32_t w = m->w, h = m->h;
    uint8_t *copy = m->pending;
    m->pending = NULL;              /* hand ownership to GL side */
    size_t cap = m->pending_cap;
    m->pending_cap = 0;
    m->dirty = 0;
    pthread_mutex_unlock(&g_net_mu);

    if (m->tex_count < MAX_TEX) {
        glGenTextures(1, &m->tex[m->tex_count]);
        m->tex_count++;
    }
    GLuint t = m->tex[m->tex_next];
    m->tex_w[m->tex_next] = (int)w;
    m->tex_h[m->tex_next] = (int)h;
    glBindTexture(GL_TEXTURE_2D, t);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)w, (GLsizei)h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, copy);
    m->tex_next = (m->tex_next + 1) % MAX_TEX;
    free(copy);
    (void)cap;
}

/* layout: monitors in a gentle arc in front of the user.
 * This is the SINGLE source of truth for quad placement: draw_monitors() and
 * ray_hits_monitor() both take x/y/z/w/h from here, so the controller ray always
 * matches what is on screen. */
#define MON_DIST      0.60f   /* metres in front of the head (0.80 read as "2-3 ft away", steer #98) */
#define MON_CURVE     0.05f   /* side monitors curve away by this much per metre of x */
#define MON_WIDTH     1.50f   /* quad width in metres: ~1.5 m at ~1.15 m => ~66 deg wide */
#define MON_GAP       0.06f   /* gap between neighbouring quads, metres */
#define MON_EYE_Y    -0.05f   /* slight downward offset so the arc is centred on the eyes */

static void monitor_transform(int idx, int total, float *out_x, float *out_y, float *out_z,
                              float *out_w, float *out_h) {
    float w = MON_WIDTH;
    float spacing = w + MON_GAP;
    float x = ((float)idx - (float)(total - 1) * 0.5f) * spacing;
    float z = -MON_DIST - fabsf(x) * MON_CURVE;   /* curve away */
    float aspect = 16.f / 9.f;
    *out_x = x;
    *out_y = MON_EYE_Y;
    *out_z = z;
    *out_w = w;
    *out_h = w / aspect;
}

static void draw_monitors(mat4_t viewproj) {
    int total = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++) if (g_mon[i].used) total++;
    int idx = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++) {
        monitor_t *m = &g_mon[i];
        if (!m->used) continue;
        upload_pending(m);
        float x, y, z, w, h;
        monitor_transform(idx++, total, &x, &y, &z, &w, &h);
        float aspect = 16.f / 9.f;
        if (m->tex_count) {
            int li = (m->tex_next + MAX_TEX - 1) % MAX_TEX;
            if (m->tex_h[li] > 0) aspect = (float)m->tex_w[li] / (float)m->tex_h[li];
        }
        float hh = w / aspect;
        mat4_t model = mat_identity();
        model.m[0]  = w;
        model.m[5]  = hh;
        model.m[10] = 1.f;
        model.m[12] = x;
        model.m[13] = y;
        model.m[14] = z;
        mat4_t mvp = mat_mul(viewproj, model);
        glUseProgram(g_prog);
        glUniformMatrix4fv(g_u_mvp, 1, GL_FALSE, mvp.m);
        glBindVertexArray(g_vao);
        if (m->tex_count) {
            int li = (m->tex_next + MAX_TEX - 1) % MAX_TEX;
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_2D, m->tex[li]);
            glUniform1i(g_u_tex, 0);
            glUniform1i(g_u_has_tex, 1);
        } else {
            glUniform1i(g_u_has_tex, 0);
            glUniform4f(g_u_tint, 0.15f, 0.18f, 0.25f, 1.f);
        }
        glDrawArrays(GL_TRIANGLES, 0, 6);
    }
    glBindVertexArray(0);
}

/* ray/quad intersection: returns normalised hit coords on the monitor */
static int ray_hits_monitor(XrVector3f origin, XrVector3f dir, int idx, int total,
                            float *u, float *v) {
    float x, y, z, w, h;
    monitor_transform(idx, total, &x, &y, &z, &w, &h);
    float aspect = 16.f / 9.f;
    float hh = w / aspect;
    if (fabsf(dir.z) < 1e-6f) return 0;
    float t = (z - origin.z) / dir.z;
    if (t <= 0.f) return 0;
    float px = origin.x + dir.x * t;
    float py = origin.y + dir.y * t;
    float lu = (px - x) / w + 0.5f;
    float lv = 0.5f - (py - y) / hh;
    if (lu < 0.f || lu > 1.f || lv < 0.f || lv > 1.f) return 0;
    *u = lu;
    *v = lv;
    return 1;
}

static void send_pointer_full(uint16_t oid, float u, float v, int action) {
    /* The bridge reads type(1) + oid(2) + x(4) + y(4) + buttons(1) + action(1) = 13
       bytes. This used to send only 12 and put `action` where `buttons` belongs,
       so the bridge consumed the first byte of the NEXT message as `action` and
       the app->bridge stream stayed one byte out of step forever. Eventually the
       bridge read a 0 byte as a message type, logged "unknown message type 0" and
       dropped the headset - which the app then reconnected, once per second. */
    uint8_t m[13];
    m[0] = MSG_POINTER;
    m[1] = (uint8_t)(oid & 0xff);
    m[2] = (uint8_t)(oid >> 8);
    memcpy(m + 3, &u, 4);
    memcpy(m + 7, &v, 4);
    m[11] = (action == 0) ? 0 : 1;   /* buttons: held while pressed */
    m[12] = (uint8_t)action;         /* 0 = move, 1 = press, 2 = release */
    out_push(m, 13);
}

/* -------------------------------------------------------------- frame loop */

static void poll_controllers(XrTime predicted, XrSpace base) {
    XrActiveActionSet aas = {g_actionset, XR_NULL_PATH};
    XrActionsSyncInfo si = {XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1;
    si.activeActionSets = &aas;
    if (XR_FAILED(xrSyncActions(g_session, &si))) return;

    int total = 0;
    for (int i = 0; i < MAX_OUTPUTS; i++) if (g_mon[i].used) total++;

    for (int h = 0; h < 2; h++) {
        XrActionStatePose pose_state = {XR_TYPE_ACTION_STATE_POSE};
        XrActionStateGetInfo gi = {XR_TYPE_ACTION_STATE_GET_INFO};
        gi.action = g_pose_action;
        gi.subactionPath = g_hand_path[h];
        xrGetActionStatePose(g_session, &gi, &pose_state);
        if (!pose_state.isActive) continue;

        XrSpaceLocation loc = {XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(g_hand_space[h], base, predicted, &loc))) continue;
        if (!(loc.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) ||
            !(loc.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) continue;

        /* aim direction = -Z of the aim pose */
        float qx = loc.pose.orientation.x, qy = loc.pose.orientation.y;
        float qz = loc.pose.orientation.z, qw = loc.pose.orientation.w;
        XrVector3f dir;
        dir.x = -2.f * (qx * qz + qw * qy);
        dir.y = -2.f * (qy * qz - qw * qx);
        dir.z = -(1.f - 2.f * (qx * qx + qy * qy));

        XrActionStateFloat trig = {XR_TYPE_ACTION_STATE_FLOAT};
        gi.action = g_trigger_action;
        xrGetActionStateFloat(g_session, &gi, &trig);
        XrActionStateBoolean click = {XR_TYPE_ACTION_STATE_BOOLEAN};
        gi.action = g_click_action;
        xrGetActionStateBoolean(g_session, &gi, &click);

        int pressed = (trig.isActive && trig.currentState > 0.55f) ||
                      (click.isActive && click.currentState);

        /* find the nearest monitor the ray hits */
        int best = -1;
        float bu = 0.f, bv = 0.f;
        for (int i = 0, idx = 0; i < MAX_OUTPUTS; i++) {
            if (!g_mon[i].used) continue;
            float u, v;
            if (ray_hits_monitor(loc.pose.position, dir, idx, total, &u, &v)) {
                best = i;
                bu = u;
                bv = v;
            }
            idx++;
        }
        if (best >= 0) {
            uint16_t oid = g_mon[best].oid;
            send_pointer_full(oid, bu, bv, 0);                 /* move */
            if (pressed && !g_prev_trigger[h]) send_pointer_full(oid, bu, bv, 1);
            if (!pressed && g_prev_trigger[h]) send_pointer_full(oid, bu, bv, 2);
        }
        g_prev_trigger[h] = pressed;
    }
}

/* One-shot eye-buffer readback for layout verification. Runs only when
 * /sdcard/vrdesk.dump exists, and only once per process. Reads back the eye
 * buffer we just drew, finds the monitor quad's non-background bounding box,
 * logs its pixel size, and writes a PPM so the result can be inspected.
 * This measures what the GPU actually drew rather than trusting the transform
 * math, which is the only way to answer "is the text big enough". */
static void dump_eye_once(int w, int h) {
    static int done = 0;
    if (done) return;
    FILE *t = fopen("/sdcard/vrdesk.dump", "r");
    if (!t) return;
    fclose(t);
    done = 1;

    size_t n = (size_t)w * (size_t)h * 4;
    uint8_t *px = malloc(n);
    if (!px) { LOGE("dump: out of memory"); return; }
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);

    /* background is the clear colour 0.04,0.05,0.08 -> ~10,13,20 */
    int minx = w, maxx = -1, miny = h, maxy = -1;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            const uint8_t *p = px + ((size_t)y * w + x) * 4;
            if (p[0] > 24 || p[1] > 24 || p[2] > 34) {
                if (x < minx) minx = x;
                if (x > maxx) maxx = x;
                if (y < miny) miny = y;
                if (y > maxy) maxy = y;
            }
        }
    }
    if (maxx >= 0) {
        int bw = maxx - minx + 1, bh = maxy - miny + 1;
        LOGI("dump: eye %dx%d quad bbox x=%d..%d y=%d..%d -> %dx%d px (%.1f%% of width)",
             w, h, minx, maxx, miny, maxy, bw, bh, 100.0 * bw / (double)w);
    } else {
        LOGI("dump: eye %dx%d no non-background pixels (nothing drawn)", w, h);
    }

    FILE *f = fopen("/sdcard/vrdesk-eye0.ppm", "wb");
    if (f) {
        fprintf(f, "P6\n%d %d\n255\n", w, h);
        for (int y = h - 1; y >= 0; y--) {          /* GL origin is bottom-left */
            for (int x = 0; x < w; x++) {
                const uint8_t *p = px + ((size_t)y * w + x) * 4;
                fwrite(p, 1, 3, f);
            }
        }
        fclose(f);
        LOGI("dump: wrote /sdcard/vrdesk-eye0.ppm");
    } else {
        LOGE("dump: cannot write /sdcard/vrdesk-eye0.ppm");
    }
    free(px);
}

static void render_frame(XrTime predicted, XrDuration timeout) {
    XrFrameWaitInfo fwi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(g_session, &fwi, &fs))) return;
    XrFrameBeginInfo fbi = {XR_TYPE_FRAME_BEGIN_INFO};
    if (XR_FAILED(xrBeginFrame(g_session, &fbi))) return;

    XrFrameEndInfo fei = {XR_TYPE_FRAME_END_INFO};
    fei.displayTime = fs.predictedDisplayTime;
    fei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

    XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView views[2];
    memset(views, 0, sizeof(views));
    XrView views_loc[2];
    memset(views_loc, 0, sizeof(views_loc));
    for (int i = 0; i < 2; i++) views_loc[i].type = XR_TYPE_VIEW;

    if (!fs.shouldRender || !g_session_ready) {
        fei.layerCount = 0;
        fei.layers = NULL;
        xrEndFrame(g_session, &fei);
        return;
    }

    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t nview = 0;
    XrViewLocateInfo vli = {XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = g_local;
    if (XR_FAILED(xrLocateViews(g_session, &vli, &vs, 2, &nview, views_loc)) || nview < 2) {
        fei.layerCount = 0;
        xrEndFrame(g_session, &fei);
        return;
    }

    poll_controllers(fs.predictedDisplayTime, g_local);

    for (uint32_t i = 0; i < nview; i++) {
        swapchain_t *sc = &g_sc[i];
        XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        uint32_t img = 0;
        if (XR_FAILED(xrAcquireSwapchainImage(sc->handle, &ai, &img))) continue;
        XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(sc->handle, &wi);

        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                               sc->imgs[img].image, 0);
        glViewport(0, 0, (GLsizei)sc->w, (GLsizei)sc->h);
        glEnable(GL_DEPTH_TEST);
        glClearColor(0.04f, 0.05f, 0.08f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

        mat4_t proj = mat_projection(views_loc[i].fov, 0.05f, 100.f);
        mat4_t view = mat_view(views_loc[i].pose);
        mat4_t vp = mat_mul(proj, view);
        draw_monitors(vp);

        /* One-shot debug readback of the rendered eye buffer, so the layout can
           be MEASURED from what the GPU actually drew instead of inferred from
           the transform math. Triggered by /sdcard/vrdesk.dump; writes
           /sdcard/vrdesk-eye<i>.ppm and logs the quad's pixel bounding box. */
        if (i == 0) dump_eye_once(sc->w, sc->h);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glDeleteFramebuffers(1, &fbo);

        XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(sc->handle, &ri);

        views[i].type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
        views[i].pose = views_loc[i].pose;
        views[i].fov = views_loc[i].fov;
        views[i].subImage.swapchain = sc->handle;
        views[i].subImage.imageRect.offset.x = 0;
        views[i].subImage.imageRect.offset.y = 0;
        views[i].subImage.imageRect.extent.width = sc->w;
        views[i].subImage.imageRect.extent.height = sc->h;
        views[i].subImage.imageArrayIndex = 0;
    }

    layer.space = g_local;
    layer.viewCount = nview;
    layer.views = views;
    const XrCompositionLayerBaseHeader *layers[1] = {(const XrCompositionLayerBaseHeader *)&layer};
    fei.layerCount = 1;
    fei.layers = layers;
    xrEndFrame(g_session, &fei);
    (void)timeout;
}

/* ------------------------------------------------------------ app glue */

static int swapchain_create(void) {
    uint32_t nfmt = 0;
    xrEnumerateSwapchainFormats(g_session, 0, &nfmt, NULL);
    if (!nfmt) { LOGE("no swapchain formats"); return 0; }
    int64_t *fmts = calloc(nfmt, sizeof(int64_t));
    xrEnumerateSwapchainFormats(g_session, nfmt, &nfmt, fmts);
    int64_t chosen = fmts[0];
    for (uint32_t i = 0; i < nfmt; i++) if (fmts[i] == 0x8C43 /* GL_SRGB8_ALPHA8 */) chosen = fmts[i];
    free(fmts);

    XrViewConfigurationView vcv[2];
    memset(vcv, 0, sizeof(vcv));
    for (int i = 0; i < 2; i++) vcv[i].type = XR_TYPE_VIEW_CONFIGURATION_VIEW;
    uint32_t nv = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(g_instance, g_system,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv)) || nv < 2) {
        LOGE("enumerate view config views failed");
        return 0;
    }
    g_sc_count = 2;
    for (uint32_t i = 0; i < 2; i++) {
        swapchain_t *sc = &g_sc[i];
        sc->w = (int32_t)vcv[i].recommendedImageRectWidth;
        sc->h = (int32_t)vcv[i].recommendedImageRectHeight;
        XrSwapchainCreateInfo sci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        sci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        sci.format = chosen;
        sci.sampleCount = 1;
        sci.width = sc->w;
        sci.height = sc->h;
        sci.faceCount = 1;
        sci.arraySize = 1;
        sci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(g_session, &sci, &sc->handle))) {
            LOGE("xrCreateSwapchain %u failed", i);
            return 0;
        }
        xrEnumerateSwapchainImages(sc->handle, 0, &sc->n, NULL);
        sc->imgs = calloc(sc->n, sizeof(XrSwapchainImageOpenGLESKHR));
        for (uint32_t k = 0; k < sc->n; k++) sc->imgs[k].type = XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR;
        xrEnumerateSwapchainImages(sc->handle, sc->n, &sc->n,
                                   (XrSwapchainImageBaseHeader *)sc->imgs);
        LOGI("swapchain %u: %dx%d, %u images, format %lld", i, sc->w, sc->h, sc->n,
             (long long)chosen);
    }
    return 1;
}

static void app_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        LOGI("APP_CMD_INIT_WINDOW");
        /* Guard on the session, not the instance: Horizon OS cycles
           INIT_WINDOW/TERM_WINDOW while the volumetric window is placed, and
           after a TERM_WINDOW the instance still exists but the session does
           not. Guarding on g_instance skipped setup on every re-init and left
           the app with no session at all. */
        if (g_session == XR_NULL_HANDLE) {
            if (g_instance == XR_NULL_HANDLE && !xr_init(app)) {
                LOGE("xr_init failed"); return;
            }
            if (!egl_init()) { LOGE("egl_init failed"); return; }
            if (!session_create()) { LOGE("session_create failed"); return; }
            if (!swapchain_create()) { LOGE("swapchain_create failed"); return; }
            if (!g_net_running) {
                pthread_t t;
                g_net_running = 1;
                pthread_create(&t, NULL, net_thread, NULL);
                pthread_detach(t);
            }
            LOGI("window ready: session live");
        }
        break;
    case APP_CMD_TERM_WINDOW:
        LOGI("APP_CMD_TERM_WINDOW");
        /* The surface is gone but the OpenXR session is still valid; keep it
           (and the swapchains) so the next INIT_WINDOW can render again.
           Only the EGL surface is tied to the window. */
        break;
    case APP_CMD_DESTROY:
        g_quit = 1;
        break;
    default:
        break;
    }
}

static int32_t app_input(struct android_app *app, AInputEvent *ev) {
    (void)app; (void)ev;
    return 0;
}

void android_main(struct android_app *app) {
    app->onAppCmd = app_cmd;
    app->onInputEvent = app_input;

    /* allow overriding the bridge address via a file pushed with adb */
    FILE *f = fopen("/sdcard/vrdesk.conf", "r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char *nl = strchr(line, '\n');
            if (nl) *nl = 0;
            if (!strncmp(line, "host=", 5)) snprintf(g_host, sizeof(g_host), "%s", line + 5);
            else if (!strncmp(line, "port=", 5)) g_port = atoi(line + 5);
        }
        fclose(f);
    }
    LOGI("bridge target %s:%d", g_host, g_port);

    for (;;) {
        int events = 0;
        struct android_poll_source *src = NULL;
        /* Never block indefinitely: once the session exists we must keep
           polling xrPollEvent to see XR_SESSION_STATE_READY. Blocking with -1
           while g_session_ready is 0 (the state before READY!) deadlocked the
           state machine: the session was created but never began. */
        int timeout_ms = (g_session != XR_NULL_HANDLE) ? 0 : -1;
        while (ALooper_pollOnce(timeout_ms, NULL, &events, (void **)&src) >= 0) {
            if (src) src->process(app, src);
            if (app->destroyRequested) { g_quit = 1; return; }
            if (g_session != XR_NULL_HANDLE) break;
        }
        if (app->destroyRequested) { g_quit = 1; return; }
        if (g_session == XR_NULL_HANDLE) continue;

        /* Drain events BEFORE rendering, and only render once the session has
           actually begun. Previously render_frame() ran unconditionally, so
           xrWaitFrame() was called on a session that was never begun: the
           runtime logged "session is not running" tens of thousands of times
           and the state machine never got a chance to see READY. */
        XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
        while (xrPollEvent(g_instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                XrEventDataSessionStateChanged *sc = (XrEventDataSessionStateChanged *)&ev;
                g_state = sc->state;
                LOGI("session state -> %d", (int)g_state);
                if (g_state == XR_SESSION_STATE_READY) {
                    XrSessionBeginInfo sbi = {XR_TYPE_SESSION_BEGIN_INFO};
                    sbi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(g_session, &sbi))) g_session_ready = 1;
                } else if (g_state == XR_SESSION_STATE_STOPPING) {
                    g_session_ready = 0;
                    xrEndSession(g_session);
                } else if (g_state == XR_SESSION_STATE_EXITING ||
                           g_state == XR_SESSION_STATE_LOSS_PENDING) {
                    g_quit = 1;
                    return;
                }
            }
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
            ev.next = NULL;
        }

        if (g_session_ready) {
            render_frame(0, 0);
        } else {
            /* Session exists but has not begun yet: yield instead of spinning
               so the runtime can deliver the state-changed event. */
            usleep(2000);
        }
    }
}
