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

#include "OgreHeaderPrefix.h"

namespace Ogre
{
    class PhotonVoxelizerSourceBase;
    class PhotonVoxelVisualizer;
    struct PhotonShaderVoxelLight;

    class PhotonVoxelLighting : public IdObject, public TextureGpuListener
    {
    public:

    protected:
        /// When mAnisotropic == false, mLightVoxel[0] contains all the mips.
        ///
        /// When mAnisotropic == true, mLightVoxel[0] contains mip 0.
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
        /// Jahshaka (PHOTON-VOXEL-3/-4): THE LAST FOUR ENTRIES - from mLightVoxel[4]
        /// anisotropic, mLightVoxel[1] isotropic - are the voxelizer's COVERAGE PER HALF-AXIS
        /// (coverageIndex(0 / 1): the faces looking +a / -a) and its SURFACE POSITION per
        /// half (positionIndex(0 / 1)), NOT owned here: every reader of the light volumes
        /// reads the opacity along its ray from the coverage and the origin plane from the
        /// position, so they ride the same list and every reader that binds
        /// getNumVoxelTextures() volumes per cascade binds them too.
        ///
        /// Jahshaka (PHOTON-VOXEL-5): on the anisotropic tiers two more follow - mLightVoxel[8],
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
        /// Created with the bounce textures, on the anisotropic tiers.
        HlmsComputeJob             *mAnisoGeneratorStep0;
        FastArray<HlmsComputeJob *> mAnisoGeneratorStep1;

        HlmsComputeJob *mLightVctBounceInject;
        TextureGpu     *mLightBounce;

        /// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): THE DIRECT TERM, KEPT.
        ///
        /// The bounce is a fixed-point iteration over the radiance in the volume,
        /// L = D + rho * G( L ), and it needs D -- the light INJECTED from the
        /// scene's lamps, before any bounce -- at every pass. Upstream's bounce job
        /// re-read the running TOTAL and added to it (L_n = L_n-1 + rho * G( L_n-1 )),
        /// which is a binomial series in (1 + rho*G) and never contracts: the first
        /// bounce's energy is counted again at every later pass. The two textures
        /// mLightVoxel[0]/mLightBounce ping-pong, so by the second pass the direct
        /// term is the one being overwritten and cannot be recovered from them.
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
        /// Jahshaka (PHOTON-VOXEL-5), the anisotropic tiers with a bounce: the BACK side's direct
        /// term and the directional level 0's direct part per axis (float like mLightDirect and
        /// the total since CONTACT-OCCLUSION-1 - an emitter exceeds the lamps' ceiling; level 0 only; both signs packed along x like mLightVoxel[1..3]) - what
        /// the bounce's fixed point and step 0 need per side and per half.
        TextureGpu *mLightDirectBack;
        TextureGpu *mLightDirectDir[3];
        ShaderParams::Param *mInjectHigherMipHalfWidth;

        float mBakingMultiplier;
        float mInvBakingMultiplier;

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

        bool  mAnisotropic;

        /// When we do multiple bounces, cascades can be used to improve accuracy
        FastArray<PhotonVoxelLighting *> mExtraCascades;

        ShaderParams::Param *mNumLights;
        /// THE LIGHT SLOTS' LAST READING (V2-P0A, the scale suite's W2 row): of the
        /// lights the last update() saw, how many passed the range cull (their range
        /// sphere touches this cascade's box) and how many of those took one of the
        /// light buffer's slots — the rest were dropped, first come first served.
        uint32 mLastLightsInRange = 0u;
        uint32 mLastLightsInjected = 0u;
        ShaderParams::Param *mBakingMultiplierParam;
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
        ShaderParams::Param *mBounceEnvGainMips;  ///< Jahshaka (PHOTON-ENV-1): rgb gain / multiplier, w mips
        ShaderParams::Param *mBounceEnvSh;        ///< Jahshaka (PHOTON-ENV-1): nine float4, / multiplier
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

        /** Sets the intensity/brightness of the GI. e.g. to make the GI 2x brighter, set it to 2.0
            To make the GI darker, set it to 0.5

            Default value is 1.0

            Valid range is (0; inf)

            @remark	 PUBLIC VARIABLE. This variable can be altered directly.
                     Changes are reflected immediately.
        */
        float mMultiplier;

