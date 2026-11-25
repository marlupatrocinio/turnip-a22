/*
 * vkhal_shim - expose an Android Vulkan HAL (HMI) as a plain Vulkan library.
 *
 * Turnip's Android build exports only `HMI`, the hwvulkan module. Tools that
 * load a Vulkan library by path (dEQP-VK --deqp-vk-library-path, ad-hoc
 * benchmarks) expect `vkGetInstanceProcAddr`. This shim dlopens the HAL,
 * opens HWVULKAN_DEVICE_0 and forwards vkGetInstanceProcAddr to the device's
 * GetInstanceProcAddr - exactly what a22test does, no loader involved.
 *
 * Driver path: $VKHAL_DRIVER, else libvulkan_test.so next to this shim.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "android_hwvulkan.h"
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr hal_gipa;
static pthread_once_t once = PTHREAD_ONCE_INIT;

static void load_hal(void)
{
    char path[4096];
    const char *env = getenv("VKHAL_DRIVER");
    if (env && *env) {
        snprintf(path, sizeof(path), "%s", env);
    } else {
        Dl_info info;
        if (!dladdr((void *)load_hal, &info) || !info.dli_fname) {
            fprintf(stderr, "vkhal_shim: dladdr failed\n");
            return;
        }
        snprintf(path, sizeof(path), "%s", info.dli_fname);
        char *slash = strrchr(path, '/');
        size_t dir = slash ? (size_t)(slash - path + 1) : 0;
        snprintf(path + dir, sizeof(path) - dir, "libvulkan_test.so");
    }

    void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "vkhal_shim: dlopen(%s): %s\n", path, dlerror());
        return;
    }
    hwvulkan_module_t *mod = (hwvulkan_module_t *)dlsym(h, "HMI");
    if (!mod) {
        fprintf(stderr, "vkhal_shim: %s has no HMI symbol\n", path);
        return;
    }
    struct hw_device_t *dev = NULL;
    int rc = mod->common.methods->open(&mod->common, HWVULKAN_DEVICE_0, &dev);
    if (rc != 0 || !dev) {
        fprintf(stderr, "vkhal_shim: HAL open failed: %d\n", rc);
        return;
    }
    hal_gipa = ((hwvulkan_device_t *)dev)->GetInstanceProcAddr;
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vkGetInstanceProcAddr(VkInstance instance, const char *name)
{
    pthread_once(&once, load_hal);
    if (!hal_gipa)
        return NULL;
    if (name && strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkGetInstanceProcAddr;
    return hal_gipa(instance, name);
}

__attribute__((visibility("default")))
PFN_vkVoidFunction vk_icdGetInstanceProcAddr(VkInstance instance, const char *name)
{
    return vkGetInstanceProcAddr(instance, name);
}
