using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
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
        public RayTracingAccelerationStructure accelerationStructure;

        public RayTracingAccelerationStructure.Settings settings;
        public ComputeBuffer _instanceBuffer;
        public ComputeBuffer _primitiveBuffer;

        public List<InstanceData> instanceDataList = new List<InstanceData>();
        public List<PrimitiveData> primitiveDataList = new List<PrimitiveData>();

        [ContextMenu("Build RTAS and Buffers")]
        public void Build()
        {
            if (accelerationStructure != null)
            {
                accelerationStructure.Release();
                accelerationStructure = null;
            }
            
            instanceDataList.Clear();
            primitiveDataList.Clear();
            
            settings = new RayTracingAccelerationStructure.Settings
            {
                managementMode = RayTracingAccelerationStructure.ManagementMode.Automatic,
                rayTracingModeMask = RayTracingAccelerationStructure.RayTracingModeMask.Everything
            };

            accelerationStructure = new RayTracingAccelerationStructure(settings);
            accelerationStructure.Build();

            
            Debug.Log("GetInstanceCount  " +accelerationStructure.GetInstanceCount());

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

                instanceDataList.Add(inst);


                // --- 向 RTAS 添加实例 ---
                // 关键点：InstanceID 必须对应 instanceDataList 的索引
                accelerationStructure.UpdateInstanceID(renderers[i], instanceID: (uint)i);

                currentPrimitiveOffset += (uint)(triangles.Length / 3);
            }

            _instanceBuffer = new ComputeBuffer(instanceDataList.Count, Marshal.SizeOf<InstanceData>());
            _instanceBuffer.SetData(instanceDataList.ToArray());

            _primitiveBuffer = new ComputeBuffer(primitiveDataList.Count, Marshal.SizeOf<PrimitiveData>());
            _primitiveBuffer.SetData(primitiveDataList.ToArray());
            accelerationStructure.Build();
            
            
            Debug.Log("GetInstanceCount " +accelerationStructure.GetInstanceCount());
            Debug.Log($"Built RTAS with {instanceDataList.Count} instances and {primitiveDataList.Count} primitives.");
        }

        Vector2 PackNormal(Vector3 n)
        {
            /* 实现编码逻辑 */
            return new Vector2(n.x, n.y);
        }
    }
}