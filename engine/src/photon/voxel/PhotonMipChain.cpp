// THE PHOTON MIP CHAIN (photon/voxel/PhotonMipChain.h has the design).
#include "photon/voxel/PhotonMipChain.h"

#include "photon/PhotonDispatch.h"

#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreHlmsSamplerblock.h"
#include "OgreLwString.h"
#include "OgreRenderSystem.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpu.h"

namespace Ogre
{
    namespace
    {
        unsigned long long gMipDispatches = 0ull;
        uint32 gMipCloneId = 0u;
    }  // namespace

    unsigned long long PhotonMipChain::numDispatches() { return gMipDispatches; }

    PhotonMipChain::~PhotonMipChain() { destroy(); }

    void PhotonMipChain::destroy()
    {
        if( !mHlmsManager )
            return;
        HlmsCompute *hlmsCompute = mHlmsManager->getComputeHlms();
        for( HlmsComputeJob *job : mJobs )
            hlmsCompute->destroyComputeJob( job->getName() );
        mJobs.clear();
        mHlmsManager = nullptr;
    }

    void PhotonMipChain::generate( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                   TextureGpu *texture )
    {
        const uint8 numMips = texture->getNumMipmaps();
        if( numMips <= 1u )
            return;
        if( mHlmsManager && mHlmsManager != hlmsManager )
            destroy();
        HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
        mHlmsManager = hlmsManager;
        if( mJobs.size() < size_t( numMips - 1u ) )
        {
            // THE BLIT'S SAMPLING: linear, clamped to the edge, read at the view's only level.
            // Each clone holds its own reference (setSamplerblock), released with the clone.
            HlmsSamplerblock samplerblock;
            samplerblock.mMinFilter = FO_LINEAR;
            samplerblock.mMagFilter = FO_LINEAR;
            samplerblock.mMipFilter = FO_POINT;
            samplerblock.setAddressingMode( TAM_CLAMP );
            HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( "Photon/MipDown" );
            char tmpBuffer[96];
            LwString name( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            while( mJobs.size() < size_t( numMips - 1u ) )
            {
                name.clear();
                name.a( "Photon/MipDown/", ++gMipCloneId );
                HlmsComputeJob *job = baseJob->clone( name.c_str() );
                job->setSamplerblock( 0, samplerblock );
                mJobs.push_back( job );
            }
        }

        renderSystem->debugAnnotationPush( "Photon mips" );
        for( uint8 level = 1u; level < numMips; ++level )
        {
            HlmsComputeJob *job = mJobs[level - 1u];

            DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
            texSlot.texture = texture;
            texSlot.mipmapLevel = uint8( level - 1u );
            texSlot.numMipmaps = 1u;
            job->setTexture( 0, texSlot );

            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            uavSlot.texture = texture;
            uavSlot.mipmapLevel = level;
            uavSlot.access = ResourceAccess::Write;
            job->_setUavTexture( 0, uavSlot );

            ++gMipDispatches;
            jahshaka::engine::detail::photonDispatch( hlmsCompute, job, renderSystem );
        }
        renderSystem->debugAnnotationPop();
    }
}  // namespace Ogre
