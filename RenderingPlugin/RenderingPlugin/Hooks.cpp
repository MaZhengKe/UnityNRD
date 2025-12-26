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

const uint32_t srvBindlessDescriptorStart = 31u;
const uint32_t numAdditionalSrv = 600u;


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


struct CommandListStateData
{
    unsigned isInHookedCmpRootSig : 1;
    unsigned isInHookedGfxRootSig : 1;

    unsigned isHookedCmpDescSetAssigned : 1;
    unsigned isHookedGfxDescSetAssigned : 1;

    unsigned assignedHookedHeap : 16;

    unsigned bindlessCmpSrvDescId;
    unsigned bindlessGfxSrvDescId;
    //unsigned remains;
};

static inline CommandListStateData GetCommandListState(ID3D12GraphicsCommandList* cmdList)
{
    if (cmdList == nullptr)
    {
        return {};
    }

    UINT dsize = sizeof(CommandListStateData);
    CommandListStateData ret;
    auto res = cmdList->GetPrivateData(MeetemBindlessData, &dsize, &ret);
    if ((FAILED(res)) || (dsize != sizeof(CommandListStateData)))
        return {};

    return ret;
}

const unsigned NoBindless = 0;

static inline bool IsCommandStateHadBindless(CommandListStateData dt)
{
    return (dt.bindlessCmpSrvDescId != NoBindless) | (dt.bindlessGfxSrvDescId != NoBindless) | (dt.isInHookedCmpRootSig) | (dt.isInHookedGfxRootSig) | (dt.isHookedCmpDescSetAssigned) | (dt.isHookedGfxDescSetAssigned) | (dt.
        assignedHookedHeap);
}

static inline bool SetCommandListState(ID3D12GraphicsCommandList* cmdList, CommandListStateData state)
{
    if (cmdList == nullptr)
    {
        return true;
    }

    UINT dsize = sizeof(CommandListStateData);
    CommandListStateData ret;
    auto res = cmdList->SetPrivateData(MeetemBindlessData, dsize, &state);
    return !FAILED(res);
}


template <class T>
static inline bool TryGetBindlessData(ID3D12Object* iface, T& outputSig)
{
    if (iface == nullptr)
    {
        outputSig = {};
        return false;
    }

    UINT dsize = sizeof(T);
    auto res = iface->GetPrivateData(MeetemBindlessData, &dsize, &outputSig);
    return !FAILED(res) && dsize == sizeof(T);
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
    HookedRootSignature data;
    auto hasHookedRootsig = false;


    // UnityLog::Debug("CreateGraphicsPipelineState creating with root sig %p\n", pDesc->pRootSignature);

    if (pDesc->pRootSignature != nullptr)
    {
        hasHookedRootsig = TryGetBindlessData(pDesc->pRootSignature, data);
        if (hasHookedRootsig)
        {
            UnityLog::Debug("CreateGraphicsPipelineState found using the hooked root signature\n");
        }
    }

    auto res = OrigCreateGraphicsPipelineState(This, pDesc, riid, ppPipelineState);
    if (!FAILED(res) && hasHookedRootsig)
    {
        ID3D12PipelineState* state = (ID3D12PipelineState*)*ppPipelineState;
        state->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &data);
    }

    return res;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateComputePipelineState(
    ID3D12Device* This,
    _In_ const D3D12_COMPUTE_PIPELINE_STATE_DESC* pDesc,
    REFIID riid,
    _COM_Outptr_ void** ppPipelineState)
{
    HookedRootSignature data;
    auto hasHookedRootsig = false;


    if (pDesc->pRootSignature != nullptr)
    {
        hasHookedRootsig = TryGetBindlessData(pDesc->pRootSignature, data);
        if (hasHookedRootsig)
        {
            UnityLog::Debug("CreateComputePipelineState found using the hooked root signature\n");
        }
    }

    auto res = OrigCreateComputePipelineState(This, pDesc, riid, ppPipelineState);
    if (!FAILED(res) && hasHookedRootsig)
    {
        ID3D12PipelineState* state = (ID3D12PipelineState*)*ppPipelineState;
        state->SetPrivateData(MeetemBindlessData, sizeof(HookedRootSignature), &data);
    }
    return res;
}

