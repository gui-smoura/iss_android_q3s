/* libissopenxr.so: minimal zero-eye-buffer OpenXR quad wrapper for Quest/HorizonOS.
 * Creates an OpenXR Vulkan/GLES session with ZERO XrCompositionLayerProjection layers
 * (no eye swapchains), exposes an Android Surface backed by xrCreateSwapchainAndroidSurfaceKHR,
 * and submits only one flat XrCompositionLayerQuad in LOCAL space. */
#define _POSIX_C_SOURCE 200809L
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define XR_USE_PLATFORM_ANDROID 1
#define XR_USE_GRAPHICS_API_OPENGL_ES 1
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define TAG "IssOpenXr"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

static struct {
    pthread_mutex_t lock;
    pthread_cond_t init_cond;
    JavaVM* vm;
    jclass cls;
    jmethodID on_state;
    jobject activity;
    jobject surface;
    pthread_t thread;
    int started;
    int thread_alive;
    int thread_joinable;
    int stop_requested;
    int init_done; /* 0 pending, 1 ready, -1 failed */
    int width;
    int height;
    char log_path[512];
    char last_error[512];
    atomic_int session_state;
    atomic_ullong xr_frames;
} X = {
    .lock = PTHREAD_MUTEX_INITIALIZER,
    .init_cond = PTHREAD_COND_INITIALIZER,
    .session_state = XR_SESSION_STATE_UNKNOWN,
};

static void xr_log(int is_err, const char* fmt, ...) {
    char msg[512];
    va_list args;
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);

    if (is_err) LOGE("%s", msg);
    else LOGI("%s", msg);

    char path[512];
    pthread_mutex_lock(&X.lock);
    snprintf(path, sizeof(path), "%s", X.log_path);
    if (is_err) snprintf(X.last_error, sizeof(X.last_error), "%s", msg);
    pthread_mutex_unlock(&X.lock);

    if (path[0]) {
        FILE* f = fopen(path, "ab");
        if (f) {
            fprintf(f, "[OpenXR] %s\n", msg);
            fclose(f);
        }
    }
}

static void notify_state(JNIEnv* jenv, int state) {
    if (!jenv) return;
    jclass cls;
    jmethodID mid;
    pthread_mutex_lock(&X.lock);
    cls = X.cls;
    mid = X.on_state;
    pthread_mutex_unlock(&X.lock);
    if (cls && mid) {
        (*jenv)->CallStaticVoidMethod(jenv, cls, mid, (jint)state);
        if ((*jenv)->ExceptionCheck(jenv)) {
            (*jenv)->ExceptionDescribe(jenv);
            (*jenv)->ExceptionClear(jenv);
        }
    }
}

typedef struct {
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLContext ctx;
    EGLSurface pbuf;
    int major;
    int minor;
} XrEglCtx;

static int create_pbuffer_egl(XrEglCtx* g) {
    memset(g, 0, sizeof(*g));
    g->dpy = EGL_NO_DISPLAY;
    g->ctx = EGL_NO_CONTEXT;
    g->pbuf = EGL_NO_SURFACE;

    g->dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g->dpy == EGL_NO_DISPLAY || !eglInitialize(g->dpy, NULL, NULL)) {
        xr_log(1, "eglGetDisplay/eglInitialize failed (0x%x)", eglGetError());
        return 0;
    }

    static const EGLint es3_cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };
    static const EGLint es2_cfg_attrs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 0,
        EGL_STENCIL_SIZE, 0,
        EGL_NONE
    };

    EGLint num_cfg = 0;
    int client_ver = 3;
    if (!eglChooseConfig(g->dpy, es3_cfg_attrs, &g->cfg, 1, &num_cfg) || num_cfg < 1) {
        client_ver = 2;
        if (!eglChooseConfig(g->dpy, es2_cfg_attrs, &g->cfg, 1, &num_cfg) || num_cfg < 1) {
            xr_log(1, "eglChooseConfig failed (0x%x)", eglGetError());
            return 0;
        }
    }

    EGLint ctx_attrs[] = {
        EGL_CONTEXT_CLIENT_VERSION, client_ver,
        EGL_NONE
    };
    g->ctx = eglCreateContext(g->dpy, g->cfg, EGL_NO_CONTEXT, ctx_attrs);
    if (g->ctx == EGL_NO_CONTEXT && client_ver == 3) {
        client_ver = 2;
        ctx_attrs[1] = 2;
        g->ctx = eglCreateContext(g->dpy, g->cfg, EGL_NO_CONTEXT, ctx_attrs);
    }
    if (g->ctx == EGL_NO_CONTEXT) {
        xr_log(1, "eglCreateContext failed (0x%x)", eglGetError());
        return 0;
    }

    static const EGLint pbuf_attrs[] = {
        EGL_WIDTH, 1,
        EGL_HEIGHT, 1,
        EGL_NONE
    };
    g->pbuf = eglCreatePbufferSurface(g->dpy, g->cfg, pbuf_attrs);
    if (g->pbuf == EGL_NO_SURFACE) {
        xr_log(1, "eglCreatePbufferSurface failed (0x%x)", eglGetError());
        return 0;
    }

    if (!eglMakeCurrent(g->dpy, g->pbuf, g->pbuf, g->ctx)) {
        xr_log(1, "eglMakeCurrent on OpenXR pbuffer failed (0x%x)", eglGetError());
        return 0;
    }

    g->major = client_ver;
    g->minor = 0;
    return 1;
}

