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
#ifndef JAH_PHOTON_VOXEL_LIGHTING_H
#define JAH_PHOTON_VOXEL_LIGHTING_H

#include "OgreHlmsPbsPrerequisites.h"

#include "OgreId.h"
#include "OgreResourceTransition.h"
#include "OgreShaderParams.h"
#include "OgreTextureGpuListener.h"
#include "Vao/OgreAsyncTicket.h"

#include <algorithm>
#include <vector>

#include "SceneLightList.h"

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelizerSourceBase;
    class PhotonVoxelVisualizer;
    struct PhotonShaderVoxelLight;

    /** THE COMMIT'S LIGHT (PHOTON-7 stage 2, AB S4/A7; PREFLIGHT_1_2 W1): what the next update() does at a scroll's
        commit, after the store and the light were shifted in place.
        `delta`: the direct term is re-injected AS A DELTA - total += D_new - D_old and back += Db_new - Db_old (the
        direct terms written whole as always) - so the bounces the shifted total holds are kept and a voxel whose
        direct did not change keeps every bit; a slab voxel's old terms are the shift's zeros. The voxels written
        are COUNTED (takeCommitCount): those outside [keepLo, keepHi) (the slab) and those inside (the changed
        direct set: the band).
        `injectBoxes`: the direct term is computed over these boxes of cells only (each aligned to the injection's
        4^3 groups; they may overlap - a delta is idempotent): the committed cascade's CHANGED-DIRECT SET lies
        within its own-cell march of the two faces a shift moved (the slab's side and the trailing face); none =
        the whole volume (a cascade inside the committed one: its outer shells cross the committed cells anywhere).
        `region`: the sky pass and the bounce passes run over the cells [regionLo, regionHi) only (the slab and
        its bounce reach, aligned to 8 cells), each written into the bounce target and copied back, and the
        directional levels regenerated over what covers the region.
        Null: a whole injection (every other update). Cleared by the update it shapes. */
    struct PhotonCommitLight
    {
        bool delta = false;
        uint32 keepLo[3] = { 0u, 0u, 0u }, keepHi[3] = { 0u, 0u, 0u };
        static constexpr uint32 kMaxInjectBoxes = 6u;   ///< two faces per moved axis
        uint32 numInjectBoxes = 0u;
        uint32 injectLo[kMaxInjectBoxes][3] = {}, injectHi[kMaxInjectBoxes][3] = {};
        bool region = false;
        uint32 regionLo[3] = { 0u, 0u, 0u }, regionHi[3] = { 0u, 0u, 0u };
    };

    class PhotonVoxelLighting : public IdObject, public TextureGpuListener
    {
    public:

    protected:
        /// mLightVoxel[0] contains mip 0 (and its whole chain only under the specular SDF
        /// quality, which the empty-space skip reads) - ONE LIGHT STORE AT EVERY TIER
        /// (PHOTON-MARCH-1: the isotropic store Low kept is deleted; its colour per unit of
        /// opacity mixed every surface in a texel, a wall read 0.87 L beside a floor):
        ///  * mLightVoxel[1] contains mipmaps in -X and +X
        ///  * mLightVoxel[2] contains mipmaps in -Y and +Y
        ///  * mLightVoxel[3] contains mipmaps in -Z and +Z
        ///
        /// The negative axis is in
        ///     [0; mLightVoxel[1]->getWidth() / 2)
        /// and the positive axis is in
        ///     [mLightVoxel[1]->getWidth() / 2; mLightVoxel[1]->getWidth())
        ///
        /// We don't put all mips in the same texture (i.e. making mLightVoxel[2]) because we
        /// would need mLightVoxel[1] to be of resolution:
        ///     mLightVoxel[1].width  = mLightVoxel[0].width
        ///     mLightVoxel[1].height = mLightVoxel[0].height / 2
        ///     mLightVoxel[1].depth  = mLightVoxel[0].depth * 1.5
        ///
        /// Since most GPUs out there only support up to 2048 resolution in any axis,
        /// we wouldn't be able to support anisotropic mips for high resolution voxels.
        /// But more importantly, we would waste 1/4th of memory (actually 1/2 of memory
        /// because GPUs like GCN round memory consumption to the next power of 2).
        ///
        /// Jahshaka (PHOTON-VOXEL-3/-4): mLightVoxel[4..7] are the voxelizer's COVERAGE PER HALF-AXIS
        /// (coverageIndex(0 / 1): the faces looking +a / -a) and its SURFACE POSITION per
        /// half (positionIndex(0 / 1)), NOT owned here: every reader of the light volumes
        /// reads the opacity along its ray from the coverage and the origin plane from the
        /// position, so they ride the same list and every reader that binds
        /// getNumVoxelTextures() volumes per cascade binds them too.
        ///
        /// Jahshaka (PHOTON-VOXEL-5): two more follow - mLightVoxel[8],
        /// level 0's BACK side (owned; backIndex()), and mLightVoxel[9], the voxelizer's NORMAL
        /// (not owned; normalIndex()): level 0 holds the MEAN of a voxel's two sides
        /// (mLightVoxel[0]) and its back, and the normal says which side a half-axis's faces look
        /// to. The directional volumes' level 0 is composited by the light injection itself from
        /// each voxel's light PER HALF-AXIS (the fused step 0).
        TextureGpu             *mLightVoxel[10];
        HlmsSamplerblock const *mSamplerblockTrilinear;

        PhotonVoxelizerSourceBase *mVoxelizer;
        bool                    mVoxelizerTexturesChanged;
        bool                    mVoxelizerListenersRemoved;

        HlmsComputeJob    *mLightInjectionJob;
        ConstBufferPacked *mLightsConstBuffer;

        /// Anisotropic mipmap generation consists of 2 main steps:
        ///
        /// Step 1 takes mLightVoxel[0] and computes
        /// mLightVoxel[1].mip[0], mLightVoxel[2].mip[0] & mLightVoxel[3].mip[0]
        ///
        /// Step 2 takes mLightVoxel[i].mip[n] and computes mLightVoxel[i].mip[n+1]
        /// where i is in range [1; 3] and n is the number of mipmaps in those textures.
        /// Jahshaka (PHOTON-VOXEL-5): step 0 is the BOUNCE'S part of the directional level 0 -
        /// the injection composites the direct part itself (into mLightDirectDir when a bounce
        /// follows); after every bounce this job adds the bounce's part per half-axis to it.
        /// Created with the bounce textures.
        /// POOLED, NOT OWNED (BUGS-6 stage 3): one job per (aniso volume size, format, step,
        /// level), shared by every lighting of that size - every cascade, every rebuild - its
        /// textures bound per dispatch; see pooledMipStep in the .cpp. Released by
        /// releaseMipStepPool before the compute Hlms dies.
        HlmsComputeJob             *mAnisoGeneratorStep0;
        FastArray<HlmsComputeJob *> mAnisoGeneratorStep1;

        HlmsComputeJob *mLightVctBounceInject;
        /// The bounce pass's TARGET (PHOTON-7 stage 2): a pass writes the new total here and the region it covered is
        /// copied back into mLightVoxel[0] - the textures' identities never change (the old ping-pong swapped them and
        /// every reader had to be rebound).
        TextureGpu     *mLightBounce;

        /// Jahshaka (PHOTON-5 stage 5): THE BOUNCE ENERGY - the light volume's sum after the base
        /// (the injection and the sky pass) and after each bounce pass, one range of per-workgroup
        /// partials per sum in mEnergyBuffer, read back through mEnergyTicket (see
        /// setMeasureBounceEnergy). The job is shared by name ("Photon/VoxelEnergy"), its bindings
        /// asserted per dispatch.
        bool              mMeasureEnergy = false;
        HlmsComputeJob   *mEnergyJob = nullptr;
        UavBufferPacked  *mEnergyBuffer = nullptr;
        AsyncTicketPtr    mEnergyTicket;
        uint32            mEnergyGroups = 0u;     ///< partials per sum (the workgroups of one dispatch)
        uint32            mEnergyStride = 0u;     ///< floats between two sums' ranges (aligned)
        uint32            mEnergySums = 0u;       ///< sums the ticket in flight holds
        void              dispatchEnergy( uint32 slot );

        /// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): THE DIRECT TERM, KEPT.
        ///
        /// The bounce is a fixed-point iteration over the radiance in the volume,
        /// L = D + rho * G( L ), and it needs D -- the light INJECTED from the
        /// scene's lamps, before any bounce -- at every pass. Upstream's bounce job
        /// re-read the running TOTAL and added to it (L_n = L_n-1 + rho * G( L_n-1 )),
        /// which is a binomial series in (1 + rho*G) and never contracts: the first
        /// bounce's energy is counted again at every later pass. Every pass rebuilds
        /// the total from D (written into mLightBounce, copied back into
        /// mLightVoxel[0]), so D cannot be recovered from the total after the
        /// first pass.
        ///
        /// This is that third volume: mip 0 only (the bounce reads it with a
        /// Load3D at its own voxel, never filtered), same resolution and format as
        /// mLightVoxel[0], written by the light-injection job's second UAV in the
        /// same dispatch that writes the total -- so keeping it costs one image
        /// store per voxel and no copy, no readback and no CPU path. It lives
        /// exactly as long as mLightBounce does (see setAllowMultipleBounces): a
        /// volume that cannot bounce has no use for it.
        TextureGpu *mLightDirect;
        /// setStoreSkyLight (BOUNCES-ZERO-1).
        bool mStoreSkyLight = true;
        /// Jahshaka (PHOTON-VOXEL-5), with a bounce: the BACK side's direct
        /// term and the directional level 0's direct part per axis (float like mLightDirect and
        /// the total since CONTACT-OCCLUSION-1 - an emitter exceeds the lamps' ceiling; level 0 only; both signs packed along x like mLightVoxel[1..3]) - what
        /// the bounce's fixed point and step 0 need per side and per half.
        TextureGpu *mLightDirectBack;
        TextureGpu *mLightDirectDir[3];
        ShaderParams::Param *mInjectHigherMipHalfWidth;
        /// Jahshaka (PHOTON-7 stage 1): the injection's chain - this cascade's entry and, per entry, its box
        /// corner relative to this one's with its cell and its resolution with its shell's end (setChainIndex).
        ShaderParams::Param *mInjectChainSelf = nullptr;
        ShaderParams::Param *mInjectChainOrigin = nullptr;
        ShaderParams::Param *mInjectChainRes = nullptr;
        /// THE COMMIT'S LIGHT (setCommitLight) while one is set, its counter and its read-back.
        struct CommitLightState
        {
            bool on = false;
            PhotonCommitLight light;
        } mCommit;
        UavBufferPacked *mCommitCountBuf = nullptr;    ///< four words, cumulative (created zeroed)
        AsyncTicketPtr   mCommitTicket;                ///< the read-back in flight
        bool             mCommitDownloadOwed = false;  ///< a delta injection ran while a read-back was in flight
        uint32           mCommitSeen[3] = { 0u, 0u, 0u };   ///< the counter's words at the last landed reading
        /// THE BRICK MASK (PHOTON-7 stage 3; GiBrickMask): two words per 8^3 brick of this cascade's cells - [0] the most
        /// a voxel's direct term changed in the update's injection, [1] the most its last pass (the sky's or a bounce's)
        /// moved the total - each the float bits of a relative change (LightInjection_cs.glsl's jahBrickChange), cleared
        /// at every update() and read back without a wait through a ring of tickets (an update whose reading finds every
        /// slot in flight is counted, not read).
        UavBufferPacked *mBrickBuffer = nullptr;
        uint32 mBrickDims[3] = { 0u, 0u, 0u };
        std::vector<uint32> mBrickZeros;
        static constexpr uint32 kBrickReadingSlots = 4u;
        struct BrickReading
        {
            AsyncTicketPtr ticket;
            Vector3 origin = Vector3::ZERO;
        };
        BrickReading mBrickReadings[kBrickReadingSlots];
        uint32 mBrickNextIssue = 0u, mBrickNextTake = 0u;
        uint64 mBrickUpdates = 0u, mBrickReadingsSkipped = 0u;
        ShaderParams::Param *mInjectBrickDims = nullptr;
        ShaderParams::Param *mBounceBrickMark = nullptr;
        void ensureBrickMask();
        void requestBrickReading();
        ShaderParams::Param *mInjectKeepLo = nullptr;
        ShaderParams::Param *mInjectKeepHi = nullptr;
        ShaderParams::Param *mInjectOrigin = nullptr;
        /// This cascade's place in the scene's chain (0 = the innermost; the cascades outside it are
        /// mExtraCascades): the injection binds the whole chain and marches from this entry outward.
        uint32 mChainIndex = 0u;
        /// THE CHAIN'S CELLS, innermost first (setChainIndex): what the shells' ends sum - the WORLD's chain's, the
        /// cascades inside this one included (this lighting holds only those outside it).
        std::vector<float> mChainCells;
        /// The shell ends the last update() marched (getLastShellEnds).
        std::vector<float> mLastShellEnds;

        /// The scene's chain is this one cascade (the constructor's; the SDF skip's condition).
        bool  mChainOfOne;

        /// THE ONE ENVIRONMENT, FOR THE BOUNCE (Jahshaka, PHOTON-ENV-1): what a bounce
        /// cone that escapes the voxels sees - the host's disc-free sky cube (or null:
        /// no sky) read at the cone's aperture, times the environment light's gain, or
        /// with no cube the environment's nine-band SH in the cone's direction. It
        /// REPLACES the two-colour hemisphere pair this class used to hand the pixel
        /// shader (upstream's setAmbient): every escape in the renderer now reads one
        /// environment (jah_environment.glsl), and the pixel shader binds its own copy
        /// of it per pass. Set by the host before update(); read only by the bounce job.
        TextureGpu *mEnvCube;
        float       mEnvGain[3];
        float       mEnvSh[27];  ///< world basis {1,y,z,x,xy,yz,3z^2-1,zx,x^2-y^2}, radiance units

        /// When we do multiple bounces, cascades can be used to improve accuracy
        FastArray<PhotonVoxelLighting *> mExtraCascades;

        ShaderParams::Param *mNumLights;
        /// Jahshaka (PHOTON-7 stage 1): the injection's per-voxel light count (the first of the order that reach it).
        ShaderParams::Param *mLightsPerVoxel = nullptr;
        /// THE LAST INJECTION'S LIGHTS (P1C-LIGHT-LIST): the selection from the scene's
        /// one light list, and its counts — the lights whose range reaches this
        /// cascade's box, how many took a slot, how many the budget could not hold.
        jahshaka::engine::SceneLightList::Selection mSelection;
        uint32 mLastLightsInRange = 0u;
        uint32 mLastLightsInjected = 0u;
        uint32 mLastLightsOverBudget = 0u;
        /// THE BOX OVER ITS LIGHT SLOTS (PHOTON-7 stage 1's read F4), over this lighting's life: the most lights in
        /// range of its box at any injection, and the injections whose box held more than kLightSlots - where a coarse
        /// voxel (its budget the slots: lightBudgetFor) takes the box's first 128 rather than its own first K (L1 broken
        /// there).
        uint32 mLightsInRangeMax = 0u;
        uint64 mInjectionsOverSlots = 0u;
        uint32 mLightBudget = 16u;   ///< this cascade's budget (lightBudgetFor), set by update()
        bool   mOverBudgetLogged = false;
        ShaderParams::Param *mVoxelCellSize;
        ShaderParams::Param *mInvVoxelResolution;
        ShaderParams        *mShaderParams;

        typedef vector<ShaderParams::Param>::type ParamVec;
        ParamVec                                  mLocalBounceShaderParams;
        ShaderParams::Param                      *mBounceVoxelCellSize;
        ShaderParams::Param                      *mBounceInvVoxelResolution;
        ShaderParams::Param                      *mBounceIterationDampening;
        ShaderParams::Param                      *mBounceInvResMaxLod;
        ShaderParams::Param *mBounceFromPreviousProbeToNext;  ///< Used when cascades > 1
        ShaderParams::Param *mBounceEnvGainMips;  ///< Jahshaka (PHOTON-ENV-1): rgb gain (radiance), w mips
        ShaderParams::Param *mBounceEnvSh;        ///< Jahshaka (PHOTON-ENV-1): nine float4 (radiance)
        ShaderParams::Param *mBounceRegionOrigin = nullptr;   ///< PHOTON-7 stage 2: a region pass's first cell
        ShaderParams        *mBounceShaderParams;

        ResourceTransitionArray mResourceTransitions;

    public:
        /** When roughness is close to 0.02, specular cone tracing becomes path tracing.
            This is very slow. However we can greatly speed it up by skipping gaps of empty
            voxels.

            We use the alpha (opacity) component of the higher mips to approximate
            the SDF (Signed Distance Field) and thus know how much to skip. This is
            theoretically wrong, but not very wrong because the mips are very close to
            its true SDF representation thus in it works practice.

            Some of these formulas have been empirically tuned to match a good
            performance/quality ratio

            Once the roughness is higher, this formula starts hurting quality (produces
            noticeable artifacts) and thus we disable it.

            This formula has tweakable parameters to leverage performance vs quality

            Recommended range is [0; 1] where 1 is high quality and 0 is high performance
            (artifacts may appear).

            However you can go outside that range.
        @remarks
            When resolution is <= 32; we completely disable this hack as it only hurts
            performance (fetching the opacity is more expensive than skipping pixels)

            PUBLIC VARIABLE. This variable can be altered directly.
            Changes are reflected immediately.
        */
        float mSpecularSdfQuality;

    protected:
        PhotonVoxelVisualizer *mDebugVoxelVisualizer;

        ShaderParams::Param *addLocalBounceShaderParam( const char *name );


        void addLight( PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight, Light *light, const Vector3 &voxelOrigin );

        void createTextures();
        void destroyTextures();
        void checkTextures();
        /// Re-derives the bounce injection job's texture UNIT COUNT and writes every
        /// slot it reads from the CURRENT textures. `bSetSamplerRefs` false skips the
        /// OpenGL-only samplerblock reference-counting, which makes the call safe to
        /// repeat per dispatch (the textures move under it; the samplers do not).
        void setupBounceTextures( bool bSetSamplerRefs = true );
        void setupGlslTextureUnits();

        /// The directional levels from level 0 (step 0 with a bounce, then step 1 per level) - over the cells
        /// [regionLo, regionHi) of level 0 and what covers them at every coarser level (PHOTON-7 stage 2), or whole.
        void generateAnisotropicMips( const uint32 *regionLo = nullptr, const uint32 *regionHi = nullptr );

        /// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): the pass index is gone with the per-iteration
        /// dampening it fed (upstream's commented-out 1 / ( pi * ( n/2 + 1 ) )).
        /// Every pass of a Jacobi iteration is the same operator; nothing about it
        /// depends on which pass it is.
        ///
        /// Jahshaka (CONTACT-OCCLUSION-1, SKY-BOUNCE-1): `envOnly` runs the same pass with
        /// the voxels' share of the gather left out - new = direct + rho * E_sky, the
        /// ENVIRONMENT'S DIRECT TERM at every voxel (the sky through the escape of the
        /// voxel's own cones: its visibility). update() runs it first, whenever an
        /// environment carries light, so the sky is a light like any lamp: a surface it
        /// lights re-emits albedo x its sky irradiance at EVERY bounce count, where it
        /// used to exist only from the second bounce on (and not at all at one).
        /// PHOTON-7 stage 2: over the cells [regionLo, regionHi) only (aligned to 8 cells), or whole - written into
        /// mLightBounce and that region copied back into the total.
        void runBounce( bool envOnly = false, const uint32 *regionLo = nullptr, const uint32 *regionHi = nullptr,
                        bool markBricks = false );
        /// The injection job's UAV slots, its slot count and its two properties, as update() dispatches it - the
        /// delta's (a scroll's commit: four slots read-write, the counter last) or a whole injection's.
        void bindInjectionUavs( bool delta );
        /// True when the environment setEnvironment was handed carries any light.
        bool hasEnvironmentLight() const;

    public:
        /// `chainOfOne`: the scene's cascade chain is this one cascade (the specular SDF skip's
        /// condition: shouldEnableSpecularSdfQuality).
        PhotonVoxelLighting( IdType id, PhotonVoxelizerSourceBase *voxelizer, bool chainOfOne );
        ~PhotonVoxelLighting() override;

        /// Used by a host's cascade chain (upstream's VctCascadedVoxelizer is not in
        /// this fork). By having extra cascade info, we can calculate multiple bounces
        /// with extra info
        ///
        /// This function calls mExtraCascades.reserve
        void reserveExtraCascades( size_t numExtraCascades );

        /// Used by a host's cascade chain (upstream's VctCascadedVoxelizer is not in
        /// this fork). By having extra cascade info, we can calculate multiple bounces
        /// with extra info
        void addCascade( PhotonVoxelLighting *cascade );

        /** This function allows PhotonVoxelLighting::update to pass numBounces > 0 as argument.
            Note however, that multiple bounces requires creating another RGBA32_UNORM texture
            of the same resolution as the voxel texture.

            This can cause increase memory consumption.
        @remarks
            It is valid to call setAllowMultipleBounces( false ) right after calling
            update( sceneManager, numBounces > 0 ) in order to release the extra memory.

            However remember to call setAllowMultipleBounces( true ) before calling update()
            again with numBounces > 0.
        @param bAllowMultipleBounces
            True to allow multiple bounces, and consume more memory.
            False to no longer allow multiple bounces, and release memory.
        */
        void setAllowMultipleBounces( bool bAllowMultipleBounces );
        bool getAllowMultipleBounces() const;

        /// Jahshaka (BOUNCES-ZERO-1): whether update() stores the SKY's light on the surfaces
        /// (the sky pass). True by default: the sky is a light like any lamp. False is the
        /// document's numBounces 0 - THE DIRECT STORE ALONE: the lamps' direct light and
        /// nothing a surface re-emits of the sky or of another surface.
        void setStoreSkyLight( bool on ) { mStoreSkyLight = on; }
        bool getStoreSkyLight() const { return mStoreSkyLight; }

        /// THE LIGHT SLOTS (P1C-LIGHT-LIST): the const buffer and LightInjection_piece_cs's
        /// array (vct_light_budget) hold kLightSlots lights in every cascade.
        /// Destroys the pooled anisotropic mip-step jobs (BUGS-6 stage 3) - the engine's teardown.
        static void releaseMipStepPool();
        /// Mip-step jobs ever cloned, process-wide (a pooled one is cloned once per kind).
        static unsigned long long numMipStepJobs();

        static constexpr uint32 kLightSlots = 128u;
        /// THE PER-CASCADE LIGHT BUDGET, by measurement (spikes/p1c-light-list): a lamp costs
        /// the injection its march over the voxels inside its range, ~0.06 ms per million
        /// voxels at High (RTX 4080 SUPER, Debug). The budget bounds a cascade's worst case — every
        /// lamp reaching every voxel — at what 16 lamps cost a 128^3 cascade (~2 ms): 16 x 128^3
        /// voxel-lamps, so 16 at 128^3 and 128 (the slots) at 64^3; never under the old 16.
        static constexpr uint64 kLightBudgetVoxelLamps = 16ull * 128ull * 128ull * 128ull;
        static uint32 lightBudgetFor( uint32 voxels )
        {
            const uint64 b = voxels ? kLightBudgetVoxelLamps / voxels : kLightSlots;
            return static_cast<uint32>( std::min<uint64>( kLightSlots, std::max<uint64>( 16u, b ) ) );
        }
        /// The last update()'s light reading: lights whose range reaches this cascade,
        /// lights injected, lights over the budget (inRange - injected), the budget.
        uint32 getLastLightsInRange() const { return mLastLightsInRange; }
        uint32 getLastLightsInjected() const { return mLastLightsInjected; }
        uint32 getLastLightsOverBudget() const { return mLastLightsOverBudget; }
        uint32 getLightsInRangeMax() const { return mLightsInRangeMax; }
        uint64 getInjectionsOverSlots() const { return mInjectionsOverSlots; }
        uint32 getLightCapacity() const { return mLightBudget; }

        /**
        @param sceneManager
        @param numBounces
            Number of GI bounces. This value must be 0 if getAllowMultipleBounces() == false
        @param lightMask
        @remarks
            Jahshaka (PHOTON-VOXEL-5): the injection's shadow march is exact - a 3D-DDA over
            the level-0 voxels from each half's face - so upstream's thinWallCounter and
            rayMarchStepScale (a stepped march's wall counter and step length) are gone.
        */
        void update( SceneManager *sceneManager, uint32 numBounces, uint32 lightMask = 0xffffffff );

        /// THE COMMIT'S LIGHT for the next update() (PhotonCommitLight); null a whole injection.
        void setCommitLight( const PhotonCommitLight *commit );
        /** THE DIRECTIONAL LEVELS, WHOLE, FROM THE LEVEL-0 LIGHT AS IT STANDS (PHOTON-7 stage 2's read D1; the test door
            gi.regenerateDirectional): step 0 (with a bounce) and step 1 over the whole volume - nothing else written. */
        void regenerateDirectional() { generateAnisotropicMips(); }
        /** THE BRICK MASK'S OLDEST LANDED READING (PHOTON-7 stage 3), taken: `words` two per brick (x fastest over `dims`),
            `origin` the box corner the update ran at. False while none has landed. */
        bool takeBrickReading( std::vector<uint32> &words, Vector3 &origin, uint32 dims[3] );
        uint32 getNumBricks() const { return mBrickDims[0] * mBrickDims[1] * mBrickDims[2]; }
        /// Updates that wrote a mask, and those whose reading was skipped (every slot in flight).
        uint64 getBrickUpdates() const { return mBrickUpdates; }
        uint64 getBrickReadingsSkipped() const { return mBrickReadingsSkipped; }
        /// Is the delta injection's shader built (never compiled in the frame: a permutation not built is handed to the
        /// background compiler and the answer is false until it is)? Asked when a scroll is planned and at its commit.
        bool commitLightReady();
        /// THE DELTA INJECTIONS' COUNTS since the last reading that landed (never a wait; the counter on the device is
        /// cumulative, so no injection's count is lost to a read-back in flight): voxels lit outside the kept box (the
        /// slab), inside it (the changed direct set) and the injections. False while no new reading landed.
        bool takeCommitCount( uint32 &litInSlab, uint32 &litOutside, uint32 &injections );
        /// THE BOUNCE REACH in cells (PHOTON-7 stage 2): how far from a changed voxel a bounce pass of THIS cascade
        /// gathers it - a cone reads until it hands over at the chain's max level, a footprint of 2^(maxLod + 1)
        /// cells at most, plus the cell itself; the outermost cascade's cones cross its whole box (0 = whole).
        uint32 regionReachCells() const;

        /// THE INJECTION'S PARTS, NAMED AS THEY ARE RECORDED (PHOTON-7 stage 1, the split rows): a
        /// host that times an injection hands a listener, and update() opens each part before it
        /// records it and closes it after - the direct term's dispatch, the directional levels and
        /// the mip chains after it, the sky pass, the bounce passes (each with its own levels). A
        /// host files each as a row of its own (PhotonComponent::injectCascade: vct.inject.*N),
        /// which is how a cascade's light tick is priced part by part. Null: nothing is named.
        enum class Phase
        {
            Direct,
            Mips,
            Sky,
            Bounce
        };
        class PhaseListener
        {
        public:
            virtual ~PhaseListener() {}
            virtual void phaseBegin( Phase phase ) = 0;
            virtual void phaseEnd() = 0;
        };
        void setPhaseListener( PhaseListener *listener ) { mPhaseListener = listener; }

    protected:
        /// setPhaseListener (PHOTON-7 stage 1): who names the injection's parts, or null.
        PhaseListener *mPhaseListener = nullptr;

    public:

        /** Points this PhotonVoxelLighting at a DIFFERENT voxelizer, in place.

            A PhotonVoxelLighting is normally bound to the voxelizer it was constructed with for
            its whole life. That is a problem for any caller that must re-voxelize with a
            FRESH PhotonVoxelizer rather than rebuild an existing one -- PhotonVoxelMaterial caches
            each datablock's conversion by raw pointer for the voxelizer's lifetime, so a
            material whose parameters changed (or whose datablock died and whose address
            was recycled) can only be answered by a new voxelizer. Destroying the
            PhotonVoxelLighting as well is not an option when it is part of a cascade chain:
            addCascade() hands the cascades inside this one raw pointers to it and
            fillConstBufferData() walks them on every pass, so replacing one cascade's
            lighting forces every cascade inside it to be rebuilt in the same frame.

            Everything this object holds that depends on the voxelizer is re-derived here:
            the TextureGpuListener registrations move to the new albedo/normal textures
            and the light voxel textures are re-created from the new resolution -- the
            same recovery the LostResidency path performs.
        @remarks
            The new voxelizer must already have been built (its albedo texture is what the
            light voxels are sized from), and the caller keeps ownership of both: the old
            one is only safe to destroy AFTER this returns.
        @param voxelizer
            The voxelizer to sample from now. Null or the current one is a no-op.
        */
        void setVoxelizer( PhotonVoxelizerSourceBase *voxelizer );

        /// Upstream's VctImageVoxelizer::buildRelative called this (its voxeliser's
        /// textures - albedo, normal, emissive - could be swapped for a copy). That class
        /// is not in this fork (6be1ae1d7) and nothing here calls this any more.
        ///
        /// This function notifies us that buildRelative to update some of our references
        void resetTexturesFromBuildRelative();

        size_t getNumCascades() const { return mExtraCascades.size() + 1u; }

        /// THE INJECTION'S CHAIN (PHOTON-7 stage 1, L1): this cascade's place in the scene's chain, innermost 0
        /// (the chain's builder sets it; a lone cascade is 0). The injection binds the WHOLE chain's geometry - its
        /// cascades inside this one too, never read - so one permutation serves every cascade of a chain length.
        /// THE SHELLS' ENDS (PHOTON-7 stage 1's read F1): D_j = the sum of 8 cells of every cascade up to j, over the
        /// WORLD's chain - the same numbers at every cascade - so the builder hands in every cascade's cell, innermost
        /// first (`chainCells`, the chain's length); a cascade's own lighting cannot see the cascades inside it.
        void setChainIndex( uint32 index, const std::vector<float> &chainCells )
        {
            mChainIndex = index;
            mChainCells = chainCells;
        }
        /// The shell ends D_0 .. D_{n-2} (metres from the march's start) the last update() handed its march: a door
        /// (GiStatus::CascadeStatus::shellEnds) - every cascade of a chain reads the same list.
        const std::vector<float> &getLastShellEnds() const { return mLastShellEnds; }
        uint32 getChainIndex() const { return mChainIndex; }
        /// The chain's length: the cascades inside this one, this one and those outside it.
        uint32 getChainLength() const { return mChainIndex + 1u + static_cast<uint32>( mExtraCascades.size() ); }
        /// The injection job's texture units before the cloud field's (OgreScene::bindCloudInjection binds that
        /// one at this unit): the albedo, normal, emissive, metal and emissive factor per half, then the chain's
        /// coverage and position per half, four arrays of getChainLength().
        uint8 injectionTexUnits() const { return static_cast<uint8>( 6u + 4u * getChainLength() ); }

        /** THE CASCADE CHAIN'S PARAMETERS, the one definition every reader of the chain
            takes them from (Jahshaka, PHOTON-READER-1): the pixel shader's pass buffer
            (fillConstBufferData) and the irradiance field's generation job both march the
            same chain with the same march, so they must hand it the same numbers - and
            the field has no other way to reach the extra cascades' placement.
        @param outInvResMaxLod
            4 * getNumCascades() floats: per cascade, xyz = 1 / the light volume's
            resolution per axis, w = the mip at which a cone hands over to the next
            cascade (the next cell over this one as a mip, capped at the mip count;
            256 for the last cascade).
        @param outFromPrev
            8 * ( getNumCascades() - 1 ) floats: per cascade i >= 1, two float4s -
            ( cascade i-1's normalised space to cascade i's: the scale xyz ), then ( the
            offset xyz ); both w are padding (0). (Every cascade stores radiance, k = 1:
            PHOTON-7 stage 1 deleted the hop's unit conversion that sat in the first w.)
            May be null when there is one cascade.
        */
        void getCascadeChainParams( float *RESTRICT_ALIAS outInvResMaxLod,
                                    float *RESTRICT_ALIAS outFromPrev ) const;

        size_t getConstBufferSize() const;

        void fillConstBufferData( const Matrix4 &viewMatrix, float *RESTRICT_ALIAS passBufferPtr ) const;

        bool shouldEnableSpecularSdfQuality() const;

        void setDebugVisualization( bool bShow, SceneManager *sceneManager );
        bool getDebugVisualizationMode() const;
        /// Jahshaka (PHOTON-VIEW-1): the visualizer setDebugVisualization created (null
        /// while it is off) - a host puts it on its own visibility channel or render queue
        /// without searching the scene graph for it.
        PhotonVoxelVisualizer *getDebugVisualizer() const { return mDebugVoxelVisualizer; }

        /** THE ENVIRONMENT A BOUNCE CONE ESCAPES TO (Jahshaka, PHOTON-ENV-1). Replaces
            upstream's setAmbient hemisphere pair, which the pixel shader used to add
            wherever a cone escaped: the pixel shader now reads the environment itself
            (the host binds the cube per pass), and this class keeps only what its own
            BOUNCE job needs - the sky enters the voxels once per injection, as what the
            escaping cones of the sky pass and of every bounce pass see (update(): the
            sky pass writes the sky's direct term, the bounce passes build on the lamps'
            direct volume, so the sky is counted once and bounced like a lamp).
        @param cube
            The disc-free, GGX-prefiltered environment cube, or null (no sky: the SH
            below is the whole environment). Not owned; the host keeps it alive and
            calls this again before it dies.
        @param gain
            The environment light's gain per channel (applied to the cube only).
        @param sh
            27 floats: the environment's nine-band SH in WORLD axes and radiance units,
            the gain already applied (the host's own ambient coefficients).
        */
        void setEnvironment( TextureGpu *cube, const ColourValue &gain, const float sh[27] );
        /// The environment setEnvironment was handed (Jahshaka, PHOTON-ENV-1): the
        /// irradiance field's generation job reads the same one for its escaping rays.
        TextureGpu  *getEnvironmentCube() const { return mEnvCube; }
        const float *getEnvironmentGain() const { return mEnvGain; }
        const float *getEnvironmentSh() const { return mEnvSh; }

        TextureGpu **getLightVoxelTextures() { return mLightVoxel; }
        /** THE SCROLL'S LIGHT (PHOTON-7 stage 2, AB S4): the level-0 textures a scroll's commit shifts in place with the
            geometry - the total, its back side, the direct term, its back side, and the directional level 0's direct
            part per axis (half resolution, both signs packed along x: `halfRes` true). The directional volumes are not
            among them: step 0 rebuilds their level 0 from exactly these; the bounce's target is scratch. Fills `out`
            and returns 7, or 0 on a lighting that cannot bounce (no direct term to shift).
        */
        size_t scrollLightTextures( TextureGpu *out[7], bool halfRes[7] ) const;
        /// JAHSHAKA PATCH: THE DIRECT TERM'S VOLUME (fork ae2ed529f+155a56bf8 (was 0076)'s D term), or null
        /// on a PhotonVoxelLighting that cannot bounce. Read-only, and it exists so a host
        /// can MEASURE the store -- the normalisation's own self-check is "the
        /// direct term is <= the ceiling by construction", and nothing outside this
        /// class could see the volume to check it.
        TextureGpu  *getLightDirectTexture() const { return mLightDirect; }
        /// ...and the back side's direct term (PHOTON-7 stage 2: the store door's `directBack`).
        TextureGpu  *getLightDirectBackTexture() const { return mLightDirectBack; }
        TextureGpu **getLightVoxelTextures( const size_t cascadeIdx );
        /// The light volumes a reader binds per cascade: the total (level 0) and its three
        /// directional axes, then the coverage per half-axis (+a, -a; PHOTON-VOXEL-3/-4),
        /// then the surface position per half (PHOTON-VOXEL-4), then (PHOTON-VOXEL-5) level
        /// 0's back side and the voxelizer's normal, the last two (backIndex / normalIndex).
        uint32       getNumVoxelTextures() const { return kVolumesPerCascade; }
        /// Jahshaka (OWN-GI-1): the same count without an instance — what the PBS
        /// pass claims per cascade (PhotonPassBinding) and the pass budget is sized on.
        static constexpr uint32 kVolumesPerCascade = 10u;
        /// Jahshaka (PHOTON-VOXEL-5): level 0's BACK side (the front = 2 x mLightVoxel[0] - it)
        /// and the voxelizer's normal (the side a half-axis looks to).
        uint32       backIndex() const { return 8u; }
        uint32       normalIndex() const { return 9u; }
        /// Where the coverage of half `h` (0: the faces looking +a, 1: -a) sits in
        /// getLightVoxelTextures().
        uint32       coverageIndex( uint32 h ) const { return 4u + h; }
        /// Where the surface position of half `h` sits: the last two entries.
        uint32       positionIndex( uint32 h ) const { return 6u + h; }

        const HlmsSamplerblock *getBindTrilinearSamplerblock() { return mSamplerblockTrilinear; }

        /// Jahshaka (PHOTON-5 stage 5): THE BOUNCE ENERGY. With measuring on, the next update() that
        /// runs bounce passes sums the light volume after its base (the injection and the sky pass)
        /// and after every pass and requests the sums' read-back on the frame's own submission -
        /// nothing waits. The series is Jacobi (new = direct + rho G( total )), so sum[k] - sum[k - 1]
        /// is the energy pass k ADDED, and the ratio of two increments is the series' contraction.
        /// No new reading is requested while one is in flight.
        void setMeasureBounceEnergy( bool on ) { mMeasureEnergy = on; }
        bool bounceEnergyInFlight() const { return mEnergyTicket.get() != 0; }
        /** Takes the reading in flight: sums[0] the base, sums[k] after pass k. Call it once the
            frame that recorded it is at least the VaoManager's dynamic-buffer multiplier old: the
            device has finished that frame by then, so the map never waits. False when none. */
        bool takeBounceEnergy( std::vector<double> &sums );

        const PhotonVoxelizerSourceBase *getVoxelizer() const { return mVoxelizer; }

        // TextureGpuListener overloads
        void notifyTextureChanged( TextureGpu *texture, TextureGpuListener::Reason reason,
                                   void *extraData ) override;
        bool shouldStayLoaded( TextureGpu *texture ) override { return false; }
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
