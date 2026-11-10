/*
 * rr_d3d12.c: the renderer (rr.h) on Direct3D 12 (Windows). The frame's
 * textures are default-heap textures, made before the frame is recorded and
 * written through an upload buffer at its start; its words are a root shader
 * resource and each draw's uniforms a root constant buffer; its textures a
 * descriptor table. The multisampled targets are resolved with
 * ResolveSubresource. Pipelines are made when first used and kept (a failure
 * too), found by hashing. The programs are DXIL (rr_dxil.h). One frame is in
 * flight.
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rr.h"
#include "rr_window.h"
#include "rr_dxil.h"
#include "host.h"

typedef struct { const void *vs; size_t vs_size; const void *fs; size_t fs_size; } Program;
static const Program programs[RR_PROGRAMS] = { RR_PROGRAM_TABLE };

#define FRAMES 3
#define MAX_TEXTURES 16384
#define MAX_SHADERS 256
#define RTVS 4096
#define UNUSED_FRAMES 120               /* an extra target unused this many frames is let go */
#define COLOUR DXGI_FORMAT_B8G8R8A8_UNORM
#define DEPTH DXGI_FORMAT_D24_UNORM_S8_UINT
#define SHADER_READ (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

/* what a pass draws into: the multisampled screens with depth and stencil, an
   8-bit or 32-bit integer texture, or a colour texture, the picture or the output */
enum { KIND_SCREEN, KIND_INDEX, KIND_INDEX32, KIND_COLOUR, KINDS };

typedef struct { ID3D12Resource *r; void *at; UINT64 size; } Buf;
/* a texture: rtv its first level's render-target view, the rest after it
   (an array's: layer by layer, each layer's levels together); fresh: each
   layer not drawn into yet */
typedef struct {
    ID3D12Resource *r;
    uint8_t *fresh;
    int w, h, format, rtv, rtvs, levels, layers, array, target;
    D3D12_RESOURCE_STATES state;
} Tex;
typedef struct { void *code; size_t size; } Code;

static ID3D12Device *dev;
static ID3D12CommandQueue *queue;
static ID3D12CommandAllocator *alloc;
static ID3D12GraphicsCommandList *list;
static ID3D12Fence *fence;
static HANDLE fence_event;
static UINT64 fence_at;
static IDXGIFactory4 *factory;
static IDXGISwapChain3 *swapchain;
static ID3D12Resource *sc_img[FRAMES];
static D3D12_RESOURCE_STATES sc_state[FRAMES];
static int sc_w, sc_h;
static ID3D12RootSignature *root;
static ID3D12DescriptorHeap *rtv_heap, *dsv_heap, *srv_heap;
static UINT rtv_step, dsv_step, srv_step, srv_tables;
static uint8_t rtv_taken[RTVS];         /* each render-target view slot: held */
static Tex tex[MAX_TEXTURES];
static Tex none_colour, none_index, none_array;
static Tex screen_ms[3], screen_ds[3], screen_res[3], picture, output;
static uint64_t screen_used[3], frame_no;
static Buf ubuf, dbuf, staging, readback;
static Code game_ps[MAX_SHADERS];
static int targets_w, targets_h, last_w, last_h, ever;

/* n slots together, the first that are free; -1 none */
static int rtv_alloc(int n)
{
    int at, k;
    for (at = 0; at + n <= RTVS; at++) {
        for (k = 0; k < n && !rtv_taken[at + k]; k++)
            ;
        if (k == n) {
            memset(rtv_taken + at, 1, (size_t)n);
            return at;
        }
        at += k;
    }
    return -1;
}

static void rtv_release(int at, int n)
{
    if (n > 0)
        memset(rtv_taken + at, 0, (size_t)n);
}

/* RTV slots: 0-2 the screens, 3 the output, 4 the picture, then textures drawn into */
static D3D12_CPU_DESCRIPTOR_HANDLE rtv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &h);
    h.ptr += (SIZE_T)i * rtv_step;
    return h;
}

static D3D12_CPU_DESCRIPTOR_HANDLE dsv(int i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    dsv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(dsv_heap, &h);
    h.ptr += (SIZE_T)i * dsv_step;
    return h;
}

static void wait_gpu(void)
{
    if (ID3D12Fence_GetCompletedValue(fence) < fence_at) {
        ID3D12Fence_SetEventOnCompletion(fence, fence_at, fence_event);
        WaitForSingleObject(fence_event, INFINITE);
    }
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

static void buf_make(Buf *b, UINT64 size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC d;
    D3D12_RANGE none = { 0, 0 };
    memset(&hp, 0, sizeof hp);
    memset(&d, 0, sizeof d);
    hp.Type = heap;
    d.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    d.Width = size;
    d.Height = d.DepthOrArraySize = d.MipLevels = 1;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&b->r))) {
        fprintf(stderr, "rr: d3d12: a buffer of %llu bytes could not be made\n", (unsigned long long)size);
        exit(70);
    }
    ID3D12Resource_Map(b->r, 0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : NULL, &b->at);
    b->size = size;
}

static void buf_free(Buf *b)
{
    if (b->r)
        ID3D12Resource_Release(b->r);
    memset(b, 0, sizeof *b);
}

