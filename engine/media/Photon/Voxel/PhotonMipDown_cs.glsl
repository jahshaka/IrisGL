@insertpiece( SetCrossPlatformSettings )

// PHOTON'S MIP DOWNSAMPLE (ENGINE-UPGRADE-ASYNC-3 stage 1): ONE LEVEL OF A VOLUME'S CHAIN, ON
// COMPUTE. It replaced the image blit (TextureGpu::_autogenerateMipmaps -> vkCmdBlitImage), which
// the Vulkan spec allows on a graphics queue only, so the GI region could not record on a
// compute-only family. THE FILTER IS THE BLIT'S: the blit maps the destination texel centre
// (i + 0.5) / dstSize into the source level in normalised coordinates and reads it with
// VK_FILTER_LINEAR, clamped to the edge. That is exactly this one sample: on an even axis the
// point is the shared corner of two source texels, so the 2 x 2 x 2 block is averaged with equal
// weights - the mean of 8 for every channel, alpha and colour alike (the volumes are stored
// premultiplied: the coverage-weighted mean IS the box mean of what is stored); on an odd axis it
// is the blit's own odd-axis mapping, unchanged.
//
// Texture slot 0 is a ONE-LEVEL view of level L - 1 with a linear clamp sampler; uav slot 0 is
// level L. The store format qualifier is generated from the bound level (uav0_pf_type).

vulkan_layout( ogre_t0 ) uniform texture3D srcLevel;
vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict writeonly image3D dstLevel;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	ivec3 dstSize = imageSize( dstLevel );
	ivec3 iuvw = ivec3( gl_GlobalInvocationID.xyz );
	if( iuvw.x >= dstSize.x || iuvw.y >= dstSize.y || iuvw.z >= dstSize.z )
		return;
	vec3 uvw = ( vec3( iuvw ) + 0.5 ) / vec3( dstSize );
	imageStore( dstLevel, iuvw, OGRE_SampleLevel( srcLevel, srcSampler, uvw, 0.0 ) );
}
