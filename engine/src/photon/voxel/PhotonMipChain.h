// THE PHOTON MIP CHAIN (ENGINE-UPGRADE-ASYNC-3 stage 1): a volume's levels 1..n made by COMPUTE,
// one dispatch a level through the one Photon dispatch door (photon/PhotonDispatch.h), so the GI
// region holds no graphics-only command. It replaced TextureGpu::_autogenerateMipmaps on every
// Photon volume: that is vkCmdBlitImage, graphics-queue-only by the Vulkan spec, and it needed the
// volume created RenderToTexture | AllowAutomipmaps. The job (Photon/MipDown,
// media/Photon/Voxel/PhotonMipDown_cs.glsl) applies the blit's own filter - the destination
// texel's centre read from the level below with a linear clamp sampler: on an even axis the
// equal-weight mean of the 2 x 2 x 2 block, every channel (the volumes are premultiplied).
//
// THE JOBS ARE POOLED by (format, volume size, level) - one clone of the base per kind, shared by
// every volume of that kind (every cascade, every rebuild), the texture bound per dispatch - so a
// pipeline is built once per kind (a clone per volume and level built ~220 per arm build). The
// levels are the texture's own, declared at creation (setNumMipmaps): each is written as a UAV.
#ifndef JAH_PHOTON_MIP_CHAIN_H
#define JAH_PHOTON_MIP_CHAIN_H

#include "OgrePrerequisites.h"

namespace Ogre
{
    class HlmsComputeJob;
    class HlmsManager;
    class RenderSystem;
    class TextureGpu;

    class PhotonMipChain
    {
    public:
        /// Writes levels 1..n of `texture` from level 0 (each from the one below), n + 1 being
        /// the texture's mip count. A one-level texture writes nothing.
        static void generate( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *texture );
        /// Destroys the pooled jobs - before the compute Hlms dies (the engine's teardown).
        static void releasePool();
        /// Dispatches made and jobs ever pooled, process-wide (the region row reads them).
        static unsigned long long numDispatches();
        static unsigned long long numJobs();
    };
}  // namespace Ogre

#endif