    protected:
        PhotonVoxelVisualizer *mDebugVoxelVisualizer;

        ShaderParams::Param *addLocalBounceShaderParam( const char *name );

        void restoreSwappedTextures();

        float addLight( PhotonShaderVoxelLight *RESTRICT_ALIAS vctLight, Light *light,
                        const Vector3 &voxelOrigin, const Vector3 &invVoxelSize );

        void createTextures();
        void destroyTextures();
        void checkTextures();
        /// Re-derives the bounce injection job's texture UNIT COUNT and writes every
        /// slot it reads from the CURRENT textures. `bSetSamplerRefs` false skips the
        /// OpenGL-only samplerblock reference-counting, which makes the call safe to
        /// repeat per dispatch (the textures move under it; the samplers do not).
        void setupBounceTextures( bool bSetSamplerRefs = true );
        void setupGlslTextureUnits();

        void generateAnisotropicMips();

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
        void runBounce( bool envOnly = false );
        /// True when the environment setEnvironment was handed carries any light.
        bool hasEnvironmentLight() const;

    public:
        PhotonVoxelLighting( IdType id, PhotonVoxelizerSourceBase *voxelizer, bool bAnisotropic );
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

        /** Sets baking multiplier for HDR rendering.

            Internally the lighting data is stored in RGBA8_UNORM_sRGB, which is not enough
            for HDR. More precise formats would allow for native HDR, however it's memory cost
            and bandwidth could be prohibtive.

            Hence bakingMult is used to bring down the lighting data to usable levels without
            saturation (however beware areas with very low lighting conditions may round to 0).

            During rendering, we use 1.0 / bakingMult to bring back the values its original range.

            For LDR rendering, you probably would want to set this value to 1.0.
        @remarks
            This value will be ignored if PhotonVoxelLighting::update is not called after setting this value
            or if PhotonVoxelLighting::update gets called again but autoMultiplier = true

            This function is different from PhotonVoxelLighting::mMultiplier.
            That variable controls the GI strength/brightness.
            This function only controls precision and accuracy.
        @param bakingMult
            Value to multiply against GI lighting during baking.
            Use values >= 1 when all your lights are too dim,
            but could saturate quickly if that's not the case

            Values <= 1 make when your lights are very bright,
            but can cause low light to become 0 (too dark)

            Changes to this value take effect after calling PhotonVoxelLighting::update
            and autoMultiplier must be set to false
        */
        void  setBakingMultiplier( float bakingMult );
        float getBakingMultiplier() const { return mBakingMultiplier; }

        /// If you've set setBakingMultiplier but haven't yet called PhotonVoxelLighting::update
        /// with autoMultiplier = false, this function returns the baking multiplier that
        /// is currently in use (beware of floating point accuracy differences)
        float getCurrentBakingMultiplier() const { return 1.0f / mInvBakingMultiplier; }

        /// The last update()'s light reading (see mLastLightsInRange): lights whose
        /// range reaches this cascade, lights injected, and the slot count (the cap).
        uint32 getLastLightsInRange() const { return mLastLightsInRange; }
        uint32 getLastLightsInjected() const { return mLastLightsInjected; }
        uint32 getLightCapacity() const;

