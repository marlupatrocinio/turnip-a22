/*
 * a22gmem - GMEM (tiled) vs SYSMEM (direct) rendering reproducer for Turnip.
 *
 * Frame shapes (GM_MODE):
 *
 * subpass (default) - ONE render pass, two subpasses:
 *   0  4 colour targets + depth/stencil, GM_DRAWS overlapping quads,
 *      depth LESS + write, stencil REPLACE with a per-draw reference
 *   1  reads the 4 targets as input attachments, stencil NOT_EQUAL 0 mask,
 *      writes a final RGBA8 target
 *
 * multipass - THREE render passes, the usual shape of an emulator frame:
 *   G     same G-buffer as above, CLEAR + STORE (GMEM: resolve to memory)
 *   L     samples the 4 targets as textures (texelFetch through the TP),
 *         LOADs the stored depth/stencil for the stencil mask, writes final
 *   P     LOAD_OP_LOAD of colour 0, final and depth/stencil (GMEM: unresolve)
 *         over a render area that is smaller than the frame and not
 *         bin-aligned (GM_AREA), GM_DRAWS/3 more quads depth-tested against
 *         the loaded depth. GM_POST=0 skips it.
 *
 * botw - the shape of Breath of the Wild's main pass in Eden (from a u_trace
 *   capture): 1-8 colour targets (default 6: r11g11b10, rg8, rgba8_srgb,
 *   rgba8, rgba8, rgba8), depth without stencil (default d32), colour images
 *   created MUTABLE_FORMAT (Turnip then disables UBWC on them, as in BOTW),
 *   one pass that CLEARs and draws, then GM_REPEAT passes that LOAD every
 *   attachment (no clears) and draw GM_DRAWS/3 more depth-tested quads.
 *
 * Every plane (colours, depth, stencil if any, final if any) is copied to a
 * file. Run once per mode in separate processes (TU_DEBUG is read at
 * startup) and compare:
 *
 *   TU_DEBUG=sysmem ./a22gmem lib.so sys.bin [W H]
 *   TU_DEBUG=gmem   ./a22gmem lib.so gmem.bin [W H]
 *   ./a22gmem --compare sys.bin gmem.bin
 *
 * Environment:
 *   GM_MODE     subpass (default) | multipass | botw
 *   GM_FORMATS  comma-separated colour formats: exactly 4 for subpass and
 *               multipass, 1-8 for botw. Choices: rgba8 rgba8_srgb rg8 r8
 *               rgba16f rg16f r16f r11g11b10 rgb10a2 r32f rgba32f
 *   GM_DEPTH    d32s8 | d24s8 | d32 | d16 (default d32s8, botw: d32)
 *   GM_DRAWS    quads in the first pass (default 300)
 *   GM_SEED     quad layout seed (default 1)
 *   GM_POST     multipass: 0 skips pass P (default 1)
 *   GM_AREA     multipass: pass P render area "x,y,w,h" or "full"
 *               (default: x=W/7 y=H/5+3 w=W/2+13 h=H/2+7)
 *   GM_MUTABLE  botw: 1 (default) creates colour images MUTABLE_FORMAT
 *   GM_REPEAT   botw: number of LOAD passes (default 3)
 *   GM_INCLEAR  botw: color | depth | both - vkCmdClearAttachments of one
 *               unaligned rectangle halfway through every LOAD pass
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#define VK_NO_PROTOTYPES
#include "android_hwvulkan.h"
#include <vulkan/vulkan.h>

#include "gmem_vert_spv.h"
#include "gmem_geom_frag_spv.h"
#include "gmem_light_frag_spv.h"
#include "gmem_light_tex_frag_spv.h"

#define CHECK(x) do { VkResult _r = (x); if (_r != VK_SUCCESS) { \
    printf("FAILED %s -> %d (line %d)\n", #x, _r, __LINE__); exit(1); } } while (0)

#define NCOLOR 4            /* colour targets in subpass / multipass modes */
#define MAXCOLOR 8          /* colour targets in botw mode */
#define MAXPLANES (MAXCOLOR + 3)
#define PC_STAGES (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT)

/* ------------------------------------------------------------ entrypoints */
#define VK_FUNCS(X) \
    X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceQueue) \
    X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindImageMemory) X(vkCreateImageView) X(vkCreateBuffer) \
    X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) X(vkMapMemory) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdNextSubpass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) \
    X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetStencilReference) \
    X(vkCmdDraw) X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkCmdClearAttachments) \
    X(vkCreateSampler) X(vkCreateFence) X(vkQueueSubmit) \
    X(vkWaitForFences) X(vkDeviceWaitIdle)

#define DECL(n) static PFN_##n n;
VK_FUNCS(DECL)

static PFN_vkGetInstanceProcAddr gipa;
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
    gipa = vdev->GetInstanceProcAddr;

    VkApplicationInfo app = {
        .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
        .pApplicationName = "a22gmem",
        .apiVersion = VK_API_VERSION_1_3,
    };
    VkInstanceCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
        .pApplicationInfo = &app,
    };
    CHECK(vdev->CreateInstance(&ici, NULL, &instance));

#define LOAD(n) n = (PFN_##n)gipa(instance, #n); \
    if (!n) { printf("missing entrypoint %s\n", #n); exit(1); }
    VK_FUNCS(LOAD)

    uint32_t n = 1;
    vkEnumeratePhysicalDevices(instance, &n, &phys);
    if (!n) { printf("no physical device\n"); exit(1); }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(phys, &props);
    printf("GPU: %s\n", props.deviceName);

    VkQueueFamilyProperties qp[16];
    uint32_t qn = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qp);
    queue_family = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++)
        if (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { queue_family = i; break; }
    if (queue_family == UINT32_MAX) { printf("no graphics queue\n"); exit(1); }

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
    printf("no suitable memory type\n");
    exit(1);
}

