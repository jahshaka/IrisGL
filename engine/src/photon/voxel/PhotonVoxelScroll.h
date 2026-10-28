// THE CASCADE SCROLL'S SCRATCH AND ITS SHIFT (ENGINE-UPGRADE-U5 stage 1; PHOTON-7 stage 2, AB S4/A4/A7).
//
// A cascade that scrolls by s cells keeps the voxels AND THE LIGHT its new box shares with the old one and voxelises
// only the entering slab:
//
//   1. THE SLAB, BUILT IN A SLAB-SIZED SCRATCH, in chunks over as many frames as the schedule gives it
//      (PhotonVoxelizer::buildChunks: the same dispatches, shader and merge as a whole build), each voxel stored at
//      the scratch's layout (below). The readers keep the UNSHIFTED live store the whole time: correct for where it
//      is, and nothing of it is half-built or cleared - no reader needs a valid box.
//   2. THE COMMIT, one frame: level 0 of the eleven geometry volumes and of the seven light textures (the total, its
//      back side, the direct term, its back side, the directional level 0's direct part per axis - two halves packed
//      along x, half resolution) is moved by -s cells IN PLACE, one dispatch per volume per moved axis
//      (Photon/ScrollShift: a thread per column walking toward the far end; positions are cell-relative since PHOTON-7
//      stage 1, so nothing re-resolves), the far end cleared as a whole build clears; the scratch's slabs are copied
//      into it (Photon/ScrollCopy with the slab's permutation); then the host lights what changed and regenerates the
//      mips (PhotonComponent::stepScroll, PhotonVoxelizer::finishScroll).
//
// THE SCRATCH'S LAYOUT: `layers` x res x res, every slab laid along its first axis - a slab of axis a stores the
// cell (x, y, z) at (its own axis first, the two others in increasing order) from its first layer (CellBox's
// storeAxis / storeBase). The scratch holds `layers` layers in all: a scroll carries the shifts that fit
// (PhotonComponent::planScroll), so a step on one axis is one slab and a diagonal one is two scrolls.
//
// WHY A SHIFT AND NOT A TOROIDAL ADDRESS (the preflight's ruling, the lead's decision 1; PHOTON-7: toroidal is not
// built): the readers construct the voxel address in ten places, every voxel sampler CLAMPs, the directional volumes
// pack both signs along x, and the plane march reads box-aligned planes at every mip - a wrap would mix the box's
// opposite edges. The shift changes no reader.
//
// WHY ON COMPUTE: an image copy is a transfer command and the GI region records compute only
// (gi.region_compute_only).
//
// ONE SCRATCH PER CASCADE, OWNED BY ITS CHAIN (built with the chain's volumes, a graphics region's work; destroyed
// with them): every cascade may build its slab at once, so nothing is leased and no scroll waits for another. THE
// JOBS are process-wide - a clone of Photon/ScrollShift and of Photon/ScrollCopy per volume format, made when a
// scratch is built (vr.warmup: no shader is compiled on a frame a headset is shown) and destroyed by releasePool at
// the engine's teardown, before the compute Hlms dies.
#ifndef JAH_PHOTON_VOXEL_SCROLL_H
#define JAH_PHOTON_VOXEL_SCROLL_H

#include "photon/voxel/PhotonVoxelizer.h"

