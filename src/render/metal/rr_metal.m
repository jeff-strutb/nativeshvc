/*
 * rr_metal.m: the renderer on Metal (macOS, iOS). The store of tiles, maps and
 * images is one buffer on the GPU; a picture's changes are copied in, the
 * picture is drawn by a fragment shader at its own size into a 32-bit integer
 * target, then a second fragment shader draws the window's pixels. The shaders
 * are compiled at start-up.
 */
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_window.h"
#include "rr_picture_src.h"

CAMetalLayer *host_metal_layer(void);
void host_metal_layer_fit(int *w, int *h);
__attribute__((weak)) CAMetalLayer *host_metal_layer(void) { return nil; }
__attribute__((weak)) void host_metal_layer_fit(int *w, int *h) { *w = *h = 0; }

static const char k_pre[] =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "#define PB_FN\n"
    "#define PB_VAR\n"
    "#define U32 uint\n"
    "#define I32 int\n"
    "#define F32 float\n"
    "#define U_(x) uint(x)\n"
    "#define I_(x) int(x)\n"
    "#define F_(x) float(x)\n"
    "#define F_B(x) as_type<float>(x)\n"
    "#define D_(i) pb_db[i]\n"
    "#define T_(i) pb_tb[i]\n"
    "#define P_(x, y) pb_img.read(uint2(x, y)).r\n"
    "#define L_(n, x, y) pb_list[n].read(uint2(x, y)).r\n"
    "struct PB {\n"
    "device const uint *pb_db;\n"
    "device const uint *pb_tb;\n"
    "texture2d<uint> pb_img;\n"
    "array<texture2d<uint>, 4> pb_list;\n";
static const char k_post[] =
    "};\n"
    "struct VO { float4 pos [[position]]; };\n"
    "vertex VO vs(uint i [[vertex_id]]) {\n"
    "  VO o; float2 p = float2(float((i << 1) & 2u), float(i & 2u));\n"
    "  o.pos = float4(p * float2(2, -2) + float2(-1, 1), 0, 1); return o; }\n"
    "fragment uint4 picture_fs(VO in [[stage_in]], device const uint *d [[buffer(0)]], device const uint *t [[buffer(1)]],\n"
    "                          array<texture2d<uint>, 4> lists [[texture(1)]]) {\n"
    "  PB b; b.pb_db = d; b.pb_tb = t; b.pb_list = lists;\n"
    "  if (b.pb_row_kept(uint(in.pos.y))) discard_fragment();\n"
    "  return uint4(b.pb_pixel(uint(in.pos.x), uint(in.pos.y))); }\n"
    "fragment float4 window_fs(VO in [[stage_in]], device const uint *d [[buffer(0)]], device const uint *t [[buffer(1)]],\n"
    "                          texture2d<uint> pic [[texture(0)]], array<texture2d<uint>, 4> lists [[texture(1)]]) {\n"
    "  PB b; b.pb_db = d; b.pb_tb = t; b.pb_img = pic; b.pb_list = lists;\n"
    "  uint c = b.pb_window(uint(in.pos.x), uint(in.pos.y));\n"
    "  return float4(float((c >> 16) & 255u), float((c >> 8) & 255u), float(c & 255u), 255.0) / 255.0; }\n"
    /* a draw list's vertices and fill */
    "struct LV { float x, y, u, v; uint colour; };\n"
    "struct LO { float4 pos [[position]]; float2 uv [[user(locn0)]]; uint colour [[user(locn1), flat]]; };\n"
    "struct LU { float w, h; uint base, tw, th, p0, p1, p2; float inputs[8]; };\n"
    "vertex LO list_vs(uint i [[vertex_id]], device const LV *v [[buffer(0)]], constant LU &u [[buffer(1)]]) {\n"
    "  LO o; o.pos = float4(v[i].x / u.w * 2 - 1, 1 - v[i].y / u.h * 2, 0, 1); o.uv = float2(v[i].u, v[i].v); o.colour = v[i].colour; return o; }\n"
    "fragment uint4 list_fs(LO in [[stage_in]], constant LU &u [[buffer(1)]], device const uint *t [[buffer(2)]]) {\n"
    "  if (u.tw == 0u) return uint4(in.colour);\n"
    "  uint x = uint(clamp(int(floor(in.uv.x)), 0, int(u.tw) - 1)), y = uint(clamp(int(floor(in.uv.y)), 0, int(u.th) - 1)), c = t[u.base + y * u.tw + x];\n"
    "  if (c == 0u) discard_fragment();\n"
    "  return uint4(c); }\n";

