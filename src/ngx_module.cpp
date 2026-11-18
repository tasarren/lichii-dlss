// Lichii DLSS module: NVIDIA NGX Vulkan DLSS behind Lichii's C ABI.
//
// Copyright (c) 2026 The Lichii DLSS module authors
//
// Licensed under the MIT licence; see LICENSE. This software contains
// source code provided by NVIDIA Corporation. The libnvsdk_ngx.a object code
// linked into this module is covered by NVIDIA's licence terms, which ship
// alongside the release as NVIDIA-LICENSE.txt.

#include <dlfcn.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>

#include <vulkan/vulkan.h>

extern "C" {
#include "ngx_module.h"
}

// The NGX SDK's Vulkan subset. The general helper header pulls in the D3D
// and CUDA helpers, which do not compile on Linux, so the two helper macros
// this module needs (optimal settings and stats) live inline below.
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_defs.h>
#include <nvsdk_ngx_defs_vk.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>

#define NGX_MODULE_EXPORT extern "C" __attribute__((visibility("default")))

namespace {

// A GUID-like project id, as NGX requires for custom engines.
constexpr char kProjectId[] = "6f2c4e1b-9a3d-4f58-b8e7-2d1c0a9b5f34";
constexpr char kEngineVersion[] = "1.0";

bool g_inited = false;
VkDevice g_vk_device = nullptr;
NVSDK_NGX_Parameter* g_caps = nullptr;
NVSDK_NGX_Parameter* g_params = nullptr;
void (*g_log)(const char* message, int level) = nullptr;
char g_last_error[256] = "";

void set_error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vsnprintf(g_last_error, sizeof(g_last_error), format, args);
    va_end(args);
}

const char* result_name(NVSDK_NGX_Result result) {
    switch (result) {
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
        return "feature not supported";
    case NVSDK_NGX_Result_FAIL_PlatformError:
        return "platform error";
    case NVSDK_NGX_Result_FAIL_InvalidParameter:
        return "invalid parameter";
    case NVSDK_NGX_Result_FAIL_MissingInput:
        return "missing input";
    case NVSDK_NGX_Result_FAIL_OutOfDate:
        return "out of date";
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
        return "out of GPU memory";
    case NVSDK_NGX_Result_FAIL_UnsupportedFormat:
        return "unsupported format";
    case NVSDK_NGX_Result_FAIL_UnableToInitializeFeature:
        return "unable to initialize feature";
    case NVSDK_NGX_Result_FAIL_UnableToWriteToAppDataPath:
        return "unable to write to the application data path";
    default:
        return nullptr;
    }
}

int set_ngx_error(const char* what, NVSDK_NGX_Result result) {
    const char* name = result_name(result);
    if (name) {
        set_error("%s: %s (0x%x)", what, name, static_cast<unsigned>(result));
    } else {
        set_error("%s: NGX error 0x%x", what, static_cast<unsigned>(result));
    }
    return NGX_MODULE_ERROR;
}

int map_init_result(NVSDK_NGX_Result result) {
    switch (result) {
    case NVSDK_NGX_Result_FAIL_OutOfGPUMemory:
        set_ngx_error("NVSDK_NGX_VULKAN_Init_with_ProjectID", result);
        return NGX_MODULE_OUT_OF_MEMORY;
    case NVSDK_NGX_Result_FAIL_FeatureNotSupported:
        set_ngx_error("NVSDK_NGX_VULKAN_Init_with_ProjectID", result);
        return NGX_MODULE_UNSUPPORTED;
    default:
        return set_ngx_error("NVSDK_NGX_VULKAN_Init_with_ProjectID", result);
    }
}

void NVSDK_CONV app_log(const char* message, NVSDK_NGX_Logging_Level level,
                        NVSDK_NGX_Feature source) {
    if (g_log) {
        g_log(message, static_cast<int>(level));
    }
}

// NGX widens paths itself with the C locale, so callers must hand over ASCII
// (or a /proc/self/fd alias) in the first place.
std::wstring widen_ascii(const char* path) {
    std::wstring wide;
    for (const char* p = path; *p != '\0'; ++p) {
        wide.push_back(static_cast<unsigned char>(*p));
    }
    return wide;
}

