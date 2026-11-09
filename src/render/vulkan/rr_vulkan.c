/*
 * rr_vulkan.c: the renderer on Vulkan (Windows, Linux, Android). The store of
 * tiles, maps and images and each picture's description are storage buffers;
 * the picture is drawn by a fragment shader into a 32-bit integer image, then a
 * second fragment shader draws the window's pixels. One picture is in flight
 * at a time, so writing the store never races the GPU. Vulkan's loader is found
 * at run time.
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
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceProperties) X(vkCreateDevice) X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) \
    X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) \
    X(vkCreateDescriptorSetLayout) X(vkCreatePipelineLayout) X(vkCreateGraphicsPipelines) X(vkCreateDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) \
    X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) X(vkCmdEndRenderPass) \
    X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) X(vkCmdBindVertexBuffers) X(vkCmdPushConstants) X(vkCmdClearAttachments) \
    X(vkCmdCopyImageToBuffer) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkQueueWaitIdle) X(vkCreateFence) \
    X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDeviceWaitIdle) \
    X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
#define X(f) static PFN_##f f;
VK_FUNCS(X)
#undef X

typedef struct { VkBuffer b; VkDeviceMemory m; void *at; VkDeviceSize size; } Buf;
typedef struct { VkImage i; VkDeviceMemory m; VkImageView v; VkFramebuffer f; int w, h; } Target;

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkImage sc_img[8];
static VkImageView sc_view[8];
static VkFramebuffer sc_fb[8];
static uint32_t sc_n;
static int sc_w, sc_h;
static VkRenderPass rp_pic, rp_pic_keep, rp_win;
static int pic_fresh;
static VkPipelineLayout layout;
static VkPipeline pipe_pic, pipe_win;
static VkDescriptorSet set;
static VkSampler sampler;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore sem_acquire, sem_done;
static Buf store, descb, readback;
static Target pic, off, list_t[4], none_t;
static Buf verts;
static VkRenderPass rp_list;
static VkPipeline pipe_list, pipe_lines;
static int layouts_done[4], none_done;
static VkPipeline game_pipes[256][2];
static VkShaderModule list_vert;
static int last_w, last_h, ever;

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
    VkMemoryRequirements r;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    ci.size = size;
    ci.usage = usage;
    if (vkCreateBuffer(dev, &ci, NULL, &b->b) != VK_SUCCESS)
        exit(70);
    vkGetBufferMemoryRequirements(dev, b->b, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (vkAllocateMemory(dev, &ai, NULL, &b->m) != VK_SUCCESS)
        exit(70);
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

static VkShaderModule shader(const uint32_t *code, size_t bytes)
{
    VkShaderModuleCreateInfo ci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
    VkShaderModule m;
    ci.codeSize = bytes;
    ci.pCode = code;
    return vkCreateShaderModule(dev, &ci, NULL, &m) == VK_SUCCESS ? m : VK_NULL_HANDLE;
}

static VkRenderPass render_pass_op(VkFormat f, VkImageLayout final_layout, VkAttachmentLoadOp load)
{
    int keep = load == VK_ATTACHMENT_LOAD_OP_LOAD;
    VkAttachmentDescription a = { 0 };
    VkAttachmentReference ref = { 0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
    VkSubpassDescription sp = { 0 };
    VkSubpassDependency deps[2];
    VkRenderPassCreateInfo ci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO };
    VkRenderPass rp;
    a.format = f;
    a.samples = VK_SAMPLE_COUNT_1_BIT;
    a.loadOp = load;
    a.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    a.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a.initialLayout = keep ? final_layout : VK_IMAGE_LAYOUT_UNDEFINED;
    a.finalLayout = final_layout;
    sp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sp.colorAttachmentCount = 1;
    sp.pColorAttachments = &ref;
    memset(deps, 0, sizeof deps);
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    /* what was drawn is read by the next pass's fragment shader */
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    ci.attachmentCount = 1;
    ci.pAttachments = &a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    return vkCreateRenderPass(dev, &ci, NULL, &rp) == VK_SUCCESS ? rp : VK_NULL_HANDLE;
}

static VkRenderPass render_pass(VkFormat f, VkImageLayout final_layout)
{
    return render_pass_op(f, final_layout, VK_ATTACHMENT_LOAD_OP_DONT_CARE);
}

