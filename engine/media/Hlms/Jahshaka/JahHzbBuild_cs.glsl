// Jahshaka — THE HZB IN ONE DISPATCH (SPEED-VR-MEM item 1, 2026-10-05; perf audit
// 2026-10-03 atom-raster 3a, memory M-3). Every level of the hierarchical depth
// pyramid from the depth attachment, in a single compute dispatch: the shape of
// AMD FidelityFX's single-pass downsampler (SPD) on its no-wave-ops path — each
// workgroup reduces one 64x64 tile of level 0 down to level 6 in shared memory,
// and the LAST workgroup to finish (one global atomic) reduces the tail. It
// replaced a seed copy plus one dispatch per level (13 at 1080p), each of which
// drained the pipe behind a barrier.
//
// THE TEXELS ARE THE OLD ONES, BIT FOR BIT. The level rule is the per-level
// reducer's (the one JahHzbReduce_cs held, and chain.hzb's footprint arm):
// level k texel i reads texels 2i and 2i+1 of level k-1, clamped to the level,
// plus 2i+2 when level k-1 is odd-sized and i is the LAST texel of level k.
// Composed, that rule gives every texel of level k the closed footprint
// [i 2^k, (i+1) 2^k) of level 0, except the last of each axis, which runs to the
// edge of the image. Min and max are exact, so any order of reduction over the
// same footprint gives the same float: the tile scheme below changes the order
// of the picks and nothing else.
//
// THE TILES ARE CUT SO NO FOOTPRINT CROSSES ONE. The last texel's footprint runs
// to the image edge, so a tile grid cut at every 64 texels would hand part of it
// to a neighbour. Tile t of an axis is [64t, 64t + 64), except the LAST FULL
// tile, which runs to the image edge (64 to 127 texels wide); a partial tile
// past it does nothing. An axis shorter than 64 is one tile. With that cut,
// every level-1..6 texel's footprint lies inside its own tile, and level 6 is
// exactly one texel per tile.
//
// MIP 0 STAYS A COPY OF THE DEPTH: the NEXT frame's first occlusion cull reads
// last frame's pyramid before this frame's depth exists (the depth attachment
// is discardable across frames), and an object up to two texels wide tests
// against mip 0. The copy is written here, from the same reads level 1 makes.
//
// THE SLOTS: UAV slots 0..15 are mips 0..15 (a pyramid with fewer levels binds
// its last mip into the unused slots, which nothing writes), slot 16 is the
// tail's arrival counter — a 16-byte buffer the engine owns for its lifetime,
// zero at creation and put back to zero by the last workgroup of every dispatch.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;

// Only the levels the TAIL reads across workgroups (6 and up: written by one
// workgroup, read by the last) are coherent; the tile levels never leave their
// workgroup's own shared memory, so their stores take the ordinary path.
@foreach( hzb_tile_slots, n )
layout( vulkan( ogre_u@n ) vk_comma r32f ) uniform writeonly image2D mip@n;
@end
@foreach( hzb_slots, n, hzb_tile_slots )
layout( vulkan( ogre_u@n ) vk_comma r32f ) uniform coherent image2D mip@n;
@end

layout( std430, ogre_U@value( hzb_slots ) ) coherent buffer counterLayout { uint arrivals[]; };

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

#define HZB_THREADS 256
#define HZB_TILE_LEVELS 6

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// Level 1 of a tile is at most 63 x 63 texels, level 2 at most 31 x 31: two
// buffers, alternated level by level.
shared float sA[64 * 64];
shared float sB[32 * 32];
shared uint sIsLast;

void storeLevel( int k, ivec2 uv, float v )
{
	switch( k )
	{
@foreach( hzb_slots, n )
	case @n: imageStore( mip@n, uv, vec4( v, 0.0, 0.0, 0.0 ) ); break;
@end
	default: break;
	}
}

float loadLevel( int k, ivec2 uv )
{
	switch( k )
	{
@foreach( hzb_slots, n, hzb_tile_slots )
	case @n: return imageLoad( mip@n, uv ).x;
@end
	default: return HZB_SEED;
	}
}