static id<MTLLibrary> shared_lib;
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLRenderPipelineState> pipe_picture, pipe_window, pipe_list;
static id<MTLTexture> list_tex[4], none_tex;
static id<MTLBuffer> vert_buf;
static id<MTLBuffer> store, descb;
static id<MTLTexture> pic, offscreen;
static dispatch_semaphore_t inflight;
static int last_w, last_h;

static id<MTLRenderPipelineState> pipe_of(id<MTLLibrary> lib, NSString *fs, MTLPixelFormat f);
static id<MTLRenderPipelineState> pipe_vs(id<MTLLibrary> lib, NSString *v, NSString *fs, MTLPixelFormat f)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    NSError *err = nil;
    id<MTLRenderPipelineState> p;
    d.vertexFunction = [lib newFunctionWithName:v];
    d.fragmentFunction = [lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = f;
    p = [dev newRenderPipelineStateWithDescriptor:d error:&err];
    if (!p)
        fprintf(stderr, "rr: metal pipeline %s: %s\n", [fs UTF8String], [[err description] UTF8String]);
    return p;
}

static id<MTLRenderPipelineState> pipe_of(id<MTLLibrary> lib, NSString *fs, MTLPixelFormat f) { return pipe_vs(lib, @"vs", fs, f); }

int rr_open(void)
{
    @autoreleasepool {
        NSError *err = nil;
        CAMetalLayer *ml;
        id<MTLLibrary> lib;
        NSString *src = [NSString stringWithFormat:@"%s%s%s", k_pre, k_picture_body, k_post];
        dev = MTLCreateSystemDefaultDevice();
        if (!dev)
            return 0;
        queue = [dev newCommandQueue];
        lib = [dev newLibraryWithSource:src options:nil error:&err];
        if (!lib) {
            fprintf(stderr, "rr: metal shader: %s\n", [[err description] UTF8String]);
            return 0;
        }
        pipe_picture = pipe_of(lib, @"picture_fs", MTLPixelFormatR32Uint);
        pipe_window = pipe_of(lib, @"window_fs", MTLPixelFormatBGRA8Unorm);
        shared_lib = lib;
        pipe_list = pipe_vs(lib, @"list_vs", @"list_fs", MTLPixelFormatR32Uint);
        if (!pipe_picture || !pipe_window || !pipe_list)
            return 0;
        inflight = dispatch_semaphore_create(1);
        ml = host_metal_layer();
        if (ml) {
            [ml setDevice:dev];
            [ml setMaximumDrawableCount:3];
        }
    }
    return 1;
}

void rr_close(void) {}
const char *rr_name(void) { return "metal"; }
const char *rr_shader_ext(void) { return "metal"; }

static void store_fit(size_t words)
{
    size_t bytes = words * 4;
    if (store && [store length] >= bytes)
        return;
    {
        size_t cap = store ? [store length] : 1u << 20;
        id<MTLBuffer> n;
        while (cap < bytes)
            cap *= 2;
        n = [dev newBufferWithLength:cap options:MTLResourceStorageModeShared];
        if (store)
            memcpy([n contents], [store contents], [store length]);
        store = n;
    }
}

static void apply_ops(const eng_packet *p)
{
    size_t i = 0;
    uint32_t *to;
    store_fit(p->store_words ? p->store_words : 1);
    to = (uint32_t *)[store contents];
    while (i < p->ops_words) {
        uint32_t at = p->ops[i], n = p->ops[i + 1];
        memcpy(to + at, p->ops + i + 2, (size_t)n * 4);
        i += 2 + (size_t)n;
    }
}

