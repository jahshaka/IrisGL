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

#include "photon/voxel/PhotonIrradianceField.h"
#include "photon/PhotonDispatch.h"

#include "photon/voxel/PhotonIfdProbeVisualizer.h"
#include "photon/voxel/PhotonVoxelLighting.h"
#include "photon/voxel/PhotonVoxelizerSourceBase.h"

#include "Compositor/OgreCompositorManager2.h"
#include "OgreDescriptorSetUav.h"
#include "Compositor/OgreCompositorWorkspace.h"
#include "OgreRoot.h"

#include "OgreBitwise.h"
#include "OgreHlmsCompute.h"
#include "OgreHlmsComputeJob.h"
#include "OgreHlmsManager.h"
#include "OgreLogManager.h"
#include "OgreStringConverter.h"
#include "OgreTextureGpuManager.h"
#include "Vao/OgreConstBufferPacked.h"
#include "Vao/OgreTexBufferPacked.h"
#include "Vao/OgreUavBufferPacked.h"
#include "Vao/OgreVaoManager.h"


namespace Ogre
{
    PhotonIrradianceFieldSettings::PhotonIrradianceFieldSettings() :
        mNumRaysPerPixel( 1u ),
        mDepthProbeResolution( 12u ),
        mIrradianceResolution( 6u )
    {
        for( size_t i = 0u; i < 3u; ++i )
            mNumProbes[i] = 32u;
        mNumProbes[1] = 8u;
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceFieldSettings::getTotalNumProbes() const
    {
        return mNumProbes[0] * mNumProbes[1] * mNumProbes[2];
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceFieldSettings::getDepthProbeFullResolution( uint32 &outWidth,
                                                               uint32 &outHeight ) const
    {
        // totalNumProbes is a power of 2, thus it can be expressed as 2ⁿ
        // Hence find the resolution where 2ᵃ * 2ᵇ = 2ⁿ
        const uint32 totalNumProbes = getTotalNumProbes();
        const uint32 exponent = Bitwise::ctz32( totalNumProbes );
        const uint8 borderedDepthResolution = getBorderedDepthResolution();
        outWidth = borderedDepthResolution * ( 1u << ( exponent >> 1u ) );
        outHeight = borderedDepthResolution * ( 1u << ( exponent - ( exponent >> 1u ) ) );
        OGRE_ASSERT_LOW( outWidth * outHeight ==
                         totalNumProbes * borderedDepthResolution * borderedDepthResolution );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceFieldSettings::getIrradProbeFullResolution( uint32 &outWidth,
                                                               uint32 &outHeight ) const
    {
        const uint32 totalNumProbes = getTotalNumProbes();
        const uint32 exponent = Bitwise::ctz32( totalNumProbes );
        const uint8 borderedIrradResolution = getBorderedIrradResolution();
        outWidth = borderedIrradResolution * ( 1u << ( exponent >> 1u ) );
        outHeight = borderedIrradResolution * ( 1u << ( exponent - ( exponent >> 1u ) ) );
        OGRE_ASSERT_LOW( outWidth * outHeight ==
                         totalNumProbes * borderedIrradResolution * borderedIrradResolution );
    }
    //-------------------------------------------------------------------------
    uint8 PhotonIrradianceFieldSettings::getBorderedIrradResolution() const
    {
        return mIrradianceResolution + 2u;
    }
    //-------------------------------------------------------------------------
    uint8 PhotonIrradianceFieldSettings::getBorderedDepthResolution() const
    {
        return mDepthProbeResolution + 2u;
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceFieldSettings::getNumRaysPerProbe() const
    {
        return uint32( mDepthProbeResolution ) * mDepthProbeResolution * mNumRaysPerPixel;
    }
    //-------------------------------------------------------------------------
    Vector3 PhotonIrradianceFieldSettings::getNumProbes3f() const
    {
        return Vector3( static_cast<Real>( mNumProbes[0] ), static_cast<Real>( mNumProbes[1] ),
                        static_cast<Real>( mNumProbes[2] ) );
    }
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    //-------------------------------------------------------------------------
    PhotonIrradianceField::PhotonIrradianceField( Root *root, SceneManager *sceneManager ) :
        IdObject( Id::generateNewId<PhotonIrradianceField>() ),
        mNumProbesProcessed( 0u ),
        mNumWorkBoxes( 0u ),
        mWorkTotal( 0u ),
        mWorkMode( IntegrateFresh ),
        mTargetSamples( 1u ),
        mKeepOnChange( 0u ),
        mChangeSamples( 1u ),
        mRotateRays( true ),
        mRefinesOwed( 0u ),
        mRefineWhole( true ),
        mFieldOrigin( Vector3::ZERO ),
        mFieldSize( Vector3::ZERO ),
        mVctLighting( 0 ),
        mIrradianceTex( 0 ),
        mDepthVarianceTex( 0 ),
        mGenerationJob( 0 ),
        mDepthMirrorBorderJob( 0 ),
        mColourMirrorBorderJob( 0 ),
        mIfGenParamsBuffer( 0 ),
        mIfGenParamsRingFrame( 0u ),
        mIfGenParamsRingNext( 0u ),
        mDirectionsBuffer( 0 ),
        mIfdDepthBorderMirrorParamsBuffer( 0 ),
        mIfdColourBorderMirrorParamsBuffer( 0 ),
        mDebugVisualizationMode( DebugVisualizationNone ),
        mDebugTessellation( 4u ),
        mDebugIfdProbeVisualizer( 0 ),
        mRoot( root ),
        mSceneManager( sceneManager )
    {
        memset( mWorkBoxes, 0, sizeof( mWorkBoxes ) );
        mWindowOffset[0] = mWindowOffset[1] = mWindowOffset[2] = 0u;
#if OGRE_NO_JSON
        OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                     "To use PhotonIrradianceField, Ogre must be build with JSON support "
                     "and you must include the resources bundled at "
                     "media/Photon/IrradianceField",
                     "PhotonIrradianceField::PhotonIrradianceField" );
#endif
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        mIfGenParamsBuffer = vaoManager->createConstBuffer( sizeof( IrradianceFieldGenParams ),
                                                            BT_DYNAMIC_PERSISTENT, 0, false );
        mIfGenParamsRing.push_back( mIfGenParamsBuffer );
        mIfGenParamsRingFrame = vaoManager->getFrameCount();

        mIfdDepthBorderMirrorParamsBuffer =
            vaoManager->createConstBuffer( sizeof( IfdBorderMirrorParams ), BT_DEFAULT, 0, false );
        mIfdColourBorderMirrorParamsBuffer =
            vaoManager->createConstBuffer( sizeof( IfdBorderMirrorParams ), BT_DEFAULT, 0, false );

        HlmsCompute *hlmsCompute = mRoot->getHlmsManager()->getComputeHlms();
        mGenerationJob = hlmsCompute->findComputeJobNoThrow( "IrradianceField/Gen" );
        if( mGenerationJob )
        {
            // THE INTEGRATION MODES, ONE DEFINITION (Jahshaka, PHOTON-FIELD-ROTATE-1):
            // the job compares sweep.x against these properties, written from the enum.
            mGenerationJob->setProperty( "ifd_mode_fresh", IntegrateFresh );
            mGenerationJob->setProperty( "ifd_mode_change", IntegrateChange );
            mGenerationJob->setProperty( "ifd_mode_refine", IntegrateRefine );
            mGenerationJob->setProperty( "ifd_mode_invalidate", IntegrateInvalidate );
        }

        if( !mGenerationJob )
        {
            OGRE_EXCEPT( Exception::ERR_INVALIDPARAMS,
                         "To use PhotonIrradianceField, you must include the resources bundled at "
                         "media/Photon/IrradianceField\n"
                         "Could not find IrradianceField/Gen",
                         "PhotonIrradianceField::PhotonIrradianceField" );
        }


        mDepthMirrorBorderJob = hlmsCompute->findComputeJob( "IrradianceField/BorderMirror/Depth" );
        mColourMirrorBorderJob = hlmsCompute->findComputeJob( "IrradianceField/BorderMirror/Colour" );
    }
    //-------------------------------------------------------------------------
    PhotonIrradianceField::~PhotonIrradianceField()
    {
        setDebugVisualization( DebugVisualizationNone, 0, mDebugTessellation );
        destroyTextures();

        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        for( ConstBufferPacked *buffer : mIfGenParamsRing )
        {
            if( buffer->getMappingState() != MS_UNMAPPED )
                buffer->unmap( UO_UNMAP_ALL );
            vaoManager->destroyConstBuffer( buffer );
        }
        mIfGenParamsRing.clear();
        mIfGenParamsBuffer = 0;
        // Jahshaka (POOL-LEAKS-1): the two border-mirror parameter buffers the
        // constructor creates were never destroyed — every PhotonIrradianceField (one per
        // scene with a field; a project open/close cycle makes one) left two const
        // buffers behind in the VaoManager's pools.
        if( mIfdDepthBorderMirrorParamsBuffer )
            vaoManager->destroyConstBuffer( mIfdDepthBorderMirrorParamsBuffer );
        mIfdDepthBorderMirrorParamsBuffer = 0;
        if( mIfdColourBorderMirrorParamsBuffer )
            vaoManager->destroyConstBuffer( mIfdColourBorderMirrorParamsBuffer );
        mIfdColourBorderMirrorParamsBuffer = 0;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::fillDirections( float *RESTRICT_ALIAS outBuffer )
    {
        // Jahshaka (PHOTON-FIELD-ROTATE-1): THE PROBE'S RAYS ARE A SPHERICAL FIBONACCI SET,
        // uniform in SOLID ANGLE, and they belong to no texel. Upstream shot one ray (or a
        // fixed subsample pattern) per depth texel, at the texel's octahedral direction,
        // and integrated the texels as a Riemann sum weighted by the cosine alone - but
        // the octahedral texels are not equal solid angle (dw/dudv = 1/|q|^3: 1 at the six
        // axis vertices, 5.2 at the face centres), so a small source near an axis read up
        // to 3.3x its irradiance and one near a face centre ~0.6x, whatever the ray count
        // (the one-voxel wall of gi.field_thin_wall read 1.68x at 256 rays a texel). The
        // set is rotated per integration (per probe and sample, in the generation job) and every
        // texel integrates every ray at its own direction: DDGI's estimator.
        float *RESTRICT_ALIAS updateData = reinterpret_cast<float * RESTRICT_ALIAS>( outBuffer );
        const uint32 numRays = mSettings.getNumRaysPerProbe();
        const float goldenAngle = Math::PI * ( 3.0f - Math::Sqrt( 5.0f ) );
        for( uint32 i = 0u; i < numRays; ++i )
        {
            const float z = 1.0f - ( 2.0f * float( i ) + 1.0f ) / float( numRays );
            const float r = Math::Sqrt( std::max( 0.0f, 1.0f - z * z ) );
            const float phi = goldenAngle * float( i );
            *updateData++ = r * std::cos( phi );
            *updateData++ = r * std::sin( phi );
            *updateData++ = z;
            *updateData++ = 0.0f;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setIrradianceFieldGenParams()
    {
        const uint32 depthProbeRes = mSettings.mDepthProbeResolution;
        const uint32 irradProbeRes = mSettings.mIrradianceResolution;
        const uint32 numRaysPerProbe = mSettings.getNumRaysPerProbe();

        mIfGenParams.padding0 = 0u;

        // THE BIN'S CONE, DERIVED FROM THE RAY COUNT (Jahshaka, PHOTON-WRITER-1). The
        // probe's N rays tile the sphere, each standing for a bin of 4 pi / N sr, and the
        // cone of that solid angle has 2 pi ( 1 - cos theta ) = 4 pi / N, i.e.
        // cos theta = 1 - 2 / N (tan theta = 0.1686 at the 144 rays of a 12x12 depth
        // probe). Upstream's tan( 2 pi / N ) is not a half-angle of anything (0.0437).
        // WHAT READS IT: the ENVIRONMENT an escaping ray sees, prefiltered over its bin
        // (the sky has no occlusion to bias, so the bin average is the right estimate).
        // The VOXEL march does NOT - a probe ray crosses the voxels as a RAY, at the
        // store's own one-cell floor (the generation job says why: a cone's composite
        // over-occludes grazing directions, measured).
        mIfGenParams.coneAngleTan =
            Math::Tan( Math::ACos( 1.0f - 2.0f / static_cast<float>( numRaysPerProbe ) ).valueRadians() );
        mIfGenParams.numProcessedProbes = 0u;
        fillChainParams();
        fillEnvironmentParams();

        mIfGenParams.numProbes.x = mSettings.mNumProbes[0];
        mIfGenParams.numProbes.y = mSettings.mNumProbes[1];
        mIfGenParams.numProbes.z = mSettings.mNumProbes[2];
        mIfGenParams.numProbes.w = 0u;

        const PhotonVoxelizerSourceBase *voxelizer = mVctLighting->getVoxelizer();
        Matrix4 irrProbeToVctTransform;
        irrProbeToVctTransform.makeTransform(
            ( mFieldOrigin - voxelizer->getVoxelOrigin() ) / voxelizer->getVoxelSize(),
            ( mFieldSize / voxelizer->getVoxelSize() ) / mSettings.getNumProbes3f(),
            Quaternion::IDENTITY );
        mIfGenParams.irrProbeToVctTransform = irrProbeToVctTransform;
        {
            // FIELD-DIR-1 (PHOTON-VOXEL-4): the box's aspect, for the rays' directions.
            const Vector3 boxSize = voxelizer->getVoxelSize();
            const Real minSize = std::min( boxSize.x, std::min( boxSize.y, boxSize.z ) );
            mIfGenParams.worldToVolumeDir =
                float4( Vector4( minSize / boxSize.x, minSize / boxSize.y, minSize / boxSize.z, 0.0f ) );
        }

        // ONE WORK GROUP PER PROBE (Jahshaka, PHOTON-FIELD-ROTATE-1): its threads march
        // the probe's rays into shared memory, then integrate every texel of the
        // probe's two tiles from all of them and blend each with its history - the
        // generation IS the integration, no separate pass and no ray buffer.
        mGenerationJob->setProperty( "num_rays_per_probe", static_cast<int32>( numRaysPerProbe ) );
        mGenerationJob->setProperty( "irrad_resolution", static_cast<int32>( irradProbeRes ) );
        mGenerationJob->setProperty( "irrad_full_width",
                                     static_cast<int32>( mIrradianceTex->getWidth() ) );
        mGenerationJob->setProperty( "depth_resolution", static_cast<int32>( depthProbeRes ) );
        mGenerationJob->setProperty( "depth_full_width",
                                     static_cast<int32>( mDepthVarianceTex->getWidth() ) );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setupBorderMirrorParams( uint32 borderedRes, uint32 fullWidth,
                                                   ConstBufferPacked *ifdBorderMirrorParamsBuffer,
                                                   HlmsComputeJob *job )
    {
        const uint32 totalNumProbes = mSettings.getTotalNumProbes();
        IfdBorderMirrorParams mirrorParams;
        memset( &mirrorParams, 0, sizeof( mirrorParams ) );
        mirrorParams.probeBorderedRes = borderedRes;
        mirrorParams.numPixelsInEdges = ( borderedRes - 2u ) * 2u;
        mirrorParams.numTopBottomPixels = ( borderedRes - 2u );
        mirrorParams.numGlobalThreadsForEdges = mirrorParams.numPixelsInEdges * totalNumProbes;
        mirrorParams.maxThreadId = ( mirrorParams.numPixelsInEdges + 1u ) * totalNumProbes;

        const uint32 threadsPerGroup = 128u;
        const uint32 totalThreads =
            (uint32)alignToNextMultiple( mirrorParams.maxThreadId, threadsPerGroup );
        const uint32 numWorkGroups = totalThreads / threadsPerGroup;
        const uint32 numThreadGroupsY = numWorkGroups / 65535u + 1u;
        const uint32 numThreadGroupsX = numWorkGroups / numThreadGroupsY;

        mirrorParams.threadsPerThreadRow = numThreadGroupsX;
        ifdBorderMirrorParamsBuffer->upload( &mirrorParams, 0,
                                             ifdBorderMirrorParamsBuffer->getTotalSizeBytes() );

        job->setProperty( "full_width", static_cast<int32>( fullWidth ) );
        job->setConstBuffer( 0, ifdBorderMirrorParamsBuffer );

        job->setThreadsPerGroup( 128u, 1u, 1u );
        job->setNumThreadGroups( numThreadGroupsX, numThreadGroupsY, 1u );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::initialize( const PhotonIrradianceFieldSettings &settings,
                                      const Vector3 &fieldOrigin, const Vector3 &fieldSize,
                                      PhotonVoxelLighting *vctLighting )
    {
        mSettings = settings;
        OGRE_ASSERT_LOW( mSettings.getNumRaysPerProbe() <= 1024u );

        OGRE_ASSERT_LOW( vctLighting && "vctLighting param must be provided" );
        mVctLighting = vctLighting;
        mFieldOrigin = fieldOrigin;
        mFieldSize = fieldSize;

        // Enlarge our bounds because at the borders we have
        // limited information thus there's often a hard line
        Vector3 probeBlockSize = mFieldSize / mSettings.getNumProbes3f();
        mFieldOrigin -= probeBlockSize;
        mFieldSize += probeBlockSize * 2.0f;

        mNumProbesProcessed = 0u;
        mWindowOffset[0] = mWindowOffset[1] = mWindowOffset[2] = 0u;
        setWholeWork();
        mWorkMode = IntegrateFresh;
        noteEstimatesReplaced();
        mRefinesOwed = mTargetSamples - 1u;
        mRefineWhole = true;
        createTextures();
        setIrradianceFieldGenParams();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::createTextures()
    {
        destroyTextures();

        TextureGpuManager *textureManager = mRoot->getRenderSystem()->getTextureGpuManager();

        mIrradianceTex = textureManager->createTexture(
            "IrradianceField" + StringConverter::toString( getId() ), GpuPageOutStrategy::Discard,
            TextureFlags::Uav, TextureTypes::Type2D );
        mDepthVarianceTex = textureManager->createTexture(
            "IrradianceFieldDepth" + StringConverter::toString( getId() ), GpuPageOutStrategy::Discard,
            TextureFlags::Uav, TextureTypes::Type2D );

        uint32 irradWidth, irradHeight;
        mSettings.getIrradProbeFullResolution( irradWidth, irradHeight );
        mIrradianceTex->setResolution( irradWidth, irradHeight );
        // Jahshaka (PHOTON-ENV-1): FLOAT, not upstream's R10G10B10A2_UNORM. The atlas
        // holds the probe's irradiance in the voxels' stored units (radiance over the
        // decode multiplier D_max / pi, D_max the brightest LIGHT), and since the sky
        // entered it - every escaping probe ray reads the environment - a sky
        // brighter than that ceiling (any sky in a scene with no lamp brighter than
        // it) clipped at 1.0. Half floats hold it; the cost is 4 bytes a texel here,
        // against the 8 the depth atlas gives back below.
        mIrradianceTex->setPixelFormat( PFG_RGBA16_FLOAT );

        uint32 depthWidth, depthHeight;
        mSettings.getDepthProbeFullResolution( depthWidth, depthHeight );
        mDepthVarianceTex->setResolution( depthWidth, depthHeight );
        // Upstream's two channels, the depth moments. (PHOTON-READER-1 widened it to
        // four for the probe rays' escape fraction, read by the pixel as a sky
        // visibility; PHOTON-ENV-1 put the sky itself into the irradiance atlas and
        // that channel lost its only reader.)
        mDepthVarianceTex->setPixelFormat( PFG_RG32_FLOAT );

        mIrradianceTex->scheduleTransitionTo( GpuResidency::Resident );
        mDepthVarianceTex->scheduleTransitionTo( GpuResidency::Resident );

        VaoManager *vaoManager = textureManager->getVaoManager();

        setupBorderMirrorParams( mSettings.getBorderedDepthResolution(), mDepthVarianceTex->getWidth(),
                                 mIfdDepthBorderMirrorParamsBuffer, mDepthMirrorBorderJob );
        setupBorderMirrorParams( mSettings.getBorderedIrradResolution(), mIrradianceTex->getWidth(),
                                 mIfdColourBorderMirrorParamsBuffer, mColourMirrorBorderJob );

        if( mDebugIfdProbeVisualizer )
        {
            setTextureToDebugVisualizer();
            mDebugIfdProbeVisualizer->setVisible( true );

            // Field AABB may have changed
            placeDebugVisualizer();
        }

        // Jahshaka (ENGINE-UPGRADE-2B stage 4): THE VARIANCE STATE, zeroed (no probe stopped,
        // every moment 0): four counters, a flag per probe, an M2 per interior irradiance
        // texel, the inputs' stamp per probe (K4; 0 = none: mChangeSerial starts at 1) - the
        // generation job's JAH_IFD_ layout.
        {
            const size_t probes = mSettings.getTotalNumProbes();
            const size_t res = mSettings.mIrradianceResolution;
            const size_t words = 4u + probes + probes * res * res + probes;   // ... and a stamp per probe (K4)
            vector<uint32>::type zeros( words, 0u );
            mVarianceBuffer = vaoManager->createUavBuffer( words, sizeof( uint32 ), 0, zeros.data(), false );
            mSlotSerial.assign( probes, 0u );
            mSerialFrame[mChangeSerial & 255u] = vaoManager->getFrameCount();
            mVarianceTicket.reset();
            ++mStopEpoch;
            mProbesStopped = 0u;
            mIntegrations = 0u;
        }

        if( !mVctLighting )
            return;

        const size_t updateDataSize = sizeof( float ) * 4u * mSettings.getNumRaysPerProbe();
        float *directionsBuffer =
            reinterpret_cast<float *>( OGRE_MALLOC_SIMD( updateDataSize, MEMCATEGORY_GEOMETRY ) );
        FreeOnDestructor dataPtr( directionsBuffer );
        fillDirections( directionsBuffer );
        mDirectionsBuffer = vaoManager->createTexBuffer( PFG_RGBA32_FLOAT, updateDataSize, BT_DEFAULT,
                                                         directionsBuffer, false );

        mGenerationJob->setConstBuffer( 0, mIfGenParamsBuffer );

        bindChainToGenerationJob();

    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::destroyTextures()
    {
        if( mDebugIfdProbeVisualizer )
            mDebugIfdProbeVisualizer->setVisible( false );

        TextureGpuManager *textureManager = mRoot->getRenderSystem()->getTextureGpuManager();

        if( mIrradianceTex )
        {
            textureManager->destroyTexture( mIrradianceTex );
            mIrradianceTex = 0;
        }
        if( mDepthVarianceTex )
        {
            textureManager->destroyTexture( mDepthVarianceTex );
            mDepthVarianceTex = 0;
        }
        VaoManager *vaoManager = textureManager->getVaoManager();
        if( mDirectionsBuffer )
        {
            vaoManager->destroyTexBuffer( mDirectionsBuffer );
            mDirectionsBuffer = 0;
        }
        mVarianceTicket.reset();
        if( mVarianceBuffer )
        {
            vaoManager->destroyUavBuffer( mVarianceBuffer );
            mVarianceBuffer = 0;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setFieldVolume( const Vector3 &fieldOrigin, const Vector3 &fieldSize )
    {
        mFieldOrigin = fieldOrigin;
        mFieldSize = fieldSize;
        // Jahshaka (PHOTON-WRITER-1): a RE-PLACEMENT keeps no probe, so the window
        // starts over at slot 0 and the work is the whole grid - with no history
        // (PHOTON-FIELD-ROTATE-1: every probe stands somewhere new).
        mWindowOffset[0] = mWindowOffset[1] = mWindowOffset[2] = 0u;
        setWholeWork();
        mWorkMode = IntegrateFresh;
        noteEstimatesReplaced();
        mRefinesOwed = mTargetSamples - 1u;
        mRefineWhole = true;
        invalidateAllProbes();

        // The same enlargement initialize() applies, for the same reason (limited
        // information at the borders), so that a moved field is placed exactly as a
        // field initialized at this volume would have been.
        Vector3 probeBlockSize = mFieldSize / mSettings.getNumProbes3f();
        mFieldOrigin -= probeBlockSize;
        mFieldSize += probeBlockSize * 2.0f;

        if( mDebugIfdProbeVisualizer )
            placeDebugVisualizer();

        // The probe-to-voxel transform lives in the generation params and must be
        // re-derived (the voxelizer may have moved too, which is the whole point).
        if( mVctLighting )
            setIrradianceFieldGenParams();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::bindChainToGenerationJob()
    {
        OGRE_ASSERT_LOW( mVctLighting );

        // The job's texture units, in the order the shader declares them (the same
        // order PhotonVoxelLighting::setupBounceTextures binds the bounce job's): unit 0 the
        // directions buffer, then every cascade's total (level 0), then every cascade's
        // X, every cascade's Y, every cascade's Z; then every
        // cascade's per-axis coverage (PHOTON-VOXEL-3), then every cascade's surface
        // position (PHOTON-VOXEL-4).
        const uint32 numCascades =
            std::min<uint32>( static_cast<uint32>( mVctLighting->getNumCascades() ),
                              kMaxChainCascades );
        // + the per-axis coverage and the surface position, the light-volume list's last
        // entries (PHOTON-VOXEL-3/-4): the probe rays' march reads the opacity along each
        // ray from them.
        const uint32 numVolumes = mVctLighting->getNumVoxelTextures();

        const int32 numCascadesI32 = static_cast<int32>( numCascades );
        if( mGenerationJob->getProperty( "hlms_num_vct_cascades" ) != numCascadesI32 )
            mGenerationJob->setProperty( "hlms_num_vct_cascades", numCascadesI32 );

        // Jahshaka (PHOTON-ENV-1): + the environment cube, last, while the lighting
        // has one (the job property jah_env declares it).
        TextureGpu *envCube = mVctLighting->getEnvironmentCube();
        {
            const int32 envOn = envCube ? 1 : 0;
            if( mGenerationJob->getProperty( "jah_env" ) != envOn )
                mGenerationJob->setProperty( "jah_env", envOn );
        }
        const uint8 numTexUnits =
            static_cast<uint8>( 1u + numVolumes * numCascades + ( envCube ? 1u : 0u ) );
        if( mGenerationJob->getNumTexUnits() != numTexUnits )
            mGenerationJob->setNumTexUnits( numTexUnits );

        DescriptorSetTexture2::BufferSlot bufferSlot( DescriptorSetTexture2::BufferSlot::makeEmpty() );
        bufferSlot.buffer = mDirectionsBuffer;
        mGenerationJob->setTexBuffer( 0, bufferSlot );

        // ONE sampler, bound with the first volume (the shader declares one): the other
        // units carry no samplerblock, exactly as the bounce job's extra cascades do.
        DescriptorSetTexture2::TextureSlot texSlot( DescriptorSetTexture2::TextureSlot::makeEmpty() );
        uint8 unit = 1u;
        for( uint32 v = 0u; v < numVolumes; ++v )
        {
            for( uint32 c = 0u; c < numCascades; ++c )
            {
                texSlot.texture = mVctLighting->getLightVoxelTextures( c )[v];
                if( unit == 1u )
                {
                    mGenerationJob->setTexture( unit, texSlot,
                                                mVctLighting->getBindTrilinearSamplerblock() );
                }
                else
                    mGenerationJob->setTexture( unit, texSlot, 0, false );
                ++unit;
            }
        }
        if( envCube )
        {
            texSlot.texture = envCube;
            mGenerationJob->setTexture( unit, texSlot, 0, false );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::fillEnvironmentParams()
    {
        // In RADIANCE: the march returns the rays' colour in it (the voxels hold radiance, PHOTON-7 stage 1), so the sky
        // an escaping ray adds is added as it is (the bounce job does the same, PhotonVoxelLighting::runBounce).
        TextureGpu *envCube = mVctLighting->getEnvironmentCube();
        const float *gain = mVctLighting->getEnvironmentGain();
        const float *sh = mVctLighting->getEnvironmentSh();
        mIfGenParams.envGainMips = float4(
            Vector4( gain[0], gain[1], gain[2], envCube ? Real( envCube->getNumMipmaps() ) : Real( 1 ) ) );
        for( size_t i = 0u; i < 9u; ++i )
            mIfGenParams.envSh[i] = float4( Vector4( sh[i * 3u + 0u], sh[i * 3u + 1u], sh[i * 3u + 2u], 0.0f ) );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::fillChainParams()
    {
        const size_t numCascades = mVctLighting->getNumCascades();
        float invResMaxLod[4u * 16u];
        float fromPrev[8u * 16u];
        memset( invResMaxLod, 0, sizeof( invResMaxLod ) );
        memset( fromPrev, 0, sizeof( fromPrev ) );
        if( numCascades <= 16u )
            mVctLighting->getCascadeChainParams( invResMaxLod, fromPrev );
        const size_t n = std::min<size_t>( numCascades, kMaxChainCascades );
        for( size_t i = 0u; i < kMaxChainCascades; ++i )
        {
            const float *src = &invResMaxLod[4u * i];
            mIfGenParams.vctInvResMaxLod[i] = i < n ? float4( Vector4( src[0], src[1], src[2], src[3] ) )
                                                    : float4( Vector4::ZERO );
        }
        for( size_t i = 0u; i < ( kMaxChainCascades - 1u ) * 2u; ++i )
        {
            const float *src = &fromPrev[4u * i];
            mIfGenParams.vctFromPrev[i] = i < ( n - 1u ) * 2u ? float4( Vector4( src[0], src[1], src[2], src[3] ) )
                                                              : float4( Vector4::ZERO );
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setVctLighting( PhotonVoxelLighting *vctLighting )
    {
        if( !vctLighting || !mGenerationJob )
            return;

        mVctLighting = vctLighting;

        // The bindings createTextures() made, re-made against the textures the lighting
        // owns now. Everything else it created (the atlases, the directions buffer, the
        // integration taps, the workspace) describes the FIELD and is unaffected.
        bindChainToGenerationJob();

        // The generation params carry the voxel volume's placement and the cone start
        // bias derived from its resolution: both belong to the lighting that was just
        // bound.
        setIrradianceFieldGenParams();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::invalidateAllProbes()
    {
        if( !mIrradianceTex || !mVctLighting || !mGenerationJob )
            return;
        // THE GENERATION JOB'S OWN DISPATCH in its Invalidate mode (no rays: every
        // probe's tile to value 0, count 0), not ComputeTools' clear - a clear job
        // compiled on the first re-placement is a shader compile on a frame the
        // user sees (vr.warmup counts them). The whole grid, then the caller's work
        // (the whole grid, fresh) from its start.
        setWholeWork();
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateInvalidate;
        noteEstimatesReplaced();
        update( mWorkTotal );
        setWholeWork();
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateFresh;
        noteEstimatesReplaced();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::beginEnteringWhole()
    {
        invalidateAllProbes();   // the whole grid to count 0, then the whole grid fresh from its start
        mEnterDepth = 1u;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::invalidateWork()
    {
        // The current work's probes (a scroll's entered planes) to value 0, count 0 - the same
        // Invalidate dispatch, over the work's boxes - then that work, fresh, from its start.
        if( !mIrradianceTex || !mVctLighting || !mGenerationJob || !mWorkTotal )
            return;
        const IntegrationMode mode = mWorkMode;
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateInvalidate;
        update( mWorkTotal );
        mNumProbesProcessed = 0u;
        mWorkMode = mode;
        noteEstimatesReplaced();
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceField::changeCursor() const
    {
        if( !changeRemaining() )
            return 0u;
        return ( mWorkRotate + mNumProbesProcessed ) % mWorkTotal;
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceField::changeRemaining() const
    {
        const bool inFlight = mWorkWhole && mWorkTotal &&
                              ( mWorkMode == IntegrateChange || mWorkMode == IntegrateFresh );
        return inFlight && !isWorkDone() ? mWorkTotal - mNumProbesProcessed : 0u;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::noteStamps( uint32 listStart, uint32 count )
    {
        // The job's rule, slot for slot (jahFieldWorkSlot, the history read): an invalidation stamps 0, a
        // change skips a slot already stamped with the serial, a fresh or a change integration stamps it.
        if( mSlotSerial.size() != mSettings.getTotalNumProbes() )
            return;
        const uint32 nx = mSettings.mNumProbes[0], ny = mSettings.mNumProbes[1], nz = mSettings.mNumProbes[2];
        for( uint32 p = listStart; p < listStart + count; ++p )
        {
            uint32 first = 0u, i = 0u;
            for( ; i + 1u < mNumWorkBoxes; ++i )
            {
                const ProbeBox &b = mWorkBoxes[i];
                const uint32 n = b.size[0] * b.size[1] * b.size[2];
                if( p < first + n )
                    break;
                first += n;
            }
            const ProbeBox &b = mWorkBoxes[i];
            const uint32 q = p - first;
            const uint32 x = ( b.lo[0] + q % b.size[0] ) & ( nx - 1u );
            const uint32 y = ( b.lo[1] + ( q / b.size[0] ) % b.size[1] ) & ( ny - 1u );
            const uint32 z = ( b.lo[2] + q / ( b.size[0] * b.size[1] ) ) & ( nz - 1u );
            uint32 &held = mSlotSerial[x + y * nx + z * nx * ny];
            if( mWorkMode == IntegrateInvalidate )
                held = 0u;
            else if( mWorkMode == IntegrateChange && held == mChangeSerial )
                ++mChangeSkips;
            else
                held = mChangeSerial;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::lightAgeBySlab( vector<uint32>::type &out ) const
    {
        const uint32 nx = mSettings.mNumProbes[0], ny = mSettings.mNumProbes[1], nz = mSettings.mNumProbes[2];
        out.assign( nz, 0u );
        if( mSlotSerial.size() != size_t( nx ) * ny * nz || !mRoot )
            return;
        const uint32 now = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
        for( uint32 z = 0u; z < nz; ++z )
        {
            const uint32 local = ( z + nz - mWindowOffset[2] % nz ) % nz;
            for( uint32 i = 0u; i < nx * ny; ++i )
            {
                const uint32 held = mSlotSerial[z * nx * ny + i];
                if( held == mChangeSerial )
                    continue;
                // Superseded when the serial after it was issued (the oldest the ring holds, past 255).
                const uint32 next = mChangeSerial - held > 255u ? mChangeSerial - 255u : held + 1u;
                out[local] = std::max( out[local], now - mSerialFrame[next & 255u] );
            }
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::reset()
    {
        const uint32 cursor = changeCursor();
        ++mChangeSerial;
        mSerialFrame[mChangeSerial & 255u] = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
        setWholeWork();
        mWorkRotate = mWorkTotal ? cursor % mWorkTotal : 0u;
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateChange;
        noteEstimatesReplaced();
        mRefinesOwed = mTargetSamples - 1u;
        mRefineWhole = true;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::resumeChange( uint32 cursor, uint32 remaining )
    {
        setWholeWork();
        if( !mWorkTotal )
            return;
        remaining = std::min( remaining, mWorkTotal );
        mNumProbesProcessed = mWorkTotal - remaining;
        // The rotation that puts the cursor `processed` positions into the list.
        mWorkRotate = ( cursor % mWorkTotal + mWorkTotal - mNumProbesProcessed % mWorkTotal ) % mWorkTotal;
        mWorkMode = IntegrateChange;
        noteEstimatesReplaced();
        // The refinements the change owes after it: the whole grid (reset's), whatever the scroll joined.
        mRefinesOwed = mTargetSamples - 1u;
        mRefineWhole = true;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::refine()
    {
        // OVER WHAT THE EVENT TOUCHED (scrollWindow's band), or the whole grid.
        if( mRefineWhole )
            setWholeWork();
        else
            setBoxesFromBands( mBandLo, mBandNum );
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateRefine;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setVarianceStop( uint32 minSamples, float precision, float meanFloor )
    {
        // A stop needs a variance: two samples at the very least.
        mStopMinSamples = minSamples ? std::max( 2u, minSamples ) : 0u;
        mStopPrecision = precision;
        mStopMeanFloor = meanFloor;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::requestVarianceReadout()
    {
        if( !mVarianceBuffer || mVarianceTicket )
            return;
        // INACCURATE TRACKING (the voxel gather's readout, OgreGpuVoxelGather.cpp): the copy
        // rides the frame's own submission after this frame's integrations, and the ticket is
        // done once the device finished that frame.
        mVarianceTicket = mVarianceBuffer->readRequest( 0u, 4u, false );
        mVarianceTicketEpoch = mStopEpoch;
        mVarianceTicketFrame = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::harvestVarianceReadout()
    {
        // A FIXED LATENCY IN FRAMES, never "when the device happens to finish": the reading drives the early end
        // and the gather's rest key, and a frame that depended on the GPU's speed would make the picture of a
        // given frame differ from run to run (the selftest's mover pose did: two hashes from one tip). Taken
        // exactly when every frame in flight since the request has been waited on - the transfer is done by
        // then (Ogre fences the frame before reusing its dynamic buffers), so the map never stalls.
        if( !mVarianceTicket )
            return;
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        if( vaoManager->getFrameCount() - mVarianceTicketFrame <= uint32( vaoManager->getDynamicBufferMultiplier() ) )
            return;
        const uint32 *w = static_cast<const uint32 *>( mVarianceTicket->map() );
        const uint32 stops = w[0], unstops = w[1], integrations = w[2];
        mVarianceTicket->unmap();
        mVarianceTicket.reset();
        mIntegrations = integrations;
        // A reading asked for before the last non-refining work counts stops that work cleared.
        if( mVarianceTicketEpoch == mStopEpoch )
            mProbesStopped = stops - unstops;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::noteEstimatesReplaced()
    {
        // FROM THE MOMENT THE WORK IS DEFINED, not from its first dispatch: a planner that read
        // "every probe stopped" between a scroll's (or a change's) definition and its dispatch
        // would end the very work that clears those stops.
        ++mStopEpoch;
        mProbesStopped = 0u;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::endRefinements()
    {
        // Only a REFINEMENT ends early: work that replaces estimates always runs whole - and an
        // ENTERING pass (J1) is the probes' first samples, not a refinement.
        if( ( mWorkMode != IntegrateRefine && !isWorkDone() ) || entering() )
            return;
        mRefinesOwed = 0u;
        mNumProbesProcessed = mWorkTotal;
    }
    //-------------------------------------------------------------------------
    bool PhotonIrradianceField::beginOwedRefinement()
    {
        // THE ENTERING PASSES (J1): the same boxes again, one sample deeper, before the schedule.
        if( mEnterDepth && isWorkDone() )
        {
            if( mEnterDepth < mChangeSamples )
            {
                ++mEnterDepth;
                mNumProbesProcessed = 0u;
                mWorkMode = IntegrateRefine;
                return true;
            }
            mEnterDepth = 0u;
        }
        if( !isWorkDone() || !mRefinesOwed )
            return false;
        --mRefinesOwed;
        refine();
        return true;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setIntegrationPolicy( uint32 targetSamples, uint32 keepOnChange,
                                                bool rotateRays )
    {
        mTargetSamples = std::max( 1u, targetSamples );
        mKeepOnChange = keepOnChange;
        mRotateRays = rotateRays;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setWholeWork()
    {
        mWorkRotate = 0u;
        mWorkWhole = true;
        mEnterDepth = 0u;
        mNumWorkBoxes = 1u;
        for( size_t i = 0u; i < 3u; ++i )
        {
            mWorkBoxes[0].lo[i] = 0u;
            mWorkBoxes[0].size[i] = mSettings.mNumProbes[i];
        }
        mWorkTotal = mSettings.getTotalNumProbes();
    }
    //-------------------------------------------------------------------------
    Vector3 PhotonIrradianceField::getProbeSpacing() const
    {
        return mFieldSize / mSettings.getNumProbes3f();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setBoxesFromBands( const uint32 enteredLo[3], const uint32 enteredNum[3] )
    {
        mWorkRotate = 0u;
        mWorkWhole = false;
        // ONE BOX PER AXIS WITH A BAND, DISJOINT (an integration job convolves a
        // probe's tile in place, so no probe may be integrated twice by one
        // dispatch). Box x takes the x band's planes whole; box y the y band's planes
        // minus box x's columns; box z the rest.
        mNumWorkBoxes = 0u;
        mWorkTotal = 0u;
        uint32 keptLo[3], keptNum[3];   // per axis: the slots NOT entered on it
        for( size_t a = 0u; a < 3u; ++a )
        {
            const uint32 n = mSettings.mNumProbes[a];
            keptLo[a] = ( enteredLo[a] + enteredNum[a] ) % n;
            keptNum[a] = n - enteredNum[a];
        }
        for( size_t a = 0u; a < 3u; ++a )
        {
            if( !enteredNum[a] )
                continue;
            ProbeBox &box = mWorkBoxes[mNumWorkBoxes++];
            uint32 count = 1u;
            for( size_t b = 0u; b < 3u; ++b )
            {
                if( b == a )
                {
                    box.lo[b] = enteredLo[b];
                    box.size[b] = enteredNum[b];
                }
                else if( b < a )
                {
                    box.lo[b] = keptLo[b];      // an earlier box took its entered planes
                    box.size[b] = keptNum[b];
                }
                else
                {
                    box.lo[b] = 0u;             // a later axis: all of it
                    box.size[b] = mSettings.mNumProbes[b];
                }
                count *= box.size[b];
            }
            mWorkTotal += count;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::scrollWindow( const int32 delta[3] )
    {
        const Vector3 spacing = getProbeSpacing();
        uint32 enteredLo[3];   // per axis: the first ENTERED slot (valid where delta != 0)
        uint32 enteredNum[3];
        for( size_t a = 0u; a < 3u; ++a )
        {
            const int32 n = static_cast<int32>( mSettings.mNumProbes[a] );
            const int32 d = delta[a];
            OGRE_ASSERT_LOW( d > -n && d < n && "scrollWindow: a whole-grid move keeps nothing" );
            mFieldOrigin[a] += Real( d ) * spacing[a];
            // The new window's first probe is the old one's d-th: its slot moves by d.
            const int32 newOffset = ( ( static_cast<int32>( mWindowOffset[a] ) + d ) % n + n ) % n;
            mWindowOffset[a] = static_cast<uint32>( newOffset );
            // The ENTERED planes, window-local: the far end for a positive move
            // (locals n-d .. n-1), the near end for a negative one (0 .. -d-1).
            const int32 firstLocal = d > 0 ? n - d : 0;
            enteredLo[a] = static_cast<uint32>( ( firstLocal + newOffset ) % n );
            enteredNum[a] = static_cast<uint32>( d > 0 ? d : -d );
        }
        // THE REFINEMENTS THIS SCROLL OWES cover what it touched (ENGINE-UPGRADE-2): the
        // entered planes — joined to the band an unfinished refinement of an earlier
        // scroll still owes, when the window kept moving the same way — never the whole
        // grid, whose kept probes are at the target already. A schedule that cannot be
        // a band (an unfinished whole-grid one, a reversal, a band around the grid)
        // stays whole. A pass this scroll interrupts is owed again in full (PHOTON-2 K3:
        // mRefinesOwed restarts at target - 1 below over the joined band or the grid,
        // so the probes it had not reached take their sample - and the ones it had reached
        // are refined again toward the target: a probe's group exits only at the target or
        // stopped by the variance stop).
        {
            const bool pending = mRefinesOwed > 0u || !isWorkDone();
            if( !pending )
            {
                mRefineWhole = false;
                for( size_t a = 0u; a < 3u; ++a )
                {
                    mBandLo[a] = enteredLo[a];
                    mBandNum[a] = enteredNum[a];
                }
            }
            else if( !mRefineWhole )
            {
                for( size_t a = 0u; a < 3u && !mRefineWhole; ++a )
                {
                    if( !enteredNum[a] )
                        continue;
                    const uint32 n = mSettings.mNumProbes[a];
                    if( !mBandNum[a] )
                    {
                        mBandLo[a] = enteredLo[a];
                        mBandNum[a] = enteredNum[a];
                    }
                    else if( delta[a] > 0 && ( mBandLo[a] + mBandNum[a] ) % n == enteredLo[a] )
                        mBandNum[a] += enteredNum[a];
                    else if( delta[a] < 0 && ( enteredLo[a] + enteredNum[a] ) % n == mBandLo[a] )
                    {
                        mBandLo[a] = enteredLo[a];
                        mBandNum[a] += enteredNum[a];
                    }
                    else
                        mRefineWhole = true;
                    if( mBandNum[a] >= n )
                        mRefineWhole = true;
                }
            }
        }
        // THE WORK: the entered planes, fresh - the first ENTERING pass (J1: one sample a pass, to
        // mChangeSamples, unless the caller integrates them inline: enterInline).
        setBoxesFromBands( enteredLo, enteredNum );
        mNumProbesProcessed = 0u;
        mEnterDepth = 1u;
        // The entered planes have no history (PHOTON-FIELD-ROTATE-1), and they owe the
        // refinements that bring them to the target.
        mWorkMode = IntegrateFresh;
        noteEstimatesReplaced();
        mRefinesOwed = mTargetSamples - 1u;
        // The probe-to-voxel transform reads the (moved) origin.
        if( mVctLighting )
            setIrradianceFieldGenParams();
        // Jahshaka (PHOTON-VIEW-1): and the visualizer's WINDOW OFFSET - its spheres are
        // drawn by atlas slot, and a scroll moved every probe's slot.
        if( mDebugIfdProbeVisualizer )
            placeDebugVisualizer();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::update( uint32 probesPerFrame )
    {
        // Jahshaka (PHOTON-WRITER-1): the WORK, not the whole grid (the two are the
        // same after reset()).
        const uint32 totalNumProbes = mWorkTotal;
        if( mNumProbesProcessed >= totalNumProbes )
            return;

        probesPerFrame = std::min( totalNumProbes - mNumProbesProcessed, probesPerFrame );
        // Jahshaka (K1): THE ROTATED LIST - the dispatch's first list position is the probe position
        // (rotation + progress) mod the total; a range across the list's end is two dispatches.
        const uint32 listStart = mWorkRotate ? ( mWorkRotate + mNumProbesProcessed ) % totalNumProbes
                                             : mNumProbesProcessed;
        if( listStart + probesPerFrame > totalNumProbes )
        {
            const uint32 first = totalNumProbes - listStart;
            update( first );
            update( probesPerFrame - first );
            return;
        }

        // THIS DISPATCH'S OWN PARAMETER BUFFER (the ring's reason is at its declaration).
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        if( vaoManager->getFrameCount() != mIfGenParamsRingFrame )
        {
            mIfGenParamsRingFrame = vaoManager->getFrameCount();
            mIfGenParamsRingNext = 0u;
        }
        if( mIfGenParamsRingNext >= mIfGenParamsRing.size() )
        {
            mIfGenParamsRing.push_back( vaoManager->createConstBuffer(
                sizeof( IrradianceFieldGenParams ), BT_DYNAMIC_PERSISTENT, 0, false ) );
        }
        ConstBufferPacked *paramsBuffer = mIfGenParamsRing[mIfGenParamsRingNext++];
        mGenerationJob->setConstBuffer( 0, paramsBuffer );
        IrradianceFieldGenParams *ifGenParams = reinterpret_cast<IrradianceFieldGenParams *>(
            paramsBuffer->map( 0, paramsBuffer->getNumElements() ) );

        // Most GPUs allow up to 65535 thread groups per dimension
        // Jahshaka (PHOTON-FIELD-ROTATE-1): ONE WORK GROUP PER PROBE, one thread per
        // ray (setIrradianceFieldGenParams). Any probe count dispatches (upstream's
        // ray-count arithmetic could dispatch zero groups and throw); groups past
        // the work's end exit at once (windowOffset.w).
        const uint32 numRays = mSettings.getNumRaysPerProbe();
        mGenerationJob->setThreadsPerGroup( std::min( numRays, 1024u ), 1u, 1u );
        const uint32 numThreadGroupsY = probesPerFrame / 65535u + 1u;
        const uint32 numThreadGroupsX =
            ( probesPerFrame + numThreadGroupsY - 1u ) / numThreadGroupsY;
        mGenerationJob->setNumThreadGroups( numThreadGroupsX, numThreadGroupsY, 1u );

        if( mVctLighting )
        {
            // THE CHAIN AS IT IS NOW (Jahshaka, PHOTON-READER-1): every cascade's volumes
            // and placement, re-read per dispatch. The outer cascades scroll and rebuild
            // on their own schedule and nothing notifies the field; the bindings are
            // descriptor writes that are no-ops when nothing changed, and the parameters
            // are a few hundred bytes.
            bindChainToGenerationJob();
            fillChainParams();
            fillEnvironmentParams();
        }

        mIfGenParams.numProcessedProbes = listStart;
        {
            // Jahshaka (PHOTON-WRITER-1): the work's boxes and the window's offset.
            uint32 firstPos = 0u;
            for( size_t i = 0u; i < 3u; ++i )
            {
                const bool used = i < mNumWorkBoxes;
                const ProbeBox &b = mWorkBoxes[i];
                mIfGenParams.boxLo[i].x = used ? b.lo[0] : 0u;
                mIfGenParams.boxLo[i].y = used ? b.lo[1] : 0u;
                mIfGenParams.boxLo[i].z = used ? b.lo[2] : 0u;
                mIfGenParams.boxLo[i].w = used ? firstPos : 0xFFFFFFFFu;
                mIfGenParams.boxSize[i].x = used ? b.size[0] : 1u;
                mIfGenParams.boxSize[i].y = used ? b.size[1] : 1u;
                mIfGenParams.boxSize[i].z = used ? b.size[2] : 1u;
                mIfGenParams.boxSize[i].w = 0u;
                if( used )
                    firstPos += b.size[0] * b.size[1] * b.size[2];
            }
            mIfGenParams.windowOffset.x = mWindowOffset[0];
            mIfGenParams.windowOffset.y = mWindowOffset[1];
            mIfGenParams.windowOffset.z = mWindowOffset[2];
            // THE WORK'S END for this dispatch, in list positions: a group past it
            // writes nothing (the dispatch is rounded up to whole rows).
            mIfGenParams.windowOffset.w = listStart + probesPerFrame;
        }
        if( mWorkMode != IntegrateRefine )
            noteStamps( listStart, probesPerFrame );
        {
            // Jahshaka (PHOTON-FIELD-ROTATE-1): THE RAY ROTATION IS A FUNCTION OF THE
            // PROBE'S PLACE IN THE WORLD AND ITS OWN SAMPLE INDEX (the job hashes the
            // two into a uniformly random rotation, Shoemake's quaternion): a probe at
            // one lattice point integrates the same K rotations whatever happened
            // before, so a converged field is a deterministic function of the world -
            // the property the static set had and a per-dispatch counter would lose
            // (a walk away and back, a scroll against a re-placement, two processes,
            // all converge to the same bytes).
            const Vector3 spacing = mFieldSize / mSettings.getNumProbes3f();
            mIfGenParams.latticeOrigin.x =
                static_cast<uint32>( static_cast<int32>( Math::Floor( mFieldOrigin.x / spacing.x + 0.5f ) ) );
            mIfGenParams.latticeOrigin.y =
                static_cast<uint32>( static_cast<int32>( Math::Floor( mFieldOrigin.y / spacing.y + 0.5f ) ) );
            mIfGenParams.latticeOrigin.z =
                static_cast<uint32>( static_cast<int32>( Math::Floor( mFieldOrigin.z / spacing.z + 0.5f ) ) );
            // w: THE INPUTS' SERIAL a fresh or change integration stamps (K4: mChangeSerial).
            mIfGenParams.latticeOrigin.w = mChangeSerial;
            mIfGenParams.sweep.x = static_cast<uint32>( mWorkMode );
            // An ENTERING refinement (J1) lifts its probes to mEnterDepth samples and no further.
            mIfGenParams.sweep.y = mEnterDepth >= 2u && mWorkMode == IntegrateRefine
                                       ? std::min( mTargetSamples, mEnterDepth )
                                       : mTargetSamples;
            mIfGenParams.sweep.z = mKeepOnChange;
            mIfGenParams.sweep.w = mRotateRays ? 1u : 0u;
            mIfGenParams.varianceStop = float4(
                Vector4( mStopPrecision, static_cast<Real>( mStopMinSamples ), mStopMeanFloor, 0.0f ) );
        }
        // Work that is not a refinement replaces estimates and clears their stops: every
        // reading asked for before its last dispatch is stale.
        if( mWorkMode != IntegrateRefine )
            noteEstimatesReplaced();
        mIfGenParams.probesPerRow = numThreadGroupsX;  // There's one probe per group
        *ifGenParams = mIfGenParams;

        paramsBuffer->unmap( UO_KEEP_PERSISTENT );

        dispatchGeneration();

        mNumProbesProcessed += probesPerFrame;

        // Jahshaka (PHOTON-2 stage 2a; its round's K2): A CHANGE'S OR A FRESH INTEGRATION'S BATCH TAKES
        // mChangeSamples INTEGRATIONS NOW (setChangeSamples) - the same probes, refined in place, before the
        // batch is read: a re-placement, a build, an entering plane and a change alike never leave a
        // one-sample probe.
        if( ( mWorkMode == IntegrateChange || ( mWorkMode == IntegrateFresh && !mEnterDepth ) ) &&
            mChangeSamples > 1u )
        {
            // Sub-pass i lifts a probe to i + 1 samples and no further (its target, for the pass): a probe
            // the first pass skipped (K4: it holds the inputs already, at least as deep) exits at its
            // history read in every one.
            const IntegrationMode mode = mWorkMode;
            const uint32 target = mTargetSamples;
            const uint32 start = mNumProbesProcessed - probesPerFrame;
            for( uint32 i = 1u; i < mChangeSamples; ++i )
            {
                mNumProbesProcessed = start;
                mWorkMode = IntegrateRefine;
                mTargetSamples = std::min( target, i + 1u );
                update( probesPerFrame );
            }
            mTargetSamples = target;
            mWorkMode = mode;
            mNumProbesProcessed = start + probesPerFrame;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::dispatchGeneration()
    {
        // THE GENERATION AND THE TWO BORDER MIRRORS AS DIRECT DISPATCHES (Jahshaka,
        // ENGINE-UPGRADE-2B stage 3): what the "IrradianceField/Gen/Workspace" workspace did,
        // without a nested workspace update (which, inside photon_update's PASS_CUSTOM, would be
        // a workspace driven from inside another's pass) — the integration reads and writes both
        // atlases, then each atlas's border texels mirror its interior. Recorded like every
        // Photon dispatch (photon/PhotonDispatch.h).
        RenderSystem *renderSystem = mRoot->getRenderSystem();
        HlmsCompute *hlmsCompute = mRoot->getHlmsManager()->getComputeHlms();
        ++mDispatches;
        DescriptorSetUav::TextureSlot uav( DescriptorSetUav::TextureSlot::makeEmpty() );
        uav.access = ResourceAccess::ReadWrite;
        uav.texture = mIrradianceTex;
        mGenerationJob->_setUavTexture( 0, uav );
        uav.texture = mDepthVarianceTex;
        mGenerationJob->_setUavTexture( 1, uav );
        DescriptorSetUav::BufferSlot variance( DescriptorSetUav::BufferSlot::makeEmpty() );
        variance.buffer = mVarianceBuffer;
        variance.access = ResourceAccess::ReadWrite;
        mGenerationJob->_setUavBuffer( 2, variance );
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mGenerationJob, renderSystem );
        // The descriptor holds a raw pointer a rebuild destroys: nothing stays bound.
        mGenerationJob->_setUavBuffer( 2, DescriptorSetUav::BufferSlot::makeEmpty() );
        requestVarianceReadout();
        uav.texture = mDepthVarianceTex;
        mDepthMirrorBorderJob->_setUavTexture( 0, uav );
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mDepthMirrorBorderJob, renderSystem );
        uav.texture = mIrradianceTex;
        mColourMirrorBorderJob->_setUavTexture( 0, uav );
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mColourMirrorBorderJob, renderSystem );
    }
    //-------------------------------------------------------------------------
    size_t PhotonIrradianceField::getConstBufferSize() const
    {
        // THE LAST float4 USED TO BE MISSING. fillConstBufferData() writes an
        // IrradianceFieldRenderParams: a float4x3 (12 floats), then numProbesAggregated
        // plus two paddings (4), then the depth pair (4), then the IRRADIANCE pair (4) —
        // 24 floats, 96 bytes — and the struct the generated shader declares
        // (Hlms/Pbs/Any/IrradianceField_piece_ps.any) is the same 24. This function
        // returned 20, so HlmsPbs reserved 16 bytes too few for the pass buffer and
        // advanced its write pointer by 20 floats after a 24-float write: the next
        // occupant of the pass buffer (any HlmsListener, or nothing at all in the
        // samples, which simply overrun their map) lands on top of the irradiance
        // atlas parameters and every irradiance UV collapses onto texel 0 — DDGI goes
        // to an almost-black constant, with no error anywhere.
        return sizeof( float ) * ( 4u * 3u + 4u + 4u + 4u );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::fillConstBufferData( const Matrix4 &viewMatrix,
                                               float *RESTRICT_ALIAS passBufferPtr ) const
    {
        struct IrradianceFieldRenderParams
        {
            float4x3 viewToIrradianceFieldRows;

            float2 numProbesAggregated;
            float padding0 = 0.0f;
            float padding1 = 0.0f;

            float depthBorderedRes = 0.0f;
            float depthFullWidth = 0.0f;
            float2 depthInvFullResolution;

            float irradBorderedRes = 0.0f;
            float irradFullWidth = 0.0f;
            float2 irradInvFullResolution;
        };

        const Vector3 numProbes( (Real)mSettings.mNumProbes[0],  //
                                 (Real)mSettings.mNumProbes[1],  //
                                 (Real)mSettings.mNumProbes[2] );
        const Vector3 finalSize = numProbes / mFieldSize;

        Matrix4 xform;
        xform.makeTransform( -mFieldOrigin * finalSize, finalSize, Quaternion::IDENTITY );
        xform = xform.concatenateAffine( viewMatrix.inverseAffine() );

        const float fDepthFullWidth = static_cast<float>( mDepthVarianceTex->getWidth() );
        const float fDepthFullHeight = static_cast<float>( mDepthVarianceTex->getHeight() );

        const float fIrradFullWidth = static_cast<float>( mIrradianceTex->getWidth() );
        const float fIrradFullHeight = static_cast<float>( mIrradianceTex->getHeight() );

        IrradianceFieldRenderParams *RESTRICT_ALIAS renderParams =
            reinterpret_cast<IrradianceFieldRenderParams * RESTRICT_ALIAS>( passBufferPtr );

        renderParams->viewToIrradianceFieldRows = xform;
        renderParams->numProbesAggregated.x = numProbes.x;
        renderParams->numProbesAggregated.y = numProbes.x * numProbes.y;
        renderParams->padding0 = 0;
        renderParams->padding1 = 0;

        renderParams->depthBorderedRes = static_cast<float>( mSettings.getBorderedDepthResolution() );
        renderParams->depthFullWidth = fDepthFullWidth;
        renderParams->depthInvFullResolution.x = 1.0f / fDepthFullWidth;
        renderParams->depthInvFullResolution.y = 1.0f / fDepthFullHeight;

        renderParams->irradBorderedRes = static_cast<float>( mSettings.getBorderedIrradResolution() );
        renderParams->irradFullWidth = fIrradFullWidth;
        renderParams->irradInvFullResolution.x = 1.0f / fIrradFullWidth;
        renderParams->irradInvFullResolution.y = 1.0f / fIrradFullHeight;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setDebugVisualization( DebugVisualizationMode mode, SceneManager *sceneManager,
                                                 uint8 tessellation )
    {
        if( mDebugIfdProbeVisualizer )
        {
            SceneNode *sceneNode = mDebugIfdProbeVisualizer->getParentSceneNode();
            sceneNode->getParentSceneNode()->removeAndDestroyChild( sceneNode );
            OGRE_DELETE mDebugIfdProbeVisualizer;
            mDebugIfdProbeVisualizer = 0;
        }

        mDebugVisualizationMode = mode;
        mDebugTessellation = tessellation;

        if( mode != DebugVisualizationNone )
        {
            SceneNode *rootNode = sceneManager->getRootSceneNode( SCENE_STATIC );
            SceneNode *visNode = rootNode->createChildSceneNode( SCENE_STATIC );

            mDebugIfdProbeVisualizer = OGRE_NEW PhotonIfdProbeVisualizer(
                Ogre::Id::generateNewId<Ogre::MovableObject>(),
                &sceneManager->_getEntityMemoryManager( SCENE_STATIC ), sceneManager, 0u );

            setTextureToDebugVisualizer();

            visNode->attachObject( mDebugIfdProbeVisualizer );
            placeDebugVisualizer();
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::placeDebugVisualizer()
    {
        SceneNode *sceneNode = mDebugIfdProbeVisualizer->getParentSceneNode();
        sceneNode->setPosition( mFieldOrigin );
        sceneNode->setScale( mFieldSize / mSettings.getNumProbes3f() );
        sceneNode->getCreator()->notifyStaticDirty( sceneNode );
        mDebugIfdProbeVisualizer->setWindowOffset( mWindowOffset );
    }
    //-------------------------------------------------------------------------
    PhotonIrradianceField::DebugVisualizationMode PhotonIrradianceField::getDebugVisualizationMode() const
    {
        return mDebugVisualizationMode;
    }
    //-------------------------------------------------------------------------
    uint8 PhotonIrradianceField::getDebugTessellation() const { return mDebugTessellation; }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setTextureToDebugVisualizer()
    {
        TextureGpu *trackedTex =
            mDebugVisualizationMode == DebugVisualizationColour ? mIrradianceTex : mDepthVarianceTex;
        const uint8 borderedRes = mDebugVisualizationMode == DebugVisualizationColour
                                      ? mSettings.getBorderedIrradResolution()
                                      : mSettings.getBorderedDepthResolution();
        Vector2 rangeMult( 1.0f );
        if( mDebugVisualizationMode == DebugVisualizationDepth )
        {
            // TODO: Find something better than a hardcoded 500
            rangeMult.x = 500.0f;
            rangeMult.y = rangeMult.x * rangeMult.x;
            rangeMult = 2.0f / rangeMult;
        }
        else if( mVctLighting )
        {
            // Jahshaka (D4-PHOTON-TIERS, PROBE-NAN-1): the colour probes in the pixel's unit - the atlas holds
            // radiance (PHOTON-7 stage 1: the decode multiplier the probes were scaled by is deleted).
            rangeMult = Vector2( 1.0f, 1.0f );
        }
        else
        {
            rangeMult = 2.0f / rangeMult;
        }
        mDebugIfdProbeVisualizer->setTrackingIfd( mSettings, mFieldSize, borderedRes, trackedTex,
                                                  rangeMult, mDebugTessellation );
    }
}  // namespace Ogre
