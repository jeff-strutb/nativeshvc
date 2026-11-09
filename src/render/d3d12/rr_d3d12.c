/*
 * rr_d3d12.c: the renderer on Direct3D 12 (Windows). The same two fragment
 * passes as the other backends over the same description: the picture into a
 * 32-bit integer target, then the window's pixels. The store and description
 * are upload buffers read as root shader resources. The shaders are compiled at
 * build time, by build.sh, into rr_dxil.h.
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

#define FRAMES 3
typedef struct { ID3D12Resource *r; void *at; UINT64 size; } Buf;
typedef struct { ID3D12Resource *r; int w, h; D3D12_RESOURCE_STATES state; } Target;

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
static int sc_w, sc_h;
static HWND hwnd;
static ID3D12RootSignature *root;
static ID3D12PipelineState *pso_pic, *pso_win, *pso_list, *pso_lines;
static ID3D12DescriptorHeap *rtv_heap, *srv_heap;
static UINT rtv_step;
static Buf store, descb, readback;
static Target pic, off, list_t[4], none_t;
static Buf verts;
static UINT srv_step;
static ID3D12PipelineState *game_pso[256][2];
static int last_w, last_h;

/* RTV slots: 0 the picture, 1 the offscreen window, 2.. the swapchain's */
static D3D12_CPU_DESCRIPTOR_HANDLE rtv(UINT i)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    rtv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtv_heap, &h);
    h.ptr += (SIZE_T)i * rtv_step;
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
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&b->r)))
        exit(70);
    ID3D12Resource_Map(b->r, 0, heap == D3D12_HEAP_TYPE_UPLOAD ? &none : NULL, &b->at);
    b->size = size;
}

static void buf_free(Buf *b)
{
    if (b->r)
        ID3D12Resource_Release(b->r);
    memset(b, 0, sizeof *b);
}

typedef struct { const void *code; size_t size; } Code;
#define CODE(k) ((Code){ k, sizeof k })

static ID3D12PipelineState *pso_v(Code vs, Code ps, DXGI_FORMAT f, int vertices, D3D12_PRIMITIVE_TOPOLOGY_TYPE topo)
{
    static const D3D12_INPUT_ELEMENT_DESC el[2] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32_UINT, 0, 16, D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0 },
    };
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d;
    ID3D12PipelineState *p = NULL;
    memset(&d, 0, sizeof d);
    d.pRootSignature = root;
    d.VS.pShaderBytecode = vs.code;
    d.VS.BytecodeLength = vs.size;
    d.PS.pShaderBytecode = ps.code;
    d.PS.BytecodeLength = ps.size;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = 0xFFFFFFFFu;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.PrimitiveTopologyType = topo;
    if (vertices) {
        d.InputLayout.pInputElementDescs = el;
        d.InputLayout.NumElements = 2;
    }
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = f;
    d.SampleDesc.Count = 1;
    return SUCCEEDED(ID3D12Device_CreateGraphicsPipelineState(dev, &d, &IID_ID3D12PipelineState, (void **)&p)) ? p : NULL;
}

