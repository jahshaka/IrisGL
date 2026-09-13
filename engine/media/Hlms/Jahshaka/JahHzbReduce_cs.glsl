// Jahshaka — HZB reduce (hierarchical depth pyramid, mip N from mip N-1).
//
// CLOSEST DEPTH, and which arithmetic that is depends on the depth convention:
// this engine runs Ogre's REVERSE-Z (RenderSystem::isReverseDepth, the Vulkan
// default at this pin — the shadow-map clear material already branches on it),
// where the near plane is 1 and the far plane 0, so the CLOSEST sample of a
// footprint is its MAXIMUM stored value. `hzb_reverse_z` carries that from C++
// rather than being assumed, so a build that ever turns reverse-Z off still
// produces a conservative pyramid.
//
// CONSERVATIVE ON ODD SIZES, AND ONLY WHERE IT HAS TO BE. Mip dimensions floor,
// so on an odd-sized level the LAST column/row of the source is covered by no
// destination texel at all, and a plain 2x2 gather would claim a footprint is
// FARTHER than something inside it — the one error a hierarchical depth test
// must never make. The gather therefore extends to 3 samples on that axis for
// the LAST destination texel of the axis, and only for it.
//
// The distinction matters: extending every texel is also conservative, but it
// makes each interior texel absorb its neighbour's first column/row, so the
// whole level reads closer than it is and the pyramid rejects less than it
// could. Nvidia's and Epic's reducers make the same last-texel-only choice.
// Measured here as the difference between an exactly-8-wide footprint at mip 3
// and a 9- or 10-wide one on every odd level (at 1080p: 15, 7 and 3).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

@property( syntax == glsl )
	#define ogre_u0 binding = 0
	#define ogre_u1 binding = 1
@end

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D dstMip;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece( uav1_pf_type ) )
uniform restrict readonly image2D srcMip;

@property( hzb_reverse_z )
	#define HZB_CLOSER( a, b ) max( (a), (b) )
	#define HZB_FARTHEST 0.0
@else
	#define HZB_CLOSER( a, b ) min( (a), (b) )
	#define HZB_FARTHEST 1.0
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	ivec2 dstUv = ivec2( gl_GlobalInvocationID.xy );
	ivec2 dstSize = imageSize( dstMip );
	if( dstUv.x >= dstSize.x || dstUv.y >= dstSize.y )
		return;

	ivec2 srcSize = imageSize( srcMip );
	ivec2 base = dstUv * 2;

	// The uncovered source column/row is the LAST one, so only the last
	// destination texel of each axis gathers a third sample.
	int extraX = ( ( srcSize.x & 1 ) != 0 && dstUv.x == dstSize.x - 1 ) ? 1 : 0;
	int extraY = ( ( srcSize.y & 1 ) != 0 && dstUv.y == dstSize.y - 1 ) ? 1 : 0;

	float closest = HZB_FARTHEST;
	for( int y = 0; y <= 1 + extraY; ++y )
	{
		for( int x = 0; x <= 1 + extraX; ++x )
		{
			ivec2 uv = min( base + ivec2( x, y ), srcSize - ivec2( 1, 1 ) );
			closest = HZB_CLOSER( closest, imageLoad( srcMip, uv ).x );
		}
	}

	OGRE_imageWrite2D1( dstMip, dstUv, closest );
}
