/*
 * rr_metal.m: the renderer (rr.h) on Metal (macOS, iOS). The frame's
 * textures are Metal textures; its words and uniforms are copied into two
 * buffers; each pass is a render encoder, its draws instanced draws with no
 * vertex buffers. Pipelines and depth-stencil states are made when first
 * used and kept (a failure too), found by hashing. The programs are MSL
 * (rr_msl.h), compiled at start-up; a game's shaders are MSL too, compiled
 * when loaded. The multisampled targets are never kept past their pass, so
 * they take no memory where the GPU can do without (memoryless). The window
 * pass resolves straight into the drawable. One frame is in flight. A frame
 * read back (rr_read) is run again: the backend keeps a reference to the last
 * frame it ran, and its passes but the textures' are encoded again, the
 * window's into a texture.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_window.h"
#include "rr_msl.h"
#include "host.h"

CAMetalLayer *host_metal_layer(void);
void host_metal_layer_fit(int *w, int *h);
__attribute__((weak)) CAMetalLayer *host_metal_layer(void) { return nil; }
__attribute__((weak)) void host_metal_layer_fit(int *w, int *h) { *w = *h = 0; }

typedef struct { const void *vs; size_t vs_size; const void *fs; size_t fs_size; } Program;
static const Program programs[RR_PROGRAMS] = { RR_PROGRAM_TABLE };

#define MAX_TEXTURES 16384
#define MAX_SHADERS 256
#define DEPTH_FORMAT MTLPixelFormatDepth32Float_Stencil8
#define UNUSED_FRAMES 120               /* an extra target unused this many frames is let go */

/* what a pass draws into: the multisampled screens (the window's too) with depth
   and stencil, an 8-bit or 32-bit integer texture, or a colour texture or the picture */
enum { KIND_SCREEN, KIND_INDEX, KIND_INDEX32, KIND_COLOUR, KINDS };

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static dispatch_semaphore_t inflight;
static id<MTLFunction> vfn[RR_PROGRAMS], ffn[RR_PROGRAMS], game_fns[MAX_SHADERS];
static id<MTLTexture> tex[MAX_TEXTURES];
static uint8_t tex_format[MAX_TEXTURES];
static uint8_t *tex_fresh[MAX_TEXTURES];        /* each layer: not drawn into yet */
static id<MTLTexture> none_colour, none_index, none_array;
/* the screens by index: 0 the frame, 1 and 2 the extra targets (resolved into screen_res), 3 the window (into the
   drawable), 4 and on the side targets (RR_SIDES, resolved into screen_res, or drawn into it at one sample) */
#define SCREENS (4 + RR_SIDES)
static id<MTLTexture> screen_ms[SCREENS], screen_ds[SCREENS], screen_res[SCREENS], picture, read_tex;
static int screen_samples[SCREENS];     /* the samples a side target was made with */
static int pass_samples, samples_have;  /* the screens' samples a pixel; the counts the device has (a bit each) */
static int picture_fresh, memoryless;
static int windowed;                    /* drawn for a window's layer, not headless */
static uint64_t screen_used[SCREENS], picture_used, frame_no;
static const rr_frame *last_frame;      /* the last frame run, for rr_read: its owner leaves it whole until a later one is run */
static float last_aspect;
static id<MTLBuffer> ubuf, dbuf, rbuf;
static id<MTLSamplerState> sampler, filtered, bordered;   /* at texels; filtered repeating; filtered, transparent outside */
static int last_w, last_h, ever;

/* pipelines by program (and game shader), blend and pass kind; depth-stencil
   states by their settings: hashed, a failure kept as NSNull */
static NSMutableDictionary *pipes, *dss;

static id<MTLFunction> function_of(const char *src, const char *what)
{
    NSError *err = nil;
    NSString *s = [NSString stringWithUTF8String:src];
    id<MTLLibrary> lib = s ? [dev newLibraryWithSource:s options:nil error:&err] : nil;
    id<MTLFunction> fn = lib ? [lib newFunctionWithName:@"main0"] : nil;
    if (!fn)
        fprintf(stderr, "rr: metal: %s: %s\n", what, err ? [[err description] UTF8String] : "no main0");
    return fn;
}

