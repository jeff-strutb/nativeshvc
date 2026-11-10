/*
 * rr_metal.m: the renderer (rr.h) on Metal (macOS, iOS). The frame's
 * textures are Metal textures; its words and uniforms are copied into two
 * buffers; each pass is a render encoder, its draws instanced draws with no
 * vertex buffers. Pipelines and depth-stencil states are made when first
 * used. The programs are MSL (rr_msl.h), compiled at start-up; a game's
 * shaders are MSL too, compiled when loaded. One frame is in flight.
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

/* what a pass draws into: the multisampled screens with depth and stencil,
   an integer texture, or the output */
enum { KIND_SCREEN, KIND_INDEX, KIND_COLOUR, KINDS };

static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static dispatch_semaphore_t inflight;
static id<MTLFunction> vfn[RR_PROGRAMS], ffn[RR_PROGRAMS], game_fns[MAX_SHADERS];
static id<MTLTexture> tex[MAX_TEXTURES];
static uint8_t tex_format[MAX_TEXTURES];
static id<MTLTexture> none_colour, none_index;
static id<MTLTexture> screen_ms[3], screen_ds[3], extra_res[2], output;
static id<MTLBuffer> ubuf, dbuf, rbuf;
static id<MTLSamplerState> sampler;
static int last_w, last_h, ever;

/* pipelines by program (and game shader), blend and pass kind; depth-stencil states by their settings */
#define MAX_STATES 4096
static uint32_t pipe_key[MAX_STATES], ds_key[MAX_STATES];
static id<MTLRenderPipelineState> pipe_obj[MAX_STATES];
static id<MTLDepthStencilState> ds_obj[MAX_STATES];
static size_t npipes, ndss;

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
        dev = MTLCreateSystemDefaultDevice();
        if (!dev)
            return 0;
        queue = [dev newCommandQueue];
        for (k = 0; k < RR_PROGRAMS; k++) {
            vfn[k] = function_of((const char *)programs[k].vs, "a program's vertex stage");
            ffn[k] = function_of((const char *)programs[k].fs, "a program's fragment stage");
            if (!vfn[k] || !ffn[k])
                return 0;
        }
        {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
            sampler = [dev newSamplerStateWithDescriptor:sd];
        }
        {
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:1 height:1 mipmapped:NO];
            static const uint32_t zero = 0;
            none_colour = [dev newTextureWithDescriptor:td];
            [none_colour replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:4];
            td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Uint width:1 height:1 mipmapped:NO];
            none_index = [dev newTextureWithDescriptor:td];
            [none_index replaceRegion:MTLRegionMake2D(0, 0, 1, 1) mipmapLevel:0 withBytes:&zero bytesPerRow:1];
        }
        inflight = dispatch_semaphore_create(1);
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
    if (inflight) {
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        dispatch_semaphore_signal(inflight);
    }
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
            MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format_of(o->format) width:o->w height:o->h mipmapped:NO];
            td.usage = MTLTextureUsageShaderRead | (o->flags & RR_TEX_TARGET ? MTLTextureUsageRenderTarget : 0);
            if (o->flags & RR_TEX_TARGET)
                td.storageMode = MTLStorageModePrivate;
            tex[o->id] = [dev newTextureWithDescriptor:td];
            tex_format[o->id] = o->format;
        } else if (tex[o->id]) {
            [tex[o->id] replaceRegion:MTLRegionMake2D(o->x, o->y, o->w, o->h) mipmapLevel:0 withBytes:f->upload + o->at
                          bytesPerRow:(NSUInteger)o->w * (NSUInteger)bytes_of(tex_format[o->id])];
        }
    }
}

/* a target of the window's size, multisampled or not */
static id<MTLTexture> target(id<MTLTexture> t, MTLPixelFormat f, int w, int h, int samples, MTLTextureUsage u)
{
    MTLTextureDescriptor *td;
    if (t && (int)t.width == w && (int)t.height == h)
        return t;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = u;
    td.storageMode = MTLStorageModePrivate;
    if (samples > 1) {
        td.textureType = MTLTextureType2DMultisample;
        td.sampleCount = (NSUInteger)samples;
    }
    return [dev newTextureWithDescriptor:td];
}