static void buf_fit(Buf *b, UINT64 size)
{
    if (b->size >= size)
        return;
    buf_free(b);
    buf_make(b, size * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
}

/* one subresource of a texture, from and back to the state the whole is in */
static void transition_level(ID3D12Resource *r, UINT sub, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = sub;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES *state, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    if (*state == to)
        return;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = *state;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
    *state = to;
}

/* a texture let go; its render-target views' slots are kept for the next made in its place */
static void tex_free(Tex *t)
{
    if (t->r)
        ID3D12Resource_Release(t->r);
    t->r = NULL;
    free(t->fresh);
    t->fresh = NULL;
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

/* a texture of `levels` levels; samples > 1 or flags make it a target: RT (colour), DS (depth) */
static void tex_make_layers(Tex *t, DXGI_FORMAT f, int w, int h, int samples, int levels, int layers, int array, D3D12_RESOURCE_FLAGS flags,
                            D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC d;
    D3D12_CLEAR_VALUE cv, *pcv = NULL;
    tex_free(t);
    memset(&hp, 0, sizeof hp);
    memset(&d, 0, sizeof d);
    memset(&cv, 0, sizeof cv);
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT64)w;
    d.Height = (UINT)h;
    d.DepthOrArraySize = (UINT16)layers;
    d.MipLevels = (UINT16)levels;
    d.Format = f;
    d.SampleDesc.Count = (UINT)samples;
    d.Flags = flags;
    if (flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) {
        cv.Format = f;
        cv.DepthStencil.Depth = 1.0f;
        pcv = &cv;
    }
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, pcv, &IID_ID3D12Resource, (void **)&t->r))) {
        fprintf(stderr, "rr: d3d12: a texture of %d x %d could not be made\n", w, h);
        exit(70);
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
    t->state = state;
}

static void tex_make(Tex *t, DXGI_FORMAT f, int w, int h, int samples, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    tex_make_layers(t, f, w, h, samples, 1, 1, 0, flags, state);
}

static DXGI_FORMAT format_of(int f) { return f == RR_R8U ? DXGI_FORMAT_R8_UINT : f == RR_R32U ? DXGI_FORMAT_R32_UINT : COLOUR; }
static int bytes_of(int f) { return f == RR_R8U ? 1 : 4; }

static void swapchain_buffers(void)
{
    UINT i;
    for (i = 0; i < FRAMES; i++) {
        sc_img[i] = NULL;
        IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&sc_img[i]);
        sc_state[i] = D3D12_RESOURCE_STATE_PRESENT;
    }
}

/* the shader-visible heap: room for `tables` tables of RR_TEX_SLOTS views */
static void srv_heap_fit(UINT tables)
{
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    if (srv_heap && srv_tables >= tables)
        return;
    if (srv_heap)
        ID3D12DescriptorHeap_Release(srv_heap);
    srv_tables = srv_tables ? srv_tables : 1024;
    while (srv_tables < tables)
        srv_tables *= 2;
    memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = srv_tables * RR_TEX_SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (FAILED(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap))) {
        fprintf(stderr, "rr: d3d12: a descriptor heap of %u tables could not be made\n", srv_tables);
        exit(70);
    }
}

int rr_open(void)
{
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;
    HWND hwnd;

    if (FAILED(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory)))
        return 0;
    if (FAILED(D3D12CreateDevice(NULL, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev)))
        return 0;
    memset(&qd, 0, sizeof qd);
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    if (FAILED(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue)))
        return 0;
    if (FAILED(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc)))
        return 0;
    if (FAILED(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list)))
        return 0;
    ID3D12GraphicsCommandList_Close(list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = RTVS;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    memset(rtv_taken, 1, 5);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 3;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    dsv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    srv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    srv_heap_fit(1024);

    {
        /* b0 the draw's uniforms, t1 the frame's words, t2-t9 its textures, s2-s9 their samplers:
           s3, s4, s8 and s9 filtered, anisotropic and repeating, the rest at texels */
        D3D12_ROOT_PARAMETER p[3];
        D3D12_DESCRIPTOR_RANGE r;
        D3D12_STATIC_SAMPLER_DESC s[RR_TEX_SLOTS];
        D3D12_ROOT_SIGNATURE_DESC rs;
        ID3DBlob *blob = NULL, *err = NULL;
        int k;
        memset(p, 0, sizeof p);
        memset(&r, 0, sizeof r);
        memset(s, 0, sizeof s);
        memset(&rs, 0, sizeof rs);
        p[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[1].Descriptor.ShaderRegister = 1;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = RR_TEX_SLOTS;
        r.BaseShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &r;
        p[0].ShaderVisibility = p[1].ShaderVisibility = p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        for (k = 0; k < RR_TEX_SLOTS; k++) {
            int f = k == 1 || k == 2 || k >= 6;
            s[k].Filter = f ? D3D12_FILTER_ANISOTROPIC : D3D12_FILTER_MIN_MAG_MIP_POINT;
            s[k].MaxAnisotropy = f ? 16 : 1;
            s[k].AddressU = s[k].AddressV = s[k].AddressW = f ? D3D12_TEXTURE_ADDRESS_MODE_WRAP : D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            s[k].MaxLOD = D3D12_FLOAT32_MAX;
            s[k].ShaderRegister = (UINT)(2 + k);
            s[k].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        }
        rs.NumParameters = 3;
        rs.pParameters = p;
        rs.NumStaticSamplers = RR_TEX_SLOTS;
        rs.pStaticSamplers = s;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
            return 0;
        if (FAILED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                    &IID_ID3D12RootSignature, (void **)&root))) {
            ID3D10Blob_Release(blob);
            return 0;
        }
        ID3D10Blob_Release(blob);
    }
    tex_make(&none_colour, COLOUR, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    tex_make(&none_index, DXGI_FORMAT_R8_UINT, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    tex_make_layers(&none_array, COLOUR, 1, 1, 1, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    none_colour.format = RR_BGRA8;
    none_index.format = RR_R8U;
    none_array.format = RR_BGRA8;

    hwnd = (HWND)host_window_handle();
    if (hwnd) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        IDXGISwapChain1 *s1 = NULL;
        int w = 0, h = 0;
        host_window_pixels(&w, &h);
        memset(&sd, 0, sizeof sd);
        sd.Width = (UINT)(w > 0 ? w : 256);
        sd.Height = (UINT)(h > 0 ? h : 224);
        sd.Format = COLOUR;
        sd.SampleDesc.Count = 1;
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.BufferCount = FRAMES;
        sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        if (FAILED(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, hwnd, &sd, NULL, NULL, &s1)))
            return 0;
        IDXGISwapChain1_QueryInterface(s1, &IID_IDXGISwapChain3, (void **)&swapchain);
        IDXGISwapChain1_Release(s1);
        sc_w = (int)sd.Width;
        sc_h = (int)sd.Height;
        swapchain_buffers();
    }
    return 1;
}

const char *rr_name(void) { return "d3d12"; }
const char *rr_shader_ext(void) { return "dxil"; }

/* ---- pipelines: made when first asked for, a failure kept too, found by hashing ---- */
static int integer_kind(int kind) { return kind == KIND_INDEX || kind == KIND_INDEX32; }

/* blending, where the target can blend: an integer target never does */
static void blend_of(D3D12_RENDER_TARGET_BLEND_DESC *a, int blend, int kind)
{
    memset(a, 0, sizeof *a);
    a->RenderTargetWriteMask = blend == RR_BLEND_NOCOLOUR ? 0 : D3D12_COLOR_WRITE_ENABLE_ALL;
    a->BlendEnable = blend == RR_BLEND_OVER && !integer_kind(kind);
    a->LogicOp = D3D12_LOGIC_OP_NOOP;
    a->BlendOp = a->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    a->SrcBlend = a->SrcBlendAlpha = D3D12_BLEND_ONE;
    a->DestBlend = a->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
}

typedef struct { UINT64 key; ID3D12PipelineState *p; int used; } PipeSlot;
static PipeSlot *pipes;
static size_t npipes, cap_pipes;

static PipeSlot *pipe_slot(UINT64 key)
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

static DXGI_FORMAT kind_format(int kind) { return kind == KIND_INDEX ? DXGI_FORMAT_R8_UINT : kind == KIND_INDEX32 ? DXGI_FORMAT_R32_UINT : COLOUR; }

static ID3D12PipelineState *pipe_of(const rr_cmd *c, int kind)
{
    int depth = kind == KIND_SCREEN ? c->depth : 0, stencil = kind == KIND_SCREEN ? c->stencil : 0;
    UINT64 key = (UINT64)c->program | (UINT64)c->shader << 8 | (UINT64)c->blend << 24 | (UINT64)depth << 28 | (UINT64)stencil << 30 |
                 (UINT64)kind << 33 | (UINT64)(stencil ? c->read_mask : 0) << 40 | (UINT64)(stencil ? c->write_mask : 0) << 48;
    PipeSlot *slot = pipe_slot(key);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    D3D12_DEPTH_STENCILOP_DESC so;
    ID3D12PipelineState *p = NULL;
    if (slot->used)
        return slot->p;
    slot->used = 1;
    slot->key = key;
    slot->p = NULL;
    npipes++;
    if (c->shader && (c->shader >= MAX_SHADERS || !game_ps[c->shader].code)) {
        fprintf(stderr, "rr: d3d12: no game shader %u\n", c->shader);
        return NULL;
    }
    memset(&d, 0, sizeof d);
    d.pRootSignature = root;
    d.VS.pShaderBytecode = programs[c->program].vs;
    d.VS.BytecodeLength = programs[c->program].vs_size;
    d.PS.pShaderBytecode = c->shader ? game_ps[c->shader].code : programs[c->program].fs;
    d.PS.BytecodeLength = c->shader ? game_ps[c->shader].size : programs[c->program].fs_size;
    blend_of(&d.BlendState.RenderTarget[0], c->blend, kind);
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kind_format(kind);
    d.SampleDesc.Count = kind == KIND_SCREEN ? RR_SAMPLES : 1;
    if (kind == KIND_SCREEN) {
        d.DSVFormat = DEPTH;
        d.DepthStencilState.DepthEnable = depth == RR_DEPTH_LEQUAL;
        d.DepthStencilState.DepthWriteMask = depth == RR_DEPTH_LEQUAL ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
        d.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
        d.DepthStencilState.StencilEnable = stencil != RR_STENCIL_OFF;
        d.DepthStencilState.StencilReadMask = c->read_mask;
        d.DepthStencilState.StencilWriteMask = c->write_mask;
        so.StencilFailOp = so.StencilDepthFailOp = so.StencilPassOp = D3D12_STENCIL_OP_KEEP;
        so.StencilFunc = D3D12_COMPARISON_FUNC_EQUAL;
        if (stencil == RR_STENCIL_EQUAL_INC)
            so.StencilDepthFailOp = so.StencilPassOp = D3D12_STENCIL_OP_INCR_SAT;
        else if (stencil == RR_STENCIL_SET) {
            so.StencilFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            so.StencilDepthFailOp = so.StencilPassOp = D3D12_STENCIL_OP_REPLACE;
        }
        d.DepthStencilState.FrontFace = d.DepthStencilState.BackFace = so;
    }
    if (FAILED(ID3D12Device_CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&p))) {
        fprintf(stderr, "rr: d3d12: pipeline %u could not be made\n", c->program);
        p = NULL;
    }
    slot->p = p;
    return p;
}

/* ---- descriptor tables: one for each set of textures a frame binds, found by hashing ---- */
typedef struct { const Tex *t[RR_TEX_SLOTS]; UINT at; int used; } Table;
static Table *tables;
static size_t cap_table_slots;
static UINT ntables;

static const Tex *slot_tex(int32_t t, int slot)
{
    const Tex *r = NULL;
    if (t == RR_TEX_FRAME)
        r = &screen_res[0];
    else if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        r = &screen_res[t == RR_TEX_EXTRA0 ? 1 : 2];
    else if (t == RR_TEX_PICTURE)
        r = &picture;
    else if (t > 0 && t < MAX_TEXTURES && (slot >= 6) == (tex[t].array != 0))
        r = &tex[t];
    if (r && r->r)
        return r;
    return slot >= 6 ? &none_array : slot < 3 ? &none_colour : &none_index;
}

/* t's view at descriptor `at`: every level (and layer), or level `only` alone
   (>= 0), of layer `layer` alone if an array */
static void srv_put_layer(UINT at, const Tex *t, int only, int layer)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    memset(&sv, 0, sizeof sv);
    sv.Format = format_of(t->format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (t->array) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
        sv.Texture2DArray.MostDetailedMip = only >= 0 ? (UINT)only : 0;
        sv.Texture2DArray.MipLevels = only >= 0 ? 1 : (UINT)(t->levels > 0 ? t->levels : 1);
        sv.Texture2DArray.FirstArraySlice = only >= 0 ? (UINT)layer : 0;
        sv.Texture2DArray.ArraySize = only >= 0 ? 1 : (UINT)(t->layers > 0 ? t->layers : 1);
    } else {
        sv.Texture2D.MostDetailedMip = only >= 0 ? (UINT)only : 0;
        sv.Texture2D.MipLevels = only >= 0 ? 1 : (UINT)(t->levels > 0 ? t->levels : 1);
    }
    srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap, &h);
    h.ptr += (SIZE_T)at * srv_step;
    ID3D12Device_CreateShaderResourceView(dev, t->r, &sv, h);
}

static void srv_put(UINT at, const Tex *t) { srv_put_layer(at, t, -1, 0); }

static uint64_t tables_hash(const Tex *const *t)
{
    uint64_t h = 0;
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        h = mix64(h ^ (uint64_t)(uintptr_t)t[k] ^ (uint64_t)k << 56);
    return h;
}

/* a frame's tables made afresh: room for `most` of them, the heap made larger if need be (the frame before finished) */
static void tables_reset(UINT most)
{
    size_t c = cap_table_slots ? cap_table_slots : 256;
    srv_heap_fit(most);
    while (c < (size_t)most * 2)
        c *= 2;
    if (c != cap_table_slots) {
        free(tables);
        tables = (Table *)malloc(c * sizeof *tables);
        if (!tables)
            exit(70);
        cap_table_slots = c;
    }
    memset(tables, 0, cap_table_slots * sizeof *tables);
    ntables = 0;
}

static D3D12_GPU_DESCRIPTOR_HANDLE table_at(UINT at)
{
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &g);
    g.ptr += (UINT64)at * srv_step;
    return g;
}

static D3D12_GPU_DESCRIPTOR_HANDLE table_of(const int32_t *ids)
{
    const Tex *t[RR_TEX_SLOTS];
    size_t i;
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        t[k] = slot_tex(ids ? ids[k] : 0, k);
    for (i = tables_hash(t) & (cap_table_slots - 1); tables[i].used; i = (i + 1) & (cap_table_slots - 1))
        if (!memcmp(tables[i].t, t, sizeof t))
            return table_at(tables[i].at);
    tables[i].used = 1;
    memcpy(tables[i].t, t, sizeof t);
    tables[i].at = ntables++ * RR_TEX_SLOTS;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        srv_put(tables[i].at + (UINT)k, t[k]);
    return table_at(tables[i].at);
}

/* a table whose first view is level `level` of t alone (of layer `layer` if an array), for making the next */
static D3D12_GPU_DESCRIPTOR_HANDLE table_level(const Tex *t, int level, int layer)
{
    UINT at = ntables++ * RR_TEX_SLOTS;
    int k;
    srv_put_layer(at, t, level, layer);
    for (k = 1; k < RR_TEX_SLOTS; k++)
        srv_put(at + (UINT)k, k >= 6 ? &none_array : k < 3 ? &none_colour : &none_index);
    return table_at(at);
}

/* ---- a frame ---- */
static void screen_free(int k)
{
    tex_free(&screen_ms[k]);
    tex_free(&screen_ds[k]);
    tex_free(&screen_res[k]);
    screen_ms[k].w = screen_ms[k].h = 0;
}

/* a screen target (0 the frame, 1 and 2 the extra ones) of w x h */
static void screen_fit(int k, int w, int h)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    if (screen_ms[k].r && screen_ms[k].w == w && screen_ms[k].h == h)
        return;
    memset(&dv, 0, sizeof dv);
    dv.Format = DEPTH;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    tex_make(&screen_ms[k], COLOUR, w, h, RR_SAMPLES, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
    tex_make(&screen_ds[k], DEPTH, w, h, RR_SAMPLES, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    ID3D12Device_CreateRenderTargetView(dev, screen_ms[k].r, NULL, rtv(k));
    ID3D12Device_CreateDepthStencilView(dev, screen_ds[k].r, &dv, dsv(k));
    tex_make(&screen_res[k], COLOUR, w, h, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    screen_res[k].format = RR_BGRA8;
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
            screen_fit(k, w, h);
            screen_used[k] = frame_no;
        } else if (screen_ms[k].r && frame_no - screen_used[k] > UNUSED_FRAMES) {
            screen_free(k);
        }
    }
    if (targets_w == w && targets_h == h)
        return;
    tex_make(&output, COLOUR, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, SHADER_READ);
    output.format = RR_BGRA8;
    output.rtv = 3;
    ID3D12Device_CreateRenderTargetView(dev, output.r, NULL, rtv(3));
    tex_make(&picture, COLOUR, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, SHADER_READ);
    picture.format = RR_BGRA8;
    picture.rtv = 4;
    ID3D12Device_CreateRenderTargetView(dev, picture.r, NULL, rtv(4));
    targets_w = w;
    targets_h = h;
}

/* the frame's textures made, before anything reads their sizes */
static void make_textures(const rr_frame *f)
{
    size_t i;
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        Tex *t;
        int levels, lv, ly, array, layers;
        if (o->op != RR_TEX_CREATE)
            continue;
        if (o->id >= MAX_TEXTURES) {
            fprintf(stderr, "rr: d3d12: texture %u past the most\n", o->id);
            continue;
        }
        t = &tex[o->id];
        levels = o->flags & RR_TEX_MIPS && o->format == RR_BGRA8 ? levels_of((int)o->w, (int)o->h) : 1;
        array = (o->flags & RR_TEX_ARRAY) != 0;
        layers = array && o->layers ? (int)o->layers : 1;
        tex_make_layers(t, format_of(o->format), (int)o->w, (int)o->h, 1, levels, layers, array,
                        o->flags & RR_TEX_TARGET ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
        t->format = o->format;
        t->target = (o->flags & RR_TEX_TARGET) != 0;
        if (!t->target || t->rtvs < levels * layers) {
            /* the slots it held given back, unless they are enough for it now */
            rtv_release(t->rtv, t->rtvs);
            t->rtv = t->rtvs = 0;
        }
        if (t->target) {
            /* a render-target view for each layer's each level */
            if (!t->rtvs) {
                int at = rtv_alloc(levels * layers);
                if (at < 0) {
                    fprintf(stderr, "rr: d3d12: more than %d levels of textures drawn into\n", RTVS);
                    exit(70);
                }
                t->rtv = at;
                t->rtvs = levels * layers;
            }
            for (ly = 0; ly < layers; ly++)
                for (lv = 0; lv < levels; lv++) {
                    D3D12_RENDER_TARGET_VIEW_DESC rv;
                    memset(&rv, 0, sizeof rv);
                    rv.Format = format_of(o->format);
                    if (array) {
                        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
                        rv.Texture2DArray.MipSlice = (UINT)lv;
                        rv.Texture2DArray.FirstArraySlice = (UINT)ly;
                        rv.Texture2DArray.ArraySize = 1;
                    } else {
                        rv.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
                        rv.Texture2D.MipSlice = (UINT)lv;
                    }
                    ID3D12Device_CreateRenderTargetView(dev, t->r, &rv, rtv(t->rtv + ly * levels + lv));
                }
        }
    }
}

/* the frame's texture writes, into the command list */
static void record_texops(const rr_frame *f)
{
    size_t i;
    UINT64 need = 0, at = 0;
    /* the upload buffer: each write's rows at a pitch of 256 bytes, from 512-byte places */
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        if (o->op == RR_TEX_WRITE && o->id < MAX_TEXTURES)
            need += 512 + ((UINT64)o->w * 4 + 255) / 256 * 256 * o->h;
    }
    if (need)
        buf_fit(&staging, need);
    for (i = 0; i < f->ntexops; i++) {
        const rr_tex_op *o = &f->texops[i];
        Tex *t;
        D3D12_TEXTURE_COPY_LOCATION to, from;
        UINT row, pitch, y;
        if (o->op != RR_TEX_WRITE || o->id >= MAX_TEXTURES || !tex[o->id].r)
            continue;
        t = &tex[o->id];
        row = o->w * (UINT)bytes_of(t->format);
        pitch = (row + 255) / 256 * 256;
        at = (at + 511) / 512 * 512;
        for (y = 0; y < o->h; y++)
            memcpy((char *)staging.at + at + (UINT64)y * pitch, f->upload + o->at + (size_t)y * row, row);
        memset(&to, 0, sizeof to);
        memset(&from, 0, sizeof from);
        from.pResource = staging.r;
        from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        from.PlacedFootprint.Offset = at;
        from.PlacedFootprint.Footprint.Format = format_of(t->format);
        from.PlacedFootprint.Footprint.Width = o->w;
        from.PlacedFootprint.Footprint.Height = o->h;
        from.PlacedFootprint.Footprint.Depth = 1;
        from.PlacedFootprint.Footprint.RowPitch = pitch;
        to.pResource = t->r;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        transition(t->r, &t->state, D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, o->x, o->y, 0, &from, NULL);
        transition(t->r, &t->state, SHADER_READ);
        at += (UINT64)pitch * o->h;
    }
}

