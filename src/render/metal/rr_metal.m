/*
 * rr_metal.m: the renderer (rr.h) on Metal (macOS, iOS). The frame's
 * textures are Metal textures; its words and uniforms are copied into two
 * buffers; each pass is a render encoder, its draws instanced draws with no
 * vertex buffers. Pipelines and depth-stencil states are made when first
 * used and kept (a failure too), found by hashing. The programs are MSL
 * (rr_msl.h), compiled at start-up; a game's shaders are MSL too, compiled
 * when loaded. The multisampled targets are never kept past their pass, so
 * they take no memory where the GPU can do without (memoryless). One frame is
 * in flight.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_window.h"
#include "rr_msl.h"

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

/* what a pass draws into: the multisampled screens with depth and stencil, an
   8-bit or 32-bit integer texture, or a colour texture, the picture or the output */
enum { KIND_SCREEN, KIND_INDEX, KIND_INDEX32, KIND_COLOUR, KINDS };

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static dispatch_semaphore_t inflight;
static id<MTLFunction> vfn[RR_PROGRAMS], ffn[RR_PROGRAMS], game_fns[MAX_SHADERS];
static id<MTLTexture> tex[MAX_TEXTURES];
static uint8_t tex_format[MAX_TEXTURES];
static uint8_t *tex_fresh[MAX_TEXTURES];        /* each layer: not drawn into yet */
static id<MTLTexture> none_colour, none_index, none_array;
static id<MTLTexture> screen_ms[3], screen_ds[3], screen_res[3], picture, output;
static int picture_fresh, output_fresh, memoryless;
static uint64_t screen_used[3], frame_no;
static id<MTLBuffer> ubuf, dbuf, rbuf;
static id<MTLSamplerState> sampler, filtered;
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
    for (k = 0; k < 3; k++)
        screen_ms[k] = screen_ds[k] = screen_res[k] = nil;
    for (k = 0; k < MAX_SHADERS; k++)
        game_fns[k] = nil;
    for (k = 0; k < RR_PROGRAMS; k++)
        vfn[k] = ffn[k] = nil;
    picture = output = nil;
    none_colour = none_index = none_array = nil;
    ubuf = dbuf = rbuf = nil;
    pipes = dss = nil;
    sampler = filtered = nil;
    queue = nil;
    dev = nil;
    ever = 0;
    dispatch_semaphore_signal(inflight);
    inflight = nil;
}

const char *rr_name(void) { return "metal"; }
const char *rr_shader_ext(void) { return "metal"; }

