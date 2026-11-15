// Adapted from Bloodborne-Android 2e472653.
/* libisspresent.so: the app's end of the Bionic game's frame path (tools/android/present/
 * iss_present_proto.h). Listens on a unix socket, receives AHardwareBuffers and frames from the
 * game process and shows them with an ASurfaceControl child of the SurfaceView, scaled to fit.
 *
 * minSdk is 28: ASurfaceControl / ASurfaceTransaction (API 29) are resolved with dlsym; without
 * them frames are dropped (released at once) and start() says so. All state is behind one mutex.
 * The receive thread owns the socket; the display callbacks (any thread) answer RELEASE.
 *
 * Two presentation modes, chosen by the game in HELLO (ISS_PRESENT_FLAG_FIFO; user-selected VSync) and changed by its MODE message while it runs (set_fifo):
 *
 *  - Newest wins ("VSync off", the default): present_frame() applies a transaction per FRAME at once.
 *    SurfaceFlinger latches at most one buffer per refresh, so frames that arrive within one refresh
 *    replace each other: the newest is shown, the others never are (their buffers come back through
 *    on_complete as the "replaced" buffer). No tearing, lowest latency.
 *
 *  - FIFO ("VSync on"): at most ONE transaction is outstanding. A FRAME that arrives while the previous
 *    transaction is outstanding waits in a queue; when that one is done, the next queued frame is
 *    applied. So each frame is on screen for at least one refresh, in order, and with the display
 *    slower than the game the game is paced by the refresh rate: the
 *    queue is bounded (a frame in it is a buffer the game does not get back, and the game holds one
 *    to render, so with `count` buffers the app holds at most count of them: the one on the display,
 *    the outstanding one and count - 2 queued; the array has room for ISS_PRESENT_MAX_BUFFERS).
 *
 * "Done" means LATCHED, not presented: ASurfaceTransaction_setOnComplete fires after the frame has been
 * presented (its present fence), which is already past the latch deadline of the next refresh, so
 * gating the next transaction on it would show one frame every 2nd refresh (30 FPS on 60 Hz).
 * ASurfaceTransaction_setOnCommit (API 31) fires when SurfaceFlinger latches the transaction, early
 * in the refresh before the one that shows it; the next transaction applied right then is latched for
 * the following refresh. So with setOnCommit (resolved with dlsym) the commit callback clears the
 * outstanding transaction and the complete callback only answers RELEASE, as in the other mode. On
 * API 29/30 there is no setOnCommit: on_complete does both, and FIFO pacing may halve the rate there
 * (logged once).
 *
 * Who applies the next queued transaction: the receive thread, never a callback. The callbacks run on
 * a binder thread inside libgui's transaction-callback dispatch; whether applying another transaction
 * (which registers callbacks with the same TransactionCompletedListener) from inside one is safe
 * depends on its locking, which the NDK does not promise. So the callback only marks the transaction
 * done and wakes the receive thread through an eventfd; the receive thread polls the socket and the
 * eventfd, and applies from there (under the mutex like everything else). Cost: one thread wake-up
 * (well under a millisecond, far below the refresh interval).
 *
 * Callback ownership: each transaction has a DoneCtx (on_complete, frees it) and, in FIFO mode with
 * setOnCommit, a CommitCtx of its own (on_commit, frees it): each callback fires exactly once per
 * applied transaction, so each context has one owner and nothing is shared or counted. Both carry the
 * session and the transaction id; a callback of an older session or transaction does not touch
 * S.inflight.
 */
#include "geometry.h"
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/surface_control.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <dlfcn.h>
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "iss_present_proto.h"

#define TAG "IssPresent"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* API 29 entry points, from libandroid.so. */
static struct {
    ASurfaceControl* (*create_from_window)(ANativeWindow*, const char*);
    void (*release)(ASurfaceControl*);
    ASurfaceTransaction* (*txn_create)(void);
    void (*txn_delete)(ASurfaceTransaction*);
    void (*txn_apply)(ASurfaceTransaction*);
    void (*set_buffer)(ASurfaceTransaction*, ASurfaceControl*, AHardwareBuffer*, int);
    void (*set_geometry)(ASurfaceTransaction*, ASurfaceControl*, const ARect*, const ARect*, int32_t);
    void (*set_visibility)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    void (*set_transparency)(ASurfaceTransaction*, ASurfaceControl*, int8_t);
    void (*set_on_complete)(ASurfaceTransaction*, void*, void (*)(void*, ASurfaceTransactionStats*));
    void (*set_on_commit)(ASurfaceTransaction*, void*, void (*)(void*, ASurfaceTransactionStats*)); /* API 31, optional */
    int (*prev_release_fence)(ASurfaceTransactionStats*, ASurfaceControl*);
    void (*get_controls)(ASurfaceTransactionStats*, ASurfaceControl***, size_t*);
    void (*release_controls)(ASurfaceControl**);
    int64_t (*get_latch_time)(ASurfaceTransactionStats*);
    void (*set_frame_rate)(ASurfaceTransaction*, ASurfaceControl*, float, int8_t); /* API 30, optional */
    int ok;
} api;

static void load_api(void) {
    void* lib = dlopen("libandroid.so", RTLD_NOW);
    if (!lib) return;
    *(void**)&api.set_frame_rate = dlsym(lib, "ASurfaceTransaction_setFrameRate");
    *(void**)&api.set_on_commit = dlsym(lib, "ASurfaceTransaction_setOnCommit"); /* optional */
#define SYM(field, name) if (!(*(void**)&api.field = dlsym(lib, name))) return
    SYM(create_from_window, "ASurfaceControl_createFromWindow");
    SYM(release, "ASurfaceControl_release");
    SYM(txn_create, "ASurfaceTransaction_create");
    SYM(txn_delete, "ASurfaceTransaction_delete");
    SYM(txn_apply, "ASurfaceTransaction_apply");
    SYM(set_buffer, "ASurfaceTransaction_setBuffer");
    SYM(set_geometry, "ASurfaceTransaction_setGeometry");
    SYM(set_visibility, "ASurfaceTransaction_setVisibility");
    SYM(set_transparency, "ASurfaceTransaction_setBufferTransparency");
    SYM(set_on_complete, "ASurfaceTransaction_setOnComplete");
    SYM(prev_release_fence, "ASurfaceTransactionStats_getPreviousReleaseFenceFd");
    SYM(get_controls, "ASurfaceTransactionStats_getASurfaceControls");
    SYM(release_controls, "ASurfaceTransactionStats_releaseASurfaceControls");
#undef SYM
    *(void**)&api.get_latch_time = dlsym(lib, "ASurfaceTransactionStats_getLatchTime");
    api.ok = 1;
}

