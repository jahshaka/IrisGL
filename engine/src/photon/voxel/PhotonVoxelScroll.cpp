// THE CASCADE SCROLL'S TARGET (photon/voxel/PhotonVoxelScroll.h has the design).
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

namespace Ogre
{
    namespace
    {
        uint32 gScrollCloneId = 0u;

        /// The live store's volumes, in one fixed order: the copy and the clear walk it.
        constexpr size_t kVolumes = 12u;
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
            out[8] = v.positionSums;
            out[9] = v.metal;   // METAL-BOUNCE-1
            out[10] = v.emit[0];   // JUNCTION-NORMAL-1
            out[11] = v.emit[1];
        }
        const char *const kVolumeNames[kVolumes] = { "Albedo",    "Emissive",  "Normal",
                                                      "AccumVal",  "CoverageP", "CoverageN",
                                                      "PositionP", "PositionN", "PositionSums",
                                                      "Metal",     "EmitP",     "EmitN" };
    }  // namespace

    PhotonVoxelScroll::PhotonVoxelScroll( RenderSystem *renderSystem, HlmsManager *hlmsManager,
                                          TextureGpuManager *textureManager, PhotonVoxelizer *like,
                                          uint32 resolution ) :
        mRenderSystem( renderSystem ),
        mHlmsManager( hlmsManager ),
        mTextureManager( textureManager ),
        mComputeTools( new ComputeTools( hlmsManager->getComputeHlms() ) ),
        mResolution( resolution ),
        mPositionJob( 0 )
    {
        const PhotonVoxelizer::VolumeSet live = like->liveVolumes();
        TextureGpu *src[kVolumes];
        volumeList( live, src );
        TextureGpu *dst[kVolumes] = {};
        const uint32 id = ++gScrollCloneId;
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            OGRE_ASSERT_LOW( src[i] && src[i]->getWidth() == resolution );
            // LEVEL 0 ONLY: the target is copied into the live volumes, whose chains are then
            // regenerated (PhotonVoxelizer::finishScroll). A UAV and nothing more, like the live ones.
            dst[i] = textureManager->createTexture(
                "PhotonScrollVox/" + StringConverter::toString( id ) + "/" + kVolumeNames[i],
                GpuPageOutStrategy::Discard, TextureFlags::NotTexture | TextureFlags::Uav,
                TextureTypes::Type3D );
            dst[i]->setPixelFormat( src[i]->getPixelFormat() );
            dst[i]->setResolution( src[i]->getWidth(), src[i]->getHeight(), src[i]->getDepth() );
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
        mVolumes.positionSums = dst[8];
        mVolumes.metal = dst[9];
        mVolumes.emit[0] = dst[10];
        mVolumes.emit[1] = dst[11];

        HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
        HlmsComputeJob *base = hlmsCompute->findComputeJob( "Photon/ScrollPosition" );
        mPositionJob = base->clone( "Photon/ScrollPosition/" + StringConverter::toString( id ) );

        // THE SCROLL'S JOBS ARE BUILT HERE, AT THE CHAIN'S BUILD (vr.warmup: no shader is compiled on a frame a
        // headset is shown, and a scroll is what the first head movement does): one texel of every volume copied
        // and one re-resolved - every permutation a scroll and its commit dispatch (a copy per format at this
        // size, the position re-resolve), into this target, which every scroll clears before it fills it.
        mRenderSystem->endRenderPassDescriptor();
        const uint32 zero[3] = { 0u, 0u, 0u }, one[3] = { 1u, 1u, 1u };
        for( size_t i = 0u; i < kVolumes; ++i )
            copyBox( src[i], dst[i], zero, zero, one );
        copyPositions( live, mVolumes, zero, zero, one );
    }
    //-------------------------------------------------------------------------
    PhotonVoxelScroll::~PhotonVoxelScroll()
    {
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();
        for( auto &kv : mCopyJobs )
            hlmsCompute->destroyComputeJob( kv.second->getName() );
        mCopyJobs.clear();
        if( mPositionJob )
            hlmsCompute->destroyComputeJob( mPositionJob->getName() );
        mPositionJob = 0;
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
    HlmsComputeJob *PhotonVoxelScroll::copyJob( PixelFormatGpu format )
    {
        auto it = mCopyJobs.find( format );
        if( it != mCopyJobs.end() )
            return it->second;
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();
        HlmsComputeJob *base = hlmsCompute->findComputeJob( "Photon/ScrollCopy" );
        char tmpBuffer[96];
        LwString name( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
        name.a( "Photon/ScrollCopy/", ++gScrollCloneId );
        HlmsComputeJob *job = base->clone( name.c_str() );
        job->setProperty( "copy_uint", PixelFormatGpuUtils::isInteger( format ) ? 1 : 0 );
        mCopyJobs[format] = job;
        return job;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::copyBox( TextureGpu *src, TextureGpu *dst, const uint32 srcOrigin[3],
                                     const uint32 dstOrigin[3], const uint32 size[3] )
    {
        if( !size[0] || !size[1] || !size[2] )
            return;
        HlmsComputeJob *job = copyJob( dst->getPixelFormat() );
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
        job->setNumThreadGroups( ( size[0] + tpg[0] - 1u ) / tpg[0], ( size[1] + tpg[1] - 1u ) / tpg[1],
                                 ( size[2] + tpg[2] - 1u ) / tpg[2] );
        ShaderParams::Param pSrc, pDst, pSize;
        pSrc.name = "srcOrigin";
        pDst.name = "dstOrigin";
        pSize.name = "copySize";
        pSrc.setManualValue( srcOrigin, 3u );
        pDst.setManualValue( dstOrigin, 3u );
        pSize.setManualValue( size, 3u );
        ShaderParams &params = job->getShaderParams( "default" );
        params.mParams.clear();
        params.mParams.push_back( pSrc );
        params.mParams.push_back( pDst );
        params.mParams.push_back( pSize );
        params.setDirty();
        jahshaka::engine::detail::photonDispatch( mHlmsManager->getComputeHlms(), job, mRenderSystem );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::copyPositions( const PhotonVoxelizer::VolumeSet &src,
                                           const PhotonVoxelizer::VolumeSet &dst,
                                           const uint32 srcOrigin[3], const uint32 dstOrigin[3],
                                           const uint32 size[3] )
    {
        if( !size[0] || !size[1] || !size[2] )
            return;
        HlmsComputeJob *job = mPositionJob;
        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        TextureGpu *bind[5] = { src.positionSums, src.position[0], src.position[1], dst.position[0],
                                dst.position[1] };
        for( uint8 i = 0u; i < 5u; ++i )
        {
            uavSlot.texture = bind[i];
            uavSlot.pixelFormat = bind[i]->getPixelFormat();
            uavSlot.access = i < 3u ? ResourceAccess::Read : ResourceAccess::Write;
            job->_setUavTexture( i, uavSlot );
        }
        const uint32 *tpg = job->getThreadsPerGroup();
        job->setNumThreadGroups( ( size[0] + tpg[0] - 1u ) / tpg[0], ( size[1] + tpg[1] - 1u ) / tpg[1],
                                 ( size[2] + tpg[2] - 1u ) / tpg[2] );
        ShaderParams::Param pSrc, pDst, pSize;
        pSrc.name = "srcOrigin";
        pDst.name = "dstOrigin";
        pSize.name = "copySize";
        pSrc.setManualValue( srcOrigin, 3u );
        pDst.setManualValue( dstOrigin, 3u );
        pSize.setManualValue( size, 3u );
        ShaderParams &params = job->getShaderParams( "default" );
        params.mParams.clear();
        params.mParams.push_back( pSrc );
        params.mParams.push_back( pDst );
        params.mParams.push_back( pSize );
        params.setDirty();
        jahshaka::engine::detail::photonDispatch( mHlmsManager->getComputeHlms(), job, mRenderSystem );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::begin( PhotonVoxelizer *live, const int32 shift[3] )
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
        const float fZero[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        const float fNormal[4] = { 0.5f, 0.5f, 0.5f, 0.0f };
        const uint32 uZero[4] = { 0u, 0u, 0u, 0u };
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            if( PixelFormatGpuUtils::isInteger( dst[i]->getPixelFormat() ) )
                jahshaka::engine::detail::photonClearUavUint( mComputeTools, dst[i], uZero );
            else
                jahshaka::engine::detail::photonClearUavFloat( mComputeTools, dst[i],
                                                               i == 2u ? fNormal : fZero );
        }

        // THE OVERLAP: new cell n holds old cell n + shift, for every n with both inside the box.
        uint32 srcOrigin[3], dstOrigin[3], size[3];
        for( int a = 0; a < 3; ++a )
        {
            const int32 s = shift[a];
            OGRE_ASSERT_LOW( s > -int32( mResolution ) && s < int32( mResolution ) );
            srcOrigin[a] = s > 0 ? uint32( s ) : 0u;
            dstOrigin[a] = s < 0 ? uint32( -s ) : 0u;
            size[a] = mResolution - uint32( s < 0 ? -s : s );
        }
        const PhotonVoxelizer::VolumeSet src = live->liveVolumes();
        TextureGpu *from[kVolumes];
        volumeList( src, from );
        // Every volume but the positions (6, 7) and the sums (8), texel for texel - the metal (9) and the
        // emissive factors (10, 11) with them.
        for( size_t i = 0u; i < kVolumes; ++i )
            if( i < 6u || i > 8u )
                copyBox( from[i], dst[i], srcOrigin, dstOrigin, size );
        // THE SUMS are four texels a voxel interleaved in Z (Voxelizer_cs.glsl): the same box with
        // its z scaled by four is one contiguous box of texels.
        {
            const uint32 s4[3] = { srcOrigin[0], srcOrigin[1], srcOrigin[2] * 4u };
            const uint32 d4[3] = { dstOrigin[0], dstOrigin[1], dstOrigin[2] * 4u };
            const uint32 z4[3] = { size[0], size[1], size[2] * 4u };
            copyBox( from[8], dst[8], s4, d4, z4 );
        }
        // THE POSITIONS, re-resolved at the new cell from the old one's sums.
        copyPositions( src, mVolumes, srcOrigin, dstOrigin, size );
        OgreProfileGpuEnd( "Photon scroll begin" );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelScroll::commit( PhotonVoxelizer *live, bool fault )
    {
        OgreProfileGpuBegin( "Photon scroll commit" );
        mRenderSystem->endRenderPassDescriptor();
        TextureGpu *from[kVolumes], *to[kVolumes];
        volumeList( mVolumes, from );
        volumeList( live->liveVolumes(), to );
        const uint32 zero[3] = { 0u, 0u, 0u };
        for( size_t i = 0u; i < kVolumes; ++i )
        {
            // WHOLE level 0, texel for texel - the positions too: they were resolved for THIS box.
            const uint32 size[3] = { from[i]->getWidth(), from[i]->getHeight(), from[i]->getDepth() };
            copyBox( from[i], to[i], zero, zero, size );
            if( fault && i == 4u )
            {
                OgreProfileGpuEnd( "Photon scroll commit" );
                OGRE_EXCEPT( Exception::ERR_INTERNAL_ERROR,
                             "gi.cascadeFaultCommit: forced failure half way through a scroll's commit",
                             "PhotonVoxelScroll::commit" );
            }
        }
        OgreProfileGpuEnd( "Photon scroll commit" );
    }
}  // namespace Ogre
