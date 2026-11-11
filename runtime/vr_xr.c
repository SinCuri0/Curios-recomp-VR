#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "cfg.h"
#include "host.h"
#include "platform/platform.h"
#include "vk.h"

#if __has_include(<openxr/openxr.h>)

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#define SCREEN_DISTANCE_M 2.0f
#define SCREEN_WIDTH_M 3.2f
#define POSE_RING 8
#define DEFAULT_LAG 1   // camera updates between the one a picture was drawn with and the present that sends it

static struct {
    int state;   // 0 not tried, 1 instance and system ready, -1 off
    XrInstance inst;
    XrSystemId sys;
    XrSession session;
    XrSpace space, head;
    XrSwapchain sc;
    XrSwapchainImageVulkanKHR img[16];
    uint32_t n_img, w, h, idx;
    int raw_copy;
    int64_t formats[64];
    uint32_t n_formats;
    int running, in_frame, shown;
    XrFrameState fs;
    char inst_ext[2048], dev_ext[2048];
    PlatMutex pose_lock;
    int has_pose, has_shown;
    XrPosef pose, sent;             // newest; the one of the picture being sent
    XrVector3f origin;              // the first head position: the camera's zero
    int has_origin;
    XrPosef applied[POSE_RING];     // given to the camera, by camera update
    uint64_t n_applied;
    XrVector3f eye[2];              // in the head's space
    float ipd;
    float tan_x, tan_y;             // the widest half-angle tangents of the two eyes
} X;

static PFN_xrGetVulkanInstanceExtensionsKHR xrGetVulkanInstanceExtensionsKHR;
static PFN_xrGetVulkanDeviceExtensionsKHR xrGetVulkanDeviceExtensionsKHR;
static PFN_xrGetVulkanGraphicsDeviceKHR xrGetVulkanGraphicsDeviceKHR;
static PFN_xrGetVulkanGraphicsRequirementsKHR xrGetVulkanGraphicsRequirementsKHR;

static int xr_ok(XrResult r, const char *what) {
    if (XR_SUCCEEDED(r)) return 1;
    char s[XR_MAX_RESULT_STRING_SIZE] = "";
    if (X.inst) xrResultToString(X.inst, r, s);
    rt_log("vr: %s failed: %s (%d)", what, s, (int)r);
    return 0;
}

static int instance_up(void) {
    if (X.state) return X.state > 0;
    X.state = -1;
    const char *e = cfg_get("TOTK_VR");
    if (!e || !*e || !strcmp(e, "0") || !strcmp(e, "sbs")) return 0;

    uint32_t n = 0;
    if (!xr_ok(xrEnumerateInstanceExtensionProperties(NULL, 0, &n, NULL), "xrEnumerateInstanceExtensionProperties (is an OpenXR runtime active?)"))
        return 0;
    XrExtensionProperties *p = calloc(n ? n : 1, sizeof *p);
    for (uint32_t i = 0; i < n; i++) p[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    xrEnumerateInstanceExtensionProperties(NULL, n, &n, p);
    int has_vulkan = 0;
    for (uint32_t i = 0; i < n; i++) has_vulkan |= !strcmp(p[i].extensionName, XR_KHR_VULKAN_ENABLE_EXTENSION_NAME);
    free(p);
    if (!has_vulkan) { rt_log("vr: the OpenXR runtime has no %s", XR_KHR_VULKAN_ENABLE_EXTENSION_NAME); return 0; }

    const char *names[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {.type = XR_TYPE_INSTANCE_CREATE_INFO,
                               .applicationInfo = {.applicationName = "Ocarina Recomp", .applicationVersion = 1,
                                                   .engineName = "recomp", .apiVersion = XR_API_VERSION_1_0},
                               .enabledExtensionCount = 1, .enabledExtensionNames = names};
    if (!xr_ok(xrCreateInstance(&ci, &X.inst), "xrCreateInstance")) return 0;
#define LOAD(name) xr_ok(xrGetInstanceProcAddr(X.inst, #name, (PFN_xrVoidFunction *)&name), #name)
    if (!LOAD(xrGetVulkanInstanceExtensionsKHR) || !LOAD(xrGetVulkanDeviceExtensionsKHR) ||
        !LOAD(xrGetVulkanGraphicsDeviceKHR) || !LOAD(xrGetVulkanGraphicsRequirementsKHR)) {
        xrDestroyInstance(X.inst);
        X.inst = XR_NULL_HANDLE;
        return 0;
    }
#undef LOAD
    XrInstanceProperties ip = {.type = XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(X.inst, &ip)))
        rt_log("vr: OpenXR runtime %s %u.%u.%u", ip.runtimeName, (unsigned)XR_VERSION_MAJOR(ip.runtimeVersion),
               (unsigned)XR_VERSION_MINOR(ip.runtimeVersion), (unsigned)XR_VERSION_PATCH(ip.runtimeVersion));

    XrSystemGetInfo si = {.type = XR_TYPE_SYSTEM_GET_INFO, .formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY};
    if (!xr_ok(xrGetSystem(X.inst, &si, &X.sys), "xrGetSystem (is the headset connected?)")) {
        xrDestroyInstance(X.inst);
        X.inst = XR_NULL_HANDLE;
        return 0;
    }
    XrSystemProperties sp = {.type = XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(X.inst, X.sys, &sp))) rt_log("vr: headset %s", sp.systemName);
    X.state = 1;
    return 1;
}

