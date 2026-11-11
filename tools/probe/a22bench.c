/*
 * Turnip A22 micro-benchmark: render throughput + memory bandwidth.
 *
 * Runs against a packaged Android Vulkan HAL (HMI) so the measured numbers come
 * from the same binary users install. Intended for release-over-release
 * regression tracking and for comparing GMEM vs SYSMEM render modes via
 * TU_DEBUG=sysmem / TU_DEBUG=gmem (or the default autotuner).
 *
 * Build:
 *   clang --target=aarch64-linux-android35 --sysroot=$NDK/sysroot \
 *         -I tools/probe -O2 -fPIE -pie -o a22bench a22bench.c -ldl
 *
 * Env: BENCH_FRAMES (default 120), BENCH_DRAWS (default 32), BENCH_WIDTH/HEIGHT.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <dlfcn.h>

#include "android_hwvulkan.h"
#include "bench_vert_spv.h"
#include "bench_frag_spv.h"

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    fprintf(stderr, "FAILED %s -> %d (line %d)\n", #x, _r, __LINE__); exit(1); } } while (0)

static PFN_vkGetInstanceProcAddr gipa;
static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t qfamily;

static void *G(VkInstance inst, const char *name)
{
    void *p = (void *)gipa(inst, name);
    if (!p) { fprintf(stderr, "missing entrypoint %s\n", name); exit(1); }
    return p;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static int env_int(const char *name, int def)
{
    const char *v = getenv(name);
    return v && *v ? atoi(v) : def;
}

static void init_vulkan(const char *lib)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "dlopen failed: %s\n", dlerror()); exit(1); }
    hwvulkan_module_t *mod = (hwvulkan_module_t *)dlsym(h, "HMI");
    if (!mod) { fprintf(stderr, "no HMI symbol\n"); exit(1); }
    struct hw_device_t *dev = NULL;
    int rc = mod->common.methods->open(&mod->common, HWVULKAN_DEVICE_0, &dev);
    if (rc != 0 || !dev) { fprintf(stderr, "HAL open failed: %d\n", rc); exit(1); }
    hwvulkan_device_t *vdev = (hwvulkan_device_t *)dev;
    gipa = vdev->GetInstanceProcAddr;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "a22bench",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    CHECK(vdev->CreateInstance(&ici, NULL, &instance));

    uint32_t n = 0;
    ((PFN_vkEnumeratePhysicalDevices)G(instance, "vkEnumeratePhysicalDevices"))(instance, &n, NULL);
    if (!n) { fprintf(stderr, "no devices\n"); exit(1); }
    ((PFN_vkEnumeratePhysicalDevices)G(instance, "vkEnumeratePhysicalDevices"))(instance, &n, &phys);

    VkPhysicalDeviceProperties props;
    ((PFN_vkGetPhysicalDeviceProperties)G(instance, "vkGetPhysicalDeviceProperties"))(phys, &props);
    printf("GPU: %s  api %u.%u.%u\n", props.deviceName,
           VK_VERSION_MAJOR(props.apiVersion), VK_VERSION_MINOR(props.apiVersion),
           VK_VERSION_PATCH(props.apiVersion));

    uint32_t qn = 0;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)G(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))
        (phys, &qn, NULL);
    VkQueueFamilyProperties qp[16];
    if (qn > 16) qn = 16;
    ((PFN_vkGetPhysicalDeviceQueueFamilyProperties)G(instance, "vkGetPhysicalDeviceQueueFamilyProperties"))
        (phys, &qn, qp);
    qfamily = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { qfamily = i; break; }
    }
    if (qfamily == UINT32_MAX) { fprintf(stderr, "no graphics queue\n"); exit(1); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
        .queueFamilyIndex = qfamily,
        .queueCount = 1,
        .pQueuePriorities = &prio,
    };
    VkDeviceCreateInfo dci = {
        .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
        .queueCreateInfoCount = 1,
        .pQueueCreateInfos = &qci,
    };
    CHECK(((PFN_vkCreateDevice)G(instance, "vkCreateDevice"))(phys, &dci, NULL, &device));
    ((PFN_vkGetDeviceQueue)G(instance, "vkGetDeviceQueue"))(device, qfamily, 0, &queue);
}

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    ((PFN_vkGetPhysicalDeviceMemoryProperties)G(instance, "vkGetPhysicalDeviceMemoryProperties"))
        (phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    fprintf(stderr, "no suitable memory type\n");
    exit(1);
}

static int gpu_clock_mhz(void)
{
    FILE *f = fopen("/sys/class/kgsl/kgsl-3d0/clock_mhz", "r");
    int mhz = -1;
    if (f) {
        if (fscanf(f, "%d", &mhz) != 1)
            mhz = -1;
        fclose(f);
    }
    return mhz;
}

static void bench_render(const char *label, int width, int height, int draws, int frames)
{
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { (uint32_t)width, (uint32_t)height, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
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

    VkImageViewCreateInfo ivci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    VkImageView view;
    CHECK(((PFN_vkCreateImageView)G(instance, "vkCreateImageView"))(device, &ivci, NULL, &view));

    VkAttachmentDescription att = {
        .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1,
        .pColorAttachments = &ref,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1,
        .pAttachments = &att,
        .subpassCount = 1,
        .pSubpasses = &sub,
    };
    VkRenderPass rp;
    CHECK(((PFN_vkCreateRenderPass)G(instance, "vkCreateRenderPass"))(device, &rpci, NULL, &rp));

    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = 1,
        .pAttachments = &view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    VkFramebuffer fb;
    CHECK(((PFN_vkCreateFramebuffer)G(instance, "vkCreateFramebuffer"))(device, &fbci, NULL, &fb));

    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(bench_vert_spv),
        .pCode = bench_vert_spv,
    };
    VkShaderModule vs;
    CHECK(((PFN_vkCreateShaderModule)G(instance, "vkCreateShaderModule"))(device, &smci, NULL, &vs));
    smci.codeSize = sizeof(bench_frag_spv);
    smci.pCode = bench_frag_spv;
    VkShaderModule fs;
    CHECK(((PFN_vkCreateShaderModule)G(instance, "vkCreateShaderModule"))(device, &smci, NULL, &fs));

    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPipelineLayout layout;
    CHECK(((PFN_vkCreatePipelineLayout)G(instance, "vkCreatePipelineLayout"))(device, &plci, NULL, &layout));

    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkPipelineVertexInputStateCreateInfo vi = { .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkViewport vp = { 0, 0, (float)width, (float)height, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)width, (uint32_t)height } };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .pViewports = &vp, .scissorCount = 1, .pScissors = &sc,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms_state = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    VkPipelineColorBlendAttachmentState cba = { .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &cba,
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vps, .pRasterizationState = &rs,
        .pMultisampleState = &ms_state, .pColorBlendState = &cb,
        .layout = layout, .renderPass = rp,
    };
    VkPipeline pipe;
    double t_pipe = now_ms();
    CHECK(((PFN_vkCreateGraphicsPipelines)G(instance, "vkCreateGraphicsPipelines"))
          (device, NULL, 1, &gpci, NULL, &pipe));
    printf("BENCH pipeline   create %.3f ms\n", now_ms() - t_pipe);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool pool;
    CHECK(((PFN_vkCreateCommandPool)G(instance, "vkCreateCommandPool"))(device, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(((PFN_vkAllocateCommandBuffers)G(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    CHECK(((PFN_vkBeginCommandBuffer)G(instance, "vkBeginCommandBuffer"))(cmd, &bi));
    VkClearValue clear = { .color = { .float32 = { 0.1f, 0.1f, 0.1f, 1.0f } } };
    VkRenderPassBeginInfo rpb = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb,
        .renderArea = { { 0, 0 }, { (uint32_t)width, (uint32_t)height } },
        .clearValueCount = 1, .pClearValues = &clear,
    };
    ((PFN_vkCmdBeginRenderPass)G(instance, "vkCmdBeginRenderPass"))(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
    ((PFN_vkCmdBindPipeline)G(instance, "vkCmdBindPipeline"))(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    for (int i = 0; i < draws; i++)
        ((PFN_vkCmdDraw)G(instance, "vkCmdDraw"))(cmd, 3, 1, 0, 0);
    ((PFN_vkCmdEndRenderPass)G(instance, "vkCmdEndRenderPass"))(cmd);
    CHECK(((PFN_vkEndCommandBuffer)G(instance, "vkEndCommandBuffer"))(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(((PFN_vkCreateFence)G(instance, "vkCreateFence"))(device, &fci, NULL, &fence));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };

    /* Warm up until the GPU DVFS governor settles (>=2 s of continuous load). */
    double warm_start = now_ms();
    while (now_ms() - warm_start < 2000.0) {
        CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
        CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(((PFN_vkResetFences)G(instance, "vkResetFences"))(device, 1, &fence));
    }

    double t0 = now_ms();
    for (int i = 0; i < frames; i++) {
        CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
        CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(((PFN_vkResetFences)G(instance, "vkResetFences"))(device, 1, &fence));
    }
    double ms = (now_ms() - t0) / frames;
    printf("BENCH %-10s %dx%d draws=%-3d  %7.3f ms/frame  %6.1f fps  clock=%dMHz\n",
           label, width, height, draws, ms, 1000.0 / ms, gpu_clock_mhz());

    ((PFN_vkDestroyFence)G(instance, "vkDestroyFence"))(device, fence, NULL);
    ((PFN_vkFreeCommandBuffers)G(instance, "vkFreeCommandBuffers"))(device, pool, 1, &cmd);
    ((PFN_vkDestroyCommandPool)G(instance, "vkDestroyCommandPool"))(device, pool, NULL);
    ((PFN_vkDestroyPipeline)G(instance, "vkDestroyPipeline"))(device, pipe, NULL);
    ((PFN_vkDestroyPipelineLayout)G(instance, "vkDestroyPipelineLayout"))(device, layout, NULL);
    ((PFN_vkDestroyShaderModule)G(instance, "vkDestroyShaderModule"))(device, fs, NULL);
    ((PFN_vkDestroyShaderModule)G(instance, "vkDestroyShaderModule"))(device, vs, NULL);
    ((PFN_vkDestroyFramebuffer)G(instance, "vkDestroyFramebuffer"))(device, fb, NULL);
    ((PFN_vkDestroyRenderPass)G(instance, "vkDestroyRenderPass"))(device, rp, NULL);
    ((PFN_vkDestroyImageView)G(instance, "vkDestroyImageView"))(device, view, NULL);
    ((PFN_vkFreeMemory)G(instance, "vkFreeMemory"))(device, img_mem, NULL);
    ((PFN_vkDestroyImage)G(instance, "vkDestroyImage"))(device, img, NULL);
}

