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

#ifndef JAH_PHOTON_IRRADIANCE_FIELD_H
#define JAH_PHOTON_IRRADIANCE_FIELD_H

#include "OgreHlmsPbsPrerequisites.h"

#include "Math/Simple/OgreAabb.h"
#include "OgreId.h"
#include "OgreIdString.h"
#include "OgrePixelFormatGpu.h"
#include "OgreShaderPrimitives.h"

#include "ogrestd/vector.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonIfdProbeVisualizer;
    class PhotonVoxelLighting;  // (was HlmsPbs's prerequisites' forward declaration)

    struct PhotonIrradianceFieldSettings
    {
        /** Number of rays per pixel in terms of mDepthProbeResolution.

            Jahshaka (PHOTON-FIELD-ROTATE-1): the probe shoots
            mDepthProbeResolution^2 * mNumRaysPerPixel rays (getNumRaysPerProbe) in a
            spherical-Fibonacci set rotated per integration, and EVERY texel of both
            atlases integrates ALL of them (DDGI's estimator) - a ray belongs to no
            texel. At most 1024 rays a probe (one work group per probe).
        */
        uint16 mNumRaysPerPixel;
        /// Square resolution of a single probe, depth variance, e.g. 8u means each probe is 8x8.
        uint8 mDepthProbeResolution;
        /// Square resolution of a single probe, irradiance e.g. 4u means each probe is 4x4.
        /// Must be mIrradianceResolution <= mDepthProbeResolution
        /// mDepthProbeResolution must be multiple of mIrradianceResolution
        uint8 mIrradianceResolution;

        /// Number of probes in all three XYZ axes.
        /// Must be power of two
        uint32 mNumProbes[3];

    public:
        PhotonIrradianceFieldSettings();

        void testValidity()
        {
            OGRE_ASSERT_LOW( mIrradianceResolution <= mDepthProbeResolution );
            OGRE_ASSERT_LOW( ( mDepthProbeResolution % mIrradianceResolution ) == 0u );
            for( size_t i = 0u; i < 3u; ++i )
            {
                OGRE_ASSERT_LOW( ( mNumProbes[i] & ( mNumProbes[i] - 1u ) ) == 0u &&
                                 "Num probes must be a power of 2" );
            }
        }

        uint32 getTotalNumProbes() const;

        void getDepthProbeFullResolution( uint32 &outWidth, uint32 &outHeight ) const;
        void getIrradProbeFullResolution( uint32 &outWidth, uint32 &outHeight ) const;

        /// Returns mIrradianceResolution + 2u, since we need to reserverve 1 pixel border
        /// around the probe for proper interpolation.
        /// This means a 8x8 probe actually occupies 10x10.
        uint8 getBorderedIrradResolution() const;
        uint8 getBorderedDepthResolution() const;

        /// Jahshaka (PHOTON-FIELD-ROTATE-1): mDepthProbeResolution^2 * mNumRaysPerPixel.
        uint32 getNumRaysPerProbe() const;

        Vector3 getNumProbes3f() const;
    };

    /**
    @class PhotonIrradianceField
        Implements an Irradiance Field with Depth, inspired on DDGI

        We use the voxelized results from VCT.
        Afterwards once we have Raytracing, we'll also allow to use Raytracing instead;
        since both are very similars (with VCT we shoot cones instead of rays)

    @see
        Snippet taken from Dynamic Diffuse Global Illumination with Ray-Traced Irradiance Fields
        Zander Majercik, NVIDIA; Jean-Philippe Guertin, Université de Montréal;
        Derek Nowrouzezahrai, McGill University; Morgan McGuire, NVIDIA and McGill University
        http://jcgt.org/published/0008/02/01/

    @see
        https://github.com/OGRECave/ogre-next/issues/29
    */
    class PhotonIrradianceField : public IdObject
    {
    public:
        enum DebugVisualizationMode
        {
            DebugVisualizationColour,
            DebugVisualizationDepth,
            DebugVisualizationNone
        };

    public:
        /// The longest cascade chain the generation job walks (PhotonVoxelLighting supports
        /// fewer than this). The shader's parameter block is sized by it.
        static constexpr uint32 kMaxChainCascades = 8u;

        /** Jahshaka (PHOTON-FIELD-ROTATE-1): HOW AN INTEGRATION TREATS A PROBE'S HISTORY.

            A probe's value is the MEAN of its integrations since its history last
            restarted: integration m (0-based) blends with weight 1 / (m + 1), and the
            count m lives in the irradiance atlas's alpha (so the pixel's reader knows a
            probe that has never been integrated: count 0). Each integration shoots the
            probe's ray set under a fresh random rotation, so the mean converges on the
            irradiance instead of on one fixed set's aliasing.
        */
        enum IntegrationMode
        {
            /// The probe has no history (a build, a re-placement, a probe that entered
            /// the window): the integration IS the value, and the count restarts at 1.
            IntegrateFresh,
            /// The radiance the probe integrates changed (a light, a re-voxelisation in
            /// place): the history keeps at most `keepOnChange` samples' weight.
            IntegrateChange,
            /// Nothing changed: a probe below the target sample count takes one more
            /// sample, one at the target is skipped (its work group exits before a ray).
            IntegrateRefine,
            /// No rays: every probe of the work gets sample count 0 and value 0 (a
            /// re-placement's invalidation; the generation job's own dispatch, so no
            /// other shader is ever compiled for it).
            IntegrateInvalidate
        };

    protected:
        struct IrradianceFieldGenParams
        {
            // Jahshaka (PHOTON-FIELD-ROTATE-1): upstream's per-texel ray counts
            // (invNumRaysPerPixel / invNumRaysPerIrradiancePixel), the old cone start
            // bias pair and the threads-per-row packing are gone - nothing reads them.
            // One float4 of scalars, then the counts (w unused).
            uint32 probesPerRow = 0u;  // groups per dispatch row (one probe per group)
            float  coneAngleTan = 0.0f;
            uint32 numProcessedProbes = 0u;
            uint32 padding0 = 0u;

            uint4 numProbes;

            // Jahshaka (PHOTON-WRITER-1): THE WORK, as up to three disjoint boxes of
            // probe SLOTS (xyz = the box's first slot per axis, wrapping at the
            // probe count; w = the box's first position in the work list, all ones
            // for an unused box) and their sizes (xyz; w unused). A list position
            // p maps to the box it falls in and from there to a slot. The whole
            // field is one box at slot 0 with the grid's own size.
            uint4 boxLo[3];
            uint4 boxSize[3];
            // ...and THE WINDOW: the field is a toroidal window over a probe lattice
            // fixed in the world, and a probe's SLOT (its atlas tile) is its
            // lattice coordinate modulo the probe count; xyz is the slot of the
            // window's first probe (the window-local probe i lives at slot
            // (i + offset) mod N). w unused.
            uint4 windowOffset;

            float4x4 irrProbeToVctTransform;

            // Jahshaka (PHOTON-READER-1): THE CASCADE CHAIN the probe rays walk, in the
            // layout PhotonVoxelLighting::getCascadeChainParams writes and the pixel shader's
            // pass buffer carries. Appended, so the integration job's declaration of
            // this buffer (the members above) is still a prefix of it.
            float4 vctInvResMaxLod[kMaxChainCascades];
            float4 vctFromPrev[( kMaxChainCascades - 1u ) * 2u];

            // Jahshaka (PHOTON-ENV-1): THE ONE ENVIRONMENT the probe rays escape to,
            // in cascade 0's stored units (fillEnvironmentParams): rgb gain, w mips;
            // then the nine SH coefficients, world axes (w unused).
            float4 envGainMips;
            float4 envSh[9];

            // Jahshaka (PHOTON-FIELD-ROTATE-1): THE WINDOW'S PLACE ON THE WORLD LATTICE -
            // xyz: the world lattice coordinate of window-local probe 0 (round( field
            // origin / spacing )), so a probe's lattice coordinate is this plus its
            // window-local one; the job seeds each probe's ray rotation from that
            // coordinate and the probe's own sample index. w unused. And the history
            // rule: x = IntegrationMode, y = the target sample count, z =
            // keepOnChange, w = 1 to rotate the rays (0: the static measurement arm).
            uint4 latticeOrigin;
            uint4 sweep;
            // Jahshaka (PHOTON-VOXEL-4, FIELD-DIR-1): a WORLD direction to cascade 0's
            // normalised space, per axis - min( box size ) / box size (xyz; w unused).
            // A probe ray is a world direction; the march walks the volume's normalised
            // space, where a direction is the world one scaled by 1 / the box's size. All
            // ones in a cubic box (every cascade of the chain), which the job reads as "no
            // transform" so a cube's rays are the bits they always were.
            float4 worldToVolumeDir;
        };

        struct IfdBorderMirrorParams
        {
            uint32 probeBorderedRes = 0u;
            uint32 numPixelsInEdges = 0u;
            uint32 numTopBottomPixels = 0u;
            uint32 numGlobalThreadsForEdges = 0u;

            uint32 maxThreadId = 0u;
            uint32 threadsPerThreadRow = 0u;
            uint32 padding[2] = {};
        };

        PhotonIrradianceFieldSettings mSettings;
        /// Number of probes processed so far, as a position in the WORK (the boxes
        /// below). We process the work across multiple frames.
        uint32 mNumProbesProcessed;

        /// Jahshaka (PHOTON-WRITER-1): the work the next update() calls walk - up to
        /// three disjoint boxes of probe slots (reset(): the whole grid; a scroll:
        /// the planes that entered the window) - and its probe count.
        struct ProbeBox
        {
            uint32 lo[3] = {};
            uint32 size[3] = {};
        };
        ProbeBox mWorkBoxes[3];
        uint32   mNumWorkBoxes;
        uint32   mWorkTotal;
        /// The slot of the window's first probe, per axis (see IrradianceFieldGenParams).
        uint32 mWindowOffset[3];

        void setWholeWork();

        /// Jahshaka (PHOTON-FIELD-ROTATE-1): the history rule of the current work, and
        /// the policy it is applied with (setIntegrationPolicy).
        IntegrationMode mWorkMode;
        uint32          mTargetSamples;
        uint32          mKeepOnChange;
        bool            mRotateRays;
        /// Whole-grid refinements owed after the current work (every event - a build,
        /// a re-placement, a scroll, a change - owes mTargetSamples - 1 of them).
        uint32 mRefinesOwed;
        /// Jahshaka (PHOTON-FIELD-ROTATE-1): every probe's sample count to 0 - the
        /// pixel's reader weights a probe by its count (saturated), so a probe no
        /// integration has reached since is no probe at all and the reader's
        /// fallback answers its pixels.
        void invalidateAllProbes();

        Vector3 mFieldOrigin;
        Vector3 mFieldSize;

        PhotonVoxelLighting *mVctLighting;

        TextureGpu *mIrradianceTex;
        TextureGpu *mDepthVarianceTex;

        CompositorWorkspace *mGenerationWorkspace;
        HlmsComputeJob      *mGenerationJob;
        HlmsComputeJob      *mDepthMirrorBorderJob;
        HlmsComputeJob      *mColourMirrorBorderJob;

        IrradianceFieldGenParams mIfGenParams;
        ConstBufferPacked       *mIfGenParamsBuffer;
        /// Jahshaka (PHOTON-FIELD-ROTATE-1): ONE PARAMETER BUFFER PER DISPATCH OF A FRAME.
        /// A dynamic buffer maps ONCE a frame (BufferPacked::map), and the dispatches
        /// of one frame execute after it has been recorded, so every update() of a
        /// frame but the last ran with the LAST one's parameters - its work, its ray
        /// rotation, its history rule - as soon as a frame integrated twice (an inline
        /// convergence and its refinements, a scroll and a refinement). The ring hands
        /// each dispatch of a frame its own buffer; [0] is mIfGenParamsBuffer.
        vector<ConstBufferPacked *>::type mIfGenParamsRing;
        uint32                            mIfGenParamsRingFrame;
        uint32                            mIfGenParamsRingNext;
        TexBufferPacked         *mDirectionsBuffer;
        ConstBufferPacked       *mIfdDepthBorderMirrorParamsBuffer;
        ConstBufferPacked       *mIfdColourBorderMirrorParamsBuffer;

        DebugVisualizationMode mDebugVisualizationMode;
        uint8                  mDebugTessellation;
        PhotonIfdProbeVisualizer    *mDebugIfdProbeVisualizer;

        Root         *mRoot;
        SceneManager *mSceneManager;

        void fillDirections( float *RESTRICT_ALIAS outBuffer );

        void setIrradianceFieldGenParams();

        void setupBorderMirrorParams( uint32 borderedRes, uint32 fullWidth,
                                      ConstBufferPacked *ifdBorderMirrorParamsBuffer,
                                      HlmsComputeJob    *job );

        void setTextureToDebugVisualizer();
        /// Jahshaka (PHOTON-VIEW-1): the debug visualizer's static node at the field's
        /// origin and probe spacing, marked dirty for the scene manager, and its window
        /// offset - every place the field is (re)placed or scrolls calls it.
        void placeDebugVisualizer();

        /// Jahshaka (PHOTON-READER-1): binds EVERY cascade's light volumes to the
        /// generation job, from the pointers the chain holds NOW. Called before every
        /// dispatch: a cascade re-creates its volumes on a rebuild or a bounce
        /// ping-pong without telling its readers (the bounce job re-asserts its own
        /// bindings per dispatch for the same reason).
        void bindChainToGenerationJob();
        /// ...and the chain's parameters, from PhotonVoxelLighting's one definition of them.
        void fillChainParams();
        /// Jahshaka (PHOTON-ENV-1): the environment of the bound lighting, per dispatch.
        void fillEnvironmentParams();

    public:
        PhotonIrradianceField( Root *root, SceneManager *sceneManager );
        ~PhotonIrradianceField();

        void createTextures();
        void destroyTextures();

        /**
        @brief initialize
        @param settings
        @param fieldOrigin
        @param fieldSize
        @param vctLighting
            Must be non-null: the field's rays read radiance from it
         */
        void initialize( const PhotonIrradianceFieldSettings &settings, const Vector3 &fieldOrigin,
                         const Vector3 &fieldSize, PhotonVoxelLighting *vctLighting );

        /** Moves (and/or resizes) the field's volume WITHOUT re-creating its textures.

            initialize() is the only way to place the field upstream, and it calls
            createTextures(), which destroys and re-creates both atlases: a field that
            must follow the camera (e.g. one riding the innermost cascade of a
            camera-centred cascade chain) would therefore be born black on every step and
            re-converge from nothing, and its compositor workspace and directions buffer
            would be rebuilt for a change neither depends on.

            Everything that reads the placement reads it LIVE — fillConstBufferData()
            rebuilds the pixel transform per pass and the generation params below hold the
            probe-to-voxel transform — so moving the volume is exactly these two members
            plus a re-derivation of those params.

            The probe COUNTS are settings and are not touched, so the atlases stay valid.
            The atlases keep the irradiance integrated at the PREVIOUS placement: the
            caller decides whether to reset() and re-converge progressively over it or to
            converge the whole field in one update() before the next pass reads it.
        @remarks
            Must not be called before initialize().
        @param fieldOrigin
            The volume's origin, in the same units and with the same meaning initialize()
            gives it (it is enlarged by one probe block per side here too).
        @param fieldSize
            The volume's size, same meaning as initialize()'s.
        @remarks
            Jahshaka (PHOTON-FIELD-ROTATE-1): the voxel path INVALIDATES every probe
            here (its count is cleared to 0: nothing the atlas holds describes the new
            placement), so the caller may integrate the new placement over as many
            update() calls - frames - as it likes: until a probe's first integration the
            pixel's reader gives it no weight and falls back.
        */
        void setFieldVolume( const Vector3 &fieldOrigin, const Vector3 &fieldSize );

        /** Jahshaka (PHOTON-WRITER-1): SCROLLS the field's window by whole probe
            spacings, keeping every probe that stays inside it.

            The field is a toroidal window over a probe lattice fixed in the world: a
            probe's atlas tile is its lattice coordinate modulo the probe count, so a
            move of d spacings on an axis leaves N - |d| planes of probes exactly where
            they were in the atlas and in the world - their values stand - and hands
            the tiles of the |d| planes that left the window to the |d| planes that
            entered it on the far side. Those are the only ones this makes the next
            update() integrate: the work becomes the entered planes (at most three
            disjoint boxes, one per moved axis), and a caller that must show them at
            once converges them inline with update( getWorkProbeCount() ).
        @remarks
            A move of N or more spacings on any axis keeps nothing: use
            setFieldVolume() and converge the whole field. The window's authored origin
            moves by exactly d spacings; the caller owns snapping a target to it.
        @param delta
            The move, in probe spacings, per axis.
        */
        void scrollWindow( const int32 delta[3] );
        /// The slot of the window's first probe per axis (a reader's modulo offset).
        const uint32 *getWindowOffset() const { return mWindowOffset; }
        /// The probe spacing (the enlarged volume's size over the probe counts).
        Vector3 getProbeSpacing() const;
        /// How many probes the current work holds (the whole grid after reset()).
        uint32 getWorkProbeCount() const { return mWorkTotal; }

        /** Re-points the field at a PhotonVoxelLighting, and re-binds the generation job to the
            light voxel textures that object owns RIGHT NOW.

            PhotonVoxelLighting re-creates its light voxel textures whenever its voxelizer's
            textures change — on a LostResidency/Deleted notification (checkTextures()),
            and, since PhotonVoxelLighting::setVoxelizer() exists, whenever it is moved to a
            replacement voxelizer. The PhotonIrradianceField bound those textures ONCE, in
            initialize(), by pointer: after any such re-creation its generation job holds
            pointers to destroyed textures and the next update() integrates from freed
            GPU memory (or from whatever the allocator has since handed the address to).
            There is no notification from PhotonVoxelLighting to its readers, so this is the hook
            that answers it; passing the SAME PhotonVoxelLighting is meaningful and re-binds.

            Cheaper than initialize() and it keeps the atlases: nothing about the field's
            geometry has changed, only where its rays read radiance from.
        @remarks
            Must not be called before initialize() — it is a no-op then.
        */
        void setVctLighting( PhotonVoxelLighting *vctLighting );

        /// If PhotonVoxelLighting was updated with minor changes (e.g. light position/direction changed,
        /// number of bounces setting changed) then call this function so update() process it
        /// again.
        ///
        /// If major changes happens to PhotonVoxelLighting, then call initialize() again
        ///
        /// Jahshaka (PHOTON-FIELD-ROTATE-1): the whole grid, as a CHANGE (the history
        /// keeps at most keepOnChange samples' weight).
        void reset();

        /** Jahshaka (PHOTON-FIELD-ROTATE-1): the whole grid again, as a REFINEMENT: every
            probe below the target sample count takes one more sample under a new ray
            rotation; a probe at the target costs its work group one texel read.
        */
        void refine();

        /** Jahshaka (PHOTON-FIELD-ROTATE-1): the estimator's history rule.
        @param targetSamples
            How many integrations a probe's mean takes before a refinement leaves it
            alone (>= 1).
        @param keepOnChange
            How many samples' weight the history keeps when the radiance changed
            (0 = the first integration after a change replaces the probe's value).
        @param rotateRays
            False shoots the same ray set every integration (a MEASUREMENT arm: the
            static set's aliasing).
        */
        void setIntegrationPolicy( uint32 targetSamples, uint32 keepOnChange, bool rotateRays );
        uint32 getTargetSamples() const { return mTargetSamples; }
        IntegrationMode getWorkMode() const { return mWorkMode; }
        /// Every probe of the current work has been integrated once.
        bool isWorkDone() const { return mNumProbesProcessed >= mWorkTotal; }
        /// Probes of the current work not integrated yet.
        uint32 getWorkRemaining() const
        {
            return mNumProbesProcessed >= mWorkTotal ? 0u : mWorkTotal - mNumProbesProcessed;
        }
        /** Jahshaka (PHOTON-FIELD-ROTATE-1): THE FIELD'S CONVERGENCE SCHEDULE. Every event
            that gives probes a new sample - initialize(), setFieldVolume(), scrollWindow(),
            reset() - owes mTargetSamples - 1 whole-grid refinements after its own work;
            once the current work is done this starts the next one (refine()) and returns
            true, or returns false when none is owed (the field has converged and costs
            nothing until the next event).
        */
        bool beginOwedRefinement();
        uint32 getRefinesOwed() const { return mRefinesOwed; }

        void update( uint32 probesPerFrame = 200u );

        size_t getConstBufferSize() const;
        void fillConstBufferData( const Matrix4 &viewMatrix, float *RESTRICT_ALIAS passBufferPtr ) const;

        /**
        @param mode
        @param sceneManager
            Can be nullptr only if mode == PhotonIrradianceField::DebugVisualizationNone
        @param tessellation
            Value in range [3; 16]
            Note this value increases exponentially:
                tessellation = 3u -> 24 vertices (per sphere)
                tessellation = 4u -> 112 vertices
                tessellation = 5u -> 480 vertices
                tessellation = 6u -> 1984 vertices
                tessellation = 7u -> 8064 vertices
                tessellation = 8u -> 32512 vertices
                tessellation = 9u -> 130560 vertices
                tessellation = 16u -> 2.147.418.112 vertices
        */
        void  setDebugVisualization( PhotonIrradianceField::DebugVisualizationMode mode,
                                     SceneManager *sceneManager, uint8 tessellation );
        /// Jahshaka (PHOTON-VIEW-1): the MODE (upstream returned it as a bool, so
        /// DebugVisualizationColour - 0 - read as "off").
        DebugVisualizationMode getDebugVisualizationMode() const;
        uint8                  getDebugTessellation() const;
        /// Jahshaka (PHOTON-VIEW-1): the visualizer setDebugVisualization created (null
        /// while the mode is DebugVisualizationNone) - a host puts it on its own
        /// visibility channel or render queue without searching the scene graph for it.
        PhotonIfdProbeVisualizer *getDebugVisualizer() const { return mDebugIfdProbeVisualizer; }

        TextureGpu *getIrradianceTex() const { return mIrradianceTex; }
        TextureGpu *getDepthVarianceTex() const { return mDepthVarianceTex; }
        /// Jahshaka (OWN-GI-1): the textures a PBS pass reads (the two above).
        static constexpr uint32 kPassTextures = 2u;
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
