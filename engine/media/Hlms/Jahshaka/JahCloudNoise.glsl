// THE CLOUD FIELD'S NOISE (CLOUDS-2D-3): the one copy of the procedural,
// fixed-seed, TILING lattices the field is built from, included by the shape
// pass (JahCloudShape_ps.glsl) and the final bake (JahCloudBake_ps.glsl).
// Integer hashes on lattices whose periods divide the tile: the field tiles
// exactly and the same box bakes the same sky every run (the gradients pass
// through the GPU's sin and cos: another vendor may differ in the last bits).

uint jahHash( uint x, uint y, uint seed )
{
	uint h = ( x * 0x8da6b343u ) ^ ( y * 0xd8163841u ) ^ ( seed * 0xcb1ab31fu );
	h ^= h >> 13u;
	h *= 0x5bd1e995u;
	h ^= h >> 15u;
	return h;
}
float jahRand( uint h ) { return float( h & 0xFFFFFFu ) / 16777216.0; }
uint jahWrap( int i, int period ) { return uint( ( ( i % period ) + period ) % period ); }

// Gradient noise over [0,1) with `period` lattice cells, tiling; about -0.7..0.7.
float jahGradNoise( vec2 uv, int period, uint seed )
{
	const vec2 p = uv * float( period );
	const vec2 i = floor( p );
	const vec2 f = p - i;
	const int ix = int( i.x ), iy = int( i.y );
	float n[4];
	for( int k = 0; k < 4; ++k )
	{
		const int cx = ix + ( k & 1 ), cy = iy + ( k >> 1 );
		const float a = jahRand( jahHash( jahWrap( cx, period ), jahWrap( cy, period ), seed ) ) *
						6.28318531;
		n[k] = dot( vec2( cos( a ), sin( a ) ), f - vec2( float( k & 1 ), float( k >> 1 ) ) );
	}
	const vec2 s = f * f * f * ( f * ( f * 6.0 - 15.0 ) + 10.0 );
	return mix( mix( n[0], n[1], s.x ), mix( n[2], n[3], s.x ), s.y );
}

// Octaves at doubling periods, amplitudes times `gain`, normalised to the
// first octave's range.
float jahFbm( vec2 uv, int period, int octaves, float gain, uint seed )
{
	float sum = 0.0, amp = 1.0, norm = 0.0;
	for( int o = 0; o < octaves; ++o )
	{
		sum += amp * jahGradNoise( uv, period, seed + uint( o ) * 101u );
		norm += amp;
		amp *= gain;
		period *= 2;
	}
	return sum / norm;
}

// ONE CLOUD PER CELL (or none): a cone of height 1 at a random point of the
// cell, of a random radius at most half a cell -- so the 3x3 neighbourhood holds
// every cloud that can reach the texel. Returns the tallest cone here and, in
// `core`, that cloud's own thickness 0..1.
float jahCells( vec2 uv, int period, uint seed, float present, out float core )
{
	const vec2 p = uv * float( period );
	const vec2 c = floor( p );
	float best = 0.0;
	core = 0.0;
	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			const int qx = int( c.x ) + dx, qy = int( c.y ) + dy;
			const uint h = jahHash( jahWrap( qx, period ), jahWrap( qy, period ), seed );
			if( jahRand( jahHash( h, 11u, seed ) ) > present )
				continue;
			const vec2 at = vec2( float( qx ), float( qy ) ) +
							vec2( jahRand( h ), jahRand( jahHash( h, 0x9e3779b9u, seed ) ) );
			const float radius = mix( 0.2, 0.5, jahRand( jahHash( h, 7u, seed ) ) );
			const float cone = 1.0 - length( p - at ) / radius;
			if( cone > best )
			{
				best = cone;
				core = jahRand( jahHash( h, 13u, seed ) );
			}
		}
	}
	return best;
}

// The warped texel every lattice is read at.
vec2 jahWarp( vec2 uv )
{
	return uv + 0.035 * vec2( jahFbm( uv, 6, 3, 0.5, 0x51u ), jahFbm( uv, 6, 3, 0.5, 0x97u ) );
}

// THE OUTLINE'S POTENTIAL at a (warped) tile point; `core` = the owning
// cloud's thickness.
float jahPotential( vec2 w, out float core )
{
	float coreBig, coreSmall;
	const float big = jahCells( w, 12, 0x1234u, 0.85, coreBig );
	const float small = 0.9 * jahCells( w, 40, 0x5678u, 0.6, coreSmall );
	core = big >= small ? coreBig : coreSmall;
	const float outline = jahFbm( w, 24, 3, 0.5, 0x2468u );
	const float weather = jahFbm( w, 3, 2, 0.5, 0x1357u );
	return max( big, small ) + 0.35 * outline + 0.4 * weather;
}
