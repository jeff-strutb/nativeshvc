/*
 * rr_vulkan.c: the renderer backend on Vulkan (Windows, Linux, and macOS
 * through MoltenVK), the Boss Rally port's design: the frame is drawn into
 * an offscreen target the size of the window, then blitted onto a
 * swapchain image and presented. Pictures go on with the sharp bilinear
 * shader (shaders/sharp.frag, rr_size.h's weight).
 *
 * Plain Vulkan 1.0, one frame in flight. A picture is copied from a staging
 * ring into an image of its own size and drawn in a pass of its own (a copy
 * cannot run inside a pass); the frame's first pass clears the target.
 *
 * The window surface is the one OS-specific part: a CAMetalLayer on macOS,
 * an HWND on Windows. Without one (headless) frames are drawn and read
 * back, nothing is shown.
 */
#if defined(__APPLE__)
#define VK_USE_PLATFORM_METAL_EXT
#endif
#include <vulkan/vulkan.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_size.h"
#include "rr_mode7.h"
#include "rr_spv.h"
#include "host.h"

/* the host's window (a CAMetalLayer on macOS, an HWND on Windows) and its
   size in pixels; without one, headless */
void *host_window_handle(void);
void host_window_pixels(int *w, int *h);
#if !defined(_WIN32)          /* (the Windows hosts give both) */
__attribute__((weak)) void *host_window_handle(void) { return NULL; }
__attribute__((weak)) void host_window_pixels(int *w, int *h) { *w = *h = 0; }
#endif

#if defined(_WIN32)
/* the Windows host's module; the surface's create-info spelled here, field
   for field VkWin32SurfaceCreateInfoKHR, so this file needs no windows.h */
void *host_win32_instance(void);
typedef struct rr_win32_surface_info {
    VkStructureType sType;
    const void     *pNext;
    VkFlags         flags;
    void           *hinstance;
    void           *hwnd;
} rr_win32_surface_info;
typedef VkResult (VKAPI_PTR *rr_create_win32_surface)(VkInstance, const rr_win32_surface_info *,
                                                     const VkAllocationCallbacks *, VkSurfaceKHR *);
#define RR_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR ((VkStructureType)1000009000)
#define RR_KHR_WIN32_SURFACE_EXTENSION_NAME "VK_KHR_win32_surface"
#endif

#define FMT VK_FORMAT_B8G8R8A8_UNORM      /* 0xAARRGGBB little-endian, as the pictures come */
#define RING_SIZE (32u << 20)          /* the ring at first; it grows to a frame's need */

static VkInstance s_inst;
static VkPhysicalDevice s_phys;
static VkDevice s_dev;
static VkQueue s_queue;
static uint32_t s_qfam;
static VkPhysicalDeviceMemoryProperties s_memprops;
static VkCommandPool s_pool;
static VkCommandBuffer s_cmd;
static VkFence s_fence;
static int s_recording, s_submitted, s_cleared;
static uint32_t s_clear_argb;

static VkImage s_col;
static VkDeviceMemory s_col_mem;
static VkImageView s_col_view;
static VkFramebuffer s_fb;
static VkRenderPass s_pass_clear, s_pass_load;
static int s_rw, s_rh;

static VkImage s_pic;
static VkDeviceMemory s_pic_mem;
static VkImageView s_pic_view;
static int s_pw, s_ph, s_pic_fresh;

static VkDescriptorSetLayout s_lay;
static VkPipelineLayout s_playout;
static VkDescriptorPool s_dpool;
static VkDescriptorSet s_set;
static VkSampler s_smp;
static VkPipeline s_copy, s_sharp, s_over;
static int s_pipe_blend;                    /* make_pipe: the next pipeline blends */
/* the overlays: an image and a set for each of a frame's */
static VkImage s_oimg[RR_OVERLAYS];
static VkDeviceMemory s_oimg_mem[RR_OVERLAYS];
static VkImageView s_oimg_view[RR_OVERLAYS];
static VkDescriptorSet s_oset[RR_OVERLAYS];
static int s_ow[RR_OVERLAYS], s_oh[RR_OVERLAYS], s_on;

/* a frame with Mode 7 presented (rr_mode7.h, shaders/m7.frag): its data
   in the ring, read as a storage buffer, the planes in an image array */
static VkDescriptorSetLayout s_m7lay;
static VkPipelineLayout s_m7playout;
static VkDescriptorPool s_m7pool;
static VkDescriptorSet s_m7set;
static VkPipeline s_m7;
static VkSampler s_ismp;                    /* integer images: nearest */
static VkImage s_plane;
static VkDeviceMemory s_plane_mem;
static VkImageView s_plane_view;
static int s_plane_layers, s_planes, s_plane_fresh;
static uint32_t s_plane_gen;

static VkBuffer s_ring;                /* staging pictures and quads */
static VkDeviceMemory s_ring_mem;
static uint8_t *s_ring_map;
static VkDeviceSize s_ring_used;
static VkDeviceSize s_ring_size = RING_SIZE;
static VkBuffer s_read;                /* the frame read back */
static VkDeviceMemory s_read_mem;
static uint8_t *s_read_map;
static VkDeviceSize s_read_size;

static VkSurfaceKHR s_surface;
static VkSwapchainKHR s_swap;
static VkImage *s_swap_img;
static uint32_t s_nswap, s_swap_w, s_swap_h;
static VkSemaphore s_sem_acquire, *s_sem_done;

#define VK_OK(x) vk_check((x), #x)

static int vk_check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS)
        fprintf(stderr, "rr: vulkan %s: %d\n", what, (int)r);
    return r == VK_SUCCESS;
}

/* ---- memory ---------------------------------------------------------------------------- */
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    uint32_t i;
    for (i = 0; i < s_memprops.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (s_memprops.memoryTypes[i].propertyFlags & want) == want)
            return i;
    return UINT32_MAX;
}

