@insertpiece( SetCrossPlatformSettings )
#extension GL_ARB_shader_group_vote: require

#define OGRE_imageLoad3D( inImage, iuv ) imageLoad( inImage, int3( iuv ) )
#define OGRE_imageWrite3D1( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )
#define OGRE_imageWrite3D4( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )

@insertpiece( PreBindingsHeaderCS )

vulkan_layout( ogre_t0 ) uniform texture3D voxelAlbedoTex;
vulkan_layout( ogre_t1 ) uniform texture3D voxelNormalTex;
vulkan_layout( ogre_t2 ) uniform texture3D voxelEmissiveTex;

vulkan( layout( ogre_s0 ) uniform sampler voxelAlbedoSampler );

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict writeonly image3D lightVoxel;

// JAHSHAKA fork ae2ed529f+155a56bf8 (was 0076): the DIRECT term's own volume, written by the same dispatch.
// The bounce's fixed point is L = D + rho * G( L ) and needs D at every pass; the
// total's two textures ping-pong, so D cannot be recovered from them after the first
// pass. Declared only when the host has a volume to write it to (a VctLighting with
// no bounce texture has no use for it): VctLighting::update() sets the property and
// binds the slot per injection, because this job is shared by name process-wide.
@pset( jahInjUav, 1 )
@property( vct_keep_direct )
	layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
	uniform restrict writeonly image3D directVoxel;
	@add( jahInjUav, 1 )
@end

// Jahshaka (PHOTON-VOXEL-5): THE ANISOTROPIC TIERS' SIDES AND HALVES. `backVoxel` is level 0's
// BACK side (the front is 2 x the total minus it: the total holds the sides' mean);
// `dirOut0..2` the directional volumes' level 0 per axis, composited here from each voxel's
// light PER HALF-AXIS - or, with a bounce, the direct part of it (`vct_keep_direct`: the bounce
// pass adds its own part in the anisotropic mip step 0); `directBackVoxel` the back side's
// direct term for the bounce's fixed point. The slots follow u0 and the direct term in this
// order (VctLighting::update binds them so).
@property( vct_anisotropic )
	layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
	uniform restrict writeonly image3D backVoxel;
	@add( jahInjUav, 1 )
	@property( vct_keep_direct )
		layout( vulkan( ogre_u@value(jahInjUav) ) vk_comma rgba16f )
		uniform restrict writeonly image3D directBackVoxel;
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
@end

// Jahshaka (PHOTON-VOXEL-3/-4): THE PER-HALF-AXIS COVERAGE at t3 (the faces looking +a) and
// t4 (looking -a), and THE SURFACE POSITION per half at t5 / t6 (VctLighting::update binds
// them with the other voxelizer volumes): the shadow march's opacity and its origin plane.
vulkan_layout( ogre_t3 ) uniform texture3D voxelCoveragePTex;
vulkan_layout( ogre_t4 ) uniform texture3D voxelCoverageNTex;
vulkan_layout( ogre_t5 ) uniform texture3D voxelPositionPTex;
vulkan_layout( ogre_t6 ) uniform texture3D voxelPositionNTex;

// JAHSHAKA (CLOUDS-2D-2): THE CLOUD LAYER'S FIELD -- the host binds it at t7 (after the
// surface position, PHOTON-VOXEL-4)
// and sets `jah_cloud_shadow` when a 2D cloud layer shades the sun
// (OgreScene::bindCloudInjection). Its three parameters are declared in the
// Params block below whether or not it is bound, so the job's parameter list
// is one list; without the property nothing reads them.
@property( jah_cloud_shadow )
	vulkan_layout( ogre_t7 ) uniform texture2D jahCloudField;
	vulkan( layout( ogre_s7 ) uniform sampler jahCloudSampler );
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
	uniform float bakingMultiplier;
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
vulkan( }; )

#define p_numLights numLights
#define p_bakingMultiplier bakingMultiplier
//#define p_voxelOrigin voxelOrigin
#define p_voxelCellSize voxelCellSize
#define p_invVoxelResolution invVoxelResolution
#define p_higherMipHalfWidth higherMipHalfWidth

// Jahshaka (PHOTON-VOXEL-5; the VOXEL-4 audit's F3): THE ONE READER'S RULES for the shadow
// march - after the parameters, which its functions read.
@insertpiece( JahInjectionReader )

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