#include "OgrePixelFormatGpu.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class HlmsComputeJob;

    class PhotonVoxelScroll
    {
    public:
        /// ONE SLAB OF A SCROLL: a box of the NEW volume's cells [lo, hi), the axis it enters along and its first
        /// layer in the scratch.
        struct Slab
        {
            uint32 lo[3] = { 0u, 0u, 0u }, hi[3] = { 0u, 0u, 0u };
            uint32 axis = 0u;
            uint32 layer = 0u;
        };
        /// A LIGHT TEXTURE THE COMMIT SHIFTS: its level 0 at `cellsPerTexel` volume cells a texel (1, or 2 for the
        /// directional direct parts), two halves packed along x or not.
        struct LightVolume
        {
            TextureGpu *texture = nullptr;
            uint32 cellsPerTexel = 1u;
            bool packedHalves = false;
        };

        /// Creates the scratch's level-0 volumes (a texture creation: a GRAPHICS region's work - the chain's build),
        /// `layers` x `like`'s resolution^2, in `like`'s formats, and makes sure every job a scroll dispatches on a
        /// volume of these formats exists and has dispatched once.
        PhotonVoxelScroll( RenderSystem *renderSystem, HlmsManager *hlmsManager,
                           TextureGpuManager *textureManager, PhotonVoxelizer *like, uint32 layers );
        ~PhotonVoxelScroll();

        /// Destroys the process-wide jobs - before the compute Hlms dies.
        static void releasePool();

        uint32 layers() const { return mLayers; }
        uint32 resolution() const { return mResolution; }
        const PhotonVoxelizer::VolumeSet &volumes() const { return mVolumes; }
        /// The bytes this scratch holds, for the VRAM accounting a status reports.
        size_t bytes() const;

        /// THE SCROLL'S FIRST STEP: the scratch cleared as a whole build clears (every value zero but the normal's
        /// 0.5 bias) - the slab's cells keep that until its chunks write them.
        void begin();
        /** THE COMMIT: every level-0 volume of `live` and the light textures `light` shifted by -`shift` cells in
            place (a voxel at new cell n holds old cell n + shift; |shift| < the resolution, even where a light
            texture holds two cells a texel), the far ends cleared, then the scratch's `slabs` copied in. `fault`
            (the test door gi.cascadeFaultCommit) throws after the first half of the shifts - a torn live store.
        */
        void commit( PhotonVoxelizer *live, const LightVolume *light, size_t numLight, const int32 shift[3],
                     const Slab *slabs, size_t numSlabs, bool fault = false );

        /// ONE VOLUME'S LEVEL 0 SHIFTED BY -`texels` ALONG `axis` IN PLACE, the far end set to `clear`
        /// (Photon/ScrollShift; `packedHalves`: two halves along x, each shifted on its own).
        static void shiftVolume( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *volume,
                                 uint32 axis, int32 texels, bool packedHalves, const float clear[4] );
        /// ONE BOX COPIED (Photon/ScrollCopy): `size` texels of `dst` from `dstOrigin`, read from `src` at
        /// `srcOrigin` + the box's id permuted by `permAxis` (0 the identity; Slab's layout otherwise).
        static void copyBox( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *src,
                             TextureGpu *dst, const uint32 srcOrigin[3], const uint32 dstOrigin[3],
                             const uint32 size[3], uint32 permAxis = 0u );
        /// ONE BOX OF ONE VOLUME'S LEVEL 0 SET TO `clear` (Photon/ScrollCopy's clear mode; PHOTON-7 stage 8A).
        static void clearBox( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *dst,
                              const uint32 origin[3], const uint32 size[3], const float clear[4] );
        /** THE REGION REBUILD'S CLEAR (PHOTON-7 stage 8A): the box [origin, origin + size) of the eleven geometry
            volumes' level 0 set to what a whole build's clear writes (zero, the normal's 0.5 bias), so a voxeliser's
            chunk build INTO THE LIVE STORE (PhotonVoxelizer::buildChunks, the identity layout) writes the box exactly
            as a whole build would. No scratch: a scroll in flight keeps its own.
        */
        static void clearRegion( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                 const PhotonVoxelizer::VolumeSet &live, const uint32 origin[3], const uint32 size[3] );

    private:
        RenderSystem      *mRenderSystem;
        HlmsManager       *mHlmsManager;
        TextureGpuManager *mTextureManager;
        ComputeTools      *mComputeTools;
        uint32             mResolution;
        uint32             mLayers;
        PhotonVoxelizer::VolumeSet mVolumes;
        ResourceTransitionArray mResourceTransitions;
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