static int alloc_bind_image(VkImage img, VkDeviceMemory *mem)
{
    VkMemoryRequirements rq;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    vkGetImageMemoryRequirements(s_dev, img, &rq);
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !VK_OK(vkAllocateMemory(s_dev, &ai, NULL, mem)))
        return 0;
    return VK_OK(vkBindImageMemory(s_dev, img, *mem, 0));
}

static int make_buffer(VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer *buf, VkDeviceMemory *mem, uint8_t **map)
{
    VkBufferCreateInfo bi = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
    VkMemoryRequirements rq;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    void *m = NULL;
    bi.size = size;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (!VK_OK(vkCreateBuffer(s_dev, &bi, NULL, buf)))
        return 0;
    vkGetBufferMemoryRequirements(s_dev, *buf, &rq);
    ai.allocationSize = rq.size;
    ai.memoryTypeIndex = mem_type(rq.memoryTypeBits,
                                  VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (ai.memoryTypeIndex == UINT32_MAX || !VK_OK(vkAllocateMemory(s_dev, &ai, NULL, mem)))
        return 0;
    vkBindBufferMemory(s_dev, *buf, *mem, 0);
    if (!VK_OK(vkMapMemory(s_dev, *mem, 0, size, 0, &m)))
        return 0;
    *map = (uint8_t *)m;
    return 1;
}

static long ring_take(VkDeviceSize n)
{
    VkDeviceSize o = (s_ring_used + 255) & ~(VkDeviceSize)255;
    if (o + n > s_ring_size)
        return -1;
    s_ring_used = o + n;
    return (long)o;
}

static void barrier(VkCommandBuffer c, VkImage img, VkImageLayout from, VkImageLayout to, VkAccessFlags src_acc,
                    VkAccessFlags dst_acc, VkPipelineStageFlags src_st, VkPipelineStageFlags dst_st)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.oldLayout = from;
    b.newLayout = to;
    b.srcAccessMask = src_acc;
    b.dstAccessMask = dst_acc;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = 1;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(c, src_st, dst_st, 0, 0, NULL, 0, NULL, 1, &b);
}

static int image_fmt(int w, int h, VkFormat fmt, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem,
                     VkImageView *view)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent.width = (uint32_t)w;
    ii.extent.height = (uint32_t)h;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!VK_OK(vkCreateImage(s_dev, &ii, NULL, img)) || !alloc_bind_image(*img, mem))
        return 0;
    vi.image = *img;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    return VK_OK(vkCreateImageView(s_dev, &vi, NULL, view));
}

static int image(int w, int h, VkImageUsageFlags usage, VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    return image_fmt(w, h, FMT, usage, img, mem, view);
}

static void image_destroy(VkImage *img, VkDeviceMemory *mem, VkImageView *view)
{
    if (*view) vkDestroyImageView(s_dev, *view, NULL);
    if (*img) vkDestroyImage(s_dev, *img, NULL);
    if (*mem) vkFreeMemory(s_dev, *mem, NULL);
    *view = VK_NULL_HANDLE;
    *img = VK_NULL_HANDLE;
    *mem = VK_NULL_HANDLE;
}

/* ---- start-up --------------------------------------------------------------------------- */
static int has_ext(const VkExtensionProperties *e, uint32_t n, const char *name)
{
    uint32_t i;
    for (i = 0; i < n; i++)
        if (!strcmp(e[i].extensionName, name))
            return 1;
    return 0;
}

static int make_instance(int window)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ci = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    VkExtensionProperties ext[64];
    uint32_t n = 64, nw = 0;
    const char *want[5];
    vkEnumerateInstanceExtensionProperties(NULL, &n, ext);