int rr_open(void)
{
    @autoreleasepool {
        CAMetalLayer *ml;
        int k;
        /* first, so that rr_close can let go of whatever is made if a later step fails */
        inflight = dispatch_semaphore_create(1);
        dev = MTLCreateSystemDefaultDevice();
        if (!dev) {
            rr_close();
            return 0;
        }
        queue = [dev newCommandQueue];
        pipes = [NSMutableDictionary new];
        dss = [NSMutableDictionary new];
        /* memoryless targets on Apple's own GPUs */
        memoryless = 0;
        if (@available(macOS 11.0, iOS 13.0, *))
            memoryless = [dev supportsFamily:MTLGPUFamilyApple1];
        /* the samples the screens have: 8 where the device has them, else 4 */
        samples_have = 1;
        for (k = 2; k <= 16; k *= 2)
            if ([dev supportsTextureSampleCount:(NSUInteger)k])
                samples_have |= k;
        pass_samples = samples_have & 8 ? 8 : samples_have & 4 ? 4 : samples_have & 2 ? 2 : 1;
        for (k = 0; k < RR_PROGRAMS; k++) {
            vfn[k] = function_of((const char *)programs[k].vs, "a program's vertex stage");
            ffn[k] = function_of((const char *)programs[k].fs, "a program's fragment stage");
            if (!vfn[k] || !ffn[k]) {
                rr_close();
                return 0;
            }
        }
        {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler = [dev newSamplerStateWithDescriptor:sd];
            sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
            sd.mipFilter = MTLSamplerMipFilterLinear;
            sd.maxAnisotropy = 16;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeRepeat;
            filtered = [dev newSamplerStateWithDescriptor:sd];
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToZero;   /* (0, 0, 0, 0) outside: it has alpha */
            bordered = [dev newSamplerStateWithDescriptor:sd];
        }
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            static const uint32_t zero = 0;
            none_colour = [dev newTextureWithDescriptor:td];
            [none_colour replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
            td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:1 height:1 mipmapped:NO];
            none_index = [dev newTextureWithDescriptor:td];
            [none_index replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:1];
            td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            td.textureType = MTLTextureType2DArray;
            td.arrayLength = 1;
            none_array = [dev newTextureWithDescriptor:td];
            [none_array replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 slice:0 withBytes:&zero bytesPerRow:4 bytesPerImage:4];
        }
        ml = host_metal_layer();
        windowed = ml != nil;
        if (ml) {
            [ml setDevice:dev];
            [ml setMaximumDrawableCount:3];
        }
    }
    return 1;
}

void rr_close(void)
{
    int k;
    if (!inflight)
        return;
    dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
    for (k = 0; k < MAX_TEXTURES; k++) {
        tex[k] = nil;
        free(tex_fresh[k]);
        tex_fresh[k] = NULL;
    }
    for (k = 0; k < SCREENS; k++)
        screen_ms[k] = screen_ds[k] = screen_res[k] = nil;
    for (k = 0; k < MAX_SHADERS; k++)
        game_fns[k] = nil;
    for (k = 0; k < RR_PROGRAMS; k++)
        vfn[k] = ffn[k] = nil;
    picture = read_tex = nil;
    none_colour = none_index = none_array = nil;
    ubuf = dbuf = rbuf = nil;
    pipes = dss = nil;
    sampler = filtered = bordered = nil;
    queue = nil;
    dev = nil;
    ever = 0;
    /* everything as before the first rr_open, so another opens soundly */
    memset(tex_format, 0, sizeof tex_format);
    memset(screen_used, 0, sizeof screen_used);
    memset(screen_samples, 0, sizeof screen_samples);
    pass_samples = samples_have = 0;
    picture_fresh = memoryless = windowed = 0;
    picture_used = 0;
    last_frame = NULL;
    last_aspect = 0;
    frame_no = 0;
    last_w = last_h = 0;
    dispatch_semaphore_signal(inflight);
    inflight = nil;
}

const char *rr_name(void) { return "metal"; }
int rr_samples(void) { return pass_samples ? pass_samples : 4; }
int rr_samples_at_most(int n)
{
    int c;
    for (c = 16; c > 1; c /= 2)
        if (c <= n && (samples_have & c))
            return c;
    return 1;
}
const char *rr_shader_ext(void) { return "metal"; }

static MTLPixelFormat format_of(int f) { return f == RR_R8U ? MTLPixelFormatR8Uint : f == RR_R32U ? MTLPixelFormatR32Uint : MTLPixelFormatBGRA8Unorm; }
static int bytes_of(int f) { return f == RR_R8U ? 1 : 4; }

/* a texture just made reads 0 until written (rr.h): every level and layer
   written with zeros (it is the CPU's to write, as every write here is) */
static void zero_new(id<MTLTexture> t, int format)
{
    static uint8_t *zero;
    static size_t cap;
    size_t row = t.width * (size_t)bytes_of(format), need = row * t.height;
    NSUInteger lv, sl;
    int array = t.textureType == MTLTextureType2DArray;
    if (need > cap) {
        free(zero);
        if (!(zero = (uint8_t *)calloc(need, 1)))
            exit(70);
        cap = need;
    }
    for (sl = 0; sl < (array ? t.arrayLength : 1); sl++)
        for (lv = 0; lv < t.mipmapLevelCount; lv++) {
            NSUInteger w = t.width >> lv ? t.width >> lv : 1, h = t.height >> lv ? t.height >> lv : 1;
            /* the stride between images an array's only, 0 otherwise */
            [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:lv slice:sl withBytes:zero bytesPerRow:w * (NSUInteger)bytes_of(format)
                bytesPerImage:array ? w * h * (NSUInteger)bytes_of(format) : 0];
        }
}

