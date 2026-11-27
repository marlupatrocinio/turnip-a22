/*
 * a22cpu - driver CPU-overhead benchmark (no GPU-bound work).
 *
 * Measures the CPU time the driver spends on the calls an emulator issues per
 * draw / per frame, pinned to one core, as median and minimum of repeated runs:
 *
 *   record   ns per draw recorded: bind pipeline (switch every 8 draws),
 *            bind descriptor set with a dynamic offset, push constants,
 *            viewport/scissor (every 4 draws), bind vertex buffer, draw
 *   rp       ns per render pass begin+end (one draw inside), the per-pass
 *            fixed cost (Turnip finalises both GMEM and SYSMEM paths here)
 *   update   ns per vkUpdateDescriptorSetWithTemplate (UBO + texture)
 *   submit   us per vkQueueSubmit of a small command buffer (CPU side only)
 *
 *   ./a22cpu <libvulkan.so>
 *
 * Env: CPU_CORE (default 7 = Cortex-X4), REPS (default 9), DRAWS (default
 * 20000), PASSES (default 2000), UPDATES (default 20000), SUBMITS (default 1000).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <sched.h>
#include <dlfcn.h>

#define VK_NO_PROTOTYPES
#include "android_hwvulkan.h"
#include <vulkan/vulkan.h>

#include "cpu_vert_spv.h"
#include "cpu_frag_spv.h"

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FAILED %s -> %d (line %d)\n", #x, _r, __LINE__); exit(1); } } while (0)

#define VK_FUNCS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkCreateDevice) X(vkGetDeviceQueue) X(vkCreateImage) \
    X(vkGetImageMemoryRequirements) X(vkAllocateMemory) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateRenderPass) X(vkCreateFramebuffer) \
    X(vkCreateShaderModule) X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) X(vkAllocateDescriptorSets) \
    X(vkUpdateDescriptorSets) X(vkCreateDescriptorUpdateTemplate) \
    X(vkUpdateDescriptorSetWithTemplate) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdBindVertexBuffers) X(vkCmdDraw) \
    X(vkCmdPipelineBarrier) X(vkCreateSampler) X(vkCreateFence) X(vkResetFences) \
    X(vkQueueSubmit) X(vkWaitForFences) X(vkDeviceWaitIdle)

#define DECL(n) static PFN_##n n;
VK_FUNCS(DECL)

static VkInstance instance;
static VkPhysicalDevice phys;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;

static void init_vulkan(const char *lib)
{
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen failed: %s\n", dlerror()); exit(1); }
    hwvulkan_module_t *mod = (hwvulkan_module_t *)dlsym(h, "HMI");
    if (!mod) { printf("no HMI symbol\n"); exit(1); }
    struct hw_device_t *dev = NULL;
    if (mod->common.methods->open(&mod->common, HWVULKAN_DEVICE_0, &dev) || !dev) {
        printf("HAL open failed\n");
        exit(1);
    }
    hwvulkan_device_t *vdev = (hwvulkan_device_t *)dev;
    PFN_vkGetInstanceProcAddr gipa = vdev->GetInstanceProcAddr;
    VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                              .pApplicationName = "a22cpu", .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                 .pApplicationInfo = &app };
    CHECK(vdev->CreateInstance(&ici, NULL, &instance));
#define LOAD(n) n = (PFN_##n)gipa(instance, #n); \
    if (!n) { printf("missing entrypoint %s\n", #n); exit(1); }
    VK_FUNCS(LOAD)
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(instance, &n, &phys);
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s\n", props.deviceName);
    VkQueueFamilyProperties qp[16];
    uint32_t qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
    for (queue_family = 0; queue_family < qn; queue_family++)
        if (qp[queue_family].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            break;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                    .queueFamilyIndex = queue_family, .queueCount = 1,
                                    .pQueuePriorities = &prio };
    VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                               .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci };
    CHECK(vkCreateDevice(phys, &dci, NULL, &device));
    vkGetDeviceQueue(device, queue_family, 0, &queue);
}

static uint32_t find_memory(uint32_t bits, VkMemoryPropertyFlags flags)
{
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & flags) == flags)
            return i;
    printf("no memory type\n");
    exit(1);
}

static VkBuffer make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, void **map)
{
    VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                               .size = size, .usage = usage };
    VkBuffer b;
    CHECK(vkCreateBuffer(device, &bci, NULL, &b));
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, b, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) };
    VkDeviceMemory m;
    CHECK(vkAllocateMemory(device, &mai, NULL, &m));
    CHECK(vkBindBufferMemory(device, b, m, 0));
    CHECK(vkMapMemory(device, m, 0, VK_WHOLE_SIZE, 0, map));
    return b;
}

static void make_image(uint32_t w, uint32_t h, VkImageUsageFlags usage, VkImage *img,
                       VkImageView *view)
{
    VkImageCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .extent = { w, h, 1 }, .mipLevels = 1, .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = usage };
    CHECK(vkCreateImage(device, &ici, NULL, img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, *img, &req);
    VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) };
    VkDeviceMemory m;
    CHECK(vkAllocateMemory(device, &mai, NULL, &m));
    CHECK(vkBindImageMemory(device, *img, m, 0));
    VkImageViewCreateInfo vci = { .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = *img, .viewType = VK_IMAGE_VIEW_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    CHECK(vkCreateImageView(device, &vci, NULL, view));
}

static double now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec * 1e9 + t.tv_nsec;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : x > y;
}

static int env_int(const char *name, int def)
{
    const char *s = getenv(name);
    return s && *s ? atoi(s) : def;
}

static void report(const char *name, const char *unit, double *v, int n)
{
    qsort(v, n, sizeof(double), cmp_d);
    printf("CPU %-7s median %8.1f %s   min %8.1f %s   (n=%d)\n", name, v[n / 2], unit,
           v[0], unit, n);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *lib = argc > 1 ? argv[1] : "libvulkan_freedreno.so";
    int core = env_int("CPU_CORE", 7), reps = env_int("REPS", 9);
    int draws = env_int("DRAWS", 20000), passes = env_int("PASSES", 2000);
    int updates = env_int("UPDATES", 20000), submits = env_int("SUBMITS", 1000);
    if (reps > 64) reps = 64;

    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(core, &set);
    if (sched_setaffinity(0, sizeof(set), &set))
        printf("warning: could not pin to cpu %d\n", core);

    init_vulkan(lib);
    printf("pinned to cpu %d, reps %d\n", core, reps);

    /* resources */
    VkImage rt_img, tex_img;
    VkImageView rt_view, tex_view;
    make_image(256, 256, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, &rt_img, &rt_view);
    make_image(64, 64, VK_IMAGE_USAGE_SAMPLED_BIT, &tex_img, &tex_view);
    void *ubo_map, *vb_map;
    VkBuffer ubo = make_buffer(256 * 1024, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT, &ubo_map);
    VkBuffer vb = make_buffer(4096, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, &vb_map);
    float tri[6] = { -0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f };
    memcpy(vb_map, tri, sizeof(tri));
    for (int i = 0; i < 256 * 1024 / 4; i++)
        ((float *)ubo_map)[i] = 1.0f;

    VkSamplerCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_LINEAR, .minFilter = VK_FILTER_LINEAR };
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sci, NULL, &sampler));

    VkAttachmentDescription att = { .format = VK_FORMAT_R8G8B8A8_UNORM,
        .samples = VK_SAMPLE_COUNT_1_BIT, .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sub = { .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
                                 .colorAttachmentCount = 1, .pColorAttachments = &ref };
    VkRenderPassCreateInfo rpci = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 1, .pAttachments = &att, .subpassCount = 1, .pSubpasses = &sub };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &rp));
    VkFramebufferCreateInfo fbci = { .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp, .attachmentCount = 1, .pAttachments = &rt_view,
        .width = 256, .height = 256, .layers = 1 };
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(device, &fbci, NULL, &fb));

    VkDescriptorSetLayoutBinding binds[2] = {
        { 0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1,
          VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
        { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
    };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = 2, .pBindings = binds };
    VkDescriptorSetLayout dsl;
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_VERTEX_BIT, 0, 16 };
    VkPipelineLayoutCreateInfo plci = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1, .pSetLayouts = &dsl, .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pl));

    enum { NSETS = 1024 };
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, NSETS },
                                   { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, NSETS } };
    VkDescriptorPoolCreateInfo dpci = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = NSETS, .poolSizeCount = 2, .pPoolSizes = ps };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));
    VkDescriptorSet *sets = malloc(sizeof(VkDescriptorSet) * NSETS);
    VkDescriptorSetLayout *layouts = malloc(sizeof(VkDescriptorSetLayout) * NSETS);
    for (int i = 0; i < NSETS; i++)
        layouts[i] = dsl;
    VkDescriptorSetAllocateInfo dsai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool, .descriptorSetCount = NSETS, .pSetLayouts = layouts };
    CHECK(vkAllocateDescriptorSets(device, &dsai, sets));

    struct { VkDescriptorBufferInfo ubo; VkDescriptorImageInfo tex; } tdata = {
        { ubo, 0, 256 },
        { sampler, tex_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL },
    };
    VkDescriptorUpdateTemplateEntry te[2] = {
        { 0, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 0, 0 },
        { 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, sizeof(VkDescriptorBufferInfo), 0 },
    };
    VkDescriptorUpdateTemplateCreateInfo tci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_UPDATE_TEMPLATE_CREATE_INFO,
        .descriptorUpdateEntryCount = 2, .pDescriptorUpdateEntries = te,
        .templateType = VK_DESCRIPTOR_UPDATE_TEMPLATE_TYPE_DESCRIPTOR_SET,
        .descriptorSetLayout = dsl };
    VkDescriptorUpdateTemplate templ;
    CHECK(vkCreateDescriptorUpdateTemplate(device, &tci, NULL, &templ));
    for (int i = 0; i < NSETS; i++)
        vkUpdateDescriptorSetWithTemplate(device, sets[i], templ, &tdata);

    VkShaderModuleCreateInfo vsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(cpu_vert_spv), .pCode = cpu_vert_spv };
    VkShaderModuleCreateInfo fsci = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = sizeof(cpu_frag_spv), .pCode = cpu_frag_spv };
    VkShaderModule vs, fs;
    CHECK(vkCreateShaderModule(device, &vsci, NULL, &vs));
    CHECK(vkCreateShaderModule(device, &fsci, NULL, &fs));
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkVertexInputBindingDescription vib = { 0, 8, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription via = { 0, 0, VK_FORMAT_R32G32_SFLOAT, 0 };
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .vertexBindingDescriptionCount = 1, .pVertexBindingDescriptions = &vib,
        .vertexAttributeDescriptionCount = 1, .pVertexAttributeDescriptions = &via };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST };
    VkPipelineViewportStateCreateInfo vps = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1, .scissorCount = 1 };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL, .cullMode = VK_CULL_MODE_NONE, .lineWidth = 1.0f };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT };
    VkPipelineColorBlendAttachmentState cba[2] = {
        { .colorWriteMask = 0xf },
        { .blendEnable = VK_TRUE, .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
          .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
          .colorBlendOp = VK_BLEND_OP_ADD, .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
          .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO, .alphaBlendOp = VK_BLEND_OP_ADD,
          .colorWriteMask = 0xf },
    };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 2, .pDynamicStates = dyn };
    VkPipeline pipes[2];
    for (int i = 0; i < 2; i++) {
        VkPipelineColorBlendStateCreateInfo cb = {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .attachmentCount = 1, .pAttachments = &cba[i] };
        VkGraphicsPipelineCreateInfo gp = {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2, .pStages = stages, .pVertexInputState = &vi,
            .pInputAssemblyState = &ia, .pViewportState = &vps, .pRasterizationState = &rs,
            .pMultisampleState = &ms, .pColorBlendState = &cb, .pDynamicState = &dy,
            .layout = pl, .renderPass = rp };
        CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gp, NULL, &pipes[i]));
    }

    VkCommandPoolCreateInfo cpci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family };
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(device, &cpci, NULL, &cpool));
    VkCommandBuffer cmd, small[64];
    VkCommandBufferAllocateInfo cbai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
    cbai.commandBufferCount = 64;
    CHECK(vkAllocateCommandBuffers(device, &cbai, small));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, NULL, &fence));

    /* one-time layout setup: RT -> COLOR_ATTACHMENT, texture -> SHADER_READ_ONLY */
    CHECK(vkBeginCommandBuffer(cmd, &bi));
    VkImageMemoryBarrier ib[2] = {
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = rt_img,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
        { .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
          .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
          .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
          .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
          .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = tex_img,
          .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_ALL_GRAPHICS_BIT, 0, 0, NULL, 0, NULL, 2, ib);
    CHECK(vkEndCommandBuffer(cmd));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                        .pCommandBuffers = &cmd };
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

    VkViewport vp = { 0, 0, 256, 256, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { 256, 256 } };
    VkRenderPassBeginInfo rpbi = { .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp, .framebuffer = fb, .renderArea = sc };
    VkDeviceSize vb_off = 0;
    double v[64];

    /* warm-up so caches, allocations and DVFS settle before measuring */
    for (int w = 0; w < 3; w++) {
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        for (int i = 0; i < draws; i++) {
            if ((i & 7) == 0)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[(i >> 3) & 1]);
            uint32_t dyn_off = (i & 255) * 256;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
                                    &sets[i % NSETS], 1, &dyn_off);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
    }

    /* record */
    for (int r = 0; r < reps; r++) {
        vkResetCommandBuffer(cmd, 0);
        double t0 = now_ns();
        vkBeginCommandBuffer(cmd, &bi);
        vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        for (int i = 0; i < draws; i++) {
            if ((i & 7) == 0)
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[(i >> 3) & 1]);
            uint32_t dyn_off = (i & 255) * 256;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
                                    &sets[i % NSETS], 1, &dyn_off);
            float pc[4] = { (i & 15) * 0.01f, 0, 0, 0 };
            vkCmdPushConstants(cmd, pl, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
            if ((i & 3) == 0) {
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
            }
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_off);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        vkEndCommandBuffer(cmd);
        v[r] = (now_ns() - t0) / draws;
    }
    report("record", "ns/draw", v, reps);

    /* render pass begin/end with one draw */
    for (int r = 0; r < reps; r++) {
        vkResetCommandBuffer(cmd, 0);
        double t0 = now_ns();
        vkBeginCommandBuffer(cmd, &bi);
        for (int p = 0; p < passes; p++) {
            vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[p & 1]);
            uint32_t dyn_off = 0;
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1,
                                    &sets[p % NSETS], 1, &dyn_off);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &vb_off);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRenderPass(cmd);
        }
        vkEndCommandBuffer(cmd);
        v[r] = (now_ns() - t0) / passes;
    }
    report("rp", "ns/pass", v, reps);

    /* descriptor update through a template */
    for (int r = 0; r < reps; r++) {
        double t0 = now_ns();
        for (int i = 0; i < updates; i++) {
            tdata.ubo.offset = (VkDeviceSize)(i & 255) * 256;
            vkUpdateDescriptorSetWithTemplate(device, sets[i % NSETS], templ, &tdata);
        }
        v[r] = (now_ns() - t0) / updates;
    }
    report("update", "ns/upd", v, reps);

    /* submit: CPU cost of vkQueueSubmit for small command buffers */
    for (int i = 0; i < 64; i++) {
        vkBeginCommandBuffer(small[i], &bi);
        vkCmdBeginRenderPass(small[i], &rpbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(small[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pipes[0]);
        uint32_t dyn_off = 0;
        vkCmdBindDescriptorSets(small[i], VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &sets[i],
                                1, &dyn_off);
        vkCmdSetViewport(small[i], 0, 1, &vp);
        vkCmdSetScissor(small[i], 0, 1, &sc);
        vkCmdBindVertexBuffers(small[i], 0, 1, &vb, &vb_off);
        vkCmdDraw(small[i], 3, 1, 0, 0);
        vkCmdEndRenderPass(small[i]);
        vkEndCommandBuffer(small[i]);
    }
    for (int r = 0; r < reps; r++) {
        CHECK(vkDeviceWaitIdle(device));
        double t = 0;
        for (int i = 0; i < submits; i++) {
            VkSubmitInfo s1 = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1,
                                .pCommandBuffers = &small[i & 63] };
            if ((i & 63) == 0)
                CHECK(vkDeviceWaitIdle(device));
            double t0 = now_ns();
            CHECK(vkQueueSubmit(queue, 1, &s1, VK_NULL_HANDLE));
            t += now_ns() - t0;
        }
        v[r] = t / submits / 1000.0;
    }
    report("submit", "us/sub", v, reps);

    vkDeviceWaitIdle(device);
    printf("RESULT: a22cpu finished\n");
    return 0;
}