extern "C" static HRESULT STDMETHODCALLTYPE Hooked_Reset(
    ID3D12GraphicsCommandList1* This,
    _In_ ID3D12CommandAllocator* pAllocator,
    _In_opt_ ID3D12PipelineState* pInitialState
)
{
    // LOG("[HOOK Native] Hooked_Reset called.");

    SetCommandListState(This, {});
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

// 判断并修改根签名以支持 Bindless 纹理
extern "C" HRESULT STDMETHODCALLTYPE Hooked_CreateRootSignature(
    ID3D12Device* This, _In_ UINT nodeMask,
    _In_reads_(blobLengthInBytes) const void* pBlobWithRootSignature,
    _In_ SIZE_T blobLengthInBytes,
    REFIID riid,
    _COM_Outptr_ void** ppvRootSignature)
{
    ID3D12RootSignatureDeserializer* deserializer;
    auto hr = D3D12CreateRootSignatureDeserializer(pBlobWithRootSignature, blobLengthInBytes, IID_PPV_ARGS(&deserializer));


    if (FAILED(hr))
    {
        UnityLog::Debug(" Failed to create root signature deserializer.");
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

        if (p.ShaderVisibility == D3D12_SHADER_VISIBILITY_ALL || p.ShaderVisibility == D3D12_SHADER_VISIBILITY_PIXEL)
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

    // 如果不需要添加新的 SRV 描述符表，或者被标记为忽略，直接返回原始根签名
    if (ignore || !needPlaceSrv)
    {
        FreeDeepCopy(&rootSig);
        auto ret = OrigCreateRootSignature(This, nodeMask, pBlobWithRootSignature, blobLengthInBytes, riid, ppvRootSignature);
        // UnityLog::Debug("Created root desc (ignore || !needPlaceSrv) [n] %p, %p\n", *ppvRootSignature, (void*)(size_t)(ret));

        return ret;
    }


    D3D12_ROOT_PARAMETER p = {};
    p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;

    D3D12_DESCRIPTOR_RANGE newRange{};
    newRange.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    newRange.NumDescriptors = 600;
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
uint32_t srvIncrement;

std::vector<ID3D12DescriptorHeap*> srvDescriptorHeaps{};
std::vector<ID3D12DescriptorHeap*> hookedDescriptorHeaps{};

// 监听创建堆-把srv堆变大
extern "C" static HRESULT STDMETHODCALLTYPE Hooked_CreateDescriptorHeap(ID3D12Device* device, _In_ D3D12_DESCRIPTOR_HEAP_DESC* pDescriptorHeapDesc,
                                                                        REFIID riid,
                                                                        _COM_Outptr_ void** ppvHeap)
{
    UnityLog::Debug("Hooked_CreateDescriptorHeap called.");

    if (pDescriptorHeapDesc->Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
    {
        uint32_t additional = 1024;
        bool hooked = false;

        UINT num = pDescriptorHeapDesc->NumDescriptors;
        UnityLog::Debug(" Number of Descriptors: %d", num);

        if (pDescriptorHeapDesc->NumDescriptors >= mainDescHeapMagic)
        {
            hooked = true;
            srvBaseOffset = pDescriptorHeapDesc->NumDescriptors;
            pDescriptorHeapDesc->NumDescriptors += additional;
        }

        auto res = OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);

        auto ptr = (ID3D12DescriptorHeap*)*ppvHeap;

        srvDescriptorHeaps.push_back(ptr);
        if (hooked)
        {
            hookedDescriptorHeaps.push_back(ptr);
            UnityLog::Debug("Hooked CBV_SRV_UAV Descriptor Heap created for pointer : %p, total descriptors: %d\n", ptr, pDescriptorHeapDesc->NumDescriptors);
        }

        return res;
    }

    return OrigCreateDescriptorHeap(device, pDescriptorHeapDesc, riid, ppvHeap);
}

static void STDMETHODCALLTYPE Hooked_SetGraphicsRootSignature(ID3D12GraphicsCommandList* This,
                                                              _In_opt_ ID3D12RootSignature* pRootSignature)
{
    HookedRootSignature d{};
    auto hasDescHook = TryGetBindlessData(pRootSignature, d);
    auto dt = GetCommandListState(This);

    if (!hasDescHook)
    {
        if (IsCommandStateHadBindless(dt))
        {
            dt.isInHookedGfxRootSig = false;
            dt.isHookedGfxDescSetAssigned = false;
            dt.bindlessGfxSrvDescId = NoBindless;
            SetCommandListState(This, dt);
        }
    }
    else
    {
        // UnityLog::Debug("[SetGraphicsRootSignature] Found hooked root sig: %p, descriptorId: %d\n", pRootSignature, d.descriptorId);
        dt.isInHookedGfxRootSig = hasDescHook;
        dt.isHookedGfxDescSetAssigned = false;
        dt.bindlessGfxSrvDescId = hasDescHook ? (d.descriptorId + 1) : NoBindless;
        SetCommandListState(This, dt);
    }

    OrigSetGraphicsRootSignature(This, pRootSignature);
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootSignature(ID3D12GraphicsCommandList* This,
                                                                        _In_opt_ ID3D12RootSignature* pRootSignature)
{
    HookedRootSignature d{};
    auto hasDescHook = TryGetBindlessData(pRootSignature, d);
    auto dt = GetCommandListState(This);
    // UnityLog::Debug("Setting compute root sig: %d %p\n", hasDescHook, pRootSignature);

    if (!hasDescHook)
    {
        if (IsCommandStateHadBindless(dt))
        {
            dt.isInHookedCmpRootSig = false;
            dt.isHookedCmpDescSetAssigned = false;
            dt.bindlessCmpSrvDescId = NoBindless;
            SetCommandListState(This, dt);
        }
    }
    else
    {
        dt.isInHookedCmpRootSig = hasDescHook;
        dt.isHookedCmpDescSetAssigned = false;
        //dt.isHookedHeapAssigned = false;
        dt.bindlessCmpSrvDescId = hasDescHook ? (d.descriptorId + 1) : NoBindless;
        SetCommandListState(This, dt);
    }

    OrigSetComputeRootSignature(This, pRootSignature);
}


extern "C" static void STDMETHODCALLTYPE Hooked_SetDescriptorHeaps(ID3D12GraphicsCommandList10* This,
                                                                   _In_ UINT NumDescriptorHeaps,
                                                                   _In_reads_(NumDescriptorHeaps) ID3D12DescriptorHeap* const* ppDescriptorHeaps)
{
    // 1. 获取当前 CommandList 的状态跟踪对象
    // 因为会有多个线程同时记录不同的 CommandList，所以需要 dt 来记录当前这个列表是否开启了 Bindless 等信息
    auto dt = GetCommandListState(This);
    // UnityLog::Debug("Setting descriptor heaps: %d %d %d\n", NumDescriptorHeaps, dt.isInHookedCmpRootSig, dt.isInHookedGfxRootSig);

    // 2. 防护措施：如果我们还没成功 Hook 到任何堆，就直接走原始逻辑
    if (hookedDescriptorHeaps.empty())
    {
        UnityLog::LogWarning("SetDescriptorHeaps is called, but no srvHeap is set.\n");
        OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
        return;
    }


    ID3D12DescriptorHeap* heaps[128];

    // 4. 处理空绑定情况：如果 Unity 想要取消绑定所有堆
    if (ppDescriptorHeaps == nullptr || NumDescriptorHeaps == 0u)
    {
        // 如果之前处于 Bindless 状态，现在要清空它
        if (IsCommandStateHadBindless(dt))
        {
            dt.isHookedCmpDescSetAssigned = false;
            dt.isHookedGfxDescSetAssigned = false;
            dt.assignedHookedHeap = 0;
            SetCommandListState(This, dt);
        }

        return OrigSetDescriptorHeaps(This, NumDescriptorHeaps, ppDescriptorHeaps);
    }

    unsigned assigned = 0;
    bool hasSrvHeap = false;

    const auto& srvDescHeaps = srvDescriptorHeaps;
    const auto& hookedHeaps = hookedDescriptorHeaps;

    for (int i = 0; i < NumDescriptorHeaps; i++)
    {
        auto h = ppDescriptorHeaps[i];
        heaps[i] = h;

        if (h != nullptr)
        {
            for (auto v : srvDescHeaps)
            {
                if (v == h)
                {
                    hasSrvHeap = true;
                }
            }

            for (unsigned k = 0; k < hookedHeaps.size(); k++)
            {
                auto v = hookedHeaps[k];
                if (v == h)
                {
                    assigned = k + 1;
                }
            }
        }
    }

    if (!assigned && !hasSrvHeap)
    {
        heaps[NumDescriptorHeaps] = hookedHeaps[0];
        NumDescriptorHeaps++;
        assigned = 1;
    }

    // 找到了绑定的堆，更新状态


    dt.assignedHookedHeap = assigned;
    dt.isHookedCmpDescSetAssigned = false;
    dt.isHookedGfxDescSetAssigned = false;
    // 记录
    SetCommandListState(This, dt);

    // 传入修改后的堆数组
    return OrigSetDescriptorHeaps(This, NumDescriptorHeaps, heaps);
}


int currentFrameBindlessOffset;

inline int getCurrentOffset()
{
    return currentFrameBindlessOffset;
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetComputeRootDescriptorTable(ID3D12CommandList* list,
                                                                              _In_ UINT RootParameterIndex,
                                                                              _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    auto gfxList = (ID3D12GraphicsCommandList*)list;

    auto dt = GetCommandListState(gfxList);

    if (hookedDescriptorHeaps.empty())
    {
        UnityLog::LogWarning("SetComputeRootDescriptorTable is called, but no srvHeap is set.\n");
        return;
    }

    OrigSetComputeRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);


    if (dt.isInHookedCmpRootSig && dt.assignedHookedHeap && (dt.bindlessCmpSrvDescId != NoBindless))
    {
        auto targetIdx = dt.bindlessCmpSrvDescId - 1;

        if (RootParameterIndex == (targetIdx) || !dt.isHookedCmpDescSetAssigned)
        {
            dt.isHookedCmpDescSetAssigned = true;
            SetCommandListState(gfxList, dt);

            // UnityLog::Debug("Set compute root descriptor table for bindless: (hooked %d) -> %d with offset %d\n", RootParameterIndex, targetIdx, srvBaseOffset);

            auto assignedHeap = hookedDescriptorHeaps[dt.assignedHookedHeap - 1];
            CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(assignedHeap->GetGPUDescriptorHandleForHeapStart());
            gpuHandle.Offset(srvBaseOffset * srvIncrement);
            gpuHandle.Offset(getCurrentOffset() * srvIncrement);

            OrigSetComputeRootDescriptorTable(list, targetIdx, gpuHandle);
        }
        // Unity assigned descriptor
        else if (RootParameterIndex == (targetIdx))
        {
            UnityLog::LogWarning("Unity forcefully unset bindless\n");
            dt.isHookedCmpDescSetAssigned = false;
            SetCommandListState(gfxList, dt);
        }
    }
}

extern "C" static void STDMETHODCALLTYPE Hooked_SetGraphicsRootDescriptorTable(ID3D12GraphicsCommandList* list,
                                                                               _In_ UINT RootParameterIndex,
                                                                               _In_ D3D12_GPU_DESCRIPTOR_HANDLE BaseDescriptor)
{
    auto gfxList = (ID3D12GraphicsCommandList*)list;

    auto dt = GetCommandListState(gfxList);
    // UnityLog::Debug("Set graphics root descriptor table: %d %d %d %d\n", RootParameterIndex, dt.isInHookedGfxRootSig, dt.assignedHookedHeap, dt.bindlessGfxSrvDescId);


    if (hookedDescriptorHeaps.empty())
    {
        UnityLog::LogWarning("SetGraphicsRootDescriptorTable is called, but no srvHeap is set.\n");
        OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);
        return;
    }

    OrigSetGraphicsRootDescriptorTable(list, RootParameterIndex, BaseDescriptor);


    if ((dt.isInHookedGfxRootSig)
        // For graphics Unity set descriptor heaps somewhere else, 
        // without assigning the heaps after root sig changed
        //& (dt.assignedHookedHeap) 
        & (dt.bindlessGfxSrvDescId != NoBindless))
    {
        auto targetIdx = dt.bindlessGfxSrvDescId - 1;

        if (RootParameterIndex == (targetIdx) || !dt.isHookedGfxDescSetAssigned)
        {
            dt.isHookedGfxDescSetAssigned = true;
            SetCommandListState(gfxList, dt);

            // UnityLog::Debug("Set graphics root descriptor table for bindless: (hooked %d) -> %d with offset %d\n", RootParameterIndex, targetIdx, srvBaseOffset);

            auto assignedHeap = hookedDescriptorHeaps[dt.assignedHookedHeap - 1];
            CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(assignedHeap->GetGPUDescriptorHandleForHeapStart());
            gpuHandle.Offset(srvBaseOffset * srvIncrement);
            gpuHandle.Offset(getCurrentOffset() * srvIncrement);

            OrigSetGraphicsRootDescriptorTable(list, targetIdx, gpuHandle);
        }
    }
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


    srvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);

    UnityLog::Debug("HookDevice called, srvIncrement: %d\n", srvIncrement);
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

    UnityLog::Debug("HookCommandList called.\n");
    cmdListHooked = true;
}


DXGI_FORMAT typeless_fmt_to_typed(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_UINT;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_UINT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_UNORM;

    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_UINT;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_UNORM;

    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_UINT;

    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

    case DXGI_FORMAT_R8G8_TYPELESS:
        return DXGI_FORMAT_R8G8_UNORM;

    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;

    case DXGI_FORMAT_R8_TYPELESS:
        return DXGI_FORMAT_R8_UNORM;

    case DXGI_FORMAT_BC1_TYPELESS:
        return DXGI_FORMAT_BC1_UNORM;

    case DXGI_FORMAT_BC2_TYPELESS:
        return DXGI_FORMAT_BC2_UNORM;

    case DXGI_FORMAT_BC3_TYPELESS:
        return DXGI_FORMAT_BC3_UNORM;

    case DXGI_FORMAT_BC4_TYPELESS:
        return DXGI_FORMAT_BC4_UNORM;

    case DXGI_FORMAT_BC5_TYPELESS:
        return DXGI_FORMAT_BC5_UNORM;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;

    case DXGI_FORMAT_B8G8R8X8_TYPELESS:
        return DXGI_FORMAT_B8G8R8X8_UNORM_SRGB;

    case DXGI_FORMAT_BC6H_TYPELESS:
        return DXGI_FORMAT_BC6H_UF16;

    case DXGI_FORMAT_BC7_TYPELESS:
        return DXGI_FORMAT_BC7_UNORM;

    default:
        return format;
    }
}


void SetBindlessTextures(int offset, unsigned numTextures, BindlessTexture* textures)
{
    const auto& heaps = hookedDescriptorHeaps;

    for (auto heap : heaps)
    {
        CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle(heap->GetCPUDescriptorHandleForHeapStart());
        cpuHandle.Offset(srvIncrement * srvBaseOffset);
        cpuHandle.Offset(srvIncrement * offset);

        for (unsigned i = 0; i < numTextures; i++)
        {
            auto t = textures[i];

            auto texResource = (ID3D12Resource*)t.handle;
            auto desc = texResource->GetDesc();

            D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
            srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            srvDesc.Format = t.forceFormat != 0 ? (DXGI_FORMAT)t.forceFormat : typeless_fmt_to_typed(desc.Format);
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MostDetailedMip = t.minMip;
            srvDesc.Texture2D.MipLevels = t.maxMip == 255u ? desc.MipLevels : (t.maxMip - t.minMip);
            srvDesc.Texture2D.PlaneSlice = 0;
            srvDesc.Texture2D.ResourceMinLODClamp = 0.0f;

            s_device->CreateShaderResourceView(texResource, &srvDesc, cpuHandle);

            cpuHandle.Offset(srvIncrement);
        }

        UnityLog::Debug("SetBindlessTextures applied to heap %p at offset %d for %d textures.\n", heap, offset, numTextures);
    }
}