static void apply_texops(const rr_frame *f)
{
    size_t i;
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        if (o->id >= MAX_TEXTURES) {
            fprintf(stderr, "rr: metal: texture %u past the most\n", o->id);
            continue;
        }
        if (o->op == RR_TEX_CREATE) {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format_of(o->format) width:o->w height:o->h
                                                                                         mipmapped:(o->flags & RR_TEX_MIPS) != 0 && o->format == RR_BGRA8];
            uint32_t layers = o->flags & RR_TEX_ARRAY && o->layers ? o->layers : 1;
            /* the default storage, a target's too: it is written from here (replaceRegion) as any texture is */
            td.usage = MTLTextureUsageShaderRead | (o->flags & RR_TEX_TARGET ? MTLTextureUsageRenderTarget : 0);
            if (o->flags & RR_TEX_ARRAY) {
                td.textureType = MTLTextureType2DArray;
                td.arrayLength = layers;
            }
            tex[o->id] = [dev newTextureWithDescriptor:td];
            tex_format[o->id] = o->format;
            if (tex[o->id])
                zero_new(tex[o->id], o->format);
            free(tex_fresh[o->id]);
            tex_fresh[o->id] = (uint8_t *)malloc(layers);
            if (!tex_fresh[o->id])
                exit(70);
            memset(tex_fresh[o->id], 1, layers);
        } else if (tex[o->id] && ((uint64_t)o->x + o->w > tex[o->id].width || (uint64_t)o->y + o->h > tex[o->id].height || !o->w || !o->h)) {
            fprintf(stderr, "rr: metal: a write past texture %u's edge left out\n", o->id);
        } else if (tex[o->id]) {
            tex_fresh[o->id][0] = 0;    /* written: a pass that keeps what it held keeps this */
            [tex[o->id] replaceRegion:MTLRegionMake2D(o->x, o->y, o->w, o->h) mipmapLevel:0 withBytes:f->upload + o->at
                          bytesPerRow:(NSUInteger)o->w * (NSUInteger)bytes_of(tex_format[o->id])];
        }
    }
}

/* a target of w x h; one never kept past its pass takes no memory where the GPU allows */
static id<MTLTexture> target(id<MTLTexture> t, MTLPixelFormat f, int w, int h, int samples, MTLTextureUsage u, int transient)
{
    MTLTextureDescriptor *td;
    if (t && (int)t.width == w && (int)t.height == h)
        return t;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = u;
    td.storageMode = MTLStorageModePrivate;
    if (transient && memoryless) {
        if (@available(macOS 11.0, iOS 10.0, *))
            td.storageMode = MTLStorageModeMemoryless;
    }
    if (samples > 1) {
        td.textureType = MTLTextureType2DMultisample;
        td.sampleCount = (NSUInteger)samples;
    }
    return [dev newTextureWithDescriptor:td];
}

static int integer_kind(int kind) { return kind == KIND_INDEX || kind == KIND_INDEX32; }
static MTLPixelFormat kind_format(int kind)
{
    return kind == KIND_INDEX ? MTLPixelFormatR8Uint : kind == KIND_INDEX32 ? MTLPixelFormatR32Uint : MTLPixelFormatBGRA8Unorm;
}

/* blending, where the target can blend: an integer target never does */
static void blend_of(MTLRenderPipelineColorAttachmentDescriptor *a, int blend, int kind)
{
    a.blendingEnabled = blend == RR_BLEND_OVER && !integer_kind(kind);
    a.writeMask = blend == RR_BLEND_NOCOLOUR ? MTLColorWriteMaskNone : MTLColorWriteMaskAll;
    a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationAdd;
    a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOne;
    a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
}