#if defined(__APPLE__)
    if (window && has_ext(ext, n, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(ext, n, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) {
        want[nw++] = VK_KHR_SURFACE_EXTENSION_NAME;
        want[nw++] = VK_EXT_METAL_SURFACE_EXTENSION_NAME;
    }
#elif defined(_WIN32)
    if (window && has_ext(ext, n, VK_KHR_SURFACE_EXTENSION_NAME) && has_ext(ext, n, RR_KHR_WIN32_SURFACE_EXTENSION_NAME)) {
        want[nw++] = VK_KHR_SURFACE_EXTENSION_NAME;
        want[nw++] = RR_KHR_WIN32_SURFACE_EXTENSION_NAME;
    }
#else
    (void)window;
#endif
    /* a layered implementation (MoltenVK) is listed only when asked for */
    if (has_ext(ext, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        want[nw++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        ci.flags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
        if (has_ext(ext, n, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME))
            want[nw++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    }
    app.pApplicationName = "nativeshvc";
    app.apiVersion = VK_API_VERSION_1_0;
    ci.pApplicationInfo = &app;
    ci.enabledExtensionCount = nw;
    ci.ppEnabledExtensionNames = want;
    return VK_OK(vkCreateInstance(&ci, NULL, &s_inst));
}

static int surface_make(void)
{
#if defined(__APPLE__)
    VkMetalSurfaceCreateInfoEXT si = { VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT };
    si.pLayer = host_window_handle();           /* the view's CAMetalLayer */
    return si.pLayer && vkCreateMetalSurfaceEXT(s_inst, &si, NULL, &s_surface) == VK_SUCCESS;
#elif defined(_WIN32)
    rr_win32_surface_info si;
    rr_create_win32_surface fn = (rr_create_win32_surface)vkGetInstanceProcAddr(s_inst, "vkCreateWin32SurfaceKHR");
    memset(&si, 0, sizeof si);
    si.sType = RR_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    si.hinstance = host_win32_instance();
    si.hwnd = host_window_handle();
    return fn && si.hwnd && fn(s_inst, &si, NULL, &s_surface) == VK_SUCCESS;
#else
    return 0;
#endif
}

static int make_device(void)
{
    VkPhysicalDevice devs[8];
    uint32_t nd = 8, i, j, ne = 256, nw = 0;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO };
    VkDeviceCreateInfo ci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO };
    VkExtensionProperties ext[256];
    const char *want[2];
    if (!VK_OK(vkEnumeratePhysicalDevices(s_inst, &nd, devs)) || nd == 0)
        return 0;
    s_phys = VK_NULL_HANDLE;
    for (i = 0; i < nd && !s_phys; i++) {
        VkQueueFamilyProperties qf[16];
        uint32_t nq = 16;
        vkGetPhysicalDeviceQueueFamilyProperties(devs[i], &nq, qf);
        for (j = 0; j < nq; j++)
            if (qf[j].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                s_phys = devs[i];
                s_qfam = j;
                break;
            }
    }
    if (!s_phys)
        return 0;
    vkGetPhysicalDeviceMemoryProperties(s_phys, &s_memprops);
    vkEnumerateDeviceExtensionProperties(s_phys, NULL, &ne, ext);
    if (has_ext(ext, ne, VK_KHR_SWAPCHAIN_EXTENSION_NAME))
        want[nw++] = VK_KHR_SWAPCHAIN_EXTENSION_NAME;
    if (has_ext(ext, ne, "VK_KHR_portability_subset"))
        want[nw++] = "VK_KHR_portability_subset";
    qi.queueFamilyIndex = s_qfam;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    ci.queueCreateInfoCount = 1;
    ci.pQueueCreateInfos = &qi;
    ci.enabledExtensionCount = nw;
    ci.ppEnabledExtensionNames = want;
    if (!VK_OK(vkCreateDevice(s_phys, &ci, NULL, &s_dev)))
        return 0;
    vkGetDeviceQueue(s_dev, s_qfam, 0, &s_queue);
    return 1;
}

/* a pass over one FMT attachment: clear = 1 clears it, 0 loads it, 2
   leaves what was there undefined (every pixel is drawn) */
static VkRenderPass make_pass_fmt(VkFormat fmt, int clear)
{
    VkAttachmentDescription at;
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp;
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkRenderPass p = VK_NULL_HANDLE;
    memset(&at, 0, sizeof at);
    at.format = fmt;
    at.samples = VK_SAMPLE_COUNT_1_BIT;
    at.loadOp = clear == 1 ? VK_ATTACHMENT_LOAD_OP_CLEAR : clear ? VK_ATTACHMENT_LOAD_OP_DONT_CARE : VK_ATTACHMENT_LOAD_OP_LOAD;
    at.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    at.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    at.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    at.initialLayout = clear ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    at.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    memset(&sp, 0, sizeof sp);
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    ci.attachmentCount = 1;
    ci.pAttachments = &at;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    VK_OK(vkCreateRenderPass(s_dev, &ci, NULL, &p));
    return p;
}

static VkRenderPass make_pass(int clear)
{
    return make_pass_fmt(FMT, clear);
}

static void targets(int w, int h)
{
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    if (s_fb)
        vkDestroyFramebuffer(s_dev, s_fb, NULL);
    image_destroy(&s_col, &s_col_mem, &s_col_view);
    if (s_read) {
        vkDestroyBuffer(s_dev, s_read, NULL);
        vkFreeMemory(s_dev, s_read_mem, NULL);
        s_read = VK_NULL_HANDLE;
    }
    image(w, h, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, &s_col, &s_col_mem, &s_col_view);
    fi.renderPass = s_pass_clear;
    fi.attachmentCount = 1;
    fi.pAttachments = &s_col_view;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    VK_OK(vkCreateFramebuffer(s_dev, &fi, NULL, &s_fb));
    s_read_size = (VkDeviceSize)w * (VkDeviceSize)h * 4;
    make_buffer(s_read_size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &s_read, &s_read_mem, &s_read_map);
    s_rw = w;
    s_rh = h;
}

static VkShaderModule module(const uint32_t *code, size_t size)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m = VK_NULL_HANDLE;
    ci.codeSize = size;
    ci.pCode = code;
    VK_OK(vkCreateShaderModule(s_dev, &ci, NULL, &m));
    return m;
}

/* a pipeline: quads from the ring (quad = 1, pictures onto the target),
   one triangle with no vertex input (quad = 0, the compose pass), spans
   as instances (quad = 2, two uint vec4s each: the 3D canvas), or runs as
   instances (quad = 3, a uint vec4 each: the pack) */
static VkPipeline make_pipe(VkShaderModule vs, VkShaderModule fs, int quad, VkPipelineLayout layout, VkRenderPass pass)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkVertexInputBindingDescription vb = { 0, 16, VK_VERTEX_INPUT_RATE_VERTEX };
    VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 },
                                                { 1, 0, VK_FORMAT_R32G32B32A32_UINT, 16 } };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p = VK_NULL_HANDLE;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    vi.vertexBindingDescriptionCount = quad ? 1 : 0;
    vi.pVertexBindingDescriptions = &vb;
    vi.vertexAttributeDescriptionCount = quad == 2 ? 2 : quad ? 1 : 0;
    vi.pVertexAttributeDescriptions = va;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    memset(&ba, 0, sizeof ba);
    if (s_pipe_blend) {
        ba.blendEnable = VK_TRUE;
        ba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
        ba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        ba.colorBlendOp = VK_BLEND_OP_ADD;
        ba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
        ba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
        ba.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    ba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    ci.renderPass = pass;
    VK_OK(vkCreateGraphicsPipelines(s_dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}

static int make_layout(void)
{
    VkDescriptorSetLayoutBinding b = { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkPushConstantRange spc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 4 * 4 };   /* sharp bilinear's (sharp.frag SU) */
    VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 + RR_OVERLAYS };
    VkDescriptorSetLayout two[2];
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    li.bindingCount = 1;
    li.pBindings = &b;
    if (!VK_OK(vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_lay)))
        return 0;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &s_lay;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &spc;
    if (!VK_OK(vkCreatePipelineLayout(s_dev, &pi, NULL, &s_playout)))
        return 0;
    dpi.maxSets = 1 + RR_OVERLAYS;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &ps;
    if (!VK_OK(vkCreateDescriptorPool(s_dev, &dpi, NULL, &s_dpool)))
        return 0;
    two[0] = two[1] = s_lay;
    da.descriptorPool = s_dpool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = two;
    if (!VK_OK(vkAllocateDescriptorSets(s_dev, &da, &s_set)))
        return 0;
    {
        int k;
        for (k = 0; k < RR_OVERLAYS; k++)
            if (!VK_OK(vkAllocateDescriptorSets(s_dev, &da, &s_oset[k])))
                return 0;
    }
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    si.maxLod = 0.0f;
    return VK_OK(vkCreateSampler(s_dev, &si, NULL, &s_smp));
}

