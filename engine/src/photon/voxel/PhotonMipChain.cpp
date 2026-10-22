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

#include <map>

namespace Ogre
{
    namespace
    {
        unsigned long long gMipDispatches = 0ull;
        unsigned long long gMipJobs = 0ull;
        uint32 gMipCloneId = 0u;

        /// THE POOL: one job per (format, volume size, level), shared by every volume of that kind -
        /// every cascade, every rebuild - so a pipeline is built once per kind, never per volume
        /// (a clone per volume and level built ~220 pipelines per arm build). Owned by the compute
        /// Hlms it was cloned from; released by releasePool before that Hlms dies.
        struct Key
        {
            PixelFormatGpu format = PFG_UNKNOWN;
            uint32 width = 0u, height = 0u, depth = 0u;
            uint8 level = 0u;
            bool cellRelative = false;   ///< level 1 of a cell-relative position volume (generate's note)
            bool operator<( const Key &o ) const
            {
                if( cellRelative != o.cellRelative ) return cellRelative < o.cellRelative;
                if( format != o.format ) return format < o.format;
                if( width != o.width ) return width < o.width;
                if( height != o.height ) return height < o.height;
                if( depth != o.depth ) return depth < o.depth;
                return level < o.level;
            }
        };
        HlmsManager *gPoolManager = nullptr;
        std::map<Key, HlmsComputeJob *> gPool;

        HlmsComputeJob *pooledJob( HlmsManager *hlmsManager, const TextureGpu *texture, uint8 level,
                                   bool cellRelative )
        {
            if( gPoolManager && gPoolManager != hlmsManager )
                PhotonMipChain::releasePool();
            gPoolManager = hlmsManager;
            const Key key = { texture->getPixelFormat(), texture->getWidth(), texture->getHeight(),
                              texture->getDepth(), level, cellRelative };
            auto it = gPool.find( key );
            if( it != gPool.end() )
                return it->second;
            HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
            HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( "Photon/MipDown" );
            char tmpBuffer[96];
            LwString name( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            name.a( "Photon/MipDown/", ++gMipCloneId );
            HlmsComputeJob *job = baseJob->clone( name.c_str() );
            // THE BLIT'S SAMPLING: linear, clamped to the edge, read at the view's only level.
            HlmsSamplerblock samplerblock;
            samplerblock.mMinFilter = FO_LINEAR;
            samplerblock.mMagFilter = FO_LINEAR;
            samplerblock.mMipFilter = FO_POINT;
            samplerblock.setAddressingMode( TAM_CLAMP );
            job->setSamplerblock( 0, samplerblock );
            if( cellRelative )
            {
                // THE CELL-RELATIVE LEVEL 0 (PhotonMipDown_cs.glsl's jah_cell_relative): the children are
                // fetched, not filtered, beside their coverage at slot 1.
                job->setNumTexUnits( 2u );
                job->setProperty( "jah_cell_relative", 1 );
            }
            gPool[key] = job;
            ++gMipJobs;
            return job;
        }
    }  // namespace

    unsigned long long PhotonMipChain::numDispatches() { return gMipDispatches; }
    unsigned long long PhotonMipChain::numJobs() { return gMipJobs; }

    void PhotonMipChain::releasePool()
    {
        if( gPoolManager )
        {
            HlmsCompute *hlmsCompute = gPoolManager->getComputeHlms();
            for( auto &kv : gPool )
                hlmsCompute->destroyComputeJob( kv.second->getName() );
        }
        gPool.clear();
        gPoolManager = nullptr;
    }

    void PhotonMipChain::generate( HlmsManager *hlmsManager, RenderSystem *renderSystem,
                                   TextureGpu *texture, TextureGpu *cellRelativeCoverage )
    {
        if( !texture || texture->getNumMipmaps() <= 1u )
            return;
        const uint8 numMips = texture->getNumMipmaps();
        HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
        renderSystem->debugAnnotationPush( "Photon mips" );
        for( uint8 level = 1u; level < numMips; ++level )
        {
            const bool cellRelative = cellRelativeCoverage && level == 1u;
            HlmsComputeJob *job = pooledJob( hlmsManager, texture, level, cellRelative );

            DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
            texSlot.texture = texture;
            texSlot.mipmapLevel = uint8( level - 1u );
            texSlot.numMipmaps = 1u;
            job->setTexture( 0, texSlot );
            if( cellRelative )
            {
                texSlot.texture = cellRelativeCoverage;   // its level 0, the O each child is premultiplied by
                job->setTexture( 1, texSlot );
            }

            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            uavSlot.texture = texture;
            uavSlot.mipmapLevel = level;
            // READ-WRITE, not Write (the stage-1 read's F1): level L - 1 of this SAME texture is sampled
            // through the texture slot, which the job marks generalReadWrite (it is also our UAV), so
            // the solver's only transition is this slot's - and the barrier before the next level's
            // dispatch must make this level's write visible to its SHADER READ, not only to a write.
            uavSlot.access = ResourceAccess::ReadWrite;
            job->_setUavTexture( 0, uavSlot );

            ++gMipDispatches;
            jahshaka::engine::detail::photonDispatch( hlmsCompute, job, renderSystem );
        }
        renderSystem->debugAnnotationPop();
    }
}  // namespace Ogre
