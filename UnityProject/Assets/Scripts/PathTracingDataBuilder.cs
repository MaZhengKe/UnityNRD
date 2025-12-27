using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Meetem.Bindless;
using PathTracing;
using Unity.Mathematics;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace DefaultNamespace
{
    [StructLayout(LayoutKind.Sequential)]
    public struct PrimitiveData
    {
        public half2 uv0;
        public half2 uv1;
        public half2 uv2;
        public float worldArea;

        public half2 n0;
        public half2 n1;
        public half2 n2;
        public float uvArea;

        public half2 t0;
        public half2 t1;
        public half2 t2;
        public float bitangentSign;
    }

    [StructLayout(LayoutKind.Sequential)]
    public struct InstanceData
    {
        // 对应 HLSL 中的 float4
        public float4 mOverloadedMatrix0;
        public float4 mOverloadedMatrix1;
        public float4 mOverloadedMatrix2;

        public half4 baseColorAndMetalnessScale;
        public half4 emissionAndRoughnessScale;

        public half2 normalUvScale;
        public uint textureOffsetAndFlags;
        public uint primitiveOffset;
        public float scale;

        public uint morphPrimitiveOffset;
        public uint unused1;
        public uint unused2;
        public uint unused3;
    }

    public class PathTracingDataBuilder
    {
        // 位偏移定义
        private const int FLAG_FIRST_BIT = 24;
        private const uint NON_FLAG_MASK = (1u << FLAG_FIRST_BIT) - 1;

// 具体的 Flag 位定义 (对应 HLSL 的 0x01, 0x02...)
        private const uint FLAG_NON_TRANSPARENT = 0x01;
        private const uint FLAG_TRANSPARENT = 0x02;
        private const uint FLAG_FORCED_EMISSION = 0x04;
        private const uint FLAG_STATIC = 0x08;
        private const uint FLAG_HAIR = 0x10;
        private const uint FLAG_LEAF = 0x20;
        private const uint FLAG_SKIN = 0x40;
        private const uint FLAG_MORPH = 0x80;


        // 预定义默认纹理，防止材质缺失纹理导致索引错位
        public Texture2D defaultWhite; // 用于 BaseColor
        public Texture2D defaultBlack; // 用于 Emission
        public Texture2D defaultNormal; // 用于 Normal (0.5, 0.5, 1.0)
        public Texture2D defaultMask; // 用于 Roughness/Metalness (R=0, G=Roughness, B=Metal)

        // 存储最终传给 BindlessPlugin 的总列表
        public List<Texture2D> globalTexturePool = new List<Texture2D>();


        // 缓存已添加的纹理组，避免重复上传相同的材质纹理组合
        private Dictionary<string, uint> textureGroupCache = new Dictionary<string, uint>();

        private uint GetTextureGroupIndex(Material mat)
        {
            if (mat == null) return 0;

            // 获取四张纹理，如果为空则使用默认值
            Texture2D texBase = (Texture2D)mat.GetTexture("_BaseMap") ?? defaultWhite;
            Texture2D texMask = (Texture2D)mat.GetTexture("_MetallicGlossMap") ?? defaultMask;
            Texture2D texNormal = (Texture2D)mat.GetTexture("_BumpMap") ?? defaultNormal;
            Texture2D texEmission = (Texture2D)mat.GetTexture("_EmissionMap") ?? defaultBlack;

            // 生成唯一 Key 判断这四张图是否已经成组添加过
            string key = $"{texBase.GetInstanceID()}_{texMask.GetInstanceID()}_{texNormal.GetInstanceID()}_{texEmission.GetInstanceID()}";

            if (textureGroupCache.TryGetValue(key, out uint startIndex))
            {
                return startIndex;
            }

            // 如果没添加过，则按顺序连续存入 4 张
            startIndex = (uint)globalTexturePool.Count;
            globalTexturePool.Add(texBase); // index + 0
            globalTexturePool.Add(texMask); // index + 1
            globalTexturePool.Add(texNormal); // index + 2
            globalTexturePool.Add(texEmission); // index + 3

            textureGroupCache.Add(key, startIndex);
            return startIndex;
        }

        public RayTracingAccelerationStructure accelerationStructure;

        public RayTracingAccelerationStructure.Settings settings;
        public ComputeBuffer _instanceBuffer;
        public ComputeBuffer _primitiveBuffer;

        public List<InstanceData> instanceDataList = new List<InstanceData>();
        public List<PrimitiveData> primitiveDataList = new List<PrimitiveData>();

        [ContextMenu("Build RTAS and Buffers")]
        public void Build()
        {
            defaultWhite = Texture2D.whiteTexture;
            defaultBlack = Texture2D.blackTexture;
            defaultNormal = Texture2D.normalTexture;
            defaultMask = Texture2D.whiteTexture;


            if (accelerationStructure != null)
            {
                accelerationStructure.Release();
                accelerationStructure = null;
            }

            instanceDataList.Clear();
            primitiveDataList.Clear();

            globalTexturePool.Clear();
            textureGroupCache.Clear();

            settings = new RayTracingAccelerationStructure.Settings
            {
                managementMode = RayTracingAccelerationStructure.ManagementMode.Automatic,
                rayTracingModeMask = RayTracingAccelerationStructure.RayTracingModeMask.Everything
            };

            accelerationStructure = new RayTracingAccelerationStructure(settings);
            accelerationStructure.Build();


            Debug.Log("GetInstanceCount  " + accelerationStructure.GetInstanceCount());

            var renderers = Object.FindObjectsByType<Renderer>(FindObjectsSortMode.None);

            Debug.Log($"Found {renderers.Length} renderers in scene.");


            uint currentPrimitiveOffset = 0;


            for (int i = 0; i < renderers.Length; i++)
            {
                
                Debug.Log("Processing Renderer: " + renderers[i].name);
                
                
                MeshFilter mf = renderers[i].GetComponent<MeshFilter>();
                if (mf == null || mf.sharedMesh == null)
                    continue;

                Mesh mesh = mf.sharedMesh;


                Matrix4x4 localToWorld = renderers[i].transform.localToWorldMatrix;

                // --- 构造 Primitive Data (每个三角形一个) ---
                int[] triangles = mesh.triangles;
                Vector3[] vertices = mesh.vertices;
                Vector2[] uvs = mesh.uv;
                Vector3[] normals = mesh.normals;

                mesh.RecalculateTangents();
                Vector4[] tangents = mesh.tangents;

                for (int t = 0; t < triangles.Length; t += 3)
                {
                    PrimitiveData prim = new PrimitiveData();
                    int i0 = triangles[t], i1 = triangles[t + 1], i2 = triangles[t + 2];

                    prim.n0 = EncodeUnitVector(normals[i0], true);
                    prim.n1 = EncodeUnitVector(normals[i1], true);
                    prim.n2 = EncodeUnitVector(normals[i2], true);

                    prim.uv0 = new half2(uvs[i0]);
                    prim.uv1 = new half2(uvs[i1]);
                    prim.uv2 = new half2(uvs[i2]);


                    // 计算面积 (用于重要性采样)
                    Vector3 p0 = vertices[i0];
                    Vector3 p1 = vertices[i1];
                    Vector3 p2 = vertices[i2];

                    Vector3 edge20 = p2 - p0;
                    Vector3 edge10 = p1 - p0;

                    float worldArea = Vector3.Cross(edge20, edge10).magnitude * 0.5f;
                    prim.worldArea = Math.Max(worldArea, 1e-9f);
                    
                    // Debug.Log($"Triangle {t / 3}: World Area = {prim.worldArea} p0={p0} p1={p1} p2={p2}");


                    // 3. 计算 UV 面积 (原版代码逻辑)
                    Vector3 uvEdge20 = uvs[i2] - uvs[i0];
                    Vector3 uvEdge10 = uvs[i1] - uvs[i0];
                    float uvArea = Vector3.Cross(uvEdge20, uvEdge10).magnitude * 0.5f;
                    prim.uvArea = Math.Max(uvArea, 1e-9f);

                    // Debug.Log(tangents.Length);

                    // 这里演示如何从 Unity 自带的 Tangent 获取并转换
                    Vector3 tang0 = tangents[i0];
                    Vector3 tang1 = tangents[i1];
                    Vector3 tang2 = tangents[i2];

                    // 将切线转换到世界空间 (注意：这里通常需要 ITMatrix)
                    prim.t0 = EncodeUnitVector(tang0, true);
                    prim.t1 = EncodeUnitVector(tang1, true);
                    prim.t2 = EncodeUnitVector(tang2, true);
                    prim.bitangentSign = tangents[i0].w;

                    primitiveDataList.Add(prim);
                }

                // --- 构造 Instance Data ---
                InstanceData inst = new InstanceData();

                Matrix4x4 m = localToWorld;

                // 赋值前三行 (Row 0, Row 1, Row 2)
                inst.mOverloadedMatrix0 = new float4(m.m00, m.m01, m.m02, m.m03);
                inst.mOverloadedMatrix1 = new float4(m.m10, m.m11, m.m12, m.m13);
                inst.mOverloadedMatrix2 = new float4(m.m20, m.m21, m.m22, m.m23);


                inst.primitiveOffset = currentPrimitiveOffset;
                inst.baseColorAndMetalnessScale = new half4(new float4(1, 1, 1, 1)); // 可从 Material 获取
                inst.emissionAndRoughnessScale = new half4(new float4(1, 1, 1, 1)); // 可从 Material 获取
                inst.normalUvScale = new half2(new half(1), new half(1)); // 可从 Material 获取
                inst.morphPrimitiveOffset = 0; // 静态场景设为0 


                // --- 向 RTAS 添加实例 ---
                // 关键点：InstanceID 必须对应 instanceDataList 的索引
                accelerationStructure.UpdateInstanceID(renderers[i], instanceID: (uint)i);

                currentPrimitiveOffset += (uint)(triangles.Length / 3);


                Material mat = renderers[i].sharedMaterial;

                uint baseTextureIndex = GetTextureGroupIndex(mat);


                // 2. 处理 Flags
                uint currentFlags = 0;
                if (mat != null)
                {
                    // 根据 HLSL 逻辑，透明标志位影响颜色乘法：
                    // color.xyz *= geometryProps.Has( FLAG_TRANSPARENT ) ? 1.0 : Math::PositiveRcp( color.w );
                    bool isTransparent = mat.renderQueue >= 3000 || mat.IsKeywordEnabled("_SURFACE_TYPE_TRANSPARENT");
                    currentFlags |= isTransparent ? FLAG_TRANSPARENT : FLAG_NON_TRANSPARENT;

                    // 其他 Flag 根据需求添加
                    if (renderers[i].gameObject.isStatic) currentFlags |= 0x08; // FLAG_STATIC
                }


                // 3. 组合最终数据
                uint textureOffsetAndFlags = ((currentFlags & 0xFF) << FLAG_FIRST_BIT) | (baseTextureIndex & NON_FLAG_MASK);


                // 4. 填充 Instance Data 
                inst.textureOffsetAndFlags = textureOffsetAndFlags; // 核心！


                // 适配 HLSL 中的 Scale 参数
                if (mat != null)
                {
                    // baseColorAndMetalnessScale.xyz 对应 BaseColor 缩放，.w 对应 Metalness 缩放
                    Color col = mat.HasProperty("_BaseColor") ? mat.GetColor("_BaseColor") : Color.white;
                    float metalScale = 1.0f;
                    inst.baseColorAndMetalnessScale = new half4(new half(col.r), new half(col.g), new half(col.b), new half(metalScale));

                    // emissionAndRoughnessScale.xyz 对应 Emission 缩放，.w 对应 Roughness 缩放
                    Color emi = mat.HasProperty("_EmissionColor") ? mat.GetColor("_EmissionColor") : Color.black;
                    float roughScale = 1.0f;

                    // 注意：HLSL 里的 materialProps.y 通常是 Roughness，如果你材质里是 Smoothness，这里需要 (1 - s)
                    inst.emissionAndRoughnessScale = new half4(new half(emi.r), new half(emi.g), new half(emi.b), new half(roughScale));

                    // normalUvScale
                    Vector2 tiling = mat.mainTextureScale;
                    inst.normalUvScale = new half2(new half(tiling.x), new half(tiling.y));
                }


                inst.scale = renderers[i].transform.lossyScale.x;
                instanceDataList.Add(inst);
            }

            UploadToBindlessPlugin();


            _instanceBuffer = new ComputeBuffer(instanceDataList.Count, Marshal.SizeOf<InstanceData>());
            _instanceBuffer.SetData(instanceDataList.ToArray());

            _primitiveBuffer = new ComputeBuffer(primitiveDataList.Count, Marshal.SizeOf<PrimitiveData>());
            _primitiveBuffer.SetData(primitiveDataList.ToArray());
            accelerationStructure.Build();


            Debug.Log("GetInstanceCount " + accelerationStructure.GetInstanceCount());
            Debug.Log($"Built RTAS with {instanceDataList.Count} instances and {primitiveDataList.Count} primitives.");
        }

        private void UploadToBindlessPlugin()
        {
            var data = new BindlessTexture[globalTexturePool.Count];
            for (int i = 0; i < globalTexturePool.Count; i++)
            {
                data[i] = BindlessTexture.FromTexture2D(globalTexturePool[i]);
            }

            BindlessPlugin.SetBindlessTextures(0, data);
        }

        float SafeSign(float x)
        {
            return x >= 0.0f ? 1.0f : -1.0f;
        }

        float2 SafeSign(float2 v)
        {
            return new float2(SafeSign(v.x), SafeSign(v.y));
        }


        half2 EncodeUnitVector(float3 v, bool bSigned = false)
        {
            v /= math.dot(math.abs(v), 1.0f);

            float2 octWrap = (1.0f - math.abs(v.yx)) * SafeSign(v.xy);
            v.xy = v.z >= 0.0f ? v.xy : octWrap;

            return new half2(bSigned ? v.xy : 0.5f * v.xy + 0.5f);
        }
    }
}