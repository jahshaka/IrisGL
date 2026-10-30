@insertpiece( SetCrossPlatformSettings )
#extension GL_ARB_shader_group_vote: require

#define OGRE_imageLoad3D( inImage, iuv ) imageLoad( inImage, int3( iuv ) )
#define OGRE_imageWrite3D1( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )
#define OGRE_imageWrite3D4( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )

@insertpiece( PreBindingsHeaderCS )

/// Jahshaka (PHOTON-7 stage 1): the chain's geometry is bound as arrays (the bounce job's pattern): array
/// bindings are reflected when the chain holds more than one cascade (hlms_num_vct_cascades is never 0).
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

vulkan_layout( ogre_t0 ) uniform texture3D voxelAlbedoTex;
vulkan_layout( ogre_t1 ) uniform texture3D voxelNormalTex;
vulkan_layout( ogre_t2 ) uniform texture3D voxelEmissiveTex;

vulkan( layout( ogre_s0 ) uniform sampler voxelAlbedoSampler );

// PHOTON-7 stage 2: AT A SCROLL'S COMMIT THE DIRECT TERM IS RE-INJECTED AS A DELTA (jah_inject_delta): the total, its
// back side and the direct terms are READ as well (total += D_new - D_old), so they are read-write. PHOTON-7 stage 3: so
// they are at every injection - the brick mask compares each voxel's direct term with the one it replaces.
layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict image3D lightVoxel;

// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): the DIRECT term's own volume, written by the same dispatch.
// The bounce's fixed point is L = D + rho * G( L ) and needs D at every pass; the
// total's two textures ping-pong, so D cannot be recovered from them after the first
// pass. Declared only when the host has a volume to write it to (a VctLighting with
// no bounce texture has no use for it): VctLighting::update() sets the property and
// binds the slot per injection, because this job is shared by name process-wide.
@pset( jahInjUav, 1 )
@property( vct_keep_direct )
	layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
	uniform restrict image3D directVoxel;
	@add( jahInjUav, 1 )
@end

// Jahshaka (PHOTON-VOXEL-5): THE ANISOTROPIC TIERS' SIDES AND HALVES. `backVoxel` is level 0's
// BACK side (PHOTON-7 stage 7: `lightVoxel` holds the FRONT - the store keeps both sides, their mean the total);
// `dirOut0..2` the directional volumes' level 0 per axis, composited here from each voxel's
// light PER HALF-AXIS - or, with a bounce, the direct part of it (`vct_keep_direct`: the bounce
// pass adds its own part in the anisotropic mip step 0); `directBackVoxel` the back side's
// direct term for the bounce's fixed point. The slots follow u0 and the direct term in this
// order (VctLighting::update binds them so).
layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
uniform restrict image3D backVoxel;
@add( jahInjUav, 1 )
@property( vct_keep_direct )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict image3D directBackVoxel;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut0;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut1;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut2;
@else
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut0;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut1;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D dirOut2;
@end
@add( jahInjUav, 1 )
// VOXEL-R13 stage 1: THE HALF STORE - level 0's six half lights (jah_voxel_halves.glsl: one volume an axis, the -a half
// in [0, W), the +a half in [W, 2W)), each (radiance x k, k) with k the half's capped coverage; with a bounce their
// direct terms beside them (the bounce's fixed point per half, the delta's old terms). Read-write: the delta and the
// brick mask compare each half's direct term with the one it replaces.
layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
uniform restrict image3D halfLightX;
@add( jahInjUav, 1 )
layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
uniform restrict image3D halfLightY;
@add( jahInjUav, 1 )
layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
uniform restrict image3D halfLightZ;
@add( jahInjUav, 1 )
@property( vct_keep_direct )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict image3D halfDirectX;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict image3D halfDirectY;
	@add( jahInjUav, 1 )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict image3D halfDirectZ;
	@add( jahInjUav, 1 )
@end
// PHOTON-7 stage 2, THE WORK LAW'S DOOR: a delta injection counts the voxels it lit, cumulatively - word 0 those inside
// the scroll's slab (outside the box the scroll kept), word 1 those outside it (the voxels whose direct term changed:
// the band), word 2 the injections, word 3 (stage 4) the voxels it MARCHED. A UAV buffer after the textures, read back by a routed buffer copy.
@property( jah_inject_delta )
	layout( std430, ogre_U@value(jahInjUav) ) restrict coherent buffer jahCommitLayout { uint jahCommitCount[]; };
	@add( jahInjUav, 1 )
