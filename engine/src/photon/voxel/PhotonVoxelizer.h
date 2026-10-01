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
#ifndef JAH_PHOTON_VOXELIZER_H
#define JAH_PHOTON_VOXELIZER_H

#include "photon/voxel/PhotonVoxelizerSourceBase.h"

#include "OgreResourceTransition.h"
#include "Vao/OgreVertexBufferPacked.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelMaterial;

    namespace PhotonVoxelizerJobSetting
    {
        /// Jahshaka (ATOM P4): `Index32bit` and `CompressedVertexFormat` are GONE.
        /// They existed because the voxelizer owned PRIVATE COPIES of every mesh's
        /// geometry, in one of two formats, in one of two index widths - so the format
        /// was a property of the buffer a dispatch BOUND and therefore a shader
        /// permutation and a bucket key. The shader now reads each instance's geometry
        /// where the raster reads it, through the buffer device addresses and the
        /// layout in its geometry row, so the width and the packing are per-MESH DATA
        /// read at runtime and no longer split a dispatch. What is left is the one
        /// thing a dispatch genuinely binds differently: whether the material pool's
        /// rows sample an albedo and an emissive map.
        enum PhotonVoxelizerJobSetting
        {
            HasDiffuseTex = 1u << 0u,
            HasEmissiveTex = 1u << 1u,
        };
    }

    /**
    @class PhotonVoxelizer
        THE RASTER VOXELIZER, FED BY THE GPU (Jahshaka, ATOM P4b).

        Upstream's voxelizer was fed on the CPU: items were added one by one, and every
        build() downloaded their geometry, bucketed them by material, culled them against
        each octant and uploaded an instance buffer. Each of those steps is gone:

        1. GEOMETRY is read where the raster keeps it: a host-owned table of
           GeometryRows (two device addresses and a vertex layout per (mesh, level,
           submesh)), bound with setGeometrySource and written ONCE per mesh.
        2. MATERIALS live in a store the voxelizer does not own (the constructor), shared
           by a whole chain so that a material's (pool, slot) is a scene-wide fact.
        3. INSTANCES arrive as a host-written buffer of 96-byte records, grouped by
           (octant, bucket) with each group's (start, count) in a RANGES buffer - both
           written by a compute job on the device (setInstanceSource). No Item* is held,
           so the "raw Item* until removeAllItems" lifetime rule does not exist here any
           more.

        What remains here is the part that was always the voxelizer's: the voxel volumes,
        the octants, and one voxelize dispatch per (octant, bucket), each reading its
        loop bound from the ranges buffer - the dispatch's thread count is the OCTANT's,
        never the instance count's, so no indirect dispatch is needed.
    */
    class PhotonVoxelizer : public PhotonVoxelizerSourceBase
    {
    public:
        /** JAHSHAKA (ATOM P4) - ONE (mesh, LOD level, submesh)'s GEOMETRY, AS THE
            SHADER READS IT. std430, 48 bytes, three uvec4 lanes so C++ and GLSL agree
            with no padding rule to remember.

            THE WHOLE POINT OF THIS STRUCT is that it holds ADDRESSES, not data. The
            voxelizer used to download every mesh's vertices to the CPU, unpack them to
            8 floats each and upload the result into a private buffer, once per build();
            indices were copied GPU-side into two more private buffers. All of it is
            gone: a row names where the RASTER's vertices and indices already live
            (VaoManager::getBufferDeviceAddress) and how they are laid out, and the
            compute shader dereferences that through GL_EXT_buffer_reference.

            WHY THE ROW IS PER (mesh, LEVEL, submesh) AND NOT PER MESH. A LOD level is
            its own index buffer over the SAME vertex buffer, so the vertex address is
            the mesh's and the index address is the level's; and one mesh instanced at
            two distances is now voxelized at two levels in the same build (the finest
            request no longer wins for every instance of a mesh - AT-A10). A submesh is
            its own vertex buffer entirely.
        */
        struct GeometryRow
        {
            /// Device address of vertex 0 of this submesh's vertex buffer, and of index
            /// 0 of this (submesh, level)'s index range. Split into two uint32 because
            /// shaderInt64 is not enabled on this device; GLSL rebuilds the reference
            /// from a uvec2 (GL_EXT_buffer_reference_uvec2).
            uint32 posAddress[2];
            uint32 idxAddress[2];
            uint32 vertexStride;   ///< bytes between vertices
            uint32 posOffset;      ///< bytes from the vertex start to VES_POSITION
            uint32 normalOffset;   ///< ditto VES_NORMAL; 0xFFFFFFFF when absent
            uint32 uvOffset;       ///< ditto VES_TEXTURE_COORDINATES; 0xFFFFFFFF absent
            /// bit 0: indices are 32-bit. bit 1: positions are float3 (always, today -
            /// reserved so a packed position format becomes data, not a permutation).
            uint32 flags;
            /// Index ELEMENTS skipped by rounding `idxAddress` down to a 4-byte
            /// boundary. A 16-bit index buffer can start at an odd uint16, and a
            /// buffer_reference of uints must be 4-byte aligned, so the address is
            /// floored and the remainder rides here - the same bookkeeping upstream's
            /// adjustIndexOffsets16 did for its private copy, now costing no copy.
            uint32 idxBias;
            uint32 padding[2];
        };

        /** JAHSHAKA (ATOM P4b): DESCRIBE ONE (mesh, level, submesh) WITHOUT A VOXELIZER.

            The row is a fact about a MESH, not about a voxelisation: the same two
            addresses and the same layout serve every cascade that voxelises it and
            (phase D) the raster that draws it. So the table belongs to whoever owns
            meshes - the host's GPU scene, which writes one row per (mesh, level,
            submesh) ONCE when the mesh is first attached - and this is the describer
            it calls. The voxelizer used to rebuild the whole table on EVERY build()
            of EVERY cascade, which is a CPU walk over the scene's geometry per
            rebuild for data that never changes.

            THE FORMAT STAYS OGRE'S because the shader that reads it is Ogre's.
        @param level
            Clamped to the levels the submesh has.
        @return
            False - with one log line naming the mesh and the reason - when this
            (level, submesh) cannot be read in place: no index buffer, no float3
            VES_POSITION, a stride or element offset that is not a multiple of 4, or
            a device with no buffer device addresses. `out` is then untouched.
        */
        static bool describeGeometryRow( const MeshPtr &mesh, uint32 level, uint32 submesh,
                                         VaoManager *vaoManager, GeometryRow &out );

        /** THE GEOMETRY TABLE THIS VOXELIZER READS - the host's, bound for every
            dispatch (the voxelise jobs and the AABB calculator).

            A voxelizer with no source keeps NOTHING of its own: the per-build table it
            used to own is deleted, not kept as a fallback. Setting it to null makes
            every build an empty one (and says so once).
        */
        void setGeometrySource( UavBufferPacked *rows ) { mGeometryBuffer = rows; }

        /// THE INSTANCE RECORD the voxelize shader reads (`InstanceBuffer` in
        /// Voxelizer_piece_cs.any), 96 bytes: the world transform as three ROWS, the
        /// record's world AABB centre, its half size with the material SLOT bit-cast into
        /// .w, and (geometry row, first index, index count, unused). A host writing
        /// records - the GPU gather - writes exactly this.
        static constexpr uint32 kInstanceRecordBytes = 96u;
        /// The indices one record covers at most. A mesh with more triangles is split
        /// into partitions of this many indices, each with its own AABB, so a voxel group
        /// that misses a partition skips it whole - the broadphase that keeps a big mesh
        /// from costing every group every triangle. The host partitions; this is the
        /// number it partitions by.
        static constexpr uint32 kIndicesPerPartition = 2001u;

        /// THE RANGE a (octant, bucket) dispatch reads, as an index into the ranges
        /// buffer. The host's gather and build() must agree on it, so it is written once.
        static uint32 rangeIndex( uint32 octant, uint32 bucket, uint32 numBuckets )
        {
            return octant * numBuckets + bucket;
        }

        /** THE INSTANCE SOURCE - the host's, NOT OWNED, bound for every dispatch.
        @param records
            96-byte records (kInstanceRecordBytes), created with BB_FLAG_UAV |
            BB_FLAG_READONLY so this class can take its read-only view.
        @param ranges
            One uvec2 (start, count) per rangeIndex( octant, bucket, PhotonVoxelMaterial::getNumBuckets() ):
            the records of that (octant, bucket), which the voxelize shader loops over.
        @remarks
            Re-bind before every build(): a host that grows its buffers destroys the old
            ones, and a pointer taken earlier would dangle (Ogre's destroy is delayed, so
            a dangling read is not a fault - it is a hung channel).
        */
        void setInstanceSource( UavBufferPacked *records, UavBufferPacked *ranges );

        /** THE MESH-LOCAL AABB OF EVERY PARTITION, computed on the device by Ogre's own
            VCT/AabbCalculator job - the job this class used to run on every build over
            its private geometry. A partition's AABB is a fact about its MESH, so a host
            computes it once when the mesh set changes and keeps it.
        @param geometryRows
            The host's geometry row table (the rows the partitions name).
        @param partitions
            PFG_RGBA32_UINT, one (geometry row, first index, index count, 0) per
            partition.
        @param outAabbs
            Two float4 per partition: (centre, 1) and (half size, 0), mesh-local.
        */
        static void computePartitionAabbs( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                           UavBufferPacked *geometryRows,
                                           TexBufferPacked *partitions, UavBufferPacked *outAabbs,
                                           uint32 numPartitions );

    protected:
        /// HlmsComputeJob have internal caches, thus we could dynamically change properties
        /// and let the internal cache handle whether a compute job needs to be compiled.
        ///
        /// However the way we will be using may abuse the cache too much, thus we pre-set
        /// all variants as long as the number of variants is manageable.
        HlmsComputeJob *mComputeJobs[1u << 2u];

        /// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065) — THE ORDER-INDEPENDENT MERGE'S ACCUMULATOR.
        ///
        /// PFG_R32_UINT, (mWidth, mHeight, mDepth * 13): thirteen texels per voxel,
        /// interleaved in Z (albedo sum rgba, raw normal sum xyz, emissive sum
        /// rgb, folded normal sum xyz), all in the fixed-point integers
        /// Samples/Media/VCT/VoxelMerge_piece_cs.any defines — which also says
        /// why it is thirteen R32 texels and not four RGBA32 ones: MEASURED, the
        /// four-texel layout costs the same GPU time and 12 MB more per 64^3
        /// volume (VOXMERGE-2; the earlier reason, a device loss from the
        /// 128-bit clear, was fork b028638c1 (was 0067)'s hazard seen from here).
        /// 52 bytes per voxel, which is 13.6 MB at 64^3 and 109 MB at 128^3, and
        /// RESIDENT for the voxeliser's life since fork ad452604a+0338ca7f2+c4c80b5f7 (was 0071) (the per-build
        /// residency round trip of an image this size was the second Xid 109
        /// site).
        TextureGpu *mMergeAccumTex;

        /// THE HOST'S TABLES AND BUFFERS - NOT OWNED (setGeometrySource,
        /// setInstanceSource). This class used to own a geometry table rebuilt per
        /// build(), an instance buffer filled per build() on the CPU, and before that four
        /// private copies of the world's geometry.
        UavBufferPacked      *mGeometryBuffer;
        UavBufferPacked      *mRecords;
        ReadOnlyBufferPacked *mRecordsAsTex;
        UavBufferPacked      *mRanges;
        uint32                mLastDispatchCount;

        bool mNeedsAlbedoMipmaps;
        bool mNeedsAllMipmaps;

        ComputeTools *mComputeTools;

        PhotonVoxelMaterial *mVctMaterial;

        struct Octant
        {
            uint32 x, y, z;
            uint32 width, height, depth;
            Aabb   region;
        };

        FastArray<Octant> mOctants;
        /// The division `dividideOctants` was last called with, so that moving the
        /// region can re-derive the octants from the SAME division instead of
        /// leaving them describing the box that has just been replaced.
        /// 0 = never divided, which is the only state in which a move has nothing
        /// to re-derive.
        uint32 mNumOctantsX, mNumOctantsY, mNumOctantsZ;

        ResourceTransitionArray mResourceTransitions;

        void createComputeJobs();
        /// Unbinds every host buffer from the shared compute jobs - they live in
        /// HlmsCompute and outlive this build and this object, and their descriptor
        /// sets hold raw pointers.
        void clearComputeJobResources();

        void createVoxelTextures();
        /// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): drops mMergeAccumTex too.
        void destroyVoxelTextures() override;

        void clearVoxels();

    public:
        /** @param materialStore
                THE MATERIAL STORE THIS VOXELIZER USES, AND DOES NOT OWN. Required.

                It used to `new` its own, which made (bucketIdx, slotIdx) a PER-STORE
                fact: `findFreeBucketFor` fills buckets in insertion order, and a chain's
                cascades see different attach sets in different orders, so the SAME
                datablock got a different pool and slot in every cascade. Nothing
                scene-wide could then name a material - which is exactly what a GPU
                instance feed needs, one word per instance, for every cascade at once.

                Sharing one store across a chain is also the better shape on its own
                terms: one conversion per datablock instead of one per cascade, one set
                of pool const buffers, one texture pool, one by-pointer alias cache and
                one eviction call. AND IT CLOSES A HAZARD: the compute jobs the
                dispatches use are CLONED PER VARIANT AND SHARED between voxelizers, so a
                job could hold one store's texture pool in its descriptor while another
                voxelizer dispatched it. With one store there is one pool.

                The owner creates it before any voxelizer and destroys it after all of
                them. build() converts nothing - the owner does, before it gathers - so
                build() needs none of the store's temp resources.
        */
        PhotonVoxelizer( IdType id, RenderSystem *renderSystem, HlmsManager *hlmsManager,
                      bool correctAreaLightShadows, PhotonVoxelMaterial *materialStore );
        ~PhotonVoxelizer();

        void _setNeedsAllMipmaps( bool bNeedsAllMipmaps ) { mNeedsAllMipmaps = bNeedsAllMipmaps; }

        /** THE REGION, always explicit. The automatic region (fit to the added items) is
            GONE with the items: this class holds none, and the host - which fits the
            region to what it wants voxelised - always passed one anyway.
        */
        void setRegionToVoxelize( const Aabb &regionToVoxelize );

        void dividideOctants( uint32 numOctantsX, uint32 numOctantsY, uint32 numOctantsZ );

        /** Changes resolution. Note that after calling this, you will need to call
            PhotonVoxelizer::build again, and PhotonVoxelLighting::build again.
        @param width
        @param height
        @param depth
        */
        void setResolution( uint32 width, uint32 height, uint32 depth );

        void build( SceneManager *sceneManager );

        /// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065) — HOW MANY DISPATCHES THE LAST build() ISSUED: one per
        /// material bucket per octant, the buckets being the STORE's (so the scene's
        /// pool count, not this volume's: the gather writes zero records to a bucket
        /// this volume does not hold, and that dispatch loops over none), and 0 when
        /// the build had no instance source and only cleared the volume.
        uint32 getLastDispatchCount() const { return mLastDispatchCount; }
        size_t getNumOctants() const { return mOctants.size(); }
        /// The octant's world box - what a host's gather culls records against.
        const Aabb &getOctantRegion( size_t idx ) const { return mOctants[idx].region; }

        /// JAHSHAKA fork ad452604a+0338ca7f2+c4c80b5f7 (was 0081): the material store, so a host can evict a dying
        /// datablock (PhotonVoxelMaterial::removeDatablock) instead of re-voxelising every
        /// volume. NOT OWNED - see the constructor. The host that owns the store
        /// should evict on the STORE, once, rather than through each voxelizer.
        PhotonVoxelMaterial *getVctMaterial() const { return mVctMaterial; }
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