/* ---------------------------------------------------------------- formats */
struct fmt { const char *name; VkFormat vk; uint32_t bpp; };
static const struct fmt color_formats[] = {
    { "rgba8",      VK_FORMAT_R8G8B8A8_UNORM,          4 },
    { "rgba8_srgb", VK_FORMAT_R8G8B8A8_SRGB,           4 },
    { "rg8",        VK_FORMAT_R8G8_UNORM,              2 },
    { "r8",         VK_FORMAT_R8_UNORM,                1 },
    { "rgba16f",    VK_FORMAT_R16G16B16A16_SFLOAT,     8 },
    { "rg16f",      VK_FORMAT_R16G16_SFLOAT,           4 },
    { "r16f",       VK_FORMAT_R16_SFLOAT,              2 },
    { "r11g11b10",  VK_FORMAT_B10G11R11_UFLOAT_PACK32, 4 },
    { "rgb10a2",    VK_FORMAT_A2B10G10R10_UNORM_PACK32, 4 },
    { "r32f",       VK_FORMAT_R32_SFLOAT,              4 },
    { "rgba32f",    VK_FORMAT_R32G32B32A32_SFLOAT,    16 },
};

static const struct fmt *lookup_color(const char *name)
{
    for (size_t i = 0; i < sizeof(color_formats) / sizeof(color_formats[0]); i++)
        if (!strcmp(color_formats[i].name, name))
            return &color_formats[i];
    printf("unknown colour format '%s'\n", name);
    exit(1);
}

struct depth_fmt { const char *name; VkFormat vk; uint32_t depth_bpp; int stencil; };
static const struct depth_fmt depth_formats[] = {
    { "d32s8", VK_FORMAT_D32_SFLOAT_S8_UINT, 4, 1 },
    { "d24s8", VK_FORMAT_D24_UNORM_S8_UINT,  4, 1 },
    { "d32",   VK_FORMAT_D32_SFLOAT,         4, 0 },
    { "d16",   VK_FORMAT_D16_UNORM,          2, 0 },
};

static const struct depth_fmt *lookup_depth(const char *name)
{
    for (size_t i = 0; i < sizeof(depth_formats) / sizeof(depth_formats[0]); i++)
        if (!strcmp(depth_formats[i].name, name))
            return &depth_formats[i];
    printf("unknown depth format '%s'\n", name);
    exit(1);
}

/* ------------------------------------------------------------- resources */
struct image {
    VkImage img;
    VkImageView view;
    VkFormat format;
    VkImageAspectFlags aspect;
    VkImageLayout layout;    /* layout after the last recorded use */
};

static struct image make_image(uint32_t w, uint32_t h, VkFormat format,
                               VkImageUsageFlags usage, VkImageAspectFlags aspect,
                               VkImageCreateFlags flags)
{
    struct image r = { .format = format, .aspect = aspect,
                       .layout = VK_IMAGE_LAYOUT_UNDEFINED };
    VkImageCreateInfo ici = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .flags = flags,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = { w, h, 1 },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = usage,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    CHECK(vkCreateImage(device, &ici, NULL, &r.img));
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(device, r.img, &req);
    VkMemoryAllocateInfo mai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = req.size,
        .memoryTypeIndex = find_memory(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT),
    };
    VkDeviceMemory mem;
    CHECK(vkAllocateMemory(device, &mai, NULL, &mem));
    CHECK(vkBindImageMemory(device, r.img, mem, 0));
    VkImageViewCreateInfo vci = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .image = r.img,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = format,
        .subresourceRange = { aspect, 0, 1, 0, 1 },
    };
    CHECK(vkCreateImageView(device, &vci, NULL, &r.view));
    return r;
}

static VkShaderModule make_shader(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo smci = {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = code,
    };
    VkShaderModule sm;
    CHECK(vkCreateShaderModule(device, &smci, NULL, &sm));
    return sm;
}

static VkFramebuffer make_fb(VkRenderPass rp, uint32_t n, const VkImageView *views,
                             uint32_t w, uint32_t h)
{
    VkFramebufferCreateInfo fbci = {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .renderPass = rp,
        .attachmentCount = n,
        .pAttachments = views,
        .width = w,
        .height = h,
        .layers = 1,
    };
    VkFramebuffer fb;
    CHECK(vkCreateFramebuffer(device, &fbci, NULL, &fb));
    return fb;
}

/* Conservative external dependencies: everything before / after the pass. */
static const VkSubpassDependency ext_deps[2] = {
    {
        .srcSubpass = VK_SUBPASS_EXTERNAL, .dstSubpass = 0,
        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    },
    {
        .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
        .srcStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
    },
};

static VkAttachmentDescription att_desc(VkFormat f, VkAttachmentLoadOp load,
                                        VkImageLayout initial, VkImageLayout final,
                                        int has_stencil)
{
    return (VkAttachmentDescription){
        .format = f,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = load,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = has_stencil ? load : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = has_stencil ? VK_ATTACHMENT_STORE_OP_STORE
                                      : VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = initial,
        .finalLayout = final,
    };
}

/* ------------------------------------------------------------- pipelines */
static VkStencilOpState st_write = {
    .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_REPLACE,
    .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_ALWAYS,
    .compareMask = 0xff, .writeMask = 0xff,
};
static VkStencilOpState st_mask = {
    .failOp = VK_STENCIL_OP_KEEP, .passOp = VK_STENCIL_OP_KEEP,
    .depthFailOp = VK_STENCIL_OP_KEEP, .compareOp = VK_COMPARE_OP_NOT_EQUAL,
    .compareMask = 0xff, .writeMask = 0,
};

