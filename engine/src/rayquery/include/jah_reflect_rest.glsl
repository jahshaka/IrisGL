// THE REFLECTION'S REST, PER TEXEL (PHOTON-2 round 2: R1 + G1-G6) - one text for the trace, the temporal pass, the
// filter and the break's compose pass.
//
// AT REST the moving history H stays FROZEN in the half the trace reads (`historyPrev`) and the other half
// (`historyCur`) holds the REST MEAN M of the texel's own rest samples, rgb = M, w = H.w + v (v = the rest samples
// it holds - a frame that gave the texel no sample adds none: G1). The answer is COMPOSED AT THE READ (G2):
//   O = lerp( H, M, w( v ) ),  w( v ) = 1 - ( 1 - v / 2N )^2 for v < 2N, then 1
// (N = the gather's settle of the 32-frame history, 51) - the hand-over drains the motion lag at fix F1's starting
// rate (w'( 0 ) = 1 / N) and EASES OUT over 2N samples: the answer's per-frame change, (M - H) w'( v ) + (s - M) w( v )
// / v, then FALLS from the first rest frame on (the linear v / N held its noise term at (s - M) / N, flat for 51 frames
// - measured on photon-glass: 0.055 -> 0.060 codes, rising under the filter's shrinking reach), and the held answer is
// M alone, the path drained. A texel with no moving history, or a fixed estimator, takes M from its first rest sample.
// M is a running mean (M + (s - M) / v), so its fp16 error does not accumulate (the in-place O + (s - H) / N of fix
// F1 held texels up to 1.4 % off: the fix round's read #6).
// After the hand-over M goes on accumulating PER TEXEL until its per-frame change has stayed under one output code
// for kReflectStopFrames frames - then the texel STOPS tracing (R1); the view holds when every texel has stopped or at
// kReflectRestMax rest frames. The stop's count rides the four low mantissa bits of the texel's distance
// (historyDist .x - a relative error of 2^-19 on a length only compared at 5 %).
#ifndef JAH_REFLECT_REST_GLSL
#define JAH_REFLECT_REST_GLSL

const float kReflectRestMax = 512.0;
const uint kReflectStopFrames = 8u;
/// "Under one output code": a change of the texel's luma within this share of its own (or its neighbourhood's, for a
/// dark texel) - one sRGB code is 1.1 % (code 200) to 3.7 % (code 50) of linear radiance, so 1 % is under a code at
/// every tone the eye sees.
const float kReflectStopRel = 0.01;

uint jahRestStop( float distX )
{
	return distX > 0.0 ? ( floatBitsToUint( distX ) & 15u ) : 0u;
}
float jahPackRestStop( float dist, uint n )
{
	if( !( dist > 0.0 ) )
		return dist;
	return uintBitsToFloat( ( floatBitsToUint( dist ) & ~15u ) | min( n, 15u ) );
}
/// The rest samples a texel holds: its M half's count over its frozen history's.
float jahRestSamples( vec4 H, vec4 M )
{
	return max( M.w - max( H.w, 0.0 ), 0.0 );
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
/// THE COMPOSED ANSWER (G2): the hand-over from the frozen history to the rest mean, the rest mean after it.
vec3 jahRestAnswer( vec4 H, vec4 M, float N )
{
	const float v = jahRestSamples( H, M );
	if( v <= 0.0 )
		return H.xyz;
	if( H.w > 0.0 && v < jahRestHandOverLength( N ) )
		return mix( H.xyz, M.xyz, jahRestHandOverWeight( v, N ) );
	return M.xyz;
}

/// THE SAMPLE'S FLAGS (reflectSample .w): the trace's blend floor (0 < f <= 1) + 2 surface history only + 4 a warm
/// restart + 8 a fixed estimator + 16 a texel that RESUMES at rest (G6: a moved instance crossed its ray, or its
/// estimator switched). w = 0: no sample this frame.
struct JahReflectFlags { float floorBlend; bool surfaceOnly; bool warmRestart; bool fixedEstimator; bool resume; };
JahReflectFlags jahReflectFlags( float w )
{
	JahReflectFlags f;
	f.resume = w > 15.5;
	w -= f.resume ? 16.0 : 0.0;
	f.fixedEstimator = w > 7.5;
	w -= f.fixedEstimator ? 8.0 : 0.0;
	f.warmRestart = w > 3.5;
	w -= f.warmRestart ? 4.0 : 0.0;
	f.surfaceOnly = w > 1.5;
	f.floorBlend = clamp( f.surfaceOnly ? w - 2.0 : w, 0.0, 1.0 );
	return f;
}

/// THE LOBE'S FOOTPRINT AT ITS HIT, IN TRACE TEXELS (G5: the one variable at the fixed cut) - the lobe's angular width
/// at the hit, ~4 alpha, over the throw t, against one trace texel's width at the reflected image (the camera's pixel
/// angle over (eye distance + t), perspective; the image plane's texel, orthographic). Under 1 the lobe lands inside
/// one texel: the centre ray is the integral (the fixed estimator). 0 = no throw known.
float jahReflectFootprint( float lobeAlpha, float throwT, float eyeDist, float texelAngle, bool perspective )
{
	if( !( throwT > 0.0 ) )
		return 0.0;
	const float texelAtImage = perspective ? texelAngle * ( eyeDist + throwT ) : texelAngle;
	return 4.0 * lobeAlpha * throwT / max( texelAtImage, 1e-9 );
}
/// The fixed decision with its HYSTERESIS BAND (G5): a texel at the cut cannot flip on its own frozen mean length.
const float kFixedEnter = 0.8, kFixedLeave = 1.25;

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
