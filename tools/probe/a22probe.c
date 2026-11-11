/*
 * Snapdragon G2 Gen 2 (Adreno A22) driver validation probe.
 *
 * Loads a Turnip Android Vulkan HAL (the form AdrenoTools packages ship:
 * libvulkan_freedreno.so exporting HMI), creates a VkInstance/VkDevice
 * through the HAL entrypoints and prints driver properties.  Also dumps the
 * raw KGSL device properties used for device-profile tuning.
 *
 * Build:
 *   clang --target=aarch64-linux-android35 --sysroot=$NDK/.../sysroot \
 *         -I <mesa>/include/android_stub -O2 -fPIE -pie -o a22probe a22probe.c -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <dlfcn.h>

#include "msm_kgsl.h"
#include "android_hwvulkan.h"

static void kgsl_probe(void)
{
    int fd = open("/dev/kgsl-3d0", O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        printf("KGSL: open /dev/kgsl-3d0 failed: %s\n", strerror(errno));
        return;
    }
    struct kgsl_devinfo info;
    memset(&info, 0, sizeof(info));
    struct kgsl_device_getproperty gp = {
        .type = KGSL_PROP_DEVICE_INFO,
        .value = &info,
        .sizebytes = sizeof(info),
    };
    if (ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &gp) == 0) {
        printf("chip_id=0x%08x gpu_id=%u gmem=%u KiB mmu=%u\n",
               info.chip_id, info.gpu_id, (unsigned)(info.gmem_sizebytes / 1024),
               info.mmu_enabled);
    }
    uint32_t u32;
    struct kgsl_device_getproperty p = { .value = &u32, .sizebytes = sizeof(u32) };
    p.type = KGSL_PROP_UBWC_MODE;
    if (ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &p) == 0)
        printf("ubwc_mode=0x%x\n", u32);
    p.type = KGSL_PROP_HIGHEST_BANK_BIT;
    if (ioctl(fd, IOCTL_KGSL_DEVICE_GETPROPERTY, &p) == 0)
        printf("highest_bank_bit=%u\n", u32);
    close(fd);
}

int main(int argc, char **argv)
{
    const char *lib = argc > 1 ? argv[1] : "libvulkan_freedreno.so";
    printf("=== A22 driver validation ===\n");
    kgsl_probe();

    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("dlopen(%s) failed: %s\n", lib, dlerror());
        return 1;
    }
    printf("dlopen ok: %s\n", lib);

    PFN_vkGetInstanceProcAddr gipa = NULL;
    PFN_vkCreateInstance create_instance = NULL;

    hwvulkan_module_t *mod = (hwvulkan_module_t *)dlsym(h, "HMI");
    if (mod) {
        printf("HMI found (Android Vulkan HAL)\n");
        if (!mod->common.methods || !mod->common.methods->open) {
            printf("HAL has no open method\n");
            return 1;
        }
        struct hw_device_t *dev = NULL;
        int rc = mod->common.methods->open(&mod->common, HWVULKAN_DEVICE_0, &dev);
        printf("HAL open -> %d (%s)\n", rc, strerror(rc));
        if (rc != 0 || !dev)
            return 1;
        hwvulkan_device_t *vdev = (hwvulkan_device_t *)dev;
        gipa = vdev->GetInstanceProcAddr;
        create_instance = vdev->CreateInstance;
    } else {
        printf("no HMI, trying ICD entrypoint\n");
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!gipa) {
            printf("no vk_icdGetInstanceProcAddr either: %s\n", dlerror());
            return 1;
        }
        create_instance = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    }
    if (!gipa || !create_instance) {
        printf("missing entrypoints (gipa=%p create=%p)\n", (void *)gipa, (void *)create_instance);
        return 1;
    }

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "a22probe",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    VkInstance inst = NULL;
    VkResult r = create_instance(&ici, NULL, &inst);
    printf("vkCreateInstance -> %d\n", r);
    if (r != VK_SUCCESS)
        return 1;

    PFN_vkEnumeratePhysicalDevices enum_pd =
        (PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices");
    PFN_vkGetPhysicalDeviceProperties get_props =
        (PFN_vkGetPhysicalDeviceProperties)gipa(inst, "vkGetPhysicalDeviceProperties");
    PFN_vkGetPhysicalDeviceProperties2 get_props2 =
        (PFN_vkGetPhysicalDeviceProperties2)gipa(inst, "vkGetPhysicalDeviceProperties2");
    PFN_vkGetPhysicalDeviceMemoryProperties get_mem =
        (PFN_vkGetPhysicalDeviceMemoryProperties)gipa(inst, "vkGetPhysicalDeviceMemoryProperties");
    PFN_vkEnumerateDeviceExtensionProperties enum_dext =
        (PFN_vkEnumerateDeviceExtensionProperties)gipa(inst, "vkEnumerateDeviceExtensionProperties");

    uint32_t count = 0;
    enum_pd(inst, &count, NULL);
    printf("physical devices: %u\n", count);
    if (!count)
        return 1;
    VkPhysicalDevice pd = NULL;
    enum_pd(inst, &count, &pd);

    PFN_vkEnumerateInstanceExtensionProperties enum_iext =
        (PFN_vkEnumerateInstanceExtensionProperties)gipa(inst, "vkEnumerateInstanceExtensionProperties");
    if (enum_iext) {
        uint32_t ie = 0;
        enum_iext(NULL, &ie, NULL);
        VkExtensionProperties *ix = calloc(ie ? ie : 1, sizeof(*ix));
        enum_iext(NULL, &ie, ix);
        int found = 0;
        for (uint32_t i = 0; i < ie; i++)
            if (!strcmp(ix[i].extensionName, "VK_KHR_android_surface")) found = 1;
        printf("instance extensions = %u (VK_KHR_android_surface: %s)\n", ie, found ? "yes" : "no");
        free(ix);
    }
    VkPhysicalDeviceProperties props;
    get_props(pd, &props);
    printf("deviceName   = %s\n", props.deviceName);
    printf("apiVersion   = %u.%u.%u\n", VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));
    printf("driverVersion= 0x%08x\n", props.driverVersion);
    printf("vendor/device= 0x%04x / 0x%04x\n", props.vendorID, props.deviceID);
    printf("maxComputeWG = %ux%ux%u inv=%u\n", props.limits.maxComputeWorkGroupSize[0],
           props.limits.maxComputeWorkGroupSize[1],
           props.limits.maxComputeWorkGroupSize[2],
           props.limits.maxComputeWorkGroupInvocations);
    printf("maxStorageBuf= %u MiB\n", props.limits.maxStorageBufferRange >> 20);
    printf("timestampPrd = %.2f ns\n", props.limits.timestampPeriod);

    if (get_props2) {
        VkPhysicalDeviceSubgroupProperties sub = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES,
        };
        VkPhysicalDeviceProperties2 p2 = {
            .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2,
            .pNext = &sub,
        };
        get_props2(pd, &p2);
        printf("subgroupSize = %u\n", sub.subgroupSize);
    }

    VkPhysicalDeviceMemoryProperties mem;
    get_mem(pd, &mem);
    for (uint32_t i = 0; i < mem.memoryHeapCount; i++)
        printf("heap[%u] = %u MiB flags=0x%x\n", i,
               (unsigned)(mem.memoryHeaps[i].size >> 20), mem.memoryHeaps[i].flags);

    if (enum_dext) {
        uint32_t n = 0;
        enum_dext(pd, NULL, &n, NULL);
        printf("device extensions = %u\n", n);
        VkExtensionProperties *ext = calloc(n ? n : 1, sizeof(*ext));
        if (ext && n) {
            enum_dext(pd, NULL, &n, ext);
            int has_android = 0, has_ssbo = 0;
            for (uint32_t i = 0; i < n; i++) {
                if (!strcmp(ext[i].extensionName, "VK_KHR_android_surface"))
                    has_android = 1;
                if (!strcmp(ext[i].extensionName, "VK_KHR_shader_float16_int8"))
                    has_ssbo = 1;
            }
            printf("  VK_KHR_android_surface: %s\n", has_android ? "yes" : "no");
            printf("  VK_KHR_shader_float16_int8: %s\n", has_ssbo ? "yes" : "no");
        }
        free(ext);
    }
    printf("RESULT: driver loaded and enumerated OK\n");
    return 0;
}


