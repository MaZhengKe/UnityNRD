#include <cassert>
#include <mutex>
#include "RenderSystem.h"
#include "NrdInstance.h"
#include "Unity/IUnityLog.h"
#include "D3D12Hooks.h"


#pragma comment(lib, "NRD.lib")
#pragma comment(lib, "NRI.lib")


#define LOG(msg) UNITY_LOG(s_Logger, msg)


namespace
{
    IUnityInterfaces* s_UnityInterfaces = nullptr;
    IUnityGraphics* s_Graphics = nullptr;
    IUnityLog* s_Logger = nullptr;
    std::unordered_map<int32_t, NrdInstance*> g_Instances;
    std::mutex g_InstanceMutex;
    int32_t g_NextInstanceId = 1;


    // 图形设备事件回调
    void UNITY_INTERFACE_API OnGraphicsDeviceEvent(UnityGfxDeviceEventType eventType)
    {
        // 初始化时，创建图形API
        if (eventType == kUnityGfxDeviceEventInitialize)
        {
            RenderSystem::Get().Initialize(s_UnityInterfaces);
        }

        // 让图形API处理与设备相关的事件
        RenderSystem::Get().ProcessDeviceEvent(eventType, s_UnityInterfaces);

        // 在关闭时清理图形API
        if (eventType == kUnityGfxDeviceEventShutdown)
        {
            std::scoped_lock lock(g_InstanceMutex);
            for (auto& pair : g_Instances) delete pair.second;
            g_Instances.clear();

            RenderSystem::Get().Shutdown();
        }
    }

    // 渲染事件和数据的回调
    void UNITY_INTERFACE_API OnRenderEventAndData(int eventID, void* data)
    {
        if (eventID == 1)
        {
            FrameData* frameData = static_cast<FrameData*>(data);

            // LOG(("instanceId: " + std::to_string(frameData->instanceId)).c_str());
            // LOG(("w: " + std::to_string(frameData->width)).c_str());
            // LOG(("h: " + std::to_string(frameData->height)).c_str());
            // LOG(("commonSettings.resourceSize[0] : " + std::to_string(frameData->commonSettings.resourceSize[0] )).c_str());
            // LOG(("commonSettings.resourceSize[1] : " + std::to_string(frameData->commonSettings.resourceSize[1] )).c_str());
            // LOG(("commonSettings.resourceSizePrev[0] : " + std::to_string(frameData->commonSettings.resourceSizePrev[0] )).c_str());
            // LOG(("commonSettings.resourceSizePrev[1] : " + std::to_string(frameData->commonSettings.resourceSizePrev[1] )).c_str());

            std::scoped_lock lock(g_InstanceMutex);
            auto it = g_Instances.find(frameData->instanceId);
            if (it != g_Instances.end())
            {
                it->second->DispatchCompute(frameData);
            }
        }
    }
}

static bool Unprotect(void* addr)
{
    const uint64_t pageSize = 4096;

    DWORD oldProtect = 0;
    auto protectResult = VirtualProtect((void*)((((size_t)addr) / pageSize) * pageSize), pageSize, PAGE_READWRITE, &oldProtect);
    if (protectResult == 0)
    {
        // UnityLog::LogError("VirtualProtect failed, result: %d, old protect: %p\n", protectResult, (void*)(size_t)oldProtect);
        LOG("[NRD Native] VirtualProtect failed.");
        return false;
    }

    LOG("[NRD Native] VirtualProtect succeeded.");
    return true;
}

template <typename T>
static void** GetVTableEntryPtr(T* obj, int vtableOffset)
{
    size_t* vtable = *(size_t**)obj;
    return (void**)((BYTE*)vtable + (vtableOffset));
}

template <typename T>
static void* Hook(T* obj, int vtableOffset, void* newFunction)
{
    auto pptr = GetVTableEntryPtr<T>(obj, vtableOffset);
    auto old = *pptr;
    if (Unprotect(pptr))
    {
        *pptr = newFunction;
    }

    return old;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This,
    _In_ UINT nodeMask,
    _In_reads_(blobLengthInBytes) const void* pBlobWithRootSignature,
    _In_ SIZE_T blobLengthInBytes,
    REFIID riid,
    _COM_Outptr_ void** ppvRootSignature)
{
    LOG("[NRD Native] Hooked_CreateRootSignature called.");
    return OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
}

static void StartHook(ID3D12Device* device)
{
    __D3D12HOOKS_InitializeD3D12Offsets();
    HookDeviceFunc(CreateRootSignature);
}
// 加载Unity插件
extern "C" {
void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginLoad(IUnityInterfaces* unityInterfaces)
{
    s_UnityInterfaces = unityInterfaces;
    // 获取IUnityGraphics接口
    s_Graphics = s_UnityInterfaces->Get<IUnityGraphics>();
    IUnityGraphicsD3D12v8* s_d3d12 = s_UnityInterfaces->Get<IUnityGraphicsD3D12v8>();
    s_Logger = s_UnityInterfaces->Get<IUnityLog>();
    // 注册回调以接收图形设备事件
    s_Graphics->RegisterDeviceEventCallback(OnGraphicsDeviceEvent);

    StartHook(s_d3d12->GetDevice());

    // 在插件加载时手动运行OnGraphicsDeviceEvent（initialize）
    OnGraphicsDeviceEvent(kUnityGfxDeviceEventInitialize);

    LOG("[NRD Native] UnityPluginLoad completed.");
}


// 卸载Unity插件
void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UnityPluginUnload()
{
    // 取消注册图形设备事件回调
    s_Graphics->UnregisterDeviceEventCallback(OnGraphicsDeviceEvent);
    LOG("[NRD Native] UnityPluginUnload completed.");
}

// 获取渲染事件和数据的函数指针
UnityRenderingEventAndData UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API GetRenderEventAndDataFunc()
{
    return OnRenderEventAndData;
}

// C# 构造时调用
UNITY_INTERFACE_EXPORT int UNITY_INTERFACE_API CreateDenoiserInstance()
{
    std::scoped_lock lock(g_InstanceMutex);
    int id = g_NextInstanceId++;
    g_Instances[id] = new NrdInstance(s_UnityInterfaces, id);
    return id;
}

// C# Dispose 时调用
UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API DestroyDenoiserInstance(int id)
{
    std::scoped_lock lock(g_InstanceMutex);
    auto it = g_Instances.find(id);
    if (it != g_Instances.end())
    {
        delete it->second;
        g_Instances.erase(it);
    }
}

// C# Dispose 时调用
UNITY_INTERFACE_EXPORT void* UNITY_INTERFACE_API WrapD3D12Texture(ID3D12Resource* resource, DXGI_FORMAT format)
{
    return RenderSystem::Get().WrapD3D12Texture(resource, format);
}

UNITY_INTERFACE_EXPORT void UNITY_INTERFACE_API ReleaseTexture(nri::Texture* nriTex)
{
    RenderSystem::Get().Release(nriTex);
}

void UNITY_INTERFACE_EXPORT UNITY_INTERFACE_API UpdateDenoiserResources(
    int instanceId,
    NrdResourceInput* resources,
    int count)
{
    std::scoped_lock lock(g_InstanceMutex);
    auto it = g_Instances.find(instanceId);
    if (it != g_Instances.end())
    {
        it->second->UpdateResources(resources, count);
    }
}
}