typedef struct {
    uint32_t generation, index, width, height;
    uint64_t frame;
    int fence; /* acquire fence, owned here; -1 none */
} Queued;

static struct {
    pthread_mutex_t lock;
    int started;
    int fullscreen;
    float frame_rate;
    int server_fd;
    char path[108];
    pthread_t thread;
    /* Display. */
    ANativeWindow* window;
    ASurfaceControl* sc;
    int last_set; /* buffer index of the newest transaction, -1 none */
    /* FIFO mode (the connection's HELLO flags): frames wait in queue[] while a transaction is
     * outstanding. inflight is the id (txn_seq) of the newest transaction not yet completed, 0 none;
     * on_complete of an older one (after a surface loss, say) does not clear it. */
    int fifo;
    Queued queue[ISS_PRESENT_MAX_BUFFERS];
    uint32_t queue_head, queue_len;
    uint64_t txn_seq, inflight;
    int wake_fd; /* eventfd: on_complete -> the receive thread (apply the next queued frame) */
    /* Connection (session ids tell the display callbacks which connection they belong to). */
    int conn_fd;
    uint32_t session;
    uint32_t generation;
    uint32_t count;
    AHardwareBuffer* bufs[ISS_PRESENT_MAX_BUFFERS];
    int trace_indices[ISS_PRESENT_MAX_BUFFERS];
    uint64_t trace_frames[ISS_PRESENT_MAX_BUFFERS];
    atomic_ullong frames;
} S = {.lock = PTHREAD_MUTEX_INITIALIZER, .server_fd = -1, .conn_fd = -1, .last_set = -1, .wake_fd = -1};

/* Requested capture only; S.lock serializes apply/complete file writes. */
static int64_t trace_clock_ns(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000LL + t.tv_nsec;
}
static void trace_event(int index, const char* format, ...) {
    if (index < 0 || index >= 120) return;
    const char* slash = strrchr(S.path, '/'); if (!slash) return;
    char path[512];
    snprintf(path, sizeof(path), "%.*s/fsr-capture/f%03d_android_info.txt", (int)(slash-S.path), S.path, index);
    FILE* f = fopen(path, "ab"); if (!f) return;
    va_list args; va_start(args, format); vfprintf(f, format, args); va_end(args);
    fputc('\n', f); fclose(f);
}

/* ---- lock held for everything below unless noted ---- */

static void send_release(uint32_t generation, uint32_t index, uint64_t frame, int fence) {
    iss_present_msg m = iss_present_make(ISS_PRESENT_RELEASE);
    m.generation = generation;
    m.index = index;
    m.frame = frame;
    if (S.conn_fd >= 0) {
        int r = iss_present_send(S.conn_fd, &m, fence);
        if (r != 0) LOGE("release send failed: %d", r);
    }
    if (fence >= 0) close(fence);
}

/* Empties the FIFO queue. release: answer RELEASE for each frame with its own acquire fence (the
 * game's writes are what its buffer still waits for), as for a frame dropped for want of a display;
 * else the buffers are gone (RESET, new HELLO, connection closed): only the fences are closed. */
static void clear_queue(int release) {
    while (S.queue_len) {
        Queued* q = &S.queue[S.queue_head];
        if (release) send_release(q->generation, q->index, q->frame, q->fence);
        else if (q->fence >= 0) close(q->fence);
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
    }
    S.queue_head = 0;
}

static void drop_buffers(void) {
    clear_queue(0);
    S.inflight = 0;
    for (uint32_t i = 0; i < ISS_PRESENT_MAX_BUFFERS; ++i) {
        if (S.bufs[i]) AHardwareBuffer_release(S.bufs[i]);
        S.bufs[i] = NULL;
    }
    S.last_set = -1;
}

static void drop_surface_control(void) {
    /* No display: queued frames are dropped like any frame that arrives without one. A transaction
     * still outstanding on the old control is forgotten (its on_complete changes nothing). */
    clear_queue(1);
    S.inflight = 0;
    if (S.sc) {
        /* The newest buffer is on the display until the control goes: its release has no fence. */
        if (S.last_set >= 0 && S.conn_fd >= 0)
            send_release(S.generation, (uint32_t)S.last_set, 0, -1);
        S.last_set = -1;
        api.release(S.sc);
        S.sc = NULL;
    }
}

static void apply_frame_rate(void) {
    if(!api.ok || !api.set_frame_rate || !S.sc)return;
    ASurfaceTransaction* t=api.txn_create();
    api.set_frame_rate(t,S.sc,S.frame_rate,0); /* DEFAULT compatibility: game, not fixed-rate video */
    api.txn_apply(t);api.txn_delete(t);
    LOGI("display: child surface requests %.1f Hz",S.frame_rate);
}

static int egl_is_forced(void);

static void make_surface_control(void) {
    if (!S.window || S.sc || egl_is_forced()) return;
    if (!api.ok) {
        LOGI("ASurfaceControl unavailable; using zero-copy EGL AHardwareBuffer presenter");
        return;
    }
    S.sc = api.create_from_window(S.window, "isspresent");
    if (!S.sc) LOGI("ASurfaceControl_createFromWindow returned NULL; falling back to EGL AHardwareBuffer presenter");
    else { LOGI("surface control created"); apply_frame_rate(); }
}

typedef struct {
    uint32_t session, generation, index; /* the buffer the transaction replaced; index == ~0: none */
    uint64_t frame;
    uint64_t seq; /* the transaction's id (S.txn_seq), compared with S.inflight */
    int trace_index;
    uint64_t trace_frame;
    int gates;    /* FIFO without a commit callback: this on_complete ends the outstanding state */
} DoneCtx;

typedef struct {
    uint32_t session;
    uint64_t seq;
} CommitCtx;

static void wake_receiver(void) {
    uint64_t one = 1;
    if (S.wake_fd >= 0 && write(S.wake_fd, &one, sizeof one) < 0 && errno != EAGAIN)
        LOGE("wake failed: %s", strerror(errno));
}

