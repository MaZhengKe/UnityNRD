Shader "Custom/BindlessTest"
{
    Properties
    {
        [MainColor] _BaseColor("Base Color", Color) = (1, 1, 1, 1)
        [MainTexture] _BaseMap("Base Map", 2D) = "white"
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

            TEXTURE2D(_BaseMap);
            SAMPLER(sampler_BaseMap);

            float a;

            Texture2D TextureTable[2048] : register(t31, space0);
            SamplerState my_linear_clamp_sampler;

            CBUFFER_START(UnityPerMaterial)
                half4 _BaseColor;
                float4 _BaseMap_ST;
            CBUFFER_END

            Varyings vert(Attributes IN)
            {
                Varyings OUT;
                OUT.positionHCS = TransformObjectToHClip(IN.positionOS.xyz);
                OUT.uv = TRANSFORM_TEX(IN.uv, _BaseMap);
                return OUT;
            }
 

            half4 frag(Varyings IN) : SV_Target
            {
                
               uint  numTextures = 4;
               int baseTexture = 0;
                
                half4 color = SAMPLE_TEXTURE2D(_BaseMap, sampler_BaseMap, IN.uv) * _BaseColor;
                color = float4(0, 0, 0, 1);

                float2 textureIds = IN.uv.xy * numTextures;
                int texIdFlat = int(int(textureIds.x) + int(textureIds.y) * numTextures) + baseTexture;
                texIdFlat = max(texIdFlat, 0);

                float4 v = TextureTable[texIdFlat].Sample(my_linear_clamp_sampler, frac(IN.uv.xy * numTextures));
                return v;

                return color;
            }
            ENDHLSL
        }
    }
}