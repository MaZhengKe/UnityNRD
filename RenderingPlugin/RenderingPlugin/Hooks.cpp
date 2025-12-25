#include <Windows.h>
#define D3D12_HOOKS_IMPLEMENTATION // 关键：让宏在此定义变量
#include <cstdint>

#include "D3D12Hooks.h"

// 假设我们依然需要日志，这里需要引用外部日志句柄或定义
IUnityLog* s_Logger;
#define LOG(msg) if(s_Logger) s_Logger->Log(kUnityLogTypeLog, msg, __FILE__, __LINE__)

// --- 工具函数 ---
static bool Unprotect(void* addr)
{
    const uint64_t pageSize = 4096;
    DWORD oldProtect = 0;
    void* pageAddr = (void*)((((size_t)addr) / pageSize) * pageSize);
    if (VirtualProtect(pageAddr, pageSize, PAGE_READWRITE, &oldProtect)) return true;
    return false;
}

static void* ApplyHook(void* obj, int vtableOffset, void* newFunction)
{
    size_t* vtable = *(size_t**)obj;
    void** pEntry = (void**)((BYTE*)vtable + vtableOffset);
    void* oldFunc = *pEntry;
    if (Unprotect(pEntry))
    {
        *pEntry = newFunction;
    }
    return oldFunc;
}

// --- Hooked 函数实现 ---


extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(
        ID3D12Device* This,
        _In_  const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
        REFIID riid,
        _COM_Outptr_  void** ppPipelineState
)
{
    LOG("[HOOK Native] Hooked_CreateGraphicsPipelineState called.");
    return OrigCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device* This,
    _In_  const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_  void** ppPipelineState)
{
    LOG("[HOOK Native] Hooked_CreateComputePipelineState called.");
    return OrigCreateComputePipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(
    ID3D12GraphicsCommandList1* This,
    _In_  ID3D12CommandAllocator* pAllocator,
    _In_opt_  ID3D12PipelineState* pInitialState
)
{
    LOG("[HOOK Native] Hooked_Reset called.");
    return OrigReset(This, pAllocator,  pInitialState);
}

extern "C" HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This, UINT nodeMask, const void* pBlob, SIZE_T blobLength, REFIID riid, void** ppv)
{
    // 你的逻辑
    LOG("[HOOK Native] Hooked_CreateRootSignature called.");
    return OrigCreateRootSignature(This, nodeMask, pBlob, blobLength, riid, ppv);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device * device, _In_  D3D12_DESCRIPTOR_HEAP_DESC * pDescriptorHeapDesc,
    REFIID riid,
    _COM_Outptr_  void** ppvHeap)
{
    LOG("[HOOK Native] Hooked_CreateDescriptorHeap called.");
    return OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(ID3D12GraphicsCommandList* This,
    _In_opt_  ID3D12RootSignature* pRootSignature)
{
    LOG("[HOOK Native] Hooked_SetGraphicsRootSignature called.");
    OrigSetGraphicsRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(ID3D12GraphicsCommandList* This,
    _In_opt_  ID3D12RootSignature* pRootSignature)
{
    LOG("[HOOK Native] Hooked_SetComputeRootSignature called.");
    OrigSetComputeRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(ID3D12GraphicsCommandList10* This,
    _In_  UINT NumDescriptorHeaps,
    _In_reads_(NumDescriptorHeaps)  ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    LOG("[HOOK Native] Hooked_SetDescriptorHeaps called.");
    OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(ID3D12CommandList* list,
    _In_  UINT RootParameterIndex,
    _In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    LOG("[HOOK Native] Hooked_SetComputeRootDescriptorTable called.");
    OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list,
    _In_  UINT RootParameterIndex,
    _In_  D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    LOG("[HOOK Native] Hooked_SetGraphicsRootDescriptorTable called.");
    OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

// --- 启动接口 ---
void StartD3D12Hooks(ID3D12Device* device, IUnityLog* logger)
{
    static bool initialized = false;
    if (initialized) return;

    s_Logger = logger;
    // 1. 初始化偏移量 (调用 C 代码)
    __D3D12HOOKS_InitializeD3D12Offsets();

    // 2. 执行 Hook
    HookDeviceFunc(device, CreateDescriptorHeap);
    HookDeviceFunc(device, CreateRootSignature);
    HookDeviceFunc(device, CreateComputePipelineState);
    HookDeviceFunc(device, CreateGraphicsPipelineState);

    initialized = true;
}