static void destroy_pbuffer_egl(XrEglCtx* g) {
    if (g->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(g->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g->pbuf != EGL_NO_SURFACE) eglDestroySurface(g->dpy, g->pbuf);
        if (g->ctx != EGL_NO_CONTEXT) eglDestroyContext(g->dpy, g->ctx);
        eglTerminate(g->dpy);
    }
    memset(g, 0, sizeof(*g));
    g->dpy = EGL_NO_DISPLAY;
    g->ctx = EGL_NO_CONTEXT;
    g->pbuf = EGL_NO_SURFACE;
}

static void* openxr_thread(void* unused) {
    (void)unused;
    JNIEnv* jenv = NULL;
    int attached = 0;
    XrEglCtx egl = { .dpy = EGL_NO_DISPLAY, .ctx = EGL_NO_CONTEXT, .pbuf = EGL_NO_SURFACE };
    XrInstance instance = XR_NULL_HANDLE;
    XrSession session = XR_NULL_HANDLE;
    XrSpace local_space = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    jobject local_surface = NULL;
    int session_running = 0;

    pthread_mutex_lock(&X.lock);
    JavaVM* vm = X.vm;
    jobject activity = X.activity;
    int quad_w = X.width > 0 ? X.width : 1280;
    int quad_h = X.height > 0 ? X.height : 720;
    pthread_mutex_unlock(&X.lock);

    if (!vm || (*vm)->AttachCurrentThread(vm, &jenv, NULL) != JNI_OK || !jenv) {
        xr_log(1, "AttachCurrentThread failed");
        goto fail;
    }
    attached = 1;

    if (!create_pbuffer_egl(&egl)) {
        goto fail;
    }

    PFN_xrVoidFunction pfn_raw = NULL;
    PFN_xrInitializeLoaderKHR pfnInitLoader = NULL;
    XrResult res = xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", &pfn_raw);
    pfnInitLoader = (PFN_xrInitializeLoaderKHR)pfn_raw;
    if (XR_FAILED(res) || !pfnInitLoader) {
        xr_log(1, "xrGetInstanceProcAddr(xrInitializeLoaderKHR) failed (%d)", (int)res);
        goto fail;
    }
    XrLoaderInitInfoAndroidKHR loader_info = {
        .type = XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR,
        .next = NULL,
        .applicationVM = vm,
        .applicationContext = activity,
    };
    res = pfnInitLoader((const XrLoaderInitInfoBaseHeaderKHR*)&loader_info);
    if (XR_FAILED(res)) {
        xr_log(1, "xrInitializeLoaderKHR failed (%d)", (int)res);
        goto fail;
    }

    uint32_t prop_count = 0;
    res = xrEnumerateInstanceExtensionProperties(NULL, 0, &prop_count, NULL);
    if (XR_FAILED(res) || prop_count == 0) {
        xr_log(1, "xrEnumerateInstanceExtensionProperties count failed (%d)", (int)res);
        goto fail;
    }
    XrExtensionProperties* props = calloc(prop_count, sizeof(*props));
    if (!props) {
        xr_log(1, "Out of memory enumerating OpenXR extensions");
        goto fail;
    }
    for (uint32_t i = 0; i < prop_count; ++i) {
        props[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    }
    res = xrEnumerateInstanceExtensionProperties(NULL, prop_count, &prop_count, props);
    if (XR_FAILED(res)) {
        free(props);
        xr_log(1, "xrEnumerateInstanceExtensionProperties failed (%d)", (int)res);
        goto fail;
    }

    int has_android_inst = 0, has_gles = 0, has_android_surf = 0, has_fb_surf = 0;
    for (uint32_t i = 0; i < prop_count; ++i) {
        if (strcmp(props[i].extensionName, XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME) == 0)
            has_android_inst = 1;
        else if (strcmp(props[i].extensionName, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME) == 0)
            has_gles = 1;
        else if (strcmp(props[i].extensionName, XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME) == 0)
            has_android_surf = 1;
        else if (strcmp(props[i].extensionName, XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME) == 0)
            has_fb_surf = 1;
    }
    free(props);

    if (!has_android_inst || !has_gles || !has_android_surf) {
        xr_log(1, "OpenXR runtime missing required extensions (android=%d gles=%d android_surface=%d)",
               has_android_inst, has_gles, has_android_surf);
        goto fail;
    }

    const char* exts[4];
    uint32_t ext_count = 0;
    exts[ext_count++] = XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME;
    exts[ext_count++] = XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME;
    exts[ext_count++] = XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME;
    if (has_fb_surf) {
        exts[ext_count++] = XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME;
    }

    XrInstanceCreateInfoAndroidKHR android_ci = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR,
        .next = NULL,
        .applicationVM = vm,
        .applicationActivity = activity,
    };
    XrInstanceCreateInfo inst_ci = {
        .type = XR_TYPE_INSTANCE_CREATE_INFO,
        .next = &android_ci,
        .createFlags = 0,
        .applicationInfo = {
            .applicationName = "inFAMOUS Daedalus",
            .applicationVersion = 1,
            .engineName = "iss-bionic",
            .engineVersion = 1,
            .apiVersion = XR_API_VERSION_1_0,
        },
        .enabledApiLayerCount = 0,
        .enabledApiLayerNames = NULL,
        .enabledExtensionCount = ext_count,
        .enabledExtensionNames = exts,
    };
    res = xrCreateInstance(&inst_ci, &instance);
    if (XR_FAILED(res)) {
        xr_log(1, "xrCreateInstance failed (%d)", (int)res);
        goto fail;
    }

    XrSystemGetInfo sys_info = {
        .type = XR_TYPE_SYSTEM_GET_INFO,
        .next = NULL,
        .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY,
    };
    XrSystemId system_id = XR_NULL_SYSTEM_ID;
    res = xrGetSystem(instance, &sys_info, &system_id);
    if (XR_FAILED(res) || system_id == XR_NULL_SYSTEM_ID) {
        xr_log(1, "xrGetSystem(HMD) failed (%d)", (int)res);
        goto fail;
    }

    XrEnvironmentBlendMode blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    uint32_t blend_count = 0;
    if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance, system_id,
            XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &blend_count, NULL)) && blend_count > 0) {
        XrEnvironmentBlendMode modes[8];
        uint32_t cap = blend_count > 8 ? 8 : blend_count;
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(instance, system_id,
                XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, cap, &cap, modes)) && cap > 0) {
            blend_mode = modes[0];
            for (uint32_t i = 0; i < cap; ++i) {
                if (modes[i] == XR_ENVIRONMENT_BLEND_MODE_OPAQUE) {
                    blend_mode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
                    break;
                }
            }
        }
    }

    pfn_raw = NULL;
    res = xrGetInstanceProcAddr(instance, "xrGetOpenGLESGraphicsRequirementsKHR", &pfn_raw);
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfnGetGlesReqs =
        (PFN_xrGetOpenGLESGraphicsRequirementsKHR)pfn_raw;
    if (XR_FAILED(res) || !pfnGetGlesReqs) {
        xr_log(1, "xrGetInstanceProcAddr(xrGetOpenGLESGraphicsRequirementsKHR) failed (%d)", (int)res);
        goto fail;
    }
    XrGraphicsRequirementsOpenGLESKHR gles_reqs = {
        .type = XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR,
        .next = NULL,
    };
    res = pfnGetGlesReqs(instance, system_id, &gles_reqs);
    if (XR_FAILED(res)) {
        xr_log(1, "xrGetOpenGLESGraphicsRequirementsKHR failed (%d)", (int)res);
        goto fail;
    }
    xr_log(0, "GLES requirements min=%u.%u max=%u.%u (using %d.%d)",
           (unsigned)XR_VERSION_MAJOR(gles_reqs.minApiVersionSupported),
           (unsigned)XR_VERSION_MINOR(gles_reqs.minApiVersionSupported),
           (unsigned)XR_VERSION_MAJOR(gles_reqs.maxApiVersionSupported),
           (unsigned)XR_VERSION_MINOR(gles_reqs.maxApiVersionSupported),
           egl.major, egl.minor);

    XrGraphicsBindingOpenGLESAndroidKHR binding = {
        .type = XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR,
        .next = NULL,
        .display = egl.dpy,
        .config = egl.cfg,
        .context = egl.ctx,
    };
    XrSessionCreateInfo sess_ci = {
        .type = XR_TYPE_SESSION_CREATE_INFO,
        .next = &binding,
        .createFlags = 0,
        .systemId = system_id,
    };
    res = xrCreateSession(instance, &sess_ci, &session);
    if (XR_FAILED(res) || session == XR_NULL_HANDLE) {
        xr_log(1, "xrCreateSession failed (%d)", (int)res);
        goto fail;
    }

    XrReferenceSpaceCreateInfo space_ci = {
        .type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
        .next = NULL,
        .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
        .poseInReferenceSpace = {
            .orientation = { 0.f, 0.f, 0.f, 1.f },
            .position = { 0.f, 0.f, 0.f },
        },
    };
    res = xrCreateReferenceSpace(session, &space_ci, &local_space);
    if (XR_FAILED(res) || local_space == XR_NULL_HANDLE) {
        xr_log(1, "xrCreateReferenceSpace(LOCAL) failed (%d)", (int)res);
        goto fail;
    }

    pfn_raw = NULL;
    res = xrGetInstanceProcAddr(instance, "xrCreateSwapchainAndroidSurfaceKHR", &pfn_raw);
    PFN_xrCreateSwapchainAndroidSurfaceKHR pfnCreateAndroidSurface =
        (PFN_xrCreateSwapchainAndroidSurfaceKHR)pfn_raw;
    if (XR_FAILED(res) || !pfnCreateAndroidSurface) {
        xr_log(1, "xrGetInstanceProcAddr(xrCreateSwapchainAndroidSurfaceKHR) failed (%d)", (int)res);
        goto fail;
    }

    XrAndroidSurfaceSwapchainCreateInfoFB fb_surf_ci = {
        .type = XR_TYPE_ANDROID_SURFACE_SWAPCHAIN_CREATE_INFO_FB,
        .next = NULL,
        .createFlags = 0, /* async BufferQueue consumer: never stall producer */
    };
    /* XR_KHR_android_surface_swapchain valid usage requires format, sampleCount,
     * faceCount, arraySize, and mipCount to all be 0. */
    XrSwapchainCreateInfo sc_ci = {
        .type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
        .next = has_fb_surf ? (const void*)&fb_surf_ci : NULL,
        .createFlags = 0,
        .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT,
        .format = 0,
        .sampleCount = 0,
        .width = (uint32_t)quad_w,
        .height = (uint32_t)quad_h,
        .faceCount = 0,
        .arraySize = 0,
        .mipCount = 0,
    };
    res = pfnCreateAndroidSurface(session, &sc_ci, &swapchain, &local_surface);
    if (XR_FAILED(res) || swapchain == XR_NULL_HANDLE || !local_surface) {
        sc_ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        swapchain = XR_NULL_HANDLE;
        local_surface = NULL;
        res = pfnCreateAndroidSurface(session, &sc_ci, &swapchain, &local_surface);
    }
    if (XR_FAILED(res) || swapchain == XR_NULL_HANDLE || !local_surface) {
        xr_log(1, "xrCreateSwapchainAndroidSurfaceKHR failed (%d)", (int)res);
        goto fail;
    }

    xr_log(0, "Session ready: %dx%d quad swapchain (0 eye buffers, fb_create=%d, blend=%d)",
           quad_w, quad_h, has_fb_surf, (int)blend_mode);

    pthread_mutex_lock(&X.lock);
    X.surface = (*jenv)->NewGlobalRef(jenv, local_surface);
    (*jenv)->DeleteLocalRef(jenv, local_surface);
    local_surface = NULL;
    if (!X.surface) {
        X.init_done = -1;
        snprintf(X.last_error, sizeof(X.last_error), "NewGlobalRef(Surface) failed");
        pthread_cond_broadcast(&X.init_cond);
        pthread_mutex_unlock(&X.lock);
        goto cleanup;
    }
    X.init_done = 1;
    pthread_cond_broadcast(&X.init_cond);
    pthread_mutex_unlock(&X.lock);

    int exit_loop = 0;
    for (;;) {
        pthread_mutex_lock(&X.lock);
        int want_stop = X.stop_requested;
        pthread_mutex_unlock(&X.lock);
        if (want_stop) break;

        XrEventDataBuffer ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        while (xrPollEvent(instance, &ev) == XR_SUCCESS) {
            if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
                const XrEventDataSessionStateChanged* ch = (const XrEventDataSessionStateChanged*)&ev;
                int st = (int)ch->state;
                atomic_store(&X.session_state, st);
                xr_log(0, "Session state -> %d", st);

                if (st == XR_SESSION_STATE_READY) {
                    if (!session_running) {
                        notify_state(jenv, st);
                        XrSessionBeginInfo begin_info = {
                            .type = XR_TYPE_SESSION_BEGIN_INFO,
                            .next = NULL,
                            .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                        };
                        res = xrBeginSession(session, &begin_info);
                        if (XR_SUCCEEDED(res)) {
                            session_running = 1;
                        } else {
                            xr_log(1, "xrBeginSession failed (%d)", (int)res);
                            notify_state(jenv, XR_SESSION_STATE_EXITING);
                            exit_loop = 1;
                            break;
                        }
                    }
                } else if (st == XR_SESSION_STATE_SYNCHRONIZED ||
                           st == XR_SESSION_STATE_VISIBLE ||
                           st == XR_SESSION_STATE_FOCUSED) {
                    notify_state(jenv, st);
                } else if (st == XR_SESSION_STATE_STOPPING) {
                    /* XR_KHR_android_surface_swapchain: detach Surface producer synchronously before xrEndSession. */
                    notify_state(jenv, st);
                    if (session_running) {
                        xrEndSession(session);
                        session_running = 0;
                    }
                } else if (st == XR_SESSION_STATE_EXITING || st == XR_SESSION_STATE_LOSS_PENDING) {
                    notify_state(jenv, st);
                    exit_loop = 1;
                    break;
                }
            } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
                notify_state(jenv, XR_SESSION_STATE_LOSS_PENDING);
                exit_loop = 1;
                break;
            }
            memset(&ev, 0, sizeof(ev));
            ev.type = XR_TYPE_EVENT_DATA_BUFFER;
        }
        if (exit_loop) break;

        if (!session_running) {
            struct timespec ts = { 0, 10000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        XrFrameWaitInfo wait_info = {
            .type = XR_TYPE_FRAME_WAIT_INFO,
            .next = NULL,
        };
        XrFrameState frame_state = {
            .type = XR_TYPE_FRAME_STATE,
            .next = NULL,
        };
        res = xrWaitFrame(session, &wait_info, &frame_state);
        if (res == XR_SESSION_LOSS_PENDING) {
            notify_state(jenv, XR_SESSION_STATE_LOSS_PENDING);
            break;
        }
        if (XR_FAILED(res)) {
            xr_log(1, "xrWaitFrame failed (%d)", (int)res);
            struct timespec ts = { 0, 10000000L };
            nanosleep(&ts, NULL);
            continue;
        }

        XrFrameBeginInfo begin_frame = {
            .type = XR_TYPE_FRAME_BEGIN_INFO,
            .next = NULL,
        };
        res = xrBeginFrame(session, &begin_frame);
        if (XR_FAILED(res)) {
            xr_log(1, "xrBeginFrame failed (%d)", (int)res);
            continue;
        }

        XrCompositionLayerQuad quad = {
            .type = XR_TYPE_COMPOSITION_LAYER_QUAD,
            .next = NULL,
            .layerFlags = 0,
            .space = local_space,
            .eyeVisibility = XR_EYE_VISIBILITY_BOTH,
            .subImage = {
                .swapchain = swapchain,
                .imageRect = {
                    .offset = { 0, 0 },
                    .extent = { quad_w, quad_h },
                },
                .imageArrayIndex = 0,
            },
            .pose = {
                .orientation = { 0.f, 0.f, 0.f, 1.f },
                .position = { 0.f, -0.05f, -2.0f },
            },
            .size = { 2.4f, 2.4f * (float)quad_h / (float)(quad_w > 0 ? quad_w : 1) },
        };
        const XrCompositionLayerBaseHeader* layers[1] = {
            (const XrCompositionLayerBaseHeader*)&quad,
        };
        XrFrameEndInfo end_frame = {
            .type = XR_TYPE_FRAME_END_INFO,
            .next = NULL,
            .displayTime = frame_state.predictedDisplayTime,
            .environmentBlendMode = blend_mode,
            .layerCount = frame_state.shouldRender ? 1 : 0,
            .layers = frame_state.shouldRender ? layers : NULL,
        };
        res = xrEndFrame(session, &end_frame);
        if (XR_FAILED(res)) {
            xr_log(1, "xrEndFrame failed (%d)", (int)res);
        } else if (frame_state.shouldRender) {
            atomic_fetch_add(&X.xr_frames, 1);
        }
    }
    goto cleanup;

fail:
    pthread_mutex_lock(&X.lock);
    if (X.init_done == 0) {
        X.init_done = -1;
        pthread_cond_broadcast(&X.init_cond);
    }
    pthread_mutex_unlock(&X.lock);

cleanup:
    if (session_running && session != XR_NULL_HANDLE) {
        if (jenv) notify_state(jenv, XR_SESSION_STATE_STOPPING);
        xrEndSession(session);
        session_running = 0;
    }
    if (local_surface && jenv) {
        (*jenv)->DeleteLocalRef(jenv, local_surface);
        local_surface = NULL;
    }
    if (swapchain != XR_NULL_HANDLE) {
        xrDestroySwapchain(swapchain);
        swapchain = XR_NULL_HANDLE;
    }
    if (local_space != XR_NULL_HANDLE) {
        xrDestroySpace(local_space);
        local_space = XR_NULL_HANDLE;
    }
    if (session != XR_NULL_HANDLE) {
        xrDestroySession(session);
        session = XR_NULL_HANDLE;
    }
    if (instance != XR_NULL_HANDLE) {
        xrDestroyInstance(instance);
        instance = XR_NULL_HANDLE;
    }
    destroy_pbuffer_egl(&egl);

    pthread_mutex_lock(&X.lock);
    if (jenv) {
        if (X.surface) { (*jenv)->DeleteGlobalRef(jenv, X.surface); X.surface = NULL; }
        if (X.activity) { (*jenv)->DeleteGlobalRef(jenv, X.activity); X.activity = NULL; }
        if (X.cls) { (*jenv)->DeleteGlobalRef(jenv, X.cls); X.cls = NULL; }
    }
    X.on_state = NULL;
    X.thread_alive = 0;
    X.started = 0;
    if (X.init_done == 0) {
        X.init_done = -1;
        pthread_cond_broadcast(&X.init_cond);
    }
    pthread_mutex_unlock(&X.lock);

    if (attached && vm) {
        (*vm)->DetachCurrentThread(vm);
    }
    return NULL;
}

