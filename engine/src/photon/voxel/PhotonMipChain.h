// THE PHOTON MIP CHAIN (ENGINE-UPGRADE-ASYNC-3 stage 1): a volume's levels 1..n made by COMPUTE,
// one dispatch a level through the one Photon dispatch door (photon/PhotonDispatch.h), so the GI
// region holds no graphics-only command. It replaced TextureGpu::_autogenerateMipmaps on every
// Photon volume: that is vkCmdBlitImage, graphics-queue-only by the Vulkan spec, and it needed the
// volume created RenderToTexture | AllowAutomipmaps. The job (Photon/MipDown,
// media/Photon/Voxel/PhotonMipDown_cs.glsl) applies the blit's own filter - the destination
// texel's centre read from the level below with a linear clamp sampler: on an even axis the
// equal-weight mean of the 2 x 2 x 2 block, every channel (the volumes are premultiplied).
//
// One chain per volume ROLE (a fixed format): the jobs are clones of the base, one per level,
// cloned when the level count is first seen and kept; the texture is bound per generate() (a
// role whose texture ping-pongs, the lighting's total, rebinds two cached descriptor sets). The
// levels are the texture's own, declared at creation (setNumMipmaps): each is written as a UAV.
#ifndef JAH_PHOTON_MIP_CHAIN_H
#define JAH_PHOTON_MIP_CHAIN_H

#include "OgrePrerequisites.h"
#include "OgreFastArray.h"

namespace Ogre
{
    class HlmsComputeJob;
    class HlmsManager;
    class RenderSystem;
    class TextureGpu;

    class PhotonMipChain
    {
    public:
        PhotonMipChain() = default;
        ~PhotonMipChain();
        PhotonMipChain( const PhotonMipChain & ) = delete;
        PhotonMipChain &operator=( const PhotonMipChain & ) = delete;

        /// Writes levels 1..n of `texture` from level 0 (each from the one below), n + 1 being
        /// the texture's mip count. A one-level texture writes nothing.
        void generate( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *texture );
        /// Destroys the clones (the destructor does it too; call it before the HlmsManager dies).
        void destroy();

        /// Dispatches made, process-wide (the region row reads it beside the door's count).
        static unsigned long long numDispatches();

    private:
        HlmsManager *mHlmsManager = nullptr;
        FastArray<HlmsComputeJob *> mJobs;  ///< [L - 1] writes level L
    };
}  // namespace Ogre

#endif
