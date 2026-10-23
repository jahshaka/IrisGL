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
#include "photon/PhotonDispatch.h"
#include "photon/voxel/PhotonMipChain.h"
#include "photon/voxel/PhotonSpecularAlbedoBuffer.h"

#include "photon/voxel/PhotonVoxelizerSourceBase.h"
#include "photon/voxel/PhotonVoxelVisualizer.h"
#include "photon/voxel/PhotonVoxelScroll.h"

#include "SceneLightList.h"

#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLight.h"
#include "OgreLogManager.h"
#include "OgreLwString.h"
#include "OgrePixelFormatGpuUtils.h"
#include "OgreRenderSystem.h"
#include "OgreSceneManager.h"
#include "OgreShaderPrimitives.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreVaoManager.h"
#include "Vao/OgreUavBufferPacked.h"
#include "OgreDescriptorSetUav.h"

#include <map>

namespace Ogre
{
    struct PhotonShaderVoxelLight
    {
        // Pre-mul by PI? -No because we lose a ton of precision
        // .w = the light's RANGE (point/spot; 0 for directional and area), the
        // window of the one falloff (IMAGE-1, JahBrdf's jahLightAttenuation)
        float diffuse[4] = {};
        // For directional lights, pos.xyz contains -dir.xyz and pos.w = 0;
        // For the rest of lights, pos.xyz contains pos.xyz and pos.w = 1;
        float pos[4] = {};
        // uvwPos.w contains the light type
        float uvwPos[4] = {};

        // Used by area lights
        // points[0].w contains double sided info
        // Point/spot: points[0].xyz the spot direction, points[1].xyz the spot
        // parameters, points[2].x the SOURCE RADIUS of the one falloff
        float points[4][4] = {};
    };

    static const IdString NumVctCascadesProp = "hlms_num_vct_cascades";
    /// The source radius's store floor (addLight): the lobes' ceiling 2 x the bounce's gain at r = 0.875.
    static const float kSourceStoreHeadroom = 16.0f;
    static float maxPeak( const ColourValue &c ) { return std::max( std::max( c.r, c.g ), c.b ); }

    namespace
    {
        /// ONE PART OF AN INJECTION, named to the host's listener for as long as it records (the
        /// split rows, PhotonVoxelLighting::PhaseListener).
        struct InjectionPhase
        {
            PhotonVoxelLighting::PhaseListener *listener;
            InjectionPhase( PhotonVoxelLighting::PhaseListener *l, PhotonVoxelLighting::Phase phase ) :
                listener( l )
            {
                if( listener )
                    listener->phaseBegin( phase );
            }
            ~InjectionPhase()
            {
                if( listener )
                    listener->phaseEnd();
            }
            InjectionPhase( const InjectionPhase & ) = delete;
            InjectionPhase &operator=( const InjectionPhase & ) = delete;
        };

        /// THE MIP-STEP JOBS ARE POOLED (BUGS-6 stage 3), the way PhotonMipChain pools its own: one
        /// clone of VCT/AnisotropicMipStep0 or 1 per (aniso volume size, format, step, level), its
        /// shader parameters - the level's resolutions, which depend on nothing else - set once at
        /// its clone, its textures bound per dispatch. Measured before: every lighting cloned its own
        /// (1 + its mips), destroyed with it - 26 pipelines a chain build at the default world, 381 at
        /// a cold boot's startup gate (spikes/bugs-6/REPORT.md).
        struct MipStepKey
        {
            uint32 width = 0u, height = 0u, depth = 0u;
            PixelFormatGpu format = PFG_UNKNOWN;
            uint8 step = 0u, level = 0u;
            bool operator<( const MipStepKey &o ) const
            {
                if( width != o.width ) return width < o.width;
                if( height != o.height ) return height < o.height;
                if( depth != o.depth ) return depth < o.depth;
                if( format != o.format ) return format < o.format;
                if( step != o.step ) return step < o.step;
                return level < o.level;
            }
        };
        HlmsManager *gMipStepPoolManager = nullptr;
        std::map<MipStepKey, HlmsComputeJob *> gMipStepPool;
        unsigned long long gMipStepJobs = 0ull;
        uint32 gMipStepCloneId = 0u;

        /// The pooled job of `key`, cloned from `baseName` on first use (`isNew` says so: the
        /// caller sets its parameters then).
        HlmsComputeJob *pooledMipStep( HlmsManager *hlmsManager, const char *baseName, const MipStepKey &key,
                                       bool &isNew )
        {
            if( gMipStepPoolManager && gMipStepPoolManager != hlmsManager )
                PhotonVoxelLighting::releaseMipStepPool();
            gMipStepPoolManager = hlmsManager;
            isNew = false;
            auto it = gMipStepPool.find( key );
            if( it != gMipStepPool.end() )
                return it->second;
            HlmsCompute *hlmsCompute = hlmsManager->getComputeHlms();
            HlmsComputeJob *baseJob = hlmsCompute->findComputeJob( baseName );
            char tmpBuffer[128];
            LwString name( LwString::FromEmptyPointer( tmpBuffer, sizeof( tmpBuffer ) ) );
            name.a( baseName, "/", ++gMipStepCloneId );
            HlmsComputeJob *job = baseJob->clone( name.c_str() );
            gMipStepPool[key] = job;
            ++gMipStepJobs;
            isNew = true;
            return job;
        }
    }  // namespace

    void PhotonVoxelLighting::releaseMipStepPool()
    {
        if( gMipStepPoolManager )
        {
            HlmsCompute *hlmsCompute = gMipStepPoolManager->getComputeHlms();
            for( auto &kv : gMipStepPool )
                hlmsCompute->destroyComputeJob( kv.second->getName() );
        }
        gMipStepPool.clear();
        gMipStepPoolManager = nullptr;
    }

    unsigned long long PhotonVoxelLighting::numMipStepJobs() { return gMipStepJobs; }

    static const size_t c_maxCascades = 8u;

    // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080): THE FORMAT OF THE TOTAL VOLUME.
    //
    // The volume holds the fixed point of L = D + rho * G( L ). The DIRECT term D
    // is RADIANCE (PHOTON-7 stage 1: no per-box normalisation - a lamp is as bright as it
    // is authored), and the FIXED POINT is not bounded by D at all -- it is D * sum( (rho*f)^i ), i.e. up to
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
    PhotonVoxelLighting::PhotonVoxelLighting( IdType id, PhotonVoxelizerSourceBase *voxelizer,
                                              bool chainOfOne ) :
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
        mChainOfOne( chainOfOne ),
        mNumLights( 0 ),
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
        mLightsPerVoxel = mShaderParams->findParameter( "lightsPerVoxel" );
        mVoxelCellSize = mShaderParams->findParameter( "voxelCellSize" );
        mInvVoxelResolution = mShaderParams->findParameter( "invVoxelResolution" );
        mInjectHigherMipHalfWidth = mShaderParams->findParameter( "higherMipHalfWidth" );
        mInjectChainSelf = mShaderParams->findParameter( "chainSelf" );
        mInjectChainOrigin = mShaderParams->findParameter( "chainOrigin" );
        mInjectChainRes = mShaderParams->findParameter( "chainRes" );
        mInjectKeepLo = mShaderParams->findParameter( "keepLo" );
        mInjectKeepHi = mShaderParams->findParameter( "keepHi" );
        mInjectOrigin = mShaderParams->findParameter( "injectOrigin" );

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();
        // THE LIGHT SLOTS (P1C-LIGHT-LIST): the const buffer and the shader's array
        // (LightInjection_piece_cs's vct_light_budget) hold kLightSlots lights in every
        // cascade (the job is shared, so the array is one size); a cascade's BUDGET is
        // how many of them it fills (lightBudgetFor).
        mLightsConstBuffer = vaoManager->createConstBuffer( sizeof( PhotonShaderVoxelLight ) * kLightSlots,
                                                            BT_DYNAMIC_PERSISTENT, 0, false );
        mLightInjectionJob->setProperty( "vct_light_budget", static_cast<int32>( kLightSlots ) );

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
        mBounceRegionOrigin = addLocalBounceShaderParam( "regionOrigin" );

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

