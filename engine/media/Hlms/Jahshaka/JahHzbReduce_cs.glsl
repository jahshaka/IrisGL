// Jahshaka — HZB reduce (hierarchical depth pyramid, mip N from mip N-1).
//
// WHICH DEPTH A LEVEL KEEPS IS THE JOB'S OWN PROPERTY (`hzb_farthest`), because
// the two readers of a depth pyramid want opposite things and only one of them
// can be conservative (ATOM-SUBSTRATE-1 fix round, 2026-09-22):
//
//   * AN OCCLUSION CULL wants the FARTHEST depth of a footprint. Then
//     "my nearest point is behind this level's value" proves every pixel under
//     it already holds something in front of me. With the closest depth a texel
//     that is half wall and half sky reports the wall, and an object visible
//     through the sky half is culled — geometry lost, the one error a
//     hierarchical test may never make.
//   * A STACKLESS SCREEN-SPACE TRACE wants the CLOSEST depth, so it can skip a
//     whole region it cannot possibly have hit yet. That build is Photon's to
//     ask for when its gather lands; nothing reads it today.
//
// CLOSEST OR FARTHEST, and which arithmetic that is depends on the depth convention:
// this engine runs Ogre's REVERSE-Z (RenderSystem::isReverseDepth, the Vulkan
// default at this pin — the shadow-map clear material already branches on it),
// where the near plane is 1 and the far plane 0, so the CLOSEST sample of a
// footprint is its MAXIMUM stored value. `hzb_reverse_z` carries that from C++
// rather than being assumed, so a build that ever turns reverse-Z off still
// produces a conservative pyramid.
//
// CONSERVATIVE ON ODD SIZES, AND ONLY WHERE IT HAS TO BE. Mip dimensions floor,
// so on an odd-sized level the LAST column/row of the source is covered by no
// destination texel at all, and a plain 2x2 gather would then describe a
// footprint it has not read all of — which in either direction is a level that
// lies about a region, the one error a hierarchical depth test must never make. The gather therefore extends to 3 samples on that axis for
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

// Reverse-Z makes NEAR = 1, so "keep the nearest" is a max there and a min
// otherwise; "keep the farthest" is the other one. The seed of the extreme is
// the opposite end of the range, so an untouched footprint cannot win.
@property( hzb_reverse_z )
	@property( hzb_farthest )
		#define HZB_PICK( a, b ) min( (a), (b) )
		#define HZB_SEED 1.0
	@else
		#define HZB_PICK( a, b ) max( (a), (b) )
		#define HZB_SEED 0.0
	@end
@else
	@property( hzb_farthest )
		#define HZB_PICK( a, b ) max( (a), (b) )
		#define HZB_SEED 0.0
	@else
		#define HZB_PICK( a, b ) min( (a), (b) )
		#define HZB_SEED 1.0
	@end
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

	float kept = HZB_SEED;
	for( int y = 0; y <= 1 + extraY; ++y )
	{
		for( int x = 0; x <= 1 + extraX; ++x )
		{
			ivec2 uv = min( base + ivec2( x, y ), srcSize - ivec2( 1, 1 ) );
			kept = HZB_PICK( kept, imageLoad( srcMip, uv ).x );
		}
	}

	OGRE_imageWrite2D1( dstMip, dstUv, kept );
}
