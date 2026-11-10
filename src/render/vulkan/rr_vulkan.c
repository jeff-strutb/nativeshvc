/*
 * rr_vulkan.c: the renderer (rr.h) on Vulkan (Windows, Linux, Android). The
 * frame's textures are images, written through a staging buffer at the
 * frame's start; its words are a storage buffer and its uniforms one dynamic
 * uniform buffer; each pass is a render pass, its draws instanced draws with
 * no vertex buffers. Pipelines are made when first used. The programs are
 * SPIR-V (rr_spv.h). One frame is in flight at a time, so writing a buffer
 * never races the GPU. Vulkan's loader is found at run time.
 */
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_window.h"
#include "rr_spv.h"
#include "host.h"

void *host_vulkan_loader(void);

#define VK_FUNCS(X) \
    X(vkDestroyInstance) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceQueueFamilyProperties) \
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetBlendConstants) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDeviceWaitIdle) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
#define X(f) static PFN_##f f;
VK_FUNCS(X)
#undef X

typedef struct { const void *vs; size_t vs_size; const void *fs; size_t fs_size; } Program;
static const Program programs[RR_PROGRAMS] = { RR_PROGRAM_TABLE };

#define MAX_TEXTURES 16384
#define MAX_SHADERS 256
#define MAX_PIPES 4096
#define COLOUR VK_FORMAT_B8G8R8A8_UNORM

typedef struct { VkBuffer b; VkDeviceMemory m; void *at; VkDeviceSize size; } Buf;
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; VkFramebuffer f; int w, h, format; } Image;

/* what a pass draws into: the multisampled screens with depth and stencil,
   an integer texture, the output, the window */
enum { KIND_SCREEN, KIND_INDEX, KIND_COLOUR, KINDS };

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkFormat depth_format;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage sc_img[8];
static VkImageView sc_view[8];
static VkFramebuffer sc_fb[8];
static uint32_t sc_n;
static int sc_w, sc_h;          /* the window's size the swapchain was made for */
static int sc_ew, sc_eh;        /* the swapchain's own size */
static VkRenderPass rp_screen, rp_frame, rp_index, rp_output, rp_show;
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkSampler sampler;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore sem_acquire, sem_done;
static VkShaderModule vert_mod[RR_PROGRAMS], frag_mod[RR_PROGRAMS], game_mods[MAX_SHADERS];
static Image tex[MAX_TEXTURES];
static Image none_colour, none_index;
static Image screen_ms[3], screen_ds[3], extra_res[2], output;
static VkFramebuffer screen_fb[3];
static Buf ubuf, dbuf, staging, readback;
static uint32_t pipe_key[MAX_PIPES];
static VkPipeline pipe_obj[MAX_PIPES];
static size_t npipes;
static VkDescriptorPool pools[64];
static int npools, pool_at;
static int targets_w, targets_h, last_w, last_h, ever;

/* a call that must work, failed: said in the log, then the end */
static void fail(const char *what, VkResult r)
{
    fprintf(stderr, "rr: vulkan: %s failed (VkResult %d)\n", what, (int)r);
    exit(70);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    VkPhysicalDeviceMemoryProperties p;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & want) == want)
            return i;
    fprintf(stderr, "rr: vulkan: no suitable memory\n");
    exit(70);
}

