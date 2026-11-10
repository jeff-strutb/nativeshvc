/*
 * rr_vulkan.c: the renderer (rr.h) on Vulkan (Windows, Linux, Android). The
 * frame's textures are images, made before the frame is recorded and written
 * through a staging buffer at its start; its words are a storage buffer and
 * its uniforms one dynamic uniform buffer; each pass is a render pass, its
 * draws instanced draws with no vertex buffers. Pipelines and descriptor sets
 * are made when first needed and kept, found by hashing. The programs are
 * SPIR-V (rr_spv.h). The window pass resolves its multisampled colour straight
 * into the swapchain's image (the multisampled colour, depth and stencil are
 * transient). One frame is in flight at a time, so writing a buffer never
 * races the GPU. A frame read back (rr_read) is run again: the backend keeps
 * a reference to the last frame it ran, and its passes but the textures' are
 * recorded again, the window's into an image. Vulkan's loader is found at run
 * time.
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
    X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceImageFormatProperties) X(vkCreateDevice) X(vkDestroyDevice) X(vkGetDeviceProcAddr) \
    X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkFreeMemory) X(vkBindBufferMemory) X(vkMapMemory) X(vkCreateImage) X(vkDestroyImage) \
    X(vkGetImageMemoryRequirements) X(vkBindImageMemory) X(vkCreateImageView) X(vkDestroyImageView) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateRenderPass) X(vkDestroyRenderPass) X(vkCreateFramebuffer) X(vkDestroyFramebuffer) X(vkCreateShaderModule) \
    X(vkDestroyShaderModule) X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreatePipelineLayout) \
    X(vkDestroyPipelineLayout) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkResetDescriptorPool) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreateCommandPool) X(vkDestroyCommandPool) \
    X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkCmdBeginRenderPass) \
    X(vkCmdEndRenderPass) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdDraw) \
    X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdBlitImage) X(vkCmdClearColorImage) \
    X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdPipelineBarrier) X(vkQueueSubmit) X(vkQueueWaitIdle) \
    X(vkCreateFence) X(vkDestroyFence) X(vkWaitForFences) X(vkResetFences) X(vkCreateSemaphore) X(vkDestroySemaphore) \
    X(vkDeviceWaitIdle) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) \
    X(vkGetPhysicalDeviceSurfaceFormatsKHR) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkAcquireNextImageKHR) X(vkQueuePresentKHR) X(vkDestroySurfaceKHR)

static PFN_vkGetInstanceProcAddr vkGetInstanceProcAddr;
static PFN_vkCreateInstance vkCreateInstance;
#define X(f) static PFN_##f f;
VK_FUNCS(X)
#undef X

typedef struct { const void *vs; size_t vs_size; const void *fs; size_t fs_size; } Program;
static const Program programs[RR_PROGRAMS] = { RR_PROGRAM_TABLE };

#define MAX_TEXTURES 16384
#define MAX_SHADERS 256
#define COLOUR VK_FORMAT_B8G8R8A8_UNORM     /* the renderer's own colour targets */
#define UNUSED_FRAMES 120                   /* an extra target unused this many frames is let go */

typedef struct { VkBuffer b; VkDeviceMemory m; void *at; VkDeviceSize size; } Buf;
/* an image: v its view of every level (and layer), fv of the first level
   (drawn into, through f); an array's layers' first levels lv, through lf;
   fresh: not drawn into yet (each layer's, an array's); a target with
   smaller levels: each layer's each level its own view (mv, layer * levels +
   level) and, past the first, a framebuffer (mf), its level made from the
   one before by the renderer's own program */
typedef struct {
    VkImage i;
    VkDeviceMemory m;
    VkImageView v, fv, *lv, *mv;
    VkFramebuffer f, *lf, *mf;
    uint8_t *fresh;
    int w, h, format, levels, layers, array, target, made, unready;
} Image;

/* what a pass draws into: the multisampled screens with depth and stencil, an
   8-bit or 32-bit integer texture, a colour texture or the picture, or the
   window (a screen in the swapchain's format) */
enum { KIND_SCREEN, KIND_INDEX, KIND_INDEX32, KIND_COLOUR, KIND_WINDOW, KINDS };

static VkInstance inst;
static VkPhysicalDevice phys;
static VkDevice dev;
static VkQueue queue;
static uint32_t family;
static VkFormat depth_format;
static VkSurfaceKHR surface;
static VkSwapchainKHR swapchain;
static VkFormat sc_format;
/* the swapchain's images, as many as it made; sc_done each one's: its frame drawn, for its present */
static VkImage *sc_img;
static VkImageView *sc_view;
static VkFramebuffer *sc_fb;
static VkSemaphore *sc_done;
static uint32_t sc_n, sc_cap;           /* images; semaphores made */
static int sc_ok;                       /* a swapchain for the window as it is */
static int sc_failed;                   /* making one failed: not tried again until the window's size changes */
static int sc_lost;                     /* the surface was lost: made again from the window */
static int windowed;                    /* drawn for a window (one may come and go), not headless */
static void *sc_window;                 /* the window the surface is made on */
static int sc_w, sc_h;                  /* the window's size the swapchain was made (or tried) for */
static int sc_mirror;                   /* the picture mirrored, before it is turned, when drawn into the window's images */
static int sc_ew, sc_eh;                /* the picture's size: the surface's in the app's orientation */
static int sc_iw, sc_ih;                /* the images', in the display's own orientation */
static int sc_turns;                    /* quarter turns clockwise the picture is given in them */
/* render passes, [load]: cleared, or what it held kept; the window's is in the swapchain's format */
static VkRenderPass rp_screen, rp_colour[2], rp_index[2], rp_index32[2], rp_window;
static VkRenderPass rp_side[5];         /* a side target's, by log2 of its samples: made when first used; [0] one sample, no resolve */
static int pass_samples, samples_have;  /* the screens' samples a pixel; the counts the device has (a bit each) */
static VkDescriptorSetLayout set_layout;
static VkPipelineLayout layout;
static VkSampler sampler, filtered, bordered;   /* at texels; filtered repeating; filtered, transparent outside */
static VkCommandPool cmd_pool;
static VkCommandBuffer cmd;
static VkFence fence;
static VkSemaphore sem_acquire;
static VkShaderModule vert_mod[RR_PROGRAMS], frag_mod[RR_PROGRAMS], game_mods[MAX_SHADERS];
static Image tex[MAX_TEXTURES];
static Image none_colour, none_index, none_array;
/* the screens by index: 0 the frame, 1 and 2 the extra targets, 3 and on the side targets (RR_SIDES); each resolved
   into screen_res, or a side target at one sample drawn into it */
#define SCREENS (3 + RR_SIDES)
static Image screen_ms[SCREENS], screen_ds[SCREENS], screen_res[SCREENS], picture;   /* and the picture */
/* RR_DRAW (read at rr_open): with no window, the window's pass is run too, into draw_img (never read or shown) */
static int draw_all;
static Image draw_ms, draw_ds, draw_img;
static VkFramebuffer draw_fb;
static void sink_free(void);
static Image win_ms, win_ds;            /* the window's multisampled colour (the swapchain's format) and depth, with the swapchain */
static VkFramebuffer screen_fb[SCREENS];
static int screen_w[SCREENS], screen_h[SCREENS], screen_samp[SCREENS];
static uint64_t screen_used[SCREENS], picture_used, frame_no;
static Buf ubuf, dbuf, staging, readback;
static int last_w, last_h, ever;
static const rr_frame *last_frame;      /* the last frame run, for rr_read: its owner leaves it whole until a later one is run */
static float last_aspect;
static int last_turns, last_mirror;     /* the orientation that frame's window-sized targets were drawn in */
static uint64_t views_gen = 1;          /* changes whenever a view or buffer a set holds is made again */

/* a call that must work, failed: said in the log, then the end */
static void fail(const char *what, VkResult r)
{
    fprintf(stderr, "rr: vulkan: %s failed (VkResult %d)\n", what, (int)r);
    exit(70);
}

static uint64_t mix64(uint64_t x)
{
    x ^= x >> 33;
    x *= 0xff51afd7ed558ccdull;
    x ^= x >> 33;
    x *= 0xc4ceb9fe1a85ec53ull;
    x ^= x >> 33;
    return x;
}

/* a memory type with `want`, else with `need` */
static uint32_t mem_type2(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags need)
{
    VkPhysicalDeviceMemoryProperties p;
    uint32_t i;
    vkGetPhysicalDeviceMemoryProperties(phys, &p);
    for (i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & want) == want)
            return i;
    for (i = 0; i < p.memoryTypeCount; i++)
        if ((bits >> i & 1) && (p.memoryTypes[i].propertyFlags & need) == need)
            return i;
    fprintf(stderr, "rr: vulkan: no suitable memory\n");
    exit(70);
}

static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want) { return mem_type2(bits, want, want); }

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

/* b at least `size` bytes; made again (and the sets that hold it with it) when smaller */
static void buf_fit(Buf *b, VkDeviceSize size, VkBufferUsageFlags usage)
{
    if (b->size >= size)
        return;
    buf_free(b);
    buf_make(b, size * 2, usage);
    views_gen++;
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
    int k;
    if (t->made)
        views_gen++;
    for (k = 0; t->lv && k < t->layers; k++) {
        if (t->lf && t->lf[k])
            vkDestroyFramebuffer(dev, t->lf[k], NULL);
        if (t->lv[k])
            vkDestroyImageView(dev, t->lv[k], NULL);
    }
    for (k = 0; t->mv && k < t->layers * t->levels; k++) {
        if (t->mf[k])
            vkDestroyFramebuffer(dev, t->mf[k], NULL);
        if (t->mv[k])
            vkDestroyImageView(dev, t->mv[k], NULL);
    }
    free(t->mv);
    free(t->mf);
    free(t->lv);
    free(t->lf);
    free(t->fresh);
    if (t->f)
        vkDestroyFramebuffer(dev, t->f, NULL);
    if (t->fv && t->fv != t->v)
        vkDestroyImageView(dev, t->fv, NULL);
    if (t->v)
        vkDestroyImageView(dev, t->v, NULL);
    if (t->i) {
        vkDestroyImage(dev, t->i, NULL);
        vkFreeMemory(dev, t->m, NULL);
    }
    memset(t, 0, sizeof *t);
}

static int levels_of(int w, int h)
{
    int n = 1;
    while ((w > 1 || h > 1) && n < 16) {
        w = w > 1 ? w / 2 : 1;
        h = h > 1 ? h / 2 : 1;
        n++;
    }
    return n;
}

