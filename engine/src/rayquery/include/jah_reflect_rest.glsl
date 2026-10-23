// THE REFLECTION'S REST, PER TEXEL (PHOTON-2 round 2: R1 + G1-G6) - one text for the trace, the temporal pass, the
// filter and the break's compose pass.
//
// AT REST the moving history H stays FROZEN in the half the trace reads (`historyPrev`) and the other half
// (`historyCur`) holds the REST MEAN M of the texel's own rest samples (rgb = M); the counts are the state texel's
// (jah_reflect_state.glsl): H's moving count N, carried into the rest half, and v = the rest samples M holds - a frame
// that gave the texel no sample adds none (G1). The answer is COMPOSED AT THE READ (G2):
//   O = lerp( H, M, w( v ) ),  w( v ) = 1 - ( 1 - v / 2N )^2 for v < 2N, then 1
// (N = the gather's settle of the moving history's MaxFrames 12, 19: PHOTON-7 stage 6) - the hand-over drains the motion lag at fix F1's starting
// rate (w'( 0 ) = 1 / N) and EASES OUT over 2N samples: the answer's per-frame change, (M - H) w'( v ) + (s - M) w( v )
// / v, then FALLS from the first rest frame on (the linear v / N held its noise term at (s - M) / N, flat for 51 frames
// - measured on photon-glass: 0.055 -> 0.060 codes, rising under the filter's shrinking reach), and the held answer is
// M alone, the path drained. A texel with no moving history, or a fixed estimator, takes M from its first rest sample.
// M is a running mean (M + (s - M) / v), so its fp16 error does not accumulate (the in-place O + (s - H) / N of fix
// F1 held texels up to 1.4 % off: the fix round's read #6).
// After the hand-over M goes on accumulating PER TEXEL until ITS STANDARD ERROR - sqrt( (E[l^2] - E[l]^2) / v ), the
// luma's second moment kept beside M (the rest half's alpha: its root mean square, PHOTON-2 rev 12, P1) - has stayed
// under one output code for kReflectStopFrames frames - then the texel STOPS tracing (R1); the view holds when every
// texel has stopped or at kReflectRestMax rest frames. (The stop on the per-frame change could not see a heavy tail:
// one rare bright sample froze into a texel that stopped near 110 samples - round 2's (f) blotches.) The stop's count is
// the state texel's own field.
#ifndef JAH_REFLECT_REST_GLSL
#define JAH_REFLECT_REST_GLSL

const float kReflectRestMax = 512.0;
const uint kReflectStopFrames = 8u;
/// "Under one output code": a change of the texel's luma within this share of its own (or its neighbourhood's, for a
/// dark texel) - one sRGB code is 1.1 % (code 200) to 3.7 % (code 50) of linear radiance, so 1 % is under a code at
/// every tone the eye sees.
const float kReflectStopRel = 0.01;

/// The luma the rest's second moment is kept for (the temporal pass' own weights).
float jahRestLuma( vec3 c )
{
	return dot( c, vec3( 0.25, 0.5, 0.25 ) );
}
/// THE REST MEAN'S STANDARD ERROR in output codes (P1): the luma variance from the kept root mean square `rms` and the
/// mean's luma, over v samples, against one code of `reference` (kReflectStopRel of the texel's luma, or its
/// neighbourhood's for a dark texel). 0 for v < 1.
float jahRestErrorCodes( float rms, float meanLuma, float v, float reference )
{
	if( !( v >= 1.0 ) )
		return 0.0;
	const float variance = max( rms * rms - meanLuma * meanLuma, 0.0 );
	return sqrt( variance / v ) / max( kReflectStopRel * reference, 1e-8 );
}
/// THE FILTER'S REACH AT REST (P1): the texels a mean of standard error `e` codes must be averaged over for the
/// filtered answer to fall under HALF a code (n ~ 4 e^2 of them under the filter's soft weights: a reach of ~1.2 e
/// texels; a stop's e <= 1 keeps ~1 texel - the per-texel noise of a mean under one code shown raw measured 1.06 codes
/// mean against the reference with 0.6 e), never further than the moving reach - blended in with the hand-over's weight,
/// so the rest's first frame keeps the moving reach and a stopped texel's (v and its moment frozen) never moves.
const float kRestReachPerCode = 1.2;
float jahRestReach( float movingReach, float e, float handOverWeight )
{
	return mix( movingReach, min( movingReach, kRestReachPerCode * e ), clamp( handOverWeight, 0.0, 1.0 ) );
}
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
/// THE SECOND MOMENT'S STORE, STOCHASTICALLY ROUNDED TOO (PHOTON-2 1b P1; the rule: EVERY fp16 running statistic is): the
/// root mean square of the luma, rms' = sqrt( rms^2 + (l^2 - rms^2) / v ), makes nearly the same small step every frame
/// for a skewed texel - round to nearest rounds it the SAME way each time, the moment drifts, rms^2 - M^2 reads the
/// variance LOW and the standard-error stop fired at about half the samples (the round-4 read simulated it: 5 % rare
/// bright samples stopped at v 167 with a true standard error of 1.31 codes; stochastic rounding stops at 323 / 0.97, the
/// exact moment's 315 / 0.96). A fourth draw of the same hash (the texel, the eye, the rest frame).
float jahRestStoreMoment( float rms, ivec2 tc, int eye, float restFrame )
{
	uint s = jahRestHash( uint( tc.x ) * 73856093u ^ uint( tc.y ) * 19349663u ^ uint( eye ) * 83492791u ^
						  uint( restFrame ) * 2654435761u );
	s = jahRestHash( s + 3u );
	return jahHalfStochastic( rms, float( s >> 8u ) * ( 1.0 / 16777216.0 ) );
}
/// THE HAND-OVER'S LENGTH in rest samples (2N) and its eased weight.
float jahRestHandOverLength( float N )
{
	return 2.0 * max( N, 1.0 );
}
float jahRestHandOverWeight( float v, float N )
{
	const float x = 1.0 - min( v / jahRestHandOverLength( N ), 1.0 );
	return 1.0 - x * x;
}
/// THE COMPOSED ANSWER (G2): the hand-over from the frozen history H (`hasH`: the texel carried one) to the rest mean
/// M of v samples, the rest mean after it.
vec3 jahRestAnswer( vec3 H, bool hasH, vec3 M, float v, float N )
{
	if( v <= 0.0 )
		return H;
	if( hasH && v < jahRestHandOverLength( N ) )
		return mix( H, M, jahRestHandOverWeight( v, N ) );
	return M;
}

/// A SEGMENT THROUGH AN AXIS BOX (the slab test): origin o, unit direction d, length t.
bool jahSegmentHitsBox( vec3 o, vec3 d, float t, vec3 lo, vec3 hi )
{
	const vec3 inv = 1.0 / mix( d, vec3( 1e-12 ), lessThan( abs( d ), vec3( 1e-12 ) ) );
	const vec3 a = ( lo - o ) * inv, b = ( hi - o ) * inv;
	const vec3 tn = min( a, b ), tf = max( a, b );
	const float t0 = max( max( tn.x, tn.y ), max( tn.z, 0.0 ) );
	const float t1 = min( min( tf.x, tf.y ), min( tf.z, t ) );
	return t0 <= t1;
}

#endif