static id<MTLTexture> target_of(id<MTLTexture> t, MTLPixelFormat f, int w, int h, MTLTextureUsage u)
{
    MTLTextureDescriptor *td;
    if (t && (int)t.width == w && (int)t.height == h)
        return t;
    td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:(NSUInteger)w height:(NSUInteger)h mipmapped:NO];
    td.usage = u;
    td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}

static void pass_load(id<MTLCommandBuffer> c, id<MTLTexture> to, id<MTLRenderPipelineState> p, id<MTLTexture> from, MTLLoadAction load)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    id<MTLRenderCommandEncoder> e;
    rp.colorAttachments[0].texture = to;
    rp.colorAttachments[0].loadAction = load;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    e = [c renderCommandEncoderWithDescriptor:rp];
    [e setRenderPipelineState:p];
    [e setFragmentBuffer:descb offset:0 atIndex:0];
    [e setFragmentBuffer:store offset:0 atIndex:1];
    if (from)
        [e setFragmentTexture:from atIndex:0];
    {
        int k;
        for (k = 0; k < 4; k++)
            [e setFragmentTexture:list_tex[k] ? list_tex[k] : none_tex atIndex:(NSUInteger)(1 + k)];
    }
    [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [e endEncoding];
}

static void pass(id<MTLCommandBuffer> c, id<MTLTexture> to, id<MTLRenderPipelineState> p, id<MTLTexture> from)
{
    pass_load(c, to, p, from, MTLLoadActionDontCare);
}

/* the draw lists, each into its own target */
typedef struct { float w, h; uint32_t base, tw, th, p0, p1, p2; float inputs[8]; } LU;
#define MAX_SHADERS 256
static id<MTLRenderPipelineState> game_pipes[MAX_SHADERS];