NVSDK_NGX_Resource_VK make_resource(const ngx_module_image* image, bool read_write) {
    VkImageSubresourceRange range{image->aspect, 0, image->level_count, 0, image->layer_count};
    return NVSDK_NGX_Create_ImageView_Resource_VK(reinterpret_cast<VkImageView>(image->view),
                                                  reinterpret_cast<VkImage>(image->image), range,
                                                  static_cast<VkFormat>(image->format),
                                                  image->width, image->height, read_write);
}

int require_init() {
    if (!g_inited) {
        set_error("module is not initialized");
        return NGX_MODULE_ERROR;
    }
    return NGX_MODULE_OK;
}

} // namespace

NGX_MODULE_EXPORT uint32_t ngx_module_abi(void) {
    return NGX_MODULE_ABI;
}

NGX_MODULE_EXPORT const char* ngx_module_version(void) {
    return NGX_MODULE_VERSION_STRING;
}

NGX_MODULE_EXPORT const char* ngx_module_dlss_version(void) {
    return NGX_MODULE_DLSS_VERSION;
}

NGX_MODULE_EXPORT const char* ngx_module_last_error(void) {
    return g_last_error;
}

NGX_MODULE_EXPORT int ngx_module_init(const struct ngx_module_init_params* init) {
    if (g_inited) {
        set_error("module is already initialized");
        return NGX_MODULE_ERROR;
    }
    if (init == nullptr || init->device == 0 || init->data_path == nullptr ||
        init->get_device_proc_addr == nullptr) {
        set_error("invalid init parameters");
        return NGX_MODULE_ERROR;
    }

    // The NGX core lives in the display driver; fail before touching it.
    if (void* core = dlopen("libnvidia-ngx.so.1", RTLD_LAZY | RTLD_LOCAL)) {
        dlclose(core);
    } else {
        return NGX_MODULE_NO_NVIDIA;
    }

    const std::wstring data_path = widen_ascii(init->data_path);
    g_log = init->log;

    NVSDK_NGX_FeatureCommonInfo common{};
    const wchar_t* const paths[] = {data_path.c_str()};
    common.PathListInfo.Path = paths;
    common.PathListInfo.Length = 1;
    if (g_log) {
        common.LoggingInfo.LoggingCallback = &app_log;
        common.LoggingInfo.MinimumLoggingLevel = NVSDK_NGX_LOGGING_LEVEL_ON;
        common.LoggingInfo.DisableOtherLoggingSinks = true;
    }

    NVSDK_NGX_Result result = NVSDK_NGX_VULKAN_Init_with_ProjectID(
        kProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, kEngineVersion, data_path.c_str(),
        reinterpret_cast<VkInstance>(init->instance),
        reinterpret_cast<VkPhysicalDevice>(init->physical_device),
        reinterpret_cast<VkDevice>(init->device),
        reinterpret_cast<PFN_vkGetInstanceProcAddr>(init->get_instance_proc_addr),
        reinterpret_cast<PFN_vkGetDeviceProcAddr>(init->get_device_proc_addr), &common,
        NVSDK_NGX_Version_API);
    if (NVSDK_NGX_FAILED(result)) {
        g_log = nullptr;
        return map_init_result(result);
    }

    result = NVSDK_NGX_VULKAN_GetCapabilityParameters(&g_caps);
    if (NVSDK_NGX_SUCCEED(result)) {
        result = NVSDK_NGX_VULKAN_AllocateParameters(&g_params);
    }
    if (NVSDK_NGX_FAILED(result)) {
        if (g_caps) {
            NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
            g_caps = nullptr;
        }
        NVSDK_NGX_VULKAN_Shutdown1(reinterpret_cast<VkDevice>(init->device));
        g_log = nullptr;
        return map_init_result(result);
    }

    g_vk_device = reinterpret_cast<VkDevice>(init->device);
    g_inited = true;
    return NGX_MODULE_OK;
}

NGX_MODULE_EXPORT void ngx_module_shutdown(void) {
    if (!g_inited) {
        return;
    }
    if (g_params) {
        NVSDK_NGX_VULKAN_DestroyParameters(g_params);
        g_params = nullptr;
    }
    if (g_caps) {
        NVSDK_NGX_VULKAN_DestroyParameters(g_caps);
        g_caps = nullptr;
    }
    NVSDK_NGX_VULKAN_Shutdown1(g_vk_device);
    g_vk_device = nullptr;
    g_log = nullptr;
    g_inited = false;
}

