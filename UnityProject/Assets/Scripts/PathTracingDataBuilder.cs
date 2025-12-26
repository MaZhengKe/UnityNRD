using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Meetem.Bindless;
using PathTracing;
using UnityEngine;
using UnityEngine.Rendering;
using Object = UnityEngine.Object;

namespace DefaultNamespace
{
    // 对应 HLSL 的 PrimitiveData
    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    public struct PrimitiveData
    {
        public Vector2 uv0, uv1, uv2;
        public float worldArea;
        public Vector2 n0, n1, n2;
        public float uvArea;
        public Vector2 t0, t1, t2;
        public float bitangentSign;
    }

// 对应 HLSL 的 InstanceData
    [Serializable]
    [StructLayout(LayoutKind.Sequential)]
    public struct InstanceData
    {
        public Matrix4x4 mObjectToWorld; // 对应 NRDSample 的矩阵

        public Vector4 baseColorAndMetalnessScale;
        public Vector4 emissionAndRoughnessScale;

        public Vector2 normalUvScale;
        public uint textureOffsetAndFlags;
        public uint primitiveOffset;
        public float scale;

        public uint morphPrimitiveOffset; // 静态场景设为 0
        public uint unused1, unused2, unused3;
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
            Texture2D texBase = (Texture2D)mat.GetTexture("_BaseMap") ?? (Texture2D)mat.GetTexture("_MainTex") ?? defaultWhite;
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
            Debug.Log("startIndex : " +  startIndex);
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
                    int i0 = triangles[t], i1 = triangles[t + 1], i2 = triangles[t + 2];

                    PrimitiveData prim = new PrimitiveData();
                    prim.uv0 = uvs[i0];
                    prim.uv1 = uvs[i1];
                    prim.uv2 = uvs[i2];
                    prim.n0 = PackNormal(normals[i0]); // NRDSample 通常压缩法线，这里暂用 Vector2
                    prim.n1 = PackNormal(normals[i1]);
                    prim.n2 = PackNormal(normals[i2]);

                    // 计算面积 (用于重要性采样)
                    Vector3 v0 = localToWorld.MultiplyPoint(vertices[i0]);
                    Vector3 v1 = localToWorld.MultiplyPoint(vertices[i1]);
                    Vector3 v2 = localToWorld.MultiplyPoint(vertices[i2]);
                    prim.worldArea = Vector3.Cross(v1 - v0, v2 - v0).magnitude * 0.5f;

                    
                    // 3. 计算 UV 面积 (原版代码逻辑)
                    Vector2 uvEdge10 = uvs[i1] - uvs[i0];
                    Vector2 uvEdge20 = uvs[i2] - uvs[i0];
                    float uvArea = Math.Abs(uvEdge10.x * uvEdge20.y - uvEdge20.x * uvEdge10.y) * 0.5f;
                    prim.uvArea = Math.Max(uvArea, 1e-9f);
                    
                    // Debug.Log(tangents.Length);
                     
                    // 4. 计算切线 (简化版，若需平滑切线，则需预处理顶点数据)
                    // 这里演示如何从 Unity 自带的 Tangent 获取并转换
                    Vector4 tang0 = tangents[i0];
                    Vector4 tang1 = tangents[i1];
                    Vector4 tang2 = tangents[i2];

                    // 将切线转换到世界空间 (注意：这里通常需要 ITMatrix)
                    prim.t0 = PackTangent(localToWorld.MultiplyVector(tang0));
                    prim.t1 = PackTangent(localToWorld.MultiplyVector(tang1));
                    prim.t2 = PackTangent(localToWorld.MultiplyVector(tang2));

                    // 副切线方向 (Handedness)
                    // Unity 的 tangent.w 存储的就是 handedness (-1 或 1)
                    prim.bitangentSign = tang0.w; 
                    
                    primitiveDataList.Add(prim);
                }

                // --- 构造 Instance Data ---
                InstanceData inst = new InstanceData();
                inst.mObjectToWorld = localToWorld;
                inst.primitiveOffset = currentPrimitiveOffset;
                inst.baseColorAndMetalnessScale = new Vector4(1, 1, 1, 1); // 可从 Material 获取
                inst.emissionAndRoughnessScale = new Vector4(0, 1, 1, 1); // 可从 Material 获取
                inst.normalUvScale = new Vector2(1, 1); // 可从 Material 获取
                inst.scale = renderers[i].transform.lossyScale.x; // 简单处理

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
                    float metalScale =  1.0f;
                    inst.baseColorAndMetalnessScale = new Vector4(col.r, col.g, col.b, metalScale);

                    // emissionAndRoughnessScale.xyz 对应 Emission 缩放，.w 对应 Roughness 缩放
                    Color emi = mat.HasProperty("_EmissionColor") ? mat.GetColor("_EmissionColor") : Color.black;
                    float roughScale = 1.0f;
                    // 注意：HLSL 里的 materialProps.y 通常是 Roughness，如果你材质里是 Smoothness，这里需要 (1 - s)
                    inst.emissionAndRoughnessScale = new Vector4(emi.r, emi.g, emi.b, roughScale);

                    // normalUvScale
                    Vector2 tiling = mat.mainTextureScale;
                    inst.normalUvScale = tiling;
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

        Vector2 PackNormal(Vector3 n)
        {
            /* 实现编码逻辑 */
            return new Vector2(n.x, n.y);
        }
        
        // 辅助函数
        Vector2 PackTangent(Vector3 t) {
            // 逻辑同 PackNormal，需与 Shader 对应
            return new Vector2(t.x, t.y);
        }
    }
}