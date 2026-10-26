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
        mIrradianceResolution( 6u ),
        mRadianceResolution( 0u )
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
    void PhotonIrradianceFieldSettings::getRadianceFullResolution( uint32 &outWidth, uint32 &outHeight ) const
    {
        // A PROBE'S BLOCK (THE RADIANCE CACHE): level 0's bordered tile (34) wide, and that tile over level 1's (18)
        // tall - levels 2 and 3 (10, 6) beside level 1, level 4 (4) under level 3. Only 32 packs so.
        if( mRadianceResolution != 32u )
        {
            outWidth = outHeight = 0u;
            return;
        }
        const uint32 totalNumProbes = getTotalNumProbes();
        const uint32 exponent = Bitwise::ctz32( totalNumProbes );
        outWidth = 34u * ( 1u << ( exponent >> 1u ) );
        outHeight = 52u * ( 1u << ( exponent - ( exponent >> 1u ) ) );
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

        fillProbeTransform();

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
        // THE RADIANCE CACHE: its level 0 resolution (0: none - the job compiles no radiance at all) and the atlas
        mGenerationJob->setNumUavUnits( mRadianceTex ? 5u : 4u );
        mGenerationJob->setProperty( "rad_resolution", mRadianceTex ? int32( mSettings.mRadianceResolution ) : 0 );
        mGenerationJob->setProperty( "rad_full_width", mRadianceTex ? int32( mRadianceTex->getWidth() ) : 0 );
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
        // THE LEDGER (PHOTON-7 stage 5, 5-4): every slot empty, nothing owed
        {
            const size_t probes = mSettings.getTotalNumProbes();
            mHeld.assign( probes, 0u );
            mReplacedFrame.assign( probes, 0u );
            mStopped.assign( probes, 0u );
            mOwedRestart.assign( probes, 0u );
            mOwedRestarts = mOwedRefines = 0u;
            mStopsReadFrame = 0u;
        }
        createTextures();
        setIrradianceFieldGenParams();
        writeProbeRecords();
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
        // holds the probe's irradiance as radiance (the voxels hold radiance since
        // PHOTON-7 stage 1), and since the sky entered it - every escaping probe ray
        // reads the environment - any sky or lamp brighter than 1 clipped at 1.0. Half floats hold it; the cost is 4 bytes a texel here,
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

        // THE RADIANCE CACHE (Jahshaka, PHOTON-7 stage 10-3): the probes' radiance maps and their GGX chains, half
        // floats like the irradiance (a lamp's radiance is far over 1).
        uint32 radWidth, radHeight;
        mSettings.getRadianceFullResolution( radWidth, radHeight );
        if( radWidth )
        {
            mRadianceTex = textureManager->createTexture(
                "IrradianceFieldRadiance" + StringConverter::toString( getId() ), GpuPageOutStrategy::Discard,
                TextureFlags::Uav, TextureTypes::Type2D );
            mRadianceTex->setResolution( radWidth, radHeight );
            mRadianceTex->setPixelFormat( PFG_RGBA16_FLOAT );
            mRadianceTex->scheduleTransitionTo( GpuResidency::Resident );
        }

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
        // texel - and THE MARKS (PHOTON-7 stage 5, 5-4), four words a probe from a 256-byte
        // boundary (a storage view's offset: the accumulation binds them alone) - the
        // generation job's JAH_IFD_ layout. And the work list (U3), the marks' report.
        {
            const size_t probes = mSettings.getTotalNumProbes();
            const size_t res = mSettings.mIrradianceResolution;
            mMarkStart = uint32( ( 4u + probes + probes * res * res + 63u ) & ~size_t( 63u ) );
            const size_t words = size_t( mMarkStart ) + 4u * probes;
            vector<uint32>::type zeros( words, 0u );
            mVarianceBuffer = vaoManager->createUavBuffer( words, sizeof( uint32 ), 0, zeros.data(), false );
            mVarianceTicket.reset();
            mIntegrations = 0u;
            mWorkListBuffer = vaoManager->createUavBuffer( probes, sizeof( uint32 ), 0, zeros.data(), false );
            mMarkReport = vaoManager->createUavBuffer( 1u + probes, sizeof( uint32 ), 0, zeros.data(), false );
            for( MarkTicket &t : mMarkTickets )
                t.ticket.reset();
            mMarkTickets.assign( size_t( vaoManager->getDynamicBufferMultiplier() ) + 2u, MarkTicket() );
            mMarkNextIssue = mMarkNextTake = 0u;
            mMarkRan = false;
            mMarkKeepReport = false;
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
        if( mRadianceTex )
        {
            textureManager->destroyTexture( mRadianceTex );
            mRadianceTex = nullptr;
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
        for( MarkTicket &t : mMarkTickets )
            t.ticket.reset();
        for( UavBufferPacked **b : { &mWorkListBuffer, &mMarkReport, &mProbeRecords, &mMarkCluster } )
        {
            if( *b )
            {
                vaoManager->destroyUavBuffer( *b );
                *b = 0;
            }
        }
        mProbeRecordsDelta.reset( nullptr );
        mMarkClusterCapacity = 0u;
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
        writeProbeRecords();
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
    void PhotonIrradianceField::fillProbeTransform()
    {
        // THE PROBES IN CASCADE 0'S NORMALISED SPACE, as cascade 0 stands NOW (PHOTON-7 stage 5, 5-5: the window steps
        // with the eye and cascade 0 scrolls on its own schedule, so this is re-derived per dispatch with the chain's
        // params - a probe outside cascade 0's box starts its march in the first cascade that holds it,
        // jah_voxel_march.glsl's walk).
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
            // ...and in w the probe rays' last world shell in cascade 0's normalised units (PHOTON-7 stage 10, 10-1)
            mIfGenParams.worldToVolumeDir =
                float4( Vector4( minSize / boxSize.x, minSize / boxSize.y, minSize / boxSize.z,
                                 boxSize.x > 0.0f ? mRayLastShell / boxSize.x : 0.0f ) );
        }
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
        update( mWorkTotal );
        setWholeWork();
        mNumProbesProcessed = 0u;
        mWorkMode = IntegrateFresh;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::beginWholeFresh()
    {
        invalidateAllProbes();   // the whole grid to count 0, then the whole grid fresh (deep) from its start
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
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceField::slotAt( uint32 p ) const
    {
        // A WORK POSITION'S SLOT: the list's entry, or the box it falls in (the job's jahFieldWorkSlot, the same rule).
        if( mWorkIsList )
            return mWorkList[p];
        const uint32 nx = mSettings.mNumProbes[0], ny = mSettings.mNumProbes[1], nz = mSettings.mNumProbes[2];
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
        return x + y * nx + z * nx * ny;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::noteWork( uint32 listStart, uint32 count )
    {
        // THE LEDGER, slot for slot as the job treats them (PHOTON-7 stage 5, 5-4): an invalidation empties a slot; a
        // fresh or change integration replaces its estimate (a change's own sub-passes and K2's deep rule: the samples
        // the pass leaves), clearing its stop and its restart owed; a refinement adds one sample up to the target -
        // a stopped probe's group exits instead, which the stop's readback tells.
        if( mHeld.size() != mSettings.getTotalNumProbes() )
            return;
        const uint32 now = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
        const bool deep = ( mWorkMode == IntegrateChange || mWorkMode == IntegrateFresh ) && mChangeSamples > 1u;
        const uint16 replaced = uint16( std::min( mTargetSamples, deep ? mChangeSamples : 1u ) );
        for( uint32 p = listStart; p < listStart + count; ++p )
        {
            const uint32 slot = slotAt( p );
            uint16 &held = mHeld[slot];
            if( mWorkMode == IntegrateRefine )
            {
                if( held > 0u && held < mTargetSamples )
                    ++held;
                continue;
            }
            held = mWorkMode == IntegrateInvalidate ? 0u : replaced;
            mReplacedFrame[slot] = now;
            mStopped[slot] = 0u;
            mOwedRestart[slot] = 0u;
            if( mWorkMode == IntegrateChange )
                ++mRestarts;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::setWorkList( IntegrationMode mode )
    {
        mWorkIsList = true;
        mNumWorkBoxes = 0u;
        mWorkTotal = uint32( mWorkList.size() );
        mNumProbesProcessed = 0u;
        mWorkMode = mode;
        if( mWorkTotal && mWorkListBuffer )
            mWorkListBuffer->upload( mWorkList.data(), 0u, mWorkTotal );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::markSlots( const uint32 *slots, size_t count )
    {
        for( size_t k = 0; k < count; ++k )
        {
            const uint32 slot = slots[k];
            if( slot >= mOwedRestart.size() || !mHeld[slot] || mOwedRestart[slot] )
                continue;   // (an empty slot is integrated fresh anyway)
            mOwedRestart[slot] = 1u;
            ++mOwedRestarts;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::markAll()
    {
        ++mMarkAlls;
        for( size_t slot = 0; slot < mOwedRestart.size(); ++slot )
        {
            if( !mHeld[slot] || mOwedRestart[slot] )
                continue;
            mOwedRestart[slot] = 1u;
            ++mOwedRestarts;
        }
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::markBox( const Vector3 &lo, const Vector3 &hi )
    {
        for( uint32 slot = 0u; slot < uint32( mOwedRestart.size() ); ++slot )
        {
            if( !mHeld[slot] || mOwedRestart[slot] )
                continue;
            const Vector3 p = probeWorldPosition( slot );
            if( p.x < lo.x || p.y < lo.y || p.z < lo.z || p.x > hi.x || p.y > hi.y || p.z > hi.z )
                continue;
            mOwedRestart[slot] = 1u;
            ++mOwedRestarts;
            ++mMarkedByRegion;
        }
    }
    //-------------------------------------------------------------------------
    uint32 PhotonIrradianceField::countReadUnheld() const
    {
        const uint32 n[3] = { mSettings.mNumProbes[0], mSettings.mNumProbes[1], mSettings.mNumProbes[2] };
        if( mHeld.size() != size_t( n[0] ) * n[1] * n[2] )
            return 0u;
        uint32 lo[3], hi[3];   // the read region, window-local: [ 1, N - 2 ] (an axis under four probes: all)
        for( int a = 0; a < 3; ++a )
        {
            lo[a] = n[a] >= 4u ? 1u : 0u;
            hi[a] = n[a] >= 4u ? n[a] - 2u : n[a] - 1u;
        }
        uint32 empty = 0u;
        for( uint32 z = lo[2]; z <= hi[2]; ++z )
            for( uint32 y = lo[1]; y <= hi[1]; ++y )
                for( uint32 x = lo[0]; x <= hi[0]; ++x )
                {
                    const uint32 sx = ( x + mWindowOffset[0] ) & ( n[0] - 1u );
                    const uint32 sy = ( y + mWindowOffset[1] ) & ( n[1] - 1u );
                    const uint32 sz = ( z + mWindowOffset[2] ) & ( n[2] - 1u );
                    empty += mHeld[sx + n[0] * ( sy + n[1] * sz )] == 0u ? 1u : 0u;
                }
        return empty;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::refreshOwed()
    {
        uint32 restarts = 0u, refines = 0u, empty = 0u;
        uint64 integrations = 0u;
        for( size_t slot = 0; slot < mHeld.size(); ++slot )
        {
            if( !mHeld[slot] )
            {
                ++empty;
                integrations += std::max( mChangeSamples, 1u ) + ( mTargetSamples > mChangeSamples ? mTargetSamples - mChangeSamples : 0u );
            }
            else if( mOwedRestart[slot] )
            {
                ++restarts;
                integrations += std::max( mChangeSamples, 1u ) + ( mTargetSamples > mChangeSamples ? mTargetSamples - mChangeSamples : 0u );
            }
            else if( mHeld[slot] > 0u && mHeld[slot] < mTargetSamples && !stoppedTrusted( uint32( slot ) ) )
            {
                ++refines;
                integrations += mTargetSamples - mHeld[slot];
            }
        }
        mOwedRestarts = restarts;
        mOwedRefines = refines;
        // (the current work's own empty probes are its: they are the work, not owed past it)
        mOwedEmpty = isWorkDone() || ( mWorkMode != IntegrateFresh && mWorkMode != IntegrateInvalidate )
                         ? empty
                         : ( empty > getWorkRemaining() ? empty - getWorkRemaining() : 0u );
        mOwedIntegrations = integrations;
    }
    //-------------------------------------------------------------------------
    bool PhotonIrradianceField::beginNextWork()
    {
        if( !isWorkDone() )
            return true;   // the current work continues (a build's or a scroll's, fresh and deep, first)
        // THE EMPTY PROBES, FRESH (5-5): a fresh work a step replaced before it ended (the window's work is the newest
        // step's entered planes) leaves probes that hold nothing - never lost: integrated fresh, deep, in slot order
        mWorkList.clear();
        for( uint32 slot = 0u; slot < uint32( mHeld.size() ); ++slot )
            if( !mHeld[slot] )
                mWorkList.push_back( slot );
        if( !mWorkList.empty() )
        {
            setWorkList( IntegrateFresh );
            return true;
        }
        // THE MARKED PROBES, RESTARTED (keep 0, deep), in slot order
        mWorkList.clear();
        for( uint32 slot = 0u; slot < uint32( mOwedRestart.size() ); ++slot )
            if( mOwedRestart[slot] )
                mWorkList.push_back( slot );
        if( !mWorkList.empty() )
        {
            setWorkList( IntegrateChange );
            return true;
        }
        // ...THEN THE PROBES BELOW THE TARGET THE STOP HAS NOT ENDED, one sample a pass
        for( uint32 slot = 0u; slot < uint32( mHeld.size() ); ++slot )
            if( mHeld[slot] > 0u && mHeld[slot] < mTargetSamples && !stoppedTrusted( slot ) )
                mWorkList.push_back( slot );
        if( !mWorkList.empty() )
        {
            setWorkList( IntegrateRefine );
            return true;
        }
        mWorkIsList = false;
        return false;
    }
    //-------------------------------------------------------------------------
    Vector3 PhotonIrradianceField::probeWorldPosition( uint32 slot ) const
    {
        const uint32 nx = mSettings.mNumProbes[0], ny = mSettings.mNumProbes[1], nz = mSettings.mNumProbes[2];
        const uint32 s3[3] = { slot % nx, ( slot / nx ) % ny, slot / ( nx * ny ) };
        const uint32 n3[3] = { nx, ny, nz };
        const Vector3 spacing = getProbeSpacing();
        Vector3 p;
        for( size_t a = 0u; a < 3u; ++a )
        {
            const uint32 local = ( s3[a] + n3[a] - mWindowOffset[a] % n3[a] ) & ( n3[a] - 1u );   // jahFieldLocal
            p[a] = mFieldOrigin[a] + Real( local ) * spacing[a];
        }
        return p;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::writeProbeRecords()
    {
        // THE ACCUMULATION'S RECEIVERS (JahCardIndirect_cs.glsl's two vec4 a record): each slot's probe, a POINT -
        // ( its world position, the slot as uint bits ), ( zero half extents ).
        const uint32 probes = mSettings.getTotalNumProbes();
        if( !probes )
            return;
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        vector<float>::type rec( size_t( probes ) * 8u, 0.0f );
        for( uint32 slot = 0u; slot < probes; ++slot )
        {
            const Vector3 p = probeWorldPosition( slot );
            rec[slot * 8u + 0u] = p.x;
            rec[slot * 8u + 1u] = p.y;
            rec[slot * 8u + 2u] = p.z;
            memcpy( &rec[slot * 8u + 3u], &slot, sizeof( slot ) );
        }
        // (the CHANGED records only - a window follow moves the entered slab's probes, every other slot keeps its
        // probe: DeltaUpload, PHOTON-7's walk hitch)
        if( !mProbeRecords )
        {
            mProbeRecords = vaoManager->createUavBuffer( size_t( probes ) * 2u, 16u, 0, rec.data(), false );
            mProbeRecordsDelta.reset( mProbeRecords, rec.data(), rec.size() * sizeof( float ) );
        }
        else
            mProbeRecordsDelta.upload( mProbeRecords, rec.data(), 0u, size_t( probes ) * 2u );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::accumulateMoves( UavBufferPacked *brickState, const uint32 dims[3],
                                                 const Vector3 &corner, float brickSide, const Vector3 *finerLo,
                                                 const Vector3 *finerHi, float finerShellEnd, uint64 frame )
    {
        // THE PROBES' WOULD-MOVE (PHOTON-7 stage 5, 5-4): the cards' job (JahCardIndirect_cs.glsl) with the probes as
        // its receivers - THE REDUCE (each 4^3 cluster of this cascade's bricks: its largest absolute move and the box
        // of the bricks that moved), then THE ACCUMULATION (one thread a probe: each cluster's move times the bound of
        // the share of the probe's sphere it can fill, min( 1, S / ( pi d^2 ) ), times the probe's sensitivity - its
        // codes per unit of radiance, written by the generation job), listing a probe at a code.
        if( !brickState || !mVarianceBuffer || !mProbeRecords || !mMarkReport || !mHeld.size() )
            return;
        RenderSystem *rs = mRoot->getRenderSystem();
        HlmsCompute *hc = mRoot->getHlmsManager()->getComputeHlms();
        if( !mAccumJob )
            mAccumJob = hc->findComputeJobNoThrow( "Jahshaka/FieldAccumulate" );
        if( !mAccumJob || !mDepthVarianceTex )
            return;
        const uint32 clusters = ( ( dims[0] + 3u ) / 4u ) * ( ( dims[1] + 3u ) / 4u ) * ( ( dims[2] + 3u ) / 4u );
        VaoManager *vaoManager = rs->getVaoManager();
        if( clusters > mMarkClusterCapacity )
        {
            if( mMarkCluster )
                vaoManager->destroyUavBuffer( mMarkCluster );
            // (two records of three words a cluster: JahCardIndirect_cs.glsl's REDUCE)
            vector<uint32>::type zeros( 6u * clusters, 0u );
            mMarkCluster = vaoManager->createUavBuffer( 6u * clusters, sizeof( uint32 ), 0, zeros.data(), false );
            mMarkClusterCapacity = clusters;
        }
        const auto bufSlot = []( UavBufferPacked *b, ResourceAccess::ResourceAccess a, size_t offset, size_t bytes )
        {
            DescriptorSetUav::BufferSlot slot( DescriptorSetUav::BufferSlot::makeEmpty() );
            slot.buffer = b;
            slot.offset = offset;
            slot.sizeBytes = bytes;
            slot.access = a;
            return slot;
        };
        const uint32 probes = uint32( mHeld.size() );
        mAccumJob->_setUavBuffer( 0u, bufSlot( brickState, ResourceAccess::Read, 0u, 0u ) );
        mAccumJob->_setUavBuffer( 1u, bufSlot( mProbeRecords, ResourceAccess::Read, 0u, 0u ) );
        mAccumJob->_setUavBuffer( 2u, bufSlot( mVarianceBuffer, ResourceAccess::ReadWrite, size_t( mMarkStart ) * 4u,
                                               size_t( probes ) * 16u ) );
        mAccumJob->_setUavBuffer( 3u, bufSlot( mMarkReport, ResourceAccess::ReadWrite, 0u, 0u ) );
        mAccumJob->_setUavBuffer( 4u, bufSlot( mMarkCluster, ResourceAccess::ReadWrite, 0u, 0u ) );
        // THE VISIBILITY'S DEPTH MOMENTS (Jahshaka/FieldAccumulate, JahCardIndirect_cs.glsl's field variant)
        {
            DescriptorSetTexture2::TextureSlot tex( DescriptorSetTexture2::TextureSlot::makeEmpty() );
            tex.texture = mDepthVarianceTex;
            mAccumJob->setTexture( 0u, tex );
            tex.texture = mIrradianceTex;
            mAccumJob->setTexture( 1u, tex );
        }
        ShaderParams &params = mAccumJob->getShaderParams( "default" );
        const auto set = [&params]( const char *name, const void *v, bool isFloat )
        {
            ShaderParams::Param *p = params.findParameter( name );
            if( !p )
                return;
            if( isFloat )
                p->setManualValue( static_cast<const float *>( v ), 4u );
            else
                p->setManualValue( static_cast<const uint32 *>( v ), 4u );
        };
        // (a report a full ring could not ticket is KEPT: not cleared, the next ticket reads it with this frame's)
        if( mMarkClearFrame != frame && !mMarkKeepReport )
        {
            mMarkClearFrame = frame;
            const uint32 mode[4] = { 1u, 0u, probes, 0u };
            set( "caMode", mode, false );
            params.setDirty();
            mAccumJob->setNumThreadGroupsBasedOn( HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u );
            mAccumJob->setNumThreadGroups( 1u, 1u, 1u );
            jahshaka::engine::detail::photonDispatch( hc, mAccumJob, rs );
        }
        const float self[4] = { corner.x, corner.y, corner.z, brickSide };
        const uint32 d[4] = { dims[0], dims[1], dims[2], dims[0] * dims[1] * dims[2] };
        float lo[4] = { 0.0f, 0.0f, 0.0f, 0.0f }, hi[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
        if( finerLo && finerHi )
        {
            lo[0] = finerLo->x; lo[1] = finerLo->y; lo[2] = finerLo->z; lo[3] = 1.0f;
            hi[0] = finerHi->x; hi[1] = finerHi->y; hi[2] = finerHi->z; hi[3] = finerShellEnd;
        }
        {
            // the field: the probes per slot row and plane, the depth tile's interior and the atlas's width; the probe
            // spacing per axis and the depth's own bound in cascade 0's normalised units (the generation job's: its
            // diagonal, 1.414) - the shader takes a texel's bound in grid units as 1.414 x |d| . numProbes, the
            // generation job's own per-direction product (stage 5's read B2: never 1.414 x the largest count)
            const uint32 field[4] = { mSettings.mNumProbes[0], mSettings.mNumProbes[0] * mSettings.mNumProbes[1],
                                      mSettings.mDepthProbeResolution, mDepthVarianceTex->getWidth() };
            const Vector3 sp = getProbeSpacing();
            const float spacing[4] = { sp.x, sp.y, sp.z, 1.414213562f };
            set( "caField", field, false );
            set( "caSpacing", spacing, true );
            // ...and the irradiance tile's interior and atlas width; the probes along z (x and y are caField's)
            const uint32 field2[4] = { mSettings.mIrradianceResolution, mIrradianceTex->getWidth(),
                                       mSettings.mNumProbes[2], 0u };
            set( "caField2", field2, false );
        }
        const uint32 reduce[4] = { 2u, probes, probes, 0u };
        set( "caMode", reduce, false );
        set( "caSelf", self, true );
        set( "caDims", d, false );
        set( "caInnerLo", lo, true );
        set( "caInnerHi", hi, true );
        params.setDirty();
        mAccumJob->setNumThreadGroupsBasedOn( HlmsComputeJob::ThreadGroupsBasedOnNothing, 0u, 1u, 1u, 1u );
        mAccumJob->setNumThreadGroups( ( clusters + 63u ) / 64u, 1u, 1u );
        jahshaka::engine::detail::photonDispatch( hc, mAccumJob, rs );
        const uint32 mode[4] = { 0u, probes, probes, 0u };
        set( "caMode", mode, false );
        params.setDirty();
        mAccumJob->setNumThreadGroups( ( probes + 63u ) / 64u, 1u, 1u );
        jahshaka::engine::detail::photonDispatch( hc, mAccumJob, rs );
        mMarkRan = true;
        const DescriptorSetUav::BufferSlot empty = DescriptorSetUav::BufferSlot::makeEmpty();
        for( uint8 k = 0u; k < 5u; ++k )
            mAccumJob->_setUavBuffer( k, empty );
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::endMarkFrame( uint64 frame )
    {
        if( !mMarkReport || mMarkTickets.empty() || ( !mMarkRan && !mMarkKeepReport ) )
            return;
        mMarkRan = false;
        MarkTicket &t = mMarkTickets[mMarkNextIssue];
        if( t.ticket || ( mMarkRingHold && ( frame & 1u ) ) )
        {
            // EVERY SLOT IN FLIGHT: the report is KEPT for the next ticket (the ring's comment, PhotonIrradianceField.h)
            if( !mMarkKeepReport )
                ++mReportsKept;
            mMarkKeepReport = true;
            return;
        }
        t.ticket = mMarkReport->readRequest( 0u, 1u + mHeld.size(), false );
        t.frame = frame;
        mMarkNextIssue = ( mMarkNextIssue + 1u ) % uint32( mMarkTickets.size() );
        mMarkKeepReport = false;
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::harvestMarks( uint64 frame )
    {
        VaoManager *vao = mRoot->getRenderSystem()->getVaoManager();
        for( ; !mMarkTickets.empty(); )
        {
            MarkTicket &t = mMarkTickets[mMarkNextTake];
            if( !t.ticket )
                return;
            // TAKEN AT A FIXED LAG OF HOST FRAMES (the feedback readings' rule): a function of the frames, never of the
            // GPU's pace.
            if( frame < t.frame || frame - t.frame <= vao->getDynamicBufferMultiplier() )
                return;
            const uint32 *w = static_cast<const uint32 *>( t.ticket->map() );
            const uint32 n = std::min<uint32>( w[0], uint32( mHeld.size() ) );
            for( uint32 k = 0u; k < n; ++k )
            {
                const uint32 slot = w[1u + k];
                if( slot >= mOwedRestart.size() || !mHeld[slot] || mOwedRestart[slot] )
                    continue;
                mOwedRestart[slot] = 1u;
                ++mOwedRestarts;
                ++mMarkedByMoves;
            }
            t.ticket->unmap();
            t.ticket.reset();
            mMarkNextTake = ( mMarkNextTake + 1u ) % uint32( mMarkTickets.size() );
        }
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
        // done once the device finished that frame. The counters and the per-slot stop flags
        // (PHOTON-7 stage 5, 5-4: a refinement list leaves a stopped probe out).
        mVarianceTicket = mVarianceBuffer->readRequest( 0u, 4u + mSettings.getTotalNumProbes(), false );
        mVarianceTicketFrame = mRoot->getRenderSystem()->getVaoManager()->getFrameCount();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::harvestVarianceReadout()
    {
        // A FIXED LATENCY IN FRAMES, never "when the device happens to finish": the reading drives the
        // refinement lists and the gather's rest key, and a frame that depended on the GPU's speed would make the
        // picture of a given frame differ from run to run (the selftest's mover pose did: two hashes from one
        // tip). Taken exactly when every frame in flight since the request has been waited on - the transfer is
        // done by then (Ogre fences the frame before reusing its dynamic buffers), so the map never stalls.
        if( !mVarianceTicket )
            return;
        VaoManager *vaoManager = mRoot->getRenderSystem()->getVaoManager();
        if( vaoManager->getFrameCount() - mVarianceTicketFrame <= uint32( vaoManager->getDynamicBufferMultiplier() ) )
            return;
        const uint32 *w = static_cast<const uint32 *>( mVarianceTicket->map() );
        mIntegrations = w[2];
        // THE STOPS, trusted for a slot whose estimate was last replaced before the request (stoppedTrusted): a
        // replacement in the request's frame or after clears the flag the reading may still show.
        const size_t probes = std::min<size_t>( mStopped.size(), mSettings.getTotalNumProbes() );
        for( size_t slot = 0; slot < probes; ++slot )
            mStopped[slot] = w[4u + slot] != 0u ? 1u : 0u;
        mStopsReadFrame = mVarianceTicketFrame;
        mVarianceTicket->unmap();
        mVarianceTicket.reset();
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
        mWorkIsList = false;
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
        mWorkIsList = false;
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
        // (ITS REFINEMENTS ARE THE LEDGER'S, PHOTON-7 stage 5, 5-4: the entered probes are below the target once
        // their fresh pass is done, and beginNextWork refines every such probe - no band to join, no whole grid owed.)
        // THE WORK: the entered planes, fresh and getChangeSamples() deep (5-5: they fill in the guard band, unread).
        setBoxesFromBands( enteredLo, enteredNum );
        mNumProbesProcessed = 0u;
        // The entered planes have no history (PHOTON-FIELD-ROTATE-1), and they owe the
        // refinements that bring them to the target.
        mWorkMode = IntegrateFresh;
        // The probe-to-voxel transform reads the (moved) origin.
        if( mVctLighting )
            setIrradianceFieldGenParams();
        // Jahshaka (PHOTON-VIEW-1): and the visualizer's WINDOW OFFSET - its spheres are
        // drawn by atlas slot, and a scroll moved every probe's slot.
        if( mDebugIfdProbeVisualizer )
            placeDebugVisualizer();
        // ...and the accumulation's receivers (every slot's probe point: the entered planes' moved)
        writeProbeRecords();
    }
    //-------------------------------------------------------------------------
    void PhotonIrradianceField::update( uint32 probesPerFrame )
    {
        // Jahshaka (PHOTON-WRITER-1): the WORK, not the whole grid.
        const uint32 totalNumProbes = mWorkTotal;
        if( mNumProbesProcessed >= totalNumProbes )
            return;

        probesPerFrame = std::min( totalNumProbes - mNumProbesProcessed, probesPerFrame );
        const uint32 listStart = mNumProbesProcessed;

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
            fillProbeTransform();
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
            // (5-4) THE LIST: the work buffer's slots instead of the boxes; and where the marks begin
            mIfGenParams.boxSize[0].w = mWorkIsList ? 1u : 0u;
            mIfGenParams.numProbes.w = mMarkStart;
            mIfGenParams.windowOffset.x = mWindowOffset[0];
            mIfGenParams.windowOffset.y = mWindowOffset[1];
            mIfGenParams.windowOffset.z = mWindowOffset[2];
            // THE WORK'S END for this dispatch, in list positions: a group past it
            // writes nothing (the dispatch is rounded up to whole rows).
            mIfGenParams.windowOffset.w = listStart + probesPerFrame;
        }
        if( !mInSubPass )
            noteWork( listStart, probesPerFrame );
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
            mIfGenParams.latticeOrigin.w = 0u;   // (unused)
            mIfGenParams.sweep.x = static_cast<uint32>( mWorkMode );
            mIfGenParams.sweep.y = mTargetSamples;
            mIfGenParams.sweep.z = mKeepOnChange;
            mIfGenParams.sweep.w = mRotateRays ? 1u : 0u;
            mIfGenParams.varianceStop = float4(
                Vector4( mStopPrecision, static_cast<Real>( mStopMinSamples ), mStopMeanFloor, 0.0f ) );
        }
        mIfGenParams.probesPerRow = numThreadGroupsX;  // There's one probe per group
        *ifGenParams = mIfGenParams;

        paramsBuffer->unmap( UO_KEEP_PERSISTENT );

        dispatchGeneration();

        mNumProbesProcessed += probesPerFrame;

        // Jahshaka (PHOTON-2 stage 2a; its round's K2): A CHANGE'S OR A FRESH INTEGRATION'S BATCH TAKES
        // mChangeSamples INTEGRATIONS NOW (setChangeSamples) - the same probes, refined in place, before the
        // batch is read: a re-placement, a build, an entered plane and a change alike never leave a
        // one-sample probe.
        if( ( mWorkMode == IntegrateChange || mWorkMode == IntegrateFresh ) && mChangeSamples > 1u )
        {
            // Sub-pass i lifts a probe to i + 1 samples and no further (its target, for the pass); the ledger
            // counted the pass's samples once (noteWork).
            const IntegrationMode mode = mWorkMode;
            const uint32 target = mTargetSamples;
            const uint32 start = mNumProbesProcessed - probesPerFrame;
            mInSubPass = true;
            for( uint32 i = 1u; i < mChangeSamples; ++i )
            {
                mNumProbesProcessed = start;
                mWorkMode = IntegrateRefine;
                mTargetSamples = std::min( target, i + 1u );
                update( probesPerFrame );
            }
            mInSubPass = false;
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
        // ...and THE RADIANCE CACHE's atlas, where there is one (the job's slot 4; written whole, borders and chain
        // included, by the probe's own group)
        uav.texture = mRadianceTex;
        if( mRadianceTex )
            mGenerationJob->_setUavTexture( 4, uav );
        DescriptorSetUav::BufferSlot variance( DescriptorSetUav::BufferSlot::makeEmpty() );
        variance.buffer = mVarianceBuffer;
        variance.access = ResourceAccess::ReadWrite;
        mGenerationJob->_setUavBuffer( 2, variance );
        // ...and THE WORK LIST (PHOTON-7 stage 5, 5-4: read when the work is a list; bound always)
        DescriptorSetUav::BufferSlot work( DescriptorSetUav::BufferSlot::makeEmpty() );
        work.buffer = mWorkListBuffer;
        work.access = ResourceAccess::Read;
        mGenerationJob->_setUavBuffer( 3, work );
        jahshaka::engine::detail::photonDispatch( hlmsCompute, mGenerationJob, renderSystem );
        // The descriptor holds a raw pointer a rebuild destroys: nothing stays bound.
        mGenerationJob->_setUavBuffer( 2, DescriptorSetUav::BufferSlot::makeEmpty() );
        mGenerationJob->_setUavBuffer( 3, DescriptorSetUav::BufferSlot::makeEmpty() );
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
