// THE CASCADE SCROLL'S SCRATCH AND ITS SHIFT (photon/voxel/PhotonVoxelScroll.h has the design).
#include "photon/voxel/PhotonVoxelScroll.h"

#include "photon/PhotonDispatch.h"

#include "Compute/OgreComputeTools.h"
#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreException.h"
#include "OgreLwString.h"
#include "OgreProfiler.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreRenderSystem.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpu.h"
#include "OgreTextureGpuManager.h"

#include <map>
#include <utility>

namespace Ogre
{
    namespace
    {
        uint32 gScrollCloneId = 0u;
        /// THE PROCESS-WIDE JOBS (the header's note): a clone of Photon/ScrollShift (kind 0) and of
        /// Photon/ScrollCopy (kind 1) per volume format, and the manager they were cloned in.
        std::map<std::pair<int, PixelFormatGpu>, HlmsComputeJob *> gScrollJobs;
        HlmsManager *gScrollJobsManager = nullptr;

        HlmsComputeJob *scrollJob( HlmsManager *hlmsManager, int kind, PixelFormatGpu format )
        {
            if( gScrollJobsManager && gScrollJobsManager != hlmsManager )
                gScrollJobs.clear();   // a second engine in the process: the old one's jobs died with its Hlms
            gScrollJobsManager = hlmsManager;
            const auto key = std::make_pair( kind, format );
            const auto it = gScrollJobs.find( key );
            if( it != gScrollJobs.end() )
                return it->second;
            HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
            HlmsComputeJob *base =
                hlmsCompute->findComputeJob( kind == 0 ? "Photon/ScrollShift" : "Photon/ScrollCopy" );
            char tmpBuffer[96];
            LwString name( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            name.a( kind == 0 ? "Photon/ScrollShift/" : "Photon/ScrollCopy/", ++gScrollCloneId );
            HlmsComputeJob *job = base->clone( name.c_str() );
            job->setProperty( "copy_uint", PixelFormatGpuUtils::isInteger( format ) ? 1 : 0 );
            gScrollJobs[key] = job;
            return job;
        }

        /// The live store's volumes, in one fixed order: the shift, the copy-in and the clear walk it.
        constexpr size_t kVolumes = 16u;
        void volumeList( const PhotonVoxelizer::VolumeSet &v, TextureGpu *out[kVolumes] )
        {
            out[0] = v.albedo;
            out[1] = v.emissive;
            out[2] = v.accumVal;
            out[3] = v.coverage[0];
            out[4] = v.coverage[1];
            out[5] = v.position[0];
            out[6] = v.position[1];
            out[7] = v.metal;     // METAL-BOUNCE-1
            out[8] = v.emit[0];   // JUNCTION-NORMAL-1
            out[9] = v.emit[1];
            for( size_t a = 0u; a < 3u; ++a )   // VOXEL-R13: the halves' own faces
            {
                out[10u + a] = v.halfSide[a];
                out[13u + a] = v.halfNrm[a];
            }
        }
        const char *const kVolumeNames[kVolumes] = { "Albedo",    "Emissive",  "AccumVal",  "CoverageP",
                                                      "CoverageN", "PositionP", "PositionN", "Metal",
                                                      "EmitP",     "EmitN",     "HalfSideX", "HalfSideY",
                                                      "HalfSideZ", "HalfNrmX",  "HalfNrmY",  "HalfNrmZ" };
        /// What a whole build clears each volume to (PhotonVoxelizer::clearVoxels): zero, the halves' normals 0.5
        /// (a half's own axis, VOXEL-R13).
        const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float kHalfNrmClear[4] = { 0.5f, 0.5f, 0.5f, 0.5f };
        const float *clearOf( size_t i )
        {
            return i >= 13u && i < 16u ? kHalfNrmClear : kZero;
        }

        /// The copy job's parameters, EVERY one each dispatch (a clone keeps its last values): `clear` null = a copy,
        /// else the box is written with that value (PhotonScrollCopy_cs.glsl's clear).
        void setCopyParams( HlmsComputeJob *job, const uint32 srcOrigin[3], const uint32 dstOrigin[3],
                            const uint32 size[3], uint32 permAxis, const float *clear )
        {
            ShaderParams::Param pSrc, pDst, pSize, pPerm, pMode, pClearF, pClearU;
            pSrc.name = "srcOrigin";
            pDst.name = "dstOrigin";
            pSize.name = "copySize";
            pPerm.name = "permAxis";
            pMode.name = "copyClear";
            pClearF.name = "clearF";
            pClearU.name = "clearU";
            pSrc.setManualValue( srcOrigin, 3u );
            pDst.setManualValue( dstOrigin, 3u );
            pSize.setManualValue( size, 3u );
            pPerm.setManualValue( permAxis );
            const uint32 mode[4] = { clear ? 1u : 0u, 0u, 0u, 0u };
            pMode.setManualValue( mode, 4u );
            const float cf[4] = { clear ? clear[0] : 0.0f, clear ? clear[1] : 0.0f, clear ? clear[2] : 0.0f,
                                  clear ? clear[3] : 0.0f };
            uint32 cu[4];
            for( int k = 0; k < 4; ++k )
                cu[k] = uint32( cf[k] );
            pClearF.setManualValue( cf, 4u );
            pClearU.setManualValue( cu, 4u );
            ShaderParams &params = job->getShaderParams( "default" );
            params.mParams.clear();
            params.mParams.push_back( pSrc );
            params.mParams.push_back( pDst );
            params.mParams.push_back( pSize );
            params.mParams.push_back( pPerm );
            params.mParams.push_back( pMode );
            params.mParams.push_back( pClearF );
            params.mParams.push_back( pClearU );
            params.setDirty();
        }
    }  // namespace

