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

#include "photon/voxel/PhotonVoxelLighting.h"

#include "photon/voxel/PhotonVoxelizerSourceBase.h"
#include "photon/voxel/PhotonVoxelVisualizer.h"

#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreHlmsPbs.h"
#include "OgreLight.h"
#include "OgreLwString.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreRenderSystem.h"
#include "OgreSceneManager.h"
#include "OgreShaderPrimitives.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreVaoManager.h"

namespace Ogre
{
    struct PhotonShaderVoxelLight
    {
        // Pre-mul by PI? -No because we lose a ton of precision
        //.w contains lightDistThreshold
        float diffuse[4] = {};
        // For directional lights, pos.xyz contains -dir.xyz and pos.w = 0;
        // For the rest of lights, pos.xyz contains pos.xyz and pos.w = 1;
        float pos[4] = {};
        // uvwPos.w contains the light type
        float uvwPos[4] = {};

        // Used by area lights
        // points[0].w contains double sided info
        float points[4][4] = {};
    };

    const uint16 PhotonVoxelLighting::msDistanceThresholdCustomParam = 3876u;

    static const IdString NumVctCascadesProp = "hlms_num_vct_cascades";

    static const size_t c_maxCascades = 8u;

    // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080): THE FORMAT OF THE TOTAL VOLUME.
    //
    // The volume holds the fixed point of L = D + rho * G( L ). The DIRECT term D
    // is normalised to <= 1 by PhotonVoxelLighting::update's auto multiplier; the FIXED
    // POINT is not bounded by 1 at all -- it is D * sum( (rho*f)^i ), i.e. up to
    // D / (1 - rho*f), which for a white enclosure has no useful bound (measured:
    // 2.05x the direct term at albedo 0.79, 2.76x and climbing at albedo 1.0, 3.5x
    // in a shipped sample). An 8-bit UNORM store clips it, per channel, which
    // desaturates as it darkens and reads downstream exactly like a scene with
    // less bounce in it; and every fixed headroom that clears one room costs the
    // dark end, where the GI signal lives. So the total is a float. The direct
    // volume (fork ae2ed529f+155a56bf8 (was 0076)) keeps its 8-bit sRGB store: D <= 1 by construction.
    static PixelFormatGpu jahLightVoxelFormat() { return PFG_RGBA16_FLOAT; }
    static PixelFormatGpu jahLightVoxelUavFormat() { return PFG_RGBA16_FLOAT; }
    //-------------------------------------------------------------------------
    PhotonVoxelLighting::PhotonVoxelLighting( IdType id, PhotonVoxelizerSourceBase *voxelizer, bool bAnisotropic ) :
        IdObject( id ),
        mSamplerblockTrilinear( 0 ),
        mVoxelizer( voxelizer ),
        mVoxelizerTexturesChanged( false ),
        mVoxelizerListenersRemoved( false ),
        mLightInjectionJob( 0 ),
        mLightsConstBuffer( 0 ),
        mAnisoGeneratorStep0( 0 ),
        mLightVctBounceInject( 0 ),
        mLightBounce( 0 ),
        mLightDirect( 0 ),  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076)
        mLightDirectBack( 0 ),
        mInjectHigherMipHalfWidth( 0 ),
        mBakingMultiplier( 1.0f ),
        mInvBakingMultiplier( 1.0f ),
        mDefaultLightDistThreshold( 0.5f ),
        mAnisotropic( bAnisotropic ),
        mNumLights( 0 ),
        mBakingMultiplierParam( 0 ),
        mVoxelCellSize( 0 ),
        mInvVoxelResolution( 0 ),
        mShaderParams( 0 ),
        mBounceVoxelCellSize( 0 ),
        mBounceInvVoxelResolution( 0 ),
        mBounceIterationDampening( 0 ),
        mBounceInvResMaxLod( 0 ),
        mBounceFromPreviousProbeToNext( 0 ),
        mBounceEnvGainMips( 0 ),
        mBounceEnvSh( 0 ),
        mBounceShaderParams( 0 ),
        mSpecularSdfQuality( 0.875f ),
        mMultiplier( 1.0f ),
        mDebugVoxelVisualizer( 0 )
    {
        memset( mLightVoxel, 0, sizeof( mLightVoxel ) );
        memset( mLightDirectDir, 0, sizeof( mLightDirectDir ) );
        mEnvCube = 0;
        memset( mEnvGain, 0, sizeof( mEnvGain ) );
        memset( mEnvSh, 0, sizeof( mEnvSh ) );

        OGRE_ASSERT_LOW( mVoxelizer->getAlbedoVox() &&
                         "PhotonVoxelizer::build must've been called before creating PhotonVoxelLighting!" );

        mVoxelizer->getAlbedoVox()->addListener( this );
        mVoxelizer->getNormalVox()->addListener( this );
        mVoxelizerListenersRemoved = false;

        // PhotonVoxelizer should've already been initialized, thus no need
        // to check if JSON has been built or if the assets were added
        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightInjectionJob = hlmsCompute->findComputeJob( "VCT/LightInjection" );

        mShaderParams = &mLightInjectionJob->getShaderParams( "default" );
        mNumLights = mShaderParams->findParameter( "numLights" );
        mBakingMultiplierParam = mShaderParams->findParameter( "bakingMultiplier" );
        mVoxelCellSize = mShaderParams->findParameter( "voxelCellSize" );
        mInvVoxelResolution = mShaderParams->findParameter( "invVoxelResolution" );
        mInjectHigherMipHalfWidth = mShaderParams->findParameter( "higherMipHalfWidth" );

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();
        mLightsConstBuffer = vaoManager->createConstBuffer( sizeof( PhotonShaderVoxelLight ) * 16u,
                                                            BT_DYNAMIC_PERSISTENT, 0, false );

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        HlmsSamplerblock samplerblock;
        samplerblock.mMipFilter = FO_LINEAR;
        mSamplerblockTrilinear = hlmsManager->getSamplerblock( samplerblock );

        mLightVctBounceInject = hlmsCompute->findComputeJob( "VCT/LightVctBounceInject" );

        mBounceShaderParams = &mLightVctBounceInject->getShaderParams( "default" );

        // RESERVED FOR EVERY PARAM BELOW: the members are pointers INTO this vector.
        mLocalBounceShaderParams.reserve( 9u );

        mBounceVoxelCellSize = addLocalBounceShaderParam( "voxelCellSize" );
        mBounceInvVoxelResolution = addLocalBounceShaderParam( "invVoxelResolution" );
        mBounceIterationDampening = addLocalBounceShaderParam( "iterationDampening" );
        mBounceInvResMaxLod = addLocalBounceShaderParam( "vctInvResMaxLod" );
        mBounceFromPreviousProbeToNext = addLocalBounceShaderParam( "fromPreviousProbeToNext" );
        mBounceEnvGainMips = addLocalBounceShaderParam( "envGainMips" );
        mBounceEnvSh = addLocalBounceShaderParam( "envSh" );

        createTextures();
    }
    //-------------------------------------------------------------------------
    PhotonVoxelLighting::~PhotonVoxelLighting()
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();

        setDebugVisualization( false, 0 );

        if( mLightsConstBuffer )
        {
            if( mLightsConstBuffer->getMappingState() != MS_UNMAPPED )
                mLightsConstBuffer->unmap( UO_UNMAP_ALL );
            vaoManager->destroyConstBuffer( mLightsConstBuffer );
            mLightsConstBuffer = 0;
        }

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        hlmsManager->destroySamplerblock( mSamplerblockTrilinear );
        mSamplerblockTrilinear = 0;

        if( !mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->removeListener( this );
            mVoxelizer->getNormalVox()->removeListener( this );
            mVoxelizerListenersRemoved = true;
        }

        destroyTextures();
    }
    //-------------------------------------------------------------------------
    ShaderParams::Param *PhotonVoxelLighting::addLocalBounceShaderParam( const char *name )
    {
        mLocalBounceShaderParams.push_back( ShaderParams::Param() );
        ShaderParams::Param *retVal = &mLocalBounceShaderParams.back();
        retVal->name = name;
        return retVal;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::restoreSwappedTextures()
    {
        if( mLightVoxel[0] && mLightBounce )
        {
            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLightingBounce/Id", getId() );

            if( mLightBounce->getName() != texName.c_str() )
            {
                std::swap( mLightVoxel[0], mLightBounce );

                DescriptorSetTexture2::TextureSlot texSlot(
                    DescriptorSetTexture2::TextureSlot::makeEmpty() );
                texSlot.texture = mLightVoxel[0];
                mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );
            }
        }
    }
    //-------------------------------------------------------------------------
    float PhotonVoxelLighting::addLight( PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight, Light *light,
                                 const Vector3 &voxelOrigin, const Vector3 &invVoxelSize )
    {
        const ColourValue diffuseColour = light->getDiffuseColour() * light->getPowerScale();
        for( size_t i = 0; i < 3u; ++i )
            vctLight->diffuse[i] = static_cast<float>( diffuseColour[i] );

        const Vector4 *lightDistThreshold =
            light->getCustomParameterNoThrow( msDistanceThresholdCustomParam );
        vctLight->diffuse[3] = lightDistThreshold
                                   ? ( lightDistThreshold->x * lightDistThreshold->x )
                                   : ( mDefaultLightDistThreshold * mDefaultLightDistThreshold );

        Light::LightTypes lightType = light->getType();
        if( lightType == Light::LT_AREA_APPROX )
            lightType = Light::LT_AREA_LTC;

        Vector4 light4dVec = light->getAs4DVector();
        if( lightType != Light::LT_DIRECTIONAL )
            light4dVec -= Vector4( voxelOrigin, 0.0f );

        for( size_t i = 0; i < 4u; ++i )
            vctLight->pos[i] = static_cast<float>( light4dVec[i] );

        Vector3 uvwPos = light->getParentNode()->_getDerivedPosition();
        uvwPos = ( uvwPos - voxelOrigin ) * invVoxelSize;
        for( size_t i = 0; i < 3u; ++i )
            vctLight->uvwPos[i] = static_cast<float>( uvwPos[i] );
        vctLight->uvwPos[3] = static_cast<float>( lightType );

        Vector3 rectPoints[4];

        const Quaternion qRot = light->getParentNode()->_getDerivedOrientation();
        const Vector3 lightDir = qRot.zAxis();

        if( lightType == Light::LT_AREA_LTC )
        {
            const Vector3 lightPos( light4dVec.xyz() );
            const Vector2 rectSize = light->getDerivedRectSize() * 0.5f;
            Vector3 xAxis = qRot.xAxis() * rectSize.x;
            Vector3 yAxis = qRot.yAxis() * rectSize.y;

            rectPoints[0] = lightPos - xAxis - yAxis;
            rectPoints[1] = lightPos + xAxis - yAxis;
            rectPoints[2] = lightPos + xAxis + yAxis;
            rectPoints[3] = lightPos - xAxis + yAxis;
        }
        else
        {
            memset( rectPoints, 0, sizeof( rectPoints ) );

            if( lightType == Light::LT_SPOTLIGHT )
            {
                // float3 spotDirection
                rectPoints[0].x = -lightDir.x;
                rectPoints[0].y = -lightDir.y;
                rectPoints[0].z = -lightDir.z;

                // float3 spotParams
                const Radian innerAngle = light->getSpotlightInnerAngle();
                const Radian outerAngle = light->getSpotlightOuterAngle();
                rectPoints[1].x = 1.0f / ( cosf( innerAngle.valueRadians() * 0.5f ) -
                                           cosf( outerAngle.valueRadians() * 0.5f ) );
                rectPoints[1].y = cosf( outerAngle.valueRadians() * 0.5f );
                rectPoints[1].z = light->getSpotlightFalloff();
            }
        }

        const float isDoubleSided = light->getDoubleSided() ? 1.0f : 0.0f;
        for( size_t i = 0; i < 4u; ++i )
        {
            for( size_t j = 0; j < 3u; ++j )
                vctLight->points[i][j] = rectPoints[i][j];
            vctLight->points[i][3u] = isDoubleSided;
        }

        vctLight->points[1][3u] = static_cast<float>( lightDir.x );
        vctLight->points[2][3u] = static_cast<float>( lightDir.y );
        vctLight->points[3][3u] = static_cast<float>( lightDir.z );

        float maxValue;
        maxValue = std::max( diffuseColour.r, diffuseColour.g );
        maxValue = std::max( maxValue, diffuseColour.b );
        return maxValue;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::createTextures()
    {
        const bool allowsMultipleBounces = getAllowMultipleBounces();
        destroyTextures();

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();

        // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080): NOT Reinterpretable. The flag existed for the 8-bit
        // store, whose UAV view (RGBA8_UNORM) reinterpreted the sRGB texture; a float
        // store's UAV view IS its format. And the flag is not free: a reinterpretable
        // texture is created as its format FAMILY -- R16G16B16A16_UINT for 16F --
        // and the mip chain's linear blit (_autogenerateMipmaps) is then invalid on
        // an integer image (VUID-vkCmdBlitImage-filter-02001: the format has no
        // linear-filter feature), so the coarse mips the cone gather reads would be
        // undefined. The direct volumes are float too since CONTACT-OCCLUSION-1 and carry
        // no Reinterpretable either (their UAV views are their formats).
        uint32 texFlags = TextureFlags::Uav;

        const bool bSdfQuality = shouldEnableSpecularSdfQuality();
        if( !mAnisotropic || bSdfQuality )
            texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

        const TextureGpu *albedoVox = mVoxelizer->getAlbedoVox();

        const uint32 width = albedoVox->getWidth();
        const uint32 height = albedoVox->getHeight();
        const uint32 depth = albedoVox->getDepth();

        const uint32 widthAniso = std::max( 1u, width );
        const uint32 heightAniso = std::max( 1u, height >> 1u );
        const uint32 depthAniso = std::max( 1u, depth >> 1u );

        // Jahshaka (PHOTON-VOXEL-4): every chain stops where the SHORTEST axis reaches one
        // texel (the voxelizer's own rule, PhotonVoxelizer.cpp): a cell is a cube and every
        // texel of every level stays one. The directional volumes' real extent per axis is
        // half the voxels (both signs are packed along x): their last level is the one whose
        // shortest real axis is one texel - for a cube the pin's "2x1x1" level, unchanged;
        // for 32 x 16 x 32 cells the 4 x 1 x 2 one (step 1 would otherwise composite a
        // one-texel axis with the out-of-range texel beside it).
        const uint32 shortest = std::min( width, std::min( height, depth ) );
        const uint8 numMipsMain = ( mAnisotropic && !bSdfQuality )
                                      ? 1u
                                      : PixelFormatGpuUtils::getMaxMipmapCount( shortest );
        const uint8 numMipsAniso = PixelFormatGpuUtils::getMaxMipmapCount( std::max( 1u, shortest >> 1u ) );

        const size_t numTextures = mAnisotropic ? 4u : 1u;

        const char *names[] = {
            "Main",    //
            "X_axis",  //
            "Y_axis",  //
            "Z_axis"   //
        };

        for( size_t i = 0; i < numTextures; ++i )
        {
            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLighting_", names[i], "/Id", getId() );
            TextureGpu *texture = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );
            if( i == 0u )
            {
                texture->setResolution( width, height, depth );
                texture->setNumMipmaps( numMipsMain );
            }
            else
            {
                texture->setResolution( widthAniso, heightAniso, depthAniso );
                texture->setNumMipmaps( numMipsAniso );
            }
            texture->setPixelFormat( jahLightVoxelFormat() );  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
            texture->scheduleTransitionTo( GpuResidency::Resident );
            mLightVoxel[i] = texture;

            texFlags &= ( uint32 ) ~( TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps );
        }
        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage and the surface position per
        // half-axis, the list's last four entries (the voxelizer's, not owned -
        // destroyTextures() leaves them alone).
        for( uint32 h = 0u; h < 2u; ++h )
        {
            mLightVoxel[coverageIndex( h )] = mVoxelizer->getCoverageVox( h );
            mLightVoxel[positionIndex( h )] = mVoxelizer->getPositionVox( h );
        }

        if( mAnisotropic )
        {
            // Jahshaka (PHOTON-VOXEL-5): LEVEL 0's BACK SIDE (mLightVoxel[0] holds the sides'
            // mean) - level 0 only, read by the level-0 readers with the voxelizer's normal (the
            // side a half's faces look to), the list's last two entries.
            {
                char tmpBuffer[128];
                LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
                texName.a( "VctLighting_Back/Id", getId() );
                TextureGpu *texture = textureManager->createTexture(
                    texName.c_str(), GpuPageOutStrategy::Discard, TextureFlags::Uav, TextureTypes::Type3D );
                texture->setResolution( width, height, depth );
                texture->setNumMipmaps( 1u );
                texture->setPixelFormat( jahLightVoxelFormat() );
                texture->scheduleTransitionTo( GpuResidency::Resident );
                mLightVoxel[backIndex()] = texture;
            }
            mLightVoxel[normalIndex()] = mVoxelizer->getNormalVox();

            // Setup the compute shaders for PhotonVoxelLighting::generateAnisotropicMips()
            HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();

            char tmpBuffer[128];
            LwString jobName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            DescriptorSetTexture2::TextureSlot texSlot(
                DescriptorSetTexture2::TextureSlot::makeEmpty() );
            ShaderParams *shaderParams = 0;
            ShaderParams::Param *lowerMipResolutionParam = 0;

            // Step 1: the directional mips from level 0 (which the light injection composites
            // itself - the fused step 0 - and step 0 completes with a bounce's part).
            const uint8 numMipsOnStep1 = mLightVoxel[1]->getNumMipmaps() - 1u;
            mAnisoGeneratorStep1.resize( numMipsOnStep1 );

            HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( "VCT/AnisotropicMipStep1" );

            for( uint8 i = 0; i < numMipsOnStep1; ++i )
            {
                jobName.clear();
                jobName.a( "VCT/AnisotropicMipStep1/Id", getId(), "/Mip", i + 1u );
                HlmsComputeJob *mipJob = baseJob->clone( jobName.c_str() );

                for( uint8 axis = 0; axis < 3u; ++axis )
                {
                    texSlot.texture = mLightVoxel[axis + 1u];
                    texSlot.mipmapLevel = i;
                    texSlot.numMipmaps = 1u;
                    mipJob->setTexture( axis, texSlot );

                    DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
                    uavSlot.access = ResourceAccess::Write;
                    uavSlot.texture = mLightVoxel[axis + 1u];
                    uavSlot.mipmapLevel = i + 1u;
                    uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
                    mipJob->_setUavTexture( axis, uavSlot );
                }

                shaderParams = &mipJob->getShaderParams( "default" );
                // higherMipHalfRes_lowerMipHalfWidth
                lowerMipResolutionParam = &shaderParams->mParams.back();
                int32 resolutions[4] = { //
                                         static_cast<int32>( mLightVoxel[1]->getWidth() >> ( i + 2u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getHeight() >> ( i + 1u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getDepth() >> ( i + 1u ) ),
                                         static_cast<int32>( mLightVoxel[1]->getWidth() >> ( i + 1u ) )
                };
                for( size_t j = 0; j < 4u; ++j )
                    resolutions[j] = std::max( 1, resolutions[j] );
                lowerMipResolutionParam->setManualValue( resolutions, 4u );
                shaderParams->setDirty();

                mAnisoGeneratorStep1[i] = mipJob;
            }
        }

        setAllowMultipleBounces( allowsMultipleBounces );

        if( mDebugVoxelVisualizer )
        {
            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );
            mDebugVoxelVisualizer->setVisible( true );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::destroyTextures()
    {
        restoreSwappedTextures();

        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage and position entries are the voxelizer's,
        // and (PHOTON-VOXEL-5) so is the normal's.
        for( uint32 h = 0u; h < 2u; ++h )
        {
            mLightVoxel[coverageIndex( h )] = 0;
            mLightVoxel[positionIndex( h )] = 0;
        }
        if( mAnisotropic )
            mLightVoxel[normalIndex()] = 0;

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();
        for( size_t i = 0; i < sizeof( mLightVoxel ) / sizeof( mLightVoxel[0] ); ++i )
        {
            if( mLightVoxel[i] )
            {
                textureManager->destroyTexture( mLightVoxel[i] );
                mLightVoxel[i] = 0;
            }
        }

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();

        if( mAnisoGeneratorStep0 )
        {
            hlmsCompute->destroyComputeJob( mAnisoGeneratorStep0->getName() );
            mAnisoGeneratorStep0 = 0;
        }

        FastArray<HlmsComputeJob *>::const_iterator itor = mAnisoGeneratorStep1.begin();
        FastArray<HlmsComputeJob *>::const_iterator end = mAnisoGeneratorStep1.end();

        while( itor != end )
        {
            hlmsCompute->destroyComputeJob( ( *itor )->getName() );
            ++itor;
        }

        mAnisoGeneratorStep1.clear();
        setAllowMultipleBounces( false );

        if( mDebugVoxelVisualizer )
            mDebugVoxelVisualizer->setVisible( false );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::checkTextures()
    {
        if( mVoxelizerTexturesChanged )
        {
            createTextures();
            // The textures have been re-created; the request has been served. Without
            // this the flag stays raised for the object's whole life (it is only ever
            // assigned false in the constructor), so a voxelizer that loses residency
            // ONCE makes every subsequent update() destroy and re-create the light voxel
            // textures, for ever.
            mVoxelizerTexturesChanged = false;
        }

        if( mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->addListener( this );
            mVoxelizer->getNormalVox()->addListener( this );
            mVoxelizerListenersRemoved = false;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setVoxelizer( PhotonVoxelizerSourceBase *voxelizer )
    {
        if( !voxelizer || voxelizer == mVoxelizer )
            return;

        if( !mVoxelizerListenersRemoved )
        {
            mVoxelizer->getAlbedoVox()->removeListener( this );
            mVoxelizer->getNormalVox()->removeListener( this );
        }

        mVoxelizer = voxelizer;
        // The same two requests a LostResidency notification raises, served immediately:
        // re-create the light voxels from the new voxelizer's resolution and re-register
        // as a listener on its albedo/normal textures.
        mVoxelizerTexturesChanged = true;
        mVoxelizerListenersRemoved = true;
        checkTextures();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setupBounceTextures( bool bSetSamplerRefs )
    {
        const size_t numExtraCascades = mExtraCascades.size();

        // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): +1 for `directVoxel`, the fixed point's D term. It is
        // bound LAST so that every existing slot index -- albedo, normal, this
        // cascade's probes, the extra cascades', the three anisotropic sets -- keeps
        // the number it had, in the C++ and in the shader's ogre_tN layout alike.
        // Jahshaka (PHOTON-VOXEL-3/-4): + four per cascade - the coverage per half-axis and
        // the surface position per half, the list's last entries - bound after the probe
        // sets and before `directVoxel`.
        uint8 numNeededTexUnits;
        if( mAnisotropic )
            numNeededTexUnits = 10u + 8u * static_cast<uint8>( numExtraCascades ) + 1u;
        else
            numNeededTexUnits = 7u + 5u * static_cast<uint8>( numExtraCascades ) + 1u;
        // Jahshaka (PHOTON-ENV-1): +1 for the environment cube, at the very last unit
        // (after `directVoxel`) and only while one is set - the job's `jah_env`
        // property says so to the shader, and it is re-asserted per dispatch with
        // the rest of these bindings (the job is shared by name).
        if( mEnvCube )
            ++numNeededTexUnits;
        // Jahshaka (PHOTON-WRITER-1): +1 for the voxeliser's EMISSIVE volume, whose alpha
        // holds the voxel's roughness (the bounce's re-emission carries the diffuse
        // lobe's hemispherical albedo). Bound right after `directVoxel`.
        ++numNeededTexUnits;
        // Jahshaka (PHOTON-VOXEL-5): +1 on the anisotropic tiers for the BACK side's direct term,
        // right after the emissive volume (before the environment cube).
        if( mAnisotropic )
            ++numNeededTexUnits;

        HlmsManager *hlmsManager = mVoxelizer->getHlmsManager();
        const RenderSystemCapabilities *caps = hlmsManager->getRenderSystem()->getCapabilities();
        const bool bSetSampler = !caps->hasCapability( RSC_SEPARATE_SAMPLERS_FROM_TEXTURES );
        // The samplerblocks are bound ONCE and never move; the textures move on every
        // bounce. So a re-assert (runBounce, below) writes the textures and leaves the
        // sampler reference counting alone, which is the only reason this call is cheap
        // enough to repeat per dispatch.
        const bool bSetSamplerNow = bSetSampler && bSetSamplerRefs;

        if( mLightVctBounceInject->getNumTexUnits() != numNeededTexUnits )
        {
            mLightVctBounceInject->setNumTexUnits( numNeededTexUnits );
            if( !bSetSampler )
                mLightVctBounceInject->setNumSamplerUnits( 3u );
        }

        setupGlslTextureUnits();

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mVoxelizer->getAlbedoVox();
        mLightVctBounceInject->setTexture( 0, texSlot );
        texSlot.texture = mVoxelizer->getNormalVox();
        mLightVctBounceInject->setTexture( 1, texSlot );
        texSlot.texture = mLightVoxel[0];
        mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );

        uint8 texSlotIdx = 3u;

        for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades; ++cascadeIdx )
        {
            texSlot.texture = mExtraCascades[cascadeIdx]->mLightVoxel[0];
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
            if( bSetSamplerNow )
            {
                // Only OpenGL needs this sampler set
                hlmsManager->addReference( mSamplerblockTrilinear );
                mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u, mSamplerblockTrilinear );
            }
        }

        if( mAnisotropic )
        {
            for( uint8 i = 0u; i < 3u; ++i )
            {
                texSlot.texture = mLightVoxel[i + 1u];
                mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
                if( bSetSamplerNow )
                {
                    // Only OpenGL needs this sampler set
                    hlmsManager->addReference( mSamplerblockTrilinear );
                    mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u, mSamplerblockTrilinear );
                }

                for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades; ++cascadeIdx )
                {
                    texSlot.texture = mExtraCascades[cascadeIdx]->mLightVoxel[i + 1u];
                    mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
                    if( bSetSamplerNow )
                    {
                        // Only OpenGL needs this sampler set
                        hlmsManager->addReference( mSamplerblockTrilinear );
                        mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u,
                                                                 mSamplerblockTrilinear );
                    }
                }
            }
        }

        // Jahshaka (PHOTON-VOXEL-3/-4): every cascade's COVERAGE PER HALF-AXIS (the march
        // takes the opacity along the cone from it) and SURFACE POSITION PER HALF (its
        // origin plane) - the list's last four kinds, each over every cascade.
        for( uint32 k = 0u; k < 4u; ++k )
        {
            const uint32 h = k & 1u;
            const bool position = k >= 2u;
            texSlot.texture = position ? mVoxelizer->getPositionVox( h ) : mVoxelizer->getCoverageVox( h );
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
            if( bSetSamplerNow )
            {
                hlmsManager->addReference( mSamplerblockTrilinear );
                mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u, mSamplerblockTrilinear );
            }
            for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades; ++cascadeIdx )
            {
                PhotonVoxelLighting *extra = mExtraCascades[cascadeIdx];
                texSlot.texture = extra->mLightVoxel[position ? extra->positionIndex( h )
                                                              : extra->coverageIndex( h )];
                mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
                if( bSetSamplerNow )
                {
                    hlmsManager->addReference( mSamplerblockTrilinear );
                    mLightVctBounceInject->_setSamplerblock( texSlotIdx - 1u,
                                                             mSamplerblockTrilinear );
                }
            }
        }

        // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): THE DIRECT TERM, at the last unit. Read with a plain
        // Load3D at the voxel being written, so it needs no sampler of its own (the
        // OpenGL path's samplerblock loop above deliberately skips it).
        texSlot.texture = mLightDirect;
        mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        // Jahshaka (PHOTON-WRITER-1): the voxeliser's emissive volume (roughness in .w).
        texSlot.texture = mVoxelizer->getEmissiveVox();
        mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        // Jahshaka (PHOTON-VOXEL-5): the back side's direct term (a Load3D, no sampler).
        if( mAnisotropic )
        {
            texSlot.texture = mLightDirectBack;
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        }

        // Jahshaka (PHOTON-ENV-1): the environment cube, sampled with the probes'
        // trilinear sampler (a separate sampler object on Vulkan).
        {
            const int32 envOn = mEnvCube ? 1 : 0;
            if( mLightVctBounceInject->getProperty( "jah_env" ) != envOn )
                mLightVctBounceInject->setProperty( "jah_env", envOn );
        }
        // Jahshaka (PHOTON-VOXEL-4, CONE-SET-1): THE PIXEL'S CONE SET. The bounce is the
        // store's own integral of the diffuse the pixel reads, so it walks the pixel's
        // cones - Vct_piece_ps.any's `vct_cone_dirs`, which HlmsPbs sets from
        // getVctFullConeCount (the surface cache's card job does the same). It carried
        // the six-cone set hard-coded while the pixel and the cards ran four: two
        // quadratures of one store, the bounce's error not the pixel's.
        {
            HlmsPbs *pbs = dynamic_cast<HlmsPbs *>( hlmsManager->getHlms( HLMS_PBS ) );
            const int32 cones = ( pbs && pbs->getVctFullConeCount() ) ? 6 : 4;
            if( mLightVctBounceInject->getProperty( "vct_cone_dirs" ) != cones )
                mLightVctBounceInject->setProperty( "vct_cone_dirs", cones );
        }
        if( mEnvCube )
        {
            texSlot.texture = mEnvCube;
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        }

        // Jahshaka (PHOTON-VOXEL-5): the back side's total, u1 on the anisotropic tiers, written in
        // place: the march's level-0 read in this job takes the sides' MEAN (it binds no back), so
        // nothing here reads it. Measured: a back that ping-pongs and is read per side moved the
        // roof fixture's two-bounce underside by nothing (gi.voxel_coverage, 0.02193 both), at
        // 8 B a voxel on every bouncing tier.
        const uint8 numUavs = mAnisotropic ? 2u : 1u;
        if( mLightVctBounceInject->getNumUavUnits() != numUavs )
            mLightVctBounceInject->setNumUavUnits( numUavs );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightBounce;
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
        mLightVctBounceInject->_setUavTexture( 0, uavSlot );
        if( mAnisotropic )
        {
            uavSlot.texture = mLightVoxel[backIndex()];
            mLightVctBounceInject->_setUavTexture( 1, uavSlot );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setupGlslTextureUnits()
    {
        const size_t numExtraCascades = mExtraCascades.size();
        // THE LIST'S LENGTH IS PARAMETERS, NOT UNITS (Jahshaka, PHOTON-VOXEL-5; the VOXEL-4
        // audit's F1): the two fixed textures, then ONE array parameter per texture variable
        // (vctProbes, the three axes when anisotropic, the four split kinds - each an array over
        // every cascade), then `directVoxel` (JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076)) and voxelEmissiveTex
        // (PHOTON-WRITER-1). It was compared with a unit count (8 + 6e + 2 / 5 + 3e + 2), which the
        // list never had, so the "glsl" list was rebuilt and set dirty on every bounce dispatch.
        const size_t numTextureVariables = mAnisotropic ? 8u : 5u;
        const size_t numNeededParams = 2u + numTextureVariables + 2u + ( mAnisotropic ? 1u : 0u );

        // This code assumes there's 2 textures at the beginning that always stays the same
        // the rest of them are dynamically generated.
        //
        // We also need to check if another PhotonVoxelLighting instance set a different number of cascades
        ShaderParams &glslShaderParams = mLightVctBounceInject->getShaderParams( "glsl" );
        if( glslShaderParams.mParams.size() != numNeededParams ||
            glslShaderParams.mParams[3].mp.dataSizeBytes !=
                ( numExtraCascades + 1u ) * sizeof( uint32 ) )
        {
            glslShaderParams.mParams.resize( 2u );

            ShaderParams::Param param;
            int32 texSlotIdx = 2u;

            // Jahshaka (PHOTON-VOXEL-3/-4): + the coverage and the surface position per
            // half-axis, the last four arrays.
            const char *names[4] = { "vctProbes", "vctProbeX", "vctProbeY", "vctProbeZ" };
            const char *splitNames[4] = { "vctProbeCovP", "vctProbeCovN", "vctProbePosP",
                                          "vctProbePosN" };

            for( size_t i = 0u; i < numTextureVariables; ++i )
            {
                // the isotropic volume, the three axes (anisotropic), then the split kinds in order
                const size_t numLead = mAnisotropic ? 4u : 1u;
                param.name = i < numLead ? names[i] : splitNames[i - numLead];
                int32 textureUnitsTmp[16];
                for( size_t cascadeIdx = 0u; cascadeIdx < numExtraCascades + 1u; ++cascadeIdx )
                    textureUnitsTmp[cascadeIdx] = texSlotIdx++;
                param.setManualValue( textureUnitsTmp, static_cast<uint32>( numExtraCascades + 1u ) );
                glslShaderParams.mParams.push_back( param );
            }

            // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): the direct volume's own unit, after every probe
            // array -- the same order setupBounceTextures() binds them in.
            param.name = "directVoxel";
            param.setManualValue( texSlotIdx );
            glslShaderParams.mParams.push_back( param );
            // Jahshaka (PHOTON-WRITER-1): the voxeliser's emissive volume, right after.
            param.name = "voxelEmissiveTex";
            param.setManualValue( texSlotIdx + 1 );
            glslShaderParams.mParams.push_back( param );
            // Jahshaka (PHOTON-VOXEL-5): the back side's direct term, right after.
            if( mAnisotropic )
            {
                param.name = "directBackVoxel";
                param.setManualValue( texSlotIdx + 2 );
                glslShaderParams.mParams.push_back( param );
            }

            glslShaderParams.setDirty();
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::generateAnisotropicMips()
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        renderSystem->debugAnnotationPush( "VctLighting Anisotropic Mips" );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();

        // Jahshaka (PHOTON-VOXEL-5): step 0 exists with a bounce only - the directional level 0
        // is the injection's direct part (mLightDirectDir) plus the bounce's part per half-axis.
        // Its textures are bound here, per dispatch: the total ping-pongs under it.
        if( mAnisoGeneratorStep0 )
        {
            TextureGpu *inputs[10] = { mLightVoxel[0],
                                       mLightVoxel[backIndex()],
                                       mLightDirect,
                                       mLightDirectBack,
                                       mVoxelizer->getNormalVox(),
                                       mVoxelizer->getCoverageVox( 0u ),
                                       mVoxelizer->getCoverageVox( 1u ),
                                       mLightDirectDir[0],
                                       mLightDirectDir[1],
                                       mLightDirectDir[2] };
            DescriptorSetTexture2::TextureSlot texSlot(
                DescriptorSetTexture2::TextureSlot::makeEmpty() );
            for( uint8 i = 0u; i < 10u; ++i )
            {
                texSlot.texture = inputs[i];
                mAnisoGeneratorStep0->setTexture( i, texSlot );
            }
            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            uavSlot.access = ResourceAccess::Write;
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            for( uint8 i = 0u; i < 3u; ++i )
            {
                uavSlot.texture = mLightVoxel[i + 1u];
                mAnisoGeneratorStep0->_setUavTexture( i, uavSlot );
            }
            mAnisoGeneratorStep0->analyzeBarriers( mResourceTransitions );
            renderSystem->executeResourceTransition( mResourceTransitions );
            hlmsCompute->dispatch( mAnisoGeneratorStep0, 0, 0 );
        }

        FastArray<HlmsComputeJob *>::const_iterator itor = mAnisoGeneratorStep1.begin();
        FastArray<HlmsComputeJob *>::const_iterator endt = mAnisoGeneratorStep1.end();

        while( itor != endt )
        {
            ( *itor )->analyzeBarriers( mResourceTransitions );
            renderSystem->executeResourceTransition( mResourceTransitions );
            hlmsCompute->dispatch( *itor, 0, 0 );
            ++itor;
        }
        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::runBounce( bool envOnly )
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        renderSystem->debugAnnotationPush( envOnly ? "VctLighting Sky" : "VctLighting Bounce" );

        // Jahshaka (CONTACT-OCCLUSION-1): the sky's direct term is this pass with the
        // voxels' share of the gather compiled out (the job's `jah_env_only`). The job is
        // shared by name, so the property is re-asserted per dispatch like its bindings.
        {
            const int32 envOnlyI32 = envOnly ? 1 : 0;
            if( mLightVctBounceInject->getProperty( "jah_env_only" ) != envOnlyI32 )
                mLightVctBounceInject->setProperty( "jah_env_only", envOnlyI32 );
        }

        mBounceVoxelCellSize->setManualValue( mVoxelizer->getVoxelCellSize() );
        mBounceInvVoxelResolution->setManualValue( 1.0f / mVoxelizer->getVoxelResolution() );
        // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): ONE, AND IT IS THE PHYSICS.
        //
        // The bounce adds `albedo * G` where G is the six-cone weighted mean of the
        // voxel radiance -- weights that sum to 1 over a cosine-ish set, i.e. an
        // ESTIMATE OF E / PI already (that is exactly why HlmsPbs consumes the same
        // gather as `envColourD` with no division of its own,
        // 200.BRDFs_piece_ps.any: Rd = envColourD * albedo). The bounced outgoing
        // radiance of a Lambertian surface is rho * E / pi = rho * G: no further
        // factor exists to apply.
        //
        // Upstream divided by pi here (and its commented-out line divided by an extra
        // 1 + n/2 per pass). Both were fudges against the runaway this patch fixes at
        // the cause: the job re-gathered the TOTAL and added to it, so the series was
        // binomial in (1 + rho*G) and every extra pass amplified the first bounce
        // instead of adding a dimmer one. With the Jacobi form -- new = direct +
        // rho * G( total ) -- the series contracts by rho * f per pass on its own,
        // and a dampening of 1/pi would simply make every bounce pi times too dark
        // (which is what upstream's "more bounces for coarser cascades" stabilisation,
        // deleted by PHOTON-M1, was compensating for).
        mBounceIterationDampening->setManualValue( 1.0f );

        const size_t numCascades = mExtraCascades.size() + 1u;

        OGRE_ASSERT_LOW( numCascades < c_maxCascades && "VctLighting: Up to 16 cascades are supported" );

        // THE CHAIN'S PARAMETERS COME FROM THEIR ONE DEFINITION (Jahshaka,
        // PHOTON-READER-1): getCascadeChainParams, the same arrays the pixel pass
        // buffer and the irradiance field's generation job read, because all three run
        // the one cone march. This used to be a second formula set - a scalar
        // 1/smallestRes on all three axes, and a hop weight of the cascades'
        // mMultiplier ratio WITHOUT their baking multipliers - equal to the first only
        // while every volume is cubic and every cascade bakes at the same D_max.
        float4 vctInvResMaxLod[c_maxCascades];
        float4 fromPreviousProbeToNext[c_maxCascades][2];
        getCascadeChainParams( &vctInvResMaxLod[0].x, &fromPreviousProbeToNext[0][0].x );

        {
            const int32 numCascadesI32 = static_cast<int32>( numCascades );
            if( mLightVctBounceInject->getProperty( NumVctCascadesProp ) != numCascadesI32 )
                mLightVctBounceInject->setProperty( NumVctCascadesProp, numCascadesI32 );
        }

        mBounceInvResMaxLod->setManualValue( &vctInvResMaxLod[0].x,
                                             static_cast<uint32>( numCascades * 4u ) );
        if( !mExtraCascades.empty() )
        {
            mBounceFromPreviousProbeToNext->setManualValueEx(
                &fromPreviousProbeToNext[0]->x, static_cast<uint32>( ( numCascades - 1u ) * 2u * 4u ) );
        }
        else
        {
            mBounceFromPreviousProbeToNext->setManualValue( 0.0f );
            mBounceFromPreviousProbeToNext->isDirty = false;
        }
        // THE ENVIRONMENT, in this volume's STORED units (Jahshaka, PHOTON-ENV-1): the
        // voxels hold radiance divided by the decode multiplier the pixel shader
        // applies (fillConstBufferData's `multiplier`), so the sky an escaping bounce
        // cone adds is divided by the same number here and decodes to radiance.
        {
            const float finalMultiplier = mInvBakingMultiplier * mMultiplier;
            const float invFinal = finalMultiplier > 0.0f ? 1.0f / finalMultiplier : 0.0f;
            const float mips = mEnvCube ? float( mEnvCube->getNumMipmaps() ) : 1.0f;
            const float gainMips[4] = { mEnvGain[0] * invFinal, mEnvGain[1] * invFinal,
                                        mEnvGain[2] * invFinal, mips };
            mBounceEnvGainMips->setManualValue( gainMips, 4u );
            float sh[36];
            for( size_t i = 0u; i < 9u; ++i )
            {
                for( size_t c = 0u; c < 3u; ++c )
                    sh[i * 4u + c] = mEnvSh[i * 3u + c] * invFinal;
                sh[i * 4u + 3u] = 0.0f;
            }
            mBounceEnvSh->setManualValue( sh, 36u );
        }

        mBounceShaderParams->mParams.swap( mLocalBounceShaderParams );
        mBounceShaderParams->setDirty();

        // THE BINDINGS ARE RE-ASSERTED PER DISPATCH, and it is not belt and braces —
        // it is the only way they can be right. Two independent reasons:
        //
        // (1) THE TEXTURES MOVE UNDER THE JOB. This function ends with
        //     `std::swap( mLightVoxel[0], mLightBounce )`, and re-binds slot 2 (its own
        //     light voxel) right after it — but slots 3..N hold the EXTRA CASCADES'
        //     mLightVoxel[], written once in setupBounceTextures() and never again.
        //     Every one of those cascades runs its own runBounce() and swaps its own
        //     pointers, so after an ODD number of bounce iterations on a cascade, this
        //     job is reading the texture that cascade has just stopped writing: the
        //     cross-cascade term of the bounce integrates stale radiance, silently, with
        //     no log line and no validation error. An EVEN number happens to come back
        //     to where it started (a swap is an involution), which is why this survived:
        //     upstream's own cascade manager gives every cascade the same bounce count.
        //     A per-cascade count is not exotic — the stabilisation upstream documents
        //     (a coarser cascade gets more bounces - upstream's VctCascadedVoxelizer,
        //     which is not in this fork)
        //     produces 1/2/4/8 on a four-cascade chain at three total bounces, and
        //     odd counts on outer cascades at other totals (1/1/2/4 at two).
        //
        // (2) THE JOB IS SHARED BY NAME. "VCT/LightVctBounceInject" is found by name, so
        //     every PhotonVoxelLighting in the process uses ONE HlmsComputeJob while the
        //     bindings belong to whichever instance called setupBounceTextures() last —
        //     the same shared-job class as this file's albedo/normal listeners. Two
        //     live chains would inject one chain's voxels through the other's textures.
        //     Re-asserting here makes the bindings belong to the instance DISPATCHING.
        //
        // The cost is the slot writes themselves: the unit count is change-guarded, the
        // GLSL unit list is rewritten (cheap, and OpenGL-only), and `false` skips the
        // OpenGL-only samplerblock
        // reference counting, which would otherwise leak a reference per dispatch (the
        // samplers do not move — only the textures do).
        setupBounceTextures( false );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightVctBounceInject->analyzeBarriers( mResourceTransitions );
        renderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mLightVctBounceInject, 0, 0 );

        std::swap( mLightVoxel[0], mLightBounce );

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mLightVoxel[0];
        mLightVctBounceInject->setTexture( 2, texSlot, mSamplerblockTrilinear );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightBounce;
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
        mLightVctBounceInject->_setUavTexture( 0, uavSlot );

        if( mAnisotropic )
            generateAnisotropicMips();

        if( mLightVoxel[0]->getNumMipmaps() > 1u )
        {
            renderSystem->debugAnnotationPush( "PhotonVoxelLighting::runBounce regular mipmaps" );
            mLightVoxel[0]->_autogenerateMipmaps();
            renderSystem->endCopyEncoder();
            renderSystem->debugAnnotationPop();
        }

        mBounceShaderParams->mParams.swap( mLocalBounceShaderParams );

        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::reserveExtraCascades( size_t numExtraCascades )
    {
        mExtraCascades.reserve( numExtraCascades );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::addCascade( PhotonVoxelLighting *cascade )
    {
        mExtraCascades.push_back( cascade );

        // THE CHAIN DECIDES HOW MANY TEXTURES THE BOUNCE SHADER DECLARES, so the job has
        // to be told the moment the chain grows. runBounce() sets
        // hlms_num_vct_cascades from mExtraCascades.size() + 1 on every injection, and
        // the generated compute shader declares one light-voxel texture per cascade
        // (four with anisotropy) — but the job's TEXTURE UNIT COUNT and its bindings are
        // only ever derived in setupBounceTextures(), which upstream calls from
        // setAllowMultipleBounces() and resetTexturesFromBuildRelative(). Add a cascade
        // after enabling bounces and the shader asks for ogre_t6 while the root layout
        // still has six units: "'ogre_t6': unrecognized layout identifier", the compute
        // program fails to compile, and the whole PhotonVoxelLighting arm renders no GI at all
        // with nothing but that one log line to say so.
        //
        // Nothing documents an order for these two calls (PhotonVoxelLighting's header does not),
        // and upstream's own VctCascadedVoxelizer (not in this fork) chained BEFORE it enabled
        // bounces, which is why upstream never meets this. Now either order works.
        if( getAllowMultipleBounces() )
            setupBounceTextures();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setAllowMultipleBounces( bool bAllowMultipleBounces )
    {
        if( getAllowMultipleBounces() == bAllowMultipleBounces )
            return;

        TextureGpuManager *textureManager = mVoxelizer->getTextureGpuManager();
        if( bAllowMultipleBounces )
        {
            // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080): not Reinterpretable -- see createTextures.
            uint32 texFlags = TextureFlags::Uav;
            if( !mAnisotropic || shouldEnableSpecularSdfQuality() )
                texFlags |= TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps;

            char tmpBuffer[128];
            LwString texName( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            texName.a( "VctLightingBounce/Id", getId() );
            TextureGpu *texture = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard, texFlags, TextureTypes::Type3D );

            texture->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                    mLightVoxel[0]->getDepth() );
            texture->setNumMipmaps( mLightVoxel[0]->getNumMipmaps() );
            texture->setPixelFormat( jahLightVoxelFormat() );  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
            texture->scheduleTransitionTo( GpuResidency::Resident );
            mLightBounce = texture;

            // Jahshaka (CONTACT-OCCLUSION-1): THE DIRECT STORES ARE FLOAT, like the total. They
            // were 8-bit sRGB on the claim that D is normalised to the ceiling by construction
            // (the auto multiplier's brightest LAMP) - which an EMISSIVE surface is not: its
            // radiance x the multiplier exceeds 1 wherever it outshines the brightest lamp, and
            // every bounce pass rebuilt the total from the clipped D. The sky pass runs at every
            // bounce count, so the clip reached every tier (gi.hit_planar: an emissive panel of
            // 2.0 read 0.99 in the mirror). A float D holds what the float total holds: 8 bytes
            // a voxel instead of 4 on the five direct volumes.
            // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): the direct term's own volume, born and buried with
            // the bounce texture -- it is the bounce iteration that needs it, and
            // nothing else reads it. ONE mip: the bounce job reads it with a Load3D at
            // the voxel it is writing (the fixed point's D term at this cell), never
            // filtered and never at a coarser level, so the mip chain the total needs
            // for the cone gather would be dead weight (+14 % of the volume).
            texName.clear();
            texName.a( "VctLightingDirect/Id", getId() );
            // Jahshaka (CONTACT-OCCLUSION-1, MOVER-OCCLUSION-1): A MIP CHAIN. A ray tier's hit
            // reads D beside the total at the ray's own footprint (jah_rq_hit.glsl: the
            // store's non-direct share, total - D, is what a MOVER above the hit occludes
            // and the store cannot know), so D is filtered like the total: its own chain,
            // regenerated after every injection (update()). The bounce job still reads mip 0.
            TextureGpu *directTex = textureManager->createTexture(
                texName.c_str(), GpuPageOutStrategy::Discard,
                // NOT Reinterpretable (the total's rule, fork 0080): the store is float and its
                // UAV view IS its format, and a mutable-format image fails the linear-filter
                // blit its mip chain is built with (VUID-vkCmdBlitImage-filter-02001).
                TextureFlags::Uav | TextureFlags::RenderToTexture | TextureFlags::AllowAutomipmaps,
                TextureTypes::Type3D );
            directTex->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                      mLightVoxel[0]->getDepth() );
            directTex->setNumMipmaps( PixelFormatGpuUtils::getMaxMipmapCount(
                std::min( mLightVoxel[0]->getWidth(),
                          std::min( mLightVoxel[0]->getHeight(), mLightVoxel[0]->getDepth() ) ) ) );
            directTex->setPixelFormat( jahLightVoxelFormat() );  // Jahshaka (CONTACT-OCCLUSION-1): float, see below
            directTex->scheduleTransitionTo( GpuResidency::Resident );
            mLightDirect = directTex;

            if( mAnisotropic )
            {
                // Jahshaka (PHOTON-VOXEL-5): the BACK side's direct term and the directional level
                // 0's direct part per axis (8-bit sRGB like the direct term: it is normalised to
                // the ceiling by construction), born and buried with the bounce - the bounce's
                // fixed point per side and step 0's per half need them - and step 0 itself.
                texName.clear();
                texName.a( "VctLightingDirectBack/Id", getId() );
                mLightDirectBack = textureManager->createTexture(
                    texName.c_str(), GpuPageOutStrategy::Discard,
                    TextureFlags::Uav, TextureTypes::Type3D );
                mLightDirectBack->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                                 mLightVoxel[0]->getDepth() );
                mLightDirectBack->setNumMipmaps( 1u );
                mLightDirectBack->setPixelFormat( jahLightVoxelFormat() );
                mLightDirectBack->scheduleTransitionTo( GpuResidency::Resident );
                for( uint8 i = 0u; i < 3u; ++i )
                {
                    texName.clear();
                    texName.a( "VctLightingDirectDir", i, "/Id", getId() );
                    TextureGpu *t = textureManager->createTexture(
                        texName.c_str(), GpuPageOutStrategy::Discard,
                        TextureFlags::Uav, TextureTypes::Type3D );
                    t->setResolution( mLightVoxel[1]->getWidth(), mLightVoxel[1]->getHeight(),
                                      mLightVoxel[1]->getDepth() );
                    t->setNumMipmaps( 1u );
                    t->setPixelFormat( jahLightVoxelFormat() );
                    t->scheduleTransitionTo( GpuResidency::Resident );
                    mLightDirectDir[i] = t;
                }

                HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
                HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( "VCT/AnisotropicMipStep0" );
                texName.clear();
                texName.a( "VCT/AnisotropicMipStep0/Id", getId() );
                mAnisoGeneratorStep0 = baseJob->clone( texName.c_str() );
                ShaderParams *shaderParams = &mAnisoGeneratorStep0->getShaderParams( "default" );
                shaderParams->mParams.back().setManualValue(
                    static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) );
                shaderParams->setDirty();
            }
        }
        else
        {
            restoreSwappedTextures();
            textureManager->destroyTexture( mLightBounce );
            mLightBounce = 0;
            if( mLightDirect )
            {
                textureManager->destroyTexture( mLightDirect );  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076)
                mLightDirect = 0;
            }
            if( mLightDirectBack )
            {
                textureManager->destroyTexture( mLightDirectBack );
                mLightDirectBack = 0;
            }

            for( uint8 i = 0u; i < 3u; ++i )
            {
                if( mLightDirectDir[i] )
                {
                    textureManager->destroyTexture( mLightDirectDir[i] );
                    mLightDirectDir[i] = 0;
                }
            }
            if( mAnisoGeneratorStep0 )
            {
                HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
                hlmsCompute->destroyComputeJob( mAnisoGeneratorStep0->getName() );
                mAnisoGeneratorStep0 = 0;
            }
        }

        if( mLightVctBounceInject )
        {
            if( mAnisotropic )
            {
                mLightVctBounceInject->setProperty( "vct_anisotropic", 1 );
                mLightVctBounceInject->setNumTexUnits( 8u );  // + the coverage and the position
            }
            else
            {
                mLightVctBounceInject->setProperty( "vct_anisotropic", 0 );
                mLightVctBounceInject->setNumTexUnits( 5u );  // + the coverage and the position
            }
        }

        if( bAllowMultipleBounces )
            setupBounceTextures();
        else
            setupGlslTextureUnits();
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::getAllowMultipleBounces() const { return mLightBounce != 0; }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setBakingMultiplier( float bakingMult ) { mBakingMultiplier = bakingMult; }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::update( SceneManager *sceneManager, uint32 numBounces, bool autoMultiplier,
                              uint32 _lightMask )
    {
        checkTextures();

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();

        renderSystem->debugAnnotationPush( "VctLighting Update" );

        mLightInjectionJob->setConstBuffer( 0, mLightsConstBuffer );

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mVoxelizer->getAlbedoVox();
        mLightInjectionJob->setTexture( 0, texSlot );
        texSlot.texture = mVoxelizer->getNormalVox();
        mLightInjectionJob->setTexture( 1, texSlot );
        texSlot.texture = mVoxelizer->getEmissiveVox();
        mLightInjectionJob->setTexture( 2, texSlot );
        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage per half-axis at units 3 (+a) and 4
        // (-a) - the shadow march's opacity along the lamp's direction, the half that looks
        // back at it - and the surface position per half at 5 and 6 (its origin plane).
        // The host's cloud field is at unit 7.
        for( uint8 h = 0u; h < 2u; ++h )
        {
            texSlot.texture = mVoxelizer->getCoverageVox( h );
            mLightInjectionJob->setTexture( uint8( 3u + h ), texSlot );
            texSlot.texture = mVoxelizer->getPositionVox( h );
            mLightInjectionJob->setTexture( uint8( 5u + h ), texSlot );
        }

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightVoxel[0];
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
        mLightInjectionJob->_setUavTexture( 0, uavSlot );

        // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): THE SAME DISPATCH WRITES THE DIRECT TERM TWICE -- once
        // into the running total the bounce gathers from, once into the volume the
        // bounce's fixed point needs as its D term. It is one extra image store per
        // voxel inside a job that already walks every light's shadow ray per voxel;
        // no copy, no second dispatch, no encoder switch.
        //
        // RE-ASSERTED PER INJECTION, both the property and the slot:
        // "VCT/LightInjection" is ONE
        // HlmsComputeJob shared by name by every PhotonVoxelLighting in the process, and the
        // state in force belongs to whoever touched it last. A cascade with bounces
        // and a volume without them would otherwise take each other's UAV count --
        // and an unbound u1 on a shader that declares it is a dead descriptor.
        // The count is change-guarded because setNumUavUnits() invalidates the PSO
        // cache hash unconditionally.
        //
        // Jahshaka (PHOTON-VOXEL-5): on the anisotropic tiers the same dispatch writes level 0's
        // BACK side, the back's direct term (with a bounce) and the directional volumes' level 0
        // (the fused step 0: from each voxel's light per half-axis) - into the volumes themselves
        // without a bounce, into the direct part's own volumes with one (step 0 adds the bounce's
        // part after every bounce). The slots follow u0 in LightInjection_cs.glsl's order.
        const uint8 numUavsNeeded =
            uint8( 1u + ( mLightDirect ? 1u : 0u ) + ( mAnisotropic ? ( mLightDirect ? 5u : 4u ) : 0u ) );
        if( mLightInjectionJob->getNumUavUnits() != numUavsNeeded )
            mLightInjectionJob->setNumUavUnits( numUavsNeeded );
        mLightInjectionJob->setProperty( "vct_keep_direct", mLightDirect ? 1 : 0 );
        mLightInjectionJob->setProperty( "vct_anisotropic", mAnisotropic ? 1 : 0 );
        uint8 uavIdx = 1u;
        if( mLightDirect )
        {
            uavSlot.texture = mLightDirect;
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
        }
        if( mAnisotropic )
        {
            uavSlot.texture = mLightVoxel[backIndex()];
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
            if( mLightDirect )
            {
                uavSlot.texture = mLightDirectBack;
                uavSlot.pixelFormat = jahLightVoxelUavFormat();
                mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
            }
            for( uint8 i = 0u; i < 3u; ++i )
            {
                uavSlot.texture = mLightDirect ? mLightDirectDir[i] : mLightVoxel[i + 1u];
                uavSlot.pixelFormat = jahLightVoxelUavFormat();
                mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
            }
            mInjectHigherMipHalfWidth->setManualValue( static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) );
        }

        float autoMultiplierValue = 0.0f;

        const Vector3 voxelOrigin = mVoxelizer->getVoxelOrigin();
        const Vector3 invVoxelRes = 1.0f / mVoxelizer->getVoxelResolution();
        const Vector3 invVoxelSize = 1.0f / mVoxelizer->getVoxelSize();

        PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight = reinterpret_cast<PhotonShaderVoxelLight *>(
            mLightsConstBuffer->map( 0, mLightsConstBuffer->getNumElements() ) );
        uint32 numCollectedLights = 0;
        const uint32 maxNumLights =
            static_cast<uint32>( mLightsConstBuffer->getNumElements() / sizeof( PhotonShaderVoxelLight ) );

        const uint32 lightMask = _lightMask & VisibilityFlags::RESERVED_VISIBILITY_FLAGS;

        ObjectMemoryManager &memoryManager = sceneManager->_getLightMemoryManager();
        const size_t numRenderQueues = memoryManager.getNumRenderQueues();

        for( size_t i = 0; i < numRenderQueues; ++i )
        {
            ObjectData objData;
            const size_t totalObjs = memoryManager.getFirstObjectData( objData, i );

            for( size_t j = 0; j < totalObjs && numCollectedLights < maxNumLights;
                 j += ARRAY_PACKED_REALS )
            {
                for( size_t k = 0; k < ARRAY_PACKED_REALS && numCollectedLights < maxNumLights; ++k )
                {
                    uint32 *RESTRICT_ALIAS visibilityFlags = objData.mVisibilityFlags;

                    if( visibilityFlags[k] & VisibilityFlags::LAYER_VISIBILITY &&
                        visibilityFlags[k] & lightMask )
                    {
                        Light *light = static_cast<Light *>( objData.mOwner[k] );
                        if( light->getType() == Light::LT_DIRECTIONAL ||
                            light->getType() == Light::LT_POINT ||
                            light->getType() == Light::LT_SPOTLIGHT ||
                            light->getType() == Light::LT_AREA_APPROX ||
                            light->getType() == Light::LT_AREA_LTC )
                        {
                            const float maxVal = addLight( vctLight, light, voxelOrigin, invVoxelSize );
                            autoMultiplierValue = std::max( autoMultiplierValue, maxVal );
                            ++vctLight;
                            ++numCollectedLights;
                        }
                    }
                }

                objData.advancePack();
            }
        }

        mLightsConstBuffer->unmap( UO_KEEP_PERSISTENT );

        autoMultiplierValue /= Math::PI;
        // A SCENE WITH NO VISIBLE LIGHTS HAS NOTHING TO NORMALISE AGAINST, and the
        // arithmetic below turned that into "the VCT arm contributes nothing": the
        // maximum radiance collected above is exactly 0, its inverse is +inf, so
        // mInvBakingMultiplier comes out 0 and the shader's
        // blendWeight = blendFade * blend * multiplier is 0 for every voxel. That used
        // to be invisible (no lights, nothing to inject) but it is not any more: a
        // bound PhotonVoxelLighting switches the PBS ambient pieces off inside its volume, so an
        // AMBIENT-LIT scene whose only lamp is switched off goes black the moment VCT is
        // enabled. The fallback for "no measurement available" is the same one
        // autoMultiplier == false asks for -- mBakingMultiplier -- so the zero case is
        // folded into that branch. Note the collection loop above gathers only
        // LAYER_VISIBILITY lights, so a hidden lamp is this case too.
        if( !autoMultiplier || autoMultiplierValue <= 0.0f )
            autoMultiplierValue = mBakingMultiplier;
        else
            autoMultiplierValue = 1.0f / autoMultiplierValue;
        mInvBakingMultiplier = 1.0f / autoMultiplierValue;

        const Vector3 voxelCellSize( mVoxelizer->getVoxelCellSize() );

        mNumLights->setManualValue( numCollectedLights );
        mBakingMultiplierParam->setManualValue( autoMultiplierValue );
        mVoxelCellSize->setManualValue( voxelCellSize );
        mInvVoxelResolution->setManualValue( invVoxelRes );
        mShaderParams->setDirty();

        renderSystem->endCopyEncoder();

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        mLightInjectionJob->analyzeBarriers( mResourceTransitions );
        renderSystem->executeResourceTransition( mResourceTransitions );
        hlmsCompute->dispatch( mLightInjectionJob, 0, 0 );

        if( mAnisotropic )
            generateAnisotropicMips();

        // Jahshaka (MOVER-OCCLUSION-1): D's own chain, read by the ray tier's hits.
        if( mLightDirect && mLightDirect->getNumMipmaps() > 1u )
        {
            renderSystem->debugAnnotationPush( "PhotonVoxelLighting::update direct mipmaps" );
            mLightDirect->_autogenerateMipmaps();
            renderSystem->endCopyEncoder();
            renderSystem->debugAnnotationPop();
        }

        if( mLightVoxel[0]->getNumMipmaps() > 1u )
        {
            renderSystem->debugAnnotationPush( "PhotonVoxelLighting::update regular mipmaps" );
            mLightVoxel[0]->_autogenerateMipmaps();
            renderSystem->endCopyEncoder();
            renderSystem->debugAnnotationPop();
        }

        // Jahshaka (CONTACT-OCCLUSION-1, SKY-BOUNCE-1): THE SKY IS A LIGHT. Its direct term
        // enters the store first - total = direct + rho * E_sky, E_sky the environment
        // through the escape of each voxel's own cones (its sky visibility: a floor under
        // a roof gets none) - and every bounce pass after it is the Jacobi step it always
        // was: new = direct + rho * ( G_voxels( total ) + E_sky ), whose base is the lamps'
        // direct volume, so the sky's term is counted ONCE whatever the pass count, and a
        // sky-lit surface gets as many bounces as a lamp-lit one.
        //
        // Before this, the sky reached the store ONLY through a bounce pass's escaping
        // cones, and a document asking for one bounce runs none: a white wall under the
        // sky reflected NOTHING onto the floor beside it at High, Medium and Low
        // (gi.contact_occlusion's arm B: white minus black exactly 0), and one pass behind
        // at Epic. It is not a double count with a reader's own escape: a MISS reads the
        // sky, a HIT reads the surface - and the surface's radiance includes the sky that
        // lights it. It needs the bounce volumes (setAllowMultipleBounces) and an
        // environment that carries light; a store without either is unchanged.
        const bool skyPass = getAllowMultipleBounces() && hasEnvironmentLight();
        if( skyPass )
            runBounce( true );

        if( numBounces > 0u )
        {
            if( !getAllowMultipleBounces() )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "numBounces must be 0, else call setAllowMultipleBounces first!",
                             "PhotonVoxelLighting::update" );
            }
            for( uint32 i = 0u; i < numBounces; ++i )
                runBounce();
        }

        if( mDebugVoxelVisualizer )
        {
            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );
            // Jahshaka (PHOTON-VIEW-1; an upstream defect): setTrackingVoxel writes the
            // voxel-space box into the WORLD slot too, and the visualizer is STATIC - nothing
            // re-derives it, so the picture was culled wherever the volume is not at the
            // world origin. Re-derived here, as resetTexturesFromBuildRelative does.
            mDebugVoxelVisualizer->getWorldAabbUpdated();
        }

        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::resetTexturesFromBuildRelative()
    {
        if( mDebugVoxelVisualizer )
        {
            Node *visNode = mDebugVoxelVisualizer->getParentNode();
            visNode->setPosition( mVoxelizer->getVoxelOrigin() );
            visNode->setScale( mVoxelizer->getVoxelCellSize() );

            // The visualizer is static so force-update its transform manually
            visNode->_getFullTransformUpdated();
            mDebugVoxelVisualizer->getWorldAabbUpdated();
        }

        if( mVoxelizerTexturesChanged )
        {
            checkTextures();
            return;
        }

        if( getAllowMultipleBounces() )
            setupBounceTextures();
    }
    //-------------------------------------------------------------------------
    size_t PhotonVoxelLighting::getConstBufferSize() const
    {
        // Jahshaka (CONE-FRAME-2): + xformCube_row0..2, three float4s.
        size_t retVal = 11u * 4u * sizeof( float );
        retVal += ( 4u + 4u * 2u ) * sizeof( float ) * mExtraCascades.size();
        return retVal;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::getCascadeChainParams( float *RESTRICT_ALIAS outInvResMaxLod,
                                             float *RESTRICT_ALIAS outFromPrev ) const
    {
        // Moved out of fillConstBufferData VERBATIM (Jahshaka, PHOTON-READER-1): the pass
        // buffer's chain block is exactly these two arrays, back to back, and the
        // irradiance field's generation job reads the same chain through the same march.
        const float finalMultiplier = mInvBakingMultiplier * mMultiplier;

        const size_t numCascades = mExtraCascades.size() + 1u;

        // float4 vctInvResolution_cascadeMaxLod;
        for( size_t i = 0u; i < numCascades; ++i )
        {
            const PhotonVoxelLighting *cascade;
            if( i == 0u )
                cascade = this;
            else
                cascade = mExtraCascades[i - 1u];

            const TextureGpu *cascadeLightVoxel = cascade->mLightVoxel[0];
            const uint32 widthCascade = cascadeLightVoxel->getWidth();
            const uint32 heightCascade = cascadeLightVoxel->getHeight();
            const uint32 depthCascade = cascadeLightVoxel->getDepth();

            uint8 cascadeNumMipmaps = 0u;

            if( cascade->mAnisotropic )
            {
                // Anisotropic has the number of mipmaps calculated
                cascadeNumMipmaps = cascade->mLightVoxel[1]->getNumMipmaps();
            }
            else
            {
                cascadeNumMipmaps = cascadeLightVoxel->getNumMipmaps();
            }

            *outInvResMaxLod++ = 1.0f / static_cast<float>( widthCascade );
            *outInvResMaxLod++ = 1.0f / static_cast<float>( heightCascade );
            *outInvResMaxLod++ = 1.0f / static_cast<float>( depthCascade );
            if( i == numCascades - 1u )
            {
                *outInvResMaxLod++ = 256.0f;  // cascadeMaxLod
            }
            else
            {
                const PhotonVoxelLighting *nextCascade = mExtraCascades[i];

                const Vector3 cascadeVoxelCellSize = cascade->mVoxelizer->getVoxelCellSize();
                const Vector3 nextCascadeVoxelCellSize = nextCascade->mVoxelizer->getVoxelCellSize();

                const Vector3 currToNextFactor = nextCascadeVoxelCellSize / cascadeVoxelCellSize;
                const float maxFactor =
                    std::max( currToNextFactor.x, std::max( currToNextFactor.y, currToNextFactor.z ) );

                *outInvResMaxLod++ =
                    std::min<float>( Math::Log2( maxFactor ), cascadeNumMipmaps );  // cascadeMaxLod
            }
        }

        // float4 fromPreviousProbeToNext[numCascades - 1u][2]
        for( size_t i = 1u; i < numCascades; ++i )
        {
            const PhotonVoxelLighting *cascade = mExtraCascades[i - 1u];
            const PhotonVoxelLighting *prevCascade;
            if( i == 1u )
                prevCascade = this;
            else
                prevCascade = mExtraCascades[i - 2u];

            const Vector3 cascadeVoxelSize = cascade->mVoxelizer->getVoxelSize();
            const Vector3 vScale = prevCascade->mVoxelizer->getVoxelSize() / cascadeVoxelSize;
            const Vector3 vPos =
                ( prevCascade->mVoxelizer->getVoxelOrigin() - cascade->mVoxelizer->getVoxelOrigin() ) /
                cascadeVoxelSize;

            const float cascadeFinalMultiplier =
                cascade->mInvBakingMultiplier * cascade->mMultiplier / finalMultiplier;

            *outFromPrev++ = static_cast<float>( vScale.x );
            *outFromPrev++ = static_cast<float>( vScale.y );
            *outFromPrev++ = static_cast<float>( vScale.z );
            *outFromPrev++ = cascadeFinalMultiplier;

            *outFromPrev++ = static_cast<float>( vPos.x );
            *outFromPrev++ = static_cast<float>( vPos.y );
            *outFromPrev++ = static_cast<float>( vPos.z );
            // Jahshaka (PHOTON-PHYSICS-1, CONE-EMITTER-1): UNUSED. It held upstream's specular
            // "brightness equalisation" slope (1 / mips^3) for a hop weight of
            // mix( 0.5, cascadeFinalMultiplier, lod x slope ) - a near-mirror cone read every
            // cascade past the first at half its radiance. The walk converts a hop's units by
            // cascadeFinalMultiplier alone now (jah_voxel_march.glsl, jahConeMarch).
            *outFromPrev++ = 0.0f;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::fillConstBufferData( const Matrix4 &viewMatrix,
                                           float *RESTRICT_ALIAS passBufferPtr ) const
    {
        const uint32 width = mLightVoxel[0]->getWidth();
        const uint32 height = mLightVoxel[0]->getHeight();
        const uint32 depth = mLightVoxel[0]->getDepth();

        const float smallestRes = static_cast<float>( std::min( std::min( width, height ), depth ) );

        const float maxMipmapCount = static_cast<float>(
            PixelFormatGpuUtils::getMaxMipmapCount( static_cast<uint32>( smallestRes ) ) );

        const float mipDiff = ( maxMipmapCount - 8.0f ) * 0.5f;

        const float finalMultiplier = mInvBakingMultiplier * mMultiplier;

        const size_t numCascades = mExtraCascades.size() + 1u;

        // float4 vctInvResolution_cascadeMaxLod[numCascades];
        // float4 fromPreviousProbeToNext[numCascades - 1u][2]
        // (Jahshaka, PHOTON-READER-1: one definition, shared with the irradiance field.)
        getCascadeChainParams( passBufferPtr, passBufferPtr + 4u * numCascades );
        passBufferPtr += 4u * numCascades + 8u * ( numCascades - 1u );

        // float specSdfMaxMip;
        // float specularSdfFactor;
        // float blendFade;
        // float multiplier;
        *passBufferPtr++ = 7.0f + mipDiff;
        // Where did 0.1875f & 0.3125f come from? Empirically obtained.
        // At 128x128x128, values in range [24; 40] gave good results.
        // Below 24, quality became unnacceptable.
        // Past 40, performance only went down without visible changes.
        // Thus 24 / 128 and 40 / 128 = 0.1875f and 0.3125f
        *passBufferPtr++ = Math::lerp( 0.1875f, 0.3125f, mSpecularSdfQuality ) * smallestRes;
        *passBufferPtr++ = 1.0f;
        *passBufferPtr++ = finalMultiplier;

        Matrix4 xform, invXForm;
        xform.makeTransform( -mVoxelizer->getVoxelOrigin() / mVoxelizer->getVoxelSize(),
                             1.0f / mVoxelizer->getVoxelSize(), Quaternion::IDENTITY );
        // xform = xform * viewMatrix.inverse();
        xform = xform.concatenateAffine( viewMatrix.inverseAffine() );
        invXForm = xform.inverseAffine();

        // float4 xform_row0;
        // float4 xform_row1;
        // float4 xform_row2;
        for( size_t i = 0; i < 12u; ++i )
            *passBufferPtr++ = static_cast<float>( xform[0][i] );

        // float4 invXform_row0;
        // float4 invXform_row1;
        // float4 invXform_row2;
        for( size_t i = 0; i < 12u; ++i )
            *passBufferPtr++ = static_cast<float>( invXForm[0][i] );

        // float4 xformCube_row0;
        // float4 xformCube_row1;
        // float4 xformCube_row2;
        // Jahshaka (CONE-FRAME-2): a DIRECTION in the camera-independent CUBEMAP
        // frame (world with z negated - the frame HlmsPbs's invViewMatCubemap
        // takes a view direction to, where the pixel builds its cone frame) to
        // the probe's normalised space: the voxel volume's per-axis scale after
        // un-flipping z. A pure diagonal, so an axis direction keeps exact zeros
        // in its other two components; the cone frame no longer round-trips
        // cubemap -> view -> probe through two float rotations. (The w column is
        // unused: a direction has no translation.)
        {
            const Vector3 invSize = 1.0f / mVoxelizer->getVoxelSize();
            const float rows[12] = { invSize.x, 0.0f, 0.0f, 0.0f,  //
                                     0.0f, invSize.y, 0.0f, 0.0f,  //
                                     0.0f, 0.0f, -invSize.z, 0.0f };
            for( size_t i = 0; i < 12u; ++i )
                *passBufferPtr++ = rows[i];
        }
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::shouldEnableSpecularSdfQuality() const
    {
        return mVoxelizer->getAlbedoVox()->getWidth() > 32u &&
               mVoxelizer->getAlbedoVox()->getHeight() > 32u &&
               mVoxelizer->getAlbedoVox()->getDepth() > 32u;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setDebugVisualization( bool bShow, SceneManager *sceneManager )
    {
        if( bShow == getDebugVisualizationMode() )
            return;

        if( !bShow )
        {
            SceneNode *sceneNode = mDebugVoxelVisualizer->getParentSceneNode();
            sceneNode->getParentSceneNode()->removeAndDestroyChild( sceneNode );
            OGRE_DELETE mDebugVoxelVisualizer;
            mDebugVoxelVisualizer = 0;
        }
        else
        {
            SceneNode *rootNode = sceneManager->getRootSceneNode( SCENE_STATIC );
            SceneNode *visNode = rootNode->createChildSceneNode( SCENE_STATIC );

            mDebugVoxelVisualizer = OGRE_NEW PhotonVoxelVisualizer(
                Ogre::Id::generateNewId<Ogre::MovableObject>(),
                &sceneManager->_getEntityMemoryManager( SCENE_STATIC ), sceneManager, 0u );

            mDebugVoxelVisualizer->setTrackingVoxel( mLightVoxel[0], mLightVoxel[0], true );

            visNode->setPosition( mVoxelizer->getVoxelOrigin() );
            visNode->setScale( mVoxelizer->getVoxelCellSize() );
            visNode->attachObject( mDebugVoxelVisualizer );

            // Jahshaka (PHOTON-VIEW-1): the node and the box are STATIC - forced current
            // here as resetTexturesFromBuildRelative does, or the first frames cull the
            // picture against the voxel-space box.
            visNode->_getFullTransformUpdated();
            mDebugVoxelVisualizer->getWorldAabbUpdated();
        }
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::getDebugVisualizationMode() const { return mDebugVoxelVisualizer != 0; }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setAnisotropic( bool bAnisotropic )
    {
        if( mAnisotropic != bAnisotropic )
        {
            mAnisotropic = bAnisotropic;
            createTextures();
        }
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setEnvironment( TextureGpu *cube, const ColourValue &gain, const float sh[27] )
    {
        mEnvCube = cube;
        mEnvGain[0] = static_cast<float>( gain.r );
        mEnvGain[1] = static_cast<float>( gain.g );
        mEnvGain[2] = static_cast<float>( gain.b );
        if( sh )
            memcpy( mEnvSh, sh, sizeof( mEnvSh ) );
        else
            memset( mEnvSh, 0, sizeof( mEnvSh ) );
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::hasEnvironmentLight() const
    {
        if( mEnvCube )
            return mEnvGain[0] > 0.0f || mEnvGain[1] > 0.0f || mEnvGain[2] > 0.0f;
        for( size_t i = 0u; i < 27u; ++i )
        {
            if( mEnvSh[i] != 0.0f )
                return true;
        }
        return false;
    }
    //-------------------------------------------------------------------------
    TextureGpu **PhotonVoxelLighting::getLightVoxelTextures( const size_t cascadeIdx )
    {
        if( cascadeIdx == 0u )
            return mLightVoxel;
        else
            return mExtraCascades[cascadeIdx - 1u]->mLightVoxel;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::notifyTextureChanged( TextureGpu *texture, TextureGpuListener::Reason reason,
                                            void *extraData )
    {
        if( reason == TextureGpuListener::LostResidency || reason == TextureGpuListener::Deleted )
            mVoxelizerTexturesChanged = true;

        if( reason == TextureGpuListener::Deleted )
        {
            texture->removeListener( this );
            mVoxelizerListenersRemoved = true;
        }
    }
}  // namespace Ogre
