@insertpiece( SetCrossPlatformSettings )

#define OGRE_imageWrite3D4( outImage, iuv, value ) imageStore( outImage, int3( iuv ), value )

// Jahshaka (PHOTON-VOXEL-5): the bounce's part of the directional level 0 (AnisotropicMipVctStep0_
// piece_cs.any) - the level-0 totals (the sides' mean and the back), their direct terms, the
// normal (its side), the coverage per half, and the injection's composited direct part per axis.
vulkan_layout( ogre_t0 ) uniform texture3D inLightTotal;
vulkan_layout( ogre_t1 ) uniform texture3D inLightBack;
vulkan_layout( ogre_t2 ) uniform texture3D inDirectTotal;
vulkan_layout( ogre_t3 ) uniform texture3D inDirectBack;
vulkan_layout( ogre_t4 ) uniform texture3D inNormal;
vulkan_layout( ogre_t5 ) uniform texture3D inCoveragePTex;
vulkan_layout( ogre_t6 ) uniform texture3D inCoverageNTex;
vulkan_layout( ogre_t7 ) uniform texture3D inDirectDir0;
vulkan_layout( ogre_t8 ) uniform texture3D inDirectDir1;
vulkan_layout( ogre_t9 ) uniform texture3D inDirectDir2;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict writeonly image3D outLightHigherMip0;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict writeonly image3D outLightHigherMip1;

layout( vulkan( ogre_u2 ) vk_comma @insertpiece(uav2_pf_type) )
uniform restrict writeonly image3D outLightHigherMip2;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( HeaderCS )

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform int higherMipHalfWidth;
vulkan( }; )

#define p_higherMipHalfWidth higherMipHalfWidth

void main()
{
	@insertpiece( BodyCS )
}