static VkPipeline make_pipeline(VkRenderPass rp, uint32_t subpass, VkPipelineLayout pl,
                                VkShaderModule vs, VkShaderModule fs,
                                uint32_t ncolor, int geometry, int stencil)
{
    VkPipelineVertexInputStateCreateInfo vi = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
    };
    VkPipelineViewportStateCreateInfo vp = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .viewportCount = 1,
        .scissorCount = 1,
    };
    VkPipelineRasterizationStateCreateInfo rs = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo ms = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
    };
    /* geometry: depth LESS + write, stencil REPLACE; lighting: stencil mask */
    VkPipelineDepthStencilStateCreateInfo dss = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
        .depthTestEnable = geometry,
        .depthWriteEnable = geometry,
        .depthCompareOp = VK_COMPARE_OP_LESS,
        .stencilTestEnable = stencil,
        .front = geometry ? st_write : st_mask,
        .back = geometry ? st_write : st_mask,
    };
    VkPipelineColorBlendAttachmentState cba[MAXCOLOR];
    for (int i = 0; i < MAXCOLOR; i++)
        cba[i] = (VkPipelineColorBlendAttachmentState){ .colorWriteMask = 0xf };
    VkPipelineColorBlendStateCreateInfo cb = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .attachmentCount = ncolor,
        .pAttachments = cba,
    };
    VkDynamicState dyn[] = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
    };
    VkPipelineDynamicStateCreateInfo dy = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .dynamicStateCount = 3,
        .pDynamicStates = dyn,
    };
    VkPipelineShaderStageCreateInfo stages[2] = {
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = vs, .pName = "main" },
        { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
          .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = fs, .pName = "main" },
    };
    VkGraphicsPipelineCreateInfo gpci = {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .stageCount = 2, .pStages = stages,
        .pVertexInputState = &vi, .pInputAssemblyState = &ia,
        .pViewportState = &vp, .pRasterizationState = &rs,
        .pMultisampleState = &ms, .pDepthStencilState = &dss,
        .pColorBlendState = &cb, .pDynamicState = &dy,
        .layout = pl, .renderPass = rp, .subpass = subpass,
    };
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &gpci, NULL, &p));
    return p;
}

static VkPipelineLayout make_layout(VkDescriptorType type, VkDescriptorSetLayout *dsl_out)
{
    VkDescriptorSetLayoutBinding binds[NCOLOR];
    for (int i = 0; i < NCOLOR; i++)
        binds[i] = (VkDescriptorSetLayoutBinding){
            .binding = i,
            .descriptorType = type,
            .descriptorCount = 1,
            .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        };
    VkDescriptorSetLayoutCreateInfo dslci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .bindingCount = NCOLOR,
        .pBindings = binds,
    };
    CHECK(vkCreateDescriptorSetLayout(device, &dslci, NULL, dsl_out));
    VkPushConstantRange pcr = { PC_STAGES, 0, 32 };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .setLayoutCount = 1,
        .pSetLayouts = dsl_out,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pl));
    return pl;
}

static VkDescriptorSet make_set(VkDescriptorSetLayout dsl, VkDescriptorType type,
                                const struct image *color, VkSampler sampler)
{
    VkDescriptorPoolSize ps = { type, NCOLOR };
    VkDescriptorPoolCreateInfo dpci = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &ps,
    };
    VkDescriptorPool dpool;
    CHECK(vkCreateDescriptorPool(device, &dpci, NULL, &dpool));
    VkDescriptorSetAllocateInfo dsai = {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = dpool,
        .descriptorSetCount = 1,
        .pSetLayouts = &dsl,
    };
    VkDescriptorSet ds;
    CHECK(vkAllocateDescriptorSets(device, &dsai, &ds));
    VkDescriptorImageInfo dii[NCOLOR];
    VkWriteDescriptorSet wds[NCOLOR];
    for (int i = 0; i < NCOLOR; i++) {
        dii[i] = (VkDescriptorImageInfo){ sampler, color[i].view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        wds[i] = (VkWriteDescriptorSet){
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = ds,
            .dstBinding = i,
            .descriptorCount = 1,
            .descriptorType = type,
            .pImageInfo = &dii[i],
        };
    }
    vkUpdateDescriptorSets(device, NCOLOR, wds, 0, NULL);
    return ds;
}

static uint32_t lcg_state;
static float rnd(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return (lcg_state >> 8) * (1.0f / 16777216.0f);
}

static void draw_quads(VkCommandBuffer cmd, VkPipelineLayout pl, int first, int count)
{
    for (int i = first; i < first + count; i++) {
        float w = 0.04f + rnd() * 1.1f, h = 0.04f + rnd() * 1.1f;
        float x = rnd() * (2.0f - w) - 1.0f, y = rnd() * (2.0f - h) - 1.0f;
        float pc[8] = { x, y, x + w, y + h, 0.02f + rnd() * 0.96f, (float)i, 0, 0 };
        vkCmdPushConstants(cmd, pl, PC_STAGES, 0, sizeof(pc), pc);
        vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 1 + (i % 254));
        vkCmdDraw(cmd, 6, 1, 0, 0);
    }
}

static void draw_fullscreen(VkCommandBuffer cmd, VkPipelineLayout pl)
{
    float full[8] = { -1, -1, 1, 1, 0, 0, 0, 0 };
    vkCmdPushConstants(cmd, pl, PC_STAGES, 0, sizeof(full), full);
    vkCmdSetStencilReference(cmd, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
    vkCmdDraw(cmd, 6, 1, 0, 0);
}

static void set_area(VkCommandBuffer cmd, uint32_t W, uint32_t H, VkRect2D scissor)
{
    VkViewport viewport = { 0, 0, (float)W, (float)H, 0.0f, 1.0f };
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
}

static void begin_rp(VkCommandBuffer cmd, VkRenderPass rp, VkFramebuffer fb, VkRect2D area,
                     uint32_t nclear, const VkClearValue *clears)
{
    VkRenderPassBeginInfo rpbi = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = rp,
        .framebuffer = fb,
        .renderArea = area,
        .clearValueCount = nclear,
        .pClearValues = clears,
    };
    vkCmdBeginRenderPass(cmd, &rpbi, VK_SUBPASS_CONTENTS_INLINE);
}

struct frame {
    uint32_t W, H;
    int draws;
    uint32_t ncolor;
    int has_stencil, has_final;
    struct image color[MAXCOLOR], depth, final;
    VkShaderModule vs, fs_geom, fs_light, fs_light_tex;
    VkClearValue color_clear[MAXCOLOR], depth_clear, final_clear;
};

