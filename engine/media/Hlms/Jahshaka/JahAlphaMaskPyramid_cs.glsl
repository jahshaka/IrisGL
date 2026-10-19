// Jahshaka - A CUT-OUT'S MASK PYRAMID (ATOM-3 stage 2; AlphaMasks.h). The mask's level 0 is the texture's alpha
// through the datablock's alpha test, one bit a texel (JahAlphaMask_cs); this job builds every coarser level from
// it, ONE WORKGROUP A LEVEL (level = the workgroup's index + 1), so the passes can sample the level their pixel's UV
// footprint asks for (jah_alpha_mask.glsl) and a minified cut-out keeps the coverage its level 0 has.
//
// THE LAW, per level: the fraction of set bits equals level 0's, within one texel of the level. Each parent texel's
// COVERAGE is the fraction of set level-0 texels under it (its footprint: the level-0 texels the parent's extent
// covers - an exact partition of level 0, x0 = x * w0 / wk, x1 = (x + 1) * w0 / wk, the same in y); the parents are
// walked in ROW-MAJOR order and each bit is the running total's rounding step,
//     bit_i = round(S_i) - round(S_(i-1)),   S_i = the coverage of parents 0..i,   round(s) = floor(s + 1/2),
// which is the running-error diffusion "set the bit when the carried error plus this coverage reaches one half"
// written in closed form - deterministic, never a random dither, and with |set bits - S| <= 1/2 over the whole
// level by construction. The closed form makes it parallel: a row's first bit needs only the total of the rows
// before it, so the workgroup sums each row, then prefix-sums the rows, then writes each row's bits.
//
// THE NUMBERS are fixed point, 20 fraction bits: a coverage is n * 2^20 / A (exact where the footprint's area A is
// a power of two, which is every level of a power-of-two mask; otherwise rounded at 2^-21 of a texel, at most an
// eighth of a texel over a whole 1024-square level). A row's total fits 32 bits (512 parents x 2^20); the running
// total is kept as (whole texels, fraction).
//
// THE LAYOUT, shared with AlphaMasks.h and jah_alpha_mask.glsl: level k is max(1, w0 >> k) x max(1, h0 >> k), its
// bits row-major from word offset sum over j < k of ceil(wj * hj / 32); the levels run until both sides are 1.
@insertpiece( SetCrossPlatformSettings )

layout( std430, ogre_U0 ) restrict buffer maskLayout { uint maskBits[]; };

vulkan( layout( ogre_P0 ) uniform Params { )
	// x level 0's width, y its height, z the level count, w unused
	uniform uvec4 alphaPyramidA;
vulkan( }; )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_PYR_THREADS @value( threads_per_group_x )u
#define JAH_FRAC_BITS 20u
#define JAH_FRAC_ONE ( 1u << JAH_FRAC_BITS )
#define JAH_FRAC_MASK ( JAH_FRAC_ONE - 1u )
#define JAH_FRAC_HALF ( 1u << ( JAH_FRAC_BITS - 1u ) )
// A level-1 mask of a 1024-square level 0 has 512 rows; nothing has more.
#define JAH_MAX_ROWS 512u

shared uint sRowTotal[JAH_MAX_ROWS];
shared uint sRowWhole[JAH_MAX_ROWS];
shared uint sRowFrac[JAH_MAX_ROWS];

uvec2 jahLevelSize( uint k )
{
	return uvec2( max( alphaPyramidA.x >> k, 1u ), max( alphaPyramidA.y >> k, 1u ) );
}

uint jahLevelOffset( uint k )
{
	uint o = 0u;
	for( uint j = 0u; j < k; ++j )
	{
		const uvec2 s = jahLevelSize( j );
		o += ( s.x * s.y + 31u ) / 32u;
	}
	return o;
}

