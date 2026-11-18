/*
 * Lichii DLSS module - the C ABI between Lichii and its NVIDIA NGX backend.
 *
 * Copyright (c) 2026 The Lichii DLSS module authors
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */
#ifndef LICHII_DLSS_NGX_MODULE_H
#define LICHII_DLSS_NGX_MODULE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bump when the ABI below changes. Lichii refuses to load a module that
 * reports a different value. */
#define NGX_MODULE_ABI 1

/* Status codes returned by module entry points. */
enum {
    NGX_MODULE_OK = 0,
    NGX_MODULE_ERROR = 1,          /* generic; see ngx_module_last_error() */
    NGX_MODULE_NO_NVIDIA = 2,      /* driver NGX core (libnvidia-ngx.so.1) missing */
    NGX_MODULE_DRIVER_TOO_OLD = 3, /* driver below the NGX-reported minimum */
    NGX_MODULE_UNSUPPORTED = 4,    /* NGX or DLSS unsupported on this system */
    NGX_MODULE_OUT_OF_MEMORY = 5,
};

/* Quality values; the numbers match the NGX perf-quality values. */
enum {
    NGX_MODULE_QUALITY_PERFORMANCE = 0,
    NGX_MODULE_QUALITY_BALANCED = 1,
    NGX_MODULE_QUALITY_QUALITY = 2,
    NGX_MODULE_QUALITY_ULTRA_PERFORMANCE = 3,
    NGX_MODULE_QUALITY_ULTRA_QUALITY = 4,
    NGX_MODULE_QUALITY_DLAA = 5,
};

/* DLSS rendering presets; the numbers match the NGX preset hint values. */
enum {
    NGX_MODULE_PRESET_DEFAULT = 0,
    NGX_MODULE_PRESET_E = 5,
    NGX_MODULE_PRESET_F = 6,
    NGX_MODULE_PRESET_J = 10,
    NGX_MODULE_PRESET_K = 11,
    NGX_MODULE_PRESET_L = 12,
    NGX_MODULE_PRESET_M = 13,
};

/* The Vulkan handles NGX init needs. */
struct ngx_module_init_params {
    uint64_t instance;           /* VkInstance */
    uint64_t physical_device;     /* VkPhysicalDevice */
    uint64_t device;              /* VkDevice */
    void* get_instance_proc_addr; /* PFN_vkGetInstanceProcAddr; may be NULL */
    void* get_device_proc_addr;  /* PFN_vkGetDeviceProcAddr */
    const char* data_path;        /* NGX scratch dir; must be ASCII on Linux */
    void (*log)(const char* message, int level); /* may be NULL */
};

struct ngx_module_caps_info {
    uint32_t min_driver_major;
    uint32_t min_driver_minor;
    int32_t available;            /* NGX reports DLSS available */
    int32_t needs_updated_driver;
    int32_t feature_init_result;  /* NGX result of the last feature init attempt */
};

struct ngx_module_feature_create {
    uint32_t render_width;  /* input size */
    uint32_t render_height;
    uint32_t output_width;   /* upscaled size */
    uint32_t output_height;
    int32_t quality;         /* NGX_MODULE_QUALITY_* */
    int32_t preset;          /* NGX_MODULE_PRESET_* */
    int32_t hdr;
    int32_t auto_exposure;
    int32_t depth_inverted;
};

/* One Vulkan image taking part in an evaluation. */
struct ngx_module_image {
    uint64_t view;   /* VkImageView */
    uint64_t image;  /* VkImage */
    int32_t format;  /* VkFormat */
    uint32_t aspect; /* VkImageAspectFlags */
    uint32_t level_count;
    uint32_t layer_count;
    uint32_t width;
    uint32_t height;
};

/* One DLSS evaluation: the resources NGX reads and writes. */
struct ngx_module_eval_params {
    struct ngx_module_image color;
    struct ngx_module_image depth;
    struct ngx_module_image motion_vectors;
    struct ngx_module_image output;
    uint32_t render_width;  /* subrect used within the inputs */
    uint32_t render_height;
    float jitter_x;
    float jitter_y;
    float mv_scale_x;
    float mv_scale_y;
    float sharpness;
    int32_t reset;
};

uint32_t ngx_module_abi(void);
const char* ngx_module_version(void);
const char* ngx_module_dlss_version(void);
const char* ngx_module_last_error(void);

int ngx_module_init(const struct ngx_module_init_params* init);
void ngx_module_shutdown(void);
int ngx_module_caps(struct ngx_module_caps_info* out);

/* Optimal, minimum and maximum render sizes for a target output size. */
int ngx_module_optimal_settings(uint32_t output_width, uint32_t output_height,
                                int quality, uint32_t* opt_width, uint32_t* opt_height,
                                uint32_t* min_width, uint32_t* min_height,
                                uint32_t* max_width, uint32_t* max_height,
                                float* sharpness);

/* Returns 0 on failure, otherwise an opaque feature handle. */
uint64_t ngx_module_create(uint64_t command_buffer,
                           const struct ngx_module_feature_create* create);
int ngx_module_release(uint64_t feature);
int ngx_module_eval(uint64_t feature, uint64_t command_buffer,
                    const struct ngx_module_eval_params* eval);

/* Feature VRAM use in bytes. */
int ngx_module_stats(uint64_t* vram_bytes);

#ifdef __cplusplus
}
#endif

#endif /* LICHII_DLSS_NGX_MODULE_H */