/* ---------------------------------------------------- mode: two subpasses */
static void record_subpass(VkCommandBuffer cmd, struct frame *fr)
{
    const int S = fr->has_stencil;
    VkAttachmentDescription att[NCOLOR + 2];
    for (int i = 0; i < NCOLOR; i++)
        att[i] = att_desc(fr->color[i].format, VK_ATTACHMENT_LOAD_OP_CLEAR,
                          VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);
    att[NCOLOR] = att_desc(fr->depth.format, VK_ATTACHMENT_LOAD_OP_CLEAR,
                           VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S);
    att[NCOLOR + 1] = att_desc(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR,
                               VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, 0);

    VkAttachmentReference geom_color[NCOLOR], light_input[NCOLOR];
    for (int i = 0; i < NCOLOR; i++) {
        geom_color[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        light_input[i] = (VkAttachmentReference){ i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    }
    VkAttachmentReference geom_ds = { NCOLOR, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    VkAttachmentReference light_ds = { NCOLOR, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL };
    VkAttachmentReference light_out = { NCOLOR + 1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };

    VkSubpassDescription sub[2] = {
        {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .colorAttachmentCount = NCOLOR,
            .pColorAttachments = geom_color,
            .pDepthStencilAttachment = &geom_ds,
        },
        {
            .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
            .inputAttachmentCount = NCOLOR,
            .pInputAttachments = light_input,
            .colorAttachmentCount = 1,
            .pColorAttachments = &light_out,
            .pDepthStencilAttachment = &light_ds,
        },
    };
    const VkPipelineStageFlags frag_out =
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
        VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
        VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    const VkAccessFlags att_write =
        VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    VkSubpassDependency dep[3] = {
        {
            .srcSubpass = 0, .dstSubpass = 1,
            .srcStageMask = frag_out,
            .dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | frag_out,
            .srcAccessMask = att_write,
            .dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT |
                             VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT,
            .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT,
        },
        {
            .srcSubpass = 0, .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = frag_out,
            .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = att_write,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        },
        {
            .srcSubpass = 1, .dstSubpass = VK_SUBPASS_EXTERNAL,
            .srcStageMask = frag_out | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT,
            .srcAccessMask = att_write,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        },
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = NCOLOR + 2, .pAttachments = att,
        .subpassCount = 2, .pSubpasses = sub,
        .dependencyCount = 3, .pDependencies = dep,
    };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &rp));

    VkImageView views[NCOLOR + 2];
    for (int i = 0; i < NCOLOR; i++)
        views[i] = fr->color[i].view;
    views[NCOLOR] = fr->depth.view;
    views[NCOLOR + 1] = fr->final.view;
    VkFramebuffer fb = make_fb(rp, NCOLOR + 2, views, fr->W, fr->H);

    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl = make_layout(VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &dsl);
    VkDescriptorSet ds = make_set(dsl, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, fr->color,
                                  VK_NULL_HANDLE);
    VkPipeline geom = make_pipeline(rp, 0, pl, fr->vs, fr->fs_geom, NCOLOR, 1, S);
    VkPipeline light = make_pipeline(rp, 1, pl, fr->vs, fr->fs_light, 1, 0, S);

    VkClearValue clears[NCOLOR + 2];
    for (int i = 0; i < NCOLOR; i++)
        clears[i] = fr->color_clear[i];
    clears[NCOLOR] = fr->depth_clear;
    clears[NCOLOR + 1] = fr->final_clear;

    VkRect2D full = { { 0, 0 }, { fr->W, fr->H } };
    begin_rp(cmd, rp, fb, full, NCOLOR + 2, clears);
    set_area(cmd, fr->W, fr->H, full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, geom);
    draw_quads(cmd, pl, 0, fr->draws);
    vkCmdNextSubpass(cmd, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, light);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    draw_fullscreen(cmd, pl);
    vkCmdEndRenderPass(cmd);

    for (int i = 0; i < NCOLOR; i++)
        fr->color[i].layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    fr->depth.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    fr->final.layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

/* --------------------------------------------- mode: three render passes */
static VkRect2D post_area(uint32_t W, uint32_t H)
{
    const char *e = getenv("GM_AREA");
    if (e && !strcmp(e, "full"))
        return (VkRect2D){ { 0, 0 }, { W, H } };
    int x = W / 7, y = H / 5 + 3, w = W / 2 + 13, h = H / 2 + 7;
    if (e && *e)
        sscanf(e, "%d,%d,%d,%d", &x, &y, &w, &h);
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > (int)W) w = W - x;
    if (y + h > (int)H) h = H - y;
    return (VkRect2D){ { x, y }, { (uint32_t)w, (uint32_t)h } };
}

static void record_multipass(VkCommandBuffer cmd, struct frame *fr)
{
    const VkImageLayout RO = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const VkImageLayout CA = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkImageLayout DS = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkImageLayout TS = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    const VkImageLayout UN = VK_IMAGE_LAYOUT_UNDEFINED;
    const int S = fr->has_stencil;
    int post = !getenv("GM_POST") || atoi(getenv("GM_POST")) != 0;
    VkRect2D full = { { 0, 0 }, { fr->W, fr->H } };

    /* pass G: colours -> SHADER_READ_ONLY, depth/stencil stays attachment */
    VkAttachmentDescription g_att[NCOLOR + 1];
    VkAttachmentReference g_col[NCOLOR];
    for (int i = 0; i < NCOLOR; i++) {
        g_att[i] = att_desc(fr->color[i].format, VK_ATTACHMENT_LOAD_OP_CLEAR, UN, RO, 0);
        g_col[i] = (VkAttachmentReference){ i, CA };
    }
    g_att[NCOLOR] = att_desc(fr->depth.format, VK_ATTACHMENT_LOAD_OP_CLEAR, UN, DS, S);
    VkAttachmentReference g_ds = { NCOLOR, DS };
    VkSubpassDescription g_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = NCOLOR, .pColorAttachments = g_col,
        .pDepthStencilAttachment = &g_ds,
    };
    VkRenderPassCreateInfo g_rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = NCOLOR + 1, .pAttachments = g_att,
        .subpassCount = 1, .pSubpasses = &g_sub,
        .dependencyCount = 2, .pDependencies = ext_deps,
    };
    VkRenderPass rp_g;
    CHECK(vkCreateRenderPass(device, &g_rpci, NULL, &rp_g));
    VkImageView g_views[NCOLOR + 1];
    for (int i = 0; i < NCOLOR; i++)
        g_views[i] = fr->color[i].view;
    g_views[NCOLOR] = fr->depth.view;
    VkFramebuffer fb_g = make_fb(rp_g, NCOLOR + 1, g_views, fr->W, fr->H);

    /* pass L: final (CLEAR), depth/stencil LOADed for the stencil mask */
    VkAttachmentDescription l_att[2] = {
        att_desc(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_CLEAR, UN,
                 post ? CA : TS, 0),
        att_desc(fr->depth.format, VK_ATTACHMENT_LOAD_OP_LOAD, DS, post ? DS : TS, S),
    };
    VkAttachmentReference l_col = { 0, CA }, l_ds = { 1, DS };
    VkSubpassDescription l_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 1, .pColorAttachments = &l_col,
        .pDepthStencilAttachment = &l_ds,
    };
    VkRenderPassCreateInfo l_rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 2, .pAttachments = l_att,
        .subpassCount = 1, .pSubpasses = &l_sub,
        .dependencyCount = 2, .pDependencies = ext_deps,
    };
    VkRenderPass rp_l;
    CHECK(vkCreateRenderPass(device, &l_rpci, NULL, &rp_l));
    VkImageView l_views[2] = { fr->final.view, fr->depth.view };
    VkFramebuffer fb_l = make_fb(rp_l, 2, l_views, fr->W, fr->H);

    /* pass P: LOAD colour 0, final and depth/stencil, partial render area */
    VkAttachmentDescription p_att[3] = {
        att_desc(fr->color[0].format, VK_ATTACHMENT_LOAD_OP_LOAD, RO, TS, 0),
        att_desc(VK_FORMAT_R8G8B8A8_UNORM, VK_ATTACHMENT_LOAD_OP_LOAD, CA, TS, 0),
        att_desc(fr->depth.format, VK_ATTACHMENT_LOAD_OP_LOAD, DS, TS, S),
    };
    VkAttachmentReference p_col[2] = { { 0, CA }, { 1, CA } }, p_ds = { 2, DS };
    VkSubpassDescription p_sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = 2, .pColorAttachments = p_col,
        .pDepthStencilAttachment = &p_ds,
    };
    VkRenderPassCreateInfo p_rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = 3, .pAttachments = p_att,
        .subpassCount = 1, .pSubpasses = &p_sub,
        .dependencyCount = 2, .pDependencies = ext_deps,
    };
    VkRenderPass rp_p;
    CHECK(vkCreateRenderPass(device, &p_rpci, NULL, &rp_p));
    VkImageView p_views[3] = { fr->color[0].view, fr->final.view, fr->depth.view };
    VkFramebuffer fb_p = make_fb(rp_p, 3, p_views, fr->W, fr->H);

    /* sampled G-buffer for pass L */
    VkSamplerCreateInfo sci = {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = VK_FILTER_NEAREST,
        .minFilter = VK_FILTER_NEAREST,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
    };
    VkSampler sampler;
    CHECK(vkCreateSampler(device, &sci, NULL, &sampler));
    VkDescriptorSetLayout dsl;
    VkPipelineLayout pl = make_layout(VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dsl);
    VkDescriptorSet ds = make_set(dsl, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                  fr->color, sampler);

    VkPipeline geom = make_pipeline(rp_g, 0, pl, fr->vs, fr->fs_geom, NCOLOR, 1, S);
    VkPipeline light = make_pipeline(rp_l, 0, pl, fr->vs, fr->fs_light_tex, 1, 0, S);
    VkPipeline postp = make_pipeline(rp_p, 0, pl, fr->vs, fr->fs_geom, 2, 1, S);

    /* G */
    VkClearValue g_clear[NCOLOR + 1];
    for (int i = 0; i < NCOLOR; i++)
        g_clear[i] = fr->color_clear[i];
    g_clear[NCOLOR] = fr->depth_clear;
    begin_rp(cmd, rp_g, fb_g, full, NCOLOR + 1, g_clear);
    set_area(cmd, fr->W, fr->H, full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, geom);
    draw_quads(cmd, pl, 0, fr->draws);
    vkCmdEndRenderPass(cmd);

    /* L */
    VkClearValue l_clear[2] = { fr->final_clear, fr->depth_clear };
    begin_rp(cmd, rp_l, fb_l, full, 2, l_clear);
    set_area(cmd, fr->W, fr->H, full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, light);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 1, &ds, 0, NULL);
    draw_fullscreen(cmd, pl);
    vkCmdEndRenderPass(cmd);

    for (int i = 0; i < NCOLOR; i++)
        fr->color[i].layout = RO;
    fr->depth.layout = post ? DS : TS;
    fr->final.layout = post ? CA : TS;

    /* P */
    if (post) {
        VkRect2D area = post_area(fr->W, fr->H);
        printf("post pass area (%d,%d) %ux%u\n", area.offset.x, area.offset.y,
               area.extent.width, area.extent.height);
        begin_rp(cmd, rp_p, fb_p, area, 0, NULL);
        set_area(cmd, fr->W, fr->H, area);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, postp);
        draw_quads(cmd, pl, 10000, fr->draws / 3 + 1);
        vkCmdEndRenderPass(cmd);
        fr->color[0].layout = TS;
        fr->depth.layout = TS;
        fr->final.layout = TS;
    }
}