static ID3D12PipelineState *pso(Code vs, Code ps, DXGI_FORMAT f)
{
    return pso_v(vs, ps, f, 0, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
}

static void swapchain_buffers(void)
{
    UINT i;
    for (i = 0; i < FRAMES; i++) {
        IDXGISwapChain3_GetBuffer(swapchain, i, &IID_ID3D12Resource, (void **)&sc_img[i]);
        ID3D12Device_CreateRenderTargetView(dev, sc_img[i], NULL, rtv(2 + i));
    }
}

int rr_open(void)
{
    D3D12_COMMAND_QUEUE_DESC qd;
    D3D12_DESCRIPTOR_HEAP_DESC hd;

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
    hd.NumDescriptors = 2 + FRAMES + 5;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap);
    rtv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    srv_step = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    hd.NumDescriptors = 5;
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&srv_heap);

    {
        D3D12_ROOT_PARAMETER p[4];
        D3D12_DESCRIPTOR_RANGE r;
        D3D12_ROOT_SIGNATURE_DESC rs;
        ID3DBlob *blob = NULL, *err = NULL;
        memset(p, 0, sizeof p);
        memset(&r, 0, sizeof r);
        memset(&rs, 0, sizeof rs);
        p[0].ParameterType = p[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p[0].ShaderVisibility = p[2].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
        p[0].Descriptor.ShaderRegister = 0;
        p[1].Descriptor.ShaderRegister = 1;
        r.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        r.NumDescriptors = 5;
        r.BaseShaderRegister = 2;
        p[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        p[2].DescriptorTable.NumDescriptorRanges = 1;
        p[2].DescriptorTable.pDescriptorRanges = &r;
        p[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p[3].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        p[3].Constants.Num32BitValues = 16;
        rs.NumParameters = 4;
        rs.pParameters = p;
        rs.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
        if (FAILED(D3D12SerializeRootSignature(&rs, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &err)))
            return 0;
        if (FAILED(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(blob), ID3D10Blob_GetBufferSize(blob),
                                                    &IID_ID3D12RootSignature, (void **)&root)))
            return 0;
    }

    pso_pic = pso(CODE(k_vs), CODE(k_picture_fs), DXGI_FORMAT_R32_UINT);
    pso_win = pso(CODE(k_vs), CODE(k_window_fs), DXGI_FORMAT_B8G8R8A8_UNORM);
    pso_list = pso_v(CODE(k_list_vs), CODE(k_list_fs), DXGI_FORMAT_R32_UINT, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
    pso_lines = pso_v(CODE(k_list_vs), CODE(k_list_fs), DXGI_FORMAT_R32_UINT, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
    if (!pso_pic || !pso_win || !pso_list || !pso_lines)
        return 0;

    hwnd = (HWND)host_window_handle();
    if (hwnd) {
        DXGI_SWAP_CHAIN_DESC1 sd;
        IDXGISwapChain1 *s1 = NULL;
        int w = 0, h = 0;
        host_window_pixels(&w, &h);
        memset(&sd, 0, sizeof sd);
        sd.Width = (UINT)(w > 0 ? w : 256);
        sd.Height = (UINT)(h > 0 ? h : 224);
        sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
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

static void target_make(Target *t, DXGI_FORMAT f, int w, int h, UINT slot, D3D12_RESOURCE_STATES state)
{
    D3D12_HEAP_PROPERTIES hp;
    D3D12_RESOURCE_DESC d;
    if (t->r && t->w == w && t->h == h)
        return;
    if (t->r)
        ID3D12Resource_Release(t->r);
    memset(&hp, 0, sizeof hp);
    memset(&d, 0, sizeof d);
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = (UINT64)w;
    d.Height = (UINT)h;
    d.DepthOrArraySize = d.MipLevels = 1;
    d.Format = f;
    d.SampleDesc.Count = 1;
    d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (FAILED(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &d, state, NULL, &IID_ID3D12Resource, (void **)&t->r)))
        exit(70);
    ID3D12Device_CreateRenderTargetView(dev, t->r, NULL, rtv(slot));
    t->w = w;
    t->h = h;
    t->state = state;
}

static void transition(ID3D12Resource *r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b;
    if (from == to)
        return;
    memset(&b, 0, sizeof b);
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
}

/* slots 2 + FRAMES.. : the draw lists' targets; the empty one's the last */
static UINT list_slot(int k) { return 2 + FRAMES + (UINT)k; }

static void srv_put(UINT at, ID3D12Resource *r)
{
    D3D12_SHADER_RESOURCE_VIEW_DESC sv;
    D3D12_CPU_DESCRIPTOR_HANDLE h;
    memset(&sv, 0, sizeof sv);
    sv.Format = DXGI_FORMAT_R32_UINT;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    srv_heap->lpVtbl->GetCPUDescriptorHandleForHeapStart(srv_heap, &h);
    h.ptr += (SIZE_T)at * srv_step;
    ID3D12Device_CreateShaderResourceView(dev, r, &sv, h);
}

static void draw_lists(const eng_packet *p)
{
    int k;
    UINT64 off = 0;
    static const FLOAT zero[4] = { 0, 0, 0, 0 };
    for (k = 0; k < 4; k++) {
        const eng_list *l = &p->lists[k];
        D3D12_CPU_DESCRIPTOR_HANDLE h;
        D3D12_VIEWPORT vp;
        D3D12_RECT sc;
        D3D12_VERTEX_BUFFER_VIEW vb;
        size_t i;
        if (!l->w)
            continue;
        h = rtv(list_slot(k));
        vp.TopLeftX = vp.TopLeftY = 0;
        vp.Width = (FLOAT)l->w;
        vp.Height = (FLOAT)l->h;
        vp.MinDepth = 0;
        vp.MaxDepth = 1;
        sc.left = sc.top = 0;
        sc.right = (LONG)l->w;
        sc.bottom = (LONG)l->h;
        {
            D3D12_RESOURCE_STATES ps = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            transition(list_t[k].r, ps, D3D12_RESOURCE_STATE_RENDER_TARGET);
        }
        ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &h, FALSE, NULL);
        ID3D12GraphicsCommandList_ClearRenderTargetView(list, h, zero, 0, NULL);
        ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
        ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
        if (l->nverts) {
            vb.BufferLocation = ID3D12Resource_GetGPUVirtualAddress(verts.r) + off;
            vb.SizeInBytes = (UINT)(l->nverts * ENG_DRAW_WORDS * 4);
            vb.StrideInBytes = ENG_DRAW_WORDS * 4;
            ID3D12GraphicsCommandList_IASetVertexBuffers(list, 0, 1, &vb);
        }
        for (i = 0; i < l->ncmds; i++) {
            const uint32_t *c = p->list_data + l->cmds + i * ENG_CMD_WORDS;
            struct { float w, h; uint32_t base, tw, th, p0, p1, p2; float inputs[8]; } u = { (float)l->w, (float)l->h, c[3], c[4], c[5], 0, 0, 0, { 0 } };
            ID3D12PipelineState *chosen = c[2] ? pso_lines : pso_list;
            memcpy(u.inputs, c + 7, 32);
            if (c[6] && game_pso[c[6]][c[2] != 0])
                chosen = game_pso[c[6]][c[2] != 0];
            ID3D12GraphicsCommandList_SetPipelineState(list, chosen);
            ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, c[2] ? D3D_PRIMITIVE_TOPOLOGY_LINELIST : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(list, 3, 16, &u, 0);
            ID3D12GraphicsCommandList_DrawInstanced(list, c[1], 1, c[0], 0);
        }
        transition(list_t[k].r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        off += l->nverts * ENG_DRAW_WORDS * 4;
    }
}

static void pass(UINT slot, ID3D12PipelineState *p, int w, int h)
{
    D3D12_CPU_DESCRIPTOR_HANDLE h_rtv = rtv(slot);
    D3D12_VIEWPORT vp = { 0, 0, (FLOAT)w, (FLOAT)h, 0, 1 };
    D3D12_RECT sc = { 0, 0, w, h };
    ID3D12GraphicsCommandList_OMSetRenderTargets(list, 1, &h_rtv, FALSE, NULL);
    ID3D12GraphicsCommandList_SetPipelineState(list, p);
    ID3D12GraphicsCommandList_RSSetViewports(list, 1, &vp);
    ID3D12GraphicsCommandList_RSSetScissorRects(list, 1, &sc);
    ID3D12GraphicsCommandList_IASetPrimitiveTopology(list, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D12GraphicsCommandList_DrawInstanced(list, 3, 1, 0, 0);
}

int rr_draw(eng_packet *p)
{
    UINT64 db = (UINT64)p->desc_words * 4, sb = (UINT64)(p->store_words ? p->store_words : 1) * 4;
    size_t i = 0;
    int ow = p->width, oh = p->height, paced = 0, back = 0;
    D3D12_GPU_DESCRIPTOR_HANDLE srv;
    ID3D12CommandList *lists[1];

    wait_gpu();

    {
        uint32_t k;
        for (k = 0; k < p->nshaders; k++) {
            Code ps = { p->shaders[k].code, p->shaders[k].size };
            game_pso[p->shaders[k].id][0] = pso_v(CODE(k_list_vs), ps, DXGI_FORMAT_R32_UINT, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE);
            game_pso[p->shaders[k].id][1] = pso_v(CODE(k_list_vs), ps, DXGI_FORMAT_R32_UINT, 1, D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE);
        }
    }
    if (store.size < sb) {
        Buf n;
        UINT64 c = store.size ? store.size : 1u << 20;
        while (c < sb)
            c *= 2;
        buf_make(&n, c, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if (store.r)
            memcpy(n.at, store.at, (size_t)store.size);
        buf_free(&store);
        store = n;
    }
    while (i < p->ops_words) {
        uint32_t at = p->ops[i], n = p->ops[i + 1];
        memcpy((uint32_t *)store.at + at, p->ops + i + 2, (size_t)n * 4);
        i += 2 + (size_t)n;
    }
    if (descb.size < db) {
        buf_free(&descb);
        buf_make(&descb, db * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    if (swapchain) {
        host_window_pixels(&ow, &oh);
        if (ow < 1 || oh < 1)
            return 1;
        if (ow != sc_w || oh != sc_h) {
            UINT k;
            for (k = 0; k < FRAMES; k++)
                ID3D12Resource_Release(sc_img[k]);
            IDXGISwapChain3_ResizeBuffers(swapchain, FRAMES, (UINT)ow, (UINT)oh, DXGI_FORMAT_UNKNOWN, 0);
            swapchain_buffers();
            sc_w = ow;
            sc_h = oh;
        }
        back = (int)IDXGISwapChain3_GetCurrentBackBufferIndex(swapchain);
    }
    rr_window_words(p->desc, ow, oh);
    memcpy(descb.at, p->desc, (size_t)db);

    target_make(&pic, DXGI_FORMAT_R32_UINT, p->width, p->height, 0, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    if (!swapchain) {
        UINT64 need = ((UINT64)p->width * 4 + 255) / 256 * 256 * (UINT64)p->height;
        target_make(&off, DXGI_FORMAT_B8G8R8A8_UNORM, ow, oh, 1, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (readback.size < need) {
            buf_free(&readback);
            buf_make(&readback, need, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }
    last_w = p->width;
    last_h = p->height;

    target_make(&none_t, DXGI_FORMAT_R32_UINT, 1, 1, list_slot(4), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    srv_put(0, pic.r);
    {
        int k;
        UINT64 vbytes = 0, voff = 0;
        for (k = 0; k < 4; k++) {
            const eng_list *l = &p->lists[k];
            if (l->w)
                target_make(&list_t[k], DXGI_FORMAT_R32_UINT, (int)l->w, (int)l->h, list_slot(k), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            srv_put((UINT)(1 + k), l->w ? list_t[k].r : none_t.r);
            vbytes += l->nverts * ENG_DRAW_WORDS * 4;
        }
        if (vbytes && verts.size < vbytes) {
            buf_free(&verts);
            buf_make(&verts, vbytes * 2, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        for (k = 0; k < 4; k++) {
            const eng_list *l = &p->lists[k];
            if (l->nverts) {
                memcpy((char *)verts.at + voff, p->list_data + l->verts, l->nverts * ENG_DRAW_WORDS * 4);
                voff += l->nverts * ENG_DRAW_WORDS * 4;
            }
        }
    }
    srv_heap->lpVtbl->GetGPUDescriptorHandleForHeapStart(srv_heap, &srv);

    ID3D12CommandAllocator_Reset(alloc);
    ID3D12GraphicsCommandList_Reset(list, alloc, NULL);
    ID3D12GraphicsCommandList_SetDescriptorHeaps(list, 1, &srv_heap);
    ID3D12GraphicsCommandList_SetGraphicsRootSignature(list, root);
    /* a structured buffer's elements are 4 bytes through a root view */
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(list, 0, ID3D12Resource_GetGPUVirtualAddress(descb.r));
    ID3D12GraphicsCommandList_SetGraphicsRootShaderResourceView(list, 1, ID3D12Resource_GetGPUVirtualAddress(store.r));
    ID3D12GraphicsCommandList_SetGraphicsRootDescriptorTable(list, 2, srv);

    draw_lists(p);
    transition(pic.r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
    pass(0, pso_pic, p->width, p->height);
    transition(pic.r, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    if (swapchain) {
        transition(sc_img[back], D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        pass(2 + (UINT)back, pso_win, ow, oh);
        transition(sc_img[back], D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
    } else {
        D3D12_TEXTURE_COPY_LOCATION to, from;
        pass(1, pso_win, ow, oh);
        transition(pic.r, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
        memset(&to, 0, sizeof to);
        memset(&from, 0, sizeof from);
        to.pResource = readback.r;
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        to.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R32_UINT;
        to.PlacedFootprint.Footprint.Width = (UINT)p->width;
        to.PlacedFootprint.Footprint.Height = (UINT)p->height;
        to.PlacedFootprint.Footprint.Depth = 1;
        to.PlacedFootprint.Footprint.RowPitch = ((UINT)p->width * 4 + 255) / 256 * 256;
        from.pResource = pic.r;
        from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        ID3D12GraphicsCommandList_CopyTextureRegion(list, &to, 0, 0, 0, &from, NULL);
        transition(pic.r, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    }
    ID3D12GraphicsCommandList_Close(list);
    lists[0] = (ID3D12CommandList *)list;
    ID3D12CommandQueue_ExecuteCommandLists(queue, 1, lists);
    if (swapchain) {
        IDXGISwapChain3_Present(swapchain, 1, 0);
        paced = 1;
    }
    ID3D12CommandQueue_Signal(queue, fence, ++fence_at);
    return paced;
}

int rr_read(uint32_t *argb, int *w, int *h)
{
    *w = last_w;
    *h = last_h;
    if (!readback.r)
        return 0;
    if (argb) {
        int y;
        UINT pitch = ((UINT)last_w * 4 + 255) / 256 * 256;
        wait_gpu();
        for (y = 0; y < last_h; y++)
            memcpy(argb + (size_t)y * last_w, (const char *)readback.at + (size_t)y * pitch, (size_t)last_w * 4);
    }
    return 1;
}

int rr_read_window(uint32_t *argb, int *w, int *h)
{
    (void)argb;
    *w = *h = 0;
    return 0;
}
