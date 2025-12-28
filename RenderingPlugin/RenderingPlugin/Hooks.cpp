#include <Windows.h>
#define D3D12_HOOKS_IMPLEMENTATION // 关键：让宏在此定义变量
#include <cstdint>
#include <d3dx12_root_signature.h>
#include <string>
#include <unordered_map>
#include <vector>

#include "D3D12Hooks.h"
#include "UnityLog.h"

// --- 工具函数 ---
static bool Unprotect(void* addr)
{
    const uint64_t pageSize = 4096;
    DWORD oldProtect = 0;
    void* pageAddr = reinterpret_cast<void*>(reinterpret_cast<size_t>(addr) / pageSize * pageSize);
    if (VirtualProtect(pageAddr, pageSize, PAGE_READWRITE, &oldProtect)) return true;
    return false;
}

static void* ApplyHook(void* obj, int vtableOffset, void* newFunction)
{
    size_t* vtable = *static_cast<size_t**>(obj);
    void** pEntry = reinterpret_cast<void**>(reinterpret_cast<BYTE*>(vtable) + vtableOffset);
    void* oldFunc = *pEntry;
    if (Unprotect(pEntry))
    {
        *pEntry = newFunction;
    }
    return oldFunc;
}


// --- Hooked 函数实现 ---