/* ----------------------------------- mode: BOTW main-pass shape (Eden) */
static VkRenderPass botw_rp(struct frame *fr, VkAttachmentLoadOp load)
{
    const VkImageLayout CA = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    const VkImageLayout DS = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    const VkImageLayout init_c = load == VK_ATTACHMENT_LOAD_OP_LOAD ? CA : VK_IMAGE_LAYOUT_UNDEFINED;
    const VkImageLayout init_d = load == VK_ATTACHMENT_LOAD_OP_LOAD ? DS : VK_IMAGE_LAYOUT_UNDEFINED;
    VkAttachmentDescription att[MAXCOLOR + 1];
    VkAttachmentReference col[MAXCOLOR];
    for (uint32_t i = 0; i < fr->ncolor; i++) {
        att[i] = att_desc(fr->color[i].format, load, init_c, CA, 0);
        col[i] = (VkAttachmentReference){ i, CA };
    }
    att[fr->ncolor] = att_desc(fr->depth.format, load, init_d, DS, fr->has_stencil);
    VkAttachmentReference ds = { fr->ncolor, DS };
    VkSubpassDescription sub = {
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .colorAttachmentCount = fr->ncolor, .pColorAttachments = col,
        .pDepthStencilAttachment = &ds,
    };
    VkRenderPassCreateInfo rpci = {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .attachmentCount = fr->ncolor + 1, .pAttachments = att,
        .subpassCount = 1, .pSubpasses = &sub,
        .dependencyCount = 2, .pDependencies = ext_deps,
    };
    VkRenderPass rp;
    CHECK(vkCreateRenderPass(device, &rpci, NULL, &rp));
    return rp;
}

