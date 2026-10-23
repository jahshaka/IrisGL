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
        constexpr size_t kVolumes = 11u;
        void volumeList( const PhotonVoxelizer::VolumeSet &v, TextureGpu *out[kVolumes] )
        {
            out[0] = v.albedo;
            out[1] = v.emissive;
            out[2] = v.normal;
            out[3] = v.accumVal;
            out[4] = v.coverage[0];
            out[5] = v.coverage[1];
            out[6] = v.position[0];
            out[7] = v.position[1];
            out[8] = v.metal;     // METAL-BOUNCE-1
            out[9] = v.emit[0];   // JUNCTION-NORMAL-1
            out[10] = v.emit[1];
        }
        const char *const kVolumeNames[kVolumes] = { "Albedo",    "Emissive",  "Normal",
                                                      "AccumVal",  "CoverageP", "CoverageN",
                                                      "PositionP", "PositionN", "Metal",
                                                      "EmitP",     "EmitN" };
        /// What a whole build clears each volume to (PhotonVoxelizer::clearVoxels): zero, the normal's 0.5 bias.
        const float kZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float kNormalClear[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
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
        mVolumes.normal = dst[2];
        mVolumes.accumVal = dst[3];
        mVolumes.coverage[0] = dst[4];
        mVolumes.coverage[1] = dst[5];
        mVolumes.position[0] = dst[6];
        mVolumes.position[1] = dst[7];
        mVolumes.metal = dst[8];
        mVolumes.emit[0] = dst[9];
        mVolumes.emit[1] = dst[10];

        // THE SCROLL'S JOBS ARE BUILT HERE, AT THE CHAIN'S BUILD (vr.warmup: no shader is compiled on a frame a headset
        // is shown, and a scroll is what the first head movement does): every format's shift and copy dispatched once
        // with nothing to do - a shift of 0 texels and a copy of an empty box, on this scratch's own volumes.
        mRenderSystem->endRenderPassDescriptor();
        const uint32 zero[3] = { 0u, 0u, 0u };
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            shiftVolume( mHlmsManager, mRenderSystem, dst[i], 0u, 0, false, kZero );
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
                                         uint32 axis, int32 texels, bool packedHalves, const float clear[4] )
    {
        HlmsComputeJob *job = scrollJob( hlmsManager, 0, volume->getPixelFormat() );
        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.texture = volume;
        uavSlot.pixelFormat = volume->getPixelFormat();
        uavSlot.access = ResourceAccess::ReadWrite;
        job->_setUavTexture( 0u, uavSlot );
        const uint32 dims[3] = { volume->getWidth(), volume->getHeight(), volume->getDepth() };
        const bool halves = packedHalves && axis == 0u;
        // THE COLUMNS: the two other axes in increasing order, and the halves of a packed volume along x.
        const uint32 b = axis == 0u ? 1u : 0u, c = axis == 2u ? 1u : 2u;
        const uint32 len = halves ? dims[0] / 2u : dims[axis];
        const uint32 shiftAxis[4] = { axis, uint32( texels ), len, halves ? dims[0] / 2u : 0u };
        const uint32 columns[4] = { dims[b], dims[c], halves ? 2u : 1u, 0u };
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
        ShaderParams::Param pSrc, pDst, pSize, pPerm;
        pSrc.name = "srcOrigin";
        pDst.name = "dstOrigin";
        pSize.name = "copySize";
        pPerm.name = "permAxis";
        pSrc.setManualValue( srcOrigin, 3u );
        pDst.setManualValue( dstOrigin, 3u );
        pSize.setManualValue( size, 3u );
        pPerm.setManualValue( permAxis );
        ShaderParams &params = job->getShaderParams( "default" );
        params.mParams.clear();
        params.mParams.push_back( pSrc );
        params.mParams.push_back( pDst );
        params.mParams.push_back( pSize );
        params.mParams.push_back( pPerm );
        params.setDirty();
        jahshaka::engine::detail::photonDispatch( hlmsManager->getComputeHlms(), job, renderSystem );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::begin()
    {
        OgreProfileGpuBegin( "Photon scroll begin" );
        mRenderSystem->endRenderPassDescriptor();
        // CLEARED AS A WHOLE BUILD CLEARS (PhotonVoxelizer::clearVoxels): every value zero but the
        // normal's 0.5 bias. The entering slab's cells keep these until its chunks write them.
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
                jahshaka::engine::detail::photonClearUavFloat( mComputeTools, dst[i],
                                                               i == 2u ? kNormalClear : kZero );
        }
        OgreProfileGpuEnd( "Photon scroll begin" );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::commit( PhotonVoxelizer *live, const LightVolume *light, size_t numLight,
                                    const int32 shift[3], const Slab *slabs, size_t numSlabs, bool fault )
    {
        OgreProfileGpuBegin( "Photon scroll commit" );
        mRenderSystem->endRenderPassDescriptor();
        // THE SLAB'S BUILD IS SUBMITTED BEFORE THE STORE MOVES (measured, PHOTON-7 stage 2): with the slab's last chunks
        // voxelised earlier in the SAME command buffer, the copy-in below read the scratch half-built and its writes
        // reached the shift's reads of the slab's texels - the entering slab and the |shift| cells beside it read empty
        // in every one-frame commit of a 16-cell shift (gi.scroll_slab), although the barrier solver held the scratch's
        // and the store's records (compute stage, written) and Vulkan synchronization validation reported no hazard on
        // these dispatches. A submission here (after the chunks, before the shifts) makes every arm exact; one after the
        // scratch's clear does not. The mechanism inside the pin's compute recording is UNRESOLVED (the report names it);
        // a commit is one submission per scroll.
        mRenderSystem->flushCommands();
        TextureGpu *to[kVolumes];
        volumeList( live->liveVolumes(), to );
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
                    shiftVolume( mHlmsManager, mRenderSystem, to[i], a, shift[a], false,
                                 i == 2u ? kNormalClear : kZero );
                }
                else
                {
                    const LightVolume &l = light[i - kVolumes];
                    OGRE_ASSERT_LOW( shift[a] % int32( l.cellsPerTexel ) == 0 );
                    shiftVolume( mHlmsManager, mRenderSystem, l.texture, a, shift[a] / int32( l.cellsPerTexel ),
                                 l.packedHalves, kZero );
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
