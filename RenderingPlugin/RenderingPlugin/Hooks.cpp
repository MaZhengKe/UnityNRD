#include <Windows.h>
#define D3D12_HOOKS_IMPLEMENTATION // 关键：让宏在此定义变量
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "D3D12Hooks.h"
#include "UnityLog.h"

// 假设我们依然需要日志，这里需要引用外部日志句柄或定义
IUnityLog* s_Logger;
#define LOG(msg) if(s_Logger) s_Logger->Log(kUnityLogTypeLog, msg, __FILE__, __LINE__)

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
extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateGraphicsPipelineState(
    ID3D12Device* This,
    _In_ const D3D12_GRAPHICS_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_ void** ppPipelineState
)
{
    // LOG("[HOOK Native] Hooked_CreateGraphicsPipelineState called.");
    return OrigCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device* This,
    _In_ const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_ void** ppPipelineState)
{
    // LOG("[HOOK Native] Hooked_CreateComputePipelineState called.");
    return OrigCreateComputePipelineState(This, pDesc, riid, ppPipelineState);
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(
    ID3D12GraphicsCommandList1* This,
    _In_ ID3D12CommandAllocator* pAllocator,
    _In_opt_ ID3D12PipelineState* pInitialState
)
{
    // LOG("[HOOK Native] Hooked_Reset called.");
    return OrigReset(This, pAllocator, pInitialState);
}


static D3D12_ROOT_SIGNATURE_DESC DeepCopy(const D3D12_ROOT_SIGNATURE_DESC* original)
{
    const unsigned MaxParams = 128;
    const unsigned MaxRangesPerTable = 64;

    uint8_t* mem = (uint8_t*)malloc(1024 * 1024);
    memset(mem, 0, 1024 * 1024);

    D3D12_ROOT_SIGNATURE_DESC o{};
    o.Flags = original->Flags;

    // Setup params
    o.NumParameters = original->NumParameters;
    o.pParameters = (D3D12_ROOT_PARAMETER*)mem;
    mem += sizeof(D3D12_ROOT_PARAMETER) * MaxParams;

    // Copy params
    auto outParams = (D3D12_ROOT_PARAMETER*)o.pParameters;
    for (unsigned pid = 0; pid < original->NumParameters; pid++)
    {
        D3D12_ROOT_PARAMETER pCopy = original->pParameters[pid];

        // Descriptor table, copy ranges.
        if (pCopy.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
        {
            auto inputRanges = pCopy.DescriptorTable.pDescriptorRanges;
            auto outputRanges = (D3D12_DESCRIPTOR_RANGE*)mem;
            mem += sizeof(D3D12_DESCRIPTOR_RANGE) * MaxRangesPerTable;

            for (unsigned rid = 0; rid < pCopy.DescriptorTable.NumDescriptorRanges; rid++)
            {
                outputRanges[rid] = inputRanges[rid];
            }

            pCopy.DescriptorTable.pDescriptorRanges = outputRanges;
            outParams[pid] = pCopy;
        }
        // Straight copy.
        else
        {
            outParams[pid] = pCopy;
        }
    }

    // Copy samplers
    o.NumStaticSamplers = original->NumStaticSamplers;
    o.pStaticSamplers = (D3D12_STATIC_SAMPLER_DESC*)mem;
    for (unsigned sid = 0; sid < original->NumStaticSamplers; sid++)
    {
        ((D3D12_STATIC_SAMPLER_DESC*)o.pStaticSamplers)[sid] = original->pStaticSamplers[sid];
    }

    return o;
}


const uint32_t srvBindlessDescriptorStart = 31u;
const uint32_t numAdditionalSrv = 10u;


static void FreeDeepCopy(D3D12_ROOT_SIGNATURE_DESC* p)
{
    if (p->pParameters != nullptr)
    {
        free((void*)p->pParameters);
        p->pParameters = nullptr;
    }
}


struct HookedRootSignature
{
    unsigned descriptorId;
    unsigned numMaxBindings;
};


static std::unordered_map<size_t, HookedRootSignature> hookedDescriptors;

static const GUID MeetemBindlessData =
    {0xcad4de65, 0x63e8, 0x4cf6, {0xb8, 0x2c, 0x6f, 0x7e, 0xe6, 0x77, 0x63, 0x33}};


extern "C" HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This, _In_ UINT nodeMask,
    _In_reads_(blobLengthInBytes) const void* pBlobWithRootSignature,
    _In_ SIZE_T blobLengthInBytes,
    REFIID riid,
    _COM_Outptr_ void** ppvRootSignature)
{
    // 你的逻辑
    // LOG("[HOOK Native] Hooked_CreateRootSignature called.");

    ID3D12RootSignatureDeserializer* deserializer;
    auto hr = D3D12CreateRootSignatureDeserializer(pBlobWithRootSignature, blobLengthInBytes, IID_PPV_ARGS(&deserializer));


    if (FAILED(hr))
    {
        LOG("[HOOK Native] Failed to create root signature deserializer.");
        return OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
    }


    auto rootSig = DeepCopy(deserializer->GetRootSignatureDesc());
    deserializer->Release();


    bool needPlaceSrv = false;
    bool ignore = false;

    std::vector<D3D12_DESCRIPTOR_RANGE> newRanges{};
    newRanges.reserve(8192);

    auto writableParams = ((D3D12_ROOT_PARAMETER*)(rootSig.pParameters));
    for (unsigned i = 0; i < rootSig.NumParameters; i++)
    {
        auto& p = writableParams[i];
        if (p.ParameterType != D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) continue;

        UnityLog::Debug("Param: %d, type: %d, shader vis: %d\n", i, p.ParameterType, p.ShaderVisibility);

        if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL)
        {
            auto& v = p.DescriptorTable;
            UnityLog::Debug("Descriptor Table: %d, num ranges %d\n", i, v.NumDescriptorRanges);
            newRanges.clear();


            auto writableDescriptors = (D3D12_DESCRIPTOR_RANGE*)v.pDescriptorRanges;
            for (unsigned x = 0; x < v.NumDescriptorRanges; x++)
            {
                auto& dr = writableDescriptors[x];


                UnityLog::Debug("DescriptorTable: %d, type: %d, base register: %d, numDescriptors: %d, offset: %d\n",
                                x, dr.RangeType, dr.BaseShaderRegister, dr.NumDescriptors, dr.OffsetInDescriptorsFromTableStart);

                // 只处理寄存器空间 0 (space0) 且类型为 SRV (纹理) 的范围
                if (dr.RegisterSpace != 0 || dr.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SRV)
                {
                    newRanges.push_back(dr);
                    continue;
                }

                // 不是我们要找的范围，继续
                if (dr.BaseShaderRegister + dr.NumDescriptors != (srvBindlessDescriptorStart + 1))
                {
                    if (dr.BaseShaderRegister + dr.NumDescriptors > (srvBindlessDescriptorStart + 1))
                    {
                        ignore = true;

                        UnityLog::LogWarning("Some shader uses more than %d texture registers, this shader is not eligible for bindless.", (srvBindlessDescriptorStart + 1));
                        UnityLog::LogWarning("DescriptorTable: %d, type: %d, base register: %d, numDescriptors: %d, offset: %d\n", x, dr.RangeType, dr.BaseShaderRegister, dr.NumDescriptors, dr.OffsetInDescriptorsFromTableStart);
                    }

                    newRanges.push_back(dr);
                    continue;
                }

                // 如果已经设置了忽略标志，后续直接跳过
                if (ignore)
                {
                    needPlaceSrv = false;
                    newRanges.push_back(dr);
                    continue;
                }

                // 找到了符合条件的范围，进行修改
                needPlaceSrv = true;

                if (dr.NumDescriptors == 1)
                {
                    // Just remove it.
                    UnityLog::Log("Removing descriptor range\n");
                    continue;
                }

                UnityLog::Log("Modified descriptor range.\n");
                dr.NumDescriptors--;
                newRanges.push_back(dr);
            }

            // Copy the list.

            v.NumDescriptorRanges = newRanges.size();
            for (unsigned k = 0; k < newRanges.size(); k++)
            {
                writableDescriptors[k] = newRanges.at(k);
            }
        }
    }

    if (ignore || !needPlaceSrv)
    {
        FreeDeepCopy(&rootSig);
        auto ret = OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
        UnityLog::Debug("Created root desc (ignore || !needPlaceSrv) [n] %p, %p\n", *ppvRootSignature, (void*)(size_t)(ret));

        return ret;
    }


    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE newRange{};
    newRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    newRange.NumDescriptors = 10;
    newRange.BaseShaderRegister = srvBindlessDescriptorStart;
    newRange.RegisterSpace = 0;
    newRange.OffsetInDescriptorsFromTableStart = D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND;


    p.DescriptorTable.NumDescriptorRanges = 1;
    p.DescriptorTable.pDescriptorRanges = &newRange;


    // Append to the end.
    writableParams[rootSig.NumParameters] = p;
    rootSig.NumParameters++;


    HookedRootSignature hookedValue{};
    hookedValue.descriptorId = rootSig.NumParameters - 1;
    hookedValue.numMaxBindings = numAdditionalSrv;


    UnityLog::Debug("Adding new descriptor table, new num: %d\n", rootSig.NumParameters);


    ID3DBlob* serializedBlob = nullptr;
    ID3DBlob* errorBlob = nullptr;


    auto serializeResult = D3D12SerializeRootSignature(&rootSig, D3D_ROOT_SIGNATURE_VERSION_1_0, &serializedBlob, &errorBlob);
    if (FAILED(serializeResult))
    {
        UnityLog::LogError("Failed to serialize new root signature %p.\n", (void*)(size_t)serializeResult);

        // Null terminate if needed.
        if (errorBlob != nullptr && errorBlob->GetBufferSize() > 0)
        {
            auto sz = errorBlob->GetBufferSize() - 1;
            auto ptr = (char*)errorBlob->GetBufferPointer();
            if (ptr[sz] != 0)
                ptr[sz] = 0;

            UnityLog::LogError("ErrorBlob: %s\n", errorBlob->GetBufferPointer());
            errorBlob->Release();
        }

        // Return unmodified.
        auto ret = OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
        UnityLog::Debug("Created root desc (FAILED(serializeResult)) [f] %p\n", *ppvRootSignature);
        return ret;
    }
    auto ret = OrigCreateRootSignature(This, nodeMask, serializedBlob->GetBufferPointer(), serializedBlob->GetBufferSize(), riid, ppvRootSignature);
    serializedBlob->Release();

    // 如果成功了
    if (needPlaceSrv && !FAILED(ret) && (*ppvRootSignature) != nullptr)
    {
        ID3D12RootSignature* sig = (ID3D12RootSignature*)*ppvRootSignature;
        hookedDescriptors[(size_t)sig] = hookedValue;
        UnityLog::Debug("Created root desc (hooked) [s] %p\n", *ppvRootSignature);

        // Set via private data
        if (FAILED(sig->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &hookedValue)))
        {
            UnityLog::LogError("Can't set private data\n");
            abort();
        }
    }

    FreeDeepCopy(&rootSig);
    return ret;
}