static void pass_begin(void);

/* the Mode 7 pipeline: set 0 the ring (binding 0) and the planes
   (binding 1), the push constants the presentation and the data's word
   offsets in the ring */
/* the Mode 7 pass's storage buffer: the ring, as far as the device lets
   one binding reach */
static void ring_bind(void)
{
    VkDescriptorBufferInfo bi;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    VkPhysicalDeviceProperties pp;
    vkGetPhysicalDeviceProperties(s_phys, &pp);
    bi.buffer = s_ring;
    bi.offset = 0;
    bi.range = s_ring_size < pp.limits.maxStorageBufferRange ? s_ring_size : pp.limits.maxStorageBufferRange;
    wr.dstSet = s_m7set;
    wr.dstBinding = 0;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &bi;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
}

static void wait_frame(void);

/* room in the ring for n more bytes this frame: when there is not, what
   the frame recorded so far is run first (it reads the ring), and the ring
   made again large enough. 0 when it cannot be. */
static int ring_fit(VkDeviceSize n)
{
    VkDeviceSize size = s_ring_size;
    if (((s_ring_used + 255) & ~(VkDeviceSize)255) + n <= s_ring_size)
        return 1;
    if (s_ring_used) {
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        vkEndCommandBuffer(s_cmd);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &s_cmd;
        vkQueueSubmit(s_queue, 1, &si, s_fence);
        s_submitted = 1;
        wait_frame();
        vkResetFences(s_dev, 1, &s_fence);
        vkResetCommandBuffer(s_cmd, 0);
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(s_cmd, &bi);
        s_ring_used = 0;
    }
    if (n <= s_ring_size)
        return 1;
    wait_frame();
    vkDeviceWaitIdle(s_dev);
    while (size < n)
        size *= 2;
    vkUnmapMemory(s_dev, s_ring_mem);
    vkDestroyBuffer(s_dev, s_ring, NULL);
    vkFreeMemory(s_dev, s_ring_mem, NULL);
    s_ring = VK_NULL_HANDLE;
    s_ring_mem = VK_NULL_HANDLE;
    s_ring_size = size;
    if (!make_buffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                               VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     &s_ring, &s_ring_mem, &s_ring_map)) {
        fprintf(stderr, "vulkan: no staging buffer of %llu MB\n", (unsigned long long)(size >> 20));
        s_ring_size = 0;
        return 0;
    }
    ring_bind();
    return 1;
}