static void buf_make(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    VkBufferCreateInfo ci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkResult e;
    VkMemoryRequirements r;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ci.size = size;
    ci.usage = usage;
    if ((e = vkCreateBuffer(dev, &ci, NULL, &b->b)) != VK_SUCCESS)
        fail("vkCreateBuffer", e);
    vkGetBufferMemoryRequirements(dev, b->b, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if ((e = vkAllocateMemory(dev, &ai, NULL, &b->m)) != VK_SUCCESS)
        fail("vkAllocateMemory (a buffer)", e);
    vkBindBufferMemory(dev, b->b, b->m, 0);
    vkMapMemory(dev, b->m, 0, VK_WHOLE_SIZE, 0, &b->at);
    b->size = size;
}

static void buf_free(Buf *b)
{
    if (b->b) {
        vkDestroyBuffer(dev, b->b, NULL);
        vkFreeMemory(dev, b->m, NULL);
    }
    memset(b, 0, sizeof *b);
}

/* b at least `size` bytes; 1 when it was made again */
static int buf_fit(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (b->size >= size)
        return 0;
    buf_free(b);
    buf_make(b, size * 2, usage);
    return 1;
}

static VkShaderModule shader(const void *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m;
    ci.codeSize = bytes;
    ci.pCode = (const uint32_t *)code;
    return vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static void image_free(Image *t)
{
    if (t->f)
        vkDestroyFramebuffer(dev, t->f, NULL);
    if (t->v)
        vkDestroyImageView(dev, t->v, NULL);
    if (t->i) {
        vkDestroyImage(dev, t->i, NULL);
        vkFreeMemory(dev, t->m, NULL);
    }
    memset(t, 0, sizeof *t);
}

static void image_make(Image *t, VkFormat f, int w, int h, int samples, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkMemoryRequirements r;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    VkResult e;
    image_free(t);
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent.width = (uint32_t)w;
    ci.extent.height = (uint32_t)h;
    ci.extent.depth = 1;
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if ((e = vkCreateImage(dev, &ci, NULL, &t->i)) != VK_SUCCESS)
        fail("vkCreateImage", e);
    vkGetImageMemoryRequirements(dev, t->i, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((e = vkAllocateMemory(dev, &ai, NULL, &t->m)) != VK_SUCCESS)
        fail("vkAllocateMemory (an image)", e);
    vkBindImageMemory(dev, t->i, t->m, 0);
    vi.image = t->i;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    if ((e = vkCreateImageView(dev, &vi, NULL, &t->v)) != VK_SUCCESS)
        fail("vkCreateImageView", e);
    t->w = w;
    t->h = h;
}

static VkFramebuffer framebuffer(VkRenderPass rp, const VkImageView *views, uint32_t n, int w, int h)
{
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkFramebuffer fb;
    VkResult e;
    fi.renderPass = rp;
    fi.attachmentCount = n;
    fi.pAttachments = views;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    if ((e = vkCreateFramebuffer(dev, &fi, NULL, &fb)) != VK_SUCCESS)
        fail("vkCreateFramebuffer", e);
    return fb;
}

static void barrier(VkImage i, VkImageLayout from, VkImageLayout to, VkAccessFlags src, VkAccessFlags dst, VkPipelineStageFlags ss,
                    VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

#define SHADER_STAGES (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)
#define ALL_WRITES (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
#define ALL_READS (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | \
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT)
#define ALL_STAGES (VK_PIPELINE_STAGE_TRANSFER_BIT | SHADER_STAGES | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | \
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)

/* a render pass of colour (samples), with depth and stencil and a resolve
   if `screen`; what came before it is done before it, and it before what follows */
static VkRenderPass render_pass(VkFormat f, int samples, int screen, VkAttachmentLoadOp load, VkImageLayout initial, VkImageLayout final)
{
    VkAttachmentDescription a[3];
    VkAttachmentReference cr = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL }, dr = { 1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL },
                          rr = { 2, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp;
    VkSubpassDependency deps[2];
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkRenderPass rp;
    memset(a, 0, sizeof a);
    memset(&sp, 0, sizeof sp);
    memset(deps, 0, sizeof deps);
    a[0].format = f;
    a[0].samples = (VkSampleCountFlagBits)samples;
    a[0].loadOp = load;
    a[0].storeOp = screen ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = screen ? VK_IMAGE_LAYOUT_UNDEFINED : initial;
    a[0].finalLayout = screen ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : final;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &cr;
    if (screen) {
        a[1].format = depth_format;
        a[1].samples = (VkSampleCountFlagBits)samples;
        a[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        a[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        a[2].format = f;
        a[2].samples = VK_SAMPLE_COUNT_1_BIT;
        a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        a[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        a[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        a[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        a[2].finalLayout = final;
        sp.pDepthStencilAttachment = &dr;
        sp.pResolveAttachments = &rr;
    }
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = ALL_STAGES;
    deps[0].dstStageMask = ALL_STAGES;
    deps[0].srcAccessMask = ALL_WRITES;
    deps[0].dstAccessMask = ALL_READS;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = ALL_STAGES;
    deps[1].dstStageMask = ALL_STAGES;
    deps[1].srcAccessMask = ALL_WRITES;
    deps[1].dstAccessMask = ALL_READS;
    ci.attachmentCount = screen ? 3 : 1;
    ci.pAttachments = a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    if (vkCreateRenderPass(dev, &ci, NULL, &rp) != VK_SUCCESS)
        fail("vkCreateRenderPass", VK_ERROR_INITIALIZATION_FAILED);
    return rp;
}

static void blend_of(VkPipelineColorBlendAttachmentState *a, int blend)
{
    memset(a, 0, sizeof *a);
    a->colorWriteMask = 0xF;
    a->blendEnable = blend != RR_BLEND_REPLACE && blend != RR_BLEND_NOCOLOUR;
    a->colorBlendOp = a->alphaBlendOp = VK_BLEND_OP_ADD;
    a->srcColorBlendFactor = a->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    a->dstColorBlendFactor = a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    switch (blend) {
    case RR_BLEND_NOCOLOUR:
        a->colorWriteMask = 0;
        break;
    case RR_BLEND_ADD_BY_A:
    case RR_BLEND_RSUB_BY_A:
        a->colorWriteMask = 7;
        a->srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        a->dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
        a->colorBlendOp = blend == RR_BLEND_ADD_BY_A ? VK_BLEND_OP_ADD : VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case RR_BLEND_MIX_BY_A:
    case RR_BLEND_RMIX_BY_A:
        a->colorWriteMask = 7;
        a->srcColorBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        a->dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        a->colorBlendOp = blend == RR_BLEND_MIX_BY_A ? VK_BLEND_OP_ADD : VK_BLEND_OP_REVERSE_SUBTRACT;
        break;
    case RR_BLEND_A_ZERO:
        a->colorWriteMask = 8;
        a->srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        break;
    case RR_BLEND_RGB_ZERO_A_ADD:
        a->srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        a->dstColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        a->srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case RR_BLEND_A_ADD:
        a->colorWriteMask = 8;
        a->srcAlphaBlendFactor = VK_BLEND_FACTOR_DST_ALPHA;
        a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        break;
    case RR_BLEND_RGB_SCALE:
        a->colorWriteMask = 7;
        a->srcColorBlendFactor = VK_BLEND_FACTOR_ZERO;
        a->dstColorBlendFactor = VK_BLEND_FACTOR_CONSTANT_COLOR;
        break;
    case RR_BLEND_OVER:
        a->dstColorBlendFactor = a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        break;
    default:
        break;
    }
}

static VkRenderPass pass_of_kind(int kind) { return kind == KIND_SCREEN ? rp_screen : kind == KIND_INDEX ? rp_index : rp_output; }

static VkPipeline pipe_of(int program, int fshader, int blend, int depth, int stencil, int topology, int kind)
{
    uint32_t key = (uint32_t)program | (uint32_t)fshader << 4 | (uint32_t)blend << 13 | (uint32_t)depth << 17 | (uint32_t)stencil << 19 |
                   (uint32_t)topology << 22 | (uint32_t)kind << 23;
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[6] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_BLEND_CONSTANTS,
                              VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkStencilOpState so;
    VkPipeline p;
    VkShaderModule fs = fshader ? game_mods[fshader] : frag_mod[program];
    size_t i;
    for (i = 0; i < npipes; i++)
        if (pipe_key[i] == key)
            return pipe_obj[i];
    if (!fs || !vert_mod[program])
        return VK_NULL_HANDLE;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vert_mod[program];
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    ia.topology = topology == RR_LINES ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    ms.rasterizationSamples = kind == KIND_SCREEN ? (VkSampleCountFlagBits)RR_SAMPLES : VK_SAMPLE_COUNT_1_BIT;
    memset(&so, 0, sizeof so);
    so.failOp = so.passOp = so.depthFailOp = VK_STENCIL_OP_KEEP;
    so.compareOp = VK_COMPARE_OP_EQUAL;
    if (stencil == RR_STENCIL_EQUAL_INC)
        so.passOp = so.depthFailOp = VK_STENCIL_OP_INCREMENT_AND_CLAMP;
    else if (stencil == RR_STENCIL_SET) {
        so.compareOp = VK_COMPARE_OP_ALWAYS;
        so.passOp = so.depthFailOp = VK_STENCIL_OP_REPLACE;
    }
    dsi.depthTestEnable = depth == RR_DEPTH_LEQUAL;
    dsi.depthWriteEnable = depth == RR_DEPTH_LEQUAL;
    dsi.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    dsi.stencilTestEnable = stencil != RR_STENCIL_OFF;
    dsi.front = dsi.back = so;
    dsi.maxDepthBounds = 1;
    blend_of(&ba, blend);
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    ds.dynamicStateCount = 6;
    ds.pDynamicStates = dyn;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = kind == KIND_SCREEN ? &dsi : NULL;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    ci.renderPass = pass_of_kind(kind);
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "rr: vulkan: pipeline %d could not be made\n", program);
        return VK_NULL_HANDLE;
    }
    if (npipes < MAX_PIPES) {
        pipe_key[npipes] = key;
        pipe_obj[npipes] = p;
        npipes++;
    }
    return p;
}

/* ---- the window ---- */
static int make_surface(void)
{
#if defined(_WIN32)
    typedef struct { VkStructureType s; const void *n; VkFlags f; void *hinst; void *hwnd; } Si;
    typedef VkResult (VKAPI_PTR *Fn)(VkInstance, const Si *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    extern void *host_win32_instance(void);
    Fn fn = (Fn)vkGetInstanceProcAddr(inst, "vkCreateWin32SurfaceKHR");
    Si si;
    memset(&si, 0, sizeof si);
    si.s = (VkStructureType)1000009000; /* VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR */
    si.hinst = host_win32_instance();
    si.hwnd = host_window_handle();
    return si.hwnd && fn && fn(inst, &si, NULL, &surface) == VK_SUCCESS;
#elif defined(__ANDROID__)
    typedef struct { VkStructureType s; const void *n; VkFlags f; void *win; } Si;
    typedef VkResult (VKAPI_PTR *Fn)(VkInstance, const Si *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    Fn fn = (Fn)vkGetInstanceProcAddr(inst, "vkCreateAndroidSurfaceKHR");
    Si si;
    memset(&si, 0, sizeof si);
    si.s = (VkStructureType)1000008000; /* VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR */
    si.win = host_window_handle();
    return si.win && fn && fn(inst, &si, NULL, &surface) == VK_SUCCESS;
#elif defined(__linux__) && !defined(__ANDROID__)
    typedef struct { VkStructureType s; const void *n; VkFlags f; void *dpy; unsigned long win; } Si;
    typedef VkResult (VKAPI_PTR *Fn)(VkInstance, const Si *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    extern void *host_x11_display(void);
    Fn fn = (Fn)vkGetInstanceProcAddr(inst, "vkCreateXlibSurfaceKHR");
    Si si;
    memset(&si, 0, sizeof si);
    si.s = (VkStructureType)1000004000; /* VK_STRUCTURE_TYPE_XLIB_SURFACE_CREATE_INFO_KHR */
    si.dpy = host_x11_display();
    si.win = (unsigned long)host_window_handle();
    return si.dpy && si.win && fn && fn(inst, &si, NULL, &surface) == VK_SUCCESS;
#else
    return 0;
#endif
}

/* the swapchain for a window of w x h pixels, at the size the surface allows
   (its current extent when it has one, else w x h within its limits): 0 when
   that size is nothing (minimised) */
static int swapchain_make(int w, int h)
{
    VkSurfaceCapabilitiesKHR c;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = swapchain;
    VkResult e;
    uint32_t i, n;
    int ask_w = w, ask_h = h;
    vkDeviceWaitIdle(dev);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &c);
    if (c.currentExtent.width != UINT32_MAX) {
        w = (int)c.currentExtent.width;
        h = (int)c.currentExtent.height;
    } else {
        if ((uint32_t)w < c.minImageExtent.width) w = (int)c.minImageExtent.width;
        if ((uint32_t)w > c.maxImageExtent.width) w = (int)c.maxImageExtent.width;
        if ((uint32_t)h < c.minImageExtent.height) h = (int)c.minImageExtent.height;
        if ((uint32_t)h > c.maxImageExtent.height) h = (int)c.maxImageExtent.height;
    }
    if (w < 1 || h < 1)
        return 0;
    n = c.minImageCount + 1;
    if (c.maxImageCount && n > c.maxImageCount)
        n = c.maxImageCount;
    if (n > 8)
        n = 8;
    ci.surface = surface;
    ci.minImageCount = n;
    ci.imageFormat = COLOUR;
    ci.imageColorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    ci.imageExtent.width = (uint32_t)w;
    ci.imageExtent.height = (uint32_t)h;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = c.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    if ((e = vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain)) != VK_SUCCESS)
        fail("vkCreateSwapchainKHR", e);
    for (i = 0; i < sc_n; i++) {
        vkDestroyFramebuffer(dev, sc_fb[i], NULL);
        vkDestroyImageView(dev, sc_view[i], NULL);
    }
    if (old)
        vkDestroySwapchainKHR(dev, old, NULL);
    sc_n = 8;
    vkGetSwapchainImagesKHR(dev, swapchain, &sc_n, sc_img);
    for (i = 0; i < sc_n; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = sc_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = COLOUR;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &vi, NULL, &sc_view[i]);
        sc_fb[i] = framebuffer(rp_show, &sc_view[i], 1, w, h);
    }
    sc_w = ask_w;
    sc_h = ask_h;
    sc_ew = w;
    sc_eh = h;
    return 1;
}

/* ---- opening ---- */
static int depth_ok(VkFormat f)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(phys, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

int rr_open(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[2] = { "VK_KHR_surface", NULL };
    int windowed = host_window_handle() != NULL, k;
    VkPhysicalDevice devs[16];
    uint32_t n = 16, i, d;
    float prio = 1;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo di = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    const char *dext = "VK_KHR_swapchain";

    if (!(vkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)host_vulkan_loader()))
        return 0;
    if (!(vkCreateInstance = (PFN_vkCreateInstance)vkGetInstanceProcAddr(NULL, "vkCreateInstance")))
        return 0;
#if defined(_WIN32)
    iext[1] = "VK_KHR_win32_surface";
#elif defined(__ANDROID__)
    iext[1] = "VK_KHR_android_surface";
#elif defined(__linux__) && !defined(__ANDROID__)
    iext[1] = "VK_KHR_xlib_surface";
#endif
    app.apiVersion = VK_API_VERSION_1_0;
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = windowed && iext[1] ? 2 : 0;
    ii.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS)
        return 0;
#define X(f) f = (PFN_##f)vkGetInstanceProcAddr(inst, #f);
    VK_FUNCS(X)
#undef X
    if (windowed && !make_surface())
        return 0;
    if (vkEnumeratePhysicalDevices(inst, &n, devs) < 0 || !n)
        return 0;
    phys = VK_NULL_HANDLE;
    for (d = 0; d < n && !phys; d++) {
        VkQueueFamilyProperties q[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[d], &nq, q);
        for (i = 0; i < nq; i++) {
            VkBool32 ok = VK_TRUE;
            if (surface)
                vkGetPhysicalDeviceSurfaceSupportKHR(devs[d], i, surface, &ok);
            if ((q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && ok) {
                phys = devs[d];
                family = i;
                break;
            }
        }
    }
    if (!phys)
        return 0;
    depth_format = depth_ok(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT
                 : depth_ok(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
    if (depth_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "rr: vulkan: no depth and stencil format\n");
        return 0;
    }
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = surface ? 1 : 0;
    di.ppEnabledExtensionNames = &dext;
    if (vkCreateDevice(phys, &di, NULL, &dev) != VK_SUCCESS)
        return 0;
    vkGetDeviceQueue(dev, family, 0, &queue);

    rp_screen = render_pass(COLOUR, RR_SAMPLES, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rp_frame = render_pass(COLOUR, RR_SAMPLES, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    rp_index = render_pass(VK_FORMAT_R8_UINT, 1, 0, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rp_output = render_pass(COLOUR, 1, 0, VK_ATTACHMENT_LOAD_OP_LOAD, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rp_show = render_pass(COLOUR, 1, 0, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_IMAGE_LAYOUT_UNDEFINED,
                          surface ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL);
    {
        VkDescriptorSetLayoutBinding b[2 + RR_TEX_SLOTS];
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        memset(b, 0, sizeof b);
        for (i = 0; i < 2 + RR_TEX_SLOTS; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            b[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC : i == 1 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
                                                                                               : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        li.bindingCount = 2 + RR_TEX_SLOTS;
        li.pBindings = b;
        if (vkCreateDescriptorSetLayout(dev, &li, NULL, &set_layout) != VK_SUCCESS)
            return 0;
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &set_layout;
        if (vkCreatePipelineLayout(dev, &pi, NULL, &layout) != VK_SUCCESS)
            return 0;
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &si, NULL, &sampler) != VK_SUCCESS)
            return 0;
    }
    for (k = 0; k < RR_PROGRAMS; k++) {
        vert_mod[k] = shader(programs[k].vs, programs[k].vs_size);
        frag_mod[k] = shader(programs[k].fs, programs[k].fs_size);
        if (!vert_mod[k] || !frag_mod[k])
            return 0;
    }
    {
        VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandPool pool;
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo se = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        vkCreateCommandPool(dev, &ci, NULL, &pool);
        ai.commandPool = pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        vkAllocateCommandBuffers(dev, &ai, &cmd);
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fi, NULL, &fence);
        vkCreateSemaphore(dev, &se, NULL, &sem_acquire);
        vkCreateSemaphore(dev, &se, NULL, &sem_done);
    }
    image_make(&none_colour, COLOUR, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    image_make(&none_index, VK_FORMAT_R8_UINT, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    return 1;
}

void rr_close(void)
{
    if (dev)
        vkDeviceWaitIdle(dev);
}

const char *rr_name(void) { return "vulkan"; }
const char *rr_shader_ext(void) { return "spv"; }

static VkFormat format_of(int f) { return f == RR_R8U ? VK_FORMAT_R8_UINT : f == RR_R32U ? VK_FORMAT_R32_UINT : COLOUR; }

/* ---- descriptor sets: one for each set of textures a frame binds ---- */
typedef struct { VkImageView v[RR_TEX_SLOTS]; VkDescriptorSet s; } SetEntry;
static SetEntry *sets;
static size_t nsets, cap_sets;

static VkDescriptorSet set_alloc(void)
{
    VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkDescriptorSet s;
    for (;;) {
        if (pool_at == npools) {
            VkDescriptorPoolSize ps[3] = { { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 256 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 256 },
                                           { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 256 * RR_TEX_SLOTS } };
            VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
            VkResult e;
            if (npools == 64)
                fail("vkCreateDescriptorPool (more than 64)", VK_ERROR_OUT_OF_POOL_MEMORY);
            dp.maxSets = 256;
            dp.poolSizeCount = 3;
            dp.pPoolSizes = ps;
            if ((e = vkCreateDescriptorPool(dev, &dp, NULL, &pools[npools])) != VK_SUCCESS)
                fail("vkCreateDescriptorPool", e);
            npools++;
        }
        ai.descriptorPool = pools[pool_at];
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &set_layout;
        if (vkAllocateDescriptorSets(dev, &ai, &s) == VK_SUCCESS)
            return s;
        pool_at++;
    }
}

static VkImageView slot_view(int32_t t, int slot)
{
    if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        return extra_res[t == RR_TEX_EXTRA0 ? 0 : 1].v;
    if (t > 0 && t < MAX_TEXTURES && tex[t].v)
        return tex[t].v;
    return slot < 2 ? none_colour.v : none_index.v;
}

static VkDescriptorSet set_of(const int32_t *t)
{
    VkImageView v[RR_TEX_SLOTS];
    VkDescriptorBufferInfo ub = { ubuf.b, 0, 256 }, db = { dbuf.b, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii[RR_TEX_SLOTS];
    VkWriteDescriptorSet w[2 + RR_TEX_SLOTS];
    SetEntry *e;
    size_t i;
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        v[k] = slot_view(t ? t[k] : 0, k);
    for (i = 0; i < nsets; i++)
        if (!memcmp(sets[i].v, v, sizeof v))
            return sets[i].s;
    if (nsets == cap_sets) {
        SetEntry *n = (SetEntry *)realloc(sets, (cap_sets ? cap_sets * 2 : 64) * sizeof *n);
        if (!n)
            exit(70);
        sets = n;
        cap_sets = cap_sets ? cap_sets * 2 : 64;
    }
    e = &sets[nsets++];
    memcpy(e->v, v, sizeof v);
    e->s = set_alloc();
    memset(w, 0, sizeof w);
    for (k = 0; k < 2 + RR_TEX_SLOTS; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = e->s;
        w[k].dstBinding = (uint32_t)k;
        w[k].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    w[0].pBufferInfo = &ub;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[1].pBufferInfo = &db;
    for (k = 0; k < RR_TEX_SLOTS; k++) {
        ii[k].sampler = sampler;
        ii[k].imageView = v[k];
        ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[2 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev, 2 + RR_TEX_SLOTS, w, 0, NULL);
    return e->s;
}

/* ---- a frame ---- */
static void targets_fit(int w, int h)
{
    int k;
    VkImageView v[3];
    if (targets_w == w && targets_h == h)
        return;
    vkDeviceWaitIdle(dev);
    for (k = 0; k < 3; k++) {
        if (screen_fb[k])
            vkDestroyFramebuffer(dev, screen_fb[k], NULL);
        image_make(&screen_ms[k], COLOUR, w, h, RR_SAMPLES, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
        image_make(&screen_ds[k], depth_format, w, h, RR_SAMPLES,
                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    }
    image_make(&output, COLOUR, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT);
    for (k = 0; k < 2; k++)
        image_make(&extra_res[k], COLOUR, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    for (k = 0; k < 3; k++) {
        v[0] = screen_ms[k].v;
        v[1] = screen_ds[k].v;
        v[2] = k == 0 ? output.v : extra_res[k - 1].v;
        screen_fb[k] = framebuffer(k == 0 ? rp_frame : rp_screen, v, 3, w, h);
    }
    output.f = framebuffer(rp_output, &output.v, 1, w, h);
    targets_w = w;
    targets_h = h;
}

static void apply_texops(const rr_frame *f)
{
    size_t i;
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        Image *t;
        if (o->id >= MAX_TEXTURES) {
            fprintf(stderr, "rr: vulkan: texture %u past the most\n", o->id);
            continue;
        }
        t = &tex[o->id];
        if (o->op == RR_TEX_CREATE) {
            VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            if (o->flags & RR_TEX_TARGET)
                u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            image_make(t, format_of(o->format), (int)o->w, (int)o->h, 1, u, VK_IMAGE_ASPECT_COLOR_BIT);
            t->format = o->format;
            if (o->flags & RR_TEX_TARGET)
                t->f = framebuffer(rp_index, &t->v, 1, (int)o->w, (int)o->h);
            /* readable from the first: unwritten texels are what they are */
            barrier(t->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SHADER_STAGES);
        } else if (t->i) {
            VkBufferImageCopy c;
            memset(&c, 0, sizeof c);
            c.bufferOffset = o->at;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageOffset.x = (int32_t)o->x;
            c.imageOffset.y = (int32_t)o->y;
            c.imageExtent.width = o->w;
            c.imageExtent.height = o->h;
            c.imageExtent.depth = 1;
            barrier(t->i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                    VK_ACCESS_TRANSFER_WRITE_BIT, SHADER_STAGES | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            vkCmdCopyBufferToImage(cmd, staging.b, t->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            barrier(t->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, SHADER_STAGES);
        }
    }
}

int rr_draw(rr_frame *f)
{
    int ow = f->width, oh = f->height, paced = 0, show = 0, vx, vy, vw, vh, k;
    float aspect = f->aspect;
    uint32_t image = 0;
    size_t i, p;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
    for (i = 0; i < f->nshaders; i++) {
        /* kept: its pipelines are made as they are needed */
        VkShaderModule m = shader(f->shaders[i].code, f->shaders[i].size);
        if (m && f->shaders[i].id < MAX_SHADERS)
            game_mods[f->shaders[i].id] = m;
        else
            fprintf(stderr, "rr: vulkan: shader %u could not be made\n", f->shaders[i].id);
    }

    if (surface) {
        int ww = 0, wh = 0;
        host_window_pixels(&ww, &wh);
        if (ww >= 1 && wh >= 1 && (ww != sc_w || wh != sc_h))
            swapchain_make(ww, wh);
        if (ww >= 1 && wh >= 1 && swapchain) {
            ow = sc_ew;
            oh = sc_eh;
            show = 1;
        } else if (targets_w) {
            /* minimised: drawn as before, not shown */
            ow = targets_w;
            oh = targets_h;
        }
    } else {
        const char *e = getenv("RR_SIZE");
        if (e)
            sscanf(e, "%dx%d", &ow, &oh);
        else
            aspect = 0;
    }
    if (ow < 1 || oh < 1)
        ow = oh = 1;
    targets_fit(ow, oh);
    rr_view(aspect, ow, oh, &vx, &vy, &vw, &vh);

    /* the frame's bytes: texels to upload, words, uniforms (each block given its target's size and samples) */
    buf_fit(&staging, f->upload_bytes + 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(staging.at, f->upload, f->upload_bytes);
    buf_fit(&dbuf, (f->data_words + 1) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(dbuf.at, f->data, f->data_words * 4);
    buf_fit(&ubuf, (f->nuniforms + 1) * 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        int tw = ow, th = oh, samples = ps->target <= RR_TARGET_EXTRA1 ? RR_SAMPLES : 1;
        if (ps->target == RR_TARGET_TEXTURE && ps->texture < MAX_TEXTURES) {
            tw = tex[ps->texture].w;
            th = tex[ps->texture].h;
        }
        for (i = ps->first; i < ps->first + ps->count; i++) {
            rr_uniform u = f->uniforms[f->cmds[i].uniform];
            u.f[0][0] = (float)tw;
            u.f[0][1] = (float)th;
            u.f[0][2] = (float)samples;
            u.f[0][3] = 1.0f;
            memcpy((char *)ubuf.at + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
        }
    }
    {
        /* the show pass's block, past the frame's */
        rr_uniform u;
        memset(&u, 0, sizeof u);
        u.f[0][0] = (float)ow;
        u.f[0][1] = (float)oh;
        u.f[0][2] = 1.0f;
        u.f[0][3] = 1.0f;
        memcpy((char *)ubuf.at + f->nuniforms * 256, &u, sizeof u);
    }
    nsets = 0;
    for (k = 0; k < npools; k++)
        vkResetDescriptorPool(dev, pools[k], 0);
    pool_at = 0;

    if (show) {
        VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR)) {
            sc_w = 0;
            show = 0;
        } else {
            paced = 1;
        }
    }

    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    if (!ever) {
        barrier(none_colour.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SHADER_STAGES);
        barrier(none_index.i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SHADER_STAGES);
    }
    apply_texops(f);
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue clr[2];
        int kind, tw = ow, th = oh;
        memset(clr, 0, sizeof clr);
        if (ps->target <= RR_TARGET_EXTRA1) {
            kind = KIND_SCREEN;
            bi.renderPass = ps->target == RR_TARGET_FRAME ? rp_frame : rp_screen;
            bi.framebuffer = screen_fb[ps->target];
            memcpy(clr[0].color.float32, ps->clear, sizeof ps->clear);
            clr[1].depthStencil.depth = 1;
            clr[1].depthStencil.stencil = 0;
            bi.clearValueCount = 2;
        } else if (ps->target == RR_TARGET_TEXTURE) {
            if (ps->texture >= MAX_TEXTURES || !tex[ps->texture].f)
                continue;
            kind = KIND_INDEX;
            tw = tex[ps->texture].w;
            th = tex[ps->texture].h;
            bi.renderPass = rp_index;
            bi.framebuffer = tex[ps->texture].f;
            bi.clearValueCount = 1;
        } else {
            kind = KIND_COLOUR;
            bi.renderPass = rp_output;
            bi.framebuffer = output.f;
        }
        bi.renderArea.extent.width = (uint32_t)tw;
        bi.renderArea.extent.height = (uint32_t)th;
        bi.pClearValues = clr;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        for (i = ps->first; i < ps->first + ps->count; i++) {
            const rr_cmd *c = &f->cmds[i];
            VkPipeline pl = pipe_of(c->program, c->shader < MAX_SHADERS ? c->shader : 0, c->blend, kind == KIND_SCREEN ? c->depth : 0,
                                    kind == KIND_SCREEN ? c->stencil : 0, c->topology, kind);
            VkViewport vp;
            VkRect2D sc;
            VkDescriptorSet set;
            uint32_t off = c->uniform * 256;
            float bc[4];
            if (!pl || !c->vertices || !c->instances)
                continue;
            if (c->space == RR_SPACE_VIEW && kind != KIND_INDEX) {
                vp.x = (float)vx;
                vp.y = (float)vy;
                vp.width = (float)vw;
                vp.height = (float)vh;
            } else {
                vp.x = vp.y = 0;
                vp.width = (float)tw;
                vp.height = (float)th;
            }
            vp.minDepth = 0;
            vp.maxDepth = 1;
            sc.offset.x = sc.offset.y = 0;
            sc.extent.width = (uint32_t)tw;
            sc.extent.height = (uint32_t)th;
            if (c->scissor) {
                int x0, y0, x1, y1;
                rr_scissor(f->uniforms[c->uniform].f[2], f->width, f->height, (int)vp.x, (int)vp.y, (int)vp.width, (int)vp.height, &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                sc.offset.x = x0;
                sc.offset.y = y0;
                sc.extent.width = (uint32_t)(x1 - x0);
                sc.extent.height = (uint32_t)(y1 - y0);
            }
            set = set_of(c->tex);
            bc[0] = bc[1] = bc[2] = bc[3] = c->constant;
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &off);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdSetBlendConstants(cmd, bc);
            vkCmdSetStencilReference(cmd, VK_STENCIL_FRONT_AND_BACK, c->ref);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FRONT_AND_BACK, c->read_mask);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FRONT_AND_BACK, c->write_mask);
            vkCmdDraw(cmd, c->vertices, c->instances, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
    }
    if (show) {
        /* the output, shown */
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkPipeline pl = pipe_of(RR_SHOW, 0, RR_BLEND_REPLACE, 0, 0, RR_TRIANGLES, KIND_COLOUR);
        VkViewport vp = { 0, 0, (float)ow, (float)oh, 0, 1 };
        VkRect2D sc = { { 0, 0 }, { (uint32_t)ow, (uint32_t)oh } };
        int32_t t[RR_TEX_SLOTS] = { 0 };
        VkDescriptorSet set;
        uint32_t off = (uint32_t)f->nuniforms * 256;
        bi.renderPass = rp_show;
        bi.framebuffer = sc_fb[image];
        bi.renderArea = sc;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        if (pl) {
            set = set_alloc();
            {
                VkDescriptorBufferInfo ub = { ubuf.b, 0, 256 }, db = { dbuf.b, 0, VK_WHOLE_SIZE };
                VkDescriptorImageInfo ii[RR_TEX_SLOTS];
                VkWriteDescriptorSet w[2 + RR_TEX_SLOTS];
                memset(w, 0, sizeof w);
                for (k = 0; k < 2 + RR_TEX_SLOTS; k++) {
                    w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    w[k].dstSet = set;
                    w[k].dstBinding = (uint32_t)k;
                    w[k].descriptorCount = 1;
                }
                w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
                w[0].pBufferInfo = &ub;
                w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
                w[1].pBufferInfo = &db;
                for (k = 0; k < RR_TEX_SLOTS; k++) {
                    ii[k].sampler = sampler;
                    ii[k].imageView = k == 0 ? output.v : slot_view(t[k], k);
                    ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                    w[2 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                    w[2 + k].pImageInfo = &ii[k];
                }
                vkUpdateDescriptorSets(dev, 2 + RR_TEX_SLOTS, w, 0, NULL);
            }
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &off);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdDraw(cmd, 6, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
    }
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (show) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_acquire;
        si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem_done;
    }
    vkQueueSubmit(queue, 1, &si, fence);
    if (show) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult r;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sem_done;
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR)
            sc_w = 0;
    }
    ever = 1;
    last_w = ow;
    last_h = oh;
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!ever)
        return 0;
    if (argb) {
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkBufferImageCopy c;
        VkDeviceSize bytes = (VkDeviceSize)last_w * (VkDeviceSize)last_h * 4;
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        if (readback.size < bytes) {
            buf_free(&readback);
            buf_make(&readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = (uint32_t)last_w;
        c.imageExtent.height = (uint32_t)last_h;
        c.imageExtent.depth = 1;
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        barrier(output.i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, ALL_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, output.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &c);
        barrier(output.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_READ_BIT,
                VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, ALL_STAGES);
        vkEndCommandBuffer(cmd);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        vkQueueSubmit(queue, 1, &si, fence);
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        /* BGRA bytes are 0xAARRGGBB words */
        memcpy(argb, readback.at, (size_t)bytes);
    }
    return 1;
}
