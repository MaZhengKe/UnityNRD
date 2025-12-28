// This file must be C file, not CPP.

#include "d3d12.h"
#define D3D12_HOOKS_DECLARE_OFFSETS // 关键：切换宏模式
#include "D3D12Hooks.h"
#include <stddef.h>


// 辅助宏简化 C 文件书写
#define SetDevOff(Name) __D3D12_VTOFFS_##Name = offsetof(ID3D12DeviceVtbl, Name)
#define SetListOff(Name) __D3D12_VTOFFS_##Name = offsetof(ID3D12GraphicsCommandListVtbl, Name)



void __D3D12HOOKS_InitializeD3D12Offsets() {

    SetDevOff(CreateDescriptorHeap);
    SetDevOff(CreateRootSignature);
    SetDevOff(CreateComputePipelineState);
    SetDevOff(CreateGraphicsPipelineState);
    SetDevOff(CreateCommandList);

    SetListOff(SetPipelineState);
    SetListOff(SetDescriptorHeaps);
    SetListOff(SetComputeRootDescriptorTable);
    SetListOff(SetGraphicsRootDescriptorTable);
    SetListOff(SetComputeRootSignature);
    SetListOff(SetGraphicsRootSignature);
    SetListOff(Reset);
    
    SetListOff(ExecuteBundle);

}