static MTLPixelFormat kind_format(int kind) { return kind == KIND_INDEX ? MTLPixelFormatR8Uint : MTLPixelFormatBGRA8Unorm; }

static void blend_of(MTLRenderPipelineColorAttachmentDescriptor *a, int blend)
{
    a.blendingEnabled = blend != RR_BLEND_REPLACE && blend != RR_BLEND_NOCOLOUR;
    a.writeMask = MTLColorWriteMaskAll;
    a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationAdd;
    a.sourceRGBBlendFactor = a.sourceAlphaBlendFactor = MTLBlendFactorOne;
    a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorZero;
    switch (blend) {
    case RR_BLEND_NOCOLOUR:
        a.writeMask = MTLColorWriteMaskNone;
        break;
    case RR_BLEND_ADD_BY_A:
    case RR_BLEND_RSUB_BY_A:
        a.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        a.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
        a.destinationRGBBlendFactor = MTLBlendFactorOne;
        a.rgbBlendOperation = blend == RR_BLEND_ADD_BY_A ? MTLBlendOperationAdd : MTLBlendOperationReverseSubtract;
        break;
    case RR_BLEND_MIX_BY_A:
    case RR_BLEND_RMIX_BY_A:
        a.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        a.sourceRGBBlendFactor = MTLBlendFactorDestinationAlpha;
        a.destinationRGBBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
        a.rgbBlendOperation = blend == RR_BLEND_MIX_BY_A ? MTLBlendOperationAdd : MTLBlendOperationReverseSubtract;
        break;
    case RR_BLEND_A_ZERO:
        a.writeMask = MTLColorWriteMaskAlpha;
        a.sourceAlphaBlendFactor = MTLBlendFactorZero;
        a.destinationAlphaBlendFactor = MTLBlendFactorZero;
        break;
    case RR_BLEND_RGB_ZERO_A_ADD:
        a.sourceRGBBlendFactor = MTLBlendFactorZero;
        a.destinationRGBBlendFactor = MTLBlendFactorZero;
        a.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
        a.destinationAlphaBlendFactor = MTLBlendFactorOne;
        break;
    case RR_BLEND_A_ADD:
        a.writeMask = MTLColorWriteMaskAlpha;
        a.sourceAlphaBlendFactor = MTLBlendFactorDestinationAlpha;
        a.destinationAlphaBlendFactor = MTLBlendFactorOne;
        break;
    case RR_BLEND_RGB_SCALE:
        a.writeMask = MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        a.sourceRGBBlendFactor = MTLBlendFactorZero;
        a.destinationRGBBlendFactor = MTLBlendFactorBlendColor;
        break;
    case RR_BLEND_OVER:
        a.destinationRGBBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
        break;
    default:
        break;
    }
}

static id<MTLRenderPipelineState> pipe_of(const rr_cmd *c, int kind)
{
    uint32_t key = (uint32_t)c->program | (uint32_t)c->shader << 8 | (uint32_t)c->blend << 24 | (uint32_t)kind << 29;
    MTLRenderPipelineDescriptor *d;
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    id<MTLFunction> fs = c->shader && c->shader < MAX_SHADERS ? game_fns[c->shader] : ffn[c->program];
    size_t i;
    for (i = 0; i < npipes; i++)
        if (pipe_key[i] == key)
            return pipe_obj[i];
    if (!fs)
        return nil;
    d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vfn[c->program];
    d.fragmentFunction = fs;
    d.colorAttachments[0].pixelFormat = kind_format(kind);
    blend_of(d.colorAttachments[0], c->blend);
    if (kind == KIND_SCREEN) {
        d.depthAttachmentPixelFormat = DEPTH_FORMAT;
        d.stencilAttachmentPixelFormat = DEPTH_FORMAT;
        d.rasterSampleCount = RR_SAMPLES;
    }
    p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p) {
        fprintf(stderr, "rr: metal: pipeline %u: %s\n", c->program, [[err description] UTF8String]);
        return nil;
    }
    if (npipes < MAX_STATES) {
        pipe_key[npipes] = key;
        pipe_obj[npipes] = p;
        npipes++;
    }
    return p;
}