static int m7_make(VkShaderModule vs)
{
    VkDescriptorSetLayoutBinding b[2] = { { 0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL },
                                          { 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
    VkPushConstantRange pc = { VK_SHADER_STAGE_FRAGMENT_BIT, 0, 19 * 4 };
    VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
    VkDescriptorSetAllocateInfo da = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
    VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    li.bindingCount = 2;
    li.pBindings = b;
    if (!VK_OK(vkCreateDescriptorSetLayout(s_dev, &li, NULL, &s_m7lay)))
        return 0;
    pi.setLayoutCount = 1;
    pi.pSetLayouts = &s_m7lay;
    pi.pushConstantRangeCount = 1;
    pi.pPushConstantRanges = &pc;
    if (!VK_OK(vkCreatePipelineLayout(s_dev, &pi, NULL, &s_m7playout)))
        return 0;
    dpi.maxSets = 1;
    dpi.poolSizeCount = 2;
    dpi.pPoolSizes = ps;
    if (!VK_OK(vkCreateDescriptorPool(s_dev, &dpi, NULL, &s_m7pool)))
        return 0;
    da.descriptorPool = s_m7pool;
    da.descriptorSetCount = 1;
    da.pSetLayouts = &s_m7lay;
    if (!VK_OK(vkAllocateDescriptorSets(s_dev, &da, &s_m7set)))
        return 0;
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!VK_OK(vkCreateSampler(s_dev, &si, NULL, &s_ismp)))
        return 0;
    ring_bind();
    s_m7 = make_pipe(vs, module(k_m7_frag, sizeof k_m7_frag), 1, s_m7playout, s_pass_load);
    return s_m7 != VK_NULL_HANDLE;
}

/* the planes' image, layers deep, made again when it must hold more */
static int m7_plane_image(int layers)
{
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkDescriptorImageInfo di;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    if (s_plane && s_plane_layers >= layers)
        return 1;
    if (s_plane) {
        vkDeviceWaitIdle(s_dev);
        image_destroy(&s_plane, &s_plane_mem, &s_plane_view);
    }
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = VK_FORMAT_R8_UINT;
    ii.extent.width = ii.extent.height = 1024;
    ii.extent.depth = 1;
    ii.mipLevels = 1;
    ii.arrayLayers = (uint32_t)layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (!VK_OK(vkCreateImage(s_dev, &ii, NULL, &s_plane)) || !alloc_bind_image(s_plane, &s_plane_mem))
        return 0;
    vi.image = s_plane;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = VK_FORMAT_R8_UINT;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = (uint32_t)layers;
    if (!VK_OK(vkCreateImageView(s_dev, &vi, NULL, &s_plane_view)))
        return 0;
    di.sampler = s_ismp;
    di.imageView = s_plane_view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.dstSet = s_m7set;
    wr.dstBinding = 1;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    s_plane_layers = layers;
    s_plane_fresh = 1;
    s_planes = 0;
    return 1;
}

/* n bytes into the ring: their offset in words, or -1 */
static long ring_put(const void *p, size_t n)
{
    long o = ring_take(n ? n : 4);
    if (o >= 0 && n)
        memcpy(s_ring_map + o, p, n);
    return o < 0 ? -1 : o / 4;
}

void rr_mode7(const struct RrMode7 *m, const int d[4], const struct RrM7Samples *s)
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    int ow = d[2] - d[0], oh = d[3] - d[1], k;
    size_t np = (size_t)m->pw * (size_t)m->ph;
    int32_t pcv[19], uv[56];
    long off[9], qo;
    float c[4][4] = { { (float)d[0], (float)d[1], 0, 0 }, { (float)d[2], (float)d[1], 1, 0 },
                      { (float)d[0], (float)d[3], 0, 1 }, { (float)d[2], (float)d[3], 1, 1 } };
    float *q;
    VkDeviceSize vo;
    if (!s_recording || ow <= 0 || oh <= 0 || np == 0)
        return;
    if (!m7_plane_image(m->vrams ? m->vrams : 1))
        return;
    {
        /* everything this draw puts in the ring, each part aligned */
        VkDeviceSize need = (VkDeviceSize)m->vrams * 0x100000u + np * sizeof *m->px + (size_t)m->segs * sizeof *m->seg +
                            (size_t)m->pals * 512 + (size_t)m->vrams * 0x10000 + np * 4 + (size_t)ow * 12 +
                            (size_t)oh * 16 + sizeof uv + (m->cv ? m->cv_len : 0) + 6 * 16 + 12 * 256;
        if (!ring_fit(need))
            return;
    }
    if (s_planes < m->vrams || s_plane_gen != m->plane_gen) {
        /* the planes changed: copied in from the ring before this frame's
           draws read them */
        long po = ring_take((size_t)m->vrams * 0x100000u);
        VkBufferImageCopy cp;
        if (po < 0)
            return;
        memcpy(s_ring_map + po, m->plane, (size_t)m->vrams * 0x100000u);
        {
            VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
            b.oldLayout = s_plane_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            b.image = s_plane;
            b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            b.subresourceRange.levelCount = 1;
            b.subresourceRange.layerCount = (uint32_t)s_plane_layers;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &b);
            memset(&cp, 0, sizeof cp);
            cp.bufferOffset = (VkDeviceSize)po;
            cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            cp.imageSubresource.layerCount = (uint32_t)m->vrams;
            cp.imageExtent.width = cp.imageExtent.height = 1024;
            cp.imageExtent.depth = 1;
            if (m->vrams)
                vkCmdCopyBufferToImage(s_cmd, s_ring, s_plane, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(s_cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0,
                                 NULL, 1, &b);
        }
        s_plane_fresh = 0;
        s_plane_gen = m->plane_gen;
        s_planes = m->vrams;
    }
    /* the supersamples' offsets: 4 from 0, 8 from 4, 16 from 12; then dv */
    memcpy(uv, s->du4, sizeof s->du4);
    memcpy(uv + 4, s->du8, sizeof s->du8);
    memcpy(uv + 12, s->du16, sizeof s->du16);
    memcpy(uv + 28, s->dv4, sizeof s->dv4);
    memcpy(uv + 32, s->dv8, sizeof s->dv8);
    memcpy(uv + 40, s->dv16, sizeof s->dv16);
    off[0] = ring_put(m->px, np * sizeof *m->px);
    off[1] = ring_put(m->seg, (size_t)m->segs * sizeof *m->seg);
    off[2] = ring_put(m->pal, (size_t)m->pals * 512);
    off[3] = ring_put(m->vram, (size_t)m->vrams * 0x10000);
    off[4] = ring_put(m->pic, np * 4);
    off[5] = ring_put(s->cols, (size_t)ow * 3 * 4);
    off[6] = ring_put(s->rows, (size_t)oh * 4 * 4);
    off[7] = ring_put(uv, sizeof uv);
    {
        /* the canvas's texels, padded to whole words with 0 (a word where
           there is none) */
        size_t n = m->cv ? m->cv_len : 0, n4 = (n + 3) & ~(size_t)3;
        long o = ring_take(n4 ? n4 : 4);
        if (o >= 0) {
            memset(s_ring_map + o, 0, n4 ? n4 : 4);
            if (n)
                memcpy(s_ring_map + o, m->cv, n);
        }
        off[8] = o < 0 ? -1 : o / 4;
    }
    for (k = 0; k < 9; k++)
        if (off[k] < 0)
            return;
    if ((qo = ring_take(6 * 16)) < 0)
        return;
    q = (float *)(s_ring_map + qo);
    for (k = 0; k < 6; k++) {
        q[4 * k + 0] = c[ix[k]][0] / s_rw * 2 - 1;
        q[4 * k + 1] = 1 - c[ix[k]][1] / s_rh * 2;
        q[4 * k + 2] = c[ix[k]][2];
        q[4 * k + 3] = c[ix[k]][3];
    }
    pcv[0] = m->pw;
    pcv[1] = m->ph;
    pcv[2] = d[0];
    pcv[3] = d[1];
    pcv[4] = s->step_u;
    pcv[5] = s->step_v;
    for (k = 0; k < 8; k++)
        pcv[6 + k] = (int32_t)off[k];
    pcv[14] = m->cpp;
    pcv[15] = m->left;
    pcv[16] = m->cv_k;
    pcv[17] = (int32_t)m->cv_len;
    pcv[18] = (int32_t)off[8];
    pass_begin();
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_m7);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_m7playout, 0, 1, &s_m7set, 0, NULL);
    vkCmdPushConstants(s_cmd, s_m7playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof pcv, pcv);
    vo = (VkDeviceSize)qo;
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring, &vo);
    vkCmdDraw(s_cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(s_cmd);
}