static void record_botw(VkCommandBuffer cmd, struct frame *fr)
{
    int repeat = getenv("GM_REPEAT") ? atoi(getenv("GM_REPEAT")) : 3;
    VkRenderPass rp_clear = botw_rp(fr, VK_ATTACHMENT_LOAD_OP_CLEAR);
    VkRenderPass rp_load = botw_rp(fr, VK_ATTACHMENT_LOAD_OP_LOAD);
    VkImageView views[MAXCOLOR + 1];
    for (uint32_t i = 0; i < fr->ncolor; i++)
        views[i] = fr->color[i].view;
    views[fr->ncolor] = fr->depth.view;
    VkFramebuffer fb_clear = make_fb(rp_clear, fr->ncolor + 1, views, fr->W, fr->H);
    VkFramebuffer fb_load = make_fb(rp_load, fr->ncolor + 1, views, fr->W, fr->H);

    VkPushConstantRange pcr = { PC_STAGES, 0, 32 };
    VkPipelineLayoutCreateInfo plci = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &pcr,
    };
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(device, &plci, NULL, &pl));
    VkPipeline p_clear = make_pipeline(rp_clear, 0, pl, fr->vs, fr->fs_geom, fr->ncolor, 1,
                                       fr->has_stencil);
    VkPipeline p_load = make_pipeline(rp_load, 0, pl, fr->vs, fr->fs_geom, fr->ncolor, 1,
                                      fr->has_stencil);

    VkClearValue clears[MAXCOLOR + 1];
    for (uint32_t i = 0; i < fr->ncolor; i++)
        clears[i] = fr->color_clear[i];
    clears[fr->ncolor] = fr->depth_clear;
    VkRect2D full = { { 0, 0 }, { fr->W, fr->H } };

    begin_rp(cmd, rp_clear, fb_clear, full, fr->ncolor + 1, clears);
    set_area(cmd, fr->W, fr->H, full);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p_clear);
    draw_quads(cmd, pl, 0, fr->draws);
    vkCmdEndRenderPass(cmd);

    /* GM_INCLEAR: vkCmdClearAttachments in the middle of every LOAD pass, one
     * rectangle that is not bin-aligned (BOTW does this in multi-bin passes) */
    const char *ic = getenv("GM_INCLEAR");
    int clear_color = ic && (!strcmp(ic, "color") || !strcmp(ic, "both"));
    int clear_depth = ic && (!strcmp(ic, "depth") || !strcmp(ic, "both"));
    int half = (fr->draws / 3 + 1) / 2;
    for (int r = 0; r < repeat; r++) {
        int first = 10000 + r * 1000;
        begin_rp(cmd, rp_load, fb_load, full, 0, NULL);
        set_area(cmd, fr->W, fr->H, full);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p_load);
        draw_quads(cmd, pl, first, half);
        if (clear_color || clear_depth) {
            VkClearAttachment ca[2];
            uint32_t n = 0;
            if (clear_color)
                ca[n++] = (VkClearAttachment){
                    .aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = 0,
                    .clearValue.color = { .float32 = { 0.625f, 0.125f + 0.25f * r, 0.75f, 1.0f } } };
            if (clear_depth)
                ca[n++] = (VkClearAttachment){
                    .aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT |
                                  (fr->has_stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0),
                    .clearValue.depthStencil = { 0.5f, 7 } };
            VkClearRect rect = {
                .rect = { { (int32_t)(fr->W / 5 + 3 + 17 * r), (int32_t)(fr->H / 7 + 1 + 11 * r) },
                          { fr->W / 3 + 11, fr->H / 4 + 5 } },
                .baseArrayLayer = 0, .layerCount = 1,
            };
            vkCmdClearAttachments(cmd, n, ca, 1, &rect);
        }
        draw_quads(cmd, pl, first + half, fr->draws / 3 + 1 - half);
        vkCmdEndRenderPass(cmd);
    }
    for (uint32_t i = 0; i < fr->ncolor; i++)
        fr->color[i].layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    fr->depth.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
}

/* ------------------------------------------------------------- dump file */
struct plane_hdr {
    char name[16];
    uint32_t bpp;
    uint32_t width, height;
};

static void to_transfer_src(VkCommandBuffer cmd, struct image *im)
{
    if (im->layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL)
        return;
    VkImageMemoryBarrier b = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
        .oldLayout = im->layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = im->img,
        .subresourceRange = { im->aspect, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &b);
    im->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
}

struct plane {
    char name[16];
    uint32_t bpp;
    VkImage img;
    VkImageAspectFlags aspect;
    VkDeviceSize off;
};