/// The set bits of level 0 in bit range [a, b) (row-major bit indices; level 0 starts at word 0).
uint jahCountRange( uint a, uint b )
{
	uint n = 0u;
	while( a < b )
	{
		const uint word = a >> 5u, first = a & 31u;
		const uint span = min( 32u - first, b - a );
		const uint bits = maskBits[word] >> first;
		n += bitCount( span == 32u ? bits : ( bits & ( ( 1u << span ) - 1u ) ) );
		a += span;
	}
	return n;
}

/// Parent (x, y) of level k: its coverage in 2^-20 texels (its footprint's set fraction of level 0).
uint jahCoverage( uint x, uint y, uvec2 sk )
{
	const uint w0 = max( alphaPyramidA.x, 1u ), h0 = max( alphaPyramidA.y, 1u );
	const uint x0 = x * w0 / sk.x, x1 = ( x + 1u ) * w0 / sk.x;
	const uint y0 = y * h0 / sk.y, y1 = ( y + 1u ) * h0 / sk.y;
	uint n = 0u;
	for( uint yy = y0; yy < y1; ++yy )
		n += jahCountRange( yy * w0 + x0, yy * w0 + x1 );
	const uint area = ( x1 - x0 ) * ( y1 - y0 );
	if( area == 0u )
		return 0u;
	if( ( area & ( area - 1u ) ) == 0u )
		return n << ( JAH_FRAC_BITS - uint( findMSB( area ) ) );   // exact: area <= 2^20
	return min( uint( round( float( n ) * float( JAH_FRAC_ONE ) / float( area ) ) ), JAH_FRAC_ONE );
}

/// round(S) = floor(S + 1/2) of a running total held as (whole, fraction).
uint jahRounded( uint whole, uint frac )
{
	return whole + ( frac >= JAH_FRAC_HALF ? 1u : 0u );
}

void main()
{
	const uint k = gl_WorkGroupID.x + 1u;
	if( k >= alphaPyramidA.z )
		return;
	const uint tid = gl_LocalInvocationIndex;
	const uvec2 sk = jahLevelSize( k );
	const uint base = jahLevelOffset( k );
	const uint words = ( sk.x * sk.y + 31u ) / 32u;

	// (1) THE LEVEL'S WORDS CLEARED (the bits are OR-ed in below: a word may hold the end of one row and the start
	// of the next) and EACH ROW'S TOTAL COVERAGE.
	for( uint i = tid; i < words; i += JAH_PYR_THREADS )
		maskBits[base + i] = 0u;
	for( uint r = tid; r < sk.y; r += JAH_PYR_THREADS )
	{
		uint t = 0u;
		for( uint x = 0u; x < sk.x; ++x )
			t += jahCoverage( x, r, sk );
		sRowTotal[r] = t;
	}
	memoryBarrierShared();
	memoryBarrierBuffer();
	barrier();

	// (2) THE ROWS' PREFIX: the running total before each row, as (whole texels, fraction).
	if( tid == 0u )
	{
		uint whole = 0u, frac = 0u;
		for( uint r = 0u; r < sk.y; ++r )
		{
			sRowWhole[r] = whole;
			sRowFrac[r] = frac;
			frac += sRowTotal[r];
			whole += frac >> JAH_FRAC_BITS;
			frac &= JAH_FRAC_MASK;
		}
	}
	memoryBarrierShared();
	barrier();

	// (3) EACH ROW'S BITS: bit_i = round(S_i) - round(S_(i-1)).
	for( uint r = tid; r < sk.y; r += JAH_PYR_THREADS )
	{
		uint whole = sRowWhole[r], frac = sRowFrac[r];
		uint before = jahRounded( whole, frac );
		for( uint x = 0u; x < sk.x; ++x )
		{
			frac += jahCoverage( x, r, sk );
			whole += frac >> JAH_FRAC_BITS;
			frac &= JAH_FRAC_MASK;
			const uint now = jahRounded( whole, frac );
			if( now != before )
			{
				const uint i = r * sk.x + x;
				atomicOr( maskBits[base + ( i >> 5u )], 1u << ( i & 31u ) );
			}
			before = now;
		}
	}
}
