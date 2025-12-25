Shader "Custom/BindlessTest"
{
    Properties
    {
        _NumTextures("Number of Textures", Range(1, 20)) = 1
        _BaseTexture("Base Texture Index", Range(0, 599)) = 0
    }

    SubShader
    {
        Tags
        {
            "RenderType" = "Opaque" "RenderPipeline" = "UniversalPipeline"
        }

        Pass
        {
            HLSLPROGRAM
            #pragma only_renderers d3d11

            #pragma vertex vert
            #pragma fragment frag

            #pragma use_dxc

            #include "Packages/com.unity.render-pipelines.universal/ShaderLibrary/Core.hlsl"

            struct Attributes
            {
                float4 positionOS : POSITION;
                float2 uv : TEXCOORD0;
            };

            struct Varyings
            {
                float4 positionHCS : SV_POSITION;
                float2 uv : TEXCOORD0;
            };

            Texture2D TextureTable[600] : register(t31, space0);
            SamplerState my_linear_clamp_sampler;

            CBUFFER_START(UnityPerMaterial)
                int _NumTextures;
                int _BaseTexture;
            CBUFFER_END

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                OUT.positionHCS = TransformObjectToHClip(IN.positionOS.xyz);
                OUT.uv = IN.uv;
                return OUT;
            }


            half4 frag(Varyings IN) : SV_Target
            {
 

                float2 textureIds = IN.uv.xy * _NumTextures;
                int texIdFlat = int(int(textureIds.x) + int(textureIds.y) * _NumTextures) + _BaseTexture;
                texIdFlat = max(texIdFlat, 0);

                float4 v = TextureTable[texIdFlat].Sample(my_linear_clamp_sampler, frac(IN.uv.xy * _NumTextures));
                return float4(v.xyz, 1);

                // return color;
            }
            ENDHLSL
        }
    }
}