@end
// PHOTON-7 stage 3, THE BRICK MASK: two words per 8^3 brick of this cascade's cells - [0] the most a voxel's DIRECT term
// changed in this injection, [1] the most the update's last pass moved its total (LightVctBounceInject) - each the float
// bits of a relative change (jahBrickChange), max-reduced. Cleared before every update (PhotonVoxelLighting::update), the
// last UAV.
layout( std430, ogre_U@value(jahInjUav) ) restrict coherent buffer jahBrickLayout { uint jahBrickMask[]; };
@add( jahInjUav, 1 )
// PHOTON-7 stage 5: THE FEEDBACK'S BRICK STATE (10 words a brick, PhotonBrickFeedback_cs.glsl's FB_W; word 9 its moved flux): word 7 takes the most a
// voxel's total changed in this injection, absolute radiance (atomicMax of the float bits; the frame's CLEAR zeroes it) -
// the cards' accumulation reads it beside a pass's move.
layout( std430, ogre_U@value(jahInjUav) ) restrict coherent buffer jahFbStateLayout { uint jahFbState[]; };

// Jahshaka (METAL-BOUNCE-1): THE METAL at t3 - the F0 (rgb) and Fresnel-90 weight (alpha) of every surface's
// specular lobe, re-emitted by the lobe's directional integral (JahSpecularAlbedo).
vulkan_layout( ogre_t3 ) uniform texture3D voxelMetalTex;
// Jahshaka (JUNCTION-NORMAL-1): THE EMISSIVE FACTOR PER HALF-AXIS at t4 (the faces looking +a) and t5
// (looking -a): what each half's faces emit over the voxel's area mean (VoxelMerge_piece_cs.any).
vulkan_layout( ogre_t4 ) uniform texture3D voxelEmitPTex;
vulkan_layout( ogre_t5 ) uniform texture3D voxelEmitNTex;
// VOXEL-R13 stage 1: THE HALVES' OWN FACES at t6..t8 (where each half's faces lie across its axis) and t9..t11 (their
// mean normal), both halves an axis (PhotonVoxelizerSourceBase.h): a half's lamp march starts on its own faces and its
// cosine takes its own normal.
vulkan_layout( ogre_t6 ) uniform texture3D voxelHalfSideX;
vulkan_layout( ogre_t7 ) uniform texture3D voxelHalfSideY;
vulkan_layout( ogre_t8 ) uniform texture3D voxelHalfSideZ;
vulkan_layout( ogre_t9 ) uniform texture3D voxelHalfNrmX;
vulkan_layout( ogre_t10 ) uniform texture3D voxelHalfNrmY;
vulkan_layout( ogre_t11 ) uniform texture3D voxelHalfNrmZ;

