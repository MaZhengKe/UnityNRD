using System.Collections.Generic;
using System.Linq;
using UnityEngine;

namespace DefaultNamespace
{
    public class PrepareLightsPass
    {
        const uint RTXDI_INVALID_LIGHT_INDEX = 0xFFFFFFFF;


       public static (uint, uint,uint) CountLightsInScene(PathTracingDataBuilder dataBuilder)
        {
            uint numEmissiveMeshes = 0;
            uint numEmissiveTriangles = 0;
            uint numGeometryInstances = 0;

            foreach (var keyValuePair in dataBuilder.rendererInstanceIndexMap)
            {
                var r = keyValuePair.Key;
                uint instanceIndex = keyValuePair.Value;

                MeshFilter mf = r.GetComponent<MeshFilter>();

                if (mf == null || mf.sharedMesh == null)
                    continue;

                Mesh mesh = mf.sharedMesh;
                int subMeshCount = mesh.subMeshCount;


                for (int subIdx = 0; subIdx < subMeshCount; subIdx++)
                {
                    numGeometryInstances += 1;
                    Material mat = r.sharedMaterials[subIdx];

                    var hasEmission = (mat.IsKeywordEnabled("_EMISSION"));

                    if (!hasEmission)
                        continue;


                    numEmissiveMeshes += 1;
                    int[] subMeshTriangles = mesh.GetTriangles(subIdx);
                    numEmissiveTriangles += (uint)(subMeshTriangles.Length / 3);
                }
            }
             


            return (numEmissiveMeshes, numEmissiveTriangles, numGeometryInstances);
        }


        public static RTXDI_LightBufferParameters Process(RtxdiResources resources, PathTracingDataBuilder dataBuilder)
        {
            RTXDI_LightBufferParameters outLightBufferParams = new RTXDI_LightBufferParameters();

            List<PrepareLightsTask> tasks = new List<PrepareLightsTask>();

            uint lightBufferOffset = 0;

            var allMeshRenderers = dataBuilder.rendererInstanceIndexMap.Keys;
            int geometryInstanceCount = allMeshRenderers.Sum(r =>
            {
                MeshFilter mf = r.GetComponent<MeshFilter>();
                if (mf != null && mf.sharedMesh != null)
                {
                    return mf.sharedMesh.subMeshCount;
                }
                else
                {
                    return 0;
                }
            });

            Debug.Log($"PrepareLightsPass GeometryInstanceCount: {geometryInstanceCount}");

            uint[] geometryInstanceToLight = new uint[geometryInstanceCount];

            System.Array.Fill(geometryInstanceToLight, RTXDI_INVALID_LIGHT_INDEX);

            foreach (var keyValuePair in dataBuilder.rendererInstanceIndexMap)
            {
                var r = keyValuePair.Key;
                uint instanceIndex = keyValuePair.Value;

                MeshFilter mf = r.GetComponent<MeshFilter>();

                if (mf == null || mf.sharedMesh == null)
                    continue;

                Mesh mesh = mf.sharedMesh;
                int subMeshCount = mesh.subMeshCount;


                for (int subIdx = 0; subIdx < subMeshCount; subIdx++)
                {
                    Material mat = r.sharedMaterials[subIdx];

                    var hasEmission = (mat.IsKeywordEnabled("_EMISSION"));

                    if (!hasEmission)
                        continue;

                    geometryInstanceToLight[instanceIndex] = lightBufferOffset;

                    int[] subMeshTriangles = mesh.GetTriangles(subIdx);

                    PrepareLightsTask task;
                    task.instanceIndex = (uint)instanceIndex;
                    task.geometryIndex = 0;
                    task.lightBufferOffset = lightBufferOffset;
                    task.triangleCount = (uint)(subMeshTriangles.Length / 3);

                    lightBufferOffset += task.triangleCount;

                    tasks.Add(task);
                }
            }

            resources.GeometryInstanceToLightBuffer.SetData(geometryInstanceToLight);

            outLightBufferParams.localLightBufferRegion.firstLightIndex = 0;
            outLightBufferParams.localLightBufferRegion.numLights = lightBufferOffset;
            outLightBufferParams.infiniteLightBufferRegion.firstLightIndex = 0;
            outLightBufferParams.infiniteLightBufferRegion.numLights = 0;
            outLightBufferParams.environmentLightParams.lightIndex = RTXDI_INVALID_LIGHT_INDEX;
            outLightBufferParams.environmentLightParams.lightPresent = 0;

            resources.TaskBuffer.SetData(tasks.ToArray());

            Debug.Log($"rtxdiResources.TaskBuffer Count: {tasks.Count}");
            
            foreach (var prepareLightsTask in tasks)
            {
                Debug.Log($"PrepareLightsTask: instanceIndex={prepareLightsTask.instanceIndex}, geometryIndex={prepareLightsTask.geometryIndex}, lightBufferOffset={prepareLightsTask.lightBufferOffset}, triangleCount={prepareLightsTask.triangleCount}");
            }

            return outLightBufferParams;
        }
    }
}