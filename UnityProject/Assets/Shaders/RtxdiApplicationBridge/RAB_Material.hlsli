#ifndef RAB_MATERIAL_HLSLI
#define RAB_MATERIAL_HLSLI

static const float kMinRoughness = 0.05f;

struct RAB_Material
{
    float3 diffuseAlbedo;
    float3 specularF0;
    float roughness;
};

RAB_Material RAB_EmptyMaterial()
{
    RAB_Material material = (RAB_Material)0;

    return material;
}

float3 GetDiffuseAlbedo(RAB_Material material)
{
    return float3(0.0, 0.0, 0.0);
}

float3 GetSpecularF0(RAB_Material material)
{
    return float3(0.0, 0.0, 0.0);
}

float GetRoughness(RAB_Material material)
{
    return 0.0;
}

RAB_Material RAB_GetGBufferMaterial(
    int2 pixelPosition,
    PlanarViewConstants view,
    RWTexture2D<uint> diffuseAlbedoTexture,
    RWTexture2D<uint> specularRoughTexture)
{
    RAB_Material material = RAB_EmptyMaterial();

    if (any(pixelPosition >= view.viewportSize))
        return material;

    material.diffuseAlbedo = Unpack_R11G11B10_UFLOAT(diffuseAlbedoTexture[pixelPosition]).rgb;
    float4 specularRough = Unpack_R8G8B8A8_Gamma_UFLOAT(specularRoughTexture[pixelPosition]);
    material.roughness = specularRough.a;
    material.specularF0 = specularRough.rgb;

    return material;
}

// Compare the materials of two surfaces to improve resampling quality.
// Just say that everything is similar for simplicity.
bool RAB_AreMaterialsSimilar(RAB_Material a, RAB_Material b)
{
    return true;
}


#endif // RAB_MATERIAL_HLSLI