static void on_complete(void* ctx_, ASurfaceTransactionStats* stats) {
    DoneCtx* ctx = ctx_;
    int fence = -1;
    ASurfaceControl** controls = NULL;
    size_t n = 0;
    api.get_controls(stats, &controls, &n);
    if (n > 0) fence = api.prev_release_fence(stats, controls[0]);
    if (controls) api.release_controls(controls);
    const int64_t latch = ctx->trace_index >= 0 && api.get_latch_time ? api.get_latch_time(stats) : -1;
    pthread_mutex_lock(&S.lock);
    if (ctx->trace_index >= 0 && ctx->session == S.session)
        trace_event(ctx->trace_index, "event=complete frame=%llu txn=%llu latch_ns=%lld completed_ns=%lld previous_buffer=%u release_fence=%d",
                    (unsigned long long)ctx->trace_frame, (unsigned long long)ctx->seq,
                    (long long)latch, (long long)trace_clock_ns(), ctx->index, fence >= 0);
    if (ctx->index != ~0u && ctx->session == S.session && S.conn_fd >= 0) {
        send_release(ctx->generation, ctx->index, ctx->frame, fence);
    } else if (fence >= 0) {
        close(fence);
    }
    /* FIFO: this transaction is on the display, the next queued frame may go (applied by the
     * receive thread, see the top of the file). */
    if (ctx->gates && S.fifo && ctx->session == S.session && ctx->seq == S.inflight) {
        S.inflight = 0;
        if (S.queue_len) wake_receiver();
    }
    pthread_mutex_unlock(&S.lock);
    free(ctx);
}

/* FIFO, API 31: SurfaceFlinger latched the transaction; the next queued frame may go. */
static void on_commit(void* ctx_, ASurfaceTransactionStats* stats) {
    (void)stats;
    CommitCtx* ctx = ctx_;
    pthread_mutex_lock(&S.lock);
    if (S.fifo && ctx->session == S.session && ctx->seq == S.inflight) {
        S.inflight = 0;
        if (S.queue_len) wake_receiver();
    }
    pthread_mutex_unlock(&S.lock);
    free(ctx);
}

/* Hands one frame to SurfaceFlinger: a transaction with the buffer, the fit-to-window geometry and
 * the acquire fence (taken). Needs S.sc and the API; the frame's buffer is checked by the callers. */
static void show_frame(uint32_t generation, uint32_t index, uint64_t frame, uint32_t width, uint32_t height,
                       int acquire_fd) {
    int w = ANativeWindow_getWidth(S.window), h = ANativeWindow_getHeight(S.window);
    if (w <= 0 || h <= 0) { w = (int)width; h = (int)height; }
    int dw, dh;
    gow3_display_size(w, h, width, height, S.fullscreen, &dw, &dh);
    ARect src = {0, 0, (int32_t)width, (int32_t)height};
    ARect dst = {(w - dw) / 2, (h - dh) / 2, (w - dw) / 2 + dw, (h - dh) / 2 + dh};
    const int trace_index = S.trace_frames[index] == frame ? S.trace_indices[index] : -1;
    DoneCtx* ctx = malloc(sizeof *ctx);
    if (!ctx) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=context_allocation time_ns=%lld",
            (unsigned long long)frame, (long long)trace_clock_ns());
        send_release(generation, index, frame, acquire_fd); return;
    }
    ctx->session = S.session;
    ctx->generation = S.generation;
    ctx->index = S.last_set >= 0 ? (uint32_t)S.last_set : ~0u; /* replaced by this transaction */
    ctx->frame = 0;
    ctx->seq = ++S.txn_seq;
    ctx->gates = 0;
    ctx->trace_index = trace_index;
    ctx->trace_frame = frame;
    if (ctx->trace_index >= 0) trace_event(ctx->trace_index, "event=apply frame=%llu txn=%llu generation=%u buffer=%u applied_ns=%lld source=%d,%d,%d,%d destination=%d,%d,%d,%d window=%d,%d fifo=%d acquire_fence=%d",
                (unsigned long long)frame, (unsigned long long)ctx->seq, generation, index,
                (long long)trace_clock_ns(), src.left, src.top, src.right, src.bottom,
                dst.left, dst.top, dst.right, dst.bottom, w, h, S.fifo, acquire_fd >= 0);
    CommitCtx* commit = NULL;
    if (S.fifo) {
        if (api.set_on_commit && (commit = malloc(sizeof *commit))) {
            commit->session = ctx->session;
            commit->seq = ctx->seq;
        } else {
            ctx->gates = 1; /* no commit callback: the complete one gates (API 29/30, or no memory) */
        }
    }
    ASurfaceTransaction* t = api.txn_create();
    api.set_buffer(t, S.sc, S.bufs[index], acquire_fd); /* takes the fd */
    api.set_geometry(t, S.sc, &src, &dst, 0);
    api.set_transparency(t, S.sc, ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE);
    api.set_visibility(t, S.sc, ASURFACE_TRANSACTION_VISIBILITY_SHOW);
    api.set_on_complete(t, ctx, on_complete);
    if (commit) api.set_on_commit(t, commit, on_commit);
    api.txn_apply(t);
    api.txn_delete(t);
    S.last_set = (int)index;
    if (S.fifo) S.inflight = ctx->seq;
}

/* FIFO: applies the oldest queued frame when no transaction is outstanding. Called from the receive
 * thread only (a new frame, or the wake-up from on_complete). */
static void pump_queue(void) {
    while (S.fifo && S.queue_len && !S.inflight) {
        Queued q = S.queue[S.queue_head];
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
        if (!S.sc || !api.ok || q.generation != S.generation || q.index >= S.count || !S.bufs[q.index]) {
            send_release(q.generation, q.index, q.frame, q.fence); /* cannot be shown: give it back */
            continue;
        }
        show_frame(q.generation, q.index, q.frame, q.width, q.height, q.fence); /* sets S.inflight: the loop ends */
    }
}

/* Warns once that FIFO may run at half rate (API 29/30: no commit callback). */
static void warn_fifo_without_commit(void) {
    static int warned;
    if (!warned && !api.set_on_commit) {
        warned = 1;
        LOGI("no ASurfaceTransaction_setOnCommit (needs Android 12): FIFO waits for the frame to be "
             "presented before the next, which may halve the frame rate");
    }
}

/* The game's MODE message: FIFO <-> newest wins, between two frames. Lock held.
 *  - FIFO -> newest wins: the newest queued frame is applied now (if it can be shown), every older
 *    queued frame is released unseen with its own acquire fence, as for any frame the display does
 *    not take. The transaction still outstanding stays (its callbacks only answer RELEASE now: they
 *    clear S.inflight only in FIFO mode, and S.inflight is 0 from here).
 *  - newest wins -> FIFO: nothing is queued yet and nothing outstanding counts, so the next frame is
 *    applied at once and the ones after it queue behind its commit. */