        mCommitTicket.reset();
        if( mCommitCountBuf )
        {
            mVoxelizer->getRenderSystem()->getVaoManager()->destroyUavBuffer( mCommitCountBuf );
            mCommitCountBuf = nullptr;
        }

        mEnergyTicket.reset();
        if( mEnergyBuffer )
        {
            vaoManager->destroyUavBuffer( mEnergyBuffer );
            mEnergyBuffer = nullptr;
        }

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
    void PhotonVoxelLighting::addLight( PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight, Light *light,
                                        const Vector3 &voxelOrigin )
    {
        const ColourValue diffuseColour = light->getDiffuseColour() * light->getPowerScale();
        for( size_t i = 0; i < 3u; ++i )
            vctLight->diffuse[i] = static_cast<float>( diffuseColour[i] );

        // THE ONE FALLOFF (IMAGE-1): a point or spot light's voxel radiance is
        // windowed and inverse-square exactly as the pixel's is — the range here,
        // the source radius in points[2].x below (OgreScene packs them into the
        // light's attenuation as range and "linear"). Directional and area
        // lights carry 0: the shader applies the falloff to points and spots only.
        const Light::LightTypes srcType = light->getType();
        const bool punctual = srcType == Light::LT_POINT || srcType == Light::LT_SPOTLIGHT;
        // An AREA light carries its range too: the injection windows its LTC form
        // factor to it exactly as the pixel windows the area approximation (IMAGE-1).
        const bool ranged = punctual || srcType == Light::LT_AREA_APPROX || srcType == Light::LT_AREA_LTC;
        vctLight->diffuse[3] = ranged ? static_cast<float>( light->getAttenuationRange() ) : 0.0f;

        Light::LightTypes lightType = srcType;
        if( lightType == Light::LT_AREA_APPROX )
            lightType = Light::LT_AREA_LTC;

        Vector4 light4dVec = light->getAs4DVector();
        if( lightType != Light::LT_DIRECTIONAL )
            light4dVec -= Vector4( voxelOrigin, 0.0f );

        for( size_t i = 0; i < 4u; ++i )
            vctLight->pos[i] = static_cast<float>( light4dVec[i] );

        // (uvwPos.xyz was the light in this box's normalised space, which the box-bound march read; PHOTON-7 stage 1's
        // march runs in metres through the chain and reads pos. Only the type is left in it.)
        for( size_t i = 0; i < 3u; ++i )
            vctLight->uvwPos[i] = 0.0f;
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
            // THE SOURCE RADIUS (PHOTON-7 stage 1): the store holds radiance (k = 1) in RGBA16F, and a point or spot
            // light's peak irradiance is I / rSrc^2 (JahBrdf's falloff) - a 1 mm source of 8 is 8e6, past the
            // format's 65504: +inf in the store, spread by every pass and mip. So the voxel list holds a source at no
            // less than its STORE FLOOR rMin = sqrt( 16 I / 65504 ), I the peak intensity (D / pi): the lobes' ceiling
            // (a surface re-emits at most its diffuse plus its specular albedo, 2) times the bounce's gain at r = 0.875
            // (8; the measured cascades reach 0.22 and the sealed room 0.50) - 0.0156 sqrt( I ) m, 4.4 cm for the
            // default lamp of 8 (whose default 0.1 m radius is above it; the document's 1 mm floor is not). Only the
            // voxel store's near field of such a lamp changes; the pixel's own light is the lamp's as authored.
            if( punctual )
            {
                const float peakI = static_cast<float>( maxPeak( diffuseColour ) ) / Math::PI;
                const float rMin = std::sqrt( kSourceStoreHeadroom * std::max( peakI, 0.0f ) / 65504.0f );
                rectPoints[2].x = std::max( static_cast<float>( light->getAttenuationLinear() ), rMin );
            }
        }

        const float isDoubleSided = light->getDoubleSided() ? 1.0f : 0.0f;
        for( size_t i = 0; i < 4u; ++i )
        {
            for( size_t j = 0; j < 3u; ++j )
                vctLight->points[i][j] = rectPoints[i][j];
            vctLight->points[i][3u] = isDoubleSided;
        }
        // (points[1..3].w held the area light's straight direction, which only the deleted march blend read.)
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
        // and the mip chain's linear filter is then invalid on an integer image (the
        // format has no linear-filter feature), so the coarse mips the cone gather reads
        // would be undefined. The direct volumes are float too since CONTACT-OCCLUSION-1
        // and carry no Reinterpretable either (their UAV views are their formats).
        // Jahshaka (ENGINE-UPGRADE-ASYNC-3 stage 1): a UAV and nothing more - the chains are
        // written by compute (PhotonMipChain), never blitted.
        const uint32 texFlags = TextureFlags::Uav;

        const bool bSdfQuality = shouldEnableSpecularSdfQuality();

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
        // THE TOTAL IS LEVEL 0 ONLY (PHOTON-MARCH-1): every reader takes level 0 from it (the mean
        // of a voxel's two sides, with the back beside it) and every coarser level from the
        // directional volumes - the light per facing half-axis, the colour law at every tier. Its
        // mips exist only under the specular SDF quality: the empty-space skip
        // (jah_voxel_march.glsl, JAH_MARCH_SDF) reads its coarse opacity, nothing else does,
        // and it runs on a one-cascade chain only (shouldEnableSpecularSdfQuality) - a shipped
        // chain of 2-4 cascades allocated the chain and regenerated it after every injection
        // and every bounce for no reader.
        const uint8 numMipsMain = bSdfQuality ? PixelFormatGpuUtils::getMaxMipmapCount( shortest ) : 1u;
        const uint8 numMipsAniso = PixelFormatGpuUtils::getMaxMipmapCount( std::max( 1u, shortest >> 1u ) );