NGX_MODULE_EXPORT int ngx_module_caps(struct ngx_module_caps_info* out) {
    if (int status = require_init()) {
        return status;
    }
    if (out == nullptr) {
        set_error("caps output pointer is null");
        return NGX_MODULE_ERROR;
    }

    *out = {};
    int available = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(g_caps, NVSDK_NGX_Parameter_SuperSampling_Available,
                                                   &available))) {
        out->available = available;
    }
    int needs_update = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
            g_caps, NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_update))) {
        out->needs_updated_driver = needs_update;
    }
    unsigned int major = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(
            g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &major))) {
        out->min_driver_major = major;
    }
    unsigned int minor = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetUI(
            g_caps, NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &minor))) {
        out->min_driver_minor = minor;
    }
    int init_result = 0;
    if (NVSDK_NGX_SUCCEED(NVSDK_NGX_Parameter_GetI(
            g_caps, NVSDK_NGX_Parameter_SuperSampling_FeatureInitResult, &init_result))) {
        out->feature_init_result = init_result;
    }
    return NGX_MODULE_OK;
}

NGX_MODULE_EXPORT int ngx_module_optimal_settings(uint32_t output_width, uint32_t output_height,
                                                  int quality, uint32_t* opt_width,
                                                  uint32_t* opt_height, uint32_t* min_width,
                                                  uint32_t* min_height, uint32_t* max_width,
                                                  uint32_t* max_height, float* sharpness) {
    if (int status = require_init()) {
        return status;
    }

    // The optimal-settings callback only exists on the capability parameters,
    // not on plain allocated parameters.
    void* callback = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, NVSDK_NGX_Parameter_DLSSOptimalSettingsCallback,
                                       &callback);
    if (callback == nullptr) {
        set_error("the installed DLSS library does not report optimal settings");
        return NGX_MODULE_UNSUPPORTED;
    }

    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Width, output_width);
    NVSDK_NGX_Parameter_SetUI(g_caps, NVSDK_NGX_Parameter_Height, output_height);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_PerfQualityValue, quality);
    NVSDK_NGX_Parameter_SetI(g_caps, NVSDK_NGX_Parameter_RTXValue, 0);

    auto get_settings = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetOptimalSettingsCallback>(callback);
    NVSDK_NGX_Result result = get_settings(g_caps);
    if (NVSDK_NGX_FAILED(result)) {
        return set_ngx_error("DLSS optimal settings", result);
    }

    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutWidth, opt_width);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_OutHeight, opt_height);
    // Older DLSS libraries only report the optimal size.
    *max_width = *opt_width;
    *max_height = *opt_height;
    *min_width = *opt_width;
    *min_height = *opt_height;
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Width,
                              max_width);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Max_Render_Height,
                              max_height);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Width,
                              min_width);
    NVSDK_NGX_Parameter_GetUI(g_caps, NVSDK_NGX_Parameter_DLSS_Get_Dynamic_Min_Render_Height,
                              min_height);
    NVSDK_NGX_Parameter_GetF(g_caps, NVSDK_NGX_Parameter_Sharpness, sharpness);
    return NGX_MODULE_OK;
}

NGX_MODULE_EXPORT uint64_t ngx_module_create(uint64_t command_buffer,
                                             const struct ngx_module_feature_create* create) {
    if (int status = require_init()) {
        return 0;
    }
    if (command_buffer == 0 || create == nullptr) {
        set_error("invalid create parameters");
        return 0;
    }

    int flags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes;
    if (create->hdr) {
        flags |= NVSDK_NGX_DLSS_Feature_Flags_IsHDR;
    }
    if (create->auto_exposure) {
        flags |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
    }
    if (create->depth_inverted) {
        flags |= NVSDK_NGX_DLSS_Feature_Flags_DepthInverted;
    }

    // The preset applies to every quality mode; NGX keeps per-mode hints.
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, create->preset);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, create->preset);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, create->preset);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, create->preset);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, create->preset);
    NVSDK_NGX_Parameter_SetUI(g_params, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, create->preset);

    NVSDK_NGX_DLSS_Create_Params params{};
    params.Feature.InWidth = create->render_width;
    params.Feature.InHeight = create->render_height;
    params.Feature.InTargetWidth = create->output_width;
    params.Feature.InTargetHeight = create->output_height;
    params.Feature.InPerfQualityValue = static_cast<NVSDK_NGX_PerfQuality_Value>(create->quality);
    params.InFeatureCreateFlags = flags;
    params.InEnableOutputSubrects = false;

    NVSDK_NGX_Handle* handle = nullptr;
    NVSDK_NGX_Result result = NGX_VULKAN_CREATE_DLSS_EXT(
        reinterpret_cast<VkCommandBuffer>(command_buffer), 1, 1, &handle, g_params, &params);
    if (NVSDK_NGX_FAILED(result)) {
        set_ngx_error("create DLSS feature", result);
        return 0;
    }
    return reinterpret_cast<uint64_t>(handle);
}