static void set_fifo(int fifo) {
    fifo = fifo != 0;
    if (fifo == S.fifo) return;
    S.fifo = fifo;
    S.inflight = 0;
    if (!fifo) {
        Queued newest = {0};
        int have = 0;
        while (S.queue_len) {
            Queued q = S.queue[S.queue_head];
            S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
            S.queue_len--;
            if (have) send_release(newest.generation, newest.index, newest.frame, newest.fence); /* replaced unseen */
            newest = q;
            have = 1;
        }
        S.queue_head = 0;
        if (have) {
            if (!S.sc || !api.ok || newest.generation != S.generation || newest.index >= S.count ||
                !S.bufs[newest.index])
                send_release(newest.generation, newest.index, newest.frame, newest.fence);
            else
                show_frame(newest.generation, newest.index, newest.frame, newest.width, newest.height, newest.fence);
        }
    } else {
        warn_fifo_without_commit();
    }
    LOGI("presentation: %s", fifo ? "FIFO (VSync on)" : "newest frame (VSync off)");
}

static void present_frame(const iss_present_msg* m, int acquire_fd) {
    const int trace_index = iss_present_trace_index(m);
    if (m->generation != S.generation || m->index >= S.count || !S.bufs[m->index]) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=invalid_buffer time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        if (acquire_fd >= 0) close(acquire_fd);
        return; /* a frame of a set we do not have: nothing to show, nothing to release */
    }
    S.trace_indices[m->index] = trace_index;
    S.trace_frames[m->index] = m->frame;
    if (trace_index >= 0) trace_event(trace_index, "event=receive frame=%llu generation=%u buffer=%u received_ns=%lld",
                (unsigned long long)m->frame, m->generation, m->index, (long long)trace_clock_ns());
    atomic_fetch_add(&S.frames, 1);
    if (!S.sc || !api.ok) {
        /* No display now (surface lost, app paused): the frame is dropped; its own acquire fence
         * is the release (the game's writes are what the buffer still waits for). */
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=no_surface time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        send_release(m->generation, m->index, m->frame, acquire_fd);
        return;
    }
    if (!S.fifo) {
        show_frame(m->generation, m->index, m->frame, m->width, m->height, acquire_fd);
        return;
    }
    if (S.queue_len == ISS_PRESENT_MAX_BUFFERS) {
        /* Cannot happen with a game that waits for RELEASE (it holds at most count <= MAX_BUFFERS
         * buffers); a broken one loses its oldest queued frame instead of growing the queue. */
        LOGE("frame queue full: dropping the oldest");
        Queued* old = &S.queue[S.queue_head];
        send_release(old->generation, old->index, old->frame, old->fence);
        S.queue_head = (S.queue_head + 1) % ISS_PRESENT_MAX_BUFFERS;
        S.queue_len--;
    }
    uint32_t tail = (S.queue_head + S.queue_len) % ISS_PRESENT_MAX_BUFFERS;
    S.queue[tail].generation = m->generation;
    S.queue[tail].index = m->index;
    S.queue[tail].width = m->width;
    S.queue[tail].height = m->height;
    S.queue[tail].frame = m->frame;
    S.queue[tail].fence = acquire_fd;
    S.queue_len++;
    pump_queue();
}

typedef struct {
    EGLImageKHR image;
    uint64_t seq;
} EglSlotImage;

static struct {
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface surf;
    int has_surface;
    int initialized;
    int failed;
    int force_egl;
    int shutdown;
    int surf_w, surf_h;
    GLuint prog;
    GLuint tex;
    GLint a_pos;
    GLint a_uv;
    GLint u_tex;
    EglSlotImage imgs[ISS_PRESENT_MAX_BUFFERS];
    uint64_t buf_seq[ISS_PRESENT_MAX_BUFFERS];
    uint64_t buf_gen;
    uint64_t win_seq;
    uint64_t applied_seq;
    int thread_alive;
    pthread_cond_t win_cond;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC get_native_buf;
    PFNEGLCREATEIMAGEKHRPROC create_image;
    PFNEGLDESTROYIMAGEKHRPROC destroy_image;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC image_target_tex_2d;
    PFNEGLCREATESYNCKHRPROC create_sync;
    PFNEGLDESTROYSYNCKHRPROC destroy_sync;
    PFNEGLWAITSYNCKHRPROC wait_sync;
    PFNEGLDUPNATIVEFENCEFDANDROIDPROC dup_fence_fd;
    int has_native_fence;
    int has_wait_sync;
} E = {
    .dpy = EGL_NO_DISPLAY,
    .ctx = EGL_NO_CONTEXT,
    .surf = EGL_NO_SURFACE,
    .a_pos = -1,
    .a_uv = -1,
    .u_tex = -1,
    .win_cond = PTHREAD_COND_INITIALIZER,
};

static int egl_is_forced(void) {
    return E.force_egl;
}

static GLuint egl_compile_shader(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    if (!sh) return 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char logbuf[512];
        glGetShaderInfoLog(sh, (GLsizei)sizeof(logbuf), NULL, logbuf);
        LOGE("GLES2 shader compile failed (%u): %s", (unsigned)type, logbuf);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int egl_init_gl(void) {
    if (E.prog && E.tex) return 1;
    static const char* vs_src =
        "attribute vec2 aPos;\n"
        "attribute vec2 aUv;\n"
        "varying vec2 vUv;\n"
        "void main() {\n"
        "  gl_Position = vec4(aPos, 0.0, 1.0);\n"
        "  vUv = aUv;\n"
        "}\n";
    static const char* fs_src =
        "#extension GL_OES_EGL_image_external : require\n"
        "precision mediump float;\n"
        "varying vec2 vUv;\n"
        "uniform samplerExternalOES uTex;\n"
        "void main() {\n"
        "  gl_FragColor = texture2D(uTex, vUv);\n"
        "}\n";
    GLuint vs = egl_compile_shader(GL_VERTEX_SHADER, vs_src);
    GLuint fs = egl_compile_shader(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return 0;
    }
    GLuint prog = glCreateProgram();
    if (!prog) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
        char logbuf[512];
        glGetProgramInfoLog(prog, (GLsizei)sizeof(logbuf), NULL, logbuf);
        LOGE("GLES2 program link failed: %s", logbuf);
        glDeleteProgram(prog);
        return 0;
    }
    GLuint tex = 0;
    glGenTextures(1, &tex);
    if (!tex) {
        glDeleteProgram(prog);
        return 0;
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, tex);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    E.prog = prog;
    E.tex = tex;
    E.a_pos = glGetAttribLocation(prog, "aPos");
    E.a_uv = glGetAttribLocation(prog, "aUv");
    E.u_tex = glGetUniformLocation(prog, "uTex");
    return E.a_pos >= 0 && E.a_uv >= 0 && E.u_tex >= 0;
}

