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

#include "OgreStableHeaders.h"

#include "photon/voxel/PhotonVoxelMaterial.h"

#include "Compositor/OgreCompositorManager2.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "OgreDepthBuffer.h"
#include "OgreHlms.h"
#include "OgreHlmsPbsDatablock.h"
#include "OgreLogManager.h"
#include "OgreMaterialManager.h"
#include "OgrePass.h"
#include "OgreSceneManager.h"
#include "OgreTechnique.h"
#include "OgreTextureBox.h"
#include "OgreTextureGpuManager.h"
#include "OgreTextureUnitState.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreVaoManager.h"

namespace Ogre
{
    static const size_t c_numDatablocksPerConstBuffer = 1024u;

    struct PhotonShaderVoxelMaterial
    {
        float bgDiffuse[4];
        float diffuse[4];
        float emissive[4];
        uint32 diffuseTexIdx;
        uint32 emissiveTexIdx;
        // Jahshaka (PHOTON-WRITER-1): the datablock's perceptual roughness, so the
        // voxel's stored radiance can be what its surface renders (the diffuse
        // lobe's directional albedo depends on it; LightInjection reads it).
        float roughness;
        uint32 padding0;
    };
    //-------------------------------------------------------------------------
    PhotonVoxelMaterial::PhotonVoxelMaterial( IdType id, VaoManager *vaoManager, CompositorManager2 *compositorManager,
                              TextureGpuManager *textureGpuManager ) :
        IdObject( id ),
        mVaoManager( vaoManager ),
        mNumUsedPoolSlices( 0u ),
        mTexturePool( 0 ),
        mTextureGpuManager( textureGpuManager ),
        mCompositorManager( compositorManager ),
        mDownsampleTex( 0 ),
        mDownsampleMatPass2DArray( 0 ),
        mDownsampleMatPass2D( 0 ),
        mDownsampleWorkspace2DArray( 0 ),
        mDownsampleWorkspace2D( 0 )
    {
        MaterialPtr mat;
        mat = std::static_pointer_cast<Material>( MaterialManager::getSingleton().load(
            "Ogre/Copy/4xFP32_2DArray", ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME ) );
        mDownsampleMatPass2DArray = mat->getTechnique( 0 )->getPass( 0 );

        mat = std::static_pointer_cast<Material>( MaterialManager::getSingleton().load(
            "Ogre/Copy/4xFP32", ResourceGroupManager::DEFAULT_RESOURCE_GROUP_NAME ) );
        mDownsampleMatPass2D = mat->getTechnique( 0 )->getPass( 0 );
    }
    //-------------------------------------------------------------------------
    PhotonVoxelMaterial::~PhotonVoxelMaterial()
    {
        BucketVec::const_iterator itor = mBuckets.begin();
        BucketVec::const_iterator end = mBuckets.end();

        while( itor != end )
        {
            mVaoManager->destroyConstBuffer( itor->buffer );
            ++itor;
        }

        mBuckets.clear();
        mDatablockConversionResults.clear();

        if( mTexturePool )
        {
            mTextureGpuManager->destroyTexture( mTexturePool );
            mTexturePool = 0;
        }
    }
    //-------------------------------------------------------------------------
    /// ONE ROW, WRITTEN AT A SLOT THE CALLER CHOSE. The conversion itself - the
    /// datablock's colours, its background diffuse, its transparency and the texture
    /// pool slices of its albedo and emissive maps - is the same whether the slot is
    /// new or the datablock already owns it, so both the first conversion and an
    /// in-place refresh go through here and cannot drift apart.
    void PhotonVoxelMaterial::writeRow( HlmsDatablock *datablock, MaterialBucket &bucket, uint32 slot,
                                DatablockConversionResult &out )
    {
        OGRE_ASSERT_MEDIUM( slot < c_numDatablocksPerConstBuffer );

        PhotonShaderVoxelMaterial shaderMaterial;
        memset( &shaderMaterial, 0, sizeof( shaderMaterial ) );

        {
            ColourValue diffuseCol = datablock->getDiffuseColour();
            ColourValue emissiveCol = datablock->getEmissiveColour();
            for( size_t i = 0; i < 4; ++i )
            {
                shaderMaterial.bgDiffuse[i] = 1.0f;
                shaderMaterial.diffuse[i] = diffuseCol[i];
                shaderMaterial.emissive[i] = emissiveCol[i];
            }

            shaderMaterial.diffuse[3] = std::max( diffuseCol.a, emissiveCol.a );
        }

        if( datablock->getCreator()->getType() == HLMS_PBS )
        {
            OGRE_ASSERT_HIGH( dynamic_cast<HlmsPbsDatablock *>( datablock ) );
            HlmsPbsDatablock *pbsDatablock = static_cast<HlmsPbsDatablock *>( datablock );

            ColourValue bgDiffuse = pbsDatablock->getBackgroundDiffuse();
            float transparency = pbsDatablock->getTransparency();

            for( size_t i = 0; i < 4; ++i )
                shaderMaterial.bgDiffuse[i] = bgDiffuse[i];
            shaderMaterial.diffuse[3] = transparency;
            shaderMaterial.emissive[3] = 1.0f;
            // The scalar only: a roughness MAP is not voxelised (the voxel is one
            // surface-averaged texel either way).
            shaderMaterial.roughness = pbsDatablock->getRoughness();
        }
        else
        {
            // Not a PBS datablock (no diffuse lobe of HlmsPbs's to match): the
            // matte end.
            shaderMaterial.roughness = 1.0f;
        }

        TextureGpu *diffuseTex = datablock->getDiffuseTexture();
        TextureGpu *emissiveTex = datablock->getEmissiveTexture();

        out = DatablockConversionResult();
        out.slotIdx = slot;
        out.bucketIdx = static_cast<uint32>( &bucket - &mBuckets.front() );
        out.constBuffer = bucket.buffer;
        if( diffuseTex )
        {
            out.diffuseTexIdx = getPoolSliceIdxForTexture( diffuseTex );
            shaderMaterial.diffuseTexIdx = out.diffuseTexIdx;
        }
        if( emissiveTex )
        {
            out.emissiveTexIdx = getPoolSliceIdxForTexture( emissiveTex );
            shaderMaterial.emissiveTexIdx = out.emissiveTexIdx;
        }

        bucket.buffer->upload( &shaderMaterial, slot * sizeof( PhotonShaderVoxelMaterial ),
                               sizeof( PhotonShaderVoxelMaterial ) );
    }
    //-------------------------------------------------------------------------
    PhotonVoxelMaterial::DatablockConversionResult PhotonVoxelMaterial::addDatablockToBucket( HlmsDatablock *datablock,
                                                                              MaterialBucket &bucket )
    {
        // SLOTS ARE NUMBERED BY THE MEMBERSHIP SET'S SIZE (0081 keeps dead and moved
        // pointers in it on purpose - see removeDatablock), so a new datablock takes
        // the next slot and no live one's slot is ever handed out twice.
        const uint32 slot = static_cast<uint32>( bucket.datablocks.size() );
        DatablockConversionResult conversionResult;
        writeRow( datablock, bucket, slot, conversionResult );
        bucket.datablocks.insert( datablock );
        mDatablockConversionResults[datablock] = conversionResult;
        return conversionResult;
    }
    //-------------------------------------------------------------------------
    const PhotonVoxelMaterial::DatablockConversionResult *PhotonVoxelMaterial::lookupDatablock(
        const HlmsDatablock *datablock ) const
    {
        DatablockConversionResultMap::const_iterator itor =
            mDatablockConversionResults.find( const_cast<HlmsDatablock *>( datablock ) );
        return itor != mDatablockConversionResults.end() ? &itor->second : 0;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelMaterial::refreshAll( FastArray<HlmsDatablock *> *moved )
    {
        // THE TEXTURE POOL IS RE-COPIED TOO. Its slices are cached by TextureGpu POINTER
        // (getPoolSliceIdxForTexture), so a store that outlives rebuilds would keep a
        // texture's first pixels for ever and hand a dead texture's slice to whatever
        // texture is later allocated at its address. Forgetting the cache here makes
        // every row re-written below copy its textures afresh into slices numbered from
        // zero; the pool keeps its size.
        mTextureToPoolEntry.clear();
        mNumUsedPoolSlices = 0u;

        // THE KEYS FIRST: a datablock that has to MOVE is erased from the map and
        // re-inserted while this walks, so the walk is over a copy.
        FastArray<HlmsDatablock *> datablocks;
        datablocks.reserve( mDatablockConversionResults.size() );
        DatablockConversionResultMap::const_iterator itor = mDatablockConversionResults.begin();
        DatablockConversionResultMap::const_iterator endt = mDatablockConversionResults.end();
        while( itor != endt )
        {
            datablocks.push_back( itor->first );
            ++itor;
        }

        FastArray<HlmsDatablock *>::const_iterator itDb = datablocks.begin();
        FastArray<HlmsDatablock *>::const_iterator enDb = datablocks.end();
        while( itDb != enDb )
        {
            bool bMoved = false;
            addDatablock( *itDb, &bMoved );
            if( bMoved && moved )
                moved->push_back( *itDb );
            ++itDb;
        }
    }
    //-------------------------------------------------------------------------
    uint16 PhotonVoxelMaterial::getPoolSliceIdxForTexture( TextureGpu *texture )
    {
        TextureToPoolEntryMap::const_iterator itor = mTextureToPoolEntry.find( texture );
        if( itor != mTextureToPoolEntry.end() )
            return itor->second;  // We already copied that texture. We're done

        if( !mTexturePool || mNumUsedPoolSlices >= mTexturePool->getNumSlices() )
            resizeTexturePool();

        texture->waitForData();

        if( texture->getInternalTextureType() == TextureTypes::Type2DArray )
        {
            GpuProgramParametersSharedPtr psParams =
                mDownsampleMatPass2DArray->getFragmentProgramParameters();
            psParams->setNamedConstant( "sliceIdx",
                                        static_cast<float>( texture->getInternalSliceStart() ) );
            mDownsampleMatPass2DArray->getTextureUnitState( 0 )->setTexture( texture );
            mDownsampleWorkspace2DArray->_update();
        }
        else
        {
            mDownsampleMatPass2D->getTextureUnitState( 0 )->setTexture( texture );
            mDownsampleWorkspace2D->_update();
        }

        const uint16 sliceIdx = mNumUsedPoolSlices++;

        TextureBox dstBox = mTexturePool->getEmptyBox( 0u );
        dstBox.sliceStart = sliceIdx;
        dstBox.numSlices = 1u;

        mDownsampleTex->copyTo( mTexturePool, dstBox, 0u, mDownsampleTex->getEmptyBox( 0u ), 0u, true );

        mTextureToPoolEntry[texture] = sliceIdx;
        return sliceIdx;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelMaterial::resizeTexturePool()
    {
        String texName = "VctMaterial" + StringConverter::toString( getId() ) + "/" +
                         StringConverter::toString( mNumUsedPoolSlices );
        TextureGpu *newPool =
            mTextureGpuManager->createTexture( texName, texName, GpuPageOutStrategy::Discard,
                                               TextureFlags::ManualTexture, TextureTypes::Type2DArray );
        newPool->setResolution( 64u, 64u, 64u );
        newPool->setPixelFormat( PFG_RGBA8_UNORM_SRGB );
        if( mTexturePool )
        {
            // We use quadratic growth because AMD GCN cards already round up to the next power of 2.
            newPool->setResolution( 64u, 64u, mTexturePool->getDepthOrSlices() << 1u );
        }
        newPool->scheduleTransitionTo( GpuResidency::Resident );

        if( mTexturePool )
        {
            TextureBox box( mTexturePool->getEmptyBox( 0u ) );
            mTexturePool->copyTo( newPool, box, 0u, box, 0u );
            mTextureGpuManager->destroyTexture( mTexturePool );
        }

        mTexturePool = newPool;
    }
    //-------------------------------------------------------------------------
    PhotonVoxelMaterial::MaterialBucket *PhotonVoxelMaterial::findFreeBucketFor( HlmsDatablock *datablock )
    {
        const bool needsDiffuse = datablock->getDiffuseTexture() != 0;
        const bool needsEmissive = datablock->getEmissiveTexture() != 0;

        BucketVec::iterator itor = mBuckets.begin();
        BucketVec::iterator end = mBuckets.end();

        while( itor != end &&
               ( itor->datablocks.size() >= c_numDatablocksPerConstBuffer ||
                 itor->hasDiffuse != needsDiffuse || itor->hasEmissive != needsEmissive ) )
        {
            ++itor;
        }

        MaterialBucket *retVal = 0;
        if( itor != end )
            retVal = &( *itor );

        return retVal;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelMaterial::initTempResources( SceneManager *sceneManager )
    {
        // IDEMPOTENT. The texture and the camera are created under FIXED NAMES, so a
        // second init without a destroy throws on the duplicate - which is how a
        // rebuild that threw between the bracket's two halves used to poison every
        // rebuild after it. Now the owner may call this freely and the failure path may
        // call destroyTempResources without knowing whether init ran.
        if( mDownsampleTex )
            return;

        mDownsampleTex = mTextureGpuManager->createTexture(
            "VctMaterialDownsampleTex", "VctMaterialDownsampleTex", GpuPageOutStrategy::Discard,
            TextureFlags::RenderToTexture | TextureFlags::DiscardableContent, TextureTypes::Type2D );
        mDownsampleTex->setResolution( 64u, 64u );
        mDownsampleTex->setPixelFormat( PFG_RGBA8_UNORM_SRGB );
        mDownsampleTex->_setDepthBufferDefaults( DepthBuffer::POOL_NO_DEPTH, false, PFG_UNKNOWN );
        mDownsampleTex->scheduleTransitionTo( GpuResidency::Resident );

        Camera *dummyCamera = sceneManager->createCamera( "VctMaterialCam" );

        mDownsampleWorkspace2DArray = mCompositorManager->addWorkspace(
            sceneManager, mDownsampleTex, dummyCamera, "VctTexDownsampleWorkspace", false, -1, 0, 0,
            Vector4::ZERO, 0x00, 0x01 );
        mDownsampleWorkspace2D = mCompositorManager->addWorkspace(
            sceneManager, mDownsampleTex, dummyCamera, "VctTexDownsampleWorkspace", false, -1, 0, 0,
            Vector4::ZERO, 0x00, 0x02 );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelMaterial::destroyTempResources()
    {
        if( !mDownsampleTex )
            return;  // never inited, or already torn down - see initTempResources

        mTextureGpuManager->destroyTexture( mDownsampleTex );
        mDownsampleTex = 0;

        Camera *dummyCamera = mDownsampleWorkspace2DArray->getDefaultCamera();
        SceneManager *sceneManager = mDownsampleWorkspace2DArray->getSceneManager();
        sceneManager->destroyCamera( dummyCamera );
        dummyCamera = 0;

        mCompositorManager->removeWorkspace( mDownsampleWorkspace2DArray );
        mDownsampleWorkspace2DArray = 0;

        mCompositorManager->removeWorkspace( mDownsampleWorkspace2D );
        mDownsampleWorkspace2D = 0;
    }
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    void PhotonVoxelMaterial::removeDatablock( const HlmsDatablock *datablock )
    {
        // JAHSHAKA fork ad452604a+0338ca7f2+c4c80b5f7 (was 0081) -- see the header. The map is keyed by a non-const
        // pointer; the lookup does not write through it.
        DatablockConversionResultMap::iterator it =
            mDatablockConversionResults.find( const_cast<HlmsDatablock *>( datablock ) );
        if( it != mDatablockConversionResults.end() )
            mDatablockConversionResults.erase( it );
    }
    //-------------------------------------------------------------------------
    PhotonVoxelMaterial::DatablockConversionResult PhotonVoxelMaterial::addDatablock( HlmsDatablock *datablock,
                                                                      bool *outMoved )
    {
        if( outMoved )
            *outMoved = false;

        DatablockConversionResultMap::iterator itResult = mDatablockConversionResults.find( datablock );
        if( itResult != mDatablockConversionResults.end() )
        {
            // A HIT IS RE-READ, NEVER TRUSTED. The row used to be handed back as it was
            // first written, which was harmless while every store died with its
            // voxelizer on each rebuild; a store that outlives rebuilds would then keep
            // a material's FIRST parameters for the rest of its life (measured: the
            // same datablock converted at emissive 0.5 read 0.5 at 1, 3 and 12). The row
            // is re-written in place, at the slot the datablock already owns, so every
            // consumer that names that slot sees the material as it is now.
            MaterialBucket &bucket = mBuckets[itResult->second.bucketIdx];
            const bool needsDiffuse = datablock->getDiffuseTexture() != 0;
            const bool needsEmissive = datablock->getEmissiveTexture() != 0;
            if( bucket.hasDiffuse == needsDiffuse && bucket.hasEmissive == needsEmissive )
            {
                writeRow( datablock, bucket, itResult->second.slotIdx, itResult->second );
                return itResult->second;
            }

            // THE BUCKET'S CLASS NO LONGER FITS - a texture was added to or taken off the
            // material, and a bucket is homogeneous in that (it decides the compute job
            // variant). The datablock MOVES. Its old slot is abandoned, not freed: the
            // membership set keeps the pointer, exactly as 0081 keeps a dead one, because
            // slots are numbered by that set's size. The caller is told, because anything
            // that recorded the old (bucket, slot) is now wrong.
            mDatablockConversionResults.erase( itResult );
            if( outMoved )
                *outMoved = true;
        }

        MaterialBucket *bucket = findFreeBucketFor( datablock );
        if( !bucket )
        {
            // Create a new bucket
            MaterialBucket newBucket;
            newBucket.buffer = mVaoManager->createConstBuffer(
                c_numDatablocksPerConstBuffer * sizeof( PhotonShaderVoxelMaterial ), BT_DEFAULT, 0, false );
            newBucket.hasDiffuse = datablock->getDiffuseTexture() != 0;
            newBucket.hasEmissive = datablock->getEmissiveTexture() != 0;
            mBuckets.push_back( newBucket );
            bucket = &mBuckets.back();
        }

        return addDatablockToBucket( datablock, *bucket );
    }
}  // namespace Ogre
