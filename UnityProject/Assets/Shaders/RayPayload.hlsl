#include "ml.hlsli"

#define INF                                 1e5
#define FP16_VIEWZ_SCALE                    0.125 // TODO: tuned for meters, needs to be scaled down for cm and mm

// Spatial HAsh-based Radiance Cache ( SHARC )
#define SHARC_CAPACITY                      ( 1 << 22 )
#define SHARC_SCENE_SCALE                   45.0
#define SHARC_DOWNSCALE                     5
#define SHARC_ANTI_FIREFLY                  false
#define SHARC_STALE_FRAME_NUM_MIN           32 // new version uses 8 by default, old value offers more stability in voxels with low number of samples ( critical for glass )
#define SHARC_SEPARATE_EMISSIVE             1
#define SHARC_MATERIAL_DEMODULATION         1
#define SHARC_USE_FP16                      0

struct RayPayload
{
    float k;
    float3 albedo;
    float3 emission;
    uint bounceIndexOpaque;
    uint bounceIndexTransparent;
    float3 bounceRayOrigin;
    float3 bounceRayDirection;
    // float3 worldFaceNormal;
    uint rngState;
};


cbuffer PathTracingParams : register(b0)
{
    float4x4 gViewToWorld;
    float4x4 gViewToClip;
    float4x4 gWorldToView;
    float4x4 gWorldToViewPrev;
    float4x4 gWorldToClip;
    float4x4 gWorldToClipPrev;
    float4 gHitDistParams;
    float4 gCameraFrustum;
    float4 gSunBasisX;
    float4 gSunBasisY;
    float4 gSunDirection;
    float4 gCameraGlobalPos;
    float4 gCameraGlobalPosPrev;
    float4 gViewDirection;
    float4 gHairBaseColor;
    float2 gHairBetas;
    float2 gOutputSize; // represents native resolution ( >= gRenderSize )
    float2 gRenderSize; // up to native resolution ( >= gRectSize )
    float2 gRectSize; // dynamic resolution scaling
    float2 gInvOutputSize;
    float2 gInvRenderSize;
    float2 gInvRectSize;
    float2 gRectSizePrev;
    float2 gJitter;
    float gEmissionIntensity;
    float gNearZ;
    float gSeparator;
    float gRoughnessOverride;
    float gMetalnessOverride;
    float gUnitToMetersMultiplier;
    float gTanSunAngularRadius;
    float gTanPixelAngularRadius;
    float gDebug;
    float gPrevFrameConfidence;
    float gUnproject;
    float gAperture;
    float gFocalDistance;
    float gFocalLength;
    float gTAA;
    float gHdrScale;
    float gExposure;
    float gMipBias;
    float gOrthoMode;
    float gIndirectDiffuse;
    float gIndirectSpecular;
    float gMinProbability;
    uint32_t gSharcMaxAccumulatedFrameNum;
    uint32_t gDenoiserType;
    uint32_t gDisableShadowsAndEnableImportanceSampling; // TODO: remove - modify GetSunIntensity to return 0 if sun is below horizon
    uint32_t gFrameIndex;
    uint32_t gForcedMaterial;
    uint32_t gUseNormalMap;
    uint32_t gBounceNum;
    uint32_t gResolve;
    uint32_t gValidation;
    uint32_t gSR;
    uint32_t gRR;
    uint32_t gIsSrgb;
    uint32_t gOnScreen;
    uint32_t gTracingMode;
    uint32_t gSampleNum;
    uint32_t gPSR;
    uint32_t gSHARC;
    uint32_t gTrimLobe;
};


struct MainRayPayload
{
    float3 X; // 命中点的世界空间坐标
    float3 Xprev;
    float4 T; // 切线向量（xyz）和副切线符号（w）
    float3 N; // 法线向量（世界空间）
    float hitT; // 光线命中的距离（t值），INF表示未命中
    float curvature; // 曲率估算值（用于材质、去噪等）
    float2 mipAndCone;
    uint instanceIndex; // 命中的实例索引（用于查找InstanceData）

    float3 matN;

    float3 Lemi;
    float3 baseColor;
    float roughness;
    float metalness;

    float3 GetXoffset(float3 offsetDir, float amount)
    {
        float viewZ = Geometry::AffineTransform(gWorldToView, X).z;
        amount *= gUnproject * abs(viewZ);
        return X + offsetDir * max(amount, 0.00001);
    }

    bool IsMiss()
    {
        return hitT == INF;
    }
};


// Instance flags
#define FLAG_FIRST_BIT                      24 // this + number of flags must be <= 32
#define NON_FLAG_MASK                       ( ( 1 << FLAG_FIRST_BIT ) - 1 )

