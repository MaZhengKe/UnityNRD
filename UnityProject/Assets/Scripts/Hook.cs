using System.Runtime.InteropServices;
using UnityEngine;

namespace PathTracing
{
    public class Hook : MonoBehaviour
    {
        [DllImport("RenderingPlugin")]
        private static extern void Init();
        
        [ContextMenu("Init Hook")]
        public void InitHook()
        { 
            Init();
        }
    }
}