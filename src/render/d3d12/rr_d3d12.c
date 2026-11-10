/*
 * rr_d3d12.c: the renderer (rr.h) on Direct3D 12 (Windows). The frame's
 * textures are default-heap textures, written through an upload buffer at
 * the frame's start; its words are a root shader resource and each draw's
 * uniforms a root constant buffer; its textures a descriptor table. The
 * multisampled targets are resolved with ResolveSubresource. Pipelines are
 * made when first used. The programs are DXIL (rr_dxil.h). One frame is in
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
#define MAX_PIPES 4096
#define MAX_TABLES 8192
#define COLOUR DXGI_FORMAT_B8G8R8A8_UNORM
#define DEPTH DXGI_FORMAT_D24_UNORM_S8_UINT
#define SHADER_READ (D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)

enum { KIND_SCREEN, KIND_INDEX, KIND_COLOUR, KINDS };

typedef struct { ID3D12Resource *r; void *at; UINT64 size; } Buf;
typedef struct { ID3D12Resource *r; int w, h, format, rtv; D3D12_RESOURCE_STATES state; } Tex;
typedef struct { const void *code; size_t size; } Code;

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
static UINT rtv_step, dsv_step, srv_step;
static int rtv_next;
static Tex tex[MAX_TEXTURES];
static Tex none_colour, none_index;
static Tex screen_ms[3], screen_ds[3], extra_res[2], output;
static Buf ubuf, dbuf, staging, readback;
static Code game_ps[MAX_SHADERS];
static UINT64 pipe_key[MAX_PIPES];
static ID3D12PipelineState *pipe_obj[MAX_PIPES];
static size_t npipes;
static int targets_w, targets_h, last_w, last_h, ever;

/* RTV slots: 0-2 the screens, 3 the output, 4.. the swapchain's, then textures drawn into */
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

static void tex_free(Tex *t)
{
    if (t->r)
        ID3D12Resource_Release(t->r);
    t->r = NULL;
}

/* a texture; samples > 1 or flags make it a target: RT (colour), DS (depth) */
static void tex_make(Tex *t, DXGI_FORMAT f, int w, int h, int samples, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
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
    d.DepthOrArraySize = d.MipLevels = 1;
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
    t->w = w;
    t->h = h;
    t->state = state;
}

static DXGI_FORMAT format_of(int f) { return f == RR_R8U ? DXGI_FORMAT_R8_UINT : f == RR_R32U ? DXGI_FORMAT_R32_UINT : COLOUR; }
static int bytes_of(int f) { return f == RR_R8U ? 1 : 4; }