#define FLAG_NON_TRANSPARENT                0x01 // geometry flag: non-transparent
#define FLAG_TRANSPARENT                    0x02 // geometry flag: transparent
#define FLAG_FORCED_EMISSION                0x04 // animated emissive cube
#define FLAG_STATIC                         0x08 // no velocity
#define FLAG_HAIR                           0x10 // hair
#define FLAG_LEAF                           0x20 // leaf
#define FLAG_SKIN                           0x40 // skin
#define FLAG_MORPH                          0x80 // morph

#define GEOMETRY_ALL                        ( FLAG_NON_TRANSPARENT | FLAG_TRANSPARENT )


struct GeometryProps
{
    float3 X; // 命中点的世界空间坐标
    float3 Xprev; // 命中点在上一帧的世界空间坐标（用于时序去噪/运动矢量）
    float3 V; // 视线方向（通常为 -ray 方向）
    float4 T; // 切线向量（xyz）和副切线符号（w）
    float3 N; // 法线向量（世界空间）
    float2 uv;
    float mip;
    float hitT; // 光线命中的距离（t值），INF表示未命中
    float curvature; // 曲率估算值（用于材质、去噪等）
    uint textureOffsetAndFlags;
    uint instanceIndex; // 命中的实例索引（用于查找InstanceData）

    #define PT_BOUNCE_RAY_OFFSET                0.25 // pixels

    float3 GetXoffset(float3 offsetDir, float amount = PT_BOUNCE_RAY_OFFSET)
    {
        float viewZ = Geometry::AffineTransform(gWorldToView, X).z;
        amount *= gUnproject * abs(viewZ);

        return X + offsetDir * max(amount, 0.00001);
    }

    uint GetBaseTexture( )
    { return textureOffsetAndFlags & NON_FLAG_MASK; }

    
    bool Has( uint flag )
    { return ( textureOffsetAndFlags & ( flag << FLAG_FIRST_BIT ) ) != 0; }
    
    
    float3 GetForcedEmissionColor( )
    { return ( ( textureOffsetAndFlags >> 2 ) & 0x1 ) ? float3( 1.0, 0.0, 0.0 ) : float3( 0.0, 1.0, 0.0 ); }

    
    
    bool IsMiss()
    {
        return hitT == INF;
    }
};


struct MaterialProps
{
    float3 Lemi;
    float3 N;
    float3 T;
    float3 baseColor;
    float roughness;
    float metalness;
    float curvature;
};


#define SKY_INTENSITY 1.0
#define SUN_INTENSITY 10.0


float3 GetSunIntensity(float3 v)
{
    float b = dot(v, gSunDirection.xyz);
    float d = length(v - gSunDirection.xyz * b);

    float glow = saturate(1.015 - d);
    glow *= b * 0.5 + 0.5;
    glow *= 0.6;

    float a = Math::Sqrt01(1.0 - b * b) / b;
    float sun = 1.0 - Math::SmoothStep(gTanSunAngularRadius * 0.9, gTanSunAngularRadius * 1.66 + 0.01, a);
    sun *= float(b > 0.0);
    sun *= 1.0 - Math::Pow01(1.0 - v.y, 4.85);
    sun *= Math::SmoothStep(0.0, 0.1, gSunDirection.y);
    sun += glow;

    float3 sunColor = lerp(float3(1.0, 0.6, 0.3), float3(1.0, 0.9, 0.7), Math::Sqrt01(gSunDirection.y));
    sunColor *= sun;

    sunColor *= Math::SmoothStep(-0.01, 0.05, gSunDirection.y);

    return Color::FromGamma(sunColor) * SUN_INTENSITY;
}


float3 GetSkyIntensity(float3 v)
{
    float atmosphere = sqrt(1.0 - saturate(v.y));

    float scatter = pow(saturate(gSunDirection.y), 1.0 / 15.0);
    scatter = 1.0 - clamp(scatter, 0.8, 1.0);

    float3 scatterColor = lerp(float3(1.0, 1.0, 1.0), float3(1.0, 0.3, 0.0) * 1.5, scatter);
    float3 skyColor = lerp(float3(0.2, 0.4, 0.8), float3(scatterColor), atmosphere / 1.3);
    skyColor *= saturate(1.0 + gSunDirection.y);

    float ground = 0.5 + 0.5 * Math::SmoothStep(-1.0, 0.0, v.y);
    skyColor *= ground;

    return Color::FromGamma(skyColor) * SKY_INTENSITY + GetSunIntensity(v);
}