static int egl_ensure_display(void) {
    if (E.initialized) return 1;
    if (E.failed) return 0;

    E.dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (E.dpy == EGL_NO_DISPLAY || !eglInitialize(E.dpy, NULL, NULL)) {
        LOGE("EGL initialization failed (0x%x)", eglGetError());
        E.failed = 1;
        return 0;
    }

    static const EGLint cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_NONE
    };
    EGLint num_cfg = 0;
    if (!eglChooseConfig(E.dpy, cfg_attrs, &E.cfg, 1, &num_cfg) || num_cfg < 1) {
        LOGE("eglChooseConfig failed (0x%x)", eglGetError());
        E.failed = 1;
        return 0;
    }

    static const EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, 2,
        EGL_NONE
    };
    E.ctx = eglCreateContext(E.dpy, E.cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (E.ctx == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext failed (0x%x)", eglGetError());
        E.failed = 1;
        return 0;
    }

    *(void**)&E.get_native_buf = (void*)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    *(void**)&E.create_image = (void*)eglGetProcAddress("eglCreateImageKHR");
    *(void**)&E.destroy_image = (void*)eglGetProcAddress("eglDestroyImageKHR");
    *(void**)&E.image_target_tex_2d = (void*)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    *(void**)&E.create_sync = (void*)eglGetProcAddress("eglCreateSyncKHR");
    *(void**)&E.destroy_sync = (void*)eglGetProcAddress("eglDestroySyncKHR");
    *(void**)&E.wait_sync = (void*)eglGetProcAddress("eglWaitSyncKHR");
    *(void**)&E.dup_fence_fd = (void*)eglGetProcAddress("eglDupNativeFenceFDANDROID");

    if (!E.get_native_buf || !E.create_image || !E.destroy_image || !E.image_target_tex_2d) {
        LOGE("Missing required EGL/GLES2 AHardwareBuffer extensions");
        E.failed = 1;
        return 0;
    }

    const char* exts = eglQueryString(E.dpy, EGL_EXTENSIONS);
    E.has_native_fence = exts && strstr(exts, "EGL_ANDROID_native_fence_sync") &&
        E.create_sync && E.destroy_sync && E.dup_fence_fd;
    E.has_wait_sync = E.has_native_fence && strstr(exts, "EGL_KHR_wait_sync") && E.wait_sync;

    E.initialized = 1;
    LOGI("EGL AHB presenter initialized (native_fence=%d wait_sync=%d)", E.has_native_fence, E.has_wait_sync);
    return 1;
}

static void egl_clear_images(void) {
    if (E.dpy != EGL_NO_DISPLAY && E.destroy_image) {
        for (uint32_t i = 0; i < ISS_PRESENT_MAX_BUFFERS; ++i) {
            if (E.imgs[i].image != EGL_NO_IMAGE_KHR) {
                E.destroy_image(E.dpy, E.imgs[i].image);
                E.imgs[i].image = EGL_NO_IMAGE_KHR;
            }
            E.imgs[i].seq = 0;
        }
    }
}

static void egl_detach_surface_locked(void) {
    if (E.dpy != EGL_NO_DISPLAY) {
        if (eglGetCurrentContext() == E.ctx && E.tex) {
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);
        }
        egl_clear_images();
        eglMakeCurrent(E.dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (E.surf != EGL_NO_SURFACE) {
            eglDestroySurface(E.dpy, E.surf);
            E.surf = EGL_NO_SURFACE;
        }
    }
    E.has_surface = 0;
    E.surf_w = 0;
    E.surf_h = 0;
}

static void egl_sync_window_locked(void) {
    egl_detach_surface_locked();
    if (S.window && !S.sc && !E.shutdown) {
        if (egl_ensure_display()) {
            static const EGLint surf_attrs[] = { EGL_NONE };
            E.surf = eglCreateWindowSurface(E.dpy, E.cfg, S.window, surf_attrs);
            if (E.surf == EGL_NO_SURFACE) {
                LOGE("eglCreateWindowSurface failed (0x%x)", eglGetError());
            } else if (!eglMakeCurrent(E.dpy, E.surf, E.surf, E.ctx)) {
                LOGE("eglMakeCurrent failed (0x%x)", eglGetError());
                eglDestroySurface(E.dpy, E.surf);
                E.surf = EGL_NO_SURFACE;
            } else {
                eglSwapInterval(E.dpy, 0);
                if (!egl_init_gl()) {
                    LOGE("GLES2 init failed");
                    egl_detach_surface_locked();
                    E.failed = 1;
                } else {
                    eglQuerySurface(E.dpy, E.surf, EGL_WIDTH, &E.surf_w);
                    eglQuerySurface(E.dpy, E.surf, EGL_HEIGHT, &E.surf_h);
                    E.has_surface = 1;
                    LOGI("EGL AHB presenter surface attached (%dx%d)", E.surf_w, E.surf_h);
                }
            }
        }
    }
    E.applied_seq = E.win_seq;
    pthread_cond_broadcast(&E.win_cond);
}

static void egl_shutdown_locked(void) {
    if (E.dpy != EGL_NO_DISPLAY) {
        if (E.surf != EGL_NO_SURFACE && eglMakeCurrent(E.dpy, E.surf, E.surf, E.ctx)) {
            if (E.tex) { glDeleteTextures(1, &E.tex); E.tex = 0; }
            if (E.prog) { glDeleteProgram(E.prog); E.prog = 0; }
        }
        egl_detach_surface_locked();
        if (E.ctx != EGL_NO_CONTEXT) {
            eglDestroyContext(E.dpy, E.ctx);
            E.ctx = EGL_NO_CONTEXT;
        }
        eglTerminate(E.dpy);
        E.dpy = EGL_NO_DISPLAY;
    }
    E.tex = 0;
    E.prog = 0;
    E.a_pos = -1;
    E.a_uv = -1;
    E.u_tex = -1;
    E.initialized = 0;
    E.failed = 0;
    E.has_surface = 0;
    E.surf_w = 0;
    E.surf_h = 0;
}