/* ---- JNI ---- */

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    (void)reserved;
    pthread_mutex_lock(&X.lock);
    X.vm = vm;
    pthread_mutex_unlock(&X.lock);
    return JNI_VERSION_1_6;
}

JNIEXPORT jstring JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStart(
        JNIEnv* env, jclass cls, jobject activity, jint width, jint height, jstring jlogPath) {
    if (!activity) return (*env)->NewStringUTF(env, "missing Activity");
    const char* log_path = jlogPath ? (*env)->GetStringUTFChars(env, jlogPath, NULL) : NULL;

    pthread_t stale_th = 0;
    int join_stale = 0;
    pthread_mutex_lock(&X.lock);
    if (X.started || X.thread_alive) {
        pthread_mutex_unlock(&X.lock);
        if (jlogPath && log_path) (*env)->ReleaseStringUTFChars(env, jlogPath, log_path);
        return (*env)->NewStringUTF(env, "openxr running");
    }
    if (X.thread_joinable) {
        stale_th = X.thread;
        join_stale = 1;
        X.thread_joinable = 0;
    }
    pthread_mutex_unlock(&X.lock);
    if (join_stale) pthread_join(stale_th, NULL);

    pthread_mutex_lock(&X.lock);
    if (X.started || X.thread_alive) {
        pthread_mutex_unlock(&X.lock);
        if (jlogPath && log_path) (*env)->ReleaseStringUTFChars(env, jlogPath, log_path);
        return (*env)->NewStringUTF(env, "openxr running");
    }

    if (X.vm == NULL) {
        (*env)->GetJavaVM(env, &X.vm);
    }
    X.cls = (*env)->NewGlobalRef(env, cls);
    X.on_state = X.cls ? (*env)->GetStaticMethodID(env, X.cls, "onNativeSessionState", "(I)V") : NULL;
    if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    X.activity = (*env)->NewGlobalRef(env, activity);
    X.surface = NULL;
    X.width = width > 0 ? (int)width : 1280;
    X.height = height > 0 ? (int)height : 720;
    X.init_done = 0;
    X.stop_requested = 0;
    X.last_error[0] = '\0';
    atomic_store(&X.session_state, XR_SESSION_STATE_UNKNOWN);
    atomic_store(&X.xr_frames, 0);
    if (log_path) snprintf(X.log_path, sizeof(X.log_path), "%s", log_path);
    else X.log_path[0] = '\0';

    if (jlogPath && log_path) (*env)->ReleaseStringUTFChars(env, jlogPath, log_path);

    if (!X.cls || !X.on_state || !X.activity) {
        if (X.surface) { (*env)->DeleteGlobalRef(env, X.surface); X.surface = NULL; }
        if (X.activity) { (*env)->DeleteGlobalRef(env, X.activity); X.activity = NULL; }
        if (X.cls) { (*env)->DeleteGlobalRef(env, X.cls); X.cls = NULL; }
        X.on_state = NULL;
        pthread_mutex_unlock(&X.lock);
        return (*env)->NewStringUTF(env, "JNI setup failed for IssOpenXr");
    }

    X.started = 1;
    X.thread_alive = 1;
    X.thread_joinable = 1;
    if (pthread_create(&X.thread, NULL, openxr_thread, NULL) != 0) {
        X.started = 0;
        X.thread_alive = 0;
        X.thread_joinable = 0;
        (*env)->DeleteGlobalRef(env, X.activity); X.activity = NULL;
        (*env)->DeleteGlobalRef(env, X.cls); X.cls = NULL;
        X.on_state = NULL;
        pthread_mutex_unlock(&X.lock);
        return (*env)->NewStringUTF(env, "pthread_create failed for OpenXR");
    }

    while (X.init_done == 0 && X.thread_alive) {
        pthread_cond_wait(&X.init_cond, &X.lock);
    }
    int ready = (X.init_done == 1 && X.surface != NULL);
    char err_copy[512];
    snprintf(err_copy, sizeof(err_copy), "%s", X.last_error[0] ? X.last_error : "OpenXR init failed");
    pthread_t fail_th = X.thread;
    int need_join = !ready && X.thread_joinable;
    if (need_join) X.thread_joinable = 0;
    pthread_mutex_unlock(&X.lock);

    if (!ready) {
        if (need_join) pthread_join(fail_th, NULL);
        return (*env)->NewStringUTF(env, err_copy);
    }
    return (*env)->NewStringUTF(env, "openxr running");
}