static id<MTLRenderPipelineState> pipe_of(const rr_cmd *c, int kind, int samples)
{
    NSNumber *key = @((uint64_t)c->program | (uint64_t)c->shader << 8 | (uint64_t)c->blend << 24 | (uint64_t)kind << 32 | (uint64_t)samples << 40);
    id found = pipes[key];
    MTLRenderPipelineDescriptor *d;
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    id<MTLFunction> fs = c->shader ? (c->shader < MAX_SHADERS ? game_fns[c->shader] : nil) : ffn[c->program];
    if (found)
        return found == [NSNull null] ? nil : found;
    p = nil;
    if (fs) {
        d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = vfn[c->program];
        d.fragmentFunction = fs;
        d.colorAttachments[0].pixelFormat = kind_format(kind);
        blend_of(d.colorAttachments[0], c->blend, kind);
        if (kind == KIND_SCREEN) {
            d.depthAttachmentPixelFormat = DEPTH_FORMAT;
            d.stencilAttachmentPixelFormat = DEPTH_FORMAT;
            d.rasterSampleCount = (NSUInteger)samples;
        }
        p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
        if (!p)
            fprintf(stderr, "rr: metal: pipeline %u: %s\n", c->program, [[err description] UTF8String]);
    } else {
        fprintf(stderr, "rr: metal: no shader for program %u (game shader %u)\n", c->program, c->shader);
    }
    pipes[key] = p ? (id)p : (id)[NSNull null];
    return p;
}

static id<MTLDepthStencilState> ds_of(const rr_cmd *c, int kind)
{
    uint32_t k = kind == KIND_SCREEN ? (uint32_t)c->depth | (uint32_t)c->stencil << 4 | (uint32_t)c->read_mask << 8 | (uint32_t)c->write_mask << 16 : 0;
    NSNumber *key = @(k);
    id found = dss[key];
    id<MTLDepthStencilState> s;
    MTLDepthStencilDescriptor *d;
    if (found)
        return found == [NSNull null] ? nil : found;
    d = [MTLDepthStencilDescriptor new];
    d.depthCompareFunction = MTLCompareFunctionAlways;
    d.depthWriteEnabled = NO;
    if (kind == KIND_SCREEN) {
        if (c->depth == RR_DEPTH_LEQUAL) {
            d.depthCompareFunction = MTLCompareFunctionLessEqual;
            d.depthWriteEnabled = YES;
        }
        if (c->stencil != RR_STENCIL_OFF) {
            MTLStencilDescriptor *st = [MTLStencilDescriptor new];
            st.readMask = c->read_mask;
            st.writeMask = c->write_mask;
            st.stencilFailureOperation = MTLStencilOperationKeep;
            st.depthFailureOperation = MTLStencilOperationKeep;
            st.depthStencilPassOperation = MTLStencilOperationKeep;
            st.stencilCompareFunction = MTLCompareFunctionEqual;
            if (c->stencil == RR_STENCIL_EQUAL_INC) {
                st.depthFailureOperation = MTLStencilOperationIncrementClamp;
                st.depthStencilPassOperation = MTLStencilOperationIncrementClamp;
            } else if (c->stencil == RR_STENCIL_SET) {
                st.stencilCompareFunction = MTLCompareFunctionAlways;
                st.depthFailureOperation = MTLStencilOperationReplace;
                st.depthStencilPassOperation = MTLStencilOperationReplace;
            }
            d.frontFaceStencil = st;
            d.backFaceStencil = st;
        }
    }
    s = [dev newDepthStencilStateWithDescriptor:d];
    if (!s)
        fprintf(stderr, "rr: metal: a depth and stencil state could not be made\n");
    dss[key] = s ? (id)s : (id)[NSNull null];
    return s;
}

/* the texture binding `slot` reads: one of another kind than the binding's
   (colour, integer, array of colour) reads as none, said once a frame */
static id<MTLTexture> slot_texture(int32_t t, int slot)
{
    static uint64_t said;
    id<MTLTexture> r = nil;
    if (t == RR_TEX_FRAME)
        r = screen_res[0];
    else if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        r = screen_res[t == RR_TEX_EXTRA0 ? 1 : 2];
    else if (t == RR_TEX_PICTURE)
        r = picture;
    else if (t <= RR_TEX_SIDE0 && t > RR_TEX_SIDE0 - RR_SIDES)
        r = screen_res[4 + (RR_TEX_SIDE0 - t)];
    else if (t > 0 && t < MAX_TEXTURES)
        r = tex[t];
    if (r) {
        int array = r.textureType == MTLTextureType2DArray;
        int integer = r.pixelFormat != MTLPixelFormatBGRA8Unorm;
        if (slot < 3 || slot >= 9 ? !array && !integer : slot < 6 ? !array && integer : array && !integer)
            return r;
        if (said != frame_no) {
            said = frame_no;
            fprintf(stderr, "rr: metal: texture %d is of another kind than binding %d reads: read as none\n", (int)t, slot + 2);
        }
    }
    return slot >= 9 || slot < 3 ? none_colour : slot >= 6 ? none_array : none_index;
}