/* an image; transient ones (never kept past their pass) in lazily allocated memory where the GPU has it */
static void image_make_layers(Image *t, VkFormat f, int w, int h, int samples, int levels, int layers, int array, VkImageUsageFlags usage,
                              VkImageAspectFlags aspect)
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
    ci.mipLevels = (uint32_t)levels;
    ci.arrayLayers = (uint32_t)layers;
    ci.samples = (VkSampleCountFlagBits)samples;
    ci.tiling = VK_IMAGE_TILING_OPTIMAL;
    ci.usage = usage;
    if ((e = vkCreateImage(dev, &ci, NULL, &t->i)) != VK_SUCCESS)
        fail("vkCreateImage", e);
    vkGetImageMemoryRequirements(dev, t->i, &r);
    ai.allocationSize = r.size;
    ai.memoryTypeIndex = usage & VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT
                             ? mem_type2(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT,
                                         VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                             : mem_type(r.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if ((e = vkAllocateMemory(dev, &ai, NULL, &t->m)) != VK_SUCCESS)
        fail("vkAllocateMemory (an image)", e);
    vkBindImageMemory(dev, t->i, t->m, 0);
    vi.image = t->i;
    vi.viewType = array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    vi.format = f;
    vi.subresourceRange.aspectMask = aspect;
    vi.subresourceRange.levelCount = (uint32_t)levels;
    vi.subresourceRange.layerCount = (uint32_t)layers;
    if ((e = vkCreateImageView(dev, &vi, NULL, &t->v)) != VK_SUCCESS)
        fail("vkCreateImageView", e);
    t->fv = t->v;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange.levelCount = 1;
    vi.subresourceRange.layerCount = 1;
    if (array) {
        int k;
        t->lv = (VkImageView *)calloc((size_t)layers, sizeof *t->lv);
        t->lf = (VkFramebuffer *)calloc((size_t)layers, sizeof *t->lf);
        if (!t->lv || !t->lf)
            exit(70);
        for (k = 0; k < layers; k++) {
            vi.subresourceRange.baseArrayLayer = (uint32_t)k;
            if ((e = vkCreateImageView(dev, &vi, NULL, &t->lv[k])) != VK_SUCCESS)
                fail("vkCreateImageView", e);
        }
    } else if (levels > 1) {
        if ((e = vkCreateImageView(dev, &vi, NULL, &t->fv)) != VK_SUCCESS)
            fail("vkCreateImageView", e);
    }
    t->fresh = (uint8_t *)malloc((size_t)layers);
    if (!t->fresh)
        exit(70);
    memset(t->fresh, 1, (size_t)layers);
    t->w = w;
    t->h = h;
    t->levels = levels;
    t->layers = layers;
    t->array = array;
    t->made = 1;
    t->unready = (usage & VK_IMAGE_USAGE_SAMPLED_BIT) != 0;   /* to be made readable before anything reads it */
}

static void image_make(Image *t, VkFormat f, int w, int h, int samples, VkImageUsageFlags usage, VkImageAspectFlags aspect)
{
    image_make_layers(t, f, w, h, samples, 1, 1, 0, usage, aspect);
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

/* a colour target's views of each layer's each level, and framebuffers for
   all but the first (mv, mf), its smaller levels drawn from them */
static void mip_views(Image *t, VkFormat f, VkRenderPass rp)
{
    int ly, lv, n = t->layers * t->levels;
    VkResult e;
    t->mv = (VkImageView *)calloc((size_t)n, sizeof *t->mv);
    t->mf = (VkFramebuffer *)calloc((size_t)n, sizeof *t->mf);
    if (!t->mv || !t->mf)
        exit(70);
    for (ly = 0; ly < t->layers; ly++)
        for (lv = 0; lv < t->levels; lv++) {
            VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
            int k = ly * t->levels + lv, w = t->w >> lv ? t->w >> lv : 1, h = t->h >> lv ? t->h >> lv : 1;
            vi.image = t->i;
            vi.viewType = t->array ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
            vi.format = f;
            vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            vi.subresourceRange.baseMipLevel = (uint32_t)lv;
            vi.subresourceRange.levelCount = 1;
            vi.subresourceRange.baseArrayLayer = (uint32_t)ly;
            vi.subresourceRange.layerCount = 1;
            if ((e = vkCreateImageView(dev, &vi, NULL, &t->mv[k])) != VK_SUCCESS)
                fail("vkCreateImageView (a level's)", e);
            if (lv)
                t->mf[k] = framebuffer(rp, &t->mv[k], 1, w, h);
        }
}

static void barrier_levels(VkImage i, uint32_t layer, uint32_t level, uint32_t count, VkImageLayout from, VkImageLayout to, VkAccessFlags src,
                           VkAccessFlags dst, VkPipelineStageFlags ss, VkPipelineStageFlags ds)
{
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = src;
    b.dstAccessMask = dst;
    b.oldLayout = from;
    b.newLayout = to;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = i;
    b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    b.subresourceRange.baseMipLevel = level;
    b.subresourceRange.levelCount = count;
    b.subresourceRange.baseArrayLayer = layer;
    b.subresourceRange.layerCount = 1;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
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
    b.subresourceRange.levelCount = VK_REMAINING_MIP_LEVELS;
    b.subresourceRange.layerCount = VK_REMAINING_ARRAY_LAYERS;
    vkCmdPipelineBarrier(cmd, ss, ds, 0, 0, NULL, 0, NULL, 1, &b);
}

#define SHADER_STAGES (VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT)

/* a texture just made reads 0 until written (rr.h): every level and layer
   cleared, then readable */
static void clear_new(VkImage i)
{
    VkClearColorValue z;
    VkImageSubresourceRange r;
    memset(&z, 0, sizeof z);
    memset(&r, 0, sizeof r);
    r.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    r.levelCount = VK_REMAINING_MIP_LEVELS;
    r.layerCount = VK_REMAINING_ARRAY_LAYERS;
    barrier(i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT);
    vkCmdClearColorImage(cmd, i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &z, 1, &r);
    barrier(i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT, SHADER_STAGES);
}

#define ALL_WRITES (VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT)
#define ALL_READS (VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | \
                   VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT)
#define ALL_STAGES (VK_PIPELINE_STAGE_TRANSFER_BIT | SHADER_STAGES | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | \
                    VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT)

/* a render pass of colour (samples), with depth and stencil and a resolve
   if `screen` is 1, with depth and stencil and no resolve if 2 (one sample,
   drawn straight into its image); what came before it is done before it, and
   it before what follows */
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
    a[0].storeOp = screen == 1 ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE;
    a[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    a[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    a[0].initialLayout = screen ? VK_IMAGE_LAYOUT_UNDEFINED : initial;
    a[0].finalLayout = screen == 1 ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : final;
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
        sp.pDepthStencilAttachment = &dr;
        if (screen == 1) {
            a[2].format = f;
            a[2].samples = VK_SAMPLE_COUNT_1_BIT;
            a[2].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[2].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
            a[2].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
            a[2].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
            a[2].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            a[2].finalLayout = final;
            sp.pResolveAttachments = &rr;
        }
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
    ci.attachmentCount = screen == 1 ? 3 : screen == 2 ? 2 : 1;
    ci.pAttachments = a;
    ci.subpassCount = 1;
    ci.pSubpasses = &sp;
    ci.dependencyCount = 2;
    ci.pDependencies = deps;
    if (vkCreateRenderPass(dev, &ci, NULL, &rp) != VK_SUCCESS)
        fail("vkCreateRenderPass", VK_ERROR_INITIALIZATION_FAILED);
    return rp;
}

static int integer_kind(int kind) { return kind == KIND_INDEX || kind == KIND_INDEX32; }

/* blending, where the target can blend: an integer target never does */
static void blend_of(VkPipelineColorBlendAttachmentState *a, int blend, int kind)
{
    memset(a, 0, sizeof *a);
    a->colorWriteMask = blend == RR_BLEND_NOCOLOUR ? 0 : 0xF;
    a->blendEnable = blend == RR_BLEND_OVER && !integer_kind(kind);
    a->colorBlendOp = a->alphaBlendOp = VK_BLEND_OP_ADD;
    a->srcColorBlendFactor = a->srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    a->dstColorBlendFactor = a->dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
}

/* the render pass of a side target of `samples`, made when first asked for */
static VkRenderPass side_pass(int samples)
{
    int i = samples == 1 ? 0 : samples == 2 ? 1 : samples == 4 ? 2 : samples == 8 ? 3 : 4;
    if (!rp_side[i])
        rp_side[i] = render_pass(COLOUR, samples, samples == 1 ? 2 : 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED,
                                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return rp_side[i];
}

static VkRenderPass pass_of_kind(int kind, int samples)
{
    switch (kind) {
    case KIND_SCREEN: return samples == pass_samples ? rp_screen : side_pass(samples);
    case KIND_INDEX: return rp_index[0];
    case KIND_INDEX32: return rp_index32[0];
    case KIND_WINDOW: return rp_window;
    default: return rp_colour[0];
    }
}

/* ---- pipelines: made when first asked for, a failure kept too, found by hashing ---- */
typedef struct { uint64_t key; VkPipeline p; int used; } PipeSlot;
static PipeSlot *pipes;
static size_t npipes, cap_pipes;

static PipeSlot *pipe_slot(uint64_t key)
{
    size_t i;
    if ((npipes + 1) * 2 > cap_pipes) {
        /* twice as many slots, every pipeline placed again */
        size_t c = cap_pipes ? cap_pipes * 2 : 256, k;
        PipeSlot *n = (PipeSlot *)calloc(c, sizeof *n);
        if (!n)
            exit(70);
        for (k = 0; k < cap_pipes; k++)
            if (pipes[k].used) {
                for (i = mix64(pipes[k].key) & (c - 1); n[i].used; i = (i + 1) & (c - 1))
                    ;
                n[i] = pipes[k];
            }
        free(pipes);
        pipes = n;
        cap_pipes = c;
    }
    for (i = mix64(key) & (cap_pipes - 1); pipes[i].used && pipes[i].key != key; i = (i + 1) & (cap_pipes - 1))
        ;
    return &pipes[i];
}

static VkPipeline pipe_of(int program, int fshader, int blend, int depth, int stencil, int kind, int samples)
{
    uint64_t key = (uint64_t)program | (uint64_t)fshader << 8 | (uint64_t)blend << 20 | (uint64_t)depth << 24 | (uint64_t)stencil << 28 |
                   (uint64_t)kind << 32 | (uint64_t)samples << 40;
    PipeSlot *slot = pipe_slot(key);
    VkPipelineShaderStageCreateInfo st[2];
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO };
    VkPipelineViewportStateCreateInfo vp = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO };
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO };
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO };
    VkPipelineDepthStencilStateCreateInfo dsi = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO };
    VkPipelineColorBlendAttachmentState ba;
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO };
    VkDynamicState dyn[5] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                              VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
    VkPipelineDynamicStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO };
    VkGraphicsPipelineCreateInfo ci = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO };
    VkStencilOpState so;
    VkPipeline p = VK_NULL_HANDLE;
    VkShaderModule fs = fshader ? game_mods[fshader] : frag_mod[program];
    if (slot->used)
        return slot->p;
    slot->used = 1;
    slot->key = key;
    slot->p = VK_NULL_HANDLE;
    npipes++;
    if (!fs || !vert_mod[program]) {
        fprintf(stderr, "rr: vulkan: no shader for program %d (game shader %d)\n", program, fshader);
        return VK_NULL_HANDLE;
    }
    memset(st, 0, sizeof st);
    st[0].sType = st[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    st[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    st[0].module = vert_mod[program];
    st[0].pName = "main";
    st[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    st[1].module = fs;
    st[1].pName = "main";
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    vp.viewportCount = vp.scissorCount = 1;
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1;
    ms.rasterizationSamples = kind == KIND_SCREEN ? (VkSampleCountFlagBits)samples : kind == KIND_WINDOW ? (VkSampleCountFlagBits)pass_samples : VK_SAMPLE_COUNT_1_BIT;
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
    blend_of(&ba, blend, kind);
    cb.attachmentCount = 1;
    cb.pAttachments = &ba;
    ds.dynamicStateCount = 5;
    ds.pDynamicStates = dyn;
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vp;
    ci.pRasterizationState = &rs;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = kind == KIND_SCREEN || kind == KIND_WINDOW ? &dsi : NULL;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &ds;
    ci.layout = layout;
    ci.renderPass = pass_of_kind(kind, samples);
    if (vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p) != VK_SUCCESS) {
        fprintf(stderr, "rr: vulkan: pipeline %d could not be made\n", program);
        p = VK_NULL_HANDLE;
    }
    slot->p = p;
    return p;
}

/* every pipeline drawing into the window, made or failed, let go: its render pass is made again */
static void pipes_drop_window(void)
{
    size_t i;
    for (i = 0; i < cap_pipes; i++)
        if (pipes[i].used && (pipes[i].key >> 32) == KIND_WINDOW) {
            if (pipes[i].p)
                vkDestroyPipeline(dev, pipes[i].p, NULL);
            pipes[i].p = VK_NULL_HANDLE;
            pipes[i].used = 0;
        }
    if (cap_pipes) {
        /* the hash's chains made whole again */
        PipeSlot *old = pipes;
        size_t c = cap_pipes, k;
        pipes = NULL;
        cap_pipes = npipes = 0;
        for (k = 0; k < c; k++)
            if (old[k].used) {
                PipeSlot *s = pipe_slot(old[k].key);
                *s = old[k];
                npipes++;
            }
        free(old);
    }
}

/* ---- descriptor sets: one for each set of views, kept across frames until a view or buffer is made again ---- */
typedef struct { VkImageView v[RR_TEX_SLOTS]; VkDescriptorSet s; int used; } SetSlot;
static SetSlot *sets;
static size_t nsets, cap_sets;
static VkDescriptorPool *pools;
static size_t npools, cap_pools, pool_at;
static uint64_t sets_gen;

static uint64_t views_hash(const VkImageView *v)
{
    uint64_t h = 0;
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        h = mix64(h ^ (uint64_t)(uintptr_t)v[k] ^ (uint64_t)k << 56);
    return h;
}

/* every set let go (at a frame's start, the frame before finished) */
static void sets_reset(void)
{
    size_t k;
    for (k = 0; k < npools; k++)
        vkResetDescriptorPool(dev, pools[k], 0);
    pool_at = 0;
    if (sets)
        memset(sets, 0, cap_sets * sizeof *sets);
    nsets = 0;
    sets_gen = views_gen;
}

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
            if (npools == cap_pools) {
                VkDescriptorPool *n = (VkDescriptorPool *)realloc(pools, (cap_pools ? cap_pools * 2 : 8) * sizeof *n);
                if (!n)
                    exit(70);
                pools = n;
                cap_pools = cap_pools ? cap_pools * 2 : 8;
            }
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

/* the view binding `slot` reads: a texture of another kind than the binding's
   (colour, integer, array of colour) reads as none, said once a frame */
static VkImageView slot_view(int32_t t, int slot)
{
    static uint64_t said;
    const Image *r = NULL;
    if (t == RR_TEX_FRAME)
        r = &screen_res[0];
    else if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        r = &screen_res[t == RR_TEX_EXTRA0 ? 1 : 2];
    else if (t == RR_TEX_PICTURE)
        r = &picture;
    else if (t <= RR_TEX_SIDE0 && t > RR_TEX_SIDE0 - RR_SIDES)
        r = &screen_res[3 + (RR_TEX_SIDE0 - t)];
    else if (t > 0 && t < MAX_TEXTURES)
        r = &tex[t];
    if (r && r->v) {
        int integer = r->format != RR_BGRA8;
        if (slot < 3 || slot >= 9 ? !r->array && !integer : slot < 6 ? !r->array && integer : r->array && !integer)
            return r->v;
        if (said != frame_no) {
            said = frame_no;
            fprintf(stderr, "rr: vulkan: texture %d is of another kind than binding %d reads: read as none\n", (int)t, slot + 2);
        }
    }
    return slot >= 9 || slot < 3 ? none_colour.v : slot >= 6 ? none_array.v : none_index.v;
}

/* the set holding these views (and the frame's buffers) */
static VkDescriptorSet set_of_views(const VkImageView *v)
{
    VkDescriptorBufferInfo ub = { ubuf.b, 0, 256 }, db = { dbuf.b, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo ii[RR_TEX_SLOTS];
    VkWriteDescriptorSet w[2 + RR_TEX_SLOTS];
    SetSlot *e;
    size_t i;
    int k;
    if ((nsets + 1) * 2 > cap_sets) {
        size_t c = cap_sets ? cap_sets * 2 : 256, j;
        SetSlot *n = (SetSlot *)calloc(c, sizeof *n);
        if (!n)
            exit(70);
        for (j = 0; j < cap_sets; j++)
            if (sets[j].used) {
                for (i = views_hash(sets[j].v) & (c - 1); n[i].used; i = (i + 1) & (c - 1))
                    ;
                n[i] = sets[j];
            }
        free(sets);
        sets = n;
        cap_sets = c;
    }
    for (i = views_hash(v) & (cap_sets - 1); sets[i].used; i = (i + 1) & (cap_sets - 1))
        if (!memcmp(sets[i].v, v, sizeof sets[i].v))
            return sets[i].s;
    e = &sets[i];
    e->used = 1;
    memcpy(e->v, v, sizeof e->v);
    e->s = set_alloc();
    nsets++;
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
        ii[k].sampler = k == 1 || k == 6 ? filtered : k == 2 || (k >= 7 && k < 9) ? bordered : sampler;
        ii[k].imageView = v[k];
        ii[k].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        w[2 + k].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2 + k].pImageInfo = &ii[k];
    }
    vkUpdateDescriptorSets(dev, 2 + RR_TEX_SLOTS, w, 0, NULL);
    return e->s;
}

static VkDescriptorSet set_of(const int32_t *t)
{
    VkImageView v[RR_TEX_SLOTS];
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        v[k] = slot_view(t ? t[k] : 0, k);
    return set_of_views(v);
}

/* ---- the window ---- */
static int make_surface(void *window)
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
    si.hwnd = window;
    return si.hwnd && fn && fn(inst, &si, NULL, &surface) == VK_SUCCESS;
#elif defined(__ANDROID__)
    typedef struct { VkStructureType s; const void *n; VkFlags f; void *win; } Si;
    typedef VkResult (VKAPI_PTR *Fn)(VkInstance, const Si *, const VkAllocationCallbacks *, VkSurfaceKHR *);
    Fn fn = (Fn)vkGetInstanceProcAddr(inst, "vkCreateAndroidSurfaceKHR");
    Si si;
    memset(&si, 0, sizeof si);
    si.s = (VkStructureType)1000008000; /* VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR */
    si.win = window;
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
    si.win = (unsigned long)window;
    return si.dpy && si.win && fn && fn(inst, &si, NULL, &surface) == VK_SUCCESS;
#else
    return 0;
#endif
}

/* the swapchain's images' views and framebuffers let go */
static void swapchain_views_free(void)
{
    uint32_t i;
    for (i = 0; i < sc_n; i++) {
        vkDestroyFramebuffer(dev, sc_fb[i], NULL);
        vkDestroyImageView(dev, sc_view[i], NULL);
    }
    sc_n = 0;
    image_free(&win_ms);
    image_free(&win_ds);
}

/* the window let go: its swapchain and surface, once the GPU is done with them */
static void window_drop(void)
{
    if (!surface && !swapchain) {
        sc_window = NULL;
        return;
    }
    vkDeviceWaitIdle(dev);
    swapchain_views_free();
    if (swapchain)
        vkDestroySwapchainKHR(dev, swapchain, NULL);
    swapchain = VK_NULL_HANDLE;
    if (surface)
        vkDestroySurfaceKHR(inst, surface, NULL);
    surface = VK_NULL_HANDLE;
    sc_window = NULL;
    sc_ok = sc_failed = sc_lost = 0;
    sc_w = sc_h = 0;
}

/* a surface on a window come (back): one this device's queue can show to,
   else none, and the window kept as the one tried, not tried again until
   another comes (or it goes and comes back). The tried window is given back
   as kept like one with a surface, so a host whose window goes waits for
   the renderer to forget it: one made again at the same address is tried
   anew */
static void window_make(void *window)
{
    VkBool32 ok = VK_FALSE;
    sc_window = window;
    if (!make_surface(window)) {
        fprintf(stderr, "rr: vulkan: no surface could be made on the window: nothing shown until another comes\n");
        return;
    }
    if (vkGetPhysicalDeviceSurfaceSupportKHR(phys, family, surface, &ok) != VK_SUCCESS || !ok) {
        fprintf(stderr, "rr: vulkan: the window cannot be shown from this device's queue: nothing shown until another comes\n");
        vkDestroySurfaceKHR(inst, surface, NULL);
        surface = VK_NULL_HANDLE;
    }
}

/* the surface's format: 8-bit BGRA or RGBA without sRGB encoding, in the
   sRGB colour space, where it offers one (the picture's values are shown as
   they are); else such a format in another colour space; else its first */
static VkSurfaceFormatKHR surface_format(void)
{
    VkSurfaceFormatKHR fs[64], best;
    uint32_t n = 64, i;
    int rank = 1 << 30;
    memset(&best, 0, sizeof best);
    best.format = COLOUR;
    best.colorSpace = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
    if (vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &n, fs) < 0 || !n)
        return best;
    if (n == 1 && fs[0].format == VK_FORMAT_UNDEFINED) {
        best.colorSpace = fs[0].colorSpace;
        return best;
    }
    for (i = 0; i < n; i++) {
        int r = fs[i].format == VK_FORMAT_B8G8R8A8_UNORM ? 0 : fs[i].format == VK_FORMAT_R8G8B8A8_UNORM ? 1
              : fs[i].format == VK_FORMAT_A8B8G8R8_UNORM_PACK32 ? 2 : 100 + (int)i;
        if (fs[i].colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR && r < 100)
            r += 10;
        if (r < rank) {
            rank = r;
            best = fs[i];
        }
    }
    return best;
}