int rr_open(void)
{
    VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
    VkShaderModule vs, cfs, sfs;
    int w = 0, h = 0, win = host_window_handle() != NULL;
    if (!make_instance(win))
        return 0;
    if (win && !surface_make())
        fprintf(stderr, "rr: vulkan: no window surface; drawing offscreen\n");
    if (!make_device())
        return 0;
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = s_qfam;
    if (!VK_OK(vkCreateCommandPool(s_dev, &pci, NULL, &s_pool)))
        return 0;
    cai.commandPool = s_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    if (!VK_OK(vkAllocateCommandBuffers(s_dev, &cai, &s_cmd)))
        return 0;
    if (!VK_OK(vkCreateFence(s_dev, &fci, NULL, &s_fence)) || !VK_OK(vkCreateSemaphore(s_dev, &sci, NULL, &s_sem_acquire)))
        return 0;
    s_pass_clear = make_pass(1);
    s_pass_load = make_pass(0);
    if (!s_pass_clear || !s_pass_load || !make_layout())
        return 0;
    vs = module(k_quad_vert, sizeof k_quad_vert);
    cfs = module(k_copy_frag, sizeof k_copy_frag);
    sfs = module(k_sharp_frag, sizeof k_sharp_frag);
    s_copy = make_pipe(vs, cfs, 1, s_playout, s_pass_load);
    s_sharp = make_pipe(vs, sfs, 1, s_playout, s_pass_load);
    s_pipe_blend = 1;
    s_over = make_pipe(vs, module(k_over_frag, sizeof k_over_frag), 1, s_playout, s_pass_load);
    s_pipe_blend = 0;
    if (!s_over)
        return 0;
    if (!s_copy || !s_sharp)
        return 0;
    if (!make_buffer(RING_SIZE, VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
                                    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     &s_ring, &s_ring_mem, &s_ring_map))
        return 0;
    if (!m7_make(vs))
        return 0;
    if (s_surface)
        host_window_pixels(&w, &h);
    if (w < 1 || h < 1)
        rr_headless_size(&w, &h);
    targets(w, h);
    return 1;
}

void rr_close(void)
{
    if (s_dev)
        vkDeviceWaitIdle(s_dev);
}

const char *rr_name(void) { return "vulkan"; }

static void wait_frame(void)
{
    if (s_submitted) {
        vkWaitForFences(s_dev, 1, &s_fence, VK_TRUE, UINT64_MAX);
        s_submitted = 0;
    }
}

void rr_target(int *w, int *h)
{
    if (s_surface && !s_recording) {
        int fw, fh;
        host_window_pixels(&fw, &fh);
        if (fw > 0 && fh > 0 && (fw != s_rw || fh != s_rh)) {
            wait_frame();
            targets(fw, fh);
        }
    }
    *w = s_rw;
    *h = s_rh;
}

void rr_begin(uint32_t argb)
{
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    int w, h;
    rr_target(&w, &h);
    wait_frame();
    vkResetFences(s_dev, 1, &s_fence);
    vkResetCommandBuffer(s_cmd, 0);
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(s_cmd, &bi);
    s_recording = 1;
    s_cleared = 0;
    s_clear_argb = argb;
    s_ring_used = 0;
    s_on = 0;
}

/* a pass over the target: the frame's first clears it */
static void pass_begin(void)
{
    VkRenderPassBeginInfo rp = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkClearValue cv;
    VkViewport v = { 0, 0, (float)s_rw, (float)s_rh, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)s_rw, (uint32_t)s_rh } };
    cv.color.float32[0] = ((s_clear_argb >> 16) & 0xFF) / 255.0f;
    cv.color.float32[1] = ((s_clear_argb >> 8) & 0xFF) / 255.0f;
    cv.color.float32[2] = (s_clear_argb & 0xFF) / 255.0f;
    cv.color.float32[3] = 1.0f;
    rp.renderPass = s_cleared ? s_pass_load : s_pass_clear;
    rp.framebuffer = s_fb;
    rp.renderArea = sc;
    rp.clearValueCount = 1;
    rp.pClearValues = &cv;
    vkCmdBeginRenderPass(s_cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(s_cmd, 0, 1, &v);
    vkCmdSetScissor(s_cmd, 0, 1, &sc);
    s_cleared = 1;
}

static void picture_image(int w, int h)
{
    VkDescriptorImageInfo di;
    VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
    if (w == s_pw && h == s_ph && s_pic)
        return;
    wait_frame();
    vkDeviceWaitIdle(s_dev);
    image_destroy(&s_pic, &s_pic_mem, &s_pic_view);
    image(w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &s_pic, &s_pic_mem, &s_pic_view);
    di.sampler = s_smp;
    di.imageView = s_pic_view;
    di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    wr.dstSet = s_set;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr.pImageInfo = &di;
    vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
    s_pw = w;
    s_ph = h;
    s_pic_fresh = 1;
}

/* the image of set (w x h) drawn over target rectangle d in a pass of its
   own: one to one when d is w x h, else sharp bilinear */
