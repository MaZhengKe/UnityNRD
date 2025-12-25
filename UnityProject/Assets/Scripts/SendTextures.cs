using System.Collections.Generic;
using Meetem.Bindless;
using UnityEngine;

namespace PathTracing
{
    public class SendTextures : MonoBehaviour
    {
        public List<Texture2D> textures;
        
        [ContextMenu("Send Textures")]
        private void Send()
        {
            
            var data = new BindlessTexture[textures.Count];
            for (int i = 0; i < textures.Count; i++)
            {
                data[i] = BindlessTexture.FromTexture2D(textures[i]);
            }

            BindlessPlugin.SetBindlessTextures(0, data);
            
        }
    }
}