/* a pass's target: its kind, size, and whether it can be drawn into */
static int pass_target(const rr_pass *ps, int ow, int oh, int *kind, int *tw, int *th, Tex **to)
{
    *to = NULL;
    *tw = ow;
    *th = oh;
    if (ps->target <= RR_TARGET_EXTRA1) {
        *kind = KIND_SCREEN;
        return screen_ms[ps->target].r != NULL;
    }
    if (ps->target == RR_TARGET_TEXTURE) {
        Tex *t;
        if (ps->texture >= MAX_TEXTURES)
            return 0;
        t = &tex[ps->texture];
        /* a texture not made to be drawn into, or an array's layer it has not, is no target */
        if (!t->r || !t->target || (t->array && ps->layer >= (uint32_t)t->layers))
            return 0;
        *to = t;
        *kind = t->format == RR_R8U ? KIND_INDEX : t->format == RR_R32U ? KIND_INDEX32 : KIND_COLOUR;
    } else {
        *to = ps->target == RR_TARGET_PICTURE ? &picture : &output;
        *kind = KIND_COLOUR;
    }
    *tw = (*to)->w;
    *th = (*to)->h;
    return 1;
}

int rr_draw(rr_frame *f)
{
    size_t mip_blocks = 0, mip_at = 0, i, p;
    int ow = f->width, oh = f->height, paced = 0, back = 0, show = 0;
    float aspect = f->aspect;
    ID3D12CommandList *lists[1];

    wait_gpu();
    frame_no++;
    for (i = 0; i < f->nshaders; i++) {
        /* kept, for its pipelines to be made as they are needed; let go at the end */
        uint32_t id = f->shaders[i].id;
        if (id && id < MAX_SHADERS) {
            free(game_ps[id].code);
            game_ps[id].code = (void *)f->shaders[i].code;
            game_ps[id].size = f->shaders[i].size;
        } else {
            fprintf(stderr, "rr: d3d12: shader %u past the most\n", id);
            free((void *)f->shaders[i].code);
        }
        f->shaders[i].code = NULL;
    }
    f->nshaders = 0;
    if (swapchain) {
        int ww = 0, wh = 0;
        host_window_pixels(&ww, &wh);
        if (ww >= 1 && wh >= 1) {
            show = 1;
            if (ww != sc_w || wh != sc_h) {
                UINT j;
                HRESULT hr;
                for (j = 0; j < FRAMES; j++)
                    if (sc_img[j])
                        ID3D12Resource_Release(sc_img[j]);
                hr = IDXGISwapChain3_ResizeBuffers(swapchain, FRAMES, (UINT)ww, (UINT)wh, DXGI_FORMAT_UNKNOWN, 0);
                /* the buffers again, made at the new size or kept at the old */
                swapchain_buffers();
                if (SUCCEEDED(hr)) {
                    sc_w = ww;
                    sc_h = wh;
                } else {
                    fprintf(stderr, "rr: d3d12: the window's buffers could not be made %d x %d (0x%08lx)\n", ww, wh, (unsigned long)hr);
                    show = 0;
                }
            }
            ow = ww;
            oh = wh;
            back = (int)IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
            if (!sc_img[back])
                show = 0;
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

    /* everything this frame makes, made before anything reads a size */
    targets_fit(f, ow, oh);
    make_textures(f);

    /* the words and the uniforms, each block given its target's size and samples */
    buf_fit(&dbuf, (f->data_words + 1) * 4);
    memcpy(dbuf.at, f->data, f->data_words * 4);
    /* past the frame's blocks, one for each level the mips make */
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        Tex *to;
        int kind, tw, th;
        if (ps->target == RR_TARGET_TEXTURE && ps->mips && pass_target(ps, ow, oh, &kind, &tw, &th, &to) && to->levels > 1)
            mip_blocks += (size_t)(to->levels - 1);
    }
    buf_fit(&ubuf, (f->nuniforms + mip_blocks + 1) * 256);
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        Tex *to;
        int kind, tw, th, samples = ps->target <= RR_TARGET_EXTRA1 ? RR_SAMPLES : 1;
        pass_target(ps, ow, oh, &kind, &tw, &th, &to);
        for (i = ps->first; i < ps->first + ps->count; i++) {
            rr_uniform u = f->uniforms[f->cmds[i].uniform];
            u.f[0][0] = (float)tw;
            u.f[0][1] = (float)th;
            u.f[0][2] = (float)samples;
            u.f[0][3] = -1.0f;
            memcpy((char *)ubuf.at + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
        }
    }
    /* room for a table for every draw and every level made, however few are different */
    tables_reset((UINT)(f->ncmds + mip_blocks + 1));

    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(list, 1, ID3D12Resource_GetGPUVirtualAddress(dbuf.r));
    record_texops(f);
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        D3D12_CPU_DESCRIPTOR_HANDLE rt;
        int kind, tw, th, layer = 0, vx, vy, vw, vh;
        Tex *to;
        if (!pass_target(ps, ow, oh, &kind, &tw, &th, &to)) {
            if (ps->count)
                fprintf(stderr, "rr: d3d12: a pass into no target (%u, texture %u, layer %u) left out\n", ps->target, ps->texture, ps->layer);
            continue;
        }
        /* the view: the frame's units shown at its aspect, centred in this target */
        rr_view(aspect, tw, th, &vx, &vy, &vw, &vh);
        if (kind == KIND_SCREEN) {
            D3D12_CPU_DESCRIPTOR_HANDLE ds = dsv(ps->target);
            rt = rtv(ps->target);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rt, FALSE, &ds);
            ID3D12GraphicsCommandList_ClearRenderTargetView(list, rt, ps->clear, 0, NULL);
            ID3D12GraphicsCommandList_ClearDepthStencilView(list, ds, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
        } else {
            layer = to->array ? (int)ps->layer : 0;
            transition(to->r, &to->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            rt = rtv(to->rtv + layer * to->levels);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rt, FALSE, NULL);
            /* kept, unless just made: then cleared */
            if (!ps->load || to->fresh[layer]) {
                FLOAT clear[4];
                memcpy(clear, ps->clear, sizeof clear);
                if (integer_kind(kind))
                    clear[1] = clear[2] = clear[3] = 0;
                ID3D12GraphicsCommandList_ClearRenderTargetView(list, rt, clear, 0, NULL);
            }
            to->fresh[layer] = 0;
        }
        for (i = ps->first; i < ps->first + ps->count; i++) {
            const rr_cmd *c = &f->cmds[i];
            ID3D12PipelineState *pl = pipe_of(c, kind);
            D3D12_VIEWPORT vp;
            D3D12_RECT sc;
            int view = c->space == RR_SPACE_VIEW;
            if (!pl || !c->vertices || !c->instances)
                continue;
            vp.TopLeftX = view ? (FLOAT)vx : 0.0f;
            vp.TopLeftY = view ? (FLOAT)vy : 0.0f;
            vp.Width = view ? (FLOAT)vw : (FLOAT)tw;
            vp.Height = view ? (FLOAT)vh : (FLOAT)th;
            vp.MinDepth = 0;
            vp.MaxDepth = 1;
            sc.left = sc.top = 0;
            sc.right = tw;
            sc.bottom = th;
            if (c->scissor) {
                /* the rectangle in the draw's own units: the frame's in view space, target pixels else */
                int x0, y0, x1, y1;
                rr_scissor(f->uniforms[c->uniform].f[2], view ? f->width : tw, view ? f->height : th, (int)vp.TopLeftX, (int)vp.TopLeftY,
                           (int)vp.Width, (int)vp.Height, &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                sc.left = x0;
                sc.top = y0;
                sc.right = x1;
                sc.bottom = y1;
            }
            ID3D12GraphicsCommandList_SetPipelineState(list, pl);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(ubuf.r) + (UINT64)c->uniform * 256);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, table_of(c->tex));
            ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
            ID3D12GraphicsCommandList_OMSetStencilRef(list, c->ref);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_DrawInstanced(list, c->vertices, c->instances, 0, 0);
        }
        if (kind == KIND_SCREEN) {
            /* resolved into its texture */
            Tex *dst = &screen_res[ps->target];
            Tex *src = &screen_ms[ps->target];
            transition(src->r, &src->state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(dst->r, &dst->state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            ID3D12GraphicsCommandList_ResolveSubresource(list, dst->r, 0, src->r, 0, COLOUR);
            transition(src->r, &src->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(dst->r, &dst->state, SHADER_READ);
            continue;
        }
        transition(to->r, &to->state, SHADER_READ);
        if (ps->target == RR_TARGET_TEXTURE && ps->mips && to->levels > 1 && !integer_kind(kind)) {
            /* its smaller levels (that layer's), each the mean of the one before (the renderer's own program) */
            rr_cmd mc;
            ID3D12PipelineState *pl;
            int lv, w = to->w, h = to->h;
            memset(&mc, 0, sizeof mc);
            mc.program = to->array ? RR_MIPS_LAYER : RR_MIPS;
            pl = pipe_of(&mc, KIND_COLOUR);
            for (lv = 1; pl && lv < to->levels && mip_at < mip_blocks; lv++) {
                D3D12_CPU_DESCRIPTOR_HANDLE lr = rtv(to->rtv + layer * to->levels + lv);
                UINT sub = (UINT)(lv + layer * to->levels);
                D3D12_VIEWPORT vp;
                D3D12_RECT sc;
                rr_uniform u;
                w = w > 1 ? w / 2 : 1;
                h = h > 1 ? h / 2 : 1;
                memset(&u, 0, sizeof u);
                u.f[0][0] = (float)w;
                u.f[0][1] = (float)h;
                u.f[0][2] = 1.0f;
                u.f[0][3] = -1.0f;
                memcpy((char *)ubuf.at + (f->nuniforms + mip_at) * 256, &u, sizeof u);
                vp.TopLeftX = vp.TopLeftY = 0;
                vp.Width = (FLOAT)w;
                vp.Height = (FLOAT)h;
                vp.MinDepth = 0;
                vp.MaxDepth = 1;
                sc.left = sc.top = 0;
                sc.right = w;
                sc.bottom = h;
                transition_level(to->r, sub, SHADER_READ, D3D12_RESOURCE_STATE_RENDER_TARGET);
                ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &lr, FALSE, NULL);
                ID3D12GraphicsCommandList_SetPipelineState(list, pl);
                ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(ubuf.r) +
                                                                                (UINT64)(f->nuniforms + mip_at) * 256);
                ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, table_level(to, lv - 1, layer));
                ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
                ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
                ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
                ID3D12GraphicsCommandList_DrawInstanced(list, 6, 1, 0, 0);
                transition_level(to->r, sub, D3D12_RESOURCE_STATE_RENDER_TARGET, SHADER_READ);
                mip_at++;
            }
        }
    }
    if (show) {
        /* the output, shown */
        transition(output.r, &output.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        transition(sc_img[back], &sc_state[back], D3D12_RESOURCE_STATE_COPY_DEST);
        ID3D12GraphicsCommandList_CopyResource(list, sc_img[back], output.r);
        transition(sc_img[back], &sc_state[back], D3D12_RESOURCE_STATE_PRESENT);
        transition(output.r, &output.state, SHADER_READ);
    }
    ID3D12GraphicsCommandList_Close(list);
    lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    if (show) {
        IDXGISwapChain3_Present(swapchain, 1, 0);
        paced = 1;
    }
    ID3D12CommandQueue_Signal(queue, fence, ++fence_at);
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
        D3D12_TEXTURE_COPY_LOCATION to, from;
        ID3D12CommandList *lists[1];
        UINT pitch = ((UINT)last_w * 4 + 255) / 256 * 256;
        UINT64 need = (UINT64)pitch * (UINT64)last_h;
        int y;
        wait_gpu();
        if (readback.size < need) {
            buf_free(&readback);
            buf_make(&readback, need, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        ID3D12CommandAllocator_Reset(alloc);
        ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
        memset(&to, 0, sizeof to);
        memset(&from, 0, sizeof from);
        to.pResource = readback.r;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = COLOUR;
        to.PlacedFootprint.Footprint.Width = (UINT)last_w;
        to.PlacedFootprint.Footprint.Height = (UINT)last_h;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = pitch;
        from.pResource = output.r;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        transition(output.r, &output.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
        transition(output.r, &output.state, SHADER_READ);
        ID3D12GraphicsCommandList_Close(list);
        lists[0] = (ID3D12CommandList *)list;
        ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
        ID3D12CommandQueue_Signal(queue, fence, ++fence_at);
        wait_gpu();
        /* BGRA bytes are 0xAARRGGBB words */
        for (y = 0; y < last_h; y++)
            memcpy(argb + (size_t)y * last_w, (const char *)readback.at + (size_t)y * pitch, (size_t)last_w * 4);
    }
    return 1;
}

void rr_close(void)
{
    size_t i;
    int k;
    if (!queue)
        return;
    ID3D12CommandQueue_Signal(queue, fence, ++fence_at);
    wait_gpu();
    for (i = 0; i < cap_pipes; i++)
        if (pipes[i].used && pipes[i].p)
            ID3D12PipelineState_Release(pipes[i].p);
    free(pipes);
    pipes = NULL;
    cap_pipes = npipes = 0;
    free(tables);
    tables = NULL;
    cap_table_slots = 0;
    for (k = 0; k < MAX_TEXTURES; k++)
        tex_free(&tex[k]);
    for (k = 0; k < 3; k++)
        screen_free(k);
    tex_free(&picture);
    tex_free(&output);
    tex_free(&none_colour);
    tex_free(&none_index);
    tex_free(&none_array);
    for (k = 0; k < MAX_SHADERS; k++) {
        free(game_ps[k].code);
        game_ps[k].code = NULL;
    }
    buf_free(&ubuf);
    buf_free(&dbuf);
    buf_free(&staging);
    buf_free(&readback);
    for (k = 0; k < FRAMES; k++)
        if (sc_img[k]) {
            ID3D12Resource_Release(sc_img[k]);
            sc_img[k] = NULL;
        }
    if (swapchain)
        IDXGISwapChain3_Release(swapchain);
    if (srv_heap)
        ID3D12DescriptorHeap_Release(srv_heap);
    if (rtv_heap)
        ID3D12DescriptorHeap_Release(rtv_heap);
    if (dsv_heap)
        ID3D12DescriptorHeap_Release(dsv_heap);
    if (root)
        ID3D12RootSignature_Release(root);
    if (list)
        ID3D12GraphicsCommandList_Release(list);
    if (alloc)
        ID3D12CommandAllocator_Release(alloc);
    if (fence)
        ID3D12Fence_Release(fence);
    if (fence_event)
        CloseHandle(fence_event);
    ID3D12CommandQueue_Release(queue);
    queue = NULL;
    if (dev)
        ID3D12Device_Release(dev);
    if (factory)
        IDXGIFactory4_Release(factory);
}