static void draw(VkDescriptorSet set, int w, int h, const int d[4])
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4] = { { (float)d[0], (float)d[1], 0, 0 }, { (float)d[2], (float)d[1], 1, 0 },
                      { (float)d[0], (float)d[3], 0, 1 }, { (float)d[2], (float)d[3], 1, 1 } };
    long qo;
    VkDeviceSize vo;
    float *q;
    int i;
    if ((qo = ring_take(6 * 16)) < 0)
        return;
    q = (float *)(s_ring_map + qo);
    for (i = 0; i < 6; i++) {
        q[4 * i + 0] = c[ix[i]][0] / s_rw * 2 - 1;
        q[4 * i + 1] = 1 - c[ix[i]][1] / s_rh * 2;
        q[4 * i + 2] = c[ix[i]][2];
        q[4 * i + 3] = c[ix[i]][3];
    }
    pass_begin();
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      d[2] - d[0] == w && d[3] - d[1] == h ? s_copy : s_sharp);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &set, 0, NULL);
    if (d[2] - d[0] != w || d[3] - d[1] != h) {
        /* where the target's pixels sample: its rectangle's corner, and
           texels a target pixel (rr_ref_image's dx, dy) */
        float su[4] = { (float)d[0], (float)d[1], (float)w / (float)(d[2] - d[0]), (float)h / (float)(d[3] - d[1]) };
        vkCmdPushConstants(s_cmd, s_playout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof su, su);
    }
    vo = (VkDeviceSize)qo;
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring, &vo);
    vkCmdDraw(s_cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(s_cmd);
}