static void swapchain_buffers(void)
{
    UINT i;
    for (i = 0; i < FRAMES; i++) {
        IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&sc_img[i]);
        sc_state[i] = D3D12_RESOURCE_STATE_PRESENT;
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
    ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&alloc);
    ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);
    ID3D12GraphicsCommandList_Close(list);
    ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence);
    fence_event = CreateEventA(NULL, FALSE, FALSE, NULL);

    memset(&hd, 0, sizeof hd);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 256;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    rtv_next = 4 + FRAMES;
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_DSV;
    hd.NumDescriptors = 3;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&dsv_heap);
    dsv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_DSV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = MAX_TABLES * RR_TEX_SLOTS;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);
    srv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    {
        /* b0 the draw's uniforms, t1 the frame's words, t2-t6 its textures, s2-s6 their samplers */
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
            s[k].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
            s[k].AddressU = s[k].AddressV = s[k].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
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
                                                    &IID_ID3D12RootSignature, (void **)&root)))
            return 0;
    }
    tex_make(&none_colour, COLOUR, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    tex_make(&none_index, DXGI_FORMAT_R8_UINT, 1, 1, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
    none_colour.format = RR_BGRA8;
    none_index.format = RR_R8U;

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

void rr_close(void)
{
    if (queue) {
        ID3D12CommandQueue_Signal(queue, fence, ++fence_at);
        wait_gpu();
    }
}

const char *rr_name(void) { return "d3d12"; }
const char *rr_shader_ext(void) { return "dxil"; }

/* ---- pipelines ---- */
static void blend_of(D3D12_RENDER_TARGET_BLEND_DESC *a, int blend)
{
    memset(a, 0, sizeof *a);
    a->RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    a->BlendEnable = blend != RR_BLEND_REPLACE && blend != RR_BLEND_NOCOLOUR;
    a->LogicOp = D3D12_LOGIC_OP_NOOP;
    a->BlendOp = a->BlendOpAlpha = D3D12_BLEND_OP_ADD;
    a->SrcBlend = a->SrcBlendAlpha = D3D12_BLEND_ONE;
    a->DestBlend = a->DestBlendAlpha = D3D12_BLEND_ZERO;
    switch (blend) {
    case RR_BLEND_NOCOLOUR:
        a->RenderTargetWriteMask = 0;
        break;
    case RR_BLEND_ADD_BY_A:
    case RR_BLEND_RSUB_BY_A:
        a->RenderTargetWriteMask = 7;
        a->SrcBlend = D3D12_BLEND_DEST_ALPHA;
        a->DestBlend = D3D12_BLEND_ONE;
        a->BlendOp = blend == RR_BLEND_ADD_BY_A ? D3D12_BLEND_OP_ADD : D3D12_BLEND_OP_REV_SUBTRACT;
        break;
    case RR_BLEND_MIX_BY_A:
    case RR_BLEND_RMIX_BY_A:
        a->RenderTargetWriteMask = 7;
        a->SrcBlend = D3D12_BLEND_DEST_ALPHA;
        a->DestBlend = D3D12_BLEND_INV_DEST_ALPHA;
        a->BlendOp = blend == RR_BLEND_MIX_BY_A ? D3D12_BLEND_OP_ADD : D3D12_BLEND_OP_REV_SUBTRACT;
        break;
    case RR_BLEND_A_ZERO:
        a->RenderTargetWriteMask = 8;
        a->SrcBlendAlpha = D3D12_BLEND_ZERO;
        a->DestBlendAlpha = D3D12_BLEND_ZERO;
        break;
    case RR_BLEND_RGB_ZERO_A_ADD:
        a->SrcBlend = D3D12_BLEND_ZERO;
        a->DestBlend = D3D12_BLEND_ZERO;
        a->SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
        a->DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case RR_BLEND_A_ADD:
        a->RenderTargetWriteMask = 8;
        a->SrcBlendAlpha = D3D12_BLEND_DEST_ALPHA;
        a->DestBlendAlpha = D3D12_BLEND_ONE;
        break;
    case RR_BLEND_RGB_SCALE:
        a->RenderTargetWriteMask = 7;
        a->SrcBlend = D3D12_BLEND_ZERO;
        a->DestBlend = D3D12_BLEND_BLEND_FACTOR;
        break;
    case RR_BLEND_OVER:
        a->DestBlend = a->DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
        break;
    default:
        break;
    }
}

static ID3D12PipelineState *pipe_of(const rr_cmd *c, int kind)
{
    int depth = kind == KIND_SCREEN ? c->depth : 0, stencil = kind == KIND_SCREEN ? c->stencil : 0;
    int shader = c->shader < MAX_SHADERS ? c->shader : 0;
    UINT64 key = (UINT64)c->program | (UINT64)shader << 4 | (UINT64)c->blend << 13 | (UINT64)depth << 17 | (UINT64)stencil << 19 |
                 (UINT64)c->topology << 22 | (UINT64)kind << 23 | (UINT64)(stencil ? c->read_mask : 0) << 32 |
                 (UINT64)(stencil ? c->write_mask : 0) << 40;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    D3D12_DEPTH_STENCILOP_DESC so;
    ID3D12PipelineState *p = NULL;
    size_t i;
    for (i = 0; i < npipes; i++)
        if (pipe_key[i] == key)
            return pipe_obj[i];
    if (shader && !game_ps[shader].code)
        return NULL;
    memset(&d, 0, sizeof d);
    d.pRootSignature = root;
    d.VS.pShaderBytecode = programs[c->program].vs;
    d.VS.BytecodeLength = programs[c->program].vs_size;
    d.PS.pShaderBytecode = shader ? game_ps[shader].code : programs[c->program].fs;
    d.PS.BytecodeLength = shader ? game_ps[shader].size : programs[c->program].fs_size;
    blend_of(&d.BlendState.RenderTarget[0], c->blend);
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = c->topology == RR_LINES ? D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE : D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = kind == KIND_INDEX ? DXGI_FORMAT_R8_UINT : COLOUR;
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
        return NULL;
    }
    if (npipes < MAX_PIPES) {
        pipe_key[npipes] = key;
        pipe_obj[npipes] = p;
        npipes++;
    }
    return p;
}

/* ---- descriptor tables: one for each set of textures a frame binds ---- */
typedef struct { const Tex *t[RR_TEX_SLOTS]; UINT at; } Table;
static Table tables[MAX_TABLES];
static UINT ntables;

static const Tex *slot_tex(int32_t t, int slot)
{
    if (t == RR_TEX_EXTRA0 || t == RR_TEX_EXTRA1)
        return &extra_res[t == RR_TEX_EXTRA0 ? 0 : 1];
    if (t > 0 && t < MAX_TEXTURES && tex[t].r)
        return &tex[t];
    return slot < 2 ? &none_colour : &none_index;
}

static void srv_put(UINT at, const Tex *t)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    memset(&sv, 0, sizeof sv);
    sv.Format = format_of(t->format);
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap, &h);
    h.ptr += (SIZE_T)at * srv_step;
    ID3D12Device_CreateShaderResourceView(dev, t->r, &sv, h);
}