static void bench_copy(const char *label, uint32_t mib, int iters)
{
    VkDeviceSize size = (VkDeviceSize)mib << 20;
    VkBuffer bufs[2];
    VkDeviceMemory mems[2];
    for (int i = 0; i < 2; i++) {
        VkBufferCreateInfo bci = {
            .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
            .size = size,
            .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
        };
        CHECK(((PFN_vkCreateBuffer)G(instance, "vkCreateBuffer"))(device, &bci, NULL, &bufs[i]));
        VkMemoryRequirements req;
        ((PFN_vkGetBufferMemoryRequirements)G(instance, "vkGetBufferMemoryRequirements"))(device, bufs[i], &req);
        VkMemoryAllocateInfo mai = {
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = req.size,
            .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
        };
        CHECK(((PFN_vkAllocateMemory)G(instance, "vkAllocateMemory"))(device, &mai, NULL, &mems[i]));
        CHECK(((PFN_vkBindBufferMemory)G(instance, "vkBindBufferMemory"))(device, bufs[i], mems[i], 0));
    }

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = qfamily,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
    };
    VkCommandPool pool;
    CHECK(((PFN_vkCreateCommandPool)G(instance, "vkCreateCommandPool"))(device, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(((PFN_vkAllocateCommandBuffers)G(instance, "vkAllocateCommandBuffers"))(device, &cbai, &cmd));

    VkCommandBufferBeginInfo bi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT,
    };
    CHECK(((PFN_vkBeginCommandBuffer)G(instance, "vkBeginCommandBuffer"))(cmd, &bi));
    VkBufferCopy region = { 0, 0, size };
    ((PFN_vkCmdCopyBuffer)G(instance, "vkCmdCopyBuffer"))(cmd, bufs[0], bufs[1], 1, &region);
    CHECK(((PFN_vkEndCommandBuffer)G(instance, "vkEndCommandBuffer"))(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(((PFN_vkCreateFence)G(instance, "vkCreateFence"))(device, &fci, NULL, &fence));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1, .pCommandBuffers = &cmd,
    };

    for (int i = 0; i < 5; i++) {
        CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
        CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(((PFN_vkResetFences)G(instance, "vkResetFences"))(device, 1, &fence));
    }
    double t0 = now_ms();
    for (int i = 0; i < iters; i++) {
        CHECK(((PFN_vkQueueSubmit)G(instance, "vkQueueSubmit"))(queue, 1, &si, fence));
        CHECK(((PFN_vkWaitForFences)G(instance, "vkWaitForFences"))(device, 1, &fence, VK_TRUE, UINT64_MAX));
        CHECK(((PFN_vkResetFences)G(instance, "vkResetFences"))(device, 1, &fence));
    }
    double ms = (now_ms() - t0) / iters;
    double gbps = (2.0 * size * iters) / ((ms * iters) / 1e3) / 1e9;
    printf("BENCH %-10s %u MiB copy  %7.3f ms  %6.1f GB/s (r+w)\n", label, mib, ms, gbps);

    ((PFN_vkDestroyFence)G(instance, "vkDestroyFence"))(device, fence, NULL);
    ((PFN_vkFreeCommandBuffers)G(instance, "vkFreeCommandBuffers"))(device, pool, 1, &cmd);
    ((PFN_vkDestroyCommandPool)G(instance, "vkDestroyCommandPool"))(device, pool, NULL);
    for (int i = 0; i < 2; i++) {
        ((PFN_vkFreeMemory)G(instance, "vkFreeMemory"))(device, mems[i], NULL);
        ((PFN_vkDestroyBuffer)G(instance, "vkDestroyBuffer"))(device, bufs[i], NULL);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *lib = argc > 1 ? argv[1] : "libvulkan_freedreno.so";
    int frames = env_int("BENCH_FRAMES", 120);
    int draws = env_int("BENCH_DRAWS", 32);
    int width = env_int("BENCH_WIDTH", 1920);
    int height = env_int("BENCH_HEIGHT", 1080);
    const char *mode = getenv("TU_DEBUG") ? getenv("TU_DEBUG") : "autotune";

    init_vulkan(lib);
    printf("mode: %s  frames: %d  draws: %d\n", mode, frames, draws);

    char label[64];
    snprintf(label, sizeof(label), "%s", mode);
    bench_render(label, width, height, draws, frames);
    bench_render(label, 1280, 720, draws, frames);
    bench_copy(label, 64, 20);

    ((PFN_vkDeviceWaitIdle)G(instance, "vkDeviceWaitIdle"))(device);
    printf("BENCH done\n");
    return 0;
}


