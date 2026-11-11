/*
 * Minimal Android Vulkan HAL ABI, self-contained for the probe/test tools.
 *
 * Derived from AOSP hardware/libhardware/include/hardware/hardware.h and
 * hardware/hwvulkan.h (Copyright (C) 2008, 2015 The Android Open Source
 * Project, Apache License 2.0). Only the structs needed to open an
 * Android Vulkan HAL and reach its entry points are reproduced here, so the
 * tools build against the NDK alone (no Mesa source tree, no cutils).
 */
#ifndef A22_ANDROID_HWVULKAN_H
#define A22_ANDROID_HWVULKAN_H

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define HARDWARE_MODULE_TAG 0x48574D54u /* 'HWMT' */
#define HARDWARE_DEVICE_TAG 0x48574454u /* 'HWDT' */
#define HWVULKAN_DEVICE_0 "vk0"

typedef struct native_handle {
    int version;
    int numFds;
    int numInts;
    int data[0];
} native_handle_t;

typedef const native_handle_t *buffer_handle_t;

struct hw_module_t;
struct hw_module_methods_t;
struct hw_device_t;

typedef struct hw_module_t {
    uint32_t tag;
    uint16_t module_api_version;
    uint16_t hal_api_version;
    const char *id;
    const char *name;
    const char *author;
    struct hw_module_methods_t *methods;
    void *dso;
    uint32_t reserved[32 - 7];
} hw_module_t;

typedef struct hw_module_methods_t {
    int (*open)(const struct hw_module_t *module, const char *id,
                struct hw_device_t **device);
} hw_module_methods_t;

typedef struct hw_device_t {
    uint32_t tag;
    uint32_t version;
    struct hw_module_t *module;
#ifdef __LP64__
    uint64_t reserved[12];
#else
    uint32_t reserved[12];
#endif
    int (*close)(struct hw_device_t *device);
} hw_device_t;

typedef struct hwvulkan_device_t {
    struct hw_device_t common;

    PFN_vkEnumerateInstanceExtensionProperties
        EnumerateInstanceExtensionProperties;
    PFN_vkCreateInstance CreateInstance;
    PFN_vkGetInstanceProcAddr GetInstanceProcAddr;
} hwvulkan_device_t;

typedef struct hwvulkan_module_t {
    struct hw_module_t common;
} hwvulkan_module_t;

#ifdef __cplusplus
}
#endif

#endif /* A22_ANDROID_HWVULKAN_H */