/* the swapchain for a window of w x h pixels, at the size the surface allows
   (its current extent when it has one, else w x h within its limits): 0, with
   nothing waited for or changed, when that size is nothing (minimised). Where
   the display is turned from its own orientation (the surface's transform),
   the images are made in the display's orientation and the picture is turned
   into them when shown */
static int swapchain_make(int w, int h)
{
    VkSurfaceCapabilitiesKHR c;
    VkSwapchainCreateInfoKHR ci = { VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR };
    VkSwapchainKHR old = swapchain;
    VkSurfaceFormatKHR sf;
    VkSurfaceTransformFlagBitsKHR tr;
    VkResult e;
    VkSwapchainKHR made_sc = VK_NULL_HANDLE;
    uint32_t i, n, made;
    int iw, ih, turns, mirror;
    if ((e = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &c)) != VK_SUCCESS) {
        if (e == VK_ERROR_DEVICE_LOST)
            fail("vkGetPhysicalDeviceSurfaceCapabilitiesKHR (the GPU was lost)", e);
        fprintf(stderr, "rr: vulkan: the window's surface could not be asked its sizes (VkResult %d): nothing shown until it changes\n", (int)e);
        sc_failed = 1;
        sc_lost = e == VK_ERROR_SURFACE_LOST_KHR;
        return 0;
    }
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
    /* the turn (and mirror) the display needs, given the picture as it goes into
       the images; a transform that is neither (inherited) is the presentation
       engine's, the picture given as it is */
    tr = c.currentTransform;
    switch (tr) {
    case VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR: turns = 1; mirror = 0; break;
    case VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR: turns = 2; mirror = 0; break;
    case VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR: turns = 3; mirror = 0; break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_BIT_KHR: turns = 0; mirror = 1; break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_90_BIT_KHR: turns = 1; mirror = 1; break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_180_BIT_KHR: turns = 2; mirror = 1; break;
    case VK_SURFACE_TRANSFORM_HORIZONTAL_MIRROR_ROTATE_270_BIT_KHR: turns = 3; mirror = 1; break;
    default:
        turns = mirror = 0;
        if (tr != VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR && (c.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR))
            tr = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
        break;
    }
    /* the current extent is in the display's turned orientation, the picture's: the images are in its own */
    iw = turns & 1 ? h : w;
    ih = turns & 1 ? w : h;
    if ((uint32_t)iw < c.minImageExtent.width) iw = (int)c.minImageExtent.width;
    if ((uint32_t)iw > c.maxImageExtent.width) iw = (int)c.maxImageExtent.width;
    if ((uint32_t)ih < c.minImageExtent.height) ih = (int)c.minImageExtent.height;
    if ((uint32_t)ih > c.maxImageExtent.height) ih = (int)c.maxImageExtent.height;
    vkDeviceWaitIdle(dev);
    n = c.minImageCount + 1;
    if (c.maxImageCount && n > c.maxImageCount)
        n = c.maxImageCount;
    sf = surface_format();
    if (sf.format != sc_format || !rp_window) {
        /* the window's render pass, and the pipelines drawing into it, for this format */
        pipes_drop_window();
        if (rp_window)
            vkDestroyRenderPass(dev, rp_window, NULL);
        rp_window = render_pass(sf.format, pass_samples, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
        sc_format = sf.format;
    }
    ci.surface = surface;
    ci.minImageCount = n;
    ci.imageFormat = sf.format;
    ci.imageColorSpace = sf.colorSpace;
    ci.imageExtent.width = (uint32_t)iw;
    ci.imageExtent.height = (uint32_t)ih;
    ci.imageArrayLayers = 1;
    ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    ci.preTransform = tr;
    /* opaque where the surface allows it, else what it takes; the picture's alpha is 1 */
    ci.compositeAlpha = c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR ? VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR
                      : c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR ? VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR
                      : c.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR ? VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR
                                                                                               : VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR;
    ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    ci.clipped = VK_TRUE;
    ci.oldSwapchain = old;
    e = vkCreateSwapchainKHR(dev, &ci, NULL, &made_sc);
    /* the old one is retired either way */
    swapchain_views_free();
    if (old)
        vkDestroySwapchainKHR(dev, old, NULL);
    swapchain = VK_NULL_HANDLE;
    if (e != VK_SUCCESS) {
        if (e == VK_ERROR_DEVICE_LOST)
            fail("vkCreateSwapchainKHR (the GPU was lost)", e);
        fprintf(stderr, "rr: vulkan: no swapchain for a window of %d x %d (VkResult %d): nothing shown until it changes\n", w, h, (int)e);
        sc_failed = 1;
        sc_lost = e == VK_ERROR_SURFACE_LOST_KHR;   /* a window in use stays so: tried again when its size changes */
        return 0;
    }
    swapchain = made_sc;
    /* as many images as it made, each with its views and its semaphore */
    made = 0;
    vkGetSwapchainImagesKHR(dev, swapchain, &made, NULL);
    if (made > sc_cap) {
        VkSemaphoreCreateInfo se = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        VkSemaphore *ns = (VkSemaphore *)realloc(sc_done, made * sizeof *ns);
        if (!ns)
            exit(70);
        sc_done = ns;
        for (i = sc_cap; i < made; i++)
            if ((e = vkCreateSemaphore(dev, &se, NULL, &sc_done[i])) != VK_SUCCESS)
                fail("vkCreateSemaphore", e);
        sc_cap = made;
    }
    free(sc_img);
    free(sc_view);
    free(sc_fb);
    sc_img = (VkImage *)calloc(made ? made : 1, sizeof *sc_img);
    sc_view = (VkImageView *)calloc(made ? made : 1, sizeof *sc_view);
    sc_fb = (VkFramebuffer *)calloc(made ? made : 1, sizeof *sc_fb);
    if (!sc_img || !sc_view || !sc_fb)
        exit(70);
    sc_n = made;
    vkGetSwapchainImagesKHR(dev, swapchain, &sc_n, sc_img);
    /* the window's multisampled colour and depth, never kept past a pass */
    image_make(&win_ms, sf.format, iw, ih, pass_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT);
    image_make(&win_ds, depth_format, iw, ih, pass_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    for (i = 0; i < sc_n; i++) {
        VkImageViewCreateInfo vi = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        vi.image = sc_img[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = sf.format;
        vi.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.levelCount = vi.subresourceRange.layerCount = 1;
        if ((e = vkCreateImageView(dev, &vi, NULL, &sc_view[i])) != VK_SUCCESS)
            fail("vkCreateImageView (the window's)", e);
        {
            VkImageView v[3];
            v[0] = win_ms.v;
            v[1] = win_ds.v;
            v[2] = sc_view[i];
            sc_fb[i] = framebuffer(rp_window, v, 3, iw, ih);
        }
    }
    sc_ew = turns & 1 ? ih : iw;
    sc_eh = turns & 1 ? iw : ih;
    sc_iw = iw;
    sc_ih = ih;
    sc_turns = turns;
    sc_mirror = mirror;
    return 1;
}

/* the window went or came between frames: a surface on one that went let go now */
void rr_window(void)
{
    void *wnd;
    if (!dev || !windowed)
        return;
    wnd = host_window_take(NULL, NULL);
    if (wnd != sc_window)
        window_drop();
    host_window_give(sc_window);
}

/* ---- opening ---- */
static VkPhysicalDeviceFeatures want_features;

static int depth_ok(VkFormat f)
{
    VkFormatProperties p;
    vkGetPhysicalDeviceFormatProperties(phys, f, &p);
    return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) != 0;
}

/* a step of opening failed: whatever was made, let go */
static int open_failed(void)
{
    rr_close();
    return 0;
}

int rr_open(void)
{
    VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO };
    VkInstanceCreateInfo ii = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO };
    const char *iext[2] = { "VK_KHR_surface", NULL };
    void *window;
    int k;
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
    /* drawn on a window even when none is there now (Android's comes and
       goes): one is made a surface whenever it comes */
    windowed = host_windowed();
    {
        const char *e = getenv("RR_DRAW");
        draw_all = e && *e && strcmp(e, "0");
    }
    app.apiVersion = VK_API_VERSION_1_0;
    ii.pApplicationInfo = &app;
    ii.enabledExtensionCount = windowed && iext[1] ? 2 : 0;
    ii.ppEnabledExtensionNames = iext;
    if (vkCreateInstance(&ii, NULL, &inst) != VK_SUCCESS)
        return 0;
#define X(f) f = (PFN_##f)vkGetInstanceProcAddr(inst, #f);
    VK_FUNCS(X)