void rr_image(const uint32_t *argb, int w, int h, const int d[4])
{
    size_t n = (size_t)w * (size_t)h * 4;
    long po;
    VkBufferImageCopy cp;
    if (!s_recording || w <= 0 || h <= 0 || d[2] <= d[0] || d[3] <= d[1])
        return;
    if (w != s_pw || h != s_ph) {
        /* a new picture size: the frame so far is finished first (its draws
           read the old image) */
        if (s_cleared) {
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            vkEndCommandBuffer(s_cmd);
            si.commandBufferCount = 1;
            si.pCommandBuffers = &s_cmd;
            vkQueueSubmit(s_queue, 1, &si, s_fence);
            s_submitted = 1;
            picture_image(w, h);
            vkResetFences(s_dev, 1, &s_fence);
            vkResetCommandBuffer(s_cmd, 0);
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(s_cmd, &bi);
            s_ring_used = 0;
        } else {
            picture_image(w, h);
        }
    }
    if (!ring_fit(n + 6 * 16 + 512) || (po = ring_take(n)) < 0)
        return;
    memcpy(s_ring_map + po, argb, n);
    barrier(s_cmd, s_pic, s_pic_fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    s_pic_fresh = 0;
    memset(&cp, 0, sizeof cp);
    cp.bufferOffset = (VkDeviceSize)po;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = (uint32_t)w;
    cp.imageExtent.height = (uint32_t)h;
    cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_ring, s_pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(s_cmd, s_pic, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    draw(s_set, w, h, d);
}

/* ---- the window ------------------------------------------------------------------------- */
void rr_overlay(const uint32_t *argb, int w, int h, const float q[6])
{
    static const int ix[6] = { 0, 1, 2, 2, 1, 3 };
    float c[4][4] = { { q[0], q[1], 0, 0 }, { q[2], q[3], 1, 0 }, { q[4], q[5], 0, 1 },
                      { q[2] + q[4] - q[0], q[3] + q[5] - q[1], 1, 1 } };
    size_t n = (size_t)w * (size_t)h * 4;
    int k = s_on, fresh = 0, i;
    long po, qo;
    VkBufferImageCopy cp;
    VkDeviceSize vo;
    float *v;
    if (!s_recording || w <= 0 || h <= 0 || k == RR_OVERLAYS)
        return;
    if (!s_oimg[k] || s_ow[k] != w || s_oh[k] != h) {
        VkDescriptorImageInfo di;
        VkWriteDescriptorSet wr = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
        if (s_oimg[k]) {
            vkDeviceWaitIdle(s_dev);
            image_destroy(&s_oimg[k], &s_oimg_mem[k], &s_oimg_view[k]);
        }
        if (!image(w, h, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, &s_oimg[k], &s_oimg_mem[k],
                   &s_oimg_view[k]))
            return;
        di.sampler = s_smp;
        di.imageView = s_oimg_view[k];
        di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        wr.dstSet = s_oset[k];
        wr.descriptorCount = 1;
        wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        wr.pImageInfo = &di;
        vkUpdateDescriptorSets(s_dev, 1, &wr, 0, NULL);
        s_ow[k] = w;
        s_oh[k] = h;
        fresh = 1;
    }
    if ((po = ring_take(n)) < 0 || (qo = ring_take(6 * 16)) < 0)
        return;
    s_on++;
    memcpy(s_ring_map + po, argb, n);
    barrier(s_cmd, s_oimg[k], fresh ? VK_IMAGE_LAYOUT_UNDEFINED : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    memset(&cp, 0, sizeof cp);
    cp.bufferOffset = (VkDeviceSize)po;
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = (uint32_t)w;
    cp.imageExtent.height = (uint32_t)h;
    cp.imageExtent.depth = 1;
    vkCmdCopyBufferToImage(s_cmd, s_ring, s_oimg[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &cp);
    barrier(s_cmd, s_oimg[k], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    v = (float *)(s_ring_map + qo);
    for (i = 0; i < 6; i++) {
        v[4 * i + 0] = c[ix[i]][0] / s_rw * 2 - 1;
        v[4 * i + 1] = 1 - c[ix[i]][1] / s_rh * 2;
        v[4 * i + 2] = c[ix[i]][2];
        v[4 * i + 3] = c[ix[i]][3];
    }
    pass_begin();
    vkCmdBindPipeline(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_over);
    vkCmdBindDescriptorSets(s_cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, s_playout, 0, 1, &s_oset[k], 0, NULL);
    vo = (VkDeviceSize)qo;
    vkCmdBindVertexBuffers(s_cmd, 0, 1, &s_ring, &vo);
    vkCmdDraw(s_cmd, 6, 1, 0, 0);
    vkCmdEndRenderPass(s_cmd);
}

static void swap_destroy(void)
{
    uint32_t i;
    if (s_swap) {
        vkDeviceWaitIdle(s_dev);
        vkDestroySwapchainKHR(s_dev, s_swap, NULL);
        s_swap = VK_NULL_HANDLE;
    }
    for (i = 0; s_sem_done && i < s_nswap; i++)
        vkDestroySemaphore(s_dev, s_sem_done[i], NULL);
    free(s_sem_done);
    s_sem_done = NULL;
    free(s_swap_img);
    s_swap_img = NULL;
    s_nswap = 0;
}

static int swap_make(void)
{
    VkSurfaceCapabilitiesKHR caps;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSurfaceFormatKHR fmts[32];
    uint32_t nf = 32, i;
    VkBool32 ok = VK_FALSE;
    vkGetPhysicalDeviceSurfaceSupportKHR(s_phys, s_qfam, s_surface, &ok);
    if (!ok || !VK_OK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(s_phys, s_surface, &caps)))
        return 0;
    if (caps.currentExtent.width == 0 || caps.currentExtent.height == 0)
        return 0;                                  /* minimised */
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(s_phys, s_surface, &nf, fmts);
    ci.imageFormat = nf ? fmts[0].format : FMT;
    ci.imageColorSpace = nf ? fmts[0].colorSpace : VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    for (i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM || fmts[i].format == VK_FORMAT_R8G8B8A8_UNORM) {
            ci.imageFormat = fmts[i].format;
            ci.imageColorSpace = fmts[i].colorSpace;
            break;
        }
    ci.surface = s_surface;
    ci.minImageCount = caps.minImageCount + 1;
    if (caps.maxImageCount && ci.minImageCount > caps.maxImageCount)
        ci.minImageCount = caps.maxImageCount;
    ci.imageExtent = caps.currentExtent;
    if (ci.imageExtent.width == UINT32_MAX) {
        ci.imageExtent.width = (uint32_t)s_rw;
        ci.imageExtent.height = (uint32_t)s_rh;
    }
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ci.preTransform = caps.currentTransform;
    ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    if (!VK_OK(vkCreateSwapchainKHR(s_dev, &ci, NULL, &s_swap)))
        return 0;
    s_swap_w = ci.imageExtent.width;
    s_swap_h = ci.imageExtent.height;
    vkGetSwapchainImagesKHR(s_dev, s_swap, &s_nswap, NULL);
    s_swap_img = (VkImage *)calloc(s_nswap, sizeof *s_swap_img);
    vkGetSwapchainImagesKHR(s_dev, s_swap, &s_nswap, s_swap_img);
    s_sem_done = (VkSemaphore *)calloc(s_nswap, sizeof *s_sem_done);
    for (i = 0; i < s_nswap; i++) {
        VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        if (!VK_OK(vkCreateSemaphore(s_dev, &sci, NULL, &s_sem_done[i]))) {
            swap_destroy();
            return 0;
        }
    }
    return 1;
}

/* the record copied into its images, the picture composed (one triangle
   over 256 x 224, row 0 the first line), then drawn over d as rr_image
   draws */
void rr_present(void)
{
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait_st = VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkBufferImageCopy cp;
    uint32_t img = 0;
    int show = 0;
    if (!s_recording)
        return;
    if (!s_cleared) {                              /* nothing drawn: the clear alone */
        pass_begin();
        vkCmdEndRenderPass(s_cmd);
    }
    if (s_surface && host_window_visible()) {
        if (s_swap && ((int)s_swap_w != s_rw || (int)s_swap_h != s_rh))
            swap_destroy();
        if (!s_swap)
            swap_make();
        if (s_swap) {
            VkResult r = vkAcquireNextImageKHR(s_dev, s_swap, UINT64_MAX, s_sem_acquire, VK_NULL_HANDLE, &img);
            if (r == VK_ERROR_OUT_OF_DATE_KHR)
                swap_destroy();
            else if (r == VK_SUCCESS || r == VK_SUBOPTIMAL_KHR)
                show = (int)s_swap_w == s_rw && (int)s_swap_h == s_rh;
        }
    }
    barrier(s_cmd, s_col, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (show) {
        VkImageBlit b;
        barrier(s_cmd, s_swap_img[img], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0,
                VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        memset(&b, 0, sizeof b);
        b.srcSubresource.aspectMask = b.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.srcSubresource.layerCount = b.dstSubresource.layerCount = 1;
        b.srcOffsets[1].x = b.dstOffsets[1].x = s_rw;
        b.srcOffsets[1].y = b.dstOffsets[1].y = s_rh;
        b.srcOffsets[1].z = b.dstOffsets[1].z = 1;
        vkCmdBlitImage(s_cmd, s_col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_swap_img[img],
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, VK_FILTER_NEAREST);
        barrier(s_cmd, s_swap_img[img], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                VK_ACCESS_TRANSFER_WRITE_BIT, 0, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    /* the frame kept for a read back */
    memset(&cp, 0, sizeof cp);
    cp.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    cp.imageSubresource.layerCount = 1;
    cp.imageExtent.width = (uint32_t)s_rw;
    cp.imageExtent.height = (uint32_t)s_rh;
    cp.imageExtent.depth = 1;
    vkCmdCopyImageToBuffer(s_cmd, s_col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s_read, 1, &cp);
    barrier(s_cmd, s_col, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);
    vkEndCommandBuffer(s_cmd);
    s_recording = 0;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &s_cmd;
    if (show) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &s_sem_acquire;
        si.pWaitDstStageMask = &wait_st;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &s_sem_done[img];
    }
    vkQueueSubmit(s_queue, 1, &si, s_fence);
    s_submitted = 1;
    if (show) {
        VkPresentInfoKHR pr = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult r;
        pr.waitSemaphoreCount = 1;
        pr.pWaitSemaphores = &s_sem_done[img];
        pr.swapchainCount = 1;
        pr.pSwapchains = &s_swap;
        pr.pImageIndices = &img;
        r = vkQueuePresentKHR(s_queue, &pr);
        if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
            wait_frame();
            swap_destroy();
        }
    }
}

int rr_read(uint32_t *argb)
{
    wait_frame();
    memcpy(argb, s_read_map, (size_t)s_read_size);
    return 1;
}
