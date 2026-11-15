"""Verify opt-in zero-eye-buffer OpenXR virtual screen integration and native C/Java contracts."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[3]
ANDROID_NS = "{http://schemas.android.com/apk/res/android}"
CC = shutil.which("cc")
JAVAC = shutil.which("javac")


class BionicOpenXrTests(unittest.TestCase):
    def test_manifest_preserves_identity_and_declares_vr_activity(self):
        manifest_path = ROOT / "android/app/BionicAndroidManifest.xml"
        root = ET.parse(manifest_path).getroot()
        self.assertEqual(root.attrib.get("package"), "com.daedalus.iss.bionic")
        self.assertEqual(root.attrib.get(ANDROID_NS + "versionCode"), "17")
        self.assertEqual(root.attrib.get(ANDROID_NS + "versionName"), "0.2")

        perms = {e.attrib.get(ANDROID_NS + "name") for e in root.findall("uses-permission")}
        self.assertIn("org.khronos.openxr.permission.OPENXR", perms)
        self.assertIn("org.khronos.openxr.permission.OPENXR_SYSTEM", perms)

        features = {
            e.attrib.get(ANDROID_NS + "name"): e.attrib.get(ANDROID_NS + "required")
            for e in root.findall("uses-feature")
        }
        self.assertEqual(features.get("android.hardware.vr.headtracking"), "false")

        queries = root.find("queries")
        self.assertIsNotNone(queries)
        providers = [p.attrib.get(ANDROID_NS + "authorities", "") for p in queries.findall("provider")]
        self.assertTrue(any("org.khronos.openxr.runtime_broker" in p for p in providers))

        app = root.find("application")
        meta = {m.attrib.get(ANDROID_NS + "name"): m.attrib.get(ANDROID_NS + "value") for m in app.findall("meta-data")}
        self.assertEqual(meta.get("com.oculus.vr.focusaware"), "true")
        self.assertIn("quest3s", meta.get("com.oculus.supportedDevices", ""))

        activities = {a.attrib.get(ANDROID_NS + "name"): a for a in app.findall("activity")}
        self.assertIn("ro.cobrabm.fexdroid.BionicMainActivity", activities)
        self.assertIn("ro.cobrabm.fexdroid.BionicGameActivity", activities)
        self.assertIn("ro.cobrabm.fexdroid.BionicVrGameActivity", activities)
        self.assertEqual(activities["ro.cobrabm.fexdroid.BionicGameActivity"].attrib.get(ANDROID_NS + "exported"), "false")

        main_cats = {
            c.attrib.get(ANDROID_NS + "name")
            for c in activities["ro.cobrabm.fexdroid.BionicMainActivity"].findall("intent-filter/category")
        }
        self.assertIn("android.intent.category.LAUNCHER", main_cats)

        vr = activities["ro.cobrabm.fexdroid.BionicVrGameActivity"]
        self.assertEqual(vr.attrib.get(ANDROID_NS + "launchMode"), "singleTask")
        self.assertEqual(vr.attrib.get(ANDROID_NS + "screenOrientation"), "landscape")
        self.assertEqual(vr.attrib.get(ANDROID_NS + "resizeableActivity"), "false")
        self.assertEqual(vr.attrib.get(ANDROID_NS + "excludeFromRecents"), "true")
        self.assertEqual(vr.attrib.get(ANDROID_NS + "exported"), "true")
        vr_cats = {c.attrib.get(ANDROID_NS + "name") for c in vr.findall("intent-filter/category")}
        self.assertIn("com.oculus.intent.category.VR", vr_cats)
        self.assertIn("org.khronos.openxr.intent.category.IMMERSIVE_HMD", vr_cats)

    def test_zero_eye_buffer_quad_constraints_and_state_constants(self):
        native = (ROOT / "tools/android/bionic/openxr/issopenxr.c").read_text()
        native_code = re.sub(r"/\*.*?\*/|//[^\n]*", "", native, flags=re.S)
        self.assertIn("XrCompositionLayerQuad", native_code)
        self.assertIn("xrCreateSwapchainAndroidSurfaceKHR", native_code)
        self.assertIn("XrAndroidSurfaceSwapchainCreateInfoFB", native_code)
        self.assertIn("XR_API_VERSION_1_0", native_code)
        self.assertNotIn("XrCompositionLayerProjection", native_code)
        self.assertNotIn("xrLocateViews", native_code)
        self.assertNotIn("xrEnumerateViewConfigurationViews", native_code)
        for zero_field in (".format = 0", ".sampleCount = 0", ".faceCount = 0", ".arraySize = 0", ".mipCount = 0"):
            self.assertIn(zero_field, native_code)
        self.assertIn("GetObjectRefType", native_code)
        self.assertIn("JNILocalRefType", native_code)
        self.assertEqual(len(re.findall(r"->DeleteLocalRef\(", native_code)), 1)
        for global_field in ("X.activity", "X.cls", "X.surface"):
            self.assertNotRegex(native_code, rf"DeleteLocalRef\([^)]*\b{re.escape(global_field)}\b")
            self.assertRegex(native_code, rf"DeleteGlobalRef\([^)]*\b{re.escape(global_field)}\b")

        java_xr = (ROOT / "android/app/src/ro/cobrabm/fexdroid/IssOpenXr.java").read_text()
        expected_states = {
            "STATE_UNKNOWN": 0,
            "STATE_IDLE": 1,
            "STATE_READY": 2,
            "STATE_SYNCHRONIZED": 3,
            "STATE_VISIBLE": 4,
            "STATE_FOCUSED": 5,
            "STATE_STOPPING": 6,
            "STATE_LOSS_PENDING": 7,
            "STATE_EXITING": 8,
        }
        for name, value in expected_states.items():
            self.assertRegex(java_xr, rf"static final int {name}\s*=\s*{value}\s*;")

        launcher = (ROOT / "android/app/src/ro/cobrabm/fexdroid/BionicMainActivity.java").read_text()
        self.assertIn("vrMode=BionicVrSettings.toggle(this)", launcher)
        self.assertIn("if(BionicVrSettings.enabled(this))", launcher)
        self.assertIn("BionicVrGameActivity.class", launcher)
        self.assertIn("Intent.FLAG_ACTIVITY_NEW_TASK|Intent.FLAG_ACTIVITY_CLEAR_TASK", launcher)

        present = (ROOT / "tools/android/bionic/present/isspresent.c").read_text()
        for sym in ("eglGetNativeClientBufferANDROID", "eglCreateImageKHR", "GL_TEXTURE_EXTERNAL_OES",
                    "eglDupNativeFenceFDANDROID", "E.win_cond", "E.force_egl"):
            self.assertIn(sym, present)

        present_code = re.sub(r"/\*.*?\*/|//[^\n]*", "", present, flags=re.S)
        vs_match = re.search(r'static const char\*\s+vs_src\s*=\s*((?:"[^"]*"\s*)+);', present_code, flags=re.S)
        fs_match = re.search(r'static const char\*\s+fs_src\s*=\s*((?:"[^"]*"\s*)+);', present_code, flags=re.S)
        self.assertIsNotNone(vs_match)
        self.assertIsNotNone(fs_match)
        self.assertIn("vUv = aUv;", vs_match.group(1))
        self.assertIn("texture2D(uTex, vUv);", fs_match.group(1))
        self.assertNotRegex(vs_match.group(1), r"1\.0?\s*-")
        self.assertNotRegex(fs_match.group(1), r"1\.0?\s*-")

        quad_match = re.search(
            r'static const GLfloat kQuad\[16\]\s*=\s*\{(.*?)\};',
            present_code,
            flags=re.S,
        )
        self.assertIsNotNone(quad_match)
        nums = [
            float(tok.rstrip("fF"))
            for tok in re.findall(r'[-+]?(?:\d+\.?\d*|\.\d+)f?', quad_match.group(1))
        ]
        self.assertEqual(len(nums), 16)
        vertices = [tuple(nums[i:i + 4]) for i in range(0, 16, 4)]
        self.assertEqual(
            vertices,
            [
                (-1.0,  1.0, 0.0, 1.0),
                ( 1.0,  1.0, 1.0, 1.0),
                (-1.0, -1.0, 0.0, 0.0),
                ( 1.0, -1.0, 1.0, 0.0),
            ],
        )

        present_java = (ROOT / "android/app/src/ro/cobrabm/fexdroid/IssPresent.java").read_text()
        self.assertIn("start(Surface surface, String socketPath, boolean fullscreen)", present_java)
        self.assertIn("nativeSetForceEgl(true)", present_java)

        deps = (ROOT / "tools/android/bionic/build_deps.sh").read_text()
        native_sh = (ROOT / "tools/android/bionic/build_native_runtime.sh").read_text()
        apk_sh = (ROOT / "tools/android/bionic/build_apk.sh").read_text()
        verify_py = (ROOT / "tools/android/bionic/verify_bionic.py").read_text()
        pkg_py = (ROOT / "tools/android/bionic/package_runtime.py").read_text()
        notice = (ROOT / "tools/android/bionic/NOTICE.txt").read_text()
        self.assertIn("OpenXR-SDK-1.1.65", deps)
        self.assertIn("libopenxr_loader.so", deps)
        self.assertIn("libissopenxr.so", native_sh)
        self.assertIn("libopenxr_loader.so", native_sh)
        self.assertIn("lib/arm64-v8a/libissopenxr.so", apk_sh)
        self.assertIn("lib/arm64-v8a/libopenxr_loader.so", apk_sh)
        self.assertIn("lib/libissopenxr.so", verify_py)
        self.assertIn("lib/libopenxr_loader.so", verify_py)
        self.assertIn("openxr-LICENSE.txt", pkg_py)
        self.assertIn("OpenXR SDK", notice)

    @unittest.skipUnless(CC, "C compiler required")
    def test_native_issopenxr_compiles_warning_free_and_executes_lifecycle(self):
        openxr_h = r'''
#pragma once
#include <stdint.h>
typedef int32_t XrResult;
typedef uint64_t XrFlags64;
typedef uint64_t XrSystemId;
typedef uint64_t XrInstance;
typedef uint64_t XrSession;
typedef uint64_t XrSpace;
typedef uint64_t XrSwapchain;
typedef uint64_t XrVersion;
typedef int64_t XrTime;
typedef int64_t XrDuration;
typedef uint32_t XrBool32;
typedef void (*PFN_xrVoidFunction)(void);
#define XRAPI_ATTR
#define XRAPI_CALL
#define XR_NULL_HANDLE 0
#define XR_NULL_SYSTEM_ID 0
#define XR_SUCCESS 0
#define XR_EVENT_UNAVAILABLE 1
#define XR_SESSION_LOSS_PENDING 3
#define XR_ERROR_RUNTIME_FAILURE -2
#define XR_SUCCEEDED(r) ((r) >= 0)
#define XR_FAILED(r) ((r) < 0)
#define XR_MAKE_VERSION(maj, min, pat) ((((uint64_t)(maj)&0xffffULL)<<48)|(((uint64_t)(min)&0xffffULL)<<32)|((uint64_t)(pat)&0xffffffffULL))
#define XR_API_VERSION_1_0 XR_MAKE_VERSION(1, 0, 0)
#define XR_VERSION_MAJOR(v) ((uint16_t)(((uint64_t)(v)>>48)&0xffffULL))
#define XR_VERSION_MINOR(v) ((uint16_t)(((uint64_t)(v)>>32)&0xffffULL))
#define XR_MAX_APPLICATION_NAME_SIZE 128
#define XR_MAX_ENGINE_NAME_SIZE 128
#define XR_MAX_EXTENSION_NAME_SIZE 128
typedef enum {
    XR_TYPE_UNKNOWN = 0,
    XR_TYPE_INSTANCE_CREATE_INFO = 3,
    XR_TYPE_EXTENSION_PROPERTIES = 2,
    XR_TYPE_SYSTEM_GET_INFO = 4,
    XR_TYPE_SESSION_CREATE_INFO = 8,
    XR_TYPE_SWAPCHAIN_CREATE_INFO = 9,
    XR_TYPE_REFERENCE_SPACE_CREATE_INFO = 37,
    XR_TYPE_SESSION_BEGIN_INFO = 14,
    XR_TYPE_FRAME_WAIT_INFO = 33,
    XR_TYPE_FRAME_STATE = 44,
    XR_TYPE_FRAME_BEGIN_INFO = 46,
    XR_TYPE_FRAME_END_INFO = 12,
    XR_TYPE_COMPOSITION_LAYER_QUAD = 36,
    XR_TYPE_EVENT_DATA_BUFFER = 16,
    XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED = 18,
    XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING = 17,
    XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR = 1000008000,
    XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR = 1000024001,
    XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR = 1000024002,
    XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR = 1000089000,
    XR_TYPE_ANDROID_SURFACE_SWAPCHAIN_CREATE_INFO_FB = 1000070000
} XrStructureType;
typedef enum { XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY = 1 } XrFormFactor;
typedef enum { XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO = 2 } XrViewConfigurationType;
typedef enum { XR_ENVIRONMENT_BLEND_MODE_OPAQUE = 1 } XrEnvironmentBlendMode;
typedef enum { XR_REFERENCE_SPACE_TYPE_LOCAL = 2 } XrReferenceSpaceType;
typedef enum { XR_EYE_VISIBILITY_BOTH = 0 } XrEyeVisibility;
typedef enum {
    XR_SESSION_STATE_UNKNOWN = 0,
    XR_SESSION_STATE_IDLE = 1,
    XR_SESSION_STATE_READY = 2,
    XR_SESSION_STATE_SYNCHRONIZED = 3,
    XR_SESSION_STATE_VISIBLE = 4,
    XR_SESSION_STATE_FOCUSED = 5,
    XR_SESSION_STATE_STOPPING = 6,
    XR_SESSION_STATE_LOSS_PENDING = 7,
    XR_SESSION_STATE_EXITING = 8
} XrSessionState;
#define XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT 0x1
#define XR_SWAPCHAIN_USAGE_SAMPLED_BIT 0x20
typedef struct { float x, y, z, w; } XrQuaternionf;
typedef struct { float x, y, z; } XrVector3f;
typedef struct { XrQuaternionf orientation; XrVector3f position; } XrPosef;
typedef struct { float width, height; } XrExtent2Df;
typedef struct { int32_t x, y; } XrOffset2Di;
typedef struct { int32_t width, height; } XrExtent2Di;
typedef struct { XrOffset2Di offset; XrExtent2Di extent; } XrRect2Di;
typedef struct { XrSwapchain swapchain; XrRect2Di imageRect; uint32_t imageArrayIndex; } XrSwapchainSubImage;
typedef struct { XrStructureType type; const void* next; } XrBaseOutStructure;
typedef struct { XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space; } XrCompositionLayerBaseHeader;
typedef struct {
    XrStructureType type; const void* next; XrFlags64 layerFlags; XrSpace space;
    XrEyeVisibility eyeVisibility; XrSwapchainSubImage subImage; XrPosef pose; XrExtent2Df size;
} XrCompositionLayerQuad;
typedef struct {
    XrStructureType type; void* next; char extensionName[XR_MAX_EXTENSION_NAME_SIZE]; uint32_t extensionVersion;
} XrExtensionProperties;
typedef struct {
    char applicationName[XR_MAX_APPLICATION_NAME_SIZE]; uint32_t applicationVersion;
    char engineName[XR_MAX_ENGINE_NAME_SIZE]; uint32_t engineVersion; XrVersion apiVersion;
} XrApplicationInfo;
typedef struct {
    XrStructureType type; const void* next; XrFlags64 createFlags; XrApplicationInfo applicationInfo;
    uint32_t enabledApiLayerCount; const char* const* enabledApiLayerNames;
    uint32_t enabledExtensionCount; const char* const* enabledExtensionNames;
} XrInstanceCreateInfo;
typedef struct { XrStructureType type; const void* next; XrFormFactor formFactor; } XrSystemGetInfo;
typedef struct { XrStructureType type; const void* next; XrFlags64 createFlags; XrSystemId systemId; } XrSessionCreateInfo;
typedef struct { XrStructureType type; const void* next; XrReferenceSpaceType referenceSpaceType; XrPosef poseInReferenceSpace; } XrReferenceSpaceCreateInfo;
typedef struct {
    XrStructureType type; const void* next; XrFlags64 createFlags; XrFlags64 usageFlags;
    int64_t format; uint32_t sampleCount; uint32_t width; uint32_t height;
    uint32_t faceCount; uint32_t arraySize; uint32_t mipCount;
} XrSwapchainCreateInfo;
typedef struct { XrStructureType type; void* next; uint8_t varying[4000]; } XrEventDataBuffer;
typedef struct { XrStructureType type; const void* next; XrSession session; XrSessionState state; XrTime time; } XrEventDataSessionStateChanged;
typedef struct { XrStructureType type; const void* next; XrViewConfigurationType primaryViewConfigurationType; } XrSessionBeginInfo;
typedef struct { XrStructureType type; const void* next; } XrFrameWaitInfo;
typedef struct { XrStructureType type; void* next; XrTime predictedDisplayTime; XrDuration predictedDisplayPeriod; XrBool32 shouldRender; } XrFrameState;
typedef struct { XrStructureType type; const void* next; } XrFrameBeginInfo;
typedef struct {
    XrStructureType type; const void* next; XrTime displayTime; XrEnvironmentBlendMode environmentBlendMode;
    uint32_t layerCount; const XrCompositionLayerBaseHeader* const* layers;
} XrFrameEndInfo;
XrResult xrGetInstanceProcAddr(XrInstance, const char*, PFN_xrVoidFunction*);
XrResult xrEnumerateInstanceExtensionProperties(const char*, uint32_t, uint32_t*, XrExtensionProperties*);
XrResult xrCreateInstance(const XrInstanceCreateInfo*, XrInstance*);
XrResult xrDestroyInstance(XrInstance);
XrResult xrGetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId*);
XrResult xrEnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType, uint32_t, uint32_t*, XrEnvironmentBlendMode*);
XrResult xrCreateSession(XrInstance, const XrSessionCreateInfo*, XrSession*);
XrResult xrDestroySession(XrSession);
XrResult xrBeginSession(XrSession, const XrSessionBeginInfo*);
XrResult xrEndSession(XrSession);
XrResult xrCreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo*, XrSpace*);
XrResult xrDestroySpace(XrSpace);
XrResult xrDestroySwapchain(XrSwapchain);
XrResult xrPollEvent(XrInstance, XrEventDataBuffer*);
XrResult xrWaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState*);
XrResult xrBeginFrame(XrSession, const XrFrameBeginInfo*);
XrResult xrEndFrame(XrSession, const XrFrameEndInfo*);
'''
        openxr_platform_h = r'''
#pragma once
#define XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME "XR_KHR_android_create_instance"
#define XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME "XR_KHR_opengl_es_enable"
#define XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME "XR_KHR_android_surface_swapchain"
#define XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME "XR_FB_android_surface_swapchain_create"
typedef struct { XrStructureType type; const void* next; void* applicationVM; void* applicationContext; } XrLoaderInitInfoAndroidKHR;
typedef struct { XrStructureType type; const void* next; } XrLoaderInitInfoBaseHeaderKHR;
typedef struct { XrStructureType type; const void* next; void* applicationVM; void* applicationActivity; } XrInstanceCreateInfoAndroidKHR;
typedef struct { XrStructureType type; const void* next; EGLDisplay display; EGLConfig config; EGLContext context; } XrGraphicsBindingOpenGLESAndroidKHR;
typedef struct { XrStructureType type; void* next; XrVersion minApiVersionSupported; XrVersion maxApiVersionSupported; } XrGraphicsRequirementsOpenGLESKHR;
typedef struct { XrStructureType type; const void* next; XrFlags64 createFlags; } XrAndroidSurfaceSwapchainCreateInfoFB;
typedef XrResult (*PFN_xrInitializeLoaderKHR)(const XrLoaderInitInfoBaseHeaderKHR*);
typedef XrResult (*PFN_xrGetOpenGLESGraphicsRequirementsKHR)(XrInstance, XrSystemId, XrGraphicsRequirementsOpenGLESKHR*);
typedef XrResult (*PFN_xrCreateSwapchainAndroidSurfaceKHR)(XrSession, const XrSwapchainCreateInfo*, XrSwapchain*, jobject*);
'''
        jni_h = r'''
#pragma once
#include <stdint.h>
typedef uint8_t jboolean;
typedef int32_t jint;
typedef int64_t jlong;
typedef void* jobject;
typedef jobject jclass;
typedef jobject jstring;
typedef void* jmethodID;
typedef enum jobjectRefType {
    JNIInvalidRefType = 0,
    JNILocalRefType = 1,
    JNIGlobalRefType = 2,
    JNIWeakGlobalRefType = 3
} jobjectRefType;
#define JNIEXPORT
#define JNICALL
#define JNI_OK 0
#define JNI_VERSION_1_6 0x00010006
struct JNIInvokeInterface_;
struct JNINativeInterface_;
typedef const struct JNIInvokeInterface_* JavaVM;
typedef const struct JNINativeInterface_* JNIEnv;
struct JNIInvokeInterface_ {
    jint (*AttachCurrentThread)(JavaVM*, JNIEnv**, void*);
    jint (*DetachCurrentThread)(JavaVM*);
};
struct JNINativeInterface_ {
    jint (*GetJavaVM)(JNIEnv*, JavaVM**);
    jobject (*NewGlobalRef)(JNIEnv*, jobject);
    void (*DeleteGlobalRef)(JNIEnv*, jobject);
    jobject (*NewLocalRef)(JNIEnv*, jobject);
    void (*DeleteLocalRef)(JNIEnv*, jobject);
    jobjectRefType (*GetObjectRefType)(JNIEnv*, jobject);
    jmethodID (*GetStaticMethodID)(JNIEnv*, jclass, const char*, const char*);
    void (*CallStaticVoidMethod)(JNIEnv*, jclass, jmethodID, ...);
    jboolean (*ExceptionCheck)(JNIEnv*);
    void (*ExceptionDescribe)(JNIEnv*);
    void (*ExceptionClear)(JNIEnv*);
    const char* (*GetStringUTFChars)(JNIEnv*, jstring, jboolean*);
    void (*ReleaseStringUTFChars)(JNIEnv*, jstring, const char*);
    jstring (*NewStringUTF)(JNIEnv*, const char*);
};
'''
        egl_h = r'''
#pragma once
#include <stdint.h>
typedef int32_t EGLint;
typedef void* EGLDisplay;
typedef void* EGLConfig;
typedef void* EGLContext;
typedef void* EGLSurface;
typedef unsigned int EGLBoolean;
#define EGL_DEFAULT_DISPLAY ((void*)0)
#define EGL_NO_DISPLAY ((EGLDisplay)0)
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_NO_SURFACE ((EGLSurface)0)
#define EGL_NONE 0x3038
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_OPENGL_ES3_BIT 0x0040
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
EGLDisplay eglGetDisplay(void*);
EGLBoolean eglInitialize(EGLDisplay, EGLint*, EGLint*);
EGLBoolean eglChooseConfig(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*);
EGLContext eglCreateContext(EGLDisplay, EGLConfig, EGLContext, const EGLint*);
EGLSurface eglCreatePbufferSurface(EGLDisplay, EGLConfig, const EGLint*);
EGLBoolean eglMakeCurrent(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
EGLBoolean eglDestroySurface(EGLDisplay, EGLSurface);
EGLBoolean eglDestroyContext(EGLDisplay, EGLContext);
EGLBoolean eglTerminate(EGLDisplay);
EGLint eglGetError(void);
'''
        harness_c = r'''
#include <assert.h>
#include <sched.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <EGL/egl.h>
#include <jni.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

typedef struct {
    void* target;
    jobjectRefType kind;
    int alive;
    int runtime_owned;
} RefSlot;

static int dummy_obj = 1;
static int dummy_surf = 2;
static int dummy_cls = 3;
static RefSlot g_refs[64];
static int g_ref_count = 0;
static int g_app_global_refs = 0;
static int g_del_local_calls = 0;
static int g_return_global_surf = 1;
static jobject g_runtime_surf_ref = NULL;
static atomic_int g_surface_fetched = 0;
static const char* log_str = "/tmp/issopenxr_test.log";
static int state_log[32];
static int state_count = 0;
static int ended_after_stopping_idx = -1;
static int quad_frames = 0;
static int event_step = 0;

void __android_log_print(int prio, const char* tag, const char* fmt, ...) { (void)prio; (void)tag; (void)fmt; }

static RefSlot* alloc_ref(void* target, jobjectRefType kind, int runtime_owned) {
    assert(target != NULL);
    assert(g_ref_count < 64);
    RefSlot* s = &g_refs[g_ref_count++];
    s->target = target;
    s->kind = kind;
    s->alive = 1;
    s->runtime_owned = runtime_owned;
    if (kind == JNIGlobalRefType && !runtime_owned) g_app_global_refs++;
    return s;
}

static RefSlot* find_ref(jobject o) {
    for (int i = 0; i < g_ref_count; ++i) {
        if ((jobject)&g_refs[i] == o) {
            assert(g_refs[i].alive);
            return &g_refs[i];
        }
    }
    assert(0 && "Unknown or freed JNI reference passed to JNI");
    return NULL;
}

static jint m_attach(JavaVM* vm, JNIEnv** env, void* args);
static jint m_detach(JavaVM* vm) { (void)vm; return JNI_OK; }
static struct JNIInvokeInterface_ g_vm_fn = { .AttachCurrentThread = m_attach, .DetachCurrentThread = m_detach };
static JavaVM g_vm = &g_vm_fn;

static jint m_get_vm(JNIEnv* e, JavaVM** out) { (void)e; *out = &g_vm; return JNI_OK; }
static jobject m_new_global(JNIEnv* e, jobject o) {
    (void)e;
    RefSlot* s = find_ref(o);
    return (jobject)alloc_ref(s->target, JNIGlobalRefType, 0);
}
static void m_del_global(JNIEnv* e, jobject o) {
    (void)e;
    if (!o) return;
    RefSlot* s = find_ref(o);
    assert(s->kind == JNIGlobalRefType);
    assert(!s->runtime_owned && "App must never DeleteGlobalRef runtime-owned surface ref");
    s->alive = 0;
    g_app_global_refs--;
}
static jobject m_new_local(JNIEnv* e, jobject o) {
    (void)e;
    RefSlot* s = find_ref(o);
    return (jobject)alloc_ref(s->target, JNILocalRefType, 0);
}
static void m_del_local(JNIEnv* e, jobject o) {
    (void)e;
    if (!o) return;
    RefSlot* s = find_ref(o);
    /* Mirror ART CheckJNI: abort immediately if DeleteLocalRef receives a GlobalRef. */
    assert(s->kind == JNILocalRefType && "CheckJNI: DeleteLocalRef called on non-Local ref");
    s->alive = 0;
    g_del_local_calls++;
}
static jobjectRefType m_get_ref_type(JNIEnv* e, jobject o) {
    (void)e;
    if (!o) return JNIInvalidRefType;
    return find_ref(o)->kind;
}
static jmethodID m_get_mid(JNIEnv* e, jclass c, const char* n, const char* s) {
    (void)e;
    RefSlot* cs = find_ref((jobject)c);
    assert(cs->kind == JNIGlobalRefType && cs->target == &dummy_cls);
    (void)n; (void)s;
    return (jmethodID)&dummy_obj;
}
static void m_call_static(JNIEnv* e, jclass c, jmethodID m, ...) {
    (void)e; (void)m;
    RefSlot* cs = find_ref((jobject)c);
    assert(cs->kind == JNIGlobalRefType && cs->target == &dummy_cls);
    va_list ap; va_start(ap, m); int st = va_arg(ap, int); va_end(ap);
    if (state_count < 32) state_log[state_count++] = st;
}
static jboolean m_exc_check(JNIEnv* e) { (void)e; return 0; }
static void m_exc_desc(JNIEnv* e) { (void)e; }
static void m_exc_clear(JNIEnv* e) { (void)e; }
static const char* m_get_utf(JNIEnv* e, jstring s, jboolean* c) { (void)e; if (c) *c = 0; return (const char*)s; }
static void m_rel_utf(JNIEnv* e, jstring s, const char* c) { (void)e; (void)s; (void)c; }
static jstring m_new_utf(JNIEnv* e, const char* s) { (void)e; return (jstring)strdup(s); }

static struct JNINativeInterface_ g_env_fn = {
    .GetJavaVM = m_get_vm, .NewGlobalRef = m_new_global, .DeleteGlobalRef = m_del_global,
    .NewLocalRef = m_new_local, .DeleteLocalRef = m_del_local, .GetObjectRefType = m_get_ref_type,
    .GetStaticMethodID = m_get_mid,
    .CallStaticVoidMethod = m_call_static, .ExceptionCheck = m_exc_check, .ExceptionDescribe = m_exc_desc,
    .ExceptionClear = m_exc_clear, .GetStringUTFChars = m_get_utf, .ReleaseStringUTFChars = m_rel_utf,
    .NewStringUTF = m_new_utf
};
static JNIEnv g_env = &g_env_fn;
static jint m_attach(JavaVM* vm, JNIEnv** env, void* args) { (void)vm; (void)args; *env = &g_env; return JNI_OK; }

EGLDisplay eglGetDisplay(void* d) { (void)d; return (EGLDisplay)1; }
EGLBoolean eglInitialize(EGLDisplay d, EGLint* maj, EGLint* min) { (void)d; (void)maj; (void)min; return 1; }
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint* a, EGLConfig* c, EGLint n, EGLint* out) { (void)d; (void)a; (void)n; *c = (EGLConfig)1; *out = 1; return 1; }
EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext s, const EGLint* a) { (void)d; (void)c; (void)s; (void)a; return (EGLContext)1; }
EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint* a) { (void)d; (void)c; (void)a; return (EGLSurface)1; }
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c) { (void)d; (void)dr; (void)rd; (void)c; return 1; }
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s) { (void)d; (void)s; return 1; }
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c) { (void)d; (void)c; return 1; }
EGLBoolean eglTerminate(EGLDisplay d) { (void)d; return 1; }
EGLint eglGetError(void) { return 0; }

static XrResult mock_init_loader(const XrLoaderInitInfoBaseHeaderKHR* i) {
    const XrLoaderInitInfoAndroidKHR* ai = (const XrLoaderInitInfoAndroidKHR*)i;
    RefSlot* act = find_ref(ai->applicationContext);
    assert(act->kind == JNIGlobalRefType && act->target == &dummy_obj);
    return XR_SUCCESS;
}
static XrResult mock_get_gles_reqs(XrInstance i, XrSystemId s, XrGraphicsRequirementsOpenGLESKHR* r) {
    (void)i; (void)s; r->minApiVersionSupported = XR_MAKE_VERSION(3, 0, 0); r->maxApiVersionSupported = XR_MAKE_VERSION(3, 2, 0); return XR_SUCCESS;
}
static XrResult mock_create_surf(XrSession s, const XrSwapchainCreateInfo* ci, XrSwapchain* sc, jobject* surf) {
    (void)s;
    assert(ci->format == 0 && ci->sampleCount == 0 && ci->faceCount == 0 && ci->arraySize == 0 && ci->mipCount == 0);
    assert(ci->width == 1280 && ci->height == 720);
    *sc = 77;
    if (g_return_global_surf) {
        g_runtime_surf_ref = (jobject)alloc_ref(&dummy_surf, JNIGlobalRefType, 1);
    } else {
        g_runtime_surf_ref = (jobject)alloc_ref(&dummy_surf, JNILocalRefType, 1);
    }
    *surf = g_runtime_surf_ref;
    return XR_SUCCESS;
}
XrResult xrGetInstanceProcAddr(XrInstance inst, const char* name, PFN_xrVoidFunction* fn) {
    (void)inst;
    if (strcmp(name, "xrInitializeLoaderKHR") == 0) *fn = (PFN_xrVoidFunction)mock_init_loader;
    else if (strcmp(name, "xrGetOpenGLESGraphicsRequirementsKHR") == 0) *fn = (PFN_xrVoidFunction)mock_get_gles_reqs;
    else if (strcmp(name, "xrCreateSwapchainAndroidSurfaceKHR") == 0) *fn = (PFN_xrVoidFunction)mock_create_surf;
    else *fn = NULL;
    return XR_SUCCESS;
}
XrResult xrEnumerateInstanceExtensionProperties(const char* l, uint32_t cap, uint32_t* count, XrExtensionProperties* props) {
    (void)l;
    static const char* exts[] = {
        XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
        XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
        XR_KHR_ANDROID_SURFACE_SWAPCHAIN_EXTENSION_NAME,
        XR_FB_ANDROID_SURFACE_SWAPCHAIN_CREATE_EXTENSION_NAME
    };
    *count = 4;
    if (cap >= 4 && props) {
        for (uint32_t i = 0; i < 4; ++i) snprintf(props[i].extensionName, XR_MAX_EXTENSION_NAME_SIZE, "%s", exts[i]);
    }
    return XR_SUCCESS;
}
XrResult xrCreateInstance(const XrInstanceCreateInfo* ci, XrInstance* inst) { assert(ci->applicationInfo.apiVersion == XR_API_VERSION_1_0); *inst = 11; return XR_SUCCESS; }
XrResult xrDestroyInstance(XrInstance i) { assert(i == 11); return XR_SUCCESS; }
XrResult xrGetSystem(XrInstance i, const XrSystemGetInfo* gi, XrSystemId* s) { (void)i; (void)gi; *s = 22; return XR_SUCCESS; }
XrResult xrEnumerateEnvironmentBlendModes(XrInstance i, XrSystemId s, XrViewConfigurationType v, uint32_t cap, uint32_t* count, XrEnvironmentBlendMode* modes) {
    (void)i; (void)s; (void)v; *count = 1; if (cap >= 1 && modes) modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE; return XR_SUCCESS;
}
XrResult xrCreateSession(XrInstance i, const XrSessionCreateInfo* ci, XrSession* s) { (void)i; (void)ci; *s = 33; return XR_SUCCESS; }
XrResult xrDestroySession(XrSession s) { assert(s == 33); return XR_SUCCESS; }
XrResult xrBeginSession(XrSession s, const XrSessionBeginInfo* bi) { assert(s == 33 && bi->primaryViewConfigurationType == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO); return XR_SUCCESS; }
XrResult xrEndSession(XrSession s) {
    assert(s == 33);
    ended_after_stopping_idx = state_count;
    assert(state_count > 0 && state_log[state_count - 1] == XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}
XrResult xrCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo* ci, XrSpace* sp) { (void)s; assert(ci->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL); *sp = 44; return XR_SUCCESS; }
XrResult xrDestroySpace(XrSpace sp) { assert(sp == 44); return XR_SUCCESS; }
XrResult xrDestroySwapchain(XrSwapchain sc) {
    assert(sc == 77);
    if (g_return_global_surf && g_runtime_surf_ref) {
        RefSlot* rs = find_ref(g_runtime_surf_ref);
        assert(rs->kind == JNIGlobalRefType && rs->alive);
        rs->alive = 0;
    }
    return XR_SUCCESS;
}
XrResult xrPollEvent(XrInstance i, XrEventDataBuffer* buf) {
    (void)i;
    static const XrSessionState seq[] = {
        XR_SESSION_STATE_READY, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_VISIBLE,
        XR_SESSION_STATE_FOCUSED, XR_SESSION_STATE_STOPPING, XR_SESSION_STATE_EXITING
    };
    if (event_step < 4) {
        XrEventDataSessionStateChanged* ch = (XrEventDataSessionStateChanged*)buf;
        ch->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        ch->session = 33;
        ch->state = seq[event_step++];
        return XR_SUCCESS;
    }
    if (quad_frames >= 2 && atomic_load(&g_surface_fetched) && event_step < 6) {
        XrEventDataSessionStateChanged* ch = (XrEventDataSessionStateChanged*)buf;
        ch->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
        ch->session = 33;
        ch->state = seq[event_step++];
        return XR_SUCCESS;
    }
    return XR_EVENT_UNAVAILABLE;
}
XrResult xrWaitFrame(XrSession s, const XrFrameWaitInfo* wi, XrFrameState* fs) {
    (void)s; (void)wi;
    if (quad_frames >= 2 && !atomic_load(&g_surface_fetched)) sched_yield();
    fs->predictedDisplayTime = 1000;
    fs->shouldRender = 1;
    return XR_SUCCESS;
}
XrResult xrBeginFrame(XrSession s, const XrFrameBeginInfo* bi) { (void)s; (void)bi; return XR_SUCCESS; }
XrResult xrEndFrame(XrSession s, const XrFrameEndInfo* ei) {
    (void)s;
    assert(ei->layerCount == 1 && ei->layers[0]->type == XR_TYPE_COMPOSITION_LAYER_QUAD);
    const XrCompositionLayerQuad* q = (const XrCompositionLayerQuad*)ei->layers[0];
    assert(q->subImage.swapchain == 77 && q->subImage.imageRect.extent.width == 1280 && q->subImage.imageRect.extent.height == 720);
    quad_frames++;
    return XR_SUCCESS;
}

extern jint JNI_OnLoad(JavaVM* vm, void* reserved);
extern jstring Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStart(JNIEnv* env, jclass cls, jobject activity, jint width, jint height, jstring jlogPath);
extern jobject Java_ro_cobrabm_fexdroid_IssOpenXr_nativeGetSurface(JNIEnv* env, jclass cls);
extern void Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStop(JNIEnv* env, jclass cls);
extern jlong Java_ro_cobrabm_fexdroid_IssOpenXr_nativeFrames(JNIEnv* env, jclass cls);
extern jint Java_ro_cobrabm_fexdroid_IssOpenXr_nativeSessionState(JNIEnv* env, jclass cls);

static void run_session_cycle(int return_global_surf) {
    g_ref_count = 0;
    g_app_global_refs = 0;
    g_del_local_calls = 0;
    g_return_global_surf = return_global_surf;
    g_runtime_surf_ref = NULL;
    atomic_store(&g_surface_fetched, 0);
    state_count = 0;
    ended_after_stopping_idx = -1;
    quad_frames = 0;
    event_step = 0;

    jclass cls_ref = (jclass)alloc_ref(&dummy_cls, JNILocalRefType, 0);
    jobject act_ref = (jobject)alloc_ref(&dummy_obj, JNILocalRefType, 0);
    jstring res = Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStart(&g_env, cls_ref, act_ref, 1280, 720, (jstring)log_str);
    assert(strcmp((const char*)res, "openxr running") == 0);
    jobject surf = Java_ro_cobrabm_fexdroid_IssOpenXr_nativeGetSurface(&g_env, cls_ref);
    RefSlot* ss = find_ref(surf);
    assert(ss->kind == JNILocalRefType && ss->target == &dummy_surf);
    assert(g_del_local_calls == (return_global_surf ? 0 : 1));
    atomic_store(&g_surface_fetched, 1);
    while (Java_ro_cobrabm_fexdroid_IssOpenXr_nativeSessionState(&g_env, cls_ref) != XR_SESSION_STATE_EXITING) {
        sched_yield();
    }
    Java_ro_cobrabm_fexdroid_IssOpenXr_nativeStop(&g_env, cls_ref);
    assert(g_app_global_refs == 0);
    assert(Java_ro_cobrabm_fexdroid_IssOpenXr_nativeFrames(&g_env, cls_ref) >= 2);
    assert(ended_after_stopping_idx >= 5);
    assert(state_count == 6);
    assert(state_log[0] == XR_SESSION_STATE_READY);
    assert(state_log[3] == XR_SESSION_STATE_FOCUSED);
    assert(state_log[4] == XR_SESSION_STATE_STOPPING);
    assert(state_log[5] == XR_SESSION_STATE_EXITING);
}

int main(void) {
    assert(JNI_OnLoad(&g_vm, NULL) == JNI_VERSION_1_6);
    run_session_cycle(1); /* Meta Quest HorizonOS mode: swapchain returns GlobalRef */
    run_session_cycle(0); /* LocalRef swapchain mode: freed safely via DeleteLocalRef */
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            inc = p / "inc"
            for d in ("openxr", "EGL", "GLES3", "android"):
                (inc / d).mkdir(parents=True)
            (inc / "openxr/openxr.h").write_text(openxr_h)
            (inc / "openxr/openxr_platform.h").write_text(openxr_platform_h)
            (inc / "jni.h").write_text(jni_h)
            (inc / "EGL/egl.h").write_text(egl_h)
            (inc / "GLES3/gl3.h").write_text("/* stub */\n")
            (inc / "android/log.h").write_text(
                "#pragma once\n#define ANDROID_LOG_INFO 4\n#define ANDROID_LOG_ERROR 6\n"
                "void __android_log_print(int, const char*, const char*, ...);\n"
            )
            (p / "harness.c").write_text(harness_c)
            subprocess.run(
                [CC, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread", "-I", str(inc),
                 str(ROOT / "tools/android/bionic/openxr/issopenxr.c"), str(p / "harness.c"), "-o", str(p / "test")],
                check=True,
                capture_output=True,
                text=True,
            )
            subprocess.run([str(p / "test")], check=True, timeout=10)

    @unittest.skipUnless(JAVAC, "javac unavailable")
    def test_java8_vr_sources_compile_cleanly_with_stubs(self):
        stubs = {
            "android/os/Build.java": "package android.os; public class Build { public static class VERSION { public static int SDK_INT = 35; } }",
            "android/os/Bundle.java": "package android.os; public class Bundle {}",
            "android/os/Handler.java": "package android.os; public class Handler { public Handler(Looper l) {} public boolean post(Runnable r) { return true; } public boolean postDelayed(Runnable r, long ms) { return true; } public void removeCallbacksAndMessages(Object o) {} }",
            "android/os/Looper.java": "package android.os; public class Looper { public static Looper getMainLooper() { return new Looper(); } }",
            "android/os/SystemClock.java": "package android.os; public class SystemClock { public static long uptimeMillis() { return 0; } }",
            "android/content/Context.java": "package android.content; public class Context { public SharedPreferences getSharedPreferences(String n, int m) { return null; } public java.io.File getFilesDir() { return null; } }",
            "android/content/Intent.java": "package android.content; public class Intent { public String getStringExtra(String n) { return null; } }",
            "android/content/SharedPreferences.java": "package android.content; public interface SharedPreferences { interface Editor { Editor putBoolean(String k, boolean v); void apply(); } boolean getBoolean(String k, boolean d); String getString(String k, String d); int getInt(String k, int d); Editor edit(); }",
            "android/content/DialogInterface.java": "package android.content; public interface DialogInterface { interface OnClickListener { void onClick(DialogInterface d, int w); } }",
            "android/view/View.java": "package android.view; public class View { public static final int VISIBLE = 0, GONE = 8; public void setBackgroundColor(int c) {} public void setFocusable(boolean f) {} public void setFocusableInTouchMode(boolean f) {} public boolean requestFocus() { return true; } public void setVisibility(int v) {} }",
            "android/view/Surface.java": "package android.view; public class Surface { public boolean isValid() { return true; } }",
            "android/view/SurfaceHolder.java": "package android.view; public interface SurfaceHolder { interface Callback { void surfaceCreated(SurfaceHolder h); void surfaceChanged(SurfaceHolder h, int f, int w, int ht); void surfaceDestroyed(SurfaceHolder h); } void addCallback(Callback c); void removeCallback(Callback c); Surface getSurface(); }",
            "android/view/Gravity.java": "package android.view; public class Gravity { public static final int CENTER = 17; }",
            "android/view/KeyEvent.java": "package android.view; public class KeyEvent { public static final int KEYCODE_BACK = 4, ACTION_UP = 1; public int getKeyCode() { return 0; } public int getAction() { return 0; } public boolean isCanceled() { return false; } }",
            "android/view/MotionEvent.java": "package android.view; public class MotionEvent {}",
            "android/view/Window.java": "package android.view; public class Window { public void addFlags(int f) {} }",
            "android/view/WindowManager.java": "package android.view; public interface WindowManager { class LayoutParams { public static final int FLAG_KEEP_SCREEN_ON = 128, FLAG_FULLSCREEN = 1024; } }",
            "android/widget/FrameLayout.java": "package android.widget; public class FrameLayout extends android.view.View { public FrameLayout(android.content.Context c) {} public void addView(android.view.View v) {} }",
            "android/widget/TextView.java": "package android.widget; public class TextView extends android.view.View { public TextView(android.content.Context c) {} public void setText(CharSequence t) {} public void setGravity(int g) {} public void setTextColor(int c) {} public void setTextSize(float s) {} }",
            "android/widget/CompoundButton.java": "package android.widget; public class CompoundButton extends TextView { public CompoundButton(android.content.Context c) { super(c); } public interface OnCheckedChangeListener { void onCheckedChanged(CompoundButton b, boolean c); } public void setOnCheckedChangeListener(OnCheckedChangeListener l) {} public void setChecked(boolean c) {} }",
            "android/widget/Switch.java": "package android.widget; public class Switch extends CompoundButton { public Switch(android.content.Context c) { super(c); } }",
            "android/widget/Toast.java": "package android.widget; public class Toast { public static final int LENGTH_SHORT = 0; public static Toast makeText(android.content.Context c, CharSequence s, int d) { return new Toast(); } public void show() {} }",
            "android/text/TextUtils.java": "package android.text; public class TextUtils { public static String join(CharSequence d, Iterable<?> t) { return \"\"; } }",
            "android/app/Activity.java": "package android.app; public class Activity extends android.content.Context { protected void attachBaseContext(android.content.Context b) {} protected void onCreate(android.os.Bundle s) {} protected void onResume() {} protected void onPause() {} protected void onDestroy() {} public void finish() {} public boolean isFinishing() { return false; } public android.view.Window getWindow() { return new android.view.Window(); } public android.content.Intent getIntent() { return new android.content.Intent(); } public void setContentView(android.view.View v) {} public void onBackPressed() {} public boolean dispatchKeyEvent(android.view.KeyEvent e) { return false; } public boolean onGenericMotionEvent(android.view.MotionEvent e) { return false; } }",
            "android/app/AlertDialog.java": "package android.app; public class AlertDialog { public static class Builder { public Builder(android.content.Context c) {} public Builder setTitle(CharSequence t) { return this; } public Builder setMessage(CharSequence m) { return this; } public Builder setCancelable(boolean c) { return this; } public Builder setPositiveButton(CharSequence t, android.content.DialogInterface.OnClickListener l) { return this; } public AlertDialog create() { return new AlertDialog(); } } public void show() {} public void dismiss() {} }",
            "ro/cobrabm/fexdroid/AppLanguage.java": "package ro.cobrabm.fexdroid; public final class AppLanguage { static android.content.Context attach(android.content.Context c) { return c; } static String text(String en, String pt) { return en; } static String gameLanguage() { return \"en\"; } }",
            "ro/cobrabm/fexdroid/PadBridge.java": "package ro.cobrabm.fexdroid; class PadBridge { PadBridge(java.io.File f) {} void setSwapFaceButtons(boolean e) {} void setDpadSwipe(boolean e) {} void setMobileControls(boolean e) {} boolean key(android.view.KeyEvent e) { return false; } boolean motion(android.view.MotionEvent e) { return false; } void reset() {} }",
            "ro/cobrabm/fexdroid/AudioBridge.java": "package ro.cobrabm.fexdroid; class AudioBridge { AudioBridge(java.io.File f) {} void start() {} boolean awaitReady(long ms) { return true; } void pause() {} void resume() {} void stop() {} }",
            "ro/cobrabm/fexdroid/BionicRuntimeInstaller.java": "package ro.cobrabm.fexdroid; final class BionicRuntimeInstaller { static void install(android.content.Context c) throws Exception {} }",
            "ro/cobrabm/fexdroid/FramegenSettings.java": "package ro.cobrabm.fexdroid; final class FramegenSettings { static void write(android.app.Activity a) throws java.io.IOException {} }",
            "ro/cobrabm/fexdroid/BionicPerformanceSettings.java": "package ro.cobrabm.fexdroid; final class BionicPerformanceSettings { static int limit(android.content.SharedPreferences p) { return 60; } static void write(android.app.Activity a) throws java.io.IOException {} }",
            "ro/cobrabm/fexdroid/BionicVideoSettings.java": "package ro.cobrabm.fexdroid; final class BionicVideoSettings { static String outputResolution(android.content.SharedPreferences p) { return \"1280x720\"; } static String gameDisplayMode(android.content.SharedPreferences p) { return \"fit\"; } static String effectiveResolutionPatch(android.content.SharedPreferences p) { return \"native\"; } static String internalPatchName(String k) { return null; } static int fsrEnginePreset(android.content.SharedPreferences p) { return 1; } static String fsrSharpnessValue(android.content.SharedPreferences p) { return \"0.25\"; } static boolean fsrSharpenEnabled(android.content.SharedPreferences p) { return true; } static String fsrInputSize(android.content.SharedPreferences p) { return \"960x540\"; } }",
            "ro/cobrabm/fexdroid/BionicDriverSettings.java": "package ro.cobrabm.fexdroid; final class BionicDriverSettings { static String selection(android.content.Context c) { return \"auto\"; } static java.io.File customLibrary(android.content.Context c) { return new java.io.File(\"x\"); } }",
            "ro/cobrabm/fexdroid/BionicRendererSettings.java": "package ro.cobrabm.fexdroid; final class BionicRendererSettings { static void environment(android.content.SharedPreferences p, java.util.Map<String, String> e) {} }",
            "ro/cobrabm/fexdroid/BionicSession.java": "package ro.cobrabm.fexdroid; public final class BionicSession { public static Guard acquire(java.io.File f) throws java.io.IOException { return new Guard(); } public static final class Guard implements AutoCloseable { public void close() {} } }",
            "ro/cobrabm/fexdroid/BionicSaveArchive.java": "package ro.cobrabm.fexdroid; public final class BionicSaveArchive { public static void recover(java.io.File f) throws java.io.IOException {} }",
        }
        src_dir = ROOT / "android/app/src/ro/cobrabm/fexdroid"
        targets = [
            src_dir / "IssOpenXr.java",
            src_dir / "IssPresent.java",
            src_dir / "BionicVrSettings.java",
            src_dir / "BionicLaunchEnv.java",
            src_dir / "BionicVrGameActivity.java",
        ]
        with tempfile.TemporaryDirectory() as tmp:
            p = Path(tmp)
            stub_root = p / "stubs"
            out_dir = p / "out"
            out_dir.mkdir()
            stub_files = []
            for rel, content in stubs.items():
                f = stub_root / rel
                f.parent.mkdir(parents=True, exist_ok=True)
                f.write_text(content)
                stub_files.append(str(f))
            res = subprocess.run(
                [JAVAC, "-encoding", "UTF-8", "-source", "8", "-target", "8", "-d", str(out_dir),
                 *stub_files, *(str(t) for t in targets)],
                check=False,
                capture_output=True,
                text=True,
            )
            self.assertEqual(res.returncode, 0, res.stderr)


if __name__ == "__main__":
    unittest.main()