    PhotonVoxelScroll::PhotonVoxelScroll( RenderSystem *renderSystem, HlmsManager *hlmsManager,
                                          TextureGpuManager *textureManager, PhotonVoxelizer *like,
                                          uint32 layers ) :
        mRenderSystem( renderSystem ),
        mHlmsManager( hlmsManager ),
        mTextureManager( textureManager ),
        mComputeTools( new ComputeTools( hlmsManager->getComputeHlms() ) ),
        mResolution( 0u ),
        mLayers( layers )
    {
        const PhotonVoxelizer::VolumeSet live = like->liveVolumes();
        TextureGpu *src[kVolumes];
        volumeList( live, src );
        mResolution = src[0] ? src[0]->getWidth() : 0u;
        OGRE_ASSERT_LOW( mLayers >= 2u && mLayers <= mResolution );
        TextureGpu *dst[kVolumes] = {};
        const uint32 id = ++gScrollCloneId;
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            OGRE_ASSERT_LOW( src[i] && src[i]->getWidth() == mResolution &&
                             src[i]->getHeight() == mResolution && src[i]->getDepth() == mResolution );
            // LEVEL 0 ONLY, A SLAB'S LAYERS ALONG X: copied into the live volumes at the commit, whose chains are
            // then regenerated (PhotonVoxelizer::finishScroll). A UAV and nothing more, like the live ones.
            dst[i] = textureManager->createTexture(
                "PhotonScrollScratch/" + StringConverter::toString( id ) + "/" + kVolumeNames[i],
                GpuPageOutStrategy::Discard, TextureFlags::NotTexture | TextureFlags::Uav,
                TextureTypes::Type3D );
            dst[i]->setPixelFormat( src[i]->getPixelFormat() );
            dst[i]->setResolution( mLayers, mResolution, mResolution );
            dst[i]->setNumMipmaps( 1u );
            dst[i]->scheduleTransitionTo( GpuResidency::Resident );
        }
        mVolumes.albedo = dst[0];
        mVolumes.emissive = dst[1];
        mVolumes.accumVal = dst[2];
        mVolumes.coverage[0] = dst[3];
        mVolumes.coverage[1] = dst[4];
        mVolumes.position[0] = dst[5];
        mVolumes.position[1] = dst[6];
        mVolumes.metal = dst[7];
        mVolumes.emit[0] = dst[8];
        mVolumes.emit[1] = dst[9];
        for( size_t a = 0u; a < 3u; ++a )
        {
            mVolumes.halfSide[a] = dst[10u + a];
            mVolumes.halfNrm[a] = dst[13u + a];
        }