// Jahshaka (PHOTON-7 stage 1, L1: LIGHT IS A WORLD FUNCTION): THE CHAIN'S GEOMETRY - every cascade's coverage per
// half-axis (the faces looking +a, -a) and surface position per half, level 0, as four arrays over the WHOLE tier
// chain (one permutation per chain length; this cascade is entry chainSelf, the ones inside it bound but never read):
// the shadow march runs through the chain on fixed shells (JahInjectionReader's jahInjectVisibility).
@pset( jahInjTex, 12 )
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainCovP[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainCovN[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainPosP[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainPosN[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )
// PHOTON-7 stage 5 (5-4): every cascade's DIRECT term, both sides (its total and back where it keeps none) - what a
// reader read at a cleared voxel's world point before, the next cascade out's (the absolute move's reference, below).
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainDirect[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture3D chainDirectBack[@value( hlms_num_vct_cascades )];
@add( jahInjTex, hlms_num_vct_cascades )

// JAHSHAKA (CLOUDS-2D-2): THE CLOUD LAYER'S FIELD -- the host binds it at the unit after the chain's geometry
// (PhotonVoxelLighting::injectionTexUnits) and sets `jah_cloud_shadow` when a 2D cloud layer shades the sun
// (OgreScene::bindCloudInjection). Its three parameters are declared in the Params block below whether or not it
// is bound, so the job's parameter list is one list; without the property nothing reads them.
@property( jah_cloud_shadow )
	vulkan_layout( ogre_t@value(jahInjTex) ) uniform texture2D jahCloudField;
	vulkan( layout( ogre_s@value(jahInjTex) ) uniform sampler jahCloudSampler );
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

//layout( local_size_x = 4,
//		local_size_y = 4,
//		local_size_z = 4 ) in;


@insertpiece( HeaderCS )

@property( jah_cloud_shadow )
	#define JAH_CLOUD_TAU( uv ) textureLod( sampler2D( jahCloudField, jahCloudSampler ), uv, 0.0 ).x
	@insertpiece( JahCloudShadow )
@end

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint numLights;
	//uniform float3 voxelOrigin;
	uniform float3 voxelCellSize;
	uniform float3 invVoxelResolution;
	// JAHSHAKA (CLOUDS-2D-2): the cloud layer's map (1 / tile, strength,
	// scroll xz), its sun throw (toSun.xz / toSun.y, altitude, 1 / mu_s) and
	// this volume's world origin (the voxel centres are origin-relative).
	uniform float4 jahCloudMap;
	uniform float4 jahCloudSun;
	uniform float4 jahCloudOrigin;
	// Jahshaka (PHOTON-VOXEL-5): the directional volumes' half width - where a +a texel lives.
	uniform int higherMipHalfWidth;
	// Jahshaka (PHOTON-7 stage 1): THE CHAIN - this cascade's entry, and per entry j its box corner relative to this
	// cascade's (metres; w its cell, metres) and its resolution (xyz) with the world distance its shell ends at (w,
	// from the march's start: the sum of 8 cells of every cascade up to j; the last one's past any march).
	uniform uint chainSelf;
	// Jahshaka (PHOTON-7 stage 1): THE LIGHTS A VOXEL TAKES - the first this many of the list's one order that reach it.
	uniform uint lightsPerVoxel;
	// PHOTON-7 stage 5's 5-3 fix, a measuring arm (gi.voxelAreaStrata): an area light's visibility over n x n stratified
	// marches (0: the one march to its centre).
	uniform uint areaStrata;
	// PHOTON-7 stage 2: the box of cells the scroll KEPT ([keepLo, keepHi): the overlap; its slab is outside it), read by
	// a delta injection's count (declared in every permutation, so the job's parameter list is one list).
	uniform int4 keepLo;
	uniform int4 keepHi;
	// PHOTON-7 stage 2: THE REGION a dispatch covers starts at this cell (a multiple of the 4^3 group; 0 = the whole
	// volume, the groups set by the host); w = 1 on the dispatch that counts the injection (word 2).
	uniform uint4 injectOrigin;
	// PHOTON-7 stage 4's read R4: THE UNION OF AN INJECTION'S BOXES - x the boxes dispatched before this one (a 4^3 group one
	// of them covered is not marched again), and every box of the injection in cells (aligned to the groups).
	uniform uint4 injectPrior;
	uniform uint4 injectBoxLo[16];
	uniform uint4 injectBoxHi[16];
	// PHOTON-7 stage 3: the brick mask's bricks per axis (xyz; w unused).
	uniform uint4 brickDims;
	uniform float4 chainOrigin[@value( hlms_num_vct_cascades )];
	uniform float4 chainRes[@value( hlms_num_vct_cascades )];
vulkan( }; )

#define p_numLights numLights
//#define p_voxelOrigin voxelOrigin
#define p_voxelCellSize voxelCellSize
#define p_invVoxelResolution invVoxelResolution
#define p_higherMipHalfWidth higherMipHalfWidth
#define p_chainSelf chainSelf
#define p_lightsPerVoxel lightsPerVoxel
#define p_areaStrata areaStrata
#define p_chainOrigin chainOrigin
#define p_chainRes chainRes
#define p_keepLo keepLo
#define p_keepHi keepHi
#define p_injectOrigin injectOrigin
#define p_injectPrior injectPrior
#define p_injectBoxLo injectBoxLo
#define p_injectBoxHi injectBoxHi
#define p_brickDims brickDims
#define JAH_INJ_CHAIN @value( hlms_num_vct_cascades )

// Jahshaka (PHOTON-VOXEL-5; the VOXEL-4 audit's F3): THE ONE READER'S RULES for the shadow
// march - after the parameters, which its functions read.
@insertpiece( JahInjectionReader )

// THE BRICK MASK'S CHANGE (PHOTON-7 stage 3; its twin is LightVctBounceInject_cs.glsl's): how far a voxel's light moved, per
// channel, relative to the larger of its old and new value - one code is 1 % (the reflection rest's rule) - over the float
// store's absolute floor (Types.h kGiBrickFloor: its subnormal step is one code there). A non-finite value is a change.
float jahBrickChange( float3 a, float3 b )
{
	const float3 d = abs( b - a );
	const float3 m = max( max( abs( a ), abs( b ) ), float3( 5.9604645e-6, 5.9604645e-6, 5.9604645e-6 ) );
	float r = max( max( d.x / m.x, d.y / m.y ), d.z / m.z );
	if( ( floatBitsToUint( r ) & 0x7F800000u ) == 0x7F800000u )
		r = 1.0e30;
	return r;
}
// ...and the work group's most, reduced in shared memory before one atomic per group.
shared uint jahBrickShared;
shared uint jahBrickAbsShared;
shared uint jahBrickFluxShared;

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