static int render(const char *lib, const char *out, uint32_t W, uint32_t H)
{
    const char *menv = getenv("GM_MODE");
    enum { SUBPASS, MULTIPASS, BOTW } mode =
        !menv || !*menv || !strcmp(menv, "subpass") ? SUBPASS :
        !strcmp(menv, "multipass") ? MULTIPASS :
        !strcmp(menv, "botw") ? BOTW : -1;
    if ((int)mode < 0) { printf("unknown GM_MODE '%s'\n", menv); return 1; }
    static const char *mode_name[] = { "subpass", "multipass", "botw" };

    const char *fenv = getenv("GM_FORMATS");
    const char *fdef = mode == BOTW ? "r11g11b10,rg8,rgba8_srgb,rgba8,rgba8,rgba8"
                                    : "rgba16f,r11g11b10,rgba8,rgb10a2";
    char fbuf[256];
    snprintf(fbuf, sizeof(fbuf), "%s", fenv && *fenv ? fenv : fdef);
    struct frame fr = { .W = W, .H = H, .has_final = mode != BOTW };
    const struct fmt *cf[MAXCOLOR];
    char *save = NULL;
    for (char *tok = strtok_r(fbuf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        if (fr.ncolor == MAXCOLOR) { printf("at most %d colour formats\n", MAXCOLOR); return 1; }
        cf[fr.ncolor++] = lookup_color(tok);
    }
    if (mode != BOTW && fr.ncolor != NCOLOR) {
        printf("GM_FORMATS needs exactly %d formats in %s mode\n", NCOLOR, mode_name[mode]);
        return 1;
    }
    if (fr.ncolor == 0) { printf("GM_FORMATS is empty\n"); return 1; }
    const char *denv = getenv("GM_DEPTH");
    const struct depth_fmt *df = lookup_depth(denv && *denv ? denv
                                              : mode == BOTW ? "d32" : "d32s8");
    fr.has_stencil = df->stencil;
    int mutable_fmt = mode == BOTW && (!getenv("GM_MUTABLE") || atoi(getenv("GM_MUTABLE")));
    fr.draws = getenv("GM_DRAWS") ? atoi(getenv("GM_DRAWS")) : 300;
    lcg_state = getenv("GM_SEED") ? (uint32_t)atoi(getenv("GM_SEED")) : 1u;

    init_vulkan(lib);
    const char *tu = getenv("TU_DEBUG");
    printf("mode %s  frame %ux%u  formats", mode_name[mode], W, H);
    for (uint32_t i = 0; i < fr.ncolor; i++)
        printf("%c%s", i ? ',' : ' ', cf[i]->name);
    printf("  depth %s%s  draws %d  TU_DEBUG=%s\n", df->name, mutable_fmt ? "  mutable" : "",
           fr.draws, tu ? tu : "");

    for (uint32_t i = 0; i < fr.ncolor; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(phys, cf[i]->vk, &fp);
        VkFormatFeatureFlags need = VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            (mode == MULTIPASS ? VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT : 0);
        if ((fp.optimalTilingFeatures & need) != need) {
            printf("format %s is not a colour attachment%s here\n", cf[i]->name,
                   mode == MULTIPASS ? " / sampled image" : "");
            return 1;
        }
    }

    VkImageUsageFlags cu = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                           (mode == BOTW ? 0 : VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
                                               VK_IMAGE_USAGE_SAMPLED_BIT);
    for (uint32_t i = 0; i < fr.ncolor; i++)
        fr.color[i] = make_image(W, H, cf[i]->vk, cu, VK_IMAGE_ASPECT_COLOR_BIT,
                                 mutable_fmt ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0);
    fr.depth = make_image(W, H, df->vk,
                          VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                          VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                          VK_IMAGE_ASPECT_DEPTH_BIT |
                          (df->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0), 0);
    if (fr.has_final)
        fr.final = make_image(W, H, VK_FORMAT_R8G8B8A8_UNORM,
                              VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
                              VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, 0);
    fr.vs = make_shader(gmem_vert_spv, sizeof(gmem_vert_spv));
    fr.fs_geom = make_shader(gmem_geom_frag_spv, sizeof(gmem_geom_frag_spv));
    fr.fs_light = make_shader(gmem_light_frag_spv, sizeof(gmem_light_frag_spv));
    fr.fs_light_tex = make_shader(gmem_light_tex_frag_spv, sizeof(gmem_light_tex_frag_spv));
    for (uint32_t i = 0; i < fr.ncolor; i++)
        fr.color_clear[i].color = (VkClearColorValue){
            .float32 = { 0.1f * (i % 4 + 1), 0.2f, 0.3f, 0.4f } };
    fr.depth_clear.depthStencil = (VkClearDepthStencilValue){ 1.0f, 0 };
    fr.final_clear.color = (VkClearColorValue){ .float32 = { 0.0f, 0.0f, 0.5f, 1.0f } };

    /* planes: colours, depth, stencil (if any), final (if any) */
    struct plane pl[MAXPLANES];
    uint32_t np = 0;
    for (uint32_t i = 0; i < fr.ncolor; i++) {
        pl[np] = (struct plane){ .bpp = cf[i]->bpp, .img = fr.color[i].img,
                                 .aspect = VK_IMAGE_ASPECT_COLOR_BIT };
        snprintf(pl[np].name, sizeof(pl[np].name), "c%u_%s", i, cf[i]->name);
        np++;
    }
    pl[np] = (struct plane){ .name = "depth", .bpp = df->depth_bpp, .img = fr.depth.img,
                             .aspect = VK_IMAGE_ASPECT_DEPTH_BIT };
    np++;
    if (df->stencil) {
        pl[np] = (struct plane){ .name = "stencil", .bpp = 1, .img = fr.depth.img,
                                 .aspect = VK_IMAGE_ASPECT_STENCIL_BIT };
        np++;
    }
    if (fr.has_final) {
        pl[np] = (struct plane){ .name = "final", .bpp = 4, .img = fr.final.img,
                                 .aspect = VK_IMAGE_ASPECT_COLOR_BIT };
        np++;
    }
    VkDeviceSize total = 0;
    for (uint32_t i = 0; i < np; i++) {
        pl[i].off = total;
        total += ((VkDeviceSize)W * H * pl[i].bpp + 255) & ~(VkDeviceSize)255;
    }

    VkBufferCreateInfo bci = {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = total,
        .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT,
    };
    VkBuffer rbuf;
    CHECK(vkCreateBuffer(device, &bci, NULL, &rbuf));
    VkMemoryRequirements breq;
    vkGetBufferMemoryRequirements(device, rbuf, &breq);
    VkMemoryAllocateInfo bmai = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = breq.size,
        .memoryTypeIndex = find_memory(breq.memoryTypeBits,
                                       VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                       VK_MEMORY_PROPERTY_HOST_COHERENT_BIT),
    };
    VkDeviceMemory rmem;
    CHECK(vkAllocateMemory(device, &bmai, NULL, &rmem));
    CHECK(vkBindBufferMemory(device, rbuf, rmem, 0));
    void *map;
    CHECK(vkMapMemory(device, rmem, 0, VK_WHOLE_SIZE, 0, &map));
    memset(map, 0xcd, total);

    VkCommandPoolCreateInfo cpci = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .queueFamilyIndex = queue_family,
    };
    VkCommandPool cpool;
    CHECK(vkCreateCommandPool(device, &cpci, NULL, &cpool));
    VkCommandBufferAllocateInfo cbai = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = cpool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer cmd;
    CHECK(vkAllocateCommandBuffers(device, &cbai, &cmd));
    VkCommandBufferBeginInfo cbbi = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
    };
    CHECK(vkBeginCommandBuffer(cmd, &cbbi));

    if (mode == MULTIPASS)
        record_multipass(cmd, &fr);
    else if (mode == BOTW)
        record_botw(cmd, &fr);
    else
        record_subpass(cmd, &fr);

    for (uint32_t i = 0; i < fr.ncolor; i++)
        to_transfer_src(cmd, &fr.color[i]);
    to_transfer_src(cmd, &fr.depth);
    if (fr.has_final)
        to_transfer_src(cmd, &fr.final);

    for (uint32_t i = 0; i < np; i++) {
        VkBufferImageCopy region = {
            .bufferOffset = pl[i].off,
            .imageSubresource = { pl[i].aspect, 0, 0, 1 },
            .imageExtent = { W, H, 1 },
        };
        vkCmdCopyImageToBuffer(cmd, pl[i].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rbuf,
                               1, &region);
    }
    CHECK(vkEndCommandBuffer(cmd));

    VkFenceCreateInfo fci = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence fence;
    CHECK(vkCreateFence(device, &fci, NULL, &fence));
    VkSubmitInfo si = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .commandBufferCount = 1,
        .pCommandBuffers = &cmd,
    };
    CHECK(vkQueueSubmit(queue, 1, &si, fence));
    CHECK(vkWaitForFences(device, 1, &fence, VK_TRUE, 10ull * 1000 * 1000 * 1000));

    FILE *f = fopen(out, "wb");
    if (!f) { printf("cannot write %s\n", out); return 1; }
    uint32_t magic = 0x4d4d4732; /* "2GMM" */
    fwrite(&magic, 4, 1, f);
    fwrite(&np, 4, 1, f);
    for (uint32_t i = 0; i < np; i++) {
        struct plane_hdr hdr = { .bpp = pl[i].bpp, .width = W, .height = H };
        snprintf(hdr.name, sizeof(hdr.name), "%s", pl[i].name);
        fwrite(&hdr, sizeof(hdr), 1, f);
        fwrite((uint8_t *)map + pl[i].off, 1, (size_t)W * H * pl[i].bpp, f);
    }
    fclose(f);
    vkDeviceWaitIdle(device);
    printf("wrote %s (%u planes)\n", out, np);
    return 0;
}