// 监听创建图形管线状态-关联根签名数据
extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(
    ID3D12Device* This,
    _In_ const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_ void** ppPipelineState
)
{
    UnityLog::Debug("[CreateGraphicsPipelineState] creating with root sig %p\n", pDesc->pRootSignature);
    return OrigCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateCommandList(
    ID3D12Device* This,
    _In_ UINT nodeMask,
    D3D12_COMMAND_LIST_TYPE type,
    _In_ ID3D12CommandAllocator* pCommandAllocator,
    _In_opt_ ID3D12PipelineState* pInitialState,
    REFIID riid,
    _COM_Outptr_ void** ppCommandList)
{
    // enum D3D12_COMMAND_LIST_TYPE
    // {
    //     D3D12_COMMAND_LIST_TYPE_DIRECT	= 0,
    //     D3D12_COMMAND_LIST_TYPE_BUNDLE	= 1,
    //     D3D12_COMMAND_LIST_TYPE_COMPUTE	= 2,
    //     D3D12_COMMAND_LIST_TYPE_COPY	= 3,
    //     D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE	= 4,
    //     D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS	= 5,
    //     D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE	= 6,
    //     D3D12_COMMAND_LIST_TYPE_NONE	= -1
    // } 	D3D12_COMMAND_LIST_TYPE;

    auto commandListTypeStr = std::string("Unknown");
    switch (type)
    {
    case D3D12_COMMAND_LIST_TYPE_DIRECT:
        commandListTypeStr = "Direct";
        break;      
    case D3D12_COMMAND_LIST_TYPE_BUNDLE:
        commandListTypeStr = "Bundle";
        break;
    case D3D12_COMMAND_LIST_TYPE_COMPUTE:
        commandListTypeStr = "Compute";
        break;
    case D3D12_COMMAND_LIST_TYPE_COPY:
        commandListTypeStr = "Copy";
        break;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_DECODE:
        commandListTypeStr = "Video Decode";
        break;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_PROCESS:
        commandListTypeStr = "Video Process";
        break;
    case D3D12_COMMAND_LIST_TYPE_VIDEO_ENCODE:
        commandListTypeStr = "Video Encode";
        break;
    default:
        break;
    }
    
    UnityLog::Debug("[CreateCommandList] Called with type: %s\n", commandListTypeStr.c_str());
    auto res = OrigCreateCommandList(This, nodeMask, type, pCommandAllocator, pInitialState, riid, ppCommandList);
    
    // HookCommandList ((ID3D12GraphicsCommandList*)(*ppCommandList));
    
    return res;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device* This,
    _In_ const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_ void** ppPipelineState)
{
    UnityLog::Debug("[CreateComputePipelineState] creating with root sig %p\n", pDesc->pRootSignature);
    return OrigCreateComputePipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(
    ID3D12GraphicsCommandList1* This,
    _In_ ID3D12CommandAllocator* pAllocator,
    _In_opt_ ID3D12PipelineState* pInitialState
)
{
    UnityLog::Debug("[Reset] Called\n");
    return OrigReset(This, pAllocator, pInitialState);
}

// 判断并修改根签名以支持 Bindless 纹理
extern "C" HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This, _In_ UINT nodeMask,
    _In_reads_(blobLengthInBytes) const void* pBlobWithRootSignature,
    _In_ SIZE_T blobLengthInBytes,
    REFIID riid,
    _COM_Outptr_ void** ppvRootSignature)
{
    UnityLog::Debug("[CreateRootSignature] Called with blob size: %zu bytes\n", blobLengthInBytes);
    return OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
}

// 监听创建堆-把srv堆变大
extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device* device, _In_ D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                                        REFIID riid,
                                                                        _COM_Outptr_ void** ppvHeap)
{
    UnityLog::Debug("[CreateDescriptorHeap] Called with Type: %d, NumDescriptors: %d\n", pDescriptorHeapDesc->Type, pDescriptorHeapDesc->NumDescriptors);
    return OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(ID3D12GraphicsCommandList* This,
                                                              _In_opt_ ID3D12RootSignature* pRootSignature)
{
    UnityLog::Debug("[SetGraphicsRootSignature]  pRootSignature: %p\n", pRootSignature);
    OrigSetGraphicsRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(ID3D12GraphicsCommandList* This,
                                                                        _In_opt_ ID3D12RootSignature* pRootSignature)
{
    UnityLog::Debug("[SetComputeRootSignature]  pRootSignature: %p\n", pRootSignature);
    OrigSetComputeRootSignature(This, pRootSignature);
}


// 监听设置描述符堆-如果没有设置我们需要的堆，就强制设置上，如果有，要记录当前绑定的堆索引
extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(ID3D12GraphicsCommandList10* This,
                                                                   _In_ UINT NumDescriptorHeaps,
                                                                   _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    UnityLog::Debug("[SetDescriptorHeaps] NumDescriptorHeaps: %d,point %p\n", NumDescriptorHeaps, ppDescriptorHeaps);
    OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
}

 
extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(ID3D12CommandList* list,
                                                                              _In_ UINT RootParameterIndex,
                                                                              _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    UnityLog::Debug("[SetComputeRootDescriptorTable] GPU Handle ptr: %p\n", (void*)(size_t)BaseDescriptor.ptr);
    OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list,
                                                                               _In_ UINT RootParameterIndex,
                                                                               _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    UnityLog::Debug("[SetGraphicsRootDescriptorTable] GPU Handle ptr: %p\n", (void*)(size_t)BaseDescriptor.ptr);
    OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

// ExecuteBundle
extern "C" static void STDMETHODCALLTYPE Hooked_ExecuteBundle(
    ID3D12GraphicsCommandList* This,
    _In_ ID3D12GraphicsCommandList* pCommandList)
{
    UnityLog::Debug("[ExecuteBundle] Called with command list: %p\n", pCommandList);
    OrigExecuteBundle(This, pCommandList);
}

void InitHook(IUnityLog* logger)
{
    // 1. 初始化偏移量 (调用 C 代码)
    __D3D12HOOKS_InitializeD3D12Offsets();
}

ID3D12Device* s_device;
// --- 启动接口 ---
void HookDevice(ID3D12Device* device)
{
    static bool deviceHooked = false;
    if (deviceHooked) return;

    s_device = device;
    // 2. 执行 Hook
    HookDeviceFunc(device, CreateDescriptorHeap);
    HookDeviceFunc(device, CreateRootSignature);
    HookDeviceFunc(device, CreateComputePipelineState);
    HookDeviceFunc(device, CreateGraphicsPipelineState);
    HookDeviceFunc(device, CreateCommandList);


    deviceHooked = true;
}

void HookCommandList(ID3D12GraphicsCommandList* cmdList)
{
    // static bool cmdListHooked = false;
    // if (cmdListHooked) return;

    // 2. 执行 Hook
    HookCmdListFunc(cmdList, SetDescriptorHeaps);
    HookCmdListFunc(cmdList, SetComputeRootDescriptorTable);
    HookCmdListFunc(cmdList, SetGraphicsRootDescriptorTable);
    HookCmdListFunc(cmdList, SetComputeRootSignature);
    HookCmdListFunc(cmdList, SetGraphicsRootSignature);
    HookCmdListFunc(cmdList, Reset);
    
    HookCmdListFunc(cmdList, ExecuteBundle);

    UnityLog::Debug("HookCommandList called.\n");
    // cmdListHooked = true;
}


void SetBindlessTextures(int offset, unsigned numTextures, BindlessTexture* textures)
{
    UnityLog::Debug("SetBindlessTextures applied to  at offset %d for %d textures.\n", offset, numTextures);
}