static uint32_t add_names(char *list, const char **ext, uint32_t n, uint32_t max) {
    for (char *s = strtok(list, " "); s; s = strtok(NULL, " ")) {
        uint32_t i = 0;
        while (i < n && strcmp(ext[i], s)) i++;
        if (i < n) continue;
        if (n == max) { rt_log("vr: no room for Vulkan extension %s", s); continue; }
        ext[n++] = s;
    }
    return n;
}

uint32_t vr_instance_extensions(const char **ext, uint32_t n, uint32_t max) {
    if (!instance_up()) return n;
    uint32_t len = 0;
    if (!xr_ok(xrGetVulkanInstanceExtensionsKHR(X.inst, X.sys, sizeof X.inst_ext, &len, X.inst_ext), "xrGetVulkanInstanceExtensionsKHR"))
        return n;
    rt_log("vr: Vulkan instance extensions for the headset: %s", X.inst_ext);
    return add_names(X.inst_ext, ext, n, max);
}

VkPhysicalDevice vr_physical_device(VkInstance instance) {
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!instance_up()) return pd;
    if (!xr_ok(xrGetVulkanGraphicsDeviceKHR(X.inst, X.sys, instance, &pd), "xrGetVulkanGraphicsDeviceKHR")) return VK_NULL_HANDLE;
    return pd;
}

uint32_t vr_device_extensions(const char **ext, uint32_t n, uint32_t max) {
    if (!instance_up()) return n;
    uint32_t len = 0;
    if (!xr_ok(xrGetVulkanDeviceExtensionsKHR(X.inst, X.sys, sizeof X.dev_ext, &len, X.dev_ext), "xrGetVulkanDeviceExtensionsKHR"))
        return n;
    rt_log("vr: Vulkan device extensions for the headset: %s", X.dev_ext);
    return add_names(X.dev_ext, ext, n, max);
}