static id<MTLBuffer> buffer_fit(id<MTLBuffer> b, size_t bytes)
{
    if (b && [b length] >= bytes)
        return b;
    return [dev newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
}

/* the screen (screen_ms's index) a pass target is: the frame, the extra targets, the window, a side target; -1 none */
static int screen_of(int target) { return target <= RR_TARGET_EXTRA1 ? target : target == RR_TARGET_WINDOW ? 3 : -1; }
static int screen_at(const rr_pass *ps) { return ps->target == RR_TARGET_SIDE ? 4 + (int)(ps->layer % RR_SIDES) : screen_of(ps->target); }

/* the samples a pixel of what a pass draws into has: the screens', a side target's own, a colour texture's 1 */
static int pass_n(const rr_pass *ps)
{
    int sc = screen_at(ps);
    return sc < 0 ? 1 : sc < 4 ? pass_samples : (int)ps->samples;
}

/* the window-sized targets the passes draw into, made at this size, and only
   while passes use them (`window`: the window's pass runs; `others`: the
   other screens', the side targets' and the picture's do); one not drawn into
   is let go when long unused, or no longer the window's size, and reads of it
   read nothing */
static void targets_fit(const rr_frame *f, int w, int h, int window, int others)
{
    int k, pic = 0, used[SCREENS] = { 0 }, count[SCREENS] = { 0 };
    size_t p;
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        int t = ps->target, sc = screen_at(ps);
        if (t == RR_TARGET_TEXTURE || (t == RR_TARGET_WINDOW ? !window : !others))
            continue;
        if (sc >= 0) {
            used[sc] = 1;
            count[sc] = pass_n(ps);
        } else if (t == RR_TARGET_PICTURE) {
            pic = 1;
        }
    }
    for (k = 0; k < SCREENS; k++) {
        if (used[k]) {
            int n = count[k];
            if (screen_ds[k] && screen_samples[k] != n)
                screen_ms[k] = screen_ds[k] = screen_res[k] = nil;      /* made again at the samples a side target now has */
            screen_samples[k] = n;
            if (n > 1)
                screen_ms[k] = target(screen_ms[k], MTLPixelFormatBGRA8Unorm, w, h, n, MTLTextureUsageRenderTarget, 1);
            else
                screen_ms[k] = nil;
            screen_ds[k] = target(screen_ds[k], DEPTH_FORMAT, w, h, n, MTLTextureUsageRenderTarget, 1);
            if (k != 3)
                screen_res[k] = target(screen_res[k], MTLPixelFormatBGRA8Unorm, w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
            screen_used[k] = frame_no;
        } else if (screen_ds[k] && (frame_no - screen_used[k] > UNUSED_FRAMES || (int)screen_ds[k].width != w || (int)screen_ds[k].height != h)) {
            /* not drawn into this frame: let go when long unused, or when no longer the window's size */
            screen_ms[k] = screen_ds[k] = screen_res[k] = nil;
        }
    }
    if (pic) {
        id<MTLTexture> was = picture;
        picture = target(picture, MTLPixelFormatBGRA8Unorm, w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
        picture_fresh |= picture != was;
        picture_used = frame_no;
    } else if (picture && (frame_no - picture_used > UNUSED_FRAMES || (int)picture.width != w || (int)picture.height != h)) {
        picture = nil;
    }
}

/* a pass's target: its kind, texture (the window's, `win`) and whether it can be drawn into */
static int pass_target(const rr_pass *ps, id<MTLTexture> win, int *kind, id<MTLTexture> *to)
{
    int k = screen_at(ps);
    *to = nil;
    if (k >= 0) {
        *kind = KIND_SCREEN;
        *to = k == 3 ? win : screen_res[k];
        return screen_ds[k] != nil && (screen_samples[k] <= 1 || screen_ms[k] != nil) && *to != nil;
    }
    if (ps->target == RR_TARGET_TEXTURE) {
        id<MTLTexture> t = ps->texture < MAX_TEXTURES ? tex[ps->texture] : nil;
        /* a texture not made to be drawn into, or an array's layer it has not, is no target */
        if (!t || !(t.usage & MTLTextureUsageRenderTarget) || (t.textureType == MTLTextureType2DArray && ps->layer >= t.arrayLength))
            return 0;
        *to = t;
        *kind = tex_format[ps->texture] == RR_R8U ? KIND_INDEX : tex_format[ps->texture] == RR_R32U ? KIND_INDEX32 : KIND_COLOUR;
        return 1;
    }
    *to = picture;
    *kind = KIND_COLOUR;
    return picture != nil;
}

/* the layer is the host's for good: nothing kept on a window that went */
void rr_window(void) {}

/* whether the frame has a pass into the picture target: an interlaced picture */
static int has_picture(const rr_frame *f)
{
    size_t p;
    for (p = 0; p < f->npasses; p++)
        if (f->passes[p].target == RR_TARGET_PICTURE)
            return 1;
    return 0;
}

/* the frame's passes encoded: the textures' (not when `again`, a frame being
   run again for reading: they are what they were), the window's when `window`
   and the other screens' and the picture's when `others`. The window's resolves into the drawable of `ml`, taken when its
   pass comes (*shown, if one of this size is there; with none, no window pass
   runs), else into `win` */
static void encode(id<MTLCommandBuffer> cb, const rr_frame *f, int ow, int oh, float aspect, int window, int others, int again, CAMetalLayer *ml,
                   id<MTLTexture> win, id<CAMetalDrawable> *shown)
{
    size_t i, p;
    int k;
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        id<MTLRenderCommandEncoder> e;
        id<MTLTexture> to;
        int kind, tw, th, vx, vy, vw, vh, sc = screen_at(ps);
        if (ps->target == RR_TARGET_TEXTURE ? again : ps->target == RR_TARGET_WINDOW ? !window : !others)
            continue;
        if (ps->target == RR_TARGET_WINDOW && ml) {
            /* shown only when a drawable of this size is there; paced only when shown */
            id<CAMetalDrawable> dr = [ml nextDrawable];
            if (!dr || (int)[dr texture].width != ow || (int)[dr texture].height != oh)
                continue;
            *shown = dr;
            win = [dr texture];
        }
        if (!pass_target(ps, win, &kind, &to)) {
            if (ps->count)
                fprintf(stderr, "rr: metal: a pass into no target (%u, texture %u, layer %u) left out\n", ps->target, ps->texture, ps->layer);
            continue;
        }
        tw = (int)to.width;
        th = (int)to.height;
        /* the view: the frame's units shown at their aspect, centred in this target; a
           window-sized target with no window and no RR_SIZE is the units themselves, filled */
        rr_view(ps->target == RR_TARGET_TEXTURE ? f->aspect : aspect, tw, th, &vx, &vy, &vw, &vh);
        if (kind == KIND_SCREEN) {
            rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(ps->clear[0], ps->clear[1], ps->clear[2], ps->clear[3]);
            if (screen_ms[sc]) {
                rp.colorAttachments[0].texture = screen_ms[sc];
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                rp.colorAttachments[0].resolveTexture = to;
            } else {
                rp.colorAttachments[0].texture = to;            /* one sample: drawn straight into its texture */
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            rp.depthAttachment.texture = screen_ds[sc];
            rp.depthAttachment.loadAction = MTLLoadActionClear;
            rp.depthAttachment.clearDepth = 1.0;
            rp.depthAttachment.storeAction = MTLStoreActionDontCare;
            rp.stencilAttachment.texture = screen_ds[sc];
            rp.stencilAttachment.loadAction = MTLLoadActionClear;
            rp.stencilAttachment.clearStencil = 0;
            rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
        } else {
            int fresh = 0;
            if (ps->target == RR_TARGET_TEXTURE) {
                uint32_t layer = to.textureType == MTLTextureType2DArray ? ps->layer : 0;
                fresh = tex_fresh[ps->texture][layer];
                tex_fresh[ps->texture][layer] = 0;
            } else {
                fresh = picture_fresh;
                picture_fresh = 0;
            }
            rp.colorAttachments[0].texture = to;
            if (to.textureType == MTLTextureType2DArray)
                rp.colorAttachments[0].slice = ps->layer;
            /* kept, unless just made: then cleared */
            rp.colorAttachments[0].loadAction = ps->load && !fresh ? MTLLoadActionLoad : MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(ps->clear[0], ps->clear[1], ps->clear[2], ps->clear[3]);
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        }
        e = [cb renderCommandEncoderWithDescriptor:rp];
        for (i = ps->first; i < ps->first + ps->count; i++) {
            const rr_cmd *c = &f->cmds[i];
            id<MTLRenderPipelineState> pl = pipe_of(c, kind, kind == KIND_SCREEN ? pass_n(ps) : 1);
            MTLViewport vp;
            MTLScissorRect scr = { 0, 0, (NSUInteger)tw, (NSUInteger)th };
            int view = c->space == RR_SPACE_VIEW;
            if (!pl || !c->vertices || !c->instances)
                continue;
            [e setRenderPipelineState:pl];
            [e setDepthStencilState:ds_of(c, kind)];
            [e setStencilReferenceValue:c->ref];
            vp = view ? (MTLViewport){ vx, vy, vw, vh, 0, 1 } : (MTLViewport){ 0, 0, tw, th, 0, 1 };
            [e setViewport:vp];
            if (c->scissor) {
                /* the rectangle in the draw's own units: the frame's in view space, target pixels else */
                int x0, y0, x1, y1;
                rr_scissor(f->uniforms[c->uniform].f[2], view ? f->width : tw, view ? f->height : th, (int)vp.originX, (int)vp.originY,
                           (int)vp.width, (int)vp.height, &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                scr = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
            }
            [e setScissorRect:scr];
            [e setVertexBuffer:ubuf offset:(NSUInteger)c->uniform * 256 atIndex:0];
            [e setFragmentBuffer:ubuf offset:(NSUInteger)c->uniform * 256 atIndex:0];
            [e setVertexBuffer:dbuf offset:0 atIndex:1];
            [e setFragmentBuffer:dbuf offset:0 atIndex:1];
            for (k = 0; k < RR_TEX_SLOTS; k++) {
                id<MTLTexture> t = slot_texture(c->tex[k], k);
                id<MTLSamplerState> sm = k == 1 || k == 6 ? filtered : k == 2 || (k >= 7 && k < 9) ? bordered : sampler;
                [e setVertexTexture:t atIndex:(NSUInteger)(2 + k)];
                [e setFragmentTexture:t atIndex:(NSUInteger)(2 + k)];
                [e setVertexSamplerState:sm atIndex:(NSUInteger)(2 + k)];
                [e setFragmentSamplerState:sm atIndex:(NSUInteger)(2 + k)];
            }
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:c->vertices instanceCount:c->instances];
        }
        [e endEncoding];
        if (ps->target == RR_TARGET_TEXTURE && ps->mips && to.mipmapLevelCount > 1 && kind == KIND_COLOUR) {
            /* its smaller levels (that layer's), each texel the mean of the 2 x 2 of
               the level before, by the renderer's own program: the same box on every
               backend (Metal's own mipmaps leave the filter to the GPU) */
            int array = to.textureType == MTLTextureType2DArray;
            NSUInteger lv, layer = array ? ps->layer : 0;
            id<MTLRenderPipelineState> pl;
            rr_cmd mc;
            memset(&mc, 0, sizeof mc);
            mc.program = array ? RR_MIPS_LAYER : RR_MIPS;
            mc.blend = RR_BLEND_REPLACE;
            pl = pipe_of(&mc, KIND_COLOUR, 1);
            for (lv = 1; pl && lv < to.mipmapLevelCount; lv++) {
                MTLRenderPassDescriptor *mp = [MTLRenderPassDescriptor renderPassDescriptor];
                id<MTLTexture> from = [to newTextureViewWithPixelFormat:to.pixelFormat textureType:array ? MTLTextureType2DArray : MTLTextureType2D
                                                                 levels:NSMakeRange(lv - 1, 1) slices:NSMakeRange(layer, 1)];
                id<MTLRenderCommandEncoder> me;
                rr_uniform mu;
                memset(&mu, 0, sizeof mu);
                mu.f[0][0] = (float)(to.width >> lv ? to.width >> lv : 1);
                mu.f[0][1] = (float)(to.height >> lv ? to.height >> lv : 1);
                mu.f[0][2] = 1.0f;
                mu.f[0][3] = 0.0f;       /* the orientation code: clip y up, no turn */
                mp.colorAttachments[0].texture = to;
                mp.colorAttachments[0].level = lv;
                mp.colorAttachments[0].slice = layer;
                mp.colorAttachments[0].loadAction = MTLLoadActionDontCare;   /* every texel drawn */
                mp.colorAttachments[0].storeAction = MTLStoreActionStore;
                me = [cb renderCommandEncoderWithDescriptor:mp];
                [me setRenderPipelineState:pl];
                [me setVertexBytes:&mu length:sizeof mu atIndex:0];
                [me setFragmentBytes:&mu length:sizeof mu atIndex:0];
                [me setFragmentTexture:from atIndex:2];
                [me setFragmentSamplerState:sampler atIndex:2];
                [me drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
                [me endEncoding];
            }
        }
    }
}

int rr_draw(rr_frame *f)
{
    int paced = 0;
    @autoreleasepool {
        /* the layer, held until the frame is committed (iOS lets none be drawn while the app is away) */
        CAMetalLayer *ml = windowed ? (__bridge CAMetalLayer *)host_window_take(NULL, NULL) : nil;
        id<CAMetalDrawable> dr = nil;
        id<MTLCommandBuffer> cb;
        int ow = 0, oh = 0, show, others;
        float aspect = f->aspect;
        size_t i, p;
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        frame_no++;
        read_tex = nil;                 /* made again if this frame is read */
        apply_texops(f);
        for (i = 0; i < f->nshaders; i++) {
            /* a game's fragment stage, with a program's vertex stage; its code is ours to let go */
            const rr_shader_load *s = &f->shaders[i];
            if (s->id && s->id < MAX_SHADERS) {
                char *src = (char *)malloc(s->size + 1);
                if (!src)
                    exit(70);
                memcpy(src, s->code, s->size);
                src[s->size] = 0;
                game_fns[s->id] = function_of(src, "a game's shader");
                free(src);
            } else {
                fprintf(stderr, "rr: metal: shader %u past the most\n", s->id);
            }
            free((void *)s->code);
            f->shaders[i].code = NULL;
        }
        f->nshaders = 0;
        if (ml)
            host_metal_layer_fit(&ow, &oh);
        if (ow < 1 || oh < 1) {
            if (windowed) {
                /* no window to show in now: drawn as before (at the frame's own
                   units before any), not shown; texture targets drawn all the same */
                ow = last_w ? last_w : f->width;
                oh = last_h ? last_h : f->height;
            } else {
                const char *e = getenv("RR_SIZE");
                ow = f->width;
                oh = f->height;
                if (e)
                    sscanf(e, "%dx%d", &ow, &oh);
                else
                    aspect = 0;
            }
            ml = nil;
        }
        if (ow < 1 || oh < 1)
            ow = oh = 1;
        /* the window's passes run only where a drawable of this size can be had */
        show = ml && CGSizeEqualToSize([ml drawableSize], CGSizeMake(ow, oh));
        others = show || has_picture(f);        /* an interlaced picture builds up: its passes but the window's always run */
        targets_fit(f, ow, oh, show, others);
        last_w = ow;
        last_h = oh;
        last_aspect = aspect;
        last_frame = f;
        ever = 1;

        /* the uniforms, each block given its target's size and samples */
        ubuf = buffer_fit(ubuf, (f->nuniforms + 1) * 256);
        dbuf = buffer_fit(dbuf, (f->data_words + 1) * 4);
        memcpy([dbuf contents], f->data, f->data_words * 4);
        for (p = 0; p < f->npasses; p++) {
            const rr_pass *ps = &f->passes[p];
            id<MTLTexture> to;
            int kind, samples = pass_n(ps), tw = ow, th = oh;
            if (screen_at(ps) < 0 && pass_target(ps, nil, &kind, &to) && to) {
                tw = (int)to.width;
                th = (int)to.height;
            }
            for (i = ps->first; i < ps->first + ps->count; i++) {
                rr_uniform u = f->uniforms[f->cmds[i].uniform];
                u.f[0][0] = (float)tw;
                u.f[0][1] = (float)th;
                u.f[0][2] = (float)samples;
                u.f[0][3] = 0.0f;               /* the orientation code (rr.h): clip y up, not turned */
                memcpy((char *)[ubuf contents] + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
            }
        }

        cb = [queue commandBuffer];
        {
            dispatch_semaphore_t s = inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(s); }];
        }
        encode(cb, f, ow, oh, aspect, show, others, 0, show ? ml : nil, nil, &dr);
        if (dr) {
            [cb presentDrawable:dr];
            paced = 1;
        }
        [cb commit];
        if (windowed)
            host_window_give(NULL);     /* the layer is the host's for good: nothing kept on it */
    }
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    size_t p;
    int window = 0;
    *w = last_w;
    *h = last_h;
    if (!ever || !last_frame)
        return 0;
    for (p = 0; p < last_frame->npasses; p++)
        window |= last_frame->passes[p].target == RR_TARGET_WINDOW;
    if (!window)
        return 0;
    if (argb) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb;
            id<MTLBlitCommandEncoder> b;
            size_t bytes = (size_t)last_w * (size_t)last_h * 4;
            int pic;
            dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
            if (!rbuf || [rbuf length] < bytes)
                rbuf = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            /* the last frame's passes again (its buffers are as it left them), the window's into a texture of the window's size */
            /* the window's pass again; the others' too, but of an interlaced picture, whose picture is already this frame's */
            pic = has_picture(last_frame);
            targets_fit(last_frame, last_w, last_h, 1, !pic);
            read_tex = target(read_tex, MTLPixelFormatBGRA8Unorm, last_w, last_h, 1, MTLTextureUsageRenderTarget, 0);
            cb = [queue commandBuffer];
            encode(cb, last_frame, last_w, last_h, last_aspect, 1, !pic, 1, nil, read_tex, NULL);
            b = [cb blitCommandEncoder];
            [b copyFromTexture:read_tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake((NSUInteger)last_w, (NSUInteger)last_h, 1) toBuffer:rbuf destinationOffset:0
               destinationBytesPerRow:(NSUInteger)last_w * 4 destinationBytesPerImage:bytes];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            /* BGRA bytes are 0xAARRGGBB words */
            memcpy(argb, [rbuf contents], bytes);
            read_tex = nil;
            dispatch_semaphore_signal(inflight);
        }
    }
    return 1;
}
