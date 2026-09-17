// Jahshaka — the HDR meter's histogram, cleared (EXPOSURE-2).
//
// Two rows of `bins` unsigned counters, zeroed before the build pass adds into
// them atomically. It is a pass of its own and not folded into either
// neighbour for one reason: an atomic add from thread group A must not race a
// clear from thread group B, and the only ordering the compositor gives us
// between thread groups is a pass boundary.
//
// It costs one thread group of 64 threads over 128 bins - two iterations each,
// no reads. Measured as part of the meter's total in the lane report.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

@property( syntax == glsl )
	#define ogre_u0 binding = 0
@end

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly uimage2D histogram;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	const int bins = imageSize( histogram ).x;
	for( int i = int( gl_GlobalInvocationID.x ); i < bins; i += int( gl_WorkGroupSize.x ) )
	{
		imageStore( histogram, ivec2( i, 0 ), uvec4( 0u ) );
		imageStore( histogram, ivec2( i, 1 ), uvec4( 0u ) );
	}
}
