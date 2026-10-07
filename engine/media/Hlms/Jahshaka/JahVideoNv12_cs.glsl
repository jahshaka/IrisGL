// Jahshaka — THE VIDEO FRAME'S COLOUR PASS (VIDEO-REC-1): a view's final picture,
// RGBA8 display codes, into NV12 for the hardware encoder, before the readback.
//
// THE MATRIX, stated once (View::setVideoReadback, Types.h VideoFrameNv12):
// ITU-R BT.709, Kr = 0.2126, Kb = 0.0722, on the picture AS DISPLAYED — the codes
// the chain already sRGB-encoded, read through a UNORM view so nothing linearises
// them. The picture is encoded once, by the chain; this is a matrix, never a
// second transfer curve. LIMITED RANGE: Y = 16 + 219 Y', C = 128 + 224 C'.
// The chroma of a 2x2 block is the mean of its four pixels' chroma.
//
// THE OUTPUT is one R32_UINT target a quarter of the picture wide and one and a
// half times as high, i.e. the NV12 bytes in their encoder order: rows 0 .. H-1
// are the luma plane (four luma bytes a texel, the leftmost in the low byte),
// rows H .. 3H/2-1 the interleaved chroma plane (Cb, Cr, Cb, Cr: two blocks a
// texel). A 32-bit texel is a format every device stores to; an R8 storage
// image is an optional one. One thread converts one 4x2 tile of pixels: two
// luma texels and the chroma texel of the same two blocks.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D picture;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly uimage2D nv12;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

uint jahCode( float v )
{
	return uint( clamp( floor( v + 0.5 ), 0.0, 255.0 ) );
}

void main()
{
	const ivec2 outSize = imageSize( nv12 );
	// The picture is outSize.x * 4 wide and two thirds of outSize.y high.
	const int lumaRows = ( outSize.y * 2 ) / 3;
	const ivec2 tile = ivec2( gl_GlobalInvocationID.xy );
	if( tile.x >= outSize.x || tile.y * 2 >= lumaRows )
		return;

	uint luma[2];
	luma[0] = 0u;
	luma[1] = 0u;
	vec2 chroma[2];
	chroma[0] = vec2( 0.0 );
	chroma[1] = vec2( 0.0 );
	for( int row = 0; row < 2; ++row )
	{
		for( int col = 0; col < 4; ++col )
		{
			const ivec2 px = ivec2( tile.x * 4 + col, tile.y * 2 + row );
			const vec3 c = texelFetch( picture, px, 0 ).rgb;
			const float y = dot( c, vec3( 0.2126, 0.7152, 0.0722 ) );
			luma[row] |= jahCode( 16.0 + 219.0 * y ) << uint( col * 8 );
			// Cb' = (B' - Y') / 1.8556, Cr' = (R' - Y') / 1.5748 (BT.709).
			chroma[col >> 1] += vec2( ( c.b - y ) / 1.8556, ( c.r - y ) / 1.5748 );
		}
	}
	imageStore( nv12, ivec2( tile.x, tile.y * 2 ), uvec4( luma[0], 0u, 0u, 0u ) );
	imageStore( nv12, ivec2( tile.x, tile.y * 2 + 1 ), uvec4( luma[1], 0u, 0u, 0u ) );
	const vec2 c0 = 128.0 + 224.0 * ( chroma[0] * 0.25 );
	const vec2 c1 = 128.0 + 224.0 * ( chroma[1] * 0.25 );
	const uint packedChroma = jahCode( c0.x ) | ( jahCode( c0.y ) << 8u ) |
							  ( jahCode( c1.x ) << 16u ) | ( jahCode( c1.y ) << 24u );
	imageStore( nv12, ivec2( tile.x, lumaRows + tile.y ), uvec4( packedChroma, 0u, 0u, 0u ) );
}
