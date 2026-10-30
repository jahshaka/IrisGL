@insertpiece( SetCrossPlatformSettings )
#extension GL_ARB_shader_group_vote: require

@insertpiece( DeclUavCrossPlatform )

@insertpiece( PreBindingsHeaderCS )

@pset( vctTexUnit, 2 )

/// Enable uses_array_bindings when there is more than one cascade
/// (hlms_num_vct_cascades can't be 0)
/// uses_array_bindings = hlms_num_vct_cascades - 1
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

vulkan_layout( ogre_t0 ) uniform texture3D voxelAlbedoTex;
vulkan_layout( ogre_t1 ) uniform texture3D voxelNormalTex;
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbes[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeX[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeY[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeZ[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

// Jahshaka (PHOTON-VOXEL-3): every cascade's PER-AXIS COVERAGE, after the probe
// arrays (VctLighting::setupBounceTextures' order): the march's mip-0 read takes the
// opacity along its cone from it.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-4): every cascade's SURFACE POSITION, after the coverage - the
// march's gate.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// PHOTON-7 stage 7: every cascade's BACK SIDE, after the surface position (VctLighting::setupBounceTextures' order): the
// store holds the two sides (vctProbes the front); a ray's hit reads the side facing it (stage 10, jah_voxel_ray.glsl).
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeBack[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// PHOTON-7 stage 10, 10-1: every cascade's NORMAL (the voxeliser's: the canonical normal of a two-sided voxel says which
// side is the front), after the backs - the side a ray's hit reads.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeNrm[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )

// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): THE DIRECT TERM, at the unit after every probe array (the
// same order VctLighting::setupBounceTextures binds them in). It is read with a
// plain Load3D at the voxel this invocation writes -- the D of the fixed point
// L = D + rho * G( L ) -- so it needs no sampler and no mip chain.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D directVoxel;
@add( vctTexUnit, 1 )

// JAHSHAKA (PHOTON-WRITER-1): THE VOXELISER'S EMISSIVE VOLUME, right after
// `directVoxel` (VctLighting::setupBounceTextures): its alpha is the voxel's merged
// roughness, which the bounce's re-emission reads. A plain Load3D, no sampler.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D voxelEmissiveTex;
@add( vctTexUnit, 1 )

// Jahshaka (PHOTON-VOXEL-5): THE BACK SIDE'S DIRECT TERM on the anisotropic tiers, after the
// emissive volume (VctLighting::setupBounceTextures): a two-sided voxel's back is its own fixed
// point, L_back = D_back + rho * G_back.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D directBackVoxel;
@add( vctTexUnit, 1 )

// Jahshaka (METAL-BOUNCE-1): THE METAL after the back side's direct term (VctLighting::setupBounceTextures):
// the F0 and Fresnel-90 weight of the specular lobe (every surface's, a dielectric's 0.04 too), re-emitted by the lobe's hemispherical
// integral. A plain Load3D, no sampler.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D voxelMetalTex;
@add( vctTexUnit, 1 )

// VOXEL-R13: THE HALF STORE's DIRECT TERMS after the metal (VctLighting::setupBounceTextures): each half's fixed point
// L_q = D_q + rho G_q (plain Load3Ds at the voxel written).
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D halfStoreDirect;
@add( vctTexUnit, 1 )

// JAHSHAKA (PHOTON-ENV-1): THE ONE ENVIRONMENT, at the unit after `directVoxel`
// (VctLighting::setupBounceTextures binds it last, and only while the host set a
// cube - the job property jah_env). Sampled with vctProbeSampler.
@property( jah_env )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform textureCube envCube;
	@add( vctTexUnit, 1 )
@end

vulkan( layout( ogre_s2 ) uniform sampler vctProbeSampler );

// PHOTON-7 stage 4: READ-WRITE - the pass reads back what it wrote (the store's own rounding), so a voxel at the
// float store's fixed point moves by exactly 0 and its brick can stop (PhotonBrickFeedback_cs.glsl). VOXEL-R13 stage 2:
// THE PASS'S TARGET is the half store's layout (six halves side by side), copied back over the listed bricks.
layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict image3D lightVoxel;
// PHOTON-7 stage 3, THE BRICK MASK (LightInjection_cs.glsl's buffer, word [1] of a brick): the most this frame's pass
// moved a voxel's total (relative), max-reduced per 8^3 brick - the feedback's per-brick change (stage 4).
layout( std430, ogre_U1 ) restrict coherent buffer jahBrickLayout { uint jahBrickMask[]; };
// PHOTON-7 stage 4, THE FEEDBACK (PhotonBrickFeedback_cs.glsl): the ACTIVE bricks this pass covers (8 groups of 4^3 a
// brick, an indirect dispatch), and their state - words 4 (the most a voxel's total moved, absolute) and 5 (the brick's
// sensitivity: its voxels' re-emission over their own code) written here.
layout( std430, ogre_U2 ) restrict readonly buffer jahFbListLayout { uint jahFbList[]; };
layout( std430, ogre_U3 ) restrict coherent buffer jahFbStateLayout { uint jahFbState[]; };
@property( jah_vct_tlas )
// PHOTON-7 stage 10, 10-2b: THE SIDES' GATHER BY TLAS RAYS (rayquery/rq_voxel_sides.comp, dispatched before this job over the
// same list): per listed brick slot, per voxel of it (x + 8 y + 64 z), per side, the gathered radiance as two
// packHalf2x16 words - read at the ray tier in place of the voxel rays.
layout( std430, ogre_U4 ) restrict readonly buffer jahSideGatherLayout { uint jahSideGather[]; };
@end

layout( local_size_x = @value( threads_per_group_x ),
        local_size_y = @value( threads_per_group_y ),
        local_size_z = @value( threads_per_group_z ) ) in;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform float3 voxelCellSize;
	uniform float3 invVoxelResolution;
	uniform float iterationDampening;

	uniform float4 vctInvResMaxLod[@value( hlms_num_vct_cascades )];

	@property( hlms_num_vct_cascades > 1 )
		uniform float4 fromPreviousProbeToNext[@value( hlms_num_vct_cascades ) - 1][2];
	@else
		// Unused, but declare them to shut up warnings of setting non-existant params
		uniform float4 fromPreviousProbeToNext[1][2];
	@end

	// JAHSHAKA (PHOTON-ENV-1): the environment as radiance, the store's units
	// (PHOTON-7 stage 1): w = the cube's mip count (xyz the pad: the cube holds the
	// sky's gain itself, PHOTON-7 10-3 R5); and the nine-band SH in world axes.
	uniform float4 envMips;
	uniform float4 envSh[9];
	// PHOTON-7 stage 3: yz = the bricks along x and y (the mask's and the state's index).
	uniform uint4 brickMark;
	// PHOTON-7 stage 10, 10-1: THE CACHE RAYS - xyz this cascade's volume origin (world: a voxel's world key), w the
	// last world shell (this cascade's normalised units); and x the rays a side, y the start bias (the finest cascade's
	// cell, in this cascade's normalised units), z this cascade's own world shell end D (the same units), w 1 when this
	// cascade is the finest (its planar cells take the ray's plane test)
	uniform float4 rayParams;
	uniform float4 rayCounts;
vulkan( }; )

#define p_voxelCellSize voxelCellSize
#define p_invVoxelResolution invVoxelResolution
#define p_iterationDampening iterationDampening
#define p_vctInvResMaxLod vctInvResMaxLod
#define p_vctFromPreviousProbeToNext fromPreviousProbeToNext
#define p_envMips envMips
#define p_envSh envSh
#define p_rayParams rayParams
#define p_rayCounts rayCounts
#define p_brickMark brickMark

@insertpiece( HeaderCS )

// THE BRICK MASK'S CHANGE (PHOTON-7 stage 3; LightInjection_cs.glsl's twin, the same text): how far a voxel's light moved,
// per channel, relative to the larger of its old and new value - one code is 1 % - over the float store's absolute floor.
float jahBrickChange( float3 a, float3 b )
{
	const float3 d = abs( b - a );
	const float3 m = max( max( abs( a ), abs( b ) ), float3( 5.9604645e-6, 5.9604645e-6, 5.9604645e-6 ) );
	float r = max( max( d.x / m.x, d.y / m.y ), d.z / m.z );
	if( ( floatBitsToUint( r ) & 0x7F800000u ) == 0x7F800000u )
		r = 1.0e30;
	return r;
}
shared uint jahBrickShared;
shared uint jahAbsShared;
shared uint jahSensShared;
shared uint jahPriorShared;
shared uint jahFluxShared;

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
    @insertpiece( BodyCS )
}
