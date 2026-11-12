#define _GNU_SOURCE
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <EGL/egl.h>
#include <GLES3/gl3.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#undef XRAPI_ATTR
#define XRAPI_ATTR __attribute__((visibility("default")))
#include <android/log.h>
#include <sys/system_properties.h>
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <limits.h>
#include <time.h>
#include "exports.h"

#define LOG(...) __android_log_print(ANDROID_LOG_INFO, "VDYVRInput", __VA_ARGS__)
#define REAL(name) ((PFN_##name)vd_forward_targets[VD_IDX_##name])
__attribute__((visibility("hidden"))) void *vd_forward_targets[VD_EXPORT_COUNT];
static int is_yvr;
static pthread_mutex_t diagnostic_lock = PTHREAD_MUTEX_INITIALIZER;

__attribute__((constructor)) static void initialize_adapter(void) {
    Dl_info self;
    char filename[PATH_MAX];
    char brand[PROP_VALUE_MAX] = {0};
    if (!dladdr((void *)initialize_adapter, &self) || !self.dli_fname) abort();
    if (snprintf(filename, sizeof(filename), "%s", self.dli_fname) >= (int)sizeof(filename)) abort();
    char *base = strrchr(filename, '/');
    if (!base || (size_t)(base + 1 - filename) + sizeof("libvd_original.so") > sizeof(filename)) abort();
    strcpy(base + 1, "libvd_original.so");
    void *original = dlopen(filename, RTLD_NOW | RTLD_LOCAL);
    if (!original) { LOG("original loader could not load: %s", dlerror()); abort(); }
    for (int i = 0; i < VD_EXPORT_COUNT; ++i) {
        vd_forward_targets[i] = dlsym(original, vd_export_names[i]);
        if (!vd_forward_targets[i]) { LOG("missing original export: %s", vd_export_names[i]); abort(); }
    }
    __system_property_get("ro.product.brand", brand);
    for (char *p = brand; *p; ++p) *p = (char)tolower((unsigned char)*p);
    is_yvr = strstr(brand, "yvr") != NULL;
    LOG("adapter v5 projection GPU orientation loaded, brand=%s, YVR mapping=%d, %d exports forwarded", brand, is_yvr, VD_EXPORT_COUNT);
}

static XrResult path_string(XrInstance instance, XrPath path, char out[XR_MAX_PATH_LENGTH]) {
    uint32_t count = 0;
    out[0] = 0;
    return REAL(xrPathToString)(instance, path, XR_MAX_PATH_LENGTH, &count, out);
}

XRAPI_ATTR XrResult XRAPI_CALL xrSuggestInteractionProfileBindings(XrInstance instance, const XrInteractionProfileSuggestedBinding *suggested) {
    char profile[XR_MAX_PATH_LENGTH];
    if (!is_yvr || !suggested || XR_FAILED(path_string(instance, suggested->interactionProfile, profile)) ||
        (strcmp(profile, "/interaction_profiles/pico/neo3_controller") && strcmp(profile, "/interaction_profiles/bytedance/pico_neo3_controller")))
        return REAL(xrSuggestInteractionProfileBindings)(instance, suggested);
    if (suggested->countSuggestedBindings > 4096 || (suggested->countSuggestedBindings && !suggested->suggestedBindings))
        return XR_ERROR_VALIDATION_FAILURE;
    XrInteractionProfileSuggestedBinding mapped = *suggested;
    XrResult result = REAL(xrStringToPath)(instance, "/interaction_profiles/oculus/touch_controller", &mapped.interactionProfile);
    if (XR_FAILED(result)) { LOG("Touch profile conversion failed: %d", result); return result; }
    XrActionSuggestedBinding *bindings = calloc(suggested->countSuggestedBindings ? suggested->countSuggestedBindings : 1, sizeof(*bindings));
    if (!bindings) return XR_ERROR_OUT_OF_MEMORY;
    uint32_t count = 0, menu_changes = 0, dropped = 0;
    for (uint32_t i = 0; i < suggested->countSuggestedBindings; ++i) {
        XrActionSuggestedBinding item = suggested->suggestedBindings[i];
        char path[XR_MAX_PATH_LENGTH];
        result = path_string(instance, item.binding, path);
        if (XR_FAILED(result)) { free(bindings); return result; }
        if (!strcmp(path, "/user/hand/right/input/back/click")) { ++dropped; continue; }
        if (!strcmp(path, "/user/hand/left/input/back/click")) {
            result = REAL(xrStringToPath)(instance, "/user/hand/left/input/menu/click", &item.binding);
            if (XR_FAILED(result)) { free(bindings); return result; }
            ++menu_changes;
        }
        bindings[count++] = item;
    }
    mapped.countSuggestedBindings = count;
    mapped.suggestedBindings = bindings;
    result = REAL(xrSuggestInteractionProfileBindings)(instance, &mapped);
    LOG("Pico -> Touch binding result=%d, bindings=%u, menu remaps=%u, right-back dropped=%u", result, count, menu_changes, dropped);
    free(bindings);
    return result;
}

