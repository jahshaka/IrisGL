// THE CASCADE SCROLL'S TARGET (ENGINE-UPGRADE-U5 stage 1).
//
// A cascade that scrolls by k cells used to throw its volume away and re-voxelise all of it in one
// frame (Bistro, Debug: 211-1,595 ms of GPU a scroll, the owner's walking jerk). Upstream scrolls
// incrementally (VctImageVoxelizer::buildRelative): it keeps the voxels the new box shares with the
// old one and voxelises only the entering slab. This is that, on our store:
//
//   1. BEGIN - this target is cleared and the overlap of the live volumes is COPIED into it,
//      shifted by the scroll (a voxel at new cell n is the old cell n + shift). Every level-0 volume
//      is copied texel for texel except the surface position, which is stored absolute in the box's
//      normalised coordinate and is RE-RESOLVED at its new cell from the merge's own integer sums
//      (PhotonScrollPosition_cs.glsl) - the bits a whole build at the new place writes.
//   2. THE ENTERING SLAB, voxelised into this target over as many frames as the per-frame budget
//      needs (PhotonVoxelizer::buildChunks: the same dispatches, shader and merge as a whole build).
//   3. COMMIT - this target is copied into the live volumes, the region moves and the mip chains
//      are regenerated whole. The readers never see a half-built box: until the commit the live
//      volumes are the old box, correct for where they are, and positions outside it fall to the
//      outer cascade by the box test as they always did.
//
// WHY A COPY AND NOT A TOROIDAL ADDRESS (the preflight's ruling, the lead's decision 1): the readers
// construct the voxel address in ten places, every voxel sampler CLAMPs, the directional volumes
// pack both signs along x, and the plane march reads box-aligned planes at every mip - a wrap would
// mix the box's opposite edges. The copy changes no reader.
//
// WHY ON COMPUTE: an image copy is a transfer command and the GI region records compute only
// (gi.region_compute_only). Two jobs: Photon/ScrollCopy (any one volume, float or uint) and
// Photon/ScrollPosition. ONE TARGET PER VOLUME SIZE, owned by the cascade chain: a scroll in flight
// holds it until it commits, and the scheduler starts no second scroll of that size meanwhile.
#ifndef JAH_PHOTON_VOXEL_SCROLL_H
#define JAH_PHOTON_VOXEL_SCROLL_H

#include "photon/voxel/PhotonVoxelizer.h"

#include "OgrePixelFormatGpu.h"

#include <map>

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class HlmsComputeJob;

    class PhotonVoxelScroll
    {
    public:
        /// Creates the target's level-0 volumes (a texture creation: a GRAPHICS region's work - the
        /// chain's build). `like` gives the formats: the target is a copy of a live store.
        PhotonVoxelScroll( RenderSystem *renderSystem, HlmsManager *hlmsManager,
                           TextureGpuManager *textureManager, PhotonVoxelizer *like, uint32 resolution );
        ~PhotonVoxelScroll();

        uint32 resolution() const { return mResolution; }
        const PhotonVoxelizer::VolumeSet &volumes() const { return mVolumes; }
        /// The bytes this target holds, for the VRAM accounting a status reports.
        size_t bytes() const;

        /// THE SCROLL'S FIRST STEP: this target cleared, then the overlap of `live`'s level 0 copied
        /// into it - a voxel at new cell n from old cell n + shift (shift = new lattice - old lattice,
        /// per axis, each |shift| < resolution).
        void begin( PhotonVoxelizer *live, const int32 shift[3] );
        /// THE COMMIT'S COPY: every level-0 volume of this target into `live`'s, whole.
        void commit( PhotonVoxelizer *live );

    private:
        void copyBox( TextureGpu *src, TextureGpu *dst, const uint32 srcOrigin[3],
                      const uint32 dstOrigin[3], const uint32 size[3] );
        void copyPositions( const PhotonVoxelizer::VolumeSet &src, const PhotonVoxelizer::VolumeSet &dst,
                            const uint32 srcOrigin[3], const uint32 dstOrigin[3], const uint32 size[3] );
        HlmsComputeJob *copyJob( PixelFormatGpu format );

        RenderSystem      *mRenderSystem;
        HlmsManager       *mHlmsManager;
        TextureGpuManager *mTextureManager;
        ComputeTools      *mComputeTools;
        uint32             mResolution;
        PhotonVoxelizer::VolumeSet mVolumes;
        /// One clone of Photon/ScrollCopy per format (the store qualifier is the bound volume's),
        /// and one of Photon/ScrollPosition - this object's, destroyed with it.
        std::map<PixelFormatGpu, HlmsComputeJob *> mCopyJobs;
        HlmsComputeJob *mPositionJob;
        ResourceTransitionArray mResourceTransitions;
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