        // THE SCROLL'S JOBS ARE BUILT HERE, AT THE CHAIN'S BUILD (vr.warmup: no shader is compiled on a frame a headset
        // is shown, and a scroll is what the first head movement does): every format's shift and copy dispatched once
        // with nothing to do - a shift of 0 texels and a copy of an empty box, on this scratch's own volumes.
        mRenderSystem->endRenderPassDescriptor();
        const uint32 zero[3] = { 0u, 0u, 0u };
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            shiftVolume( mHlmsManager, mRenderSystem, dst[i], 0u, 0, 1u, kZero );
            copyBox( mHlmsManager, mRenderSystem, dst[i], dst[i], zero, zero, zero );
        }
    }
    //-------------------------------------------------------------------------
    PhotonVoxelScroll::~PhotonVoxelScroll()
    {
        TextureGpu *dst[kVolumes];
        volumeList( mVolumes, dst );
        for( size_t i = 0u; i < kVolumes; ++i )
            if( dst[i] )
                mTextureManager->destroyTexture( dst[i] );
        mVolumes = PhotonVoxelizer::VolumeSet();
        delete mComputeTools;
        mComputeTools = 0;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::releasePool()
    {
        if( gScrollJobsManager )
        {
            HlmsCompute *hlmsCompute = gScrollJobsManager->getComputeHlms();
            for( auto &kv : gScrollJobs )
                hlmsCompute->destroyComputeJob( kv.second->getName() );
        }
        gScrollJobs.clear();
        gScrollJobsManager = nullptr;
    }
    //-------------------------------------------------------------------------
    size_t PhotonVoxelScroll::bytes() const
    {
        TextureGpu *dst[kVolumes];
        volumeList( mVolumes, dst );
        size_t total = 0u;
        for( size_t i = 0u; i < kVolumes; ++i )
            if( dst[i] )
                total += dst[i]->getSizeBytes();
        return total;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::shiftVolume( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *volume,
                                         uint32 axis, int32 texels, uint32 blocks, const float clear[4] )
    {
        HlmsComputeJob *job = scrollJob( hlmsManager, 0, volume->getPixelFormat() );
        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.texture = volume;
        uavSlot.pixelFormat = volume->getPixelFormat();
        uavSlot.access = ResourceAccess::ReadWrite;
        job->_setUavTexture( 0u, uavSlot );
        const uint32 dims[3] = { volume->getWidth(), volume->getHeight(), volume->getDepth() };
        const uint32 n = axis == 0u ? std::max( blocks, 1u ) : 1u;
        // THE COLUMNS: the two other axes in increasing order, and the blocks of a packed volume along x.
        const uint32 b = axis == 0u ? 1u : 0u, c = axis == 2u ? 1u : 2u;
        const uint32 len = axis == 0u ? dims[0] / n : dims[axis];
        const uint32 shiftAxis[4] = { axis, uint32( texels ), len, n > 1u ? dims[0] / n : 0u };
        const uint32 columns[4] = { dims[b], dims[c], n, 0u };
        const uint32 *tpg = job->getThreadsPerGroup();
        job->setNumThreadGroups( std::max( 1u, ( columns[0] + tpg[0] - 1u ) / tpg[0] ),
                                 std::max( 1u, ( columns[1] + tpg[1] - 1u ) / tpg[1] ),
                                 std::max( 1u, ( columns[2] + tpg[2] - 1u ) / tpg[2] ) );
        uint32 clearU[4];
        for( int k = 0; k < 4; ++k )
            clearU[k] = uint32( clear[k] );
        ShaderParams::Param pAxis, pColumns, pClearF, pClearU;
        pAxis.name = "shiftAxis";
        pColumns.name = "shiftColumns";
        pClearF.name = "clearF";
        pClearU.name = "clearU";
        pAxis.setManualValue( shiftAxis, 4u );
        pColumns.setManualValue( columns, 4u );
        pClearF.setManualValue( clear, 4u );
        pClearU.setManualValue( clearU, 4u );
        ShaderParams &params = job->getShaderParams( "default" );
        params.mParams.clear();
        params.mParams.push_back( pAxis );
        params.mParams.push_back( pColumns );
        params.mParams.push_back( pClearF );
        params.mParams.push_back( pClearU );
        params.setDirty();
        jahshaka::engine::detail::photonDispatch( hlmsManager->getComputeHlms(), job, renderSystem );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::copyBox( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *src,
                                     TextureGpu *dst, const uint32 srcOrigin[3], const uint32 dstOrigin[3],
                                     const uint32 size[3], uint32 permAxis )
    {
        OGRE_ASSERT_LOW( src->getPixelFormat() == dst->getPixelFormat() );
        HlmsComputeJob *job = scrollJob( hlmsManager, 1, dst->getPixelFormat() );
        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.texture = src;
        uavSlot.pixelFormat = src->getPixelFormat();
        uavSlot.access = ResourceAccess::Read;
        job->_setUavTexture( 0u, uavSlot );
        uavSlot.texture = dst;
        uavSlot.pixelFormat = dst->getPixelFormat();
        uavSlot.access = ResourceAccess::Write;
        job->_setUavTexture( 1u, uavSlot );
        const uint32 *tpg = job->getThreadsPerGroup();
        job->setNumThreadGroups( std::max( 1u, ( size[0] + tpg[0] - 1u ) / tpg[0] ),
                                 std::max( 1u, ( size[1] + tpg[1] - 1u ) / tpg[1] ),
                                 std::max( 1u, ( size[2] + tpg[2] - 1u ) / tpg[2] ) );
        setCopyParams( job, srcOrigin, dstOrigin, size, permAxis, nullptr );
        jahshaka::engine::detail::photonDispatch( hlmsManager->getComputeHlms(), job, renderSystem );
    }
    void PhotonVoxelScroll::clearBox( HlmsManager *hlmsManager, RenderSystem *renderSystem, TextureGpu *dst,
                                      const uint32 origin[3], const uint32 size[3], const float clear[4] )
    {
        HlmsComputeJob *job = scrollJob( hlmsManager, 1, dst->getPixelFormat() );
        // (the source slot is the destination itself, read nowhere: the job's layout binds two UAVs - the scratch's
        // first dispatch binds a texture to both the same way)
        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.texture = dst;
        uavSlot.pixelFormat = dst->getPixelFormat();
        uavSlot.access = ResourceAccess::Read;
        job->_setUavTexture( 0u, uavSlot );
        uavSlot.access = ResourceAccess::Write;
        job->_setUavTexture( 1u, uavSlot );
        const uint32 *tpg = job->getThreadsPerGroup();
        job->setNumThreadGroups( std::max( 1u, ( size[0] + tpg[0] - 1u ) / tpg[0] ),
                                 std::max( 1u, ( size[1] + tpg[1] - 1u ) / tpg[1] ),
                                 std::max( 1u, ( size[2] + tpg[2] - 1u ) / tpg[2] ) );
        const uint32 zero[3] = { 0u, 0u, 0u };
        setCopyParams( job, zero, origin, size, 0u, clear );
        jahshaka::engine::detail::photonDispatch( hlmsManager->getComputeHlms(), job, renderSystem );
    }
    void PhotonVoxelScroll::clearRegion( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                         const PhotonVoxelizer::VolumeSet &live, const uint32 origin[3],
                                         const uint32 size[3] )
    {
        renderSystem->endRenderPassDescriptor();
        TextureGpu *to[kVolumes];
        volumeList( live, to );
        for( size_t i = 0u; i < kVolumes; ++i )
            if( to[i] )
                clearBox( hlmsManager, renderSystem, to[i], origin, size, clearOf( i ) );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::begin()
    {
        OgreProfileGpuBegin( "Photon scroll begin" );
        mRenderSystem->endRenderPassDescriptor();
        // CLEARED AS A WHOLE BUILD CLEARS (PhotonVoxelizer::clearVoxels): every value zero but the
        // halves' normals' 0.5 (clearOf). The entering slab's cells keep these until its chunks write them.
        TextureGpu *dst[kVolumes];
        volumeList( mVolumes, dst );
        mResourceTransitions.clear();
        for( size_t i = 0u; i < kVolumes; ++i )
            mComputeTools->prepareForUavClear( mResourceTransitions, dst[i] );
        mRenderSystem->executeResourceTransition( mResourceTransitions );
        const uint32 uZero[4] = { 0u, 0u, 0u, 0u };
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            if( PixelFormatGpuUtils::isInteger( dst[i]->getPixelFormat() ) )
                jahshaka::engine::detail::photonClearUavUint( mComputeTools, dst[i], uZero );
            else
                jahshaka::engine::detail::photonClearUavFloat( mComputeTools, dst[i], clearOf( i ) );
        }
        OgreProfileGpuEnd( "Photon scroll begin" );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::commit( PhotonVoxelizer *live, const LightVolume *light, size_t numLight,
                                    const int32 shift[3], const Slab *slabs, size_t numSlabs, bool fault )
    {
        OgreProfileGpuBegin( "Photon scroll commit" );
        mRenderSystem->endRenderPassDescriptor();
        TextureGpu *to[kVolumes];
        volumeList( live->liveVolumes(), to );
        // THE SHIFT WAITS FOR WHAT LAST USED THE STORE (PHOTON-7 stage 2's read D2; stage 3's read F5): the live volumes
        // reach the shift "first seen" in the frame (the solver forgets every use at the frame's reset), and the fork's
        // Vulkan backend gives a first-seen texture whose content is kept every earlier command and memory write as its
        // source (OgreVulkanRenderSystem::executeResourceTransition) - so a one-frame commit, whose slab chunks were
        // voxelised earlier in the same command buffer, shifts a store every earlier write is visible to
        // (gi.scroll_slab H). The site's own assumeTransition of each volume's prior use is deleted with it.
        // THE SHIFT IN PLACE, axis by axis (separable: after the three the store at new cell n is the old n + shift),
        // the geometry's eleven volumes, then the light's.
        size_t shifted = 0u;
        const size_t half = ( kVolumes + numLight ) / 2u;
        for( uint32 a = 0u; a < 3u; ++a )
        {
            if( !shift[a] )
                continue;
            OGRE_ASSERT_LOW( shift[a] > -int32( mResolution ) && shift[a] < int32( mResolution ) );
            for( size_t i = 0u; i < kVolumes + numLight; ++i )
            {
                if( i < kVolumes )
                {
                    shiftVolume( mHlmsManager, mRenderSystem, to[i], a, shift[a], 1u, clearOf( i ) );
                }
                else
                {
                    const LightVolume &l = light[i - kVolumes];
                    OGRE_ASSERT_LOW( shift[a] % int32( l.cellsPerTexel ) == 0 );
                    shiftVolume( mHlmsManager, mRenderSystem, l.texture, a, shift[a] / int32( l.cellsPerTexel ),
                                 l.blocks, kZero );
                }
                if( fault && ++shifted == half )
                {
                    OgreProfileGpuEnd( "Photon scroll commit" );
                    OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR,
                                 "gi.cascadeFaultCommit: forced failure half way through a scroll's shift",
                                 "PhotonVoxelScroll::commit" );
                }
            }
        }
        // THE SLABS, from the scratch into the far ends the shift cleared: each from its first layer, in its layout.
        TextureGpu *from[kVolumes];
        volumeList( mVolumes, from );
        for( size_t k = 0u; k < numSlabs; ++k )
        {
            const Slab &s = slabs[k];
            const uint32 size[3] = { s.hi[0] - s.lo[0], s.hi[1] - s.lo[1], s.hi[2] - s.lo[2] };
            const uint32 srcOrigin[3] = { s.layer, 0u, 0u };
            OGRE_ASSERT_LOW( s.layer + size[s.axis] <= mLayers );
            for( size_t i = 0u; i < kVolumes; ++i )
                copyBox( mHlmsManager, mRenderSystem, from[i], to[i], srcOrigin, s.lo, size, s.axis );
        }
        OgreProfileGpuEnd( "Photon scroll commit" );
    }
}  // namespace Ogre