const uint32_t mainDescHeapMagic = 262144u;


uint32_t srvBaseOffset;

std::vector<ID3D12DescriptorHeap*> srvDescriptorHeaps{};
std::vector<ID3D12DescriptorHeap*> hookedDescriptorHeaps{};

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device* device, _In_ D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                                        REFIID riid,
                                                                        _COM_Outptr_ void** ppvHeap)
{
    LOG("[HOOK Native] Hooked_CreateDescriptorHeap called.");

    if (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        auto ptr = (ID3D12DescriptorHeap*)*ppvHeap;

        uint32_t additional = 1024;
        bool hooked = false;

        UINT num = pDescriptorHeapDesc->NumDescriptors;
        LOG(("[HOOK Native] Number of Descriptors: " + std::to_string(num)).c_str());

        if (pDescriptorHeapDesc->NumDescriptors >= mainDescHeapMagic)
        {
            hooked = true;
            srvBaseOffset = pDescriptorHeapDesc->NumDescriptors;
            pDescriptorHeapDesc->NumDescriptors += additional;
        }

        auto res = OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);

        srvDescriptorHeaps.push_back(ptr);
        if (hooked)
        {
            hookedDescriptorHeaps.push_back(ptr);
            LOG(("[HOOK Native] Hooked CBV_SRV_UAV Descriptor Heap created for pointer :" + std::to_string((uint64_t)(*ppvHeap))).c_str());
        }

        return res;
    }

    return OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(ID3D12GraphicsCommandList* This,
                                                              _In_opt_ ID3D12RootSignature* pRootSignature)
{
    // LOG("[HOOK Native] Hooked_SetGraphicsRootSignature called.");
    OrigSetGraphicsRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(ID3D12GraphicsCommandList* This,
                                                                        _In_opt_ ID3D12RootSignature* pRootSignature)
{
    // LOG("[HOOK Native] Hooked_SetComputeRootSignature called.");
    OrigSetComputeRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(ID3D12GraphicsCommandList10* This,
                                                                   _In_ UINT NumDescriptorHeaps,
                                                                   _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    // LOG("[HOOK Native] Hooked_SetDescriptorHeaps called.");
    OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(ID3D12CommandList* list,
                                                                              _In_ UINT RootParameterIndex,
                                                                              _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // LOG("[HOOK Native] Hooked_SetComputeRootDescriptorTable called.");
    OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list,
                                                                               _In_ UINT RootParameterIndex,
                                                                               _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    // LOG("[HOOK Native] Hooked_SetGraphicsRootDescriptorTable called.");
    OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
}

void InitHook(IUnityLog* logger)
{
    s_Logger = logger;
    // 1. 初始化偏移量 (调用 C 代码)
    __D3D12HOOKS_InitializeD3D12Offsets();
}

// --- 启动接口 ---
void HookDevice(ID3D12Device* device)
{
    static bool deviceHooked = false;
    if (deviceHooked) return;

    // 2. 执行 Hook
    HookDeviceFunc(device, CreateDescriptorHeap);
    HookDeviceFunc(device, CreateRootSignature);
    HookDeviceFunc(device, CreateComputePipelineState);
    HookDeviceFunc(device, CreateGraphicsPipelineState);

    deviceHooked = true;
}

void HookCommandList(ID3D12GraphicsCommandList* cmdList)
{
    static bool cmdListHooked = false;
    if (cmdListHooked) return;

    // 2. 执行 Hook
    HookCmdListFunc(cmdList, SetDescriptorHeaps);
    HookCmdListFunc(cmdList, SetComputeRootDescriptorTable);
    HookCmdListFunc(cmdList, SetGraphicsRootDescriptorTable);
    HookCmdListFunc(cmdList, SetComputeRootSignature);
    HookCmdListFunc(cmdList, SetGraphicsRootSignature);
    HookCmdListFunc(cmdList, Reset);

    cmdListHooked = true;
}