static MTLPixelFormat format_of(int f) { return f == RR_R8U ? MTLPixelFormatR8Uint : f == RR_R32U ? MTLPixelFormatR32Uint : MTLPixelFormatBGRA8Unorm; }
static int bytes_of(int f) { return f == RR_R8U ? 1 : 4; }

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
            free(tex_fresh[o->id]);
            tex_fresh[o->id] = (uint8_t *)malloc(layers);
            if (!tex_fresh[o->id])
                exit(70);
            memset(tex_fresh[o->id], 1, layers);
        } else if (tex[o->id]) {
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

static id<MTLRenderPipelineState> pipe_of(const rr_cmd *c, int kind)
{
    NSNumber *key = @((uint64_t)c->program | (uint64_t)c->shader << 8 | (uint64_t)c->blend << 24 | (uint64_t)kind << 32);
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
            d.rasterSampleCount = RR_SAMPLES;
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

static id<MTLTexture> slot_texture(int32_t t, int slot)
{
    id<MTLTexture> r = nil;
    if (t == RR_TEX_FRAME)
        r = screen_res[0];
    else if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        r = screen_res[t == RR_TEX_EXTRA0 ? 1 : 2];
    else if (t == RR_TEX_PICTURE)
        r = picture;
    else if (t > 0 && t < MAX_TEXTURES && tex[t] && (slot >= 6) == (tex[t].textureType == MTLTextureType2DArray))
        r = tex[t];
    if (r)
        return r;
    return slot >= 6 ? none_array : slot < 3 ? none_colour : none_index;
}

static id<MTLBuffer> buffer_fit(id<MTLBuffer> b, size_t bytes)
{
    if (b && [b length] >= bytes)
        return b;
    return [dev newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
}

/* the window-sized targets this frame draws into: made at this size, the extra ones only while passes use them */
static void targets_fit(const rr_frame *f, int w, int h)
{
    int k, used[3] = { 0, 0, 0 };
    size_t p;
    for (p = 0; p < f->npasses; p++)
        if (f->passes[p].target <= RR_TARGET_EXTRA1)
            used[f->passes[p].target] = 1;
    for (k = 0; k < 3; k++) {
        if (used[k]) {
            screen_ms[k] = target(screen_ms[k], MTLPixelFormatBGRA8Unorm, w, h, RR_SAMPLES, MTLTextureUsageRenderTarget, 1);
            screen_ds[k] = target(screen_ds[k], DEPTH_FORMAT, w, h, RR_SAMPLES, MTLTextureUsageRenderTarget, 1);
            screen_res[k] = target(screen_res[k], MTLPixelFormatBGRA8Unorm, w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
            screen_used[k] = frame_no;
        } else if (screen_res[k] && frame_no - screen_used[k] > UNUSED_FRAMES) {
            screen_ms[k] = screen_ds[k] = screen_res[k] = nil;
        }
    }
    {
        id<MTLTexture> was = picture;
        picture = target(picture, MTLPixelFormatBGRA8Unorm, w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
        picture_fresh |= picture != was;
        was = output;
        output = target(output, MTLPixelFormatBGRA8Unorm, w, h, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead, 0);
        output_fresh |= output != was;
    }
}

/* a pass's target: its kind, texture and whether it can be drawn into */
static int pass_target(const rr_pass *ps, int *kind, id<MTLTexture> *to)
{
    *to = nil;
    if (ps->target <= RR_TARGET_EXTRA1) {
        *kind = KIND_SCREEN;
        *to = screen_res[ps->target];
        return screen_ms[ps->target] != nil;
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
    *to = ps->target == RR_TARGET_PICTURE ? picture : output;
    *kind = KIND_COLOUR;
    return 1;
}

int rr_draw(rr_frame *f)
{
    int paced = 0;
    @autoreleasepool {
        CAMetalLayer *ml = host_metal_layer();
        id<CAMetalDrawable> dr = nil;
        id<MTLCommandBuffer> cb;
        int ow = 0, oh = 0, k;
        float aspect = f->aspect;
        size_t i, p;
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        frame_no++;
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
            const char *e = getenv("RR_SIZE");
            ow = f->width;
            oh = f->height;
            if (e)
                sscanf(e, "%dx%d", &ow, &oh);
            else
                aspect = 0;
            ml = nil;
        }
        if (ow < 1 || oh < 1) {
            dispatch_semaphore_signal(inflight);
            return 0;
        }
        targets_fit(f, ow, oh);
        last_w = ow;
        last_h = oh;
        ever = 1;

        /* the uniforms, each block given its target's size and samples */
        ubuf = buffer_fit(ubuf, (f->nuniforms + 1) * 256);
        dbuf = buffer_fit(dbuf, (f->data_words + 1) * 4);
        memcpy([dbuf contents], f->data, f->data_words * 4);
        for (p = 0; p < f->npasses; p++) {
            const rr_pass *ps = &f->passes[p];
            id<MTLTexture> to;
            int kind, samples = ps->target <= RR_TARGET_EXTRA1 ? RR_SAMPLES : 1, tw = ow, th = oh;
            if (pass_target(ps, &kind, &to) && to) {
                tw = (int)to.width;
                th = (int)to.height;
            }
            for (i = ps->first; i < ps->first + ps->count; i++) {
                rr_uniform u = f->uniforms[f->cmds[i].uniform];
                u.f[0][0] = (float)tw;
                u.f[0][1] = (float)th;
                u.f[0][2] = (float)samples;
                u.f[0][3] = -1.0f;
                memcpy((char *)[ubuf contents] + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
            }
        }

        cb = [queue commandBuffer];
        {
            dispatch_semaphore_t s = inflight;
            [cb addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(s); }];
        }
        for (p = 0; p < f->npasses; p++) {
            const rr_pass *ps = &f->passes[p];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> e;
            id<MTLTexture> to;
            int kind, tw, th, vx, vy, vw, vh;
            if (!pass_target(ps, &kind, &to)) {
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
                int t = ps->target;
                rp.colorAttachments[0].texture = screen_ms[t];
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(ps->clear[0], ps->clear[1], ps->clear[2], ps->clear[3]);
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                rp.colorAttachments[0].resolveTexture = screen_res[t];
                rp.depthAttachment.texture = screen_ds[t];
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                rp.stencilAttachment.texture = screen_ds[t];
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = 0;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
            } else {
                int fresh = 0;
                if (ps->target == RR_TARGET_TEXTURE) {
                    uint32_t layer = to.textureType == MTLTextureType2DArray ? ps->layer : 0;
                    fresh = tex_fresh[ps->texture][layer];
                    tex_fresh[ps->texture][layer] = 0;
                } else if (ps->target == RR_TARGET_PICTURE) {
                    fresh = picture_fresh;
                    picture_fresh = 0;
                } else {
                    fresh = output_fresh;
                    output_fresh = 0;
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
                id<MTLRenderPipelineState> pl = pipe_of(c, kind);
                MTLViewport vp;
                MTLScissorRect sc = { 0, 0, (NSUInteger)tw, (NSUInteger)th };
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
                    sc = (MTLScissorRect){ (NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0) };
                }
                [e setScissorRect:sc];
                [e setVertexBuffer:ubuf offset:(NSUInteger)c->uniform * 256 atIndex:0];
                [e setFragmentBuffer:ubuf offset:(NSUInteger)c->uniform * 256 atIndex:0];
                [e setVertexBuffer:dbuf offset:0 atIndex:1];
                [e setFragmentBuffer:dbuf offset:0 atIndex:1];
                for (k = 0; k < RR_TEX_SLOTS; k++) {
                    id<MTLTexture> t = slot_texture(c->tex[k], k);
                    id<MTLSamplerState> sm = k == 1 || k == 2 || k >= 6 ? filtered : sampler;
                    [e setVertexTexture:t atIndex:(NSUInteger)(2 + k)];
                    [e setFragmentTexture:t atIndex:(NSUInteger)(2 + k)];
                    [e setVertexSamplerState:sm atIndex:(NSUInteger)(2 + k)];
                    [e setFragmentSamplerState:sm atIndex:(NSUInteger)(2 + k)];
                }
                [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:c->vertices instanceCount:c->instances];
            }
            [e endEncoding];
            if (ps->target == RR_TARGET_TEXTURE && ps->mips && to.mipmapLevelCount > 1) {
                /* its smaller levels (that layer's), from the first */
                id<MTLTexture> t = to;
                id<MTLBlitCommandEncoder> b;
                if (t.textureType == MTLTextureType2DArray)
                    t = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2DArray
                                                  levels:NSMakeRange(0, t.mipmapLevelCount) slices:NSMakeRange(ps->layer, 1)];
                b = [cb blitCommandEncoder];
                [b generateMipmapsForTexture:t];
                [b endEncoding];
            }
        }
        /* shown only when a drawable of this size is there; paced only when shown */
        if (ml && CGSizeEqualToSize([ml drawableSize], CGSizeMake(ow, oh)))
            dr = [ml nextDrawable];
        if (dr && (int)[dr texture].width == ow && (int)[dr texture].height == oh) {
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)ow, (NSUInteger)oh, 1)
                     toTexture:[dr texture] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            [cb presentDrawable:dr];
            paced = 1;
        }
        [cb commit];
    }
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!ever || !output)
        return 0;
    if (argb) {
        @autoreleasepool {
            id<MTLCommandBuffer> cb;
            id<MTLBlitCommandEncoder> b;
            size_t bytes = (size_t)last_w * (size_t)last_h * 4;
            dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
            if (!rbuf || [rbuf length] < bytes)
                rbuf = [dev newBufferWithLength:bytes options:MTLResourceStorageModeShared];
            cb = [queue commandBuffer];
            b = [cb blitCommandEncoder];
            [b copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
                    sourceSize:MTLSizeMake((NSUInteger)last_w, (NSUInteger)last_h, 1) toBuffer:rbuf destinationOffset:0
               destinationBytesPerRow:(NSUInteger)last_w * 4 destinationBytesPerImage:bytes];
            [b endEncoding];
            [cb commit];
            [cb waitUntilCompleted];
            /* BGRA bytes are 0xAARRGGBB words */
            memcpy(argb, [rbuf contents], bytes);
            dispatch_semaphore_signal(inflight);
        }
    }
    return 1;
}