static id<MTLDepthStencilState> ds_of(const rr_cmd *c, int kind)
{
    uint32_t key = kind == KIND_SCREEN ? (uint32_t)c->depth | (uint32_t)c->stencil << 4 | (uint32_t)c->read_mask << 8 | (uint32_t)c->write_mask << 16 : 0;
    MTLDepthStencilDescriptor *d;
    id<MTLDepthStencilState> s;
    size_t i;
    for (i = 0; i < ndss; i++)
        if (ds_key[i] == key)
            return ds_obj[i];
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
    if (ndss < MAX_STATES) {
        ds_key[ndss] = key;
        ds_obj[ndss] = s;
        ndss++;
    }
    return s;
}

static id<MTLTexture> slot_texture(int32_t t, int slot)
{
    if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        return extra_res[t == RR_TEX_EXTRA0 ? 0 : 1];
    if (t > 0 && t < MAX_TEXTURES && tex[t])
        return tex[t];
    return slot < 2 ? none_colour : none_index;
}

static id<MTLBuffer> buffer_fit(id<MTLBuffer> b, size_t bytes)
{
    if (b && [b length] >= bytes)
        return b;
    return [dev newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
}

int rr_draw(rr_frame *f)
{
    int paced = 0;
    @autoreleasepool {
        CAMetalLayer *ml = host_metal_layer();
        id<CAMetalDrawable> dr = nil;
        id<MTLCommandBuffer> cb;
        int ow = 0, oh = 0, vx, vy, vw, vh, k;
        float aspect = f->aspect;
        size_t i, p;
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        apply_texops(f);
        for (i = 0; i < f->nshaders; i++) {
            /* a game's fragment stage, with a program's vertex stage */
            const rr_shader_load *s = &f->shaders[i];
            char *src = (char *)malloc(s->size + 1);
            if (!src)
                exit(70);
            memcpy(src, s->code, s->size);
            src[s->size] = 0;
            if (s->id < MAX_SHADERS)
                game_fns[s->id] = function_of(src, "a game's shader");
            free(src);
        }
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
        rr_view(aspect, ow, oh, &vx, &vy, &vw, &vh);
        for (k = 0; k < 3; k++) {
            screen_ms[k] = target(screen_ms[k], MTLPixelFormatBGRA8Unorm, ow, oh, RR_SAMPLES, MTLTextureUsageRenderTarget);
            screen_ds[k] = target(screen_ds[k], DEPTH_FORMAT, ow, oh, RR_SAMPLES, MTLTextureUsageRenderTarget);
        }
        for (k = 0; k < 2; k++)
            extra_res[k] = target(extra_res[k], MTLPixelFormatBGRA8Unorm, ow, oh, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        output = target(output, MTLPixelFormatBGRA8Unorm, ow, oh, 1, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        last_w = ow;
        last_h = oh;
        ever = 1;

        /* the uniforms, each block given its target's size and samples */
        ubuf = buffer_fit(ubuf, (f->nuniforms + 1) * 256);
        dbuf = buffer_fit(dbuf, (f->data_words + 1) * 4);
        memcpy([dbuf contents], f->data, f->data_words * 4);
        for (p = 0; p < f->npasses; p++) {
            const rr_pass *ps = &f->passes[p];
            int tw = ow, th = oh, samples = ps->target <= RR_TARGET_EXTRA1 ? RR_SAMPLES : 1;
            if (ps->target == RR_TARGET_TEXTURE && ps->texture < MAX_TEXTURES && tex[ps->texture]) {
                tw = (int)tex[ps->texture].width;
                th = (int)tex[ps->texture].height;
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
            int kind, tw = ow, th = oh;
            if (ps->target <= RR_TARGET_EXTRA1) {
                int t = ps->target;
                kind = KIND_SCREEN;
                rp.colorAttachments[0].texture = screen_ms[t];
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(ps->clear[0], ps->clear[1], ps->clear[2], ps->clear[3]);
                rp.colorAttachments[0].storeAction = MTLStoreActionMultisampleResolve;
                rp.colorAttachments[0].resolveTexture = t == RR_TARGET_FRAME ? output : extra_res[t - 1];
                rp.depthAttachment.texture = screen_ds[t];
                rp.depthAttachment.loadAction = MTLLoadActionClear;
                rp.depthAttachment.clearDepth = 1.0;
                rp.depthAttachment.storeAction = MTLStoreActionDontCare;
                rp.stencilAttachment.texture = screen_ds[t];
                rp.stencilAttachment.loadAction = MTLLoadActionClear;
                rp.stencilAttachment.clearStencil = 0;
                rp.stencilAttachment.storeAction = MTLStoreActionDontCare;
            } else if (ps->target == RR_TARGET_TEXTURE) {
                if (ps->texture >= MAX_TEXTURES || !tex[ps->texture])
                    continue;
                kind = KIND_INDEX;
                tw = (int)tex[ps->texture].width;
                th = (int)tex[ps->texture].height;
                rp.colorAttachments[0].texture = tex[ps->texture];
                rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            } else {
                kind = KIND_COLOUR;
                rp.colorAttachments[0].texture = output;
                rp.colorAttachments[0].loadAction = MTLLoadActionLoad;
                rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            }
            e = [cb renderCommandEncoderWithDescriptor:rp];
            for (i = ps->first; i < ps->first + ps->count; i++) {
                const rr_cmd *c = &f->cmds[i];
                id<MTLRenderPipelineState> pl = pipe_of(c, kind);
                MTLViewport vp;
                MTLScissorRect sc = { 0, 0, (NSUInteger)tw, (NSUInteger)th };
                if (!pl || !c->vertices || !c->instances)
                    continue;
                [e setRenderPipelineState:pl];
                [e setDepthStencilState:ds_of(c, kind)];
                [e setStencilReferenceValue:c->ref];
                [e setBlendColorRed:c->constant green:c->constant blue:c->constant alpha:c->constant];
                if (c->space == RR_SPACE_VIEW && kind != KIND_INDEX)
                    vp = (MTLViewport){ vx, vy, vw, vh, 0, 1 };
                else
                    vp = (MTLViewport){ 0, 0, tw, th, 0, 1 };
                [e setViewport:vp];
                if (c->scissor) {
                    int x0, y0, x1, y1;
                    rr_scissor(f->uniforms[c->uniform].f[2], f->width, f->height, (int)vp.originX, (int)vp.originY, (int)vp.width,
                               (int)vp.height, &x0, &y0, &x1, &y1);
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
                    [e setVertexTexture:t atIndex:(NSUInteger)(2 + k)];
                    [e setFragmentTexture:t atIndex:(NSUInteger)(2 + k)];
                    [e setVertexSamplerState:sampler atIndex:(NSUInteger)(2 + k)];
                    [e setFragmentSamplerState:sampler atIndex:(NSUInteger)(2 + k)];
                }
                [e drawPrimitives:c->topology == RR_LINES ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle vertexStart:0
                      vertexCount:c->vertices instanceCount:c->instances];
            }
            [e endEncoding];
        }
        if (ml) {
            dr = [ml nextDrawable];
            paced = 1;
        }
        if (dr && (int)[dr texture].width == ow && (int)[dr texture].height == oh) {
            /* the output, shown */
            id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
            [b copyFromTexture:output sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake((NSUInteger)ow, (NSUInteger)oh, 1)
                     toTexture:[dr texture] destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
            [b endEncoding];
            [cb presentDrawable:dr];
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