JNIEXPORT jobject JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeGetSurface(JNIEnv* env, jclass cls) {
    (void)cls;
    pthread_mutex_lock(&X.lock);
    jobject surf = X.surface ? (*env)->NewLocalRef(env, X.surface) : NULL;
    pthread_mutex_unlock(&X.lock);
    return surf;
}

JNIEXPORT void JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStop(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    pthread_t th = 0;
    int join = 0;
    pthread_mutex_lock(&X.lock);
    if (X.thread_joinable) {
        X.stop_requested = 1;
        th = X.thread;
        join = 1;
        X.thread_joinable = 0;
    }
    pthread_mutex_unlock(&X.lock);
    if (join) pthread_join(th, NULL);
}

JNIEXPORT jlong JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeFrames(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    return (jlong)atomic_load(&X.xr_frames);
}

JNIEXPORT jint JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeSessionState(JNIEnv* env, jclass cls) {
    (void)env; (void)cls;
    return (jint)atomic_load(&X.session_state);
}

JNIEXPORT jstring JNICALL Java_ro_cobrabm_fexdroid_IssOpenXr_nativeLastError(JNIEnv* env, jclass cls) {
    (void)cls;
    char copy[512];
    pthread_mutex_lock(&X.lock);
    snprintf(copy, sizeof(copy), "%s", X.last_error);
    pthread_mutex_unlock(&X.lock);
    return (*env)->NewStringUTF(env, copy);
}
