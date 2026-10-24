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
// Jahshaka (PHOTON-7 stage 1): THE CELL-RELATIVE LEVEL 0 of a surface-position volume (PhotonMipChain.h):
// the coverage the children's O x f is premultiplied by, at slot 1, level 0.
@property( jah_cell_relative )
	vulkan_layout( ogre_t1 ) uniform texture3D srcCoverage;
@end
@property( jah_mean_of_sides )
	vulkan_layout( ogre_t1 ) uniform texture3D srcOtherSide;
@end

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
@property( jah_cell_relative )
	// level 0 holds O x f per axis (f in the child's own cell, xyz) and the caster share (w); level 1 the mean of the
	// children's O x p, p = ( cell + f ) / size the box's coordinate - the mean the blit took of the absolute store
	ivec3 srcSize = textureSize( srcLevel, 0 );
	vec3 invSrc = vec3( 1.0 ) / vec3( srcSize );
	vec4 acc = vec4( 0.0 );
	for( int k = 0; k < 8; ++k )
	{
		ivec3 s = min( iuvw * 2 + ivec3( k & 1, ( k >> 1 ) & 1, k >> 2 ), srcSize - 1 );
		vec4 p = OGRE_Load3D( srcLevel, s, 0 );
		vec3 o = OGRE_Load3D( srcCoverage, s, 0 ).xyz;
		acc += vec4( ( p.xyz + o * vec3( s ) ) * invSrc, p.w );
	}
	imageStore( dstLevel, iuvw, acc * 0.125 );
@else
	vec3 uvw = ( vec3( iuvw ) + 0.5 ) / vec3( dstSize );
	@property( jah_mean_of_sides )
		// LEVEL 1 OF A TWO-SIDED LIGHT STORE (PHOTON-7 stage 7): the two sides' mean, filtered alike
		imageStore( dstLevel, iuvw,
					0.5 * ( OGRE_SampleLevel( srcLevel, srcSampler, uvw, 0.0 ) +
							OGRE_SampleLevel( srcOtherSide, srcSampler, uvw, 0.0 ) ) );
	@else
		imageStore( dstLevel, iuvw, OGRE_SampleLevel( srcLevel, srcSampler, uvw, 0.0 ) );
	@end
@end
}