static EGLImageKHR egl_get_image(uint32_t index) {
    uint64_t seq = E.buf_seq[index];
    if (seq && E.imgs[index].image != EGL_NO_IMAGE_KHR && E.imgs[index].seq == seq) {
        return E.imgs[index].image;
    }
    if (E.imgs[index].image != EGL_NO_IMAGE_KHR) {
        E.destroy_image(E.dpy, E.imgs[index].image);
        E.imgs[index].image = EGL_NO_IMAGE_KHR;
        E.imgs[index].seq = 0;
    }
    EGLClientBuffer cbuf = E.get_native_buf(S.bufs[index]);
    if (!cbuf) {
        LOGE("eglGetNativeClientBufferANDROID failed for slot %u", index);
        return EGL_NO_IMAGE_KHR;
    }
    static const EGLint attrs[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR img = E.create_image(E.dpy, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cbuf, attrs);
    if (img == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR failed for slot %u (0x%x)", index, eglGetError());
        return EGL_NO_IMAGE_KHR;
    }
    E.imgs[index].image = img;
    E.imgs[index].seq = seq;
    return img;
}

static void egl_present_frame(const iss_present_msg* m, int acquire_fd) {
    const int trace_index = iss_present_trace_index(m);
    if (m->generation != S.generation || m->index >= S.count || !S.bufs[m->index]) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=invalid_buffer time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        if (acquire_fd >= 0) close(acquire_fd);
        return;
    }
    S.trace_indices[m->index] = trace_index;
    S.trace_frames[m->index] = m->frame;
    if (trace_index >= 0) trace_event(trace_index, "event=receive frame=%llu generation=%u buffer=%u received_ns=%lld",
                (unsigned long long)m->frame, m->generation, m->index, (long long)trace_clock_ns());
    atomic_fetch_add(&S.frames, 1);

    if (E.applied_seq != E.win_seq) {
        egl_sync_window_locked();
    }
    if (!S.window || !E.has_surface || E.surf == EGL_NO_SURFACE || E.failed) {
        if (trace_index >= 0) trace_event(trace_index, "event=drop frame=%llu reason=no_surface time_ns=%lld",
                    (unsigned long long)m->frame, (long long)trace_clock_ns());
        send_release(m->generation, m->index, m->frame, acquire_fd);
        return;
    }

    if (eglGetCurrentContext() != E.ctx || eglGetCurrentSurface(EGL_DRAW) != E.surf) {
        if (!eglMakeCurrent(E.dpy, E.surf, E.surf, E.ctx)) {
            LOGE("eglMakeCurrent failed during draw (0x%x)", eglGetError());
            send_release(m->generation, m->index, m->frame, acquire_fd);
            return;
        }
    }

    if (acquire_fd >= 0) {
        if (E.has_wait_sync) {
            EGLint sync_attrs[] = { EGL_SYNC_NATIVE_FENCE_FD_ANDROID, acquire_fd, EGL_NONE };
            EGLSyncKHR acq_sync = E.create_sync(E.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, sync_attrs);
            if (acq_sync != EGL_NO_SYNC_KHR) {
                acquire_fd = -1; /* EGL took ownership of acquire_fd */
                E.wait_sync(E.dpy, acq_sync, 0);
                E.destroy_sync(E.dpy, acq_sync);
            } else {
                struct pollfd pfd = { acquire_fd, POLLIN, 0 };
                poll(&pfd, 1, 1000);
                close(acquire_fd);
                acquire_fd = -1;
            }
        } else {
            struct pollfd pfd = { acquire_fd, POLLIN, 0 };
            poll(&pfd, 1, 1000);
            close(acquire_fd);
            acquire_fd = -1;
        }
    }

    EGLImageKHR img = egl_get_image(m->index);
    if (img == EGL_NO_IMAGE_KHR) {
        send_release(m->generation, m->index, m->frame, -1);
        return;
    }

    int w = E.surf_w, h = E.surf_h;
    if (w <= 0 || h <= 0) {
        eglQuerySurface(E.dpy, E.surf, EGL_WIDTH, &w);
        eglQuerySurface(E.dpy, E.surf, EGL_HEIGHT, &h);
        if (w <= 0 || h <= 0) {
            w = ANativeWindow_getWidth(S.window);
            h = ANativeWindow_getHeight(S.window);
        }
        if (w <= 0 || h <= 0) { w = (int)m->width; h = (int)m->height; }
        E.surf_w = w;
        E.surf_h = h;
    }
    int dw = w, dh = h;
    gow3_display_size(w, h, m->width, m->height, S.fullscreen, &dw, &dh);
    if (dw < w || dh < h) {
        glViewport(0, 0, w, h);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
    }
    int vx = (w - dw) / 2;
    int vy = (h - dh) / 2;
    glViewport(vx, vy, dw, dh);

    if (trace_index >= 0) trace_event(trace_index, "event=apply frame=%llu txn=%llu generation=%u buffer=%u applied_ns=%lld source=0,0,%u,%u destination=%d,%d,%d,%d window=%d,%d fifo=0 acquire_fence=egl",
                (unsigned long long)m->frame, (unsigned long long)(++S.txn_seq), m->generation, m->index,
                (long long)trace_clock_ns(), m->width, m->height, vx, vy, vx + dw, vy + dh, w, h);

    /* AHardwareBuffer has top-left origin while GL/EGL samples from bottom-left:
     * map top NDC vertices (y = +1) to v = 1.0 and bottom (y = -1) to v = 0.0. */
    static const GLfloat kQuad[16] = {
        -1.f,  1.f, 0.f, 1.f,
         1.f,  1.f, 1.f, 1.f,
        -1.f, -1.f, 0.f, 0.f,
         1.f, -1.f, 1.f, 0.f,
    };
    glUseProgram(E.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, E.tex);
    E.image_target_tex_2d(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)img);
    glUniform1i(E.u_tex, 0);
    glVertexAttribPointer((GLuint)E.a_pos, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat), kQuad);
    glVertexAttribPointer((GLuint)E.a_uv, 2, GL_FLOAT, GL_FALSE, 4 * (GLsizei)sizeof(GLfloat), kQuad + 2);
    glEnableVertexAttribArray((GLuint)E.a_pos);
    glEnableVertexAttribArray((GLuint)E.a_uv);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    int release_fd = -1;
    if (E.has_native_fence) {
        EGLSyncKHR rel_sync = E.create_sync(E.dpy, EGL_SYNC_NATIVE_FENCE_ANDROID, NULL);
        glFlush();
        if (rel_sync != EGL_NO_SYNC_KHR) {
            release_fd = E.dup_fence_fd(E.dpy, rel_sync);
            E.destroy_sync(E.dpy, rel_sync);
            if (release_fd == EGL_NO_NATIVE_FENCE_FD_ANDROID) release_fd = -1;
        }
    } else {
        glFinish();
    }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    if (!eglSwapBuffers(E.dpy, E.surf)) {
        EGLint err = eglGetError();
        LOGE("eglSwapBuffers failed (0x%x)", err);
        if (err == EGL_CONTEXT_LOST) {
            egl_shutdown_locked();
        } else if (err == EGL_BAD_SURFACE || err == EGL_BAD_NATIVE_WINDOW) {
            egl_detach_surface_locked();
        }
    }

    if (trace_index >= 0) trace_event(trace_index, "event=complete frame=%llu txn=%llu latch_ns=-1 completed_ns=%lld previous_buffer=%u release_fence=%d",
                (unsigned long long)m->frame, (unsigned long long)S.txn_seq,
                (long long)trace_clock_ns(), m->index, release_fd >= 0);

    send_release(m->generation, m->index, m->frame, release_fd);
}

