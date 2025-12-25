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

extern "C" HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This, UINT nodeMask, const void* pBlob, SIZE_T blobLength, REFIID riid, void** ppv)
{
    // 你的逻辑
    LOG("[HOOK Native] Hooked_CreateRootSignature called.");
    return OrigCreateRootSignature(This, nodeMask, pBlob, blobLength, riid, ppv);
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
    HookDeviceFunc(device, CreateRootSignature);
    // HookDeviceFunc(device, CreateDescriptorHeap);
    // ...

    initialized = true;
}
