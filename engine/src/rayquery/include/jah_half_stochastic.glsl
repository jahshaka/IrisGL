// THE fp16 RUNNING STATISTIC'S STORE, ROUNDED STOCHASTICALLY (PHOTON_2_SPEC R1: EVERY fp16 running statistic is) - one
// text for the gather's diffuse and rough specular rest means (rq_probe_integrate.comp) and the area visibility's sums
// and rest means (jah_area_vis_law.glsl, both producers: the ray tier through glslang, the no-ray route's
// Jahshaka/ScreenAreaVis as the piece JahHalfStochastic). (The reflection's rest keeps its own statistics since
// PHOTON-7 stage 6: jah_reflect_rest.glsl's fold and its compensated sum after the hand-over.)
#ifndef JAH_HALF_STOCHASTIC_GLSL
#define JAH_HALF_STOCHASTIC_GLSL

/// THE REST MEAN'S STORE, ROUNDED STOCHASTICALLY TO HALF PRECISION (P1): the running mean M + (s - M) / v lives in an
/// rgba16f half, and from a few hundred samples on the increment falls under half a unit in the last place - round to
/// nearest then drops it, and drops it UNEVENLY (a reflection whose samples are mostly a little above the mean and rarely
/// far below loses only the small ones): measured on photon-glass, every texel running to 512 samples held 0.71 codes
/// dark of the 4,096-sample reference over 24 % of the glossy pixels. Rounding up with the probability of the remainder
/// keeps the stored mean unbiased (its noise: a random walk of ~0.3 ulp a step - 7 ulp, 0.35 %, at 512). The draw is a
/// hash of the texel and the rest frame: the held answer stays a function of the scene and the eye.
uint jahRestHash( uint x )
{
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}
float jahHalfStochastic( float x, float u )
{
	if( !( x > 0.0 ) )
		return x;
	const uint bits = packHalf2x16( vec2( x, 0.0 ) ) & 0xFFFFu;
	const float h = unpackHalf2x16( bits ).x;
	if( x > h && bits < 0x7BFFu )
	{
		const float up = unpackHalf2x16( bits + 1u ).x;
		return u < ( x - h ) / ( up - h ) ? up : h;
	}
	if( x < h && bits > 0u )
	{
		const float down = unpackHalf2x16( bits - 1u ).x;
		return u < ( h - x ) / ( h - down ) ? down : h;
	}
	return h;
}
vec3 jahRestStoreMean( vec3 m, ivec2 tc, int eye, float restFrame )
{
	uint s = jahRestHash( uint( tc.x ) * 73856093u ^ uint( tc.y ) * 19349663u ^ uint( eye ) * 83492791u ^
						  uint( restFrame ) * 2654435761u );
	vec3 r;
	for( int c = 0; c < 3; ++c )
	{
		s = jahRestHash( s + uint( c ) );
		r[c] = jahHalfStochastic( m[c], float( s >> 8u ) * ( 1.0 / 16777216.0 ) );
	}
	return r;
}

#endif