static VkPipeline pipeline_v(VkRenderPass rp, VkShaderModule vs, VkShaderModule fs, int vertices, VkPrimitiveTopology topo)
{
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba = { 0 };
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[2] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkPipeline p;
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vs;
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    ia.topology = topo;
    {
        static const VkVertexInputBindingDescription vb = { 0, ENG_DRAW_WORDS * 4, VK_VERTEX_INPUT_RATE_VERTEX };
        static const VkVertexInputAttributeDescription va[2] = { { 0, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 0 }, { 1, 0, VK_FORMAT_R32_UINT, 16 } };
        if (vertices) {
            vi.vertexBindingDescriptionCount = 1;
            vi.pVertexBindingDescriptions = &vb;
            vi.vertexAttributeDescriptionCount = 2;
            vi.pVertexAttributeDescriptions = va;
        }
    }
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    ba.colorWriteMask = 0xF;
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
    ci.renderPass = rp;
    return vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) == VK_SUCCESS ? p : VK_NULL_HANDLE;
}

static VkPipeline pipeline(VkRenderPass rp, VkShaderModule vs, VkShaderModule fs)
{
    return pipeline_v(rp, vs, fs, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
}

static void target_make(Target *t, VkFormat f, int w, int h, VkRenderPass rp, VkImageUsageFlags usage)
{
    VkImageCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
    VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
    VkMemoryRequirements r;
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
    if (t->i && t->w == w && t->h == h)
        return;
    if (t->i) {
        vkDestroyFramebuffer(dev, t->f, NULL);
        vkDestroyImageView(dev, t->v, NULL);
        vkDestroyImage(dev, t->i, NULL);
        vkFreeMemory(dev, t->m, NULL);
    }
    ci.imageType = VK_IMAGE_TYPE_2D;
    ci.format = f;
    ci.extent.width = (uint32_t)w;
    ci.extent.height = (uint32_t)h;
    ci.extent.depth = 1;
    ci.mipLevels = ci.arrayLayers = 1;
    ci.samples = VK_SAMPLE_COUNT_1_BIT;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if (vkCreateImage(dev, &ci, NULL, &t->i) != VK_SUCCESS)
        exit(70);
    vkGetImageMemoryRequirements(dev, t->i, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (vkAllocateMemory(dev, &ai, NULL, &t->m) != VK_SUCCESS)
        exit(70);
    vkBindImageMemory(dev, t->i, t->m, 0);
    vi.image = t->i;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &vi, NULL, &t->v);
    fi.renderPass = rp;
    fi.attachmentCount = 1;
    fi.pAttachments = &t->v;
    fi.width = (uint32_t)w;
    fi.height = (uint32_t)h;
    fi.layers = 1;
    vkCreateFramebuffer(dev, &fi, NULL, &t->f);
    t->w = w;
    t->h = h;
}

static void update_set(void)
{
    VkDescriptorBufferInfo b0 = { descb.b, 0, VK_WHOLE_SIZE }, b1 = { store.b, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii = { sampler, pic.v, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkDescriptorImageInfo li[4];
    VkWriteDescriptorSet w[7];
    int k;
    memset(w, 0, sizeof w);
    for (k = 0; k < 7; k++) {
        w[k].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[k].dstSet = set;
    }
    for (k = 0; k < 4; k++) {
        li[k].sampler = sampler;
        li[k].imageView = list_t[k].i && list_t[k].w ? list_t[k].v : none_t.v;
        li[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[3 + k].dstBinding = (uint32_t)(3 + k);
        w[3 + k].descriptorCount = 1;
        w[3 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[3 + k].pImageInfo = &li[k];
    }
    w[0].dstBinding = 0;
    w[1].dstBinding = 1;
    w[2].dstBinding = 2;
    w[0].descriptorCount = w[1].descriptorCount = w[2].descriptorCount = 1;
    w[0].descriptorType = w[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[0].pBufferInfo = &b0;
    w[1].pBufferInfo = &b1;
    w[2].pImageInfo = &ii;
    vkUpdateDescriptorSets(dev, 7, w, 0, NULL);
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

static void swapchain_make(int w, int h)
{
    VkSurfaceCapabilitiesKHR c;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = swapchain;
    uint32_t i, n;
    vkDeviceWaitIdle(dev);
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &c);
    n = c.minImageCount + 1;
    if (c.maxImageCount && n > c.maxImageCount)
        n = c.maxImageCount;
    if (n > 8)
        n = 8;
    ci.surface = surface;
    ci.minImageCount = n;
    ci.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
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
    if (vkCreateSwapchainKHR(dev, &ci, NULL, &swapchain) != VK_SUCCESS)
        exit(70);
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
        VkFramebufferCreateInfo fi = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO };
        vi.image = sc_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_B8G8R8A8_UNORM;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
        vkCreateImageView(dev, &vi, NULL, &sc_view[i]);
        fi.renderPass = rp_win;
        fi.attachmentCount = 1;
        fi.pAttachments = &sc_view[i];
        fi.width = (uint32_t)w;
        fi.height = (uint32_t)h;
        fi.layers = 1;
        vkCreateFramebuffer(dev, &fi, NULL, &sc_fb[i]);
    }
    sc_w = w;
    sc_h = h;
}

/* ---- opening ---- */
int rr_open(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[2] = { "VK_KHR_surface", NULL };
    int windowed = host_window_handle() != NULL;
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

    rp_pic = render_pass(VK_FORMAT_R32_UINT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    rp_pic_keep = render_pass_op(VK_FORMAT_R32_UINT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ATTACHMENT_LOAD_OP_LOAD);
    rp_win = render_pass(VK_FORMAT_B8G8R8A8_UNORM, surface ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL);
    {
        VkDescriptorSetLayoutBinding b[7];
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
        VkDescriptorSetLayout sl;
        VkPipelineLayoutCreateInfo pi = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
        VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 }, { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5 } };
        VkDescriptorPoolCreateInfo dp = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
        VkDescriptorPool pool;
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
        VkSamplerCreateInfo si = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
        VkShaderModule vs, fp, fw;
        memset(b, 0, sizeof b);
        for (i = 0; i < 7; i++) {
            b[i].binding = i;
            b[i].descriptorCount = 1;
            b[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
            b[i].descriptorType = i < 2 ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        li.bindingCount = 7;
        li.pBindings = b;
        vkCreateDescriptorSetLayout(dev, &li, NULL, &sl);
        {
            static const VkPushConstantRange pc = { VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64 };
            pi.pushConstantRangeCount = 1;
            pi.pPushConstantRanges = &pc;
        }
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &sl;
        vkCreatePipelineLayout(dev, &pi, NULL, &layout);
        dp.maxSets = 1;
        dp.poolSizeCount = 2;
        dp.pPoolSizes = ps;
        vkCreateDescriptorPool(dev, &dp, NULL, &pool);
        ai.descriptorPool = pool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &sl;
        vkAllocateDescriptorSets(dev, &ai, &set);
        vkCreateSampler(dev, &si, NULL, &sampler);
        vs = shader(k_quad_vert, sizeof k_quad_vert);
        fp = shader(k_picture_frag, sizeof k_picture_frag);
        fw = shader(k_window_frag, sizeof k_window_frag);
        pipe_pic = pipeline(rp_pic, vs, fp);
        pipe_win = pipeline(rp_win, vs, fw);
        {
            VkShaderModule lv = shader(k_list_vert, sizeof k_list_vert), lf = shader(k_list_frag, sizeof k_list_frag);
            rp_list = render_pass_op(VK_FORMAT_R32_UINT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ATTACHMENT_LOAD_OP_CLEAR);
            pipe_list = pipeline_v(rp_list, lv, lf, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            pipe_lines = pipeline_v(rp_list, lv, lf, 1, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
            list_vert = lv;
            vkDestroyShaderModule(dev, lf, NULL);
            if (!pipe_list || !pipe_lines)
                return 0;
        }
        vkDestroyShaderModule(dev, vs, NULL);
        vkDestroyShaderModule(dev, fp, NULL);
        vkDestroyShaderModule(dev, fw, NULL);
        if (!pipe_pic || !pipe_win)
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
    return 1;
}

void rr_close(void)
{
    if (dev)
        vkDeviceWaitIdle(dev);
}

const char *rr_name(void) { return "vulkan"; }
const char *rr_shader_ext(void) { return "spv"; }

#define LIST_USE (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT)

static void draw_lists(const eng_packet *p)
{
    VkDeviceSize off = 0;
    int k;
    for (k = 0; k < 4; k++) {
        const eng_list *l = &p->lists[k];
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue clr;
        VkViewport vp;
        VkRect2D sc;
        size_t i;
        if (!l->w)
            continue;
        memset(&clr, 0, sizeof clr);
        vp.x = vp.y = 0;
        vp.width = (float)l->w;
        vp.height = (float)l->h;
        vp.minDepth = 0;
        vp.maxDepth = 1;
        sc.offset.x = sc.offset.y = 0;
        sc.extent.width = l->w;
        sc.extent.height = l->h;
        bi.renderPass = rp_list;
        bi.framebuffer = list_t[k].f;
        bi.renderArea = sc;
        bi.clearValueCount = 1;
        bi.pClearValues = &clr;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
        if (l->nverts)
            vkCmdBindVertexBuffers(cmd, 0, 1, &verts.b, &off);
        for (i = 0; i < l->ncmds; i++) {
            const uint32_t *c = p->list_data + l->cmds + i * ENG_CMD_WORDS;
            struct { float w, h; uint32_t base, tw, th, p0, p1, p2; float inputs[8]; } u = { (float)l->w, (float)l->h, c[3], c[4], c[5], 0, 0, 0, { 0 } };
            VkPipeline chosen = c[2] ? pipe_lines : pipe_list;
            memcpy(u.inputs, c + 7, 32);
            if (c[6] && game_pipes[c[6]][c[2] != 0])
                chosen = game_pipes[c[6]][c[2] != 0];
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, chosen);
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 64, &u);
            vkCmdDraw(cmd, c[1], 1, c[0], 0);
        }
        vkCmdEndRenderPass(cmd);
        off += l->nverts * ENG_DRAW_WORDS * 4;
    }
}

static void pass(VkRenderPass rp, VkFramebuffer fb, VkPipeline p, int w, int h)
{
    VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    VkViewport vp = { 0, 0, (float)w, (float)h, 0, 1 };
    VkRect2D sc = { { 0, 0 }, { (uint32_t)w, (uint32_t)h } };
    bi.renderPass = rp;
    bi.framebuffer = fb;
    bi.renderArea = sc;
    vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 0, NULL);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd);
}

/* records the copy of the picture to the readback buffer */
static void copy_picture(int w, int h)
{
    VkBufferImageCopy c;
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    memset(&c, 0, sizeof c);
    c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    c.imageSubresource.layerCount = 1;
    c.imageExtent.width = (uint32_t)w;
    c.imageExtent.height = (uint32_t)h;
    c.imageExtent.depth = 1;
    b.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = pic.i;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vkCmdCopyImageToBuffer(cmd, pic.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &c);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &b);
}

int rr_draw(eng_packet *p)
{
    VkDeviceSize db = p->desc_words * 4, sb = (p->store_words ? p->store_words : 1) * 4;
    int resized = 0, ow, oh, paced = 0, grew = 0;
    uint32_t image = 0;
    size_t i = 0;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);

    {
        uint32_t k;
        for (k = 0; k < p->nshaders; k++) {
            VkShaderModule m = shader((const uint32_t *)p->shaders[k].code, p->shaders[k].size);
            if (m) {
                game_pipes[p->shaders[k].id][0] = pipeline_v(rp_list, list_vert, m, 1, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
                game_pipes[p->shaders[k].id][1] = pipeline_v(rp_list, list_vert, m, 1, VK_PRIMITIVE_TOPOLOGY_LINE_LIST);
                vkDestroyShaderModule(dev, m, NULL);
            }
        }
    }
    if (store.size < sb) {
        Buf n;
        VkDeviceSize c = store.size ? store.size : 1u << 20;
        while (c < sb)
            c *= 2;
        buf_make(&n, c, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        if (store.b)
            memcpy(n.at, store.at, (size_t)store.size);
        buf_free(&store);
        store = n;
        grew = 1;
    }
    while (i < p->ops_words) {
        uint32_t at = p->ops[i], n = p->ops[i + 1];
        memcpy((uint32_t *)store.at + at, p->ops + i + 2, (size_t)n * 4);
        i += 2 + (size_t)n;
    }
    if (descb.size < db) {
        buf_free(&descb);
        buf_make(&descb, db * 2, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
        grew = 1;
    }

    ow = p->width;
    oh = p->height;
    if (surface) {
        host_window_pixels(&ow, &oh);
        if (ow < 1 || oh < 1)
            return 1;       /* minimised: nothing to draw on */
        if (ow != sc_w || oh != sc_h)
            swapchain_make(ow, oh);
    }
    rr_window_words(p->desc, ow, oh);
    memcpy(descb.at, p->desc, (size_t)db);

    if (!pic.i || pic.w != p->width || pic.h != p->height) {
        target_make(&pic, VK_FORMAT_R32_UINT, p->width, p->height, rp_pic,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        grew = 1;
        pic_fresh = 1;
    }
    {
        int k;
        VkDeviceSize vbytes = 0, voff = 0;
        target_make(&none_t, VK_FORMAT_R32_UINT, 1, 1, rp_list, LIST_USE);
        for (k = 0; k < 4; k++) {
            const eng_list *l = &p->lists[k];
            if (l->w && (list_t[k].w != (int)l->w || list_t[k].h != (int)l->h)) {
                target_make(&list_t[k], VK_FORMAT_R32_UINT, (int)l->w, (int)l->h, rp_list, LIST_USE);
                grew = 1;
            }
            if (!l->w && layouts_done[k]) {
                /* a layer with no list reads the empty image */
                list_t[k].w = 0;
                grew = 1;
            }
            layouts_done[k] = l->w != 0;
            vbytes += l->nverts * ENG_DRAW_WORDS * 4;
        }
        if (vbytes && verts.size < vbytes) {
            buf_free(&verts);
            buf_make(&verts, vbytes * 2, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        }
        for (k = 0; k < 4; k++) {
            const eng_list *l = &p->lists[k];
            if (l->nverts) {
                memcpy((char *)verts.at + voff, p->list_data + l->verts, l->nverts * ENG_DRAW_WORDS * 4);
                voff += l->nverts * ENG_DRAW_WORDS * 4;
            }
        }
    }
    if (grew || !ever)
        update_set();
    ever = 1;
    last_w = p->width;
    last_h = p->height;

    if (surface) {
        VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &image);
        if (r == VK_ERROR_OUT_OF_DATE_KHR) {
            sc_w = 0;
            return 1;
        }
        paced = 1;
    } else {
        target_make(&off, VK_FORMAT_B8G8R8A8_UNORM, ow, oh, rp_win, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
        if (readback.size < (VkDeviceSize)p->width * p->height * 4) {
            buf_free(&readback);
            buf_make(&readback, (VkDeviceSize)p->width * p->height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
    }

    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    if (!none_done) {
        /* the empty image, cleared once and held readable */
        eng_packet none_p;
        VkRenderPassBeginInfo nb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue clr;
        VkRect2D sc = { { 0, 0 }, { 1, 1 } };
        memset(&none_p, 0, sizeof none_p);
        memset(&clr, 0, sizeof clr);
        nb.renderPass = rp_list;
        nb.framebuffer = none_t.f;
        nb.renderArea = sc;
        nb.clearValueCount = 1;
        nb.pClearValues = &clr;
        vkCmdBeginRenderPass(cmd, &nb, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(cmd);
        none_done = 1;
    }
    draw_lists(p);
    pass(p->keep && !pic_fresh ? rp_pic_keep : rp_pic, pic.f, pipe_pic, p->width, p->height);
    pic_fresh = 0;
    {
        /* the first pass's attachment store before the second's shader reads */
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        b.oldLayout = b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = pic.i;
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = b.subresourceRange.layerCount = 1;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, NULL, 0, NULL, 1, &b);
    }
    pass(rp_win, surface ? sc_fb[image] : off.f, pipe_win, ow, oh);
    if (!surface)
        copy_picture(p->width, p->height);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (surface) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_acquire;
        si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sem_done;
    }
    vkQueueSubmit(queue, 1, &si, fence);
    if (surface) {
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
    (void)resized;
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!ever || (!surface && !readback.at))
        return 0;
    if (argb) {
        vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        if (surface) {
            /* a window's pictures aren't copied as they are drawn: copied now */
            VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
            VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
            if (readback.size < (VkDeviceSize)last_w * last_h * 4) {
                buf_free(&readback);
                buf_make(&readback, (VkDeviceSize)last_w * last_h * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
            }
            vkResetFences(dev, 1, &fence);
            vkResetCommandBuffer(cmd, 0);
            vkBeginCommandBuffer(cmd, &begin);
            copy_picture(last_w, last_h);
            vkEndCommandBuffer(cmd);
            si.commandBufferCount = 1;
            si.pCommandBuffers = &cmd;
            vkQueueSubmit(queue, 1, &si, fence);
            vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX);
        }
        memcpy(argb, readback.at, (size_t)last_w * last_h * 4);
    }
    return 1;
}

int rr_read_window(uint32_t *argb, int *w, int *h)
{
    (void)argb;
    *w = *h = 0;
    return 0;
}
