@insertpiece( SetCrossPlatformSettings )

#define OGRE_imageWrite3D4( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )

vulkan_layout( ogre_t0 ) uniform texture3D inLightLowerMip0;
vulkan_layout( ogre_t1 ) uniform texture3D inLightLowerMip1;
vulkan_layout( ogre_t2 ) uniform texture3D inLightLowerMip2;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict writeonly image3D outLightHigherMip0;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict writeonly image3D outLightHigherMip1;

layout( vulkan( ogre_u2 ) vk_comma @insertpiece(uav2_pf_type) )
uniform restrict writeonly image3D outLightHigherMip2;

// PHOTON-7 stage 4: THE FEEDBACK'S BRICK LIST (jah_brick_list) - the levels regenerated over the active bricks only.
@property( jah_brick_list )
	layout( std430, ogre_U3 ) restrict readonly buffer jahFbListLayout { uint jahFbList[]; };
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

//layout( local_size_x = 4,
//		local_size_y = 4,
//		local_size_z = 4 ) in;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform int4 higherMipHalfRes_lowerMipHalfWidth;
	// PHOTON-7 stage 2: the region's first texel of the level written (a whole pass at 0).
	uniform uint4 regionOrigin;
vulkan( }; )

#define p_higherMipHalfRes higherMipHalfRes_lowerMipHalfWidth.xyz
#define p_lowerMipHalfWidth higherMipHalfRes_lowerMipHalfWidth.w
#define p_regionOrigin regionOrigin

@insertpiece( HeaderCS )

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