ivec2 levelSize( ivec2 size0, int k )
{
	return max( size0 >> k, ivec2( 1 ) );
}

// The extra (third) sample on an axis: the source level is odd-sized and this
// is the destination's last texel. Exactly the reducer's rule.
ivec2 extraOf( ivec2 dst, ivec2 dstSize, ivec2 srcSize )
{
	return ivec2( ( ( srcSize.x & 1 ) != 0 && dst.x == dstSize.x - 1 ) ? 1 : 0,
				  ( ( srcSize.y & 1 ) != 0 && dst.y == dstSize.y - 1 ) ? 1 : 0 );
}

// The tile's first texel and its end (exclusive) at level k on one axis.
ivec2 tileRange( int t, int fullTiles, int size0, int k )
{
	int sizeK = max( size0 >> k, 1 );
	int lo = ( t * 64 ) >> k;
	int hi = ( t == fullTiles - 1 ) ? sizeK : ( ( t + 1 ) * 64 ) >> k;
	return ivec2( lo, hi );
}

void main()
{
	// The pyramid's own size (mip 0 is the depth's size, 1:1) and its level
	// count: always the full chain to 1 x 1 (OgreView's hzbLevelsFor, and Ogre's
	// own maximum where only the id pass builds it).
	ivec2 size0 = imageSize( mip0 );
	int levels = 1;
	for( int m = max( size0.x, size0.y ); m > 1; m >>= 1 )
		++levels;
	uint lid = gl_LocalInvocationIndex;

	// ---- the tile ----------------------------------------------------------
	ivec2 fullTiles = max( size0 >> 6, ivec2( 1 ) );
	ivec2 tile = ivec2( gl_WorkGroupID.xy );
	bool owns = tile.x < fullTiles.x && tile.y < fullTiles.y;

	if( owns )
	{
		// MIP 0: the copy, over the tile's own level-0 region — written by level 1's
		// own reads below (every level-0 texel is read by exactly the level-1 texel whose
		// footprint holds it), so only a one-texel image copies on its own.
		if( levels == 1 )
		{
			ivec2 rx = tileRange( tile.x, fullTiles.x, size0.x, 0 );
			ivec2 ry = tileRange( tile.y, fullTiles.y, size0.y, 0 );
			ivec2 n = ivec2( rx.y - rx.x, ry.y - ry.x );
			for( int idx = int( lid ); idx < n.x * n.y; idx += HZB_THREADS )
			{
				ivec2 uv = ivec2( rx.x + idx % n.x, ry.x + idx / n.x );
				imageStore( mip0, uv, vec4( texelFetch( depthTexture, uv, 0 ).x, 0.0, 0.0, 0.0 ) );
			}
		}

		// LEVEL 1 from the depth itself — and mip 0's copy from the same reads.
		int tileTop = min( HZB_TILE_LEVELS, levels - 1 );
		ivec2 prevLo = ivec2( 0 );
		ivec2 prevN = ivec2( 0 );
		if( tileTop >= 1 )
		{
			ivec2 rx = tileRange( tile.x, fullTiles.x, size0.x, 1 );
			ivec2 ry = tileRange( tile.y, fullTiles.y, size0.y, 1 );
			ivec2 lo = ivec2( rx.x, ry.x );
			ivec2 n = ivec2( rx.y - rx.x, ry.y - ry.x );
			ivec2 dstSize = levelSize( size0, 1 );
			for( int idx = int( lid ); idx < n.x * n.y; idx += HZB_THREADS )
			{
				ivec2 local = ivec2( idx % n.x, idx / n.x );
				ivec2 dst = lo + local;
				ivec2 extra = extraOf( dst, dstSize, size0 );
				ivec2 base = dst * 2;
				float kept = HZB_SEED;
				for( int y = 0; y <= 1 + extra.y; ++y )
				{
					for( int x = 0; x <= 1 + extra.x; ++x )
					{
						ivec2 uv = min( base + ivec2( x, y ), size0 - ivec2( 1, 1 ) );
						float d = texelFetch( depthTexture, uv, 0 ).x;
						imageStore( mip0, uv, vec4( d, 0.0, 0.0, 0.0 ) );
						kept = HZB_PICK( kept, d );
					}
				}
				imageStore( mip1, dst, vec4( kept, 0.0, 0.0, 0.0 ) );
				sA[local.y * n.x + local.x] = kept;
			}
			prevLo = lo;
			prevN = n;
			memoryBarrierShared();
			barrier();
		}

		// LEVELS 2..6 from shared memory: odd levels live in A, even in B. The loop's
		// bound is a constant so the compiler unrolls it and every level's image and
		// buffer are known statically; the tile's own top ends it early.
		for( int k = 2; k <= HZB_TILE_LEVELS; ++k )
		{
			if( k > tileTop )
				break;
			ivec2 rx = tileRange( tile.x, fullTiles.x, size0.x, k );
			ivec2 ry = tileRange( tile.y, fullTiles.y, size0.y, k );
			ivec2 lo = ivec2( rx.x, ry.x );
			ivec2 n = ivec2( rx.y - rx.x, ry.y - ry.x );
			ivec2 dstSize = levelSize( size0, k );
			ivec2 srcSize = levelSize( size0, k - 1 );
			for( int idx = int( lid ); idx < n.x * n.y; idx += HZB_THREADS )
			{
				ivec2 local = ivec2( idx % n.x, idx / n.x );
				ivec2 dst = lo + local;
				ivec2 extra = extraOf( dst, dstSize, srcSize );
				ivec2 base = dst * 2;
				float kept = HZB_SEED;
				for( int y = 0; y <= 1 + extra.y; ++y )
				{
					for( int x = 0; x <= 1 + extra.x; ++x )
					{
						ivec2 s = min( base + ivec2( x, y ), srcSize - ivec2( 1, 1 ) ) - prevLo;
						kept = HZB_PICK( kept, ( ( k & 1 ) == 0 ) ? sA[s.y * prevN.x + s.x]
																	: sB[s.y * prevN.x + s.x] );
					}
				}
				storeLevel( k, dst, kept );
				if( ( k & 1 ) != 0 )
					sA[local.y * n.x + local.x] = kept;
				else
					sB[local.y * n.x + local.x] = kept;
			}
			prevLo = lo;
			prevN = n;
			memoryBarrierShared();
			barrier();
		}
	}

	// ---- the tail: levels 7.. by the last workgroup to arrive --------------
	if( levels - 1 <= HZB_TILE_LEVELS )
		return;

	// Every workgroup's level 6 is visible before its arrival is counted.
	memoryBarrierImage();
	barrier();
	if( lid == 0u )
	{
		uint groups = gl_NumWorkGroups.x * gl_NumWorkGroups.y;
		uint before = atomicAdd( arrivals[0], 1u );
		sIsLast = ( before == groups - 1u ) ? 1u : 0u;
		// The last arrival puts the counter back for the next dispatch.
		if( sIsLast != 0u )
			atomicExchange( arrivals[0], 0u );
	}
	memoryBarrierShared();
	barrier();
	if( sIsLast == 0u )
		return;
	memoryBarrierImage();

	for( int k = HZB_TILE_LEVELS + 1; k < levels; ++k )
	{
		ivec2 dstSize = levelSize( size0, k );
		ivec2 srcSize = levelSize( size0, k - 1 );
		for( int idx = int( lid ); idx < dstSize.x * dstSize.y; idx += HZB_THREADS )
		{
			ivec2 dst = ivec2( idx % dstSize.x, idx / dstSize.x );
			ivec2 extra = extraOf( dst, dstSize, srcSize );
			ivec2 base = dst * 2;
			float kept = HZB_SEED;
			for( int y = 0; y <= 1 + extra.y; ++y )
			{
				for( int x = 0; x <= 1 + extra.x; ++x )
				{
					ivec2 uv = min( base + ivec2( x, y ), srcSize - ivec2( 1, 1 ) );
					kept = HZB_PICK( kept, loadLevel( k - 1, uv ) );
				}
			}
			storeLevel( k, dst, kept );
		}
		memoryBarrierImage();
		barrier();
	}
}