#undef X
    if (windowed) {
        /* the surface the device is chosen for, where a window is there now
           (held while it is made); without one, a device that can draw, which
           can show too where windows come and go (every Android device's
           queues can show to any window) */
        window = host_window_take(NULL, NULL);
        if (window && make_surface(window))
            sc_window = window;
        host_window_give(surface ? sc_window : NULL);
        if (window && !surface)
            return open_failed();
    }
    if (vkEnumeratePhysicalDevices(inst, &n, devs) < 0 || !n)
        return open_failed();
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
        return open_failed();
    depth_format = depth_ok(VK_FORMAT_D24_UNORM_S8_UINT) ? VK_FORMAT_D24_UNORM_S8_UINT
                 : depth_ok(VK_FORMAT_D32_SFLOAT_S8_UINT) ? VK_FORMAT_D32_SFLOAT_S8_UINT : VK_FORMAT_UNDEFINED;
    if (depth_format == VK_FORMAT_UNDEFINED) {
        fprintf(stderr, "rr: vulkan: no depth and stencil format\n");
        return open_failed();
    }
    {
        /* anisotropic filtering where the GPU has it */
        VkPhysicalDeviceFeatures have;
        vkGetPhysicalDeviceFeatures(phys, &have);
        memset(&want_features, 0, sizeof want_features);
        want_features.samplerAnisotropy = have.samplerAnisotropy;
        di.pEnabledFeatures = &want_features;
    }
    qi.queueFamilyIndex = family;
    qi.queueCount = 1;
    qi.pQueuePriorities = &prio;
    di.queueCreateInfoCount = 1;
    di.pQueueCreateInfos = &qi;
    di.enabledExtensionCount = windowed ? 1 : 0;
    di.ppEnabledExtensionNames = &dext;
    if (vkCreateDevice(phys, &di, NULL, &dev) != VK_SUCCESS)
        return open_failed();
    vkGetDeviceQueue(dev, family, 0, &queue);

    {
        /* the samples the screens have: the counts the device fills for the colour and depth-stencil formats, 8 if it has them, else 4 */
        VkPhysicalDeviceProperties pp;
        VkImageFormatProperties fp;
        VkSampleCountFlags have;
        vkGetPhysicalDeviceProperties(phys, &pp);
        have = pp.limits.framebufferColorSampleCounts & pp.limits.framebufferDepthSampleCounts & pp.limits.framebufferStencilSampleCounts;
        if (vkGetPhysicalDeviceImageFormatProperties(phys, COLOUR, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, 0, &fp) ==
            VK_SUCCESS)
            have &= fp.sampleCounts;
        if (vkGetPhysicalDeviceImageFormatProperties(phys, depth_format, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0, &fp) == VK_SUCCESS)
            have &= fp.sampleCounts;
        samples_have = (int)have | 1;
        pass_samples = samples_have & 8 ? 8 : samples_have & 4 ? 4 : samples_have & 2 ? 2 : 1;
    }
    rp_screen = render_pass(COLOUR, pass_samples, 1, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    for (k = 0; k < 2; k++) {
        VkAttachmentLoadOp op = k ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR;
        VkImageLayout from = k ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED;
        rp_colour[k] = render_pass(COLOUR, 1, 0, op, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rp_index[k] = render_pass(VK_FORMAT_R8_UINT, 1, 0, op, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        rp_index32[k] = render_pass(VK_FORMAT_R32_UINT, 1, 0, op, from, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    }
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
            return open_failed();
        pi.setLayoutCount = 1;
        pi.pSetLayouts = &set_layout;
        if (vkCreatePipelineLayout(dev, &pi, NULL, &layout) != VK_SUCCESS)
            return open_failed();
        si.magFilter = si.minFilter = VK_FILTER_NEAREST;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(dev, &si, NULL, &sampler) != VK_SUCCESS)
            return open_failed();
        {
            VkPhysicalDeviceProperties pp;
            vkGetPhysicalDeviceProperties(phys, &pp);
            si.magFilter = si.minFilter = VK_FILTER_LINEAR;
            si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            si.maxLod = VK_LOD_CLAMP_NONE;
            si.anisotropyEnable = want_features.samplerAnisotropy;
            si.maxAnisotropy = pp.limits.maxSamplerAnisotropy < 16 ? pp.limits.maxSamplerAnisotropy : 16;
            if (!si.anisotropyEnable)
                si.maxAnisotropy = 1;
            if (vkCreateSampler(dev, &si, NULL, &filtered) != VK_SUCCESS)
                return open_failed();
            si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
            si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
            if (vkCreateSampler(dev, &si, NULL, &bordered) != VK_SUCCESS)
                return open_failed();
        }
    }
    for (k = 0; k < RR_PROGRAMS; k++) {
        vert_mod[k] = shader(programs[k].vs, programs[k].vs_size);
        frag_mod[k] = shader(programs[k].fs, programs[k].fs_size);
        if (!vert_mod[k] || !frag_mod[k])
            return open_failed();
    }
    {
        VkCommandPoolCreateInfo ci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        VkSemaphoreCreateInfo se = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        ci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        ci.queueFamilyIndex = family;
        if (vkCreateCommandPool(dev, &ci, NULL, &cmd_pool) != VK_SUCCESS)
            return open_failed();
        ai.commandPool = cmd_pool;
        ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ai.commandBufferCount = 1;
        if (vkAllocateCommandBuffers(dev, &ai, &cmd) != VK_SUCCESS)
            return open_failed();
        fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vkCreateFence(dev, &fi, NULL, &fence);
        vkCreateSemaphore(dev, &se, NULL, &sem_acquire);
    }
    image_make(&none_colour, COLOUR, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    image_make(&none_index, VK_FORMAT_R8_UINT, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    image_make_layers(&none_array, COLOUR, 1, 1, 1, 1, 1, 1, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    return 1;
}

static void screen_free(int k)
{
    if (screen_fb[k])
        vkDestroyFramebuffer(dev, screen_fb[k], NULL);
    screen_fb[k] = VK_NULL_HANDLE;
    image_free(&screen_ms[k]);
    image_free(&screen_ds[k]);
    image_free(&screen_res[k]);
    screen_w[k] = screen_h[k] = screen_samp[k] = 0;
}

void rr_close(void)
{
    size_t i;
    int k;
    if (!dev)
        goto instance;
    vkDeviceWaitIdle(dev);
    window_drop();
    for (i = 0; i < cap_pipes; i++)
        if (pipes[i].used && pipes[i].p)
            vkDestroyPipeline(dev, pipes[i].p, NULL);
    free(pipes);
    pipes = NULL;
    cap_pipes = npipes = 0;
    for (i = 0; i < npools; i++)
        vkDestroyDescriptorPool(dev, pools[i], NULL);
    free(pools);
    free(sets);
    pools = NULL;
    sets = NULL;
    npools = cap_pools = cap_sets = nsets = 0;
    for (k = 0; k < MAX_TEXTURES; k++)
        if (tex[k].made)
            image_free(&tex[k]);
    for (k = 0; k < SCREENS; k++)
        screen_free(k);
    sink_free();
    image_free(&picture);
    image_free(&none_colour);
    image_free(&none_index);
    image_free(&none_array);
    buf_free(&ubuf);
    buf_free(&dbuf);
    buf_free(&staging);
    buf_free(&readback);
    for (k = 0; k < RR_PROGRAMS; k++) {
        if (vert_mod[k])
            vkDestroyShaderModule(dev, vert_mod[k], NULL);
        if (frag_mod[k])
            vkDestroyShaderModule(dev, frag_mod[k], NULL);
    }
    for (k = 0; k < MAX_SHADERS; k++)
        if (game_mods[k])
            vkDestroyShaderModule(dev, game_mods[k], NULL);
    for (i = 0; i < sc_cap; i++)
        vkDestroySemaphore(dev, sc_done[i], NULL);
    free(sc_done);
    free(sc_img);
    free(sc_view);
    free(sc_fb);
    sc_done = NULL;
    sc_img = NULL;
    sc_view = NULL;
    sc_fb = NULL;
    sc_n = sc_cap = 0;
    swapchain = VK_NULL_HANDLE;
    if (sem_acquire)
        vkDestroySemaphore(dev, sem_acquire, NULL);
    if (fence)
        vkDestroyFence(dev, fence, NULL);
    if (cmd_pool)
        vkDestroyCommandPool(dev, cmd_pool, NULL);
    vkDestroyRenderPass(dev, rp_screen, NULL);
    for (k = 0; k < 5; k++)
        if (rp_side[k])
            vkDestroyRenderPass(dev, rp_side[k], NULL);
    for (k = 0; k < 2; k++) {
        vkDestroyRenderPass(dev, rp_colour[k], NULL);
        vkDestroyRenderPass(dev, rp_index[k], NULL);
        vkDestroyRenderPass(dev, rp_index32[k], NULL);
    }
    if (rp_window)
        vkDestroyRenderPass(dev, rp_window, NULL);
    vkDestroySampler(dev, sampler, NULL);
    vkDestroySampler(dev, filtered, NULL);
    vkDestroySampler(dev, bordered, NULL);
    vkDestroyPipelineLayout(dev, layout, NULL);
    vkDestroyDescriptorSetLayout(dev, set_layout, NULL);
    vkDestroyDevice(dev, NULL);
    dev = VK_NULL_HANDLE;
instance:
    if (surface)
        vkDestroySurfaceKHR(inst, surface, NULL);
    surface = VK_NULL_HANDLE;
    if (windowed)
        host_window_give(NULL);         /* nothing kept on the window */
    if (inst)
        vkDestroyInstance(inst, NULL);
    inst = VK_NULL_HANDLE;
    /* everything as before the first rr_open, so another opens soundly */
    phys = VK_NULL_HANDLE;
    queue = VK_NULL_HANDLE;
    family = 0;
    depth_format = VK_FORMAT_UNDEFINED;
    sc_format = VK_FORMAT_UNDEFINED;
    sc_window = NULL;
    sc_ok = sc_failed = sc_lost = sc_mirror = sc_turns = 0;
    sc_w = sc_h = sc_ew = sc_eh = sc_iw = sc_ih = 0;
    windowed = 0;
    rp_screen = rp_window = VK_NULL_HANDLE;
    memset(rp_side, 0, sizeof rp_side);
    pass_samples = samples_have = draw_all = 0;
    memset(rp_colour, 0, sizeof rp_colour);
    memset(rp_index, 0, sizeof rp_index);
    memset(rp_index32, 0, sizeof rp_index32);
    set_layout = VK_NULL_HANDLE;
    layout = VK_NULL_HANDLE;
    sampler = filtered = bordered = VK_NULL_HANDLE;
    cmd_pool = VK_NULL_HANDLE;
    cmd = VK_NULL_HANDLE;
    fence = VK_NULL_HANDLE;
    sem_acquire = VK_NULL_HANDLE;
    memset(vert_mod, 0, sizeof vert_mod);
    memset(frag_mod, 0, sizeof frag_mod);
    memset(game_mods, 0, sizeof game_mods);
    memset(screen_fb, 0, sizeof screen_fb);
    memset(screen_used, 0, sizeof screen_used);
    memset(screen_samp, 0, sizeof screen_samp);
    pool_at = 0;
    sets_gen = 0;
    views_gen = 1;
    last_w = last_h = 0;
    picture_used = 0;
    last_frame = NULL;
    last_aspect = 0;
    ever = 0;
    frame_no = 0;
}

const char *rr_name(void) { return "vulkan"; }
int rr_samples(void) { return pass_samples ? pass_samples : 4; }
int rr_samples_at_most(int n)
{
    int c;
    for (c = 16; c > 1; c /= 2)
        if (c <= n && (samples_have & c))
            return c;
    return 1;
}
const char *rr_shader_ext(void) { return "spv"; }

static VkFormat format_of(int f) { return f == RR_R8U ? VK_FORMAT_R8_UINT : f == RR_R32U ? VK_FORMAT_R32_UINT : COLOUR; }

/* ---- a frame ---- */

/* a screen target (0 the frame, 1 and 2 the extra ones, 3 and on the side targets) of w x h and `samples`: its
   multisampled colour and depth and stencil are never kept past the pass; at one sample, drawn straight into its image */
static void screen_fit(int k, int w, int h, int samples)
{
    VkImageView v[3];
    if (screen_w[k] == w && screen_h[k] == h && screen_samp[k] == samples)
        return;
    screen_free(k);
    if (samples > 1)
        image_make(&screen_ms[k], COLOUR, w, h, samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
    image_make(&screen_ds[k], depth_format, w, h, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    image_make(&screen_res[k], COLOUR, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    screen_res[k].format = RR_BGRA8;
    if (samples > 1) {
        v[0] = screen_ms[k].v;
        v[1] = screen_ds[k].v;
        v[2] = screen_res[k].v;
        screen_fb[k] = framebuffer(k < 3 ? rp_screen : side_pass(samples), v, 3, w, h);
    } else {
        v[0] = screen_res[k].v;
        v[1] = screen_ds[k].v;
        screen_fb[k] = framebuffer(side_pass(1), v, 2, w, h);
    }
    screen_w[k] = w;
    screen_h[k] = h;
    screen_samp[k] = samples;
}

/* the screen (screen_ms's index) a pass target is: the frame, the extra targets, a side target; -1 none (the window's
   is made with the swapchain) */
static int screen_at(const rr_pass *ps)
{
    return ps->target <= RR_TARGET_EXTRA1 ? (int)ps->target : ps->target == RR_TARGET_SIDE ? 3 + (int)(ps->layer % RR_SIDES) : -1;
}

/* the samples a pixel of what a pass draws into has */
static int pass_n(const rr_pass *ps)
{
    return ps->target <= RR_TARGET_EXTRA1 || ps->target == RR_TARGET_WINDOW ? pass_samples : ps->target == RR_TARGET_SIDE ? (int)ps->samples : 1;
}

/* the window-sized targets the passes draw into, made at this size, and only
   while passes use them (`others`: the screens', the side targets' and the
   picture's passes run; the window's own are made with the swapchain); one
   not drawn into is let go when long unused, or no longer the window's size,
   and reads of it read nothing */
static void targets_fit(const rr_frame *f, int w, int h, int others)
{
    int k, pic = 0, used[SCREENS] = { 0 }, count[SCREENS] = { 0 };
    size_t p;
    for (p = 0; others && p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        int sc = screen_at(ps);
        if (sc >= 0) {
            used[sc] = 1;
            count[sc] = pass_n(ps);
        } else if (ps->target == RR_TARGET_PICTURE) {
            pic = 1;
        }
    }
    for (k = 0; k < SCREENS; k++) {
        if (used[k]) {
            screen_fit(k, w, h, count[k]);
            screen_used[k] = frame_no;
        } else if (screen_w[k] && (frame_no - screen_used[k] > UNUSED_FRAMES || screen_w[k] != w || screen_h[k] != h)) {
            /* not drawn into this frame: let go when long unused, or when no longer the window's size */
            screen_free(k);
        }
    }
    if (pic) {
        if (!picture.i || picture.w != w || picture.h != h) {
            image_make(&picture, COLOUR, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            picture.f = framebuffer(rp_colour[0], &picture.v, 1, w, h);
            picture.format = RR_BGRA8;
        }
        picture_used = frame_no;
    } else if (picture.i && (frame_no - picture_used > UNUSED_FRAMES || picture.w != w || picture.h != h)) {
        image_free(&picture);
    }
}

/* the frame's textures made, before anything reads their sizes; to be
   made readable, and their writes, once the frame is being recorded */
static void make_textures(const rr_frame *f)
{
    size_t i;
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        Image *t;
        if (o->op != RR_TEX_CREATE)
            continue;
        if (o->id >= MAX_TEXTURES) {
            fprintf(stderr, "rr: vulkan: texture %u past the most\n", o->id);
            continue;
        }
        t = &tex[o->id];
        {
            VkImageUsageFlags u = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            int levels = o->flags & RR_TEX_MIPS && o->format == RR_BGRA8 ? levels_of((int)o->w, (int)o->h) : 1, k;
            int array = (o->flags & RR_TEX_ARRAY) != 0, layers = array && o->layers ? (int)o->layers : 1;
            if (o->flags & RR_TEX_TARGET)
                u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
            image_make_layers(t, format_of(o->format), (int)o->w, (int)o->h, 1, levels, layers, array, u, VK_IMAGE_ASPECT_COLOR_BIT);
            t->format = o->format;
            t->target = (o->flags & RR_TEX_TARGET) != 0;
            if (t->target) {
                VkRenderPass rp = o->format == RR_R8U ? rp_index[0] : o->format == RR_R32U ? rp_index32[0] : rp_colour[0];
                if (array)
                    for (k = 0; k < layers; k++)
                        t->lf[k] = framebuffer(rp, &t->lv[k], 1, (int)o->w, (int)o->h);
                else
                    t->f = framebuffer(rp, &t->fv, 1, (int)o->w, (int)o->h);
                if (levels > 1)
                    mip_views(t, format_of(o->format), rp);     /* colour only: RR_TEX_MIPS */
            }
        }
    }
}

/* the frame's texture operations by texture, each texture's in their order */
static const rr_tex_op *sort_ops;
static int cmp_op(const void *a, const void *b)
{
    size_t x = *(const size_t *)a, y = *(const size_t *)b;
    if (sort_ops[x].id != sort_ops[y].id)
        return sort_ops[x].id < sort_ops[y].id ? -1 : 1;
    return x < y ? -1 : x > y;
}

/* each texture made this frame readable from the start (unwritten texels are
   what they are); then each texture's writes since it was last made, together:
   one transition of the level and layer written into, every copy, one back */
static void record_texops(const rr_frame *f)
{
    static size_t *order;
    static size_t cap_order;
    size_t i, j, n = 0;
    if (cap_order < f->ntexops) {
        free(order);
        cap_order = f->ntexops * 2;
        order = (size_t *)malloc(cap_order * sizeof *order);
        if (!order)
            exit(70);
    }
    for (i = 0; i < f->ntexops; i++)
        if (f->texops[i].id < MAX_TEXTURES && tex[f->texops[i].id].i)
            order[n++] = i;
    sort_ops = f->texops;
    qsort(order, n, sizeof *order, cmp_op);
    for (i = 0; i < n; i = j) {
        uint32_t id = f->texops[order[i]].id;
        Image *t = &tex[id];
        size_t first = i, k;
        int writes = 0;
        for (j = i; j < n && f->texops[order[j]].id == id; j++)
            if (f->texops[order[j]].op == RR_TEX_CREATE)
                first = j;          /* writes before the last making went to the texture it replaced */
        if (f->texops[order[first]].op == RR_TEX_CREATE) {
            clear_new(t->i);            /* reads 0 until written */
            first++;
        }
        for (k = first; k < j; k++) {
            const rr_tex_op *o = &f->texops[order[k]];
            VkBufferImageCopy c;
            if ((uint64_t)o->x + o->w > (uint64_t)t->w || (uint64_t)o->y + o->h > (uint64_t)t->h || !o->w || !o->h) {
                fprintf(stderr, "rr: vulkan: a write past texture %u's edge left out\n", id);
                continue;
            }
            if (!writes++)
                barrier_levels(t->i, 0, 0, 1, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                               VK_ACCESS_TRANSFER_WRITE_BIT, SHADER_STAGES | VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            memset(&c, 0, sizeof c);
            c.bufferOffset = o->at;
            c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            c.imageSubresource.layerCount = 1;
            c.imageOffset.x = (int32_t)o->x;
            c.imageOffset.y = (int32_t)o->y;
            c.imageExtent.width = o->w;
            c.imageExtent.height = o->h;
            c.imageExtent.depth = 1;
            vkCmdCopyBufferToImage(cmd, staging.b, t->i, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &c);
            t->fresh[0] = 0;            /* written: a pass that keeps what it held keeps this */
        }
        if (writes)
            barrier_levels(t->i, 0, 0, 1, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT,
                           VK_ACCESS_SHADER_READ_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, SHADER_STAGES);
    }
}

/* the work sent: a GPU that has been lost is said so, and the end */
static void submit(const VkSubmitInfo *si)
{
    VkResult r = vkQueueSubmit(queue, 1, si, fence);
    if (r != VK_SUCCESS)
        fail(r == VK_ERROR_DEVICE_LOST ? "vkQueueSubmit (the GPU was lost)" : "vkQueueSubmit", r);
}

/* the frame before finished: waited for a second at a time, the GPU found lost
   (or still not done after 10 seconds) said so, and the end, never a hang */
static void wait_frame(void)
{
    int k;
    for (k = 0; k < 10; k++) {
        VkResult r = vkWaitForFences(dev, 1, &fence, VK_TRUE, 1000000000ull);
        if (r == VK_SUCCESS)
            return;
        if (r != VK_TIMEOUT)
            fail(r == VK_ERROR_DEVICE_LOST ? "vkWaitForFences (the GPU was lost)" : "vkWaitForFences", r);
    }
    fail("vkWaitForFences (no frame finished in 10 seconds: the GPU is lost)", VK_TIMEOUT);
}

/* a window-sized target's size in the display's own orientation, given the picture's (ow x oh) and the quarter turns */
static int image_w(int ow, int oh, int turns) { return turns & 1 ? oh : ow; }
static int image_h(int ow, int oh, int turns) { return turns & 1 ? ow : oh; }

/* a pass's target: its kind (the window's: wkind), size (a window-sized target's in the display's orientation)
   and whether it can be drawn into */
static int pass_target(const rr_pass *ps, int ow, int oh, int turns, int wkind, int wok, int *kind, int *tw, int *th, Image **to)
{
    *to = NULL;
    *tw = image_w(ow, oh, turns);
    *th = image_h(ow, oh, turns);
    if (screen_at(ps) >= 0) {
        *kind = KIND_SCREEN;
        return screen_fb[screen_at(ps)] != VK_NULL_HANDLE;
    }
    if (ps->target == RR_TARGET_WINDOW) {
        *kind = wkind;
        return wok;
    }
    if (ps->target == RR_TARGET_TEXTURE) {
        Image *t;
        if (ps->texture >= MAX_TEXTURES)
            return 0;
        t = &tex[ps->texture];
        /* a texture not made to be drawn into, or a layer it has not, is no target */
        if (!t->target || (t->array ? ps->layer >= (uint32_t)t->layers || !t->lf || !t->lf[ps->layer] : !t->f))
            return 0;
        *to = t;
    } else {
        *to = &picture;
        if (!picture.i)
            return 0;
    }
    *kind = ps->target != RR_TARGET_TEXTURE ? KIND_COLOUR : (*to)->format == RR_R8U ? KIND_INDEX : (*to)->format == RR_R32U ? KIND_INDEX32 : KIND_COLOUR;
    *tw = (*to)->w;
    *th = (*to)->h;
    return 1;
}

/* a rectangle (x, y, w, h) of a pw x ph space in the picture's orientation as
   it lies in the display's own: the picture mirrored left to right if
   `mirror`, then turned `turns` quarter turns clockwise */
static void turn_rect(int *x, int *y, int *w, int *h, int pw, int ph, int turns, int mirror)
{
    int k;
    if (mirror)
        *x = pw - *x - *w;
    for (k = 0; k < turns; k++) {
        int nx = ph - *y - *h, ny = *x, nw = *h, nh = *w, t = pw;
        *x = nx;
        *y = ny;
        *w = nw;
        *h = nh;
        pw = ph;
        ph = t;
    }
}

/* each draw's block of the frame's passes given its target's size, samples and
   orientation code (rr.h): a window-sized target's size is the picture's, its
   code the turn and mirror; a texture target is never turned. `again`: a frame
   run again for reading: the textures' passes are left as they are, and, of an
   interlaced picture (`only_window`), the others' too; its window's pass shows
   the picture (turned as it was drawn) as it was before it was turned */
static void patch_uniforms(const rr_frame *f, int ow, int oh, int turns, int mirror, int again, int only_window, int was_turns, int was_mirror)
{
    size_t p, i;
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        Image *to;
        int kind, tw, th, wsized = ps->target != RR_TARGET_TEXTURE, samples = pass_n(ps);
        if (again && (ps->target == RR_TARGET_TEXTURE || (only_window && ps->target != RR_TARGET_WINDOW)))
            continue;
        pass_target(ps, ow, oh, turns, KIND_WINDOW, 1, &kind, &tw, &th, &to);
        for (i = ps->first; i < ps->first + ps->count; i++) {
            rr_uniform u = f->uniforms[f->cmds[i].uniform];
            u.f[0][0] = (float)(wsized ? ow : tw);
            u.f[0][1] = (float)(wsized ? oh : th);
            u.f[0][2] = (float)samples;
            u.f[0][3] = (float)(1 | (wsized ? turns << 1 | mirror << 3 : 0));
            if (again && f->cmds[i].program == RR_SHOW && ps->target == RR_TARGET_WINDOW) {
                /* the picture as drawn is the picture mirrored and turned: mirrored, then turned t more, or turned 4 - t, gives it back */
                u.i[0][0] = was_mirror ? was_turns : (4 - was_turns) & 3;
                u.i[0][1] = was_mirror;
            }
            memcpy((char *)ubuf.at + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
        }
    }
}

/* whether the frame has a pass into the window */
static int has_window(const rr_frame *f)
{
    size_t p;
    for (p = 0; p < f->npasses; p++)
        if (f->passes[p].target == RR_TARGET_WINDOW)
            return 1;
    return 0;
}

/* whether the frame has a pass into the picture target: an interlaced picture */
static int has_picture(const rr_frame *f)
{
    size_t p;
    for (p = 0; p < f->npasses; p++)
        if (f->passes[p].target == RR_TARGET_PICTURE)
            return 1;
    return 0;
}

/* the frame's passes recorded: the textures' (not when `again`, a frame being
   run again for reading: they are what they were), the window's when `window`
   (into framebuffer wfb of render pass wrp, its pipelines of kind wkind), and
   the other screens' and the picture's when `others` */
static void record_passes(const rr_frame *f, int ow, int oh, int turns, int mirror, float aspect, int window, int others, int again, VkFramebuffer wfb,
                          VkRenderPass wrp, int wkind)
{
    size_t i, p;
    int k;
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        VkRenderPassBeginInfo bi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
        VkClearValue clr[2];
        Image *to;
        int kind, tw, th, vx, vy, vw, vh, wsized = ps->target != RR_TARGET_TEXTURE;
        uint32_t layer = 0;
        if (ps->target == RR_TARGET_TEXTURE ? again : ps->target == RR_TARGET_WINDOW ? !window : !others)
            continue;
        if (!pass_target(ps, ow, oh, turns, wkind, wfb != VK_NULL_HANDLE, &kind, &tw, &th, &to)) {
            if (ps->count)
                fprintf(stderr, "rr: vulkan: a pass into no target (%u, texture %u, layer %u) left out\n", ps->target, ps->texture, ps->layer);
            continue;
        }
        /* the view: the frame's units shown at their aspect, centred in this target (a window-sized
           one in the picture's orientation, turned to the display's below); a window-sized target
           with no window and no RR_SIZE is the units themselves, filled */
        if (wsized)
            rr_view(aspect, ow, oh, &vx, &vy, &vw, &vh);
        else
            rr_view(f->aspect, tw, th, &vx, &vy, &vw, &vh);
        memset(clr, 0, sizeof clr);
        memcpy(clr[0].color.float32, ps->clear, sizeof ps->clear);
        if (kind == KIND_SCREEN || kind == KIND_WINDOW) {
            bi.renderPass = ps->target == RR_TARGET_WINDOW ? wrp : ps->target == RR_TARGET_SIDE ? side_pass(pass_n(ps)) : rp_screen;
            bi.framebuffer = ps->target == RR_TARGET_WINDOW ? wfb : screen_fb[screen_at(ps)];
            clr[1].depthStencil.depth = 1;
            clr[1].depthStencil.stencil = 0;
            bi.clearValueCount = 2;
        } else {
            int keep;
            layer = to->array ? ps->layer : 0;
            /* kept, unless just made: then cleared */
            keep = ps->load && !to->fresh[layer];
            to->fresh[layer] = 0;
            bi.renderPass = kind == KIND_INDEX ? rp_index[keep] : kind == KIND_INDEX32 ? rp_index32[keep] : rp_colour[keep];
            bi.framebuffer = to->array ? to->lf[layer] : to->f;
            bi.clearValueCount = 1;
            if (integer_kind(kind)) {
                clr[0].color.uint32[0] = (uint32_t)ps->clear[0];
                clr[0].color.uint32[1] = clr[0].color.uint32[2] = clr[0].color.uint32[3] = 0;
            }
        }
        bi.renderArea.extent.width = (uint32_t)tw;
        bi.renderArea.extent.height = (uint32_t)th;
        bi.pClearValues = clr;
        vkCmdBeginRenderPass(cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);
        for (i = ps->first; i < ps->first + ps->count; i++) {
            const rr_cmd *c = &f->cmds[i];
            int screen = kind == KIND_SCREEN || kind == KIND_WINDOW;
            VkPipeline pl = pipe_of(c->program, c->shader < MAX_SHADERS ? c->shader : 0, c->blend, screen ? c->depth : 0, screen ? c->stencil : 0, kind,
                                    screen ? pass_n(ps) : 1);
            VkViewport vp;
            VkRect2D sc;
            VkDescriptorSet set;
            uint32_t off = c->uniform * 256;
            int view = c->space == RR_SPACE_VIEW;
            if (!pl || !c->vertices || !c->instances)
                continue;
            /* in the picture's orientation (a window-sized target's), then turned to the display's */
            int px = view ? vx : 0, py = view ? vy : 0, pw = view ? vw : wsized ? ow : tw, ph = view ? vh : wsized ? oh : th;
            if (wsized)
                turn_rect(&px, &py, &pw, &ph, ow, oh, turns, mirror);
            vp.x = (float)px;
            vp.y = (float)py;
            vp.width = (float)pw;
            vp.height = (float)ph;
            vp.minDepth = 0;
            vp.maxDepth = 1;
            sc.offset.x = sc.offset.y = 0;
            sc.extent.width = (uint32_t)tw;
            sc.extent.height = (uint32_t)th;
            if (c->scissor) {
                /* the rectangle in the draw's own units: the frame's in view space, target pixels else */
                int x0, y0, x1, y1, sw = wsized ? ow : tw, sh = wsized ? oh : th;
                rr_scissor(f->uniforms[c->uniform].f[2], view ? f->width : sw, view ? f->height : sh, view ? vx : 0, view ? vy : 0,
                           view ? vw : sw, view ? vh : sh, &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                x1 -= x0;
                y1 -= y0;
                if (wsized)
                    turn_rect(&x0, &y0, &x1, &y1, ow, oh, turns, mirror);
                sc.offset.x = x0;
                sc.offset.y = y0;
                sc.extent.width = (uint32_t)x1;
                sc.extent.height = (uint32_t)y1;
            }
            set = set_of(c->tex);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &off);
            vkCmdSetViewport(cmd, 0, 1, &vp);
            vkCmdSetScissor(cmd, 0, 1, &sc);
            vkCmdSetStencilReference(cmd, VK_STENCIL_FRONT_AND_BACK, c->ref);
            vkCmdSetStencilCompareMask(cmd, VK_STENCIL_FRONT_AND_BACK, c->read_mask);
            vkCmdSetStencilWriteMask(cmd, VK_STENCIL_FRONT_AND_BACK, c->write_mask);
            vkCmdDraw(cmd, c->vertices, c->instances, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        if (to && ps->mips && to->levels > 1 && to->mv) {
            /* its smaller levels (that layer's), each texel the mean of the 2 x 2 of
               the level before, by the renderer's own program: the same box on
               every backend */
            VkPipeline pl = pipe_of(to->array ? RR_MIPS_LAYER : RR_MIPS, 0, RR_BLEND_REPLACE, 0, 0, KIND_COLOUR, 1);
            uint32_t off = (uint32_t)f->nuniforms * 256;
            int lv;
            for (lv = 1; pl && lv < to->levels; lv++) {
                VkRenderPassBeginInfo mb = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
                VkClearValue none;
                VkImageView v[RR_TEX_SLOTS];
                VkDescriptorSet set;
                uint32_t mw = (uint32_t)(to->w >> lv ? to->w >> lv : 1), mh = (uint32_t)(to->h >> lv ? to->h >> lv : 1);
                VkViewport vp = { 0, 0, (float)mw, (float)mh, 0, 1 };
                VkRect2D sc = { { 0, 0 }, { mw, mh } };
                memset(&none, 0, sizeof none);
                for (k = 0; k < RR_TEX_SLOTS; k++)
                    v[k] = k == 0 ? to->mv[(int)layer * to->levels + lv - 1] : slot_view(0, k);
                set = set_of_views(v);
                mb.renderPass = rp_colour[0];
                mb.framebuffer = to->mf[(int)layer * to->levels + lv];
                mb.renderArea = sc;
                mb.clearValueCount = 1;
                mb.pClearValues = &none;
                vkCmdBeginRenderPass(cmd, &mb, VK_SUBPASS_CONTENTS_INLINE);
                vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pl);
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, 1, &set, 1, &off);
                vkCmdSetViewport(cmd, 0, 1, &vp);
                vkCmdSetScissor(cmd, 0, 1, &sc);
                vkCmdDraw(cmd, 6, 1, 0, 0);
                vkCmdEndRenderPass(cmd);
            }
        }
    }
}

/* the image RR_DRAW's window pass resolves into, w x h, and what it is drawn with */
static void sink_free(void)
{
    if (draw_fb)
        vkDestroyFramebuffer(dev, draw_fb, NULL);
    draw_fb = VK_NULL_HANDLE;
    image_free(&draw_ms);
    image_free(&draw_ds);
    image_free(&draw_img);
}

static void sink_fit(int w, int h)
{
    VkImageView v[3];
    if (draw_img.i && draw_img.w == w && draw_img.h == h)
        return;
    sink_free();
    image_make(&draw_ms, COLOUR, w, h, pass_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_COLOR_BIT);
    image_make(&draw_ds, depth_format, w, h, pass_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
               VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
    image_make(&draw_img, COLOUR, w, h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    v[0] = draw_ms.v;
    v[1] = draw_ds.v;
    v[2] = draw_img.v;
    draw_fb = framebuffer(rp_screen, v, 3, w, h);
}

int rr_draw(rr_frame *f)
{
    int ow = f->width, oh = f->height, paced = 0, show = 0, others, k, turns = 0, mirror = 0, iw, ih, sink, window;
    float aspect = f->aspect;
    uint32_t image = 0;
    size_t i;
    VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

    wait_frame();
    frame_no++;
    for (i = 0; i < f->nshaders; i++) {
        /* kept: its pipelines are made as they are needed; its code is ours to let go */
        uint32_t id = f->shaders[i].id;
        if (id && id < MAX_SHADERS) {
            VkShaderModule m = shader(f->shaders[i].code, f->shaders[i].size);
            if (m) {
                if (game_mods[id])
                    vkDestroyShaderModule(dev, game_mods[id], NULL);
                game_mods[id] = m;
            } else {
                fprintf(stderr, "rr: vulkan: shader %u could not be made\n", id);
            }
        } else {
            fprintf(stderr, "rr: vulkan: shader %u past the most\n", id);
        }
        free((void *)f->shaders[i].code);
        f->shaders[i].code = NULL;
    }
    f->nshaders = 0;

    if (windowed) {
        /* the window this frame, held until the frame is shown: when it went or is another
           (Android's, after the app was away), its surface let go, and one made on a new one */
        int ww = 0, wh = 0;
        void *wnd = host_window_take(&ww, &wh);     /* and that window's size */
        if (wnd != sc_window || (sc_lost && wnd)) {
            window_drop();
            if (wnd)
                window_make(wnd);
        }
        if (surface) {
            /* made again for a new size or when it no longer fits; until it can be made, nothing is
               shown: a size the surface has none for (minimised) is only asked, a failure not tried
               again until the size changes */
            if (ww >= 1 && wh >= 1 && ((!sc_ok && !sc_failed) || ww != sc_w || wh != sc_h)) {
                sc_w = ww;
                sc_h = wh;
                sc_failed = 0;
                sc_ok = swapchain_make(ww, wh);
                if (sc_lost)
                    window_drop();      /* made again from the window next frame */
            }
        }
        if (surface && ww >= 1 && wh >= 1 && sc_ok) {
            ow = sc_ew;
            oh = sc_eh;
            turns = sc_turns;
            mirror = sc_mirror;
            show = 1;
        } else {
            /* no window, or none to show in: drawn as before (at the frame's own
               units before any), not shown; texture targets drawn all the same */
            ow = last_w ? last_w : f->width;
            oh = last_w ? last_h : f->height;
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

    /* everything this frame makes, made before anything reads a size or a view */
    iw = image_w(ow, oh, turns);            /* the window-sized targets' own size: the display's orientation */
    ih = image_h(ow, oh, turns);
    show = show && has_window(f);           /* a frame with no window pass shows nothing */
    /* with no window at all and RR_DRAW, the window's pass runs into an image kept for it, never read or shown */
    sink = draw_all && !windowed && has_window(f);
    window = show || sink;
    others = window || has_picture(f);      /* an interlaced picture builds up: its passes but the window's always run */
    targets_fit(f, iw, ih, others);
    if (sink)
        sink_fit(iw, ih);
    else if (draw_img.i)
        sink_free();
    make_textures(f);
    buf_fit(&staging, f->upload_bytes + 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    memcpy(staging.at, f->upload, f->upload_bytes);
    buf_fit(&dbuf, (f->data_words + 1) * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
    memcpy(dbuf.at, f->data, f->data_words * 4);
    buf_fit(&ubuf, (f->nuniforms + 1) * 256, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);   /* past the frame's: the levels' */
    if (sets_gen != views_gen)
        sets_reset();

    /* the uniforms, each block given its target's size, samples and orientation */
    patch_uniforms(f, ow, oh, turns, mirror, 0, 0, 0, 0);
    {
        /* the smaller levels' block, past the frame's: a whole level drawn, up the right way */
        rr_uniform u;
        memset(&u, 0, sizeof u);
        u.f[0][2] = 1.0f;
        u.f[0][3] = 1.0f;
        memcpy((char *)ubuf.at + f->nuniforms * 256, &u, sizeof u);
    }

    if (show) {
        VkResult r = vkAcquireNextImageKHR(dev, swapchain, UINT64_MAX, sem_acquire, VK_NULL_HANDLE, &image);
        /* an index is always one of the images made (as many as there are) */
        if (r == VK_ERROR_DEVICE_LOST)
            fail("vkAcquireNextImageKHR (the GPU was lost)", r);
        if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
            show = 0;
            if (r == VK_ERROR_SURFACE_LOST_KHR)
                window_drop();          /* made again from the window next frame */
            else
                sc_ok = 0;
        } else {
            paced = 1;
        }
    }

    vkResetFences(dev, 1, &fence);
    vkResetCommandBuffer(cmd, 0);
    vkBeginCommandBuffer(cmd, &begin);
    {
        /* the renderer's own textures made since the last frame, readable from the start (the frame's are
           below): the ones read where there is no texture read 0; the targets are drawn before they are read */
        Image *own[4 + SCREENS] = { &none_colour, &none_index, &none_array, &picture };
        for (k = 0; k < SCREENS; k++)
            own[4 + k] = &screen_res[k];
        for (k = 0; k < 4 + SCREENS; k++)
            if (own[k]->i && own[k]->unready) {
                if (k < 3)
                    clear_new(own[k]->i);
                else
                    barrier(own[k]->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SHADER_STAGES);
            }
        for (k = 0; k < 4 + SCREENS; k++)
            own[k]->unready = 0;
    }
    record_texops(f);
    record_passes(f, ow, oh, turns, mirror, aspect, window, others, 0, show ? sc_fb[image] : sink ? draw_fb : VK_NULL_HANDLE,
                  show ? rp_window : rp_screen, show ? KIND_WINDOW : KIND_SCREEN);
    vkEndCommandBuffer(cmd);
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    if (show) {
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &sem_acquire;
        si.pWaitDstStageMask = &wait;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sc_done[image];
    }
    submit(&si);
    if (show) {
        VkPresentInfoKHR pi = { VK_STRUCTURE_TYPE_PRESENT_INFO_KHR };
        VkResult r;
        pi.waitSemaphoreCount = 1;
        pi.pWaitSemaphores = &sc_done[image];
        pi.swapchainCount = 1;
        pi.pSwapchains = &swapchain;
        pi.pImageIndices = &image;
        r = vkQueuePresentKHR(queue, &pi);
        if (r == VK_ERROR_DEVICE_LOST)
            fail("vkQueuePresentKHR (the GPU was lost)", r);
        if (r == VK_ERROR_SURFACE_LOST_KHR)
            window_drop();              /* made again from the window next frame */
        else if (r != VK_SUCCESS)
            sc_ok = 0;
    }
    if (windowed)
        host_window_give(sc_window);    /* kept: a surface on it, or its having been tried */
    ever = 1;
    last_w = ow;
    last_h = oh;
    last_aspect = aspect;
    last_turns = turns;
    last_mirror = mirror;
    last_frame = f;
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!ever || !last_frame || !has_window(last_frame))
        return 0;
    if (argb) {
        VkCommandBufferBeginInfo begin = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        VkBufferImageCopy c;
        VkDeviceSize bytes = (VkDeviceSize)last_w * (VkDeviceSize)last_h * 4;
        Image ms, ds, img;
        VkFramebuffer fb;
        VkImageView v[3];
        int pic = has_picture(last_frame);
        wait_frame();
        if (readback.size < bytes) {
            buf_free(&readback);
            buf_make(&readback, bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
        }
        /* the last frame's passes again (its buffers are as it left them), the window's into an image of
           the window's size; of an interlaced picture only the window's, the picture being that frame's */
        frame_no++;
        /* unturned: the picture's own size; an interlaced picture's targets are left as they are (the picture
           kept is the display's size, which this is not where it is turned) */
        if (!pic)
            targets_fit(last_frame, last_w, last_h, 1);
        patch_uniforms(last_frame, last_w, last_h, 0, 0, 1, pic, last_turns, last_mirror);
        memset(&ms, 0, sizeof ms);
        memset(&ds, 0, sizeof ds);
        memset(&img, 0, sizeof img);
        image_make(&ms, COLOUR, last_w, last_h, pass_samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_COLOR_BIT);
        image_make(&ds, depth_format, last_w, last_h, pass_samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT,
                   VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT);
        image_make(&img, COLOUR, last_w, last_h, 1, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        v[0] = ms.v;
        v[1] = ds.v;
        v[2] = img.v;
        fb = framebuffer(rp_screen, v, 3, last_w, last_h);
        if (sets_gen != views_gen)
            sets_reset();
        memset(&c, 0, sizeof c);
        c.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        c.imageSubresource.layerCount = 1;
        c.imageExtent.width = (uint32_t)last_w;
        c.imageExtent.height = (uint32_t)last_h;
        c.imageExtent.depth = 1;
        vkResetFences(dev, 1, &fence);
        vkResetCommandBuffer(cmd, 0);
        vkBeginCommandBuffer(cmd, &begin);
        {
            /* the targets made for this read: readable from the start, as in a frame */
            Image *own[1 + SCREENS] = { &picture };
            int k;
            for (k = 0; k < SCREENS; k++)
                own[1 + k] = &screen_res[k];
            for (k = 0; k < 1 + SCREENS; k++)
                if (own[k]->i && own[k]->unready) {
                    barrier(own[k]->i, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, 0, VK_ACCESS_SHADER_READ_BIT,
                            VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, SHADER_STAGES);
                    own[k]->unready = 0;
                }
        }
        record_passes(last_frame, last_w, last_h, 0, 0, last_aspect, 1, !pic, 1, fb, rp_screen, KIND_SCREEN);
        barrier(img.i, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT,
                VK_ACCESS_TRANSFER_READ_BIT, ALL_STAGES, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, img.i, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, readback.b, 1, &c);
        {
            /* the copy made visible to the CPU's reads */
            VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
            hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
            hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            hb.buffer = readback.b;
            hb.size = VK_WHOLE_SIZE;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, NULL, 1, &hb, 0, NULL);
        }
        vkEndCommandBuffer(cmd);
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        submit(&si);
        wait_frame();
        /* BGRA bytes are 0xAARRGGBB words */
        memcpy(argb, readback.at, (size_t)bytes);
        vkDestroyFramebuffer(dev, fb, NULL);
        image_free(&ms);
        image_free(&ds);
        image_free(&img);
    }
    return 1;
}