static void close_connection(void) {
    if (S.conn_fd >= 0) { close(S.conn_fd); S.conn_fd = -1; }
    S.session++;
    S.last_set = -1;
    egl_clear_images();
    E.buf_gen++;
    drop_buffers(); /* also the FIFO queue (no connection to release to) and the outstanding id */
    S.count = 0;
    S.fifo = 0;
    /* The control stays: the next game connection shows on the same surface. */
}

/* Serves one connection until it ends. Called without the lock. */
static void serve(int fd) {
    pthread_mutex_lock(&S.lock);
    S.conn_fd = fd;
    S.session++;
    S.generation = 0;
    S.count = 0;
    S.fifo = 0;
    S.inflight = 0;
    if (E.applied_seq != E.win_seq) egl_sync_window_locked();
    pthread_mutex_unlock(&S.lock);
    LOGI("game connected");
    for (;;) {
        /* Block until the game sends something, or on_complete/window sync asks us to wake up. */
        struct pollfd pfds[2] = {{fd, POLLIN, 0}, {S.wake_fd, POLLIN, 0}};
        if (poll(pfds, 2, -1) < 0) {
            if (errno == EINTR) continue;
            LOGE("poll failed: %s", strerror(errno));
            break;
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t n;
            if (read(S.wake_fd, &n, sizeof n) < 0 && errno != EAGAIN) LOGE("wake read failed: %s", strerror(errno));
            pthread_mutex_lock(&S.lock);
            if (E.applied_seq != E.win_seq) egl_sync_window_locked();
            int closing = E.shutdown;
            pump_queue();
            pthread_mutex_unlock(&S.lock);
            if (closing) break;
        }
        if (!(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;
        iss_present_msg m;
        int fence = -1;
        int r = iss_present_recv(fd, &m, &fence, 0); /* no lock: data (or the close) is there */
        if (r != 0) {
            if (r != -ECONNRESET) LOGE("receive failed: %d", r);
            break;
        }
        pthread_mutex_lock(&S.lock);
        int stop = E.shutdown;
        if (!stop) switch (m.type) {
        case ISS_PRESENT_HELLO: {
            if (m.version != ISS_PRESENT_VERSION || m.count == 0 || m.count > ISS_PRESENT_MAX_BUFFERS) {
                LOGE("bad HELLO: version %u (ours %u) count %u: the game and the app are not the same build",
                     m.version, ISS_PRESENT_VERSION, m.count);
                stop = 1;
                break;
            }
            egl_clear_images();
            E.buf_gen++;
            drop_buffers(); /* clears the FIFO queue too */
            S.generation = m.generation;
            S.count = m.count;
            S.fifo = (m.flags & ISS_PRESENT_FLAG_FIFO) != 0;
            LOGI("presentation: %s", S.fifo ? "FIFO (VSync on)" : "newest frame (VSync off)");
            if (S.fifo) warn_fifo_without_commit();
            iss_present_msg a = iss_present_make(ISS_PRESENT_HELLO);
            a.generation = m.generation;
            a.flags = m.flags & ISS_PRESENT_FLAGS_KNOWN; /* what the app will do */
            stop = iss_present_send(fd, &a, -1) != 0;
            break;
        }
        case ISS_PRESENT_RESET:
            egl_clear_images();
            E.buf_gen++;
            drop_buffers(); /* clears the FIFO queue too: its frames belong to the old set */
            S.generation = m.generation;
            S.count = m.count <= ISS_PRESENT_MAX_BUFFERS ? m.count : 0;
            break;
        case ISS_PRESENT_BUFFER: {
            AHardwareBuffer* b = NULL;
            /* The buffer is the very next datagram. */
            pthread_mutex_unlock(&S.lock);
            int rr = AHardwareBuffer_recvHandleFromUnixSocket(fd, &b);
            pthread_mutex_lock(&S.lock);
            if (rr != 0 || !b) { LOGE("receiving buffer %u failed: %d", m.index, rr); stop = 1; break; }
            if (m.generation == S.generation && m.index < S.count) {
                if (S.bufs[m.index]) AHardwareBuffer_release(S.bufs[m.index]);
                S.bufs[m.index] = b;
                E.buf_seq[m.index] = ++E.buf_gen;
            } else {
                AHardwareBuffer_release(b);
            }
            break;
        }
        case ISS_PRESENT_MODE:
            set_fifo((m.flags & ISS_PRESENT_FLAG_FIFO) != 0);
            break;
        case ISS_PRESENT_FRAME:
            if (!S.sc && S.window) {
                egl_present_frame(&m, fence);
            } else {
                present_frame(&m, fence);
            }
            fence = -1;
            break;
        case ISS_PRESENT_BYE:
            stop = 1;
            break;
        default:
            break;
        }
        pthread_mutex_unlock(&S.lock);
        if (fence >= 0) close(fence);
        if (stop) break;
    }
    pthread_mutex_lock(&S.lock);
    close_connection();
    pthread_mutex_unlock(&S.lock);
    LOGI("game disconnected");
}

static void* server_thread(void* unused) {
    (void)unused;
    for (;;) {
        int sfd, wfd;
        pthread_mutex_lock(&S.lock);
        if (E.applied_seq != E.win_seq) egl_sync_window_locked();
        if (E.shutdown || S.server_fd < 0) {
            egl_shutdown_locked();
            E.shutdown = 0;
            E.thread_alive = 0;
            pthread_cond_broadcast(&E.win_cond);
            pthread_mutex_unlock(&S.lock);
            return NULL;
        }
        sfd = S.server_fd;
        wfd = S.wake_fd;
        pthread_mutex_unlock(&S.lock);

        struct pollfd pfds[2] = {{sfd, POLLIN, 0}, {wfd, POLLIN, 0}};
        int pr = poll(pfds, 2, -1);
        if (pr < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (pfds[1].revents & POLLIN) {
            uint64_t n;
            if (read(wfd, &n, sizeof n) < 0 && errno != EAGAIN) LOGE("wake read failed: %s", strerror(errno));
            continue;
        }
        if (!(pfds[0].revents & (POLLIN | POLLHUP | POLLERR))) continue;

        int fd = accept4(sfd, NULL, NULL, SOCK_CLOEXEC);
        if (fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            pthread_mutex_lock(&S.lock);
            int closing = E.shutdown || S.server_fd < 0;
            pthread_mutex_unlock(&S.lock);
            if (closing) break;
            break;
        }
        serve(fd);
    }
    pthread_mutex_lock(&S.lock);
    egl_shutdown_locked();
    E.shutdown = 0;
    E.thread_alive = 0;
    pthread_cond_broadcast(&E.win_cond);
    pthread_mutex_unlock(&S.lock);
    return NULL;
}

/* ---- JNI ---- */

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetFullscreen(
        JNIEnv* env, jclass cls, jboolean enabled) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.lock);
    S.fullscreen = enabled != 0;
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetForceEgl(
        JNIEnv* env, jclass cls, jboolean enabled) {
    (void)env; (void)cls;
    pthread_mutex_lock(&S.lock);
    E.force_egl = enabled != 0;
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetFrameRate(JNIEnv* env,jclass cls,jfloat rate) {
    (void)env;(void)cls;
    if(!(rate>=0.f && rate<=1000.f))return;
    pthread_mutex_lock(&S.lock);
    if(S.frame_rate!=rate){S.frame_rate=rate;apply_frame_rate();}
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT jstring JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeStart(JNIEnv* env, jclass cls,
        jstring jpath) {
    (void)cls;
    const char* path = (*env)->GetStringUTFChars(env, jpath, NULL);
    char msg[160];
    pthread_mutex_lock(&S.lock);
    if (!S.started) {
        load_api();
        if (S.wake_fd < 0) /* kept for the life of the process: the receive thread may still poll it */
            S.wake_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (S.wake_fd < 0) {
            snprintf(msg, sizeof msg, "eventfd failed: %s", strerror(errno));
            goto fail;
        }
        if (strlen(path) >= sizeof S.path) {
            snprintf(msg, sizeof msg, "socket path too long");
            goto fail;
        }
        int fd = socket(AF_UNIX, SOCK_SEQPACKET | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        struct sockaddr_un a;
        memset(&a, 0, sizeof a);
        a.sun_family = AF_UNIX;
        strcpy(a.sun_path, path);
        unlink(path);
        if (fd < 0 || bind(fd, (struct sockaddr*)&a, sizeof a) != 0 || listen(fd, 1) != 0) {
            snprintf(msg, sizeof msg, "cannot listen on %s: %s", path, strerror(errno));
            if (fd >= 0) close(fd);
            goto fail;
        }
        chmod(path, 0600);
        strcpy(S.path, path);
        S.server_fd = fd;
        S.started = 1;
        E.shutdown = 0;
        E.thread_alive = 1;
        if (pthread_create(&S.thread, NULL, server_thread, NULL) != 0) {
            E.thread_alive = 0;
            S.started = 0;
            close(fd);
            S.server_fd = -1;
            snprintf(msg, sizeof msg, "pthread_create failed");
            goto fail;
        }
        LOGI("listening on %s, ASurfaceControl %s, mode %s", path,
             api.ok ? "available" : "NOT available (Android 10+)",
             E.force_egl ? "egl-forced" : "auto");
    }
    pthread_mutex_unlock(&S.lock);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (*env)->NewStringUTF(env, "present running");
fail:
    pthread_mutex_unlock(&S.lock);
    (*env)->ReleaseStringUTFChars(env, jpath, path);
    return (*env)->NewStringUTF(env, msg);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeSetSurface(JNIEnv* env, jclass cls,
        jobject surface) {
    (void)cls;
    pthread_mutex_lock(&S.lock);
    drop_surface_control();
    if (S.window) { ANativeWindow_release(S.window); S.window = NULL; }
    if (surface) {
        S.window = ANativeWindow_fromSurface(env, surface);
        make_surface_control();
    }
    E.win_seq++;
    if (E.thread_alive) {
        wake_receiver();
        if (!surface && E.has_surface) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_nsec += 500000000L;
            if (ts.tv_nsec >= 1000000000L) {
                ts.tv_sec += 1;
                ts.tv_nsec -= 1000000000L;
            }
            while (E.has_surface && E.thread_alive && !E.shutdown) {
                if (pthread_cond_timedwait(&E.win_cond, &S.lock, &ts) == ETIMEDOUT)
                    break;
            }
        }
    }
    pthread_mutex_unlock(&S.lock);
}

JNIEXPORT jlong JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeFrames(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    return (jlong)atomic_load(&S.frames);
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssPresent_nativeShutdown(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    pthread_t th = 0;
    int join = 0;
    int close_sfd = -1;
    pthread_mutex_lock(&S.lock);
    drop_surface_control();
    if (S.window) { ANativeWindow_release(S.window); S.window = NULL; }
    E.win_seq++;
    if (S.started) {
        if (S.server_fd >= 0) {
            shutdown(S.server_fd, SHUT_RDWR);
            close_sfd = S.server_fd;
            S.server_fd = -1;
        }
        unlink(S.path);
        if (S.conn_fd >= 0) shutdown(S.conn_fd, SHUT_RDWR);
        S.started = 0;
    }
    if (E.thread_alive) {
        E.shutdown = 1;
        wake_receiver();
        th = S.thread;
        join = 1;
    }
    pthread_mutex_unlock(&S.lock);
    if (join) pthread_join(th, NULL);
    if (close_sfd >= 0) close(close_sfd);
}