        /**
        @param sceneManager
        @param numBounces
            Number of GI bounces. This value must be 0 if getAllowMultipleBounces() == false
        @param autoMultiplier
            Whether we should calculate the ideal multiplier based on lights on scene.
            See PhotonVoxelLighting::setMultiplier
        @param lightMask
        @remarks
            Jahshaka (PHOTON-VOXEL-5): the injection's shadow march is exact - a 3D-DDA over
            the level-0 voxels from each half's face - so upstream's thinWallCounter and
            rayMarchStepScale (a stepped march's wall counter and step length) are gone.
        */
        void update( SceneManager *sceneManager, uint32 numBounces, bool autoMultiplier = true,
                     uint32 lightMask = 0xffffffff );

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
            ( cascade i-1's normalised space to cascade i's: the scale xyz, and cascade
            i's radiance over cascade 0's stored units ), then ( the offset xyz, and the
            specular walk's weight slope 1 / numMips^3 ).
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

        /** Toggles anisotropic mips.

            Anisotropic mips provide much higher quality and generally lower light leaking.
            However it costs a bit more performance, and increases memory consumption.

            Normally regular mipmaps of 3D textures increase memory consumption by 1/7
            Anisotropic mipmaps of 3D textures increase memory consumption by 6/7

             + Isotropic 256x256x256 RGBA8_UNORM = 256x256x256x4 * (1+1/7) = 64 MB * 1.143 =  73.14MB
             + Anisotrop 256x256x256 RGBA8_UNORM = 256x256x256x4 * (1+6/7) = 64 MB * 1.857 = 118.85MB

        @remarks
            After changing this setting, PhotonVoxelLighting::update
            *must* be called again to repopulate the light data.
        */
        void setAnisotropic( bool bAnisotropic );
        bool isAnisotropic() const { return mAnisotropic; }

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
        /// The decode multiplier the pixel shader applies to this volume's stored
        /// radiance (fillConstBufferData's `multiplier`): D_max / pi.
        float getFinalMultiplier() const { return mInvBakingMultiplier * mMultiplier; }

        TextureGpu **getLightVoxelTextures() { return mLightVoxel; }
        /// JAHSHAKA PATCH: THE DIRECT TERM'S VOLUME (fork ae2ed529f+155a56bf8 (was 0076)'s D term), or null
        /// on a PhotonVoxelLighting that cannot bounce. Read-only, and it exists so a host
        /// can MEASURE the store -- the normalisation's own self-check is "the
        /// direct term is <= the ceiling by construction", and nothing outside this
        /// class could see the volume to check it.
        TextureGpu  *getLightDirectTexture() const { return mLightDirect; }
        TextureGpu **getLightVoxelTextures( const size_t cascadeIdx );
        /// The light volumes a reader binds per cascade: the total (and its three
        /// anisotropic axes), then the coverage per half-axis (+a, -a; PHOTON-VOXEL-3/-4),
        /// then the surface position per half (PHOTON-VOXEL-4).
        /// ...and on the anisotropic tiers (PHOTON-VOXEL-5) level 0's back side and the
        /// voxelizer's normal, the last two (backIndex / normalIndex).
        uint32       getNumVoxelTextures() const { return volumesPerCascade( mAnisotropic ); }
        /// Jahshaka (OWN-GI-1): the same count without an instance — what the PBS
        /// pass claims per cascade (PhotonPassBinding) and the pass budget is sized on.
        static constexpr uint32 kVolumesPerCascadeIsotropic = 5u;
        static constexpr uint32 kVolumesPerCascadeAnisotropic = 10u;
        static constexpr uint32 volumesPerCascade( bool anisotropic )
        {
            return anisotropic ? kVolumesPerCascadeAnisotropic : kVolumesPerCascadeIsotropic;
        }
        /// Jahshaka (PHOTON-VOXEL-5): level 0's BACK side (the front = 2 x mLightVoxel[0] - it)
        /// and the voxelizer's normal (the side a half-axis looks to), anisotropic tiers only.
        uint32       backIndex() const { return 8u; }
        uint32       normalIndex() const { return 9u; }
        /// Where the coverage of half `h` (0: the faces looking +a, 1: -a) sits in
        /// getLightVoxelTextures().
        uint32       coverageIndex( uint32 h ) const { return ( mAnisotropic ? 4u : 1u ) + h; }
        /// Where the surface position of half `h` sits: the last two entries.
        uint32       positionIndex( uint32 h ) const { return ( mAnisotropic ? 6u : 3u ) + h; }

        const HlmsSamplerblock *getBindTrilinearSamplerblock() { return mSamplerblockTrilinear; }

        const PhotonVoxelizerSourceBase *getVoxelizer() const { return mVoxelizer; }

        // TextureGpuListener overloads
        void notifyTextureChanged( TextureGpu *texture, TextureGpuListener::Reason reason,
                                   void *extraData ) override;
        bool shouldStayLoaded( TextureGpu *texture ) override { return false; }
    };
}  // namespace Ogre

#include "OgreHeaderSuffix.h"

#endif