static D3D12_GPU_DESCRIPTOR_HANDLE table_of(const int32_t *ids)
{
    const Tex *t[RR_TEX_SLOTS];
    D3D12_GPU_DESCRIPTOR_HANDLE g;
    UINT i;
    int k;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        t[k] = slot_tex(ids ? ids[k] : 0, k);
    srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &g);
    for (i = 0; i < ntables; i++)
        if (!memcmp(tables[i].t, t, sizeof t)) {
            g.ptr += (UINT64)tables[i].at * srv_step;
            return g;
        }
    if (ntables == MAX_TABLES)
        i = ntables - 1;
    else
        i = ntables++;
    memcpy(tables[i].t, t, sizeof t);
    tables[i].at = i * RR_TEX_SLOTS;
    for (k = 0; k < RR_TEX_SLOTS; k++)
        srv_put(tables[i].at + (UINT)k, t[k]);
    g.ptr += (UINT64)tables[i].at * srv_step;
    return g;
}

/* ---- a frame ---- */
static void targets_fit(int w, int h)
{
    D3D12_DEPTH_STENCIL_VIEW_DESC dv;
    int k;
    if (targets_w == w && targets_h == h)
        return;
    memset(&dv, 0, sizeof dv);
    dv.Format = DEPTH;
    dv.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2DMS;
    for (k = 0; k < 3; k++) {
        tex_make(&screen_ms[k], COLOUR, w, h, RR_SAMPLES, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET);
        tex_make(&screen_ds[k], DEPTH, w, h, RR_SAMPLES, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ID3D12Device_CreateRenderTargetView(dev, screen_ms[k].r, NULL, rtv(k));
        ID3D12Device_CreateDepthStencilView(dev, screen_ds[k].r, &dv, dsv(k));
    }
    for (k = 0; k < 2; k++) {
        tex_make(&extra_res[k], COLOUR, w, h, 1, D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
        extra_res[k].format = RR_BGRA8;
    }
    tex_make(&output, COLOUR, w, h, 1, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, SHADER_READ);
    output.format = RR_BGRA8;
    ID3D12Device_CreateRenderTargetView(dev, output.r, NULL, rtv(3));
    targets_w = w;
    targets_h = h;
}

static void apply_texops(const rr_frame *f)
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
        if (o->id >= MAX_TEXTURES) {
            fprintf(stderr, "rr: d3d12: texture %u past the most\n", o->id);
            continue;
        }
        t = &tex[o->id];
        if (o->op == RR_TEX_CREATE) {
            tex_make(t, format_of(o->format), (int)o->w, (int)o->h, 1,
                     o->flags & RR_TEX_TARGET ? D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET : D3D12_RESOURCE_FLAG_NONE, SHADER_READ);
            t->format = o->format;
            if (o->flags & RR_TEX_TARGET) {
                if (!t->rtv)
                    t->rtv = rtv_next < 256 ? rtv_next++ : 255;
                ID3D12Device_CreateRenderTargetView(dev, t->r, NULL, rtv(t->rtv));
            }
        } else if (t->r) {
            D3D12_TEXTURE_COPY_LOCATION to, from;
            UINT row = o->w * (UINT)bytes_of(t->format), pitch = (row + 255) / 256 * 256, y;
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
}

int rr_draw(rr_frame *f)
{
    int ow = f->width, oh = f->height, paced = 0, back = 0, show = 0, vx, vy, vw, vh;
    float aspect = f->aspect;
    size_t i, p;
    ID3D12CommandList *lists[1];

    wait_gpu();
    for (i = 0; i < f->nshaders; i++) {
        /* kept: its pipelines are made as they are needed */
        uint32_t id = f->shaders[i].id;
        if (id < MAX_SHADERS) {
            game_ps[id].code = f->shaders[i].code;
            game_ps[id].size = f->shaders[i].size;
        } else {
            fprintf(stderr, "rr: d3d12: shader %u could not be kept\n", id);
        }
    }
    if (swapchain) {
        int ww = 0, wh = 0;
        host_window_pixels(&ww, &wh);
        if (ww >= 1 && wh >= 1) {
            if (ww != sc_w || wh != sc_h) {
                UINT j;
                for (j = 0; j < FRAMES; j++)
                    ID3D12Resource_Release(sc_img[j]);
                IDXGISwapChain3_ResizeBuffers(swapchain, FRAMES, (UINT)ww, (UINT)wh, DXGI_FORMAT_UNKNOWN, 0);
                swapchain_buffers();
                sc_w = ww;
                sc_h = wh;
            }
            ow = ww;
            oh = wh;
            show = 1;
            back = (int)IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
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

    /* the words and the uniforms, each block given its target's size and samples */
    buf_fit(&dbuf, (f->data_words + 1) * 4);
    memcpy(dbuf.at, f->data, f->data_words * 4);
    buf_fit(&ubuf, (f->nuniforms + 1) * 256);
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
            u.f[0][3] = -1.0f;
            memcpy((char *)ubuf.at + (size_t)f->cmds[i].uniform * 256, &u, sizeof u);
        }
    }
    ntables = 0;

    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(list, 1, ID3D12Resource_GetGPUVirtualAddress(dbuf.r));
    apply_texops(f);
    for (p = 0; p < f->npasses; p++) {
        const rr_pass *ps = &f->passes[p];
        D3D12_CPU_DESCRIPTOR_HANDLE rt;
        int kind, tw = ow, th = oh, screen = ps->target <= RR_TARGET_EXTRA1;
        Tex *to = NULL;
        if (screen) {
            kind = KIND_SCREEN;
            rt = rtv(ps->target);
            {
                D3D12_CPU_DESCRIPTOR_HANDLE ds = dsv(ps->target);
                ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rt, FALSE, &ds);
                ID3D12GraphicsCommandList_ClearRenderTargetView(list, rt, ps->clear, 0, NULL);
                ID3D12GraphicsCommandList_ClearDepthStencilView(list, ds, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0, NULL);
            }
        } else if (ps->target == RR_TARGET_TEXTURE) {
            static const FLOAT zero[4] = { 0, 0, 0, 0 };
            if (ps->texture >= MAX_TEXTURES || !tex[ps->texture].r || !tex[ps->texture].rtv)
                continue;
            kind = KIND_INDEX;
            to = &tex[ps->texture];
            tw = to->w;
            th = to->h;
            transition(to->r, &to->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            rt = rtv(to->rtv);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rt, FALSE, NULL);
            ID3D12GraphicsCommandList_ClearRenderTargetView(list, rt, zero, 0, NULL);
        } else {
            kind = KIND_COLOUR;
            to = &output;
            transition(output.r, &output.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            rt = rtv(3);
            ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &rt, FALSE, NULL);
        }
        for (i = ps->first; i < ps->first + ps->count; i++) {
            const rr_cmd *c = &f->cmds[i];
            ID3D12PipelineState *pl = pipe_of(c, kind);
            D3D12_VIEWPORT vp;
            D3D12_RECT sc;
            FLOAT bf[4];
            if (!pl || !c->vertices || !c->instances)
                continue;
            if (c->space == RR_SPACE_VIEW && kind != KIND_INDEX) {
                vp.TopLeftX = (FLOAT)vx;
                vp.TopLeftY = (FLOAT)vy;
                vp.Width = (FLOAT)vw;
                vp.Height = (FLOAT)vh;
            } else {
                vp.TopLeftX = vp.TopLeftY = 0;
                vp.Width = (FLOAT)tw;
                vp.Height = (FLOAT)th;
            }
            vp.MinDepth = 0;
            vp.MaxDepth = 1;
            sc.left = sc.top = 0;
            sc.right = tw;
            sc.bottom = th;
            if (c->scissor) {
                int x0, y0, x1, y1;
                rr_scissor(f->uniforms[c->uniform].f[2], f->width, f->height, (int)vp.TopLeftX, (int)vp.TopLeftY, (int)vp.Width, (int)vp.Height,
                           &x0, &y0, &x1, &y1);
                if (x1 <= x0 || y1 <= y0)
                    continue;
                sc.left = x0;
                sc.top = y0;
                sc.right = x1;
                sc.bottom = y1;
            }
            bf[0] = bf[1] = bf[2] = bf[3] = c->constant;
            ID3D12GraphicsCommandList_SetPipelineState(list, pl);
            ID3D12GraphicsCommandList_SetGraphicsRootConstantBufferView(list, 0, ID3D12Resource_GetGPUVirtualAddress(ubuf.r) + (UINT64)c->uniform * 256);
            ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, table_of(c->tex));
            ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
            ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
            ID3D12GraphicsCommandList_OMSetBlendFactor(list, bf);
            ID3D12GraphicsCommandList_OMSetStencilRef(list, c->ref);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, c->topology == RR_LINES ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_DrawInstanced(list, c->vertices, c->instances, 0, 0);
        }
        if (screen) {
            /* resolved: the frame into the output, an extra target into its texture */
            Tex *dst = ps->target == RR_TARGET_FRAME ? &output : &extra_res[ps->target - 1];
            Tex *src = &screen_ms[ps->target];
            transition(src->r, &src->state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
            transition(dst->r, &dst->state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
            ID3D12GraphicsCommandList_ResolveSubresource(list, dst->r, 0, src->r, 0, COLOUR);
            transition(src->r, &src->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
            transition(dst->r, &dst->state, SHADER_READ);
        } else if (to) {
            transition(to->r, &to->state, SHADER_READ);
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