NGX_MODULE_EXPORT int ngx_module_release(uint64_t feature) {
    if (int status = require_init()) {
        return status;
    }
    if (feature == 0) {
        set_error("invalid feature handle");
        return NGX_MODULE_ERROR;
    }
    NVSDK_NGX_Result result =
        NVSDK_NGX_VULKAN_ReleaseFeature(reinterpret_cast<NVSDK_NGX_Handle*>(feature));
    if (NVSDK_NGX_FAILED(result)) {
        return set_ngx_error("release DLSS feature", result);
    }
    return NGX_MODULE_OK;
}

NGX_MODULE_EXPORT int ngx_module_eval(uint64_t feature, uint64_t command_buffer,
                                      const struct ngx_module_eval_params* eval) {
    if (int status = require_init()) {
        return status;
    }
    if (feature == 0 || command_buffer == 0 || eval == nullptr) {
        set_error("invalid eval parameters");
        return NGX_MODULE_ERROR;
    }

    NVSDK_NGX_Resource_VK color = make_resource(&eval->color, false);
    NVSDK_NGX_Resource_VK depth = make_resource(&eval->depth, false);
    NVSDK_NGX_Resource_VK motion_vectors = make_resource(&eval->motion_vectors, false);
    NVSDK_NGX_Resource_VK output = make_resource(&eval->output, true);

    NVSDK_NGX_VK_DLSS_Eval_Params params{};
    params.Feature.pInColor = &color;
    params.Feature.pInOutput = &output;
    params.Feature.InSharpness = eval->sharpness;
    params.pInDepth = &depth;
    params.pInMotionVectors = &motion_vectors;
    params.InJitterOffsetX = eval->jitter_x;
    params.InJitterOffsetY = eval->jitter_y;
    params.InRenderSubrectDimensions.Width = eval->render_width;
    params.InRenderSubrectDimensions.Height = eval->render_height;
    params.InReset = eval->reset;
    params.InMVScaleX = eval->mv_scale_x;
    params.InMVScaleY = eval->mv_scale_y;

    NVSDK_NGX_Result result = NGX_VULKAN_EVALUATE_DLSS_EXT(
        reinterpret_cast<VkCommandBuffer>(command_buffer),
        reinterpret_cast<NVSDK_NGX_Handle*>(feature), g_params, &params);
    if (NVSDK_NGX_FAILED(result)) {
        return set_ngx_error("evaluate DLSS feature", result);
    }
    return NGX_MODULE_OK;
}

NGX_MODULE_EXPORT int ngx_module_stats(uint64_t* vram_bytes) {
    if (int status = require_init()) {
        return status;
    }
    if (vram_bytes == nullptr) {
        set_error("stats output pointer is null");
        return NGX_MODULE_ERROR;
    }

    // As with optimal settings, the stats callback lives on the capability
    // parameters only.
    void* callback = nullptr;
    NVSDK_NGX_Parameter_GetVoidPointer(g_caps, NVSDK_NGX_Parameter_DLSSGetStatsCallback, &callback);
    if (callback == nullptr) {
        set_error("the installed DLSS library does not report stats");
        return NGX_MODULE_UNSUPPORTED;
    }

    auto get_stats = reinterpret_cast<PFN_NVSDK_NGX_DLSS_GetStatsCallback>(callback);
    NVSDK_NGX_Result result = get_stats(g_caps);
    if (NVSDK_NGX_FAILED(result)) {
        return set_ngx_error("DLSS stats", result);
    }

    unsigned long long bytes = 0;
    NVSDK_NGX_Parameter_GetULL(g_caps, NVSDK_NGX_Parameter_SizeInBytes, &bytes);
    *vram_bytes = bytes;
    return NGX_MODULE_OK;
}
