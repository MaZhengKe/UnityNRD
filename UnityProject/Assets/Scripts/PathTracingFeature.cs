using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using DefaultNamespace;
using Nrd;
using Unity.Mathematics;
using UnityEngine;
using UnityEngine.Rendering;
using UnityEngine.Rendering.Universal;
using static UnityEngine.Rendering.RayTracingAccelerationStructure;

namespace PathTracing
{
    public struct InstanceData
    {
        public Matrix4x4 prevObjectToWorld;
        // 你也可以在这里存其他东西，比如材质 ID
    }


    public class PathTracingFeature : ScriptableRendererFeature
    {
        private Material finalMaterial;
        public RayTracingShader opaqueTracingShader;
        public RayTracingShader transparentTracingShader;

        public ComputeShader compositionComputeShader;
        public ComputeShader taaComputeShader;
        public ComputeShader opaqueTracingCs;

        public PathTracingSetting pathTracingSetting;

        private PathTracingPassSingle _pathTracingPass;

        // public RayTracingAccelerationStructure accelerationStructure;
        public Settings settings;

        public Texture2D gIn_ScramblingRanking;
        public Texture2D gIn_Sobol;
        public GraphicsBuffer gIn_ScramblingRankingUint;
        public GraphicsBuffer gIn_SobolUint;

        private Dictionary<int, NRDDenoiser> _denoisers = new();


        private PathTracingDataBuilder _dataBuilder = new PathTracingDataBuilder();


        [ContextMenu("ReBuild AccelerationStructure")]
        public void ReBuild()
        {
            _dataBuilder.Build();
        }
        
        
        public override void Create()
        {
            // if(_dataBuilder.IsEmpty())
            //     _dataBuilder.Build();

            if (gIn_ScramblingRankingUint == null)
            {
                // Debug.Log($"gIn_ScramblingRanking {gIn_ScramblingRanking.format} width:{gIn_ScramblingRanking.width} height:{gIn_ScramblingRanking.height}");
                // Debug.Log($"gIn_Sobol {gIn_Sobol.format} width:{gIn_Sobol.width} height:{gIn_Sobol.height}");

                gIn_ScramblingRankingUint =
                    new GraphicsBuffer(GraphicsBuffer.Target.Structured, gIn_ScramblingRanking.width * gIn_ScramblingRanking.height, 16);
                var scramblingRankingData = new uint4[gIn_ScramblingRanking.width * gIn_ScramblingRanking.height];
                byte[] rawData = gIn_ScramblingRanking.GetRawTextureData();

                Color32[] colors = gIn_ScramblingRanking.GetPixels32();


                // Debug.Log($"gIn_ScramblingRanking rawData Length: {rawData.Length}");
                // Debug.Log($"gIn_ScramblingRanking colors Length: {colors.Length}");


                int count = scramblingRankingData.Length;
                for (int i = 0; i < count; i++)
                {
                    scramblingRankingData[i] = new uint4(
                        rawData[i * 4 + 0],
                        rawData[i * 4 + 1],
                        rawData[i * 4 + 2],
                        rawData[i * 4 + 3]);
                }

                gIn_ScramblingRankingUint.SetData(scramblingRankingData);


                gIn_SobolUint = new GraphicsBuffer(GraphicsBuffer.Target.Structured, gIn_Sobol.width * gIn_Sobol.height, 16);
                var sobolData = new uint4[gIn_Sobol.width * gIn_Sobol.height];
                rawData = gIn_Sobol.GetRawTextureData();
                colors = gIn_Sobol.GetPixels32();

                // Debug.Log($"gIn_Sobol rawData Length: {rawData.Length}");
                // Debug.Log($"gIn_Sobol colors Length: {colors.Length}");

                count = sobolData.Length;
                for (int i = 0; i < count; i++)
                {
                    sobolData[i] = new uint4(
                        rawData[i * 4 + 0],
                        rawData[i * 4 + 1],
                        rawData[i * 4 + 2],
                        rawData[i * 4 + 3]);
                }

                gIn_SobolUint.SetData(sobolData);
            }

            _pathTracingPass = new PathTracingPassSingle(pathTracingSetting)
            {
                renderPassEvent = RenderPassEvent.BeforeRenderingPostProcessing,
                OpaqueTs = opaqueTracingShader,
                TransparentTs = transparentTracingShader,
                CompositionCs = compositionComputeShader,
                TaaCs = taaComputeShader,
                opaqueTracingCs = opaqueTracingCs,
                ScramblingRanking = gIn_ScramblingRankingUint,
                Sobol = gIn_SobolUint,
                _dataBuilder = _dataBuilder
            };

            if (finalMaterial == null)
            {
                var shader = Shader.Find("KM/Final");
                finalMaterial = new Material(shader);
            }

            _pathTracingPass.BiltMaterial = finalMaterial;
        }

        public override void AddRenderPasses(ScriptableRenderer renderer, ref RenderingData renderingData)
        {
            Camera cam = renderingData.cameraData.camera;
            if (cam.cameraType == CameraType.Preview || cam.cameraType == CameraType.Reflection)
                return;

            int camID = cam.GetInstanceID();

            if (!_denoisers.TryGetValue(camID, out var nrd))
            {
                nrd = new NRDDenoiser(pathTracingSetting, cam.name);
                _denoisers.Add(camID, nrd);
            }


            _pathTracingPass.NrdDenoiser = nrd;
            
            if(!_dataBuilder.IsEmpty())
                renderer.EnqueuePass(_pathTracingPass);
        }

        protected override void Dispose(bool disposing)
        {
            // Debug.Log("PathTracingFeature Dispose");
            base.Dispose(disposing);
            _pathTracingPass.Dispose();

            foreach (var denoiser in _denoisers.Values)
            {
                denoiser.Dispose();
            }

            _denoisers.Clear();
        }
    }
}