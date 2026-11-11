/*
 * Turnip functional validation for Snapdragon G2 Gen 2 / Adreno A22.
 *
 * Loads the packaged Android Vulkan HAL (HMI), creates a real VkDevice and:
 *   1. dispatches a compute shader (exercises IR3 codegen + CP submission)
 *   2. clears a 512x512 RGBA8 image and copies it back (exercises UBWC
 *      tiling, blit/clear engine and tiled->linear resolve)
 * and verifies both results.  Prints timings for a rough throughput check.
 *
 * Build:
 *   clang --target=aarch64-linux-android35 --sysroot=$NDK/sysroot \
 *         -I <mesa>/include/android_stub -O2 -fPIE -pie -o a22test a22test.c -ldl
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>

#include "android_hwvulkan.h"
#include <vulkan/vulkan.h>

#include "a22_comp_spv.h"

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FAILED %s -> %d (line %d)\n", #x, _r, __LINE__); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;

static void *G(VkInstance inst, const char *name)
{
    void *p = (void *)gipa(inst, name);
    if (!p) {
        printf("missing entrypoint %s\n", name);
        exit(1);
    }
    return p;
}

VkInstance instance;
VkPhysicalDevice phys;
VkDevice device;
VkQueue queue;
uint32_t queue_family;

static void init_vulkan(const char *lib)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        printf("dlopen failed: %s\n", dlerror());
        exit(1);
    }
    hwvulkan_module_t *mod = (hwvulkan_module_t *)dlsym(h, "HMI");
    if (!mod) {
        printf("no HMI symbol - not an Android Vulkan HAL\n");
        exit(1);
    }
    struct hw_device_t *dev = NULL;
    int rc = mod->common.methods->open(&mod->common, HWVULKAN_DEVICE_0, &dev);
    if (rc != 0 || !dev) {
        printf("HAL open failed: %d\n", rc);
        exit(1);
    }
    gipa = ((hwvulkan_device_t *)dev)->GetInstanceProcAddr;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "a22test",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &instance));

    uint32_t n = 0;
    ((PFN_vkEnumeratePhysicalDevices)G(instance, "vkEnumeratePhysicalDevices"))(instance, &n, NULL);
    if (n == 0) { printf("no physical devices\n"); exit(1); }
    ((PFN_vkEnumeratePhysicalDevices)G(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &phys);

    VkPhysicalDeviceProperties props;
    ((PFN_vkGetPhysicalDeviceProperties)G(instance, "vkGetPhysicalDeviceProperties"))(phys, &props);
    printf("GPU: %s (api %u.%u.%u)\n", props.deviceName, VK_VERSION_MAJOR(props.apiVersion),
           VK_VERSION_MINOR(props.apiVersion), VK_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)G(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))
        (phys, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)G(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))
        (phys, &qn, qp);
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qp[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { queue_family = i; break; }
    }
    if (queue_family == UINT32_MAX) { printf("no compute queue\n"); exit(1); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = queue_family,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    CHECK(((PFN_vkCreateDevice)G(instance, "vkCreateDevice"))(phys, &dci, NULL, &device));
    ((PFN_vkGetDeviceQueue)G(instance, "vkGetDeviceQueue"))(device, queue_family, 0, &queue);
}

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)G(instance, "vkGetPhysicalDeviceMemoryProperties"))
        (phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    }
    printf("no suitable memory type\n");
    exit(1);
}

static void test_compute(void)
{
    const uint32_t count = 64;
    const VkDeviceSize size = count * sizeof(uint32_t);

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    VkBuffer buf;
    CHECK(((PFN_vkCreateBuffer)G(instance, "vkCreateBuffer"))(device, &bci, NULL, &buf));
    VkMemoryRequirements req;
    ((PFN_vkGetBufferMemoryRequirements)G(instance, "vkGetBufferMemoryRequirements"))(device, buf, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory mem;
    CHECK(((PFN_vkAllocateMemory)G(instance, "vkAllocateMemory"))(device, &mai, NULL, &mem));
    CHECK(((PFN_vkBindBufferMemory)G(instance, "vkBindBufferMemory"))(device, buf, mem, 0));

    void *map;
    CHECK(((PFN_vkMapMemory)G(instance, "vkMapMemory"))(device, mem, 0, VK_WHOLE_SIZE, 0, &map));
    memset(map, 0, size);

    VkDescriptorSetLayoutBinding bind = {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT,
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 1,
        .pBindings = &bind,
    };
    VkDescriptorSetLayout dsl;
    CHECK(((PFN_vkCreateDescriptorSetLayout)G(instance, "vkCreateDescriptorSetLayout"))
          (device, &dslci, NULL, &dsl));

    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = &dsl,
    };
    VkPipelineLayout pl;
    CHECK(((PFN_vkCreatePipelineLayout)G(instance, "vkCreatePipelineLayout"))(device, &plci, NULL, &pl));

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(a22_comp_spv),
        .pCode = a22_comp_spv,
    };
    VkShaderModule sm;
    CHECK(((PFN_vkCreateShaderModule)G(instance, "vkCreateShaderModule"))(device, &smci, NULL, &sm));

    VkComputePipelineCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_COMPUTE_BIT,
            .module = sm,
            .pName = "main",
        },
        .layout = pl,
    };
    VkPipeline pipe;
    CHECK(((PFN_vkCreateComputePipelines)G(instance, "vkCreateComputePipelines"))
          (device, NULL, 1, &cpci, NULL, &pipe));

    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    VkDescriptorPool pool;
    CHECK(((PFN_vkCreateDescriptorPool)G(instance, "vkCreateDescriptorPool"))(device, &dpci, NULL, &pool));
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet ds;
    CHECK(((PFN_vkAllocateDescriptorSets)G(instance, "vkAllocateDescriptorSets"))(device, &dsai, &ds));
    VkDescriptorBufferInfo dbi = { buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wds = {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .dstSet = ds,
        .dstBinding = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
        .pBufferInfo = &dbi,
    };
    ((PFN_vkUpdateDescriptorSets)G(instance, "vkUpdateDescriptorSets"))(device, 1, &wds, 0, NULL);

    VkCommandPoolCreateInfo cpci2 = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool cmd_pool;
    CHECK(((PFN_vkCreateCommandPool)G(instance, "vkCreateCommandPool"))(device, &cpci2, NULL, &cmd_pool));

    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cmd_pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(((PFN_vkAllocateCommandBuffers)G(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(((PFN_vkCreateFence)G(instance, "vkCreateFence"))(device, &fci, NULL, &fence));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(((PFN_vkBeginCommandBuffer)G(instance, "vkBeginCommandBuffer"))(cmd, &bi));
    ((PFN_vkCmdBindPipeline)G(instance, "vkCmdBindPipeline"))(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    ((PFN_vkCmdBindDescriptorSets)G(instance, "vkCmdBindDescriptorSets"))
        (cmd, VK_PIPELINE_BIND_POINT_COMPUTE, pl, 0, 1, &ds, 0, NULL);
    ((PFN_vkCmdDispatch)G(instance, "vkCmdDispatch"))(cmd, 1, 1, 1);
    CHECK(((PFN_vkEndCommandBuffer)G(instance, "vkEndCommandBuffer"))(cmd));

    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
    CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));

    int bad = 0;
    uint32_t *out = (uint32_t *)map;
    for (uint32_t i = 0; i < count; i++) {
        if (out[i] != i * 3u + 7u) {
            if (bad < 4) printf("  compute mismatch [%u] = %u expected %u\n", i, out[i], i * 3u + 7u);
            bad++;
        }
    }
    printf("compute dispatch: %s (%u/%u correct)\n", bad ? "FAIL" : "PASS", count - bad, count);

    /* rough throughput: 200 back-to-back dispatches */
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int i = 0; i < 200; i++) {
        CHECK(((PFN_vkResetFences)G(instance, "vkResetFences"))(device, 1, &fence));
        CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
        CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double ms = (t1.tv_sec - t0.tv_sec) * 1e3 + (t1.tv_nsec - t0.tv_nsec) / 1e6;
    printf("compute round-trip: %.3f ms/dispatch (200 submits)\n", ms / 200.0);
}

static void test_clear_copy(void)
{
    const uint32_t W = 512, H = 512;
    const VkDeviceSize px = 4, img_size = (VkDeviceSize)W * H * px;

    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { W, H, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    VkImage img;
    CHECK(((PFN_vkCreateImage)G(instance, "vkCreateImage"))(device, &ici, NULL, &img));
    VkMemoryRequirements req;
    ((PFN_vkGetImageMemoryRequirements)G(instance, "vkGetImageMemoryRequirements"))(device, img, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkDeviceMemory img_mem;
    CHECK(((PFN_vkAllocateMemory)G(instance, "vkAllocateMemory"))(device, &mai, NULL, &img_mem));
    CHECK(((PFN_vkBindImageMemory)G(instance, "vkBindImageMemory"))(device, img, img_mem, 0));

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = img_size,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer buf;
    CHECK(((PFN_vkCreateBuffer)G(instance, "vkCreateBuffer"))(device, &bci, NULL, &buf));
    VkMemoryRequirements breq;
    ((PFN_vkGetBufferMemoryRequirements)G(instance, "vkGetBufferMemoryRequirements"))(device, buf, &breq);
    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = breq.size,
        .memoryTypeIndex = find_memory(breq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory buf_mem;
    CHECK(((PFN_vkAllocateMemory)G(instance, "vkAllocateMemory"))(device, &bmai, NULL, &buf_mem));
    CHECK(((PFN_vkBindBufferMemory)G(instance, "vkBindBufferMemory"))(device, buf, buf_mem, 0));
    void *map;
    CHECK(((PFN_vkMapMemory)G(instance, "vkMapMemory"))(device, buf_mem, 0, VK_WHOLE_SIZE, 0, &map));
    memset(map, 0xAB, img_size);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool pool;
    CHECK(((PFN_vkCreateCommandPool)G(instance, "vkCreateCommandPool"))(device, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(((PFN_vkAllocateCommandBuffers)G(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(((PFN_vkBeginCommandBuffer)G(instance, "vkBeginCommandBuffer"))(cmd, &bi));

    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = img,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
    };
    ((PFN_vkCmdPipelineBarrier)G(instance, "vkCmdPipelineBarrier"))
        (cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
         0, NULL, 0, NULL, 1, &b);
    VkClearColorValue color = { .float32 = { 1.0f, 0.5f, 0.25f, 1.0f } };
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    ((PFN_vkCmdClearColorImage)G(instance, "vkCmdClearColorImage"))
        (cmd, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &color, 1, &range);

    VkImageMemoryBarrier b2 = b;
    b2.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b2.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b2.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b2.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    ((PFN_vkCmdPipelineBarrier)G(instance, "vkCmdPipelineBarrier"))
        (cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
         0, NULL, 0, NULL, 1, &b2);

    VkBufferImageCopy region = {
        .bufferOffset = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageExtent = { W, H, 1 },
    };
    ((PFN_vkCmdCopyImageToBuffer)G(instance, "vkCmdCopyImageToBuffer"))
        (cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    CHECK(((PFN_vkEndCommandBuffer)G(instance, "vkEndCommandBuffer"))(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(((PFN_vkCreateFence)G(instance, "vkCreateFence"))(device, &fci, NULL, &fence));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
    CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));

    uint8_t *p = (uint8_t *)map;
    int bad = 0;
    for (uint32_t i = 0; i < W * H; i++) {
        uint8_t *q = p + i * 4;
        if (q[0] != 255 || q[1] != 128 || q[2] != 64 || q[3] != 255) {
            if (bad < 4) printf("  pixel %u = %u,%u,%u,%u\n", i, q[0], q[1], q[2], q[3]);
            bad++;
        }
    }
    printf("image clear+resolve: %s (%u/%u pixels correct)\n", bad ? "FAIL" : "PASS",
           W * H - bad, W * H);
}

int main(int argc, char **argv)
{
    const char *lib = argc > 1 ? argv[1] : "libvulkan_freedreno.so";
    init_vulkan(lib);
    test_compute();
    test_clear_copy();
    printf("RESULT: %s\n", "functional validation finished");
    ((PFN_vkDeviceWaitIdle)G(instance, "vkDeviceWaitIdle"))(device);
    return 0;
}