        const size_t numTextures = 4u;

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
        }
        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage and the surface position per
        // half-axis, the list's last four entries (the voxelizer's, not owned -
        // destroyTextures() leaves them alone).
        for( uint32 h = 0u; h < 2u; ++h )
        {
            mLightVoxel[coverageIndex( h )] = mVoxelizer->getCoverageVox( h );
            mLightVoxel[positionIndex( h )] = mVoxelizer->getPositionVox( h );
        }

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
            ShaderParams *shaderParams = 0;
            ShaderParams::Param *lowerMipResolutionParam = 0;

            // Step 1: the directional mips from level 0 (which the light injection composites
            // itself - the fused step 0 - and step 0 completes with a bounce's part).
            const uint8 numMipsOnStep1 = mLightVoxel[1]->getNumMipmaps() - 1u;
            mAnisoGeneratorStep1.resize( numMipsOnStep1 );

            for( uint8 i = 0; i < numMipsOnStep1; ++i )
            {
                // POOLED (BUGS-6 stage 3): the job of this size and level, its textures bound per
                // dispatch (generateAnisotropicMips); its parameters below set at its clone only.
                MipStepKey key;
                key.width = mLightVoxel[1]->getWidth();
                key.height = mLightVoxel[1]->getHeight();
                key.depth = mLightVoxel[1]->getDepth();
                key.format = jahLightVoxelUavFormat();
                key.step = 1u;
                key.level = uint8( i + 1u );
                bool isNew = false;
                HlmsComputeJob *mipJob = pooledMipStep( mVoxelizer->getHlmsManager(), "VCT/AnisotropicMipStep1",
                                                        key, isNew );
                mAnisoGeneratorStep1[i] = mipJob;
                if( !isNew )
                    continue;

                shaderParams = &mipJob->getShaderParams( "default" );
                // higherMipHalfRes_lowerMipHalfWidth (by name: the region origin follows it, PHOTON-7 stage 2)
                lowerMipResolutionParam = shaderParams->findParameter( "higherMipHalfRes_lowerMipHalfWidth" );
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

        // Jahshaka (PHOTON-VOXEL-3/-4): the coverage and position entries are the voxelizer's,
        // and (PHOTON-VOXEL-5) so is the normal's.
        for( uint32 h = 0u; h < 2u; ++h )
        {
            mLightVoxel[coverageIndex( h )] = 0;
            mLightVoxel[positionIndex( h )] = 0;
        }
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

        // The mip-step jobs are the pool's (BUGS-6 stage 3): let go of, never destroyed.
        mAnisoGeneratorStep0 = 0;
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
        uint8 numNeededTexUnits = 10u + 8u * static_cast<uint8>( numExtraCascades ) + 1u;
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
        // Jahshaka (PHOTON-VOXEL-5): +1 for the BACK side's direct term, right after the emissive
        // volume (before the environment cube).
        ++numNeededTexUnits;
        // Jahshaka (METAL-BOUNCE-1): +1 for the voxeliser's METAL volume, right after the back side's
        // direct term (before the environment cube).
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
        texSlot.texture = mLightDirectBack;
        mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        // Jahshaka (METAL-BOUNCE-1): the metal (a Load3D, no sampler) and its lobe's integrals.
        texSlot.texture = mVoxelizer->getMetalVox();
        mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        // The engine's one table (PhotonSpecularAlbedoBuffer.h), never a copy of it.
        mLightVctBounceInject->setConstBuffer(
            0, PhotonSpecularAlbedo::constBuffer( mVoxelizer->getRenderSystem()->getVaoManager() ) );

        // Jahshaka (PHOTON-ENV-1): the environment cube, sampled with the probes'
        // trilinear sampler (a separate sampler object on Vulkan).
        {
            const int32 envOn = mEnvCube ? 1 : 0;
            if( mLightVctBounceInject->getProperty( "jah_env" ) != envOn )
                mLightVctBounceInject->setProperty( "jah_env", envOn );
        }
        if( mEnvCube )
        {
            texSlot.texture = mEnvCube;
            mLightVctBounceInject->setTexture( texSlotIdx++, texSlot, 0, false );
        }

        // Jahshaka (PHOTON-VOXEL-5): the back side's total, u1 (every tier, PHOTON-MARCH-1), written in
        // place: the march's level-0 read in this job takes the sides' MEAN (it binds no back), so
        // nothing here reads it. Measured: a back that ping-pongs and is read per side moved the
        // roof fixture's two-bounce underside by nothing (gi.voxel_coverage, 0.02193 both), at
        // 8 B a voxel on every bouncing tier.
        if( mLightVctBounceInject->getNumUavUnits() != 2u )
            mLightVctBounceInject->setNumUavUnits( 2u );

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = ResourceAccess::Write;
        uavSlot.texture = mLightBounce;
        uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
        mLightVctBounceInject->_setUavTexture( 0, uavSlot );
        uavSlot.texture = mLightVoxel[backIndex()];
        mLightVctBounceInject->_setUavTexture( 1, uavSlot );
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setupGlslTextureUnits()
    {
        const size_t numExtraCascades = mExtraCascades.size();
        // THE LIST'S LENGTH IS PARAMETERS, NOT UNITS (Jahshaka, PHOTON-VOXEL-5; the VOXEL-4
        // audit's F1): the two fixed textures, then ONE array parameter per texture variable
        // (vctProbes, the three axes, the four split kinds - each an array over
        // every cascade), then `directVoxel` (JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076)) and voxelEmissiveTex
        // (PHOTON-WRITER-1). It was compared with a unit count (8 + 6e + 2 / 5 + 3e + 2), which the
        // list never had, so the "glsl" list was rebuilt and set dirty on every bounce dispatch.
        const size_t numTextureVariables = 8u;
        const size_t numNeededParams = 2u + numTextureVariables + 4u;

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
                // the total (level 0), the three axes, then the split kinds in order
                const size_t numLead = 4u;
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
            param.name = "directBackVoxel";
            param.setManualValue( texSlotIdx + 2 );
            glslShaderParams.mParams.push_back( param );
            // Jahshaka (METAL-BOUNCE-1): the metal, right after.
            param.name = "voxelMetalTex";
            param.setManualValue( texSlotIdx + 3 );
            glslShaderParams.mParams.push_back( param );

            glslShaderParams.setDirty();
        }
    }
    //-------------------------------------------------------------------------
    namespace
    {
        /// THE PASS'S GRID (PHOTON-7 stage 2): the texels [lo, hi) of a level whose texel covers `cellsPerTexel` volume
        /// cells - [regionLo, regionHi) of level 0 rounded OUT (every texel a changed cell reaches), or the whole level
        /// `levelRes` - its first texel as the job's regionOrigin and its 4^3 groups set by hand (the jobs are shared:
        /// the count is asserted per dispatch).
        void setRegionGrid( HlmsComputeJob *job, ShaderParams::Param *origin, const uint32 *regionLo,
                            const uint32 *regionHi, uint32 cellsPerTexel, const uint32 levelRes[3], uint32 w = 0u )
        {
            uint32 lo[4] = { 0u, 0u, 0u, w }, n[3];
            for( int a = 0; a < 3; ++a )
            {
                uint32 l = 0u, h = levelRes[a];
                if( regionLo && regionHi )
                {
                    l = std::min( regionLo[a] / cellsPerTexel, levelRes[a] );
                    h = std::min( ( regionHi[a] + cellsPerTexel - 1u ) / cellsPerTexel, levelRes[a] );
                }
                lo[a] = l;
                n[a] = std::max( h, l + 1u ) - l;
            }
            const uint32 *tpg = job->getThreadsPerGroup();
            job->setNumThreadGroupsBasedOn( HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u );
            job->setNumThreadGroups( ( n[0] + tpg[0] - 1u ) / tpg[0], ( n[1] + tpg[1] - 1u ) / tpg[1],
                                     ( n[2] + tpg[2] - 1u ) / tpg[2] );
            origin->setManualValue( lo, 4u );
        }
        ShaderParams::Param *regionOriginParam( HlmsComputeJob *job )
        {
            ShaderParams &params = job->getShaderParams( "default" );
            ShaderParams::Param *p = params.findParameter( "regionOrigin" );
            OGRE_ASSERT_LOW( p && "the job's regionOrigin parameter (Voxelizer.material.json)" );
            return p;
        }
    }  // namespace

    void PhotonVoxelLighting::generateAnisotropicMips( const uint32 *regionLo, const uint32 *regionHi )
    {
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        renderSystem->debugAnnotationPush( "VctLighting Anisotropic Mips" );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        // THE DIRECTIONAL VOLUMES' HALF RESOLUTION per level: x packs both signs (half the width each).
        const uint32 dirRes[3] = { mLightVoxel[1]->getWidth() >> 1u, mLightVoxel[1]->getHeight(),
                                   mLightVoxel[1]->getDepth() };

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
            // (one thread per directional level-0 texel of a half: two cells)
            ShaderParams::Param *origin = regionOriginParam( mAnisoGeneratorStep0 );
            setRegionGrid( mAnisoGeneratorStep0, origin, regionLo, regionHi, 2u, dirRes );
            mAnisoGeneratorStep0->getShaderParams( "default" ).setDirty();
            jahshaka::engine::detail::photonDispatch( hlmsCompute, mAnisoGeneratorStep0, renderSystem );
        }

        // STEP 1'S JOBS ARE THE POOL'S (BUGS-6 stage 3): this lighting's volumes bound per dispatch,
        // level i read through the texture slots and level i + 1 written through the UAVs.
        for( size_t i = 0u; i < mAnisoGeneratorStep1.size(); ++i )
        {
            HlmsComputeJob *mipJob = mAnisoGeneratorStep1[i];
            DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
            DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
            for( uint8 axis = 0; axis < 3u; ++axis )
            {
                texSlot.texture = mLightVoxel[axis + 1u];
                texSlot.mipmapLevel = uint8( i );
                texSlot.numMipmaps = 1u;
                mipJob->setTexture( axis, texSlot );

                uavSlot.access = ResourceAccess::Write;
                uavSlot.texture = mLightVoxel[axis + 1u];
                uavSlot.mipmapLevel = uint8( i + 1u );
                uavSlot.pixelFormat = jahLightVoxelUavFormat();  // JAHSHAKA fork ae2ed529f+155a56bf8 (was 0080)
                mipJob->_setUavTexture( axis, uavSlot );
            }
            // (one thread per texel of a half of level i + 1: 2^(i + 2) cells)
            const uint32 levelRes[3] = { std::max( 1u, dirRes[0] >> ( i + 1u ) ), std::max( 1u, dirRes[1] >> ( i + 1u ) ),
                                         std::max( 1u, dirRes[2] >> ( i + 1u ) ) };
            ShaderParams::Param *origin = regionOriginParam( mipJob );
            setRegionGrid( mipJob, origin, regionLo, regionHi, 2u << ( i + 1u ), levelRes );
            mipJob->getShaderParams( "default" ).setDirty();
            jahshaka::engine::detail::photonDispatch( hlmsCompute, mipJob, renderSystem );
        }
        renderSystem->debugAnnotationPop();
    }
    //-------------------------------------------------------------------------
    // Jahshaka (PHOTON-5 stage 5): one sum of the current light volume (level 0) into range `slot`
    // of the partials buffer, one float per 8 x 8 x 8 block (PhotonVoxelEnergy_cs.glsl).
    void PhotonVoxelLighting::dispatchEnergy( uint32 slot )
    {
        TextureGpu *volume = mLightVoxel[0];
        if( !volume )
            return;
        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();
        VaoManager *vaoManager = renderSystem->getVaoManager();
        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        if( !mEnergyJob )
            mEnergyJob = hlmsCompute->findComputeJob( "Photon/VoxelEnergy" );
        const uint32 gx = ( volume->getWidth() + 7u ) / 8u;
        const uint32 gy = ( volume->getHeight() + 7u ) / 8u;
        const uint32 gz = ( volume->getDepth() + 7u ) / 8u;
        const uint32 groups = gx * gy * gz;
        const uint32 alignFloats = std::max( vaoManager->getUavBufferAlignment(), 16u ) / 4u;
        const uint32 stride = ( groups + alignFloats - 1u ) / alignFloats * alignFloats;
        // kMaxBouncePasses (8) + the base: nine ranges.
        // The group count is kept whatever the stride did: two counts can share one aligned stride (the merge read).
        mEnergyGroups = groups;
        if( !mEnergyBuffer || stride != mEnergyStride )
        {
            mEnergyTicket.reset();
            if( mEnergyBuffer )
                vaoManager->destroyUavBuffer( mEnergyBuffer );
            mEnergyStride = stride;
            std::vector<float> zeros( size_t( stride ) * 9u, 0.0f );
            mEnergyBuffer = vaoManager->createUavBuffer( zeros.size(), sizeof( float ), 0, zeros.data(),
                                                         false );
        }
        if( slot >= 9u )
            return;
        mEnergyJob->setNumThreadGroups( gx, gy, gz );
        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = volume;
        mEnergyJob->setTexture( 0, texSlot, mSamplerblockTrilinear );
        DescriptorSetUav::BufferSlot bufSlot = DescriptorSetUav::BufferSlot::makeEmpty();
        bufSlot.buffer = mEnergyBuffer;
        bufSlot.offset = size_t( slot ) * stride * sizeof( float );
        bufSlot.sizeBytes = size_t( groups ) * sizeof( float );
        bufSlot.access = ResourceAccess::Write;
        mEnergyJob->_setUavBuffer( 0, bufSlot );
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mEnergyJob, renderSystem );
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::takeBounceEnergy( std::vector<double> &sums )
    {
        sums.clear();
        if( !mEnergyTicket.get() )
            return false;
        const float *f = static_cast<const float *>( mEnergyTicket->map() );
        for( uint32 k = 0u; k < mEnergySums; ++k )
        {
            double s = 0.0;
            for( uint32 g = 0u; g < mEnergyGroups; ++g )
                s += double( f[size_t( k ) * mEnergyStride + g] );
            sums.push_back( s );
        }
        mEnergyTicket->unmap();
        mEnergyTicket.reset();
        return true;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::runBounce( bool envOnly, const uint32 *regionLo, const uint32 *regionHi )
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
        // The bounce adds `albedo * G` where G is the four-cone weighted mean of the
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
        // THE ENVIRONMENT, in radiance (Jahshaka, PHOTON-ENV-1): the voxels hold radiance (PHOTON-7 stage 1: no
        // per-box normalisation to divide the sky by), so an escaping bounce cone adds the sky as it is.
        {
            const float mips = mEnvCube ? float( mEnvCube->getNumMipmaps() ) : 1.0f;
            const float gainMips[4] = { mEnvGain[0], mEnvGain[1], mEnvGain[2], mips };
            mBounceEnvGainMips->setManualValue( gainMips, 4u );
            float sh[36];
            for( size_t i = 0u; i < 9u; ++i )
            {
                for( size_t c = 0u; c < 3u; ++c )
                    sh[i * 4u + c] = mEnvSh[i * 3u + c];
                sh[i * 4u + 3u] = 0.0f;
            }
            mBounceEnvSh->setManualValue( sh, 36u );
        }

        mBounceShaderParams->mParams.swap( mLocalBounceShaderParams );
        mBounceShaderParams->setDirty();

        // THE BINDINGS ARE RE-ASSERTED PER DISPATCH: "VCT/LightVctBounceInject" is found by name, so every
        // PhotonVoxelLighting in the process uses ONE HlmsComputeJob while the bindings belong to whichever
        // instance called setupBounceTextures() last - two live chains would inject one chain's voxels through
        // the other's textures. Re-asserting here makes the bindings belong to the instance DISPATCHING. (The
        // textures no longer move under the job: PHOTON-7 stage 2 replaced the ping-pong swap with a copy-back,
        // so the total, the bounce target and every cascade's volumes keep their identities.) The cost is the
        // slot writes themselves: the unit count is change-guarded, the GLSL unit list is rewritten (cheap, and
        // OpenGL-only), and `false` skips the OpenGL-only samplerblock reference counting.
        setupBounceTextures( false );

        // THE PASS'S CELLS (PHOTON-7 stage 2): the region [regionLo, regionHi) (aligned to the 4^3 groups), or whole.
        const uint32 res[3] = { mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(), mLightVoxel[0]->getDepth() };
        setRegionGrid( mLightVctBounceInject, mBounceRegionOrigin, regionLo, regionHi, 1u, res );

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mLightVctBounceInject, renderSystem );

        // THE COPY-BACK: what the pass wrote into the bounce target becomes the total over the same cells (the
        // Jacobi pass read the total and wrote the target; the total's identity never changes - no reader rebinds).
        {
            uint32 lo[3], size[3];
            for( int a = 0; a < 3; ++a )
            {
                lo[a] = regionLo && regionHi ? std::min( regionLo[a], res[a] ) : 0u;
                const uint32 hi = regionLo && regionHi ? std::min( regionHi[a], res[a] ) : res[a];
                size[a] = hi > lo[a] ? hi - lo[a] : 0u;
            }
            PhotonVoxelScroll::copyBox( mVoxelizer->getHlmsManager(), renderSystem, mLightBounce, mLightVoxel[0], lo,
                                        lo, size );
        }

        generateAnisotropicMips( regionLo, regionHi );

        // (the total's levels exist under the specular SDF quality only: otherwise a no-op)
        PhotonMipChain::generate( mVoxelizer->getHlmsManager(), renderSystem, mLightVoxel[0] );

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
            const uint32 texFlags = TextureFlags::Uav;

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

            // Jahshaka (CONTACT-OCCLUSION-1): THE DIRECT STORES ARE FLOAT, like the total: D is
            // radiance (PHOTON-7 stage 1), above 1 wherever a lamp or an emissive surface is
            // brighter than that - an 8-bit store clipped it, and every bounce pass rebuilt the
            // total from the clipped D. The sky pass runs at every
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
                // UAV view IS its format, and a mutable-format image has no linear filter for
                // its mip chain (PhotonMipChain samples the level below).
                TextureFlags::Uav, TextureTypes::Type3D );
            directTex->setResolution( mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                      mLightVoxel[0]->getDepth() );
            directTex->setNumMipmaps( PixelFormatGpuUtils::getMaxMipmapCount(
                std::min( mLightVoxel[0]->getWidth(),
                          std::min( mLightVoxel[0]->getHeight(), mLightVoxel[0]->getDepth() ) ) ) );
            directTex->setPixelFormat( jahLightVoxelFormat() );  // Jahshaka (CONTACT-OCCLUSION-1): float, see below
            directTex->scheduleTransitionTo( GpuResidency::Resident );
            mLightDirect = directTex;

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

            // POOLED (BUGS-6 stage 3): the step-0 job of this size, its textures bound per dispatch.
            MipStepKey key;
            key.width = mLightVoxel[1]->getWidth();
            key.height = mLightVoxel[1]->getHeight();
            key.depth = mLightVoxel[1]->getDepth();
            key.format = jahLightVoxelUavFormat();
            key.step = 0u;
            bool isNew = false;
            mAnisoGeneratorStep0 = pooledMipStep( mVoxelizer->getHlmsManager(), "VCT/AnisotropicMipStep0", key,
                                                  isNew );
            // THE COMMIT'S COUNTER (PHOTON-7 stage 2, the work law's door): four words, cumulative, created zeroed;
            // read back by a buffer read request (a routed copy: legal inside the GI's compute region).
            if( !mCommitCountBuf )
            {
                const uint32 zeros[4] = { 0u, 0u, 0u, 0u };
                mCommitCountBuf = mVoxelizer->getRenderSystem()->getVaoManager()->createUavBuffer(
                    4u, sizeof( uint32 ), 0, const_cast<uint32 *>( zeros ), false );
                mCommitTicket.reset();
                mCommitDownloadOwed = false;
                memset( mCommitSeen, 0, sizeof( mCommitSeen ) );
            }
            if( isNew )
            {
                ShaderParams *shaderParams = &mAnisoGeneratorStep0->getShaderParams( "default" );
                shaderParams->findParameter( "higherMipHalfWidth" )->setManualValue(
                    static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) );
                shaderParams->setDirty();
            }
        }
        else
        {
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
            mAnisoGeneratorStep0 = 0;   // the pool's (BUGS-6 stage 3): let go of, never destroyed
            mCommitTicket.reset();
            mCommitDownloadOwed = false;
            if( mCommitCountBuf )
            {
                mVoxelizer->getRenderSystem()->getVaoManager()->destroyUavBuffer( mCommitCountBuf );
                mCommitCountBuf = nullptr;
            }
        }

        if( mLightVctBounceInject )
        {
            mLightVctBounceInject->setNumTexUnits( 8u );  // + the coverage and the position
        }

        if( bAllowMultipleBounces )
            setupBounceTextures();
        else
            setupGlslTextureUnits();
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::getAllowMultipleBounces() const { return mLightBounce != 0; }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::update( SceneManager *sceneManager, uint32 numBounces, uint32 _lightMask )
    {
        checkTextures();

        RenderSystem *renderSystem = mVoxelizer->getRenderSystem();

        renderSystem->debugAnnotationPush( "VctLighting Update" );

        mLightInjectionJob->setConstBuffer( 0, mLightsConstBuffer );
        // METAL-BOUNCE-1: the lobe's integrals, the engine's one table (PhotonSpecularAlbedoBuffer.h).
        mLightInjectionJob->setConstBuffer( 1, PhotonSpecularAlbedo::constBuffer( renderSystem->getVaoManager() ) );

        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        texSlot.texture = mVoxelizer->getAlbedoVox();
        mLightInjectionJob->setTexture( 0, texSlot );
        texSlot.texture = mVoxelizer->getNormalVox();
        mLightInjectionJob->setTexture( 1, texSlot );
        texSlot.texture = mVoxelizer->getEmissiveVox();
        mLightInjectionJob->setTexture( 2, texSlot );
        // (METAL-BOUNCE-1) The metal at unit 3; (JUNCTION-NORMAL-1) the emissive factor per half at 4 (+a) and 5 (-a).
        texSlot.texture = mVoxelizer->getMetalVox();
        mLightInjectionJob->setTexture( 3u, texSlot );
        for( uint8 h = 0u; h < 2u; ++h )
        {
            texSlot.texture = mVoxelizer->getEmitVox( h );
            mLightInjectionJob->setTexture( uint8( 4u + h ), texSlot );
        }
        // Jahshaka (PHOTON-7 stage 1, L1: LIGHT IS A WORLD FUNCTION): THE CHAIN'S GEOMETRY - every cascade's coverage per
        // half-axis (+a, -a) and surface position per half, four arrays of the chain's length from unit 6
        // (LightInjection_cs.glsl): the shadow march runs from this cascade's entry through every cascade outside it on
        // fixed shells. The entries inside this cascade are bound to its own volumes (never read: one permutation per
        // chain length, the job's unit count fixed for every cascade of a chain). The property and the unit count are
        // change-guarded - setNumTexUnits voids the PSO hash whatever it is told; the cloud field (OgreScene::
        // bindCloudInjection, run before this) holds the unit after the chain when it is on.
        const uint32 chainLength = getChainLength();
        {
            const int32 n = static_cast<int32>( chainLength );
            if( mLightInjectionJob->getProperty( NumVctCascadesProp ) != n )
                mLightInjectionJob->setProperty( NumVctCascadesProp, n );
            const uint8 units = static_cast<uint8>(
                injectionTexUnits() + ( mLightInjectionJob->getProperty( "jah_cloud_shadow" ) != 0 ? 1u : 0u ) );
            if( mLightInjectionJob->getNumTexUnits() != units )
                mLightInjectionJob->setNumTexUnits( units );
        }
        float chainOrigin[4u * c_maxCascades] = {};
        float chainRes[4u * c_maxCascades] = {};
        {
            OGRE_ASSERT_LOW( chainLength <= c_maxCascades );
            const Vector3 selfOrigin = mVoxelizer->getVoxelOrigin();
            // THE SHELLS: D_j, the sum of 8 cells of every cascade of the WORLD's chain up to j, from the march's start -
            // the chain's own cells (setChainIndex), never this cascade's for the entries inside it (stage 1's read F1:
            // Epic c1 marched 2.5 / 6.25 m for 1.875 / 5.625, c2 11.25 for 5.625).
            OGRE_ASSERT_LOW( mChainCells.size() == chainLength );
            double shellEnd = 0.0;
            mLastShellEnds.clear();
            for( uint32 j = 0u; j < chainLength && j < c_maxCascades; ++j )
            {
                const PhotonVoxelLighting *cj =
                    j <= mChainIndex ? this : mExtraCascades[size_t( j - mChainIndex - 1u )];
                const Vector3 cell = cj->mVoxelizer->getVoxelCellSize();
                const Vector3 res = cj->mVoxelizer->getVoxelResolution();
                // (an inner entry is this cascade's own: bound and described, never marched)
                const Vector3 rel = cj->mVoxelizer->getVoxelOrigin() - selfOrigin;
                chainOrigin[4u * j + 0u] = static_cast<float>( rel.x );
                chainOrigin[4u * j + 1u] = static_cast<float>( rel.y );
                chainOrigin[4u * j + 2u] = static_cast<float>( rel.z );
                chainOrigin[4u * j + 3u] = static_cast<float>( cell.x );
                chainRes[4u * j + 0u] = static_cast<float>( res.x );
                chainRes[4u * j + 1u] = static_cast<float>( res.y );
                chainRes[4u * j + 2u] = static_cast<float>( res.z );
                shellEnd += 8.0 * double( j < mChainCells.size() ? mChainCells[j] : cell.x );
                chainRes[4u * j + 3u] = j + 1u < chainLength ? static_cast<float>( shellEnd ) : 1e30f;
                if( j + 1u < chainLength )
                    mLastShellEnds.push_back( static_cast<float>( shellEnd ) );
                for( uint32 k = 0u; k < 4u; ++k )
                {
                    const uint32 h = k & 1u;
                    texSlot.texture = k >= 2u ? cj->mVoxelizer->getPositionVox( h ) : cj->mVoxelizer->getCoverageVox( h );
                    mLightInjectionJob->setTexture( static_cast<uint8>( 6u + k * chainLength + j ), texSlot );
                }
            }
        }
        mInjectChainSelf->setManualValue( mChainIndex );
        mInjectChainOrigin->setManualValue( chainOrigin, 4u * chainLength );
        mInjectChainRes->setManualValue( chainRes, 4u * chainLength );

        const bool delta = mCommit.on && mCommit.light.delta && mLightDirect && mCommitCountBuf;
        bindInjectionUavs( delta );
        {
            int32 keepLo[4] = { 0, 0, 0, 0 }, keepHi[4] = { 0, 0, 0, 0 };
            if( delta )
            {
                for( int a = 0; a < 3; ++a )
                {
                    keepLo[a] = int32( mCommit.light.keepLo[a] );
                    keepHi[a] = int32( mCommit.light.keepHi[a] );
                }
            }
            mInjectKeepLo->setManualValue( keepLo, 4u );
            mInjectKeepHi->setManualValue( keepHi, 4u );
        }
        mInjectHigherMipHalfWidth->setManualValue( static_cast<int32>( mLightVoxel[1]->getWidth() >> 1u ) );

        const Vector3 voxelOrigin = mVoxelizer->getVoxelOrigin();
        const Vector3 invVoxelRes = 1.0f / mVoxelizer->getVoxelResolution();
        const Vector3 cascadeSize = mVoxelizer->getVoxelSize();

        PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight = reinterpret_cast<PhotonShaderVoxelLight *>(
            mLightsConstBuffer->map( 0, mLightsConstBuffer->getNumElements() ) );
        // THE CASCADE'S LIGHTS (P1C-LIGHT-LIST; PHOTON-7 stage 1, L1): the scene's one light list (SceneLightList.h)
        // range-culled against this cascade's box - a point, spot or area light whose range sphere misses it lights
        // none of its voxels (the falloff is zero past the range), so it takes no slot and costs no march - IN THE
        // LIST'S ONE ORDER (peak intensity x range^2, directional first, then the id), up to the kLightSlots the buffer
        // holds; and each VOXEL is lit by the FIRST mLightBudget of them that reach IT (LightInjection's per-voxel
        // count: lightBudgetFor, 16 at 128^3, 128 at 64^3), so which lights a voxel holds is a function of the world
        // point, never of where the box stands (the per-box rank by the irradiance delivered to the box is deleted).
        // The lights over the slots are counted (getLastLightsOverBudget, GiStatus) and logged, never dropped silently.
        jahshaka::engine::SceneLightList::Consumer consumer;
        consumer.boxMin = voxelOrigin;
        consumer.boxMax = voxelOrigin + cascadeSize;
        const Vector3 res = mVoxelizer->getVoxelResolution();
        mLightBudget = lightBudgetFor( static_cast<uint32>( res.x * res.y * res.z ) );
        consumer.budget = kLightSlots;
        consumer.lightMask = _lightMask;
        jahshaka::engine::sceneLightListOf( sceneManager ).select( sceneManager, consumer, mSelection );

        uint32 numCollectedLights = 0;
        for( Light *light : mSelection.lights )
        {
            addLight( vctLight, light, voxelOrigin );
            ++vctLight;
            ++numCollectedLights;
        }
        mLastLightsInRange = mSelection.inRange;
        mLastLightsInjected = numCollectedLights;
        mLastLightsOverBudget = mSelection.overBudget;
        mLightsInRangeMax = std::max( mLightsInRangeMax, mSelection.inRange );
        if( mSelection.inRange > kLightSlots )
            ++mInjectionsOverSlots;   // THE BOX OVER ITS LIGHT SLOTS (the read's F4): counted, never silent
        if( mSelection.overBudget && !mOverBudgetLogged )
        {
            mOverBudgetLogged = true;
            LogManager::getSingleton().logMessage(
                "Jahshaka GI: " + StringConverter::toString( mSelection.overBudget ) + " of the " +
                StringConverter::toString( mSelection.inRange ) +
                " lights in range of a voxel cascade are over its " +
                StringConverter::toString( kLightSlots ) +
                " light slots (the last of the scene's light order; GiStatus::cascades[].lightsOverBudget)" );
        }

        mLightsConstBuffer->unmap( UO_KEEP_PERSISTENT );

        // THE STORE HOLDS RADIANCE (PHOTON-7 stage 1, L1): a voxel's value is the surface's outgoing radiance, the same
        // at every placement - upstream's per-box normalisation k = pi / D_max (the brightest light in range of the BOX)
        // made the bytes depend on which lights the box held (a lamp of 8 in range at one placement: every voxel 8x),
        // and is DELETED with every multiply and divide by it (the injection's, the pixel's decode, the hop's unit
        // conversion, the bounce's and the field's environment rescales, the cards', the ray tier's). The float store
        // holds it with headroom (Bistro Interior, cascade 0 at a lamp of 8: the direct term's peak 229.1, 286x under
        // the RGBA16F ceiling); a lamp too small for the ceiling is held at its floor radius (addLight).

        const Vector3 voxelCellSize( mVoxelizer->getVoxelCellSize() );

        mNumLights->setManualValue( numCollectedLights );
        mLightsPerVoxel->setManualValue( mLightBudget );
        mVoxelCellSize->setManualValue( voxelCellSize );
        mInvVoxelResolution->setManualValue( invVoxelRes );
        mShaderParams->setDirty();

        renderSystem->endCopyEncoder();

        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        {
            const InjectionPhase phase( mPhaseListener, Phase::Direct );
            // THE CELLS LIT (PHOTON-7 stage 2): a commit's boxes (the faces its shift moved), else the whole volume.
            // The groups are set by hand for every dispatch (the job is shared by name).
            const uint32 res[3] = { mLightVoxel[0]->getWidth(), mLightVoxel[0]->getHeight(),
                                    mLightVoxel[0]->getDepth() };
            const uint32 boxes = delta ? mCommit.light.numInjectBoxes : 0u;
            for( uint32 b = 0u; b < std::max( boxes, 1u ); ++b )
            {
                setRegionGrid( mLightInjectionJob, mInjectOrigin, boxes ? mCommit.light.injectLo[b] : nullptr,
                               boxes ? mCommit.light.injectHi[b] : nullptr, 1u, res, delta && b == 0u ? 1u : 0u );
                mShaderParams->setDirty();
                jahshaka::engine::detail::photonDispatch( hlmsCompute, mLightInjectionJob, renderSystem );
            }
        }
        // THE WORK LAW'S DOOR: the delta injection's count, read back without a wait (takeCommitCount).
        if( delta )
        {
            if( !mCommitTicket.get() )
                mCommitTicket = mCommitCountBuf->readRequest( 0u, 4u, /*accurateTracking=*/false );
            else
                mCommitDownloadOwed = true;   // read again once the reading in flight lands (takeCommitCount)
        }

        {
            const InjectionPhase phase( mPhaseListener, Phase::Mips );
            generateAnisotropicMips();

            // Jahshaka (MOVER-OCCLUSION-1): D's own chain, read by the ray tier's hits.
            if( mLightDirect )
                PhotonMipChain::generate( mVoxelizer->getHlmsManager(), renderSystem, mLightDirect );
            // (the total's levels exist under the specular SDF quality only: otherwise a no-op)
            PhotonMipChain::generate( mVoxelizer->getHlmsManager(), renderSystem, mLightVoxel[0] );
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
        const bool skyPass = mStoreSkyLight && getAllowMultipleBounces() && hasEnvironmentLight();
        // PHOTON-7 stage 2: at a scroll's commit the sky and the bounces are re-solved over THE SLAB AND ITS BOUNCE REACH
        // only (setCommitLight) - the region's voxels from the sky's direct term up, the rest holding what the shift
        // carried; elsewhere the whole volume.
        const bool regionPasses = mCommit.on && mCommit.light.region;
        // (an empty region - a cascade inside the committed one, re-injected as a delta - runs no pass)
        const bool noPasses = regionPasses && ( mCommit.light.regionHi[0] <= mCommit.light.regionLo[0] ||
                                                mCommit.light.regionHi[1] <= mCommit.light.regionLo[1] ||
                                                mCommit.light.regionHi[2] <= mCommit.light.regionLo[2] );
        const uint32 *passLo = regionPasses ? mCommit.light.regionLo : nullptr;
        const uint32 *passHi = regionPasses ? mCommit.light.regionHi : nullptr;
        if( skyPass && !noPasses )
        {
            const InjectionPhase phase( mPhaseListener, Phase::Sky );
            runBounce( true, passLo, passHi );
        }

        // Jahshaka (PHOTON-5 stage 5): THE BOUNCE ENERGY, when asked and none is in flight - the
        // base's sum, then each pass's (setMeasureBounceEnergy).
        const bool measure = mMeasureEnergy && numBounces > 0u && !mEnergyTicket.get() &&
                             getAllowMultipleBounces();
        if( measure )
            dispatchEnergy( 0u );

        if( numBounces > 0u && !noPasses )
        {
            if( !getAllowMultipleBounces() )
            {
                OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                             "numBounces must be 0, else call setAllowMultipleBounces first!",
                             "PhotonVoxelLighting::update" );
            }
            const InjectionPhase phase( mPhaseListener, Phase::Bounce );
            for( uint32 i = 0u; i < numBounces; ++i )
            {
                runBounce( false, passLo, passHi );
                if( measure )
                    dispatchEnergy( i + 1u );
            }
        }
        if( measure && mEnergyBuffer )
        {
            mEnergySums = numBounces + 1u;
            mEnergyTicket = mEnergyBuffer->readRequest( 0u, size_t( mEnergySums ) * mEnergyStride,
                                                        /*accurateTracking=*/false );
        }

        mCommit = CommitLightState();   // the commit's light shapes ONE update

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
    void PhotonVoxelLighting::bindInjectionUavs( bool delta )
    {
        // PHOTON-7 stage 2: A SCROLL'S COMMIT re-injects the direct term AS A DELTA (setCommitLight): the total, the
        // back and both direct terms are READ as well as written (their old values are the shift's), so those four
        // slots are bound read-write - the barrier solver then makes the shift's writes visible to the reads (a
        // write-only slot gets no read visibility); the voxels lit are counted into the commit counter, the last UAV.
        const ResourceAccess::ResourceAccess deltaAccess = delta ? ResourceAccess::ReadWrite : ResourceAccess::Write;

        DescriptorSetUav::TextureSlot uavSlot( DescriptorSetUav::TextureSlot::makeEmpty() );
        uavSlot.access = deltaAccess;
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
        // Jahshaka (PHOTON-VOXEL-5; every tier since PHOTON-MARCH-1): the same dispatch writes level 0's
        // BACK side, the back's direct term (with a bounce) and the directional volumes' level 0
        // (the fused step 0: from each voxel's light per half-axis) - into the volumes themselves
        // without a bounce, into the direct part's own volumes with one (step 0 adds the bounce's
        // part after every bounce). The slots follow u0 in LightInjection_cs.glsl's order.
        const uint8 numUavsNeeded =
            uint8( 1u + ( mLightDirect ? 1u : 0u ) + ( mLightDirect ? 5u : 4u ) + ( delta ? 1u : 0u ) );
        if( mLightInjectionJob->getNumUavUnits() != numUavsNeeded )
            mLightInjectionJob->setNumUavUnits( numUavsNeeded );
        mLightInjectionJob->setProperty( "vct_keep_direct", mLightDirect ? 1 : 0 );
        {
            const int32 d = delta ? 1 : 0;
            if( mLightInjectionJob->getProperty( "jah_inject_delta" ) != d )
                mLightInjectionJob->setProperty( "jah_inject_delta", d );
        }
        uint8 uavIdx = 1u;
        if( mLightDirect )
        {
            uavSlot.texture = mLightDirect;
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
        }
        uavSlot.texture = mLightVoxel[backIndex()];
        uavSlot.pixelFormat = jahLightVoxelUavFormat();
        mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
        if( mLightDirect )
        {
            uavSlot.texture = mLightDirectBack;
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
        }
        uavSlot.access = ResourceAccess::Write;   // the directional level 0: written whole
        for( uint8 i = 0u; i < 3u; ++i )
        {
            uavSlot.texture = mLightDirect ? mLightDirectDir[i] : mLightVoxel[i + 1u];
            uavSlot.pixelFormat = jahLightVoxelUavFormat();
            mLightInjectionJob->_setUavTexture( uavIdx++, uavSlot );
        }
        if( delta )
        {
            // the counter: cumulative (the readings take differences), the last UAV
            DescriptorSetUav::BufferSlot bufSlot = DescriptorSetUav::BufferSlot::makeEmpty();
            bufSlot.buffer = mCommitCountBuf;
            bufSlot.offset = 0u;
            bufSlot.sizeBytes = 4u * sizeof( uint32 );
            bufSlot.access = ResourceAccess::ReadWrite;
            mLightInjectionJob->_setUavBuffer( uavIdx++, bufSlot );
        }
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::commitLightReady()
    {
        // THE COMMIT'S PERMUTATION NEVER COMPILES IN THE FRAME (ASYNC-SHADERS-1): the delta injection's permutation
        // (jah_inject_delta, the counter's slot) is asked for with the job bound as that injection binds it - and handed
        // to the background compiler when it is not built. Asked when a scroll is planned (its chunk frames are the
        // compile's time) and again at its commit, which waits a frame while it is still building. With no
        // background service the answer is "ready": the dispatch compiles as it always did. (The job's bindings are
        // the next injection's to re-assert: every update binds every slot.)
        if( !mLightDirect || !mCommitCountBuf || !mLightInjectionJob )
            return true;
        bindInjectionUavs( true );
        HlmsCompute *hlmsCompute = mVoxelizer->getHlmsManager()->getComputeHlms();
        return hlmsCompute->requestAsync( mLightInjectionJob ) != HlmsCompute::AsyncPending;
    }
    //-------------------------------------------------------------------------
    void PhotonVoxelLighting::setCommitLight( const PhotonCommitLight *commit )
    {
        mCommit = CommitLightState();
        if( !commit )
            return;
        mCommit.on = true;
        mCommit.light = *commit;
        mCommit.light.numInjectBoxes = std::min( commit->numInjectBoxes, PhotonCommitLight::kMaxInjectBoxes );
    }
    //-------------------------------------------------------------------------
    bool PhotonVoxelLighting::takeCommitCount( uint32 &litInSlab, uint32 &litOutside, uint32 &injections )
    {
        litInSlab = litOutside = injections = 0u;
        if( !mCommitTicket.get() || !mCommitTicket->queryIsTransferDone() )
            return false;
        const uint32 *words = static_cast<const uint32 *>( mCommitTicket->map() );
        // (unsigned differences: a wrap of the device's words is harmless)
        litInSlab = words[0] - mCommitSeen[0];    // x 0: the voxels outside the kept box (the slab)
        litOutside = words[1] - mCommitSeen[1];   // x 1: inside it (the changed direct set)
        injections = words[2] - mCommitSeen[2];   // x 2: the delta injections
        for( int i = 0; i < 3; ++i )
            mCommitSeen[i] = words[i];
        mCommitTicket->unmap();
        mCommitTicket.reset();
        if( mCommitDownloadOwed && mCommitCountBuf )
        {
            mCommitTicket = mCommitCountBuf->readRequest( 0u, 4u, /*accurateTracking=*/false );
            mCommitDownloadOwed = false;
        }
        return true;
    }
    //-------------------------------------------------------------------------
    uint32 PhotonVoxelLighting::regionReachCells() const
    {
        float4 invResMaxLod[c_maxCascades];
        float4 fromPrev[c_maxCascades][2];
        getCascadeChainParams( &invResMaxLod[0].x, mExtraCascades.empty() ? nullptr : &fromPrev[0][0].x );
        const float maxLod = invResMaxLod[0].w;
        if( !( maxLod < 64.0f ) )
            return 0u;   // the outermost: its cones cross the whole box
        return static_cast<uint32>( std::ceil( std::exp2( maxLod + 1.0f ) ) ) + 1u;
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

            // the directional volumes hold every level past 0 (the total is level 0 only)
            const uint8 cascadeNumMipmaps = cascade->mLightVoxel[1]->getNumMipmaps();

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

            *outFromPrev++ = static_cast<float>( vScale.x );
            *outFromPrev++ = static_cast<float>( vScale.y );
            *outFromPrev++ = static_cast<float>( vScale.z );
            // PADDING (PHOTON-7 stage 1): it held the hop's unit conversion, cascade i's decode multiplier over cascade
            // 0's - every cascade stores radiance now, so a hop adds the next cascade's colour as it is.
            *outFromPrev++ = 0.0f;

            *outFromPrev++ = static_cast<float>( vPos.x );
            *outFromPrev++ = static_cast<float>( vPos.y );
            *outFromPrev++ = static_cast<float>( vPos.z );
            // PADDING (PHOTON-PHYSICS-1, CONE-EMITTER-1): it held upstream's specular "brightness equalisation"
            // slope (1 / mips^3) - a near-mirror cone read every cascade past the first at half its radiance.
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

        const size_t numCascades = mExtraCascades.size() + 1u;

        // float4 vctInvResolution_cascadeMaxLod[numCascades];
        // float4 fromPreviousProbeToNext[numCascades - 1u][2]
        // (Jahshaka, PHOTON-READER-1: one definition, shared with the irradiance field.)
        getCascadeChainParams( passBufferPtr, passBufferPtr + 4u * numCascades );
        passBufferPtr += 4u * numCascades + 8u * ( numCascades - 1u );

        // float specSdfMaxMip;
        // float specularSdfFactor;
        // float blendFade;
        // float padding0; (PHOTON-7 stage 1: it was the decode multiplier D_max / pi - the voxels hold radiance)
        *passBufferPtr++ = 7.0f + mipDiff;
        // Where did 0.1875f & 0.3125f come from? Empirically obtained.
        // At 128x128x128, values in range [24; 40] gave good results.
        // Below 24, quality became unnacceptable.
        // Past 40, performance only went down without visible changes.
        // Thus 24 / 128 and 40 / 128 = 0.1875f and 0.3125f
        *passBufferPtr++ = Math::lerp( 0.1875f, 0.3125f, mSpecularSdfQuality ) * smallestRes;
        *passBufferPtr++ = 1.0f;
        *passBufferPtr++ = 0.0f;

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
        // THE SKIP'S OWN PREDICATE, ONE DEFINITION (PHOTON-MARCH-1): upstream's empty-space skip
        // reads a single volume (jah_voxel_march.glsl), so it is on for a one-cascade chain of
        // more than 32 cells per axis - what the pixel's pieces and the total's mip chain key on.
        return mChainOfOne && mVoxelizer->getAlbedoVox()->getWidth() > 32u &&
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
    size_t PhotonVoxelLighting::scrollLightTextures( TextureGpu *out[7], bool halfRes[7] ) const
    {
        if( !mLightDirect || !mLightDirectBack || !mLightDirectDir[0] || !mLightDirectDir[1] || !mLightDirectDir[2] )
            return 0u;
        TextureGpu *const list[7] = { mLightVoxel[0], mLightVoxel[backIndex()], mLightDirect,  mLightDirectBack,
                                      mLightDirectDir[0], mLightDirectDir[1], mLightDirectDir[2] };
        for( size_t i = 0u; i < 7u; ++i )
        {
            out[i] = list[i];
            halfRes[i] = i >= 4u;
        }
        return 7u;
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