void vr_session_create(const VkCtx *c) {
    if (!instance_up() || X.session) return;
    XrGraphicsRequirementsVulkanKHR req = {.type = XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if (!xr_ok(xrGetVulkanGraphicsRequirementsKHR(X.inst, X.sys, &req), "xrGetVulkanGraphicsRequirementsKHR")) goto off;
    XrGraphicsBindingVulkanKHR gb = {.type = XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR, .instance = c->instance,
                                     .physicalDevice = c->phys, .device = c->device,
                                     .queueFamilyIndex = c->gfx_family, .queueIndex = 0};
    XrSessionCreateInfo si = {.type = XR_TYPE_SESSION_CREATE_INFO, .next = &gb, .systemId = X.sys};
    if (!xr_ok(xrCreateSession(X.inst, &si, &X.session), "xrCreateSession")) goto off;
    XrReferenceSpaceCreateInfo ri = {.type = XR_TYPE_REFERENCE_SPACE_CREATE_INFO,
                                     .referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL,
                                     .poseInReferenceSpace = {.orientation = {0, 0, 0, 1}}};
    if (!xr_ok(xrCreateReferenceSpace(X.session, &ri, &X.space), "xrCreateReferenceSpace")) goto off;
    ri.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (!xr_ok(xrCreateReferenceSpace(X.session, &ri, &X.head), "xrCreateReferenceSpace (view)")) goto off;
    X.n_formats = 0;
    if (!xr_ok(xrEnumerateSwapchainFormats(X.session, 64, &X.n_formats, X.formats), "xrEnumerateSwapchainFormats") || !X.n_formats)
        goto off;
    rt_log("vr: session created (%u swapchain formats, first %lld)", X.n_formats, (long long)X.formats[0]);
    return;
off:
    X.state = -1;
}

static VkFormat srgb_twin(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM ? VK_FORMAT_B8G8R8A8_SRGB : f == VK_FORMAT_R8G8B8A8_UNORM ? VK_FORMAT_R8G8B8A8_SRGB : f;
}

static int make_swapchain(VkFormat window_fmt, uint32_t w, uint32_t h) {
    if (X.sc) xrDestroySwapchain(X.sc);
    X.sc = XR_NULL_HANDLE;
    int64_t fmt = X.formats[0];
    X.raw_copy = 0;
    for (uint32_t i = 0; i < X.n_formats; i++)
        if (X.formats[i] == (int64_t)srgb_twin(window_fmt)) fmt = X.formats[i], X.raw_copy = 1;
    XrSwapchainCreateInfo ci = {.type = XR_TYPE_SWAPCHAIN_CREATE_INFO,
                                .usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT,
                                .format = fmt, .sampleCount = 1, .width = w, .height = h, .faceCount = 1, .arraySize = 1,
                                .mipCount = 1};
    if (!xr_ok(xrCreateSwapchain(X.session, &ci, &X.sc), "xrCreateSwapchain")) return 0;
    for (int i = 0; i < 16; i++) X.img[i] = (XrSwapchainImageVulkanKHR){.type = XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR};
    if (!xr_ok(xrEnumerateSwapchainImages(X.sc, 16, &X.n_img, (XrSwapchainImageBaseHeader *)X.img), "xrEnumerateSwapchainImages"))
        return 0;
    X.w = w, X.h = h;
    rt_log("vr: screen swapchain %ux%u, format %lld, %u images, %s", w, h, (long long)fmt, X.n_img,
           X.raw_copy ? "copied as it is" : "blit with conversion");
    return 1;
}

static void poll_events(void) {
    XrEventDataBuffer ev = {.type = XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(X.inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            XrSessionState s = ((XrEventDataSessionStateChanged *)&ev)->state;
            rt_log("vr: session state %d", (int)s);
            if (s == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {.type = XR_TYPE_SESSION_BEGIN_INFO,
                                         .primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
                X.running = xr_ok(xrBeginSession(X.session, &bi), "xrBeginSession");
            } else if (s == XR_SESSION_STATE_STOPPING) {
                xrEndSession(X.session);
                X.running = 0;
            } else if (s == XR_SESSION_STATE_EXITING || s == XR_SESSION_STATE_LOSS_PENDING) {
                X.running = 0;
                X.state = -1;
            }
        }
        ev = (XrEventDataBuffer){.type = XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void end_frame(int with_screen) {
    const int32_t ew = (int32_t)(X.w / 2);
    XrCompositionLayerQuad q[2];
    for (int i = 0; i < 2; i++)
        q[i] = (XrCompositionLayerQuad){
            .type = XR_TYPE_COMPOSITION_LAYER_QUAD, .space = X.space,
            .eyeVisibility = i ? XR_EYE_VISIBILITY_RIGHT : XR_EYE_VISIBILITY_LEFT,
            .subImage = {.swapchain = X.sc, .imageRect = {{i * ew, 0}, {ew, (int32_t)X.h}}},
            .pose = {.orientation = {0, 0, 0, 1}, .position = {0, 0, -SCREEN_DISTANCE_M}},
            .size = {SCREEN_WIDTH_M, ew ? SCREEN_WIDTH_M * (float)X.h / (float)ew : 1.0f}};
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerProjection world = {.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION, .space = X.space, .viewCount = 2, .views = pv};
    const int in_world = vr_mode() == VR_WORLD && X.has_shown;
    if (in_world) {
        float p00, p11;
        if (!vr_camera_projection(&p00, &p11)) vr_stereo_fov(&p00, &p11);
        const float ax = atanf(1.0f / p00), ay = atanf(1.0f / p11);
        const XrQuaternionf o = X.sent.orientation;
        for (int i = 0; i < 2; i++) {
            const XrVector3f v = X.eye[i];
            const XrVector3f a = {o.y * v.z - o.z * v.y, o.z * v.x - o.x * v.z, o.x * v.y - o.y * v.x};
            const XrVector3f b = {o.y * a.z - o.z * a.y, o.z * a.x - o.x * a.z, o.x * a.y - o.y * a.x};
            pv[i] = (XrCompositionLayerProjectionView){
                .type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
                .pose = {.orientation = o,
                         .position = {X.sent.position.x + v.x + 2 * (o.w * a.x + b.x), X.sent.position.y + v.y + 2 * (o.w * a.y + b.y),
                                      X.sent.position.z + v.z + 2 * (o.w * a.z + b.z)}},
                .fov = {-ax, ax, ay, -ay},
                .subImage = {.swapchain = X.sc, .imageRect = {{i * ew, 0}, {ew, (int32_t)X.h}}}};
        }
    }
    const XrCompositionLayerBaseHeader *layers[2] = {(const XrCompositionLayerBaseHeader *)&q[0],
                                                     (const XrCompositionLayerBaseHeader *)&q[1]};
    if (in_world) layers[0] = (const XrCompositionLayerBaseHeader *)&world;
    XrFrameEndInfo ei = {.type = XR_TYPE_FRAME_END_INFO, .displayTime = X.fs.predictedDisplayTime,
                         .environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
                         .layerCount = !with_screen ? 0u : in_world ? 1u : 2u, .layers = with_screen ? layers : NULL};
    vk_queue_lock();
    XrResult r = xrEndFrame(X.session, &ei);
    vk_queue_unlock();
    if (!xr_ok(r, "xrEndFrame")) return;
    if (with_screen && !X.shown) {
        X.shown = 1;
        rt_log("vr: the first picture went to the headset (%ux%u, %s)", X.w, X.h, in_world ? "the world around the head" : "a screen");
    }
}

VkImage vr_frame_begin(VkFormat eyes_fmt, uint32_t w, uint32_t h, int *raw_copy) {
    if (X.state <= 0 || !X.session) return VK_NULL_HANDLE;
    poll_events();
    if (!X.running) return VK_NULL_HANDLE;
    const char *lg = cfg_get("TOTK_VR_LAG");
    const uint64_t lag = lg && *lg ? strtoull(lg, NULL, 10) % POSE_RING : DEFAULT_LAG;
    plat_mutex_lock(&X.pose_lock);
    X.has_shown = X.n_applied > lag;
    if (X.has_shown) X.sent = X.applied[(X.n_applied - 1 - lag) % POSE_RING];
    plat_mutex_unlock(&X.pose_lock);
    X.fs = (XrFrameState){.type = XR_TYPE_FRAME_STATE};
    if (!xr_ok(xrWaitFrame(X.session, NULL, &X.fs), "xrWaitFrame")) return VK_NULL_HANDLE;
    XrSpaceLocation loc = {.type = XR_TYPE_SPACE_LOCATION};
    const XrSpaceLocationFlags both = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT;
    if (XR_SUCCEEDED(xrLocateSpace(X.head, X.space, X.fs.predictedDisplayTime, &loc)) && (loc.locationFlags & both) == both) {
        plat_mutex_lock(&X.pose_lock);
        X.pose = loc.pose, X.has_pose = 1;
        plat_mutex_unlock(&X.pose_lock);
    }
    XrViewLocateInfo li = {.type = XR_TYPE_VIEW_LOCATE_INFO, .viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                           .displayTime = X.fs.predictedDisplayTime, .space = X.head};
    XrViewState vs = {.type = XR_TYPE_VIEW_STATE};
    XrView views[2] = {{.type = XR_TYPE_VIEW}, {.type = XR_TYPE_VIEW}};
    uint32_t nv = 0;
    if (XR_SUCCEEDED(xrLocateViews(X.session, &li, &vs, 2, &nv, views)) && nv == 2 &&
        (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT)) {
        const float dx = views[1].pose.position.x - views[0].pose.position.x, dy = views[1].pose.position.y - views[0].pose.position.y,
                    dz = views[1].pose.position.z - views[0].pose.position.z;
        X.eye[0] = views[0].pose.position, X.eye[1] = views[1].pose.position;
        float tx = 0, ty = 0;
        for (int i = 0; i < 2; i++) {
            const XrFovf f = views[i].fov;
            const float a[4] = {fabsf(tanf(f.angleLeft)), fabsf(tanf(f.angleRight)), fabsf(tanf(f.angleUp)), fabsf(tanf(f.angleDown))};
            tx = fmaxf(tx, fmaxf(a[0], a[1])), ty = fmaxf(ty, fmaxf(a[2], a[3]));
        }
        if (tx > 0 && ty > 0 && !X.tan_x) rt_log("vr: headset view %.1f x %.1f degrees", 2 * atanf(tx) / 0.0174533f, 2 * atanf(ty) / 0.0174533f);
        X.tan_x = tx, X.tan_y = ty;
        X.ipd = sqrtf(dx * dx + dy * dy + dz * dz);
    }
    vk_queue_lock();
    XrResult r = xrBeginFrame(X.session, NULL);
    vk_queue_unlock();
    if (!xr_ok(r, "xrBeginFrame")) return VK_NULL_HANDLE;
    if (!X.fs.shouldRender) { end_frame(0); return VK_NULL_HANDLE; }
    vk_queue_lock();
    int ok = (X.sc && X.w == w && X.h == h) || make_swapchain(eyes_fmt, w, h);
    ok = ok && xr_ok(xrAcquireSwapchainImage(X.sc, NULL, &X.idx), "xrAcquireSwapchainImage");
    vk_queue_unlock();
    XrSwapchainImageWaitInfo wi = {.type = XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO, .timeout = XR_INFINITE_DURATION};
    if (!ok || !xr_ok(xrWaitSwapchainImage(X.sc, &wi), "xrWaitSwapchainImage")) { end_frame(0); return VK_NULL_HANDLE; }
    X.in_frame = 1;
    *raw_copy = X.raw_copy;
    return X.img[X.idx].image;
}

void vr_frame_end(int drawn) {
    if (!X.in_frame) return;
    X.in_frame = 0;
    vk_queue_lock();
    XrResult r = xrReleaseSwapchainImage(X.sc, NULL);
    vk_queue_unlock();
    end_frame(xr_ok(r, "xrReleaseSwapchainImage") && drawn);
}

float vr_ipd(void) { return X.ipd > 0.0f ? X.ipd : 0.064f; }

int vr_headset_tan(float *tx, float *ty) {
    if (!(X.tan_x > 0) || !(X.tan_y > 0)) return 0;
    *tx = X.tan_x, *ty = X.tan_y;
    return 1;
}

int vr_head_pose(float fwd[3], float p[3]) {
    plat_mutex_lock(&X.pose_lock);
    const int has = X.has_pose;
    XrPosef h = X.pose;
    plat_mutex_unlock(&X.pose_lock);
    if (!has) return 0;
    const float x = h.orientation.x, y = h.orientation.y, z = h.orientation.z, w = h.orientation.w;
    const float fx = -2 * (x * z + y * w), fy = -2 * (y * z - x * w), fz = -(1 - 2 * (x * x + y * y));
    const float yaw = atan2f(-fx, -fz), pitch = asinf(fy < -1 ? -1 : fy > 1 ? 1 : fy);
    const float cy = cosf(yaw * 0.5f), sy = sinf(yaw * 0.5f), cp = cosf(pitch * 0.5f), sp = sinf(pitch * 0.5f);
    h.orientation = (XrQuaternionf){cy * sp, sy * cp, -sy * sp, cy * cp};
    fwd[0] = -sinf(yaw) * cosf(pitch), fwd[1] = sinf(pitch), fwd[2] = -cosf(yaw) * cosf(pitch);
    if (!X.has_origin) {
        X.origin = h.position, X.has_origin = 1;
        rt_log("vr: head zero at %.2f %.2f %.2f m in the play space", h.position.x, h.position.y, h.position.z);
    }
    p[0] = h.position.x - X.origin.x, p[1] = h.position.y - X.origin.y, p[2] = h.position.z - X.origin.z;
    plat_mutex_lock(&X.pose_lock);
    X.applied[X.n_applied++ % POSE_RING] = h;
    plat_mutex_unlock(&X.pose_lock);
    return 1;
}

void vr_shutdown(void) {
    if (X.sc) xrDestroySwapchain(X.sc);
    if (X.head) xrDestroySpace(X.head);
    if (X.space) xrDestroySpace(X.space);
    if (X.session) xrDestroySession(X.session);
    if (X.inst) xrDestroyInstance(X.inst);
    memset(&X, 0, sizeof X);
}

#else

uint32_t vr_instance_extensions(const char **ext, uint32_t n, uint32_t max) { (void)ext, (void)max; return n; }
VkPhysicalDevice vr_physical_device(VkInstance instance) { (void)instance; return VK_NULL_HANDLE; }
uint32_t vr_device_extensions(const char **ext, uint32_t n, uint32_t max) { (void)ext, (void)max; return n; }
void vr_session_create(const VkCtx *c) {
    const char *e = cfg_get("TOTK_VR");
    (void)c;
    if (e && *e && strcmp(e, "0")) rt_log("vr: TOTK_VR is set but this build has no OpenXR (openxr/openxr.h was not found)");
}
VkImage vr_frame_begin(VkFormat eyes_fmt, uint32_t w, uint32_t h, int *raw_copy) {
    (void)eyes_fmt, (void)w, (void)h, (void)raw_copy;
    return VK_NULL_HANDLE;
}
void vr_frame_end(int drawn) { (void)drawn; }
int vr_head_pose(float fwd[3], float p[3]) { (void)fwd, (void)p; return 0; }
float vr_ipd(void) { return 0.064f; }
int vr_headset_tan(float *tx, float *ty) { (void)tx, (void)ty; return 0; }
void vr_shutdown(void) {}

#endif