/* ---------------------------------------------------------------- compare */
static uint8_t *read_file(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    if (!f) { printf("cannot open %s\n", path); exit(2); }
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *p = malloc(*len);
    if (!p || fread(p, 1, *len, f) != *len) { printf("read %s failed\n", path); exit(2); }
    fclose(f);
    return p;
}

static int compare(const char *a_path, const char *b_path)
{
    size_t la, lb;
    uint8_t *a = read_file(a_path, &la), *b = read_file(b_path, &lb);
    if (la != lb || la < 8 || memcmp(a, b, 8)) {
        printf("files differ in size or header\n");
        return 2;
    }
    uint32_t np;
    memcpy(&np, a + 4, 4);
    size_t pos = 8;
    int bad_planes = 0;
    for (uint32_t p = 0; p < np; p++) {
        struct plane_hdr h;
        memcpy(&h, a + pos, sizeof(h));
        pos += sizeof(h);
        const uint8_t *pa = a + pos, *pb = b + pos;
        uint32_t W = h.width, H = h.height, bpp = h.bpp;
        uint64_t bad = 0;
        uint32_t x0 = W, y0 = H, x1 = 0, y1 = 0;
        int shown = 0;
        for (uint32_t y = 0; y < H; y++) {
            for (uint32_t x = 0; x < W; x++) {
                size_t o = ((size_t)y * W + x) * bpp;
                if (memcmp(pa + o, pb + o, bpp)) {
                    bad++;
                    if (x < x0) x0 = x;
                    if (y < y0) y0 = y;
                    if (x > x1) x1 = x;
                    if (y > y1) y1 = y;
                    if (shown < 3) {
                        /* bytes in memory order (little-endian) */
                        printf("    %-14s (%u,%u) a=", h.name, x, y);
                        for (uint32_t k = 0; k < bpp && k < 8; k++) printf("%02x", pa[o + k]);
                        printf(" b=");
                        for (uint32_t k = 0; k < bpp && k < 8; k++) printf("%02x", pb[o + k]);
                        printf("\n");
                        shown++;
                    }
                }
            }
        }
        if (bad) {
            bad_planes++;
            printf("  %-14s DIFF %llu/%u px (%.3f%%) bbox (%u,%u)-(%u,%u)\n", h.name,
                   (unsigned long long)bad, W * H, 100.0 * bad / ((double)W * H), x0, y0, x1, y1);
        } else {
            printf("  %-14s identical\n", h.name);
        }
        pos += (size_t)W * H * bpp;
    }
    printf("RESULT: %s (%d/%u planes differ)\n", bad_planes ? "MISMATCH" : "MATCH",
           bad_planes, np);
    return bad_planes ? 1 : 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc >= 4 && !strcmp(argv[1], "--compare"))
        return compare(argv[2], argv[3]);
    if (argc < 3) {
        printf("usage: %s <libvulkan.so> <out.bin> [W H]\n"
               "       %s --compare <a.bin> <b.bin>\n", argv[0], argv[0]);
        return 2;
    }
    uint32_t W = argc > 4 ? (uint32_t)atoi(argv[3]) : 1920;
    uint32_t H = argc > 4 ? (uint32_t)atoi(argv[4]) : 1080;
    return render(argv[1], argv[2], W, H);
}