// Diagnostics observe real runtime values; no action state or poses are fabricated.
struct ActionRecord { XrAction action; char name[XR_MAX_ACTION_NAME_SIZE]; };
static struct ActionRecord action_records[128];
static unsigned action_record_count;
static const char *action_name_locked(XrAction action) {
    for (unsigned i = 0; i < action_record_count; ++i) if (action_records[i].action == action) return action_records[i].name;
    return "unknown";
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateAction(XrActionSet action_set, const XrActionCreateInfo *info, XrAction *action) {
    XrResult result = REAL(xrCreateAction)(action_set, info, action);
    if (is_yvr && XR_SUCCEEDED(result) && info && action) {
        pthread_mutex_lock(&diagnostic_lock);
        if (action_record_count < 128) {
            struct ActionRecord *rec = &action_records[action_record_count++];
            rec->action = *action;
            snprintf(rec->name, sizeof(rec->name), "%.*s", XR_MAX_ACTION_NAME_SIZE - 1, info->actionName);
        }
        pthread_mutex_unlock(&diagnostic_lock);
    }
    return result;
}
struct ActionObservation { XrAction action; XrPath hand; int last; int used; };
static struct ActionObservation pose_observations[16], trigger_observations[16];
static int changed_locked(struct ActionObservation *observations, XrAction action, XrPath hand, int value) {
    for (unsigned i = 0; i < 16; ++i) {
        struct ActionObservation *rec = &observations[i];
        if (!rec->used || (rec->action == action && rec->hand == hand)) {
            int changed = !rec->used || rec->last != value;
            *rec = (struct ActionObservation){action, hand, value, 1};
            return changed;
        }
    }
    return 0;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStatePose(XrSession session, const XrActionStateGetInfo *info, XrActionStatePose *state) {
    XrResult result = REAL(xrGetActionStatePose)(session, info, state);
    if (is_yvr && info && state) {
        pthread_mutex_lock(&diagnostic_lock);
        int value = XR_SUCCEEDED(result) ? (int)state->isActive : (int)result;
        if (changed_locked(pose_observations, info->action, info->subactionPath, value))
            LOG("pose action=%s hand=%llu result=%d active=%u", action_name_locked(info->action), (unsigned long long)info->subactionPath, result, XR_SUCCEEDED(result) ? state->isActive : 0);
        pthread_mutex_unlock(&diagnostic_lock);
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetActionStateFloat(XrSession session, const XrActionStateGetInfo *info, XrActionStateFloat *state) {
    XrResult result = REAL(xrGetActionStateFloat)(session, info, state);
    if (is_yvr && info && state) {
        pthread_mutex_lock(&diagnostic_lock);
        const char *name = action_name_locked(info->action);
        if (!strcmp(name, "left_trigger") || !strcmp(name, "right_trigger")) {
            int value = XR_FAILED(result) ? (int)result : !state->isActive ? -1000 : state->currentState > 0.85f;
            if (changed_locked(trigger_observations, info->action, info->subactionPath, value))
                LOG("trigger action=%s result=%d active=%u value=%.3f pressed=%d", name, result, XR_SUCCEEDED(result) ? state->isActive : 0, XR_SUCCEEDED(result) ? state->currentState : 0.0f, value == 1);
        }
        pthread_mutex_unlock(&diagnostic_lock);
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateInstanceExtensionProperties(const char *layer, uint32_t capacity, uint32_t *count, XrExtensionProperties *properties) {
    if (!is_yvr) return REAL(xrEnumerateInstanceExtensionProperties)(layer, capacity, count, properties);
    uint32_t available = 0;
    XrResult result = REAL(xrEnumerateInstanceExtensionProperties)(layer, 0, &available, NULL);
    if (XR_FAILED(result) || !count) return XR_FAILED(result) ? result : XR_ERROR_VALIDATION_FAILURE;
    XrExtensionProperties *all = calloc(available ? available : 1, sizeof(*all));
    if (!all) return XR_ERROR_OUT_OF_MEMORY;
    for (uint32_t i = 0; i < available; ++i) all[i].type = XR_TYPE_EXTENSION_PROPERTIES;
    result = REAL(xrEnumerateInstanceExtensionProperties)(layer, available, &available, all);
    if (XR_FAILED(result)) { free(all); return result; }
    uint32_t kept = 0, hidden = 0;
    for (uint32_t i = 0; i < available; ++i) {
        if (!strcmp(all[i].extensionName, "XR_KHR_composition_layer_cube")) { ++hidden; continue; }
        all[kept++] = all[i];
    }
    *count = kept;
    if (capacity && capacity < kept) result = XR_ERROR_SIZE_INSUFFICIENT;
    else if (capacity && !properties) result = XR_ERROR_VALIDATION_FAILURE;
    else if (capacity) memcpy(properties, all, kept * sizeof(*all));
    if (hidden) LOG("scene compatibility: disabled native cube layers; using application per-eye rendering fallback (%u extensions)", kept);
    free(all);
    return result;
}
struct SpaceRecord { XrSpace space; char name[80]; };
static struct SpaceRecord space_records[32];
static unsigned space_record_count;
static uint32_t (*runtime_tracking_enabled)(void);
static uint64_t (*runtime_loader_version)(void);
static int runtime_diagnostic_resolved;
static double runtime_diagnostic_last;
static void log_runtime_tracking_locked(double seconds) {
    if (!runtime_diagnostic_resolved) {
        void *runtime = dlopen("/system/lib64/libvrruntimeclient.so", RTLD_NOW | RTLD_NOLOAD);
        if (runtime) {
            runtime_tracking_enabled = (uint32_t (*)(void))dlsym(runtime, "OpenXRApi_IsTrackingSettingEnabled");
            runtime_loader_version = (uint64_t (*)(void))dlsym(runtime, "android_globals_get_loader_version");
        }
        runtime_diagnostic_resolved = 1;
        LOG("runtime tracking getter=%p loader-version getter=%p", (void *)runtime_tracking_enabled, (void *)runtime_loader_version);
    }
    if (runtime_tracking_enabled && seconds - runtime_diagnostic_last >= 1.0) {
        uint64_t version = runtime_loader_version ? runtime_loader_version() : 0;
        LOG("runtime trackingEnabled=%u loader=%llu.%llu.%llu", runtime_tracking_enabled(),
            (unsigned long long)(version >> 48), (unsigned long long)((version >> 32) & 65535), (unsigned long long)(version & 0xffffffff));
        runtime_diagnostic_last = seconds;
    }
}
static void record_space_locked(XrSpace space, const char *name) {
    if (space_record_count < 32) {
        struct SpaceRecord *rec = &space_records[space_record_count++];
        rec->space = space;
        snprintf(rec->name, sizeof(rec->name), "%s", name);
    }
}
static const char *space_name_locked(XrSpace space) {
    for (unsigned i = 0; i < space_record_count; ++i) if (space_records[i].space == space) return space_records[i].name;
    return "unknown";
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateReferenceSpace(XrSession session, const XrReferenceSpaceCreateInfo *info, XrSpace *space) {
    XrResult result = REAL(xrCreateReferenceSpace)(session, info, space);
    if (is_yvr && XR_SUCCEEDED(result) && info && space) {
        char name[80];
        snprintf(name, sizeof(name), "reference-%d", info->referenceSpaceType);
        pthread_mutex_lock(&diagnostic_lock);
        record_space_locked(*space, name);
        pthread_mutex_unlock(&diagnostic_lock);
        const XrPosef *p = &info->poseInReferenceSpace;
        LOG("create space=%p %s p=(%.3f,%.3f,%.3f) q=(%.3f,%.3f,%.3f,%.3f)", (void *)*space, name,
            p->position.x,p->position.y,p->position.z,p->orientation.x,p->orientation.y,p->orientation.z,p->orientation.w);
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateActionSpace(XrSession session, const XrActionSpaceCreateInfo *info, XrSpace *space) {
    XrResult result = REAL(xrCreateActionSpace)(session, info, space);
    if (is_yvr && XR_SUCCEEDED(result) && info && space) {
        char name[80];
        pthread_mutex_lock(&diagnostic_lock);
        snprintf(name, sizeof(name), "%s-hand%llu", action_name_locked(info->action), (unsigned long long)info->subactionPath);
        record_space_locked(*space, name);
        pthread_mutex_unlock(&diagnostic_lock);
        const XrPosef *p = &info->poseInActionSpace;
        LOG("create space=%p %s p=(%.3f,%.3f,%.3f) q=(%.3f,%.3f,%.3f,%.3f)", (void *)*space, name,
            p->position.x,p->position.y,p->position.z,p->orientation.x,p->orientation.y,p->orientation.z,p->orientation.w);
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetSystemProperties(XrInstance instance, XrSystemId system, XrSystemProperties *properties) {
    XrResult result = REAL(xrGetSystemProperties)(instance, system, properties);
    if (is_yvr && XR_SUCCEEDED(result) && properties)
        LOG("system name=%.*s vendor=%u", XR_MAX_SYSTEM_NAME_SIZE-1, properties->systemName, properties->vendorId);
    return result;
}
static double last_view_sample;
static unsigned view_samples;
XRAPI_ATTR XrResult XRAPI_CALL xrLocateViews(XrSession session, const XrViewLocateInfo *info, XrViewState *state, uint32_t capacity, uint32_t *count, XrView *views) {
    XrResult result = REAL(xrLocateViews)(session, info, state, capacity, count, views);
    if (is_yvr && XR_SUCCEEDED(result) && info && count && *count && capacity && views) {
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        double seconds = now.tv_sec + now.tv_nsec * 1e-9;
        pthread_mutex_lock(&diagnostic_lock);
        if (view_samples < 120 && seconds - last_view_sample >= 1.0) {
            const XrPosef *p = &views[0].pose;
            const XrFovf *f = &views[0].fov;
            LOG("view eye0 base=%s p=(%.4f,%.4f,%.4f) q=(%.4f,%.4f,%.4f,%.4f) fov=(%.4f,%.4f,%.4f,%.4f) flags=0x%llx",
                space_name_locked(info->space),p->position.x,p->position.y,p->position.z,p->orientation.x,p->orientation.y,p->orientation.z,p->orientation.w,
                f->angleLeft,f->angleRight,f->angleUp,f->angleDown,(unsigned long long)(state ? state->viewStateFlags : 0));
            last_view_sample = seconds;
            ++view_samples;
        }
        pthread_mutex_unlock(&diagnostic_lock);
    }
    return result;
}
struct SpaceObservation { XrSpace space; XrSpaceLocationFlags flags; XrResult result; int used; double last_sample; unsigned samples; };
static struct SpaceObservation space_observations[16];
XRAPI_ATTR XrResult XRAPI_CALL xrLocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation *location) {
    XrResult result = REAL(xrLocateSpace)(space, base, time, location);
    if (is_yvr && location) {
        pthread_mutex_lock(&diagnostic_lock);
        for (unsigned i = 0; i < 16; ++i) {
            struct SpaceObservation *rec = &space_observations[i];
            if (!rec->used || rec->space == space) {
                XrSpaceLocationFlags flags = XR_SUCCEEDED(result) ? location->locationFlags : 0;
                if (!rec->used || rec->flags != flags || rec->result != result)
                    LOG("locate space=%p result=%d flags=0x%llx", (void *)space, result, (unsigned long long)flags);
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                double seconds = now.tv_sec + now.tv_nsec * 1e-9;
                if (XR_SUCCEEDED(result) && rec->samples < 120 && seconds - rec->last_sample >= 1.0) {
                    log_runtime_tracking_locked(seconds);
                    const XrPosef *p = &location->pose;
                    LOG("sample %s base=%s flags=0x%llx p=(%.4f,%.4f,%.4f) q=(%.4f,%.4f,%.4f,%.4f) t=%lld",
                        space_name_locked(space), space_name_locked(base), (unsigned long long)flags,
                        p->position.x,p->position.y,p->position.z,p->orientation.x,p->orientation.y,p->orientation.z,p->orientation.w,(long long)time);
                    rec->last_sample = seconds;
                    ++rec->samples;
                }
                rec->space = space;
                rec->flags = flags;
                rec->result = result;
                rec->used = 1;
                break;
            }
        }
        pthread_mutex_unlock(&diagnostic_lock);
    }
    return result;
}
// YVR's stereo compositor ignores FB VerticalFlip. Correct only known projection textures.
#define GPU_MAX_CHAINS 64
#define GPU_MAX_IMAGES 32
typedef struct { uint32_t index; int waited, flipped; } GpuAcquisition;
typedef struct {
    XrSwapchain handle; XrSwapchainCreateInfo info;
    GLuint images[GPU_MAX_IMAGES]; uint32_t image_count;
    GpuAcquisition queue[GPU_MAX_IMAGES]; uint32_t queued;
    int projection_flip; EGLContext context;
    GLuint read_fbo, scratch_fbo, scratch_texture;
    unsigned flips, warnings;
} GpuSwapchain;
static GpuSwapchain gpu_chains[GPU_MAX_CHAINS];
static GpuSwapchain *gpu_find(XrSwapchain handle) {
    for (unsigned i=0;i<GPU_MAX_CHAINS;++i) if (gpu_chains[i].handle==handle) return &gpu_chains[i];
    return NULL;
}
static int gpu_flip(GpuSwapchain *s, uint32_t index) {
    EGLContext context=eglGetCurrentContext();
    if (index>=s->image_count || context==EGL_NO_CONTEXT || (s->context && s->context!=context) ||
        s->info.arraySize!=1 || s->info.faceCount!=1 ||
        (s->info.format!=GL_SRGB8_ALPHA8 && s->info.format!=GL_RGBA8)) {
        if (s->warnings++<4) LOG("projection flip unavailable chain=%p index=%u images=%u format=0x%llx samples=%u context=%p",(void*)s->handle,index,s->image_count,(long long)s->info.format,s->info.sampleCount,(void*)context);
        return 0;
    }
    GLint old_read,old_draw,old_texture;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&old_read);
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&old_draw);
    glGetIntegerv(GL_TEXTURE_BINDING_2D,&old_texture);
    GLboolean scissor=glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    if (!s->scratch_texture) {
        s->context=context;
        glGenTextures(1,&s->scratch_texture);
        glBindTexture(GL_TEXTURE_2D,s->scratch_texture);
        glTexStorage2D(GL_TEXTURE_2D,1,(GLenum)s->info.format,(GLsizei)s->info.width,(GLsizei)s->info.height);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glBindTexture(GL_TEXTURE_2D,(GLuint)old_texture);
        glGenFramebuffers(1,&s->read_fbo);
        glGenFramebuffers(1,&s->scratch_fbo);
    }
    glBindFramebuffer(GL_READ_FRAMEBUFFER,s->read_fbo);
    // Android's multisampled-render-to-texture extension still exposes a normal 2D image.
    glFramebufferTexture2D(GL_READ_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->images[index],0);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s->scratch_fbo);
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,s->scratch_texture,0);
    GLenum attachment=GL_COLOR_ATTACHMENT0;
    glDrawBuffers(1,&attachment);
    GLenum read_status=glCheckFramebufferStatus(GL_READ_FRAMEBUFFER);
    GLenum draw_status=glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER);
    int ok=read_status==GL_FRAMEBUFFER_COMPLETE && draw_status==GL_FRAMEBUFFER_COMPLETE;
    if (ok) {
        GLint w=(GLint)s->info.width,h=(GLint)s->info.height;
        glBlitFramebuffer(0,0,w,h,0,h,w,0,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        glBindFramebuffer(GL_READ_FRAMEBUFFER,s->scratch_fbo);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s->read_fbo);
        glDrawBuffers(1,&attachment);
        glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
        if (++s->flips==1) LOG("projection GPU flip active chain=%p image=%u texture=%u size=%ux%u format=0x%llx samples=%u FBO=complete",(void*)s->handle,index,s->images[index],s->info.width,s->info.height,(long long)s->info.format,s->info.sampleCount);
    } else if (s->warnings++<4) LOG("projection flip incomplete FBO chain=%p read=0x%x draw=0x%x",(void*)s->handle,read_status,draw_status);
    glBindFramebuffer(GL_READ_FRAMEBUFFER,(GLuint)old_read);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,(GLuint)old_draw);
    if (scissor) glEnable(GL_SCISSOR_TEST);
    return ok;
}
XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession session,const XrSwapchainCreateInfo *info,XrSwapchain *handle) {
    XrResult result=REAL(xrCreateSwapchain)(session,info,handle);
    if (is_yvr && XR_SUCCEEDED(result) && info && handle) {
        for (unsigned i=0;i<GPU_MAX_CHAINS;++i) if (!gpu_chains[i].handle) {
            gpu_chains[i].handle=*handle; gpu_chains[i].info=*info;
            LOG("swapchain tracked chain=%p size=%ux%u format=0x%llx samples=%u faces=%u array=%u",(void*)*handle,info->width,info->height,(long long)info->format,info->sampleCount,info->faceCount,info->arraySize);
            break;
        }
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain handle,uint32_t capacity,uint32_t *count,XrSwapchainImageBaseHeader *images) {
    XrResult result=REAL(xrEnumerateSwapchainImages)(handle,capacity,count,images);
    GpuSwapchain *s=is_yvr?gpu_find(handle):NULL;
    if (s && XR_SUCCEEDED(result) && count && images && capacity>=*count && *count<=GPU_MAX_IMAGES && images->type==XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR) {
        const XrSwapchainImageOpenGLESKHR *gl=(const void*)images;
        s->image_count=*count;
        for (unsigned i=0;i<*count;++i) s->images[i]=gl[i].image;
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain handle,const XrSwapchainImageAcquireInfo *info,uint32_t *index) {
    XrResult result=REAL(xrAcquireSwapchainImage)(handle,info,index);
    GpuSwapchain *s=is_yvr?gpu_find(handle):NULL;
    if (s && result==XR_SUCCESS && index && s->queued<GPU_MAX_IMAGES) s->queue[s->queued++]=(GpuAcquisition){.index=*index};
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain handle,const XrSwapchainImageWaitInfo *info) {
    XrResult result=REAL(xrWaitSwapchainImage)(handle,info);
    GpuSwapchain *s=is_yvr?gpu_find(handle):NULL;
    if (s && result==XR_SUCCESS) for (unsigned i=0;i<s->queued;++i) if (!s->queue[i].waited) { s->queue[i].waited=1; break; }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain handle,const XrSwapchainImageReleaseInfo *info) {
    GpuSwapchain *s=is_yvr?gpu_find(handle):NULL;
    if (s && s->projection_flip && s->queued && s->queue[0].waited && !s->queue[0].flipped)
        s->queue[0].flipped=gpu_flip(s,s->queue[0].index);
    XrResult result=REAL(xrReleaseSwapchainImage)(handle,info);
    if (s && result==XR_SUCCESS && s->queued) { --s->queued; memmove(s->queue,s->queue+1,s->queued*sizeof(s->queue[0])); }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain handle) {
    GpuSwapchain *s=is_yvr?gpu_find(handle):NULL;
    XrResult result=REAL(xrDestroySwapchain)(handle);
    if (s && result==XR_SUCCESS) {
        if (s->scratch_texture && s->context==eglGetCurrentContext()) {
            glDeleteTextures(1,&s->scratch_texture); glDeleteFramebuffers(1,&s->read_fbo); glDeleteFramebuffers(1,&s->scratch_fbo);
        }
        memset(s,0,sizeof(*s));
    }
    return result;
}
XRAPI_ATTR XrResult XRAPI_CALL xrEndFrame(XrSession session,const XrFrameEndInfo *info) {
    if (is_yvr && info && info->layers && info->layerCount<=32) for (unsigned i=0;i<info->layerCount;++i) {
        const XrCompositionLayerBaseHeader *layer=info->layers[i];
        if (!layer || layer->type!=XR_TYPE_COMPOSITION_LAYER_PROJECTION) continue;
        const XrCompositionLayerProjection *p=(const void*)layer;
        int flip=0; unsigned depth=0;
        for (const XrBaseInStructure *n=p->next;n && depth++<16;n=n->next) if (n->type==XR_TYPE_COMPOSITION_LAYER_IMAGE_LAYOUT_FB)
            flip=(((const XrCompositionLayerImageLayoutFB*)n)->flags&XR_COMPOSITION_LAYER_IMAGE_LAYOUT_VERTICAL_FLIP_BIT_FB)!=0;
        if (!flip || !p->views || p->viewCount>4) continue;
        for (unsigned j=0;j<p->viewCount;++j) {
            GpuSwapchain *s=gpu_find(p->views[j].subImage.swapchain);
            if (s && !s->projection_flip) { s->projection_flip=1; LOG("projection VerticalFlip registered chain=%p; GPU correction from next acquired image",(void*)s->handle); }
        }
    }
    return REAL(xrEndFrame)(session,info);
}
XRAPI_ATTR XrResult XRAPI_CALL xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    XrResult result = REAL(xrGetInstanceProcAddr)(instance, name, function);
    if (XR_SUCCEEDED(result) && name && function && *function) {
#define HOOK(fn) if (!strcmp(name, #fn)) *function = (PFN_xrVoidFunction)fn
        HOOK(xrSuggestInteractionProfileBindings);
        else HOOK(xrGetInstanceProcAddr);
        else HOOK(xrCreateAction);
        else HOOK(xrGetActionStatePose);
        else HOOK(xrGetActionStateFloat);
        else HOOK(xrLocateSpace);
        else HOOK(xrCreateActionSpace);
        else HOOK(xrCreateReferenceSpace);
        else HOOK(xrGetSystemProperties);
        else HOOK(xrEnumerateInstanceExtensionProperties);
        else HOOK(xrLocateViews);
        else HOOK(xrEndFrame);
        else HOOK(xrCreateSwapchain);
        else HOOK(xrEnumerateSwapchainImages);
        else HOOK(xrAcquireSwapchainImage);
        else HOOK(xrWaitSwapchainImage);
        else HOOK(xrReleaseSwapchainImage);
        else HOOK(xrDestroySwapchain);
#undef HOOK
    }
    return result;
}
