/*
-----------------------------------------------------------------------------
This source file is part of OGRE-Next
    (Object-oriented Graphics Rendering Engine)
For the latest info, see http://www.ogre3d.org/

Copyright (c) 2000-present Torus Knot Software Ltd

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
-----------------------------------------------------------------------------
*/
#ifndef JAH_PHOTON_VOXEL_MATERIAL_H
#define JAH_PHOTON_VOXEL_MATERIAL_H

#include "OgreHlmsPbsPrerequisites.h"

#include "OgreFastArray.h"
#include "OgreId.h"

#include "ogrestd/map.h"
#include "ogrestd/set.h"
#include "ogrestd/vector.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelMaterial : public IdObject
    {
    public:
        struct DatablockConversionResult
        {
            uint32             slotIdx;
            /// Which material POOL the slot is in, as an index into PhotonVoxelMaterial's own
            /// bucket list. Together with slotIdx it names the material without naming
            /// a pointer, which is what lets PhotonVoxelizer order its dispatches the same
            /// way in every process (see VoxelizerBucket::operator<).
            uint32             bucketIdx;
            ConstBufferPacked *constBuffer;
            uint16             diffuseTexIdx;
            uint16             emissiveTexIdx;
            DatablockConversionResult() :
                slotIdx( (uint32)-1 ),
                bucketIdx( (uint32)-1 ),
                constBuffer( 0 ),
                diffuseTexIdx( std::numeric_limits<uint16>::max() ),
                emissiveTexIdx( std::numeric_limits<uint16>::max() )
            {
            }

            bool hasDiffuseTex() const { return diffuseTexIdx != std::numeric_limits<uint16>::max(); }
            bool hasEmissiveTex() const { return emissiveTexIdx != std::numeric_limits<uint16>::max(); }
        };

    protected:
        typedef set<HlmsDatablock *>::type HlmsDatablockSet;
        struct MaterialBucket
        {
            ConstBufferPacked *buffer = nullptr;
            bool               hasDiffuse = false;
            bool               hasEmissive = false;
            HlmsDatablockSet   datablocks;
        };
        typedef vector<MaterialBucket>::type BucketVec;

        typedef map<HlmsDatablock *, DatablockConversionResult>::type DatablockConversionResultMap;

        DatablockConversionResultMap mDatablockConversionResults;

        BucketVec mBuckets;

        VaoManager *mVaoManager;

        typedef map<TextureGpu *, uint16>::type TextureToPoolEntryMap;

        uint16                mNumUsedPoolSlices;
        TextureGpu           *mTexturePool;
        TextureGpuManager    *mTextureGpuManager;
        CompositorManager2   *mCompositorManager;
        TextureGpu           *mDownsampleTex;
        Pass                 *mDownsampleMatPass2DArray;
        Pass                 *mDownsampleMatPass2D;
        CompositorWorkspace  *mDownsampleWorkspace2DArray;
        CompositorWorkspace  *mDownsampleWorkspace2D;
        TextureToPoolEntryMap mTextureToPoolEntry;

        DatablockConversionResult addDatablockToBucket( HlmsDatablock  *datablock,
                                                        MaterialBucket &bucket );
        /// The one conversion: writes `datablock`'s row at `slot` of `bucket` and fills
        /// `out`. Used for a first conversion AND for an in-place refresh.
        void writeRow( HlmsDatablock *datablock, MaterialBucket &bucket, uint32 slot,
                       DatablockConversionResult &out );

        uint16 getPoolSliceIdxForTexture( TextureGpu *texture );

        void resizeTexturePool();

        MaterialBucket *findFreeBucketFor( HlmsDatablock *datablock );

    public:
        PhotonVoxelMaterial( IdType id, VaoManager *vaoManager, CompositorManager2 *compositorManager,
                     TextureGpuManager *textureGpuManager );
        ~PhotonVoxelMaterial();

        void initTempResources( SceneManager *sceneManager );
        void destroyTempResources();

        /** Converts a datablock, or - if it is already converted - RE-READS it and
            re-writes its row in place, at the slot it already owns.
        @remarks
            If the datablock contains textures, initTempResources must already have
            been called.

            JAHSHAKA (ATOM P4b): a hit used to hand back the row exactly as it was first
            written. That was harmless while every store died with its voxelizer on each
            rebuild; a store SHARED by a chain outlives rebuilds, and a material edited
            after its first conversion kept its first parameters for the rest of its life
            (measured: emissive converted at 0.5 read 0.5 at 1, 3 and 12). The row is now
            always re-read, so (bucketIdx, slotIdx) - the pair a GPU instance table names
            a material by - stays valid AND describes the material as it is now.
        @param outMoved
            Set true when the datablock had to MOVE to another bucket: a bucket is
            homogeneous in whether its materials carry an albedo and an emissive map (it
            decides the compute job variant), so adding or removing either map changes
            the datablock's (bucketIdx, slotIdx). Anything that recorded the old pair is
            then wrong and must be told.
        */
        DatablockConversionResult addDatablock( HlmsDatablock *datablock, bool *outMoved = 0 );

        /// The cached conversion, or null - a LOOKUP ONLY. Never converts, never needs the
        /// temp resources, never re-reads the datablock: it is what a per-frame path (a
        /// GPU scene composing an instance's material word) may ask.
        const DatablockConversionResult *lookupDatablock( const HlmsDatablock *datablock ) const;

        /** RE-READS EVERY CONVERTED DATABLOCK, in place where it can, and reports the ones
            that had to move. The owner calls it when a material's parameters may have
            changed; it is the store's end of the VCT lifecycle's "always from scratch"
            rule for the one piece of state that now outlives a rebuild. The texture pool
            is RE-COPIED too (its by-pointer slice cache is forgotten first), so an edited
            or dead texture never survives in it. Needs the temp resources (every texture
            a row binds is rendered into the pool).
        */
        void refreshAll( FastArray<HlmsDatablock *> *moved );

        /// THE BUCKETS, which ARE the voxelize dispatches (one per bucket per octant): a
        /// bucket is one material POOL - one const buffer of up to 1024 rows - and it is
        /// homogeneous in whether its rows sample the albedo and emissive maps, which is
        /// what picks the compute job variant.
        size_t getNumBuckets() const { return mBuckets.size(); }
        ConstBufferPacked *getBucketBuffer( size_t idx ) const { return mBuckets[idx].buffer; }
        bool getBucketHasDiffuse( size_t idx ) const { return mBuckets[idx].hasDiffuse; }
        bool getBucketHasEmissive( size_t idx ) const { return mBuckets[idx].hasEmissive; }
        /// JAHSHAKA fork ad452604a+0338ca7f2+c4c80b5f7 (was 0081): FORGET A DATABLOCK THAT IS ABOUT TO DIE. The
        /// conversion cache is keyed by the raw datablock pointer across builds,
        /// so a datablock destroyed and another created at the same address
        /// would ALIAS the dead one's slot. The host's only answer used to be a
        /// from-scratch re-voxelisation of every volume on every material death;
        /// this erases the cache entry instead. The bucket keeps the dead pointer
        /// in its membership set on purpose: slots are numbered by that set's
        /// size, so removing it would hand the next datablock a slot another
        /// live one already holds. One slot per death leaks until the material
        /// object is recreated (a mode or quality change); stated, not hidden. A
        /// datablock that MOVES bucket (addDatablock's outMoved) leaks its old slot
        /// the same way and for the same reason.
        void removeDatablock( const HlmsDatablock *datablock );

        TextureGpu *getTexturePool() const { return mTexturePool; }

        /// Are the temp resources up? A store shared by a whole chain is bracketed ONCE
        /// per rebuild by its owner, so a voxelizer's build can assert it rather than
        /// discover it as an untextured material.
        bool hasTempResources() const { return mDownsampleTex != 0; }

    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
