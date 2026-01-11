#ifndef RTXDI_RAB_RANDOM_SAMPLER_STATE_HLSLI
#define RTXDI_RAB_RANDOM_SAMPLER_STATE_HLSLI

#include "../HelperFunctions.hlsli"

typedef RandomSamplerState RAB_RandomSamplerState;

RAB_RandomSamplerState RAB_InitRandomSampler(uint2 index, uint pass)
{
    return initRandomSampler(index, g_Const.frameIndex + pass * 13);
}

float RAB_GetNextRandom(inout RAB_RandomSamplerState rng)
{
    return sampleUniformRng(rng);
}

#endif // RTXDI_RAB_RANDOM_SAMPLER_STATE_HLSLI