static void draw_lists(id<MTLCommandBuffer> c, const eng_packet *p)
{
    int k;
    size_t bytes = 0, off = 0;
    if (!none_tex)
        none_tex = target_of(nil, MTLPixelFormatR32Uint, 1, 1, MTLTextureUsageShaderRead);
    for (k = 0; k < 4; k++)
        bytes += p->lists[k].nverts * ENG_DRAW_WORDS * 4;
    if (bytes && (!vert_buf || [vert_buf length] < bytes))
        vert_buf = [dev newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
    for (k = 0; k < 4; k++) {
        const eng_list *l = &p->lists[k];
        MTLRenderPassDescriptor *rp;
        id<MTLRenderCommandEncoder> e;
        size_t i;
        if (!l->w) {
            list_tex[k] = nil;
            continue;
        }
        list_tex[k] = target_of(list_tex[k], MTLPixelFormatR32Uint, (int)l->w, (int)l->h, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = list_tex[k];
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        e = [c renderCommandEncoderWithDescriptor:rp];
        [e setRenderPipelineState:pipe_list];
        if (l->nverts) {
            memcpy((char *)[vert_buf contents] + off, p->list_data + l->verts, l->nverts * ENG_DRAW_WORDS * 4);
            [e setVertexBuffer:vert_buf offset:off atIndex:0];
            [e setFragmentBuffer:store offset:0 atIndex:2];
        }
        for (i = 0; i < l->ncmds; i++) {
            const uint32_t *cm = p->list_data + l->cmds + i * ENG_CMD_WORDS;
            LU u = { (float)l->w, (float)l->h, cm[3], cm[4], cm[5] };
            memcpy(u.inputs, cm + 7, 32);
            [e setRenderPipelineState:cm[6] && game_pipes[cm[6]] ? game_pipes[cm[6]] : pipe_list];
            [e setVertexBytes:&u length:sizeof u atIndex:1];
            [e setFragmentBytes:&u length:sizeof u atIndex:1];
            [e drawPrimitives:cm[2] ? MTLPrimitiveTypeLine : MTLPrimitiveTypeTriangle vertexStart:cm[0] vertexCount:cm[1]];
        }
        [e endEncoding];
        off += l->nverts * ENG_DRAW_WORDS * 4;
    }
}

int rr_draw(eng_packet *p)
{
    int paced = 0;
    @autoreleasepool {
        CAMetalLayer *ml = host_metal_layer();
        id<CAMetalDrawable> dr = nil;
        id<MTLCommandBuffer> c;
        id<MTLTexture> to;
        int ow = 0, oh = 0;
        size_t bytes = p->desc_words * 4;
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        apply_ops(p);
        {
            uint32_t k;
            for (k = 0; k < p->nshaders; k++) {
                /* the game's fragment function, with the engine's vertex function */
                NSError *err = nil;
                NSString *src = [[NSString alloc] initWithBytes:p->shaders[k].code length:p->shaders[k].size encoding:NSUTF8StringEncoding];
                id<MTLLibrary> l = src ? [dev newLibraryWithSource:src options:nil error:&err] : nil;
                id<MTLFunction> fn = l ? [l newFunctionWithName:@"main0"] : nil;
                if (fn) {
                    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
                    d.vertexFunction = [shared_lib newFunctionWithName:@"list_vs"];
                    d.fragmentFunction = fn;
                    d.colorAttachments[0].pixelFormat = MTLPixelFormatR32Uint;
                    game_pipes[p->shaders[k].id] = [dev newRenderPipelineStateWithDescriptor:d error:&err];
                }
                if (!game_pipes[p->shaders[k].id])
                    fprintf(stderr, "rr: metal game shader: %s\n", [[err description] UTF8String]);
            }
        }
        if (ml)
            host_metal_layer_fit(&ow, &oh);
        if (ow < 1 || oh < 1) {
            const char *e = getenv("RR_SIZE");
            ow = p->width;
            oh = p->height;
            if (e)
                sscanf(e, "%dx%d", &ow, &oh);
            ml = nil;
        }
        rr_window_words(p->desc, ow, oh);
        if (!descb || [descb length] < bytes)
            descb = [dev newBufferWithLength:bytes * 2 options:MTLResourceStorageModeShared];
        memcpy([descb contents], p->desc, bytes);
        pic = target_of(pic, MTLPixelFormatR32Uint, p->width, p->height, MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
        last_w = p->width;
        last_h = p->height;
        c = [queue commandBuffer];
        {
            dispatch_semaphore_t s = inflight;
            [c addCompletedHandler:^(id<MTLCommandBuffer> b) { (void)b; dispatch_semaphore_signal(s); }];
        }
        draw_lists(c, p);
        pass_load(c, pic, pipe_picture, nil, p->keep ? MTLLoadActionLoad : MTLLoadActionDontCare);
        if (ml) {
            dr = [ml nextDrawable];
            paced = 1;
        }
        if (dr && (int)[dr texture].width == ow && (int)[dr texture].height == oh) {
            to = [dr texture];
        } else {
            offscreen = target_of(offscreen, MTLPixelFormatBGRA8Unorm, ow, oh, MTLTextureUsageRenderTarget);
            to = offscreen;
            dr = nil;
        }
        pass(c, to, pipe_window, pic);
        if (dr)
            [c presentDrawable:dr];
        [c commit];
    }
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!pic)
        return 0;
    if (argb) {
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        [pic getBytes:argb bytesPerRow:(NSUInteger)last_w * 4
          fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)last_w, (NSUInteger)last_h) mipmapLevel:0];
        dispatch_semaphore_signal(inflight);
    }
    return 1;
}

int rr_read_window(uint32_t *argb, int *w, int *h)
{
    if (!offscreen) {
        *w = *h = 0;
        return 0;
    }
    *w = (int)offscreen.width;
    *h = (int)offscreen.height;
    if (argb) {
        dispatch_semaphore_wait(inflight, DISPATCH_TIME_FOREVER);
        [offscreen getBytes:argb bytesPerRow:(NSUInteger)*w * 4
                 fromRegion:MTLRegionMake2D(0, 0, (NSUInteger)*w, (NSUInteger)*h) mipmapLevel:0];
        dispatch_semaphore_signal(inflight);
    }
    return 1;
}
