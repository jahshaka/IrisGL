// THE REFLECTION'S REST, PER TEXEL (PHOTON-2 round 2: R1 + G1-G6; PHOTON-7 stage 6 commit 8, PREFLIGHT_R section 3) -
// one text for the trace, the temporal pass, the filter and the break's compose pass.
//
// AT REST the moving history H stays FROZEN in the half the trace reads (`historyPrev`) and the other half
// (`historyCur`) holds the REST MEAN M of the texel's own rest samples (rgb = M, w = its second moment); the counts are
// the state texel's (jah_reflect_state.glsl): H's moving count N, carried into the rest half, and v = the rest samples
// M holds - a frame that gave the texel no sample adds none (G1). The answer is COMPOSED AT THE READ (G2):
//   O = lerp( H, M, w( v ) ),  w( v ) = 1 - ( 1 - v / 2N )^2 for v < 2N, then 1
// (N = the gather's settle of the moving history's MaxFrames 12, 19) - the hand-over drains the motion lag at fix F1's
// starting rate (w'( 0 ) = 1 / N) and EASES OUT over 2N samples; the held answer is M alone. A texel with no moving
// history, or a fixed estimator, takes M from its first rest sample.
// M IS A MEAN IN THE DENOISER SPACE (commit 8: the moving mean is one - Lumen's D( c ) = c / ( 1 + E L / 10 ) - so a
// linear rest mean stepped by the Jensen gap when the hand-over ended), stored as its linear image D^-1( M ) like the
// moving history; its w the second moment of the samples' denoiser-space luma (the moving history's alpha, the same
// quantity). THE STORES ARE ERROR-FED (jahRestFold): what a 16-bit store loses of the running mean and of the moment is
// carried into the next fold, so neither drifts however many samples it takes (round to nearest drops an increment
// under half a unit in the last place, and drops it UNEVENLY: 0.71 codes dark at 512 samples, P1) - the residual lives in
// the FROZEN HALF once the hand-over is done (no reader composes H past 2N: zero bytes, PREFLIGHT_R section 9 W5), and
// a texel with no moving history keeps it there from its first sample.
// THE STOP (the lead's levers (i) + (ii), and the share rule after the SE floor's diagnostic): M goes on accumulating
// PER TEXEL until ITS OWN rest mean's standard error, IN DISPLAY CODES (the exact slope of the view's exposure, grade,
// film curve and sRGB encode at the pixel's value: jahDisplayCodes), is within ITS SHARE of the one-code display budget
// for kReflectStopFrames frames - SE_t <= 1 code x W / sqrt( Q ) at its own pixel, W = 1 + sum w_i the spatial filter's
// rest taps' weights (the centre at 1), Q = 1 + sum w_i^2 over the taps whose answer has a variance (a centre ray's has
// none and needs no share): sqrt( n_eff ), the filter's effective tap count there. When every tap meets its own share
// the displayed variance sum w_i^2 SE_i^2 / W^2 <= 1 code^2 (exact where the neighbours' n_eff and display slope match
// the pixel's, as on a smooth surface), and NO TEXEL WAITS ON ANOTHER: the rule (ii) it replaces stopped a texel on its
// pixel's combined SE, and a texel whose stopped neighbours kept a 4-7 code SE (their own share met against wide
// filters) never could - measured on photon-glass, its own SE 0.09 codes at v 35,901 under a displayed 1.18.
// Then the texel STOPS tracing (R1), and the view holds when every texel has stopped. There
// is NO SAMPLE CAP (K_max 512 deleted): a texel's count saturates only at its 16-bit field, a CAPPED texel - stopped,
// counted (the hit list's word 3, RayQueryStatus::reflectRestCapped) as the defect reading it is, never a deadline (the
// denoiser bound's guarantee, sigma^2 <= mu (10 - mu), needs 10^4 (10 - mu) / mu samples: 42 s to 38 min at four a
// frame - PREFLIGHT_R section 3).
#ifndef JAH_REFLECT_REST_GLSL
#define JAH_REFLECT_REST_GLSL

const uint kReflectStopFrames = 8u;
/// The state's v field's ceiling (16 bits, jah_reflect_state.glsl): a texel that reaches it is CAPPED.
const uint kReflectRestVCap = 65535u;
/// THE REST'S SAMPLE SEQUENCE (commit 8): a rest sample's index is the rest sequence's base (knobs.z at rest) plus the
/// sample's own number v - per SAMPLE, never per frame, so the k samples of a frame's batch and a dropped record's retry
/// are each the sequence's next, and a held answer is a function of the scene and the eye.
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

#ifdef JAH_REFLECT_DENOISE_GLSL
/// THE REST'S RUNNING STATISTICS (commit 8; the includer has jah_reflect_denoise.glsl and its exposure first): the
/// stored mean `M` (linear, the image's) and the moment `m2` (the denoiser-space luma's, the image's), and `res` - what
/// their stores lost (the denoiser-space mean's rgb, the moment's w), carried into the next fold.
struct JahRestStats { vec3 M; float m2; vec4 res; };
/// What a 16-bit store keeps of x.
float jahHalf1( float x ) { return unpackHalf2x16( packHalf2x16( vec2( x, 0.0 ) ) ).x; }
vec3 jahHalf3( vec3 x ) { return vec3( jahHalf1( x.x ), jahHalf1( x.y ), jahHalf1( x.z ) ); }
/// Whether the frozen half holds the residual: once no reader composes H - past the hand-over, or with no H - for EVERY
/// texel, by its count alone (a texel's draw can turn from the fixed estimator to the lobe at rest, its throw settling:
/// the residual's validity may never depend on which it is). Before it, a fixed estimator keeps its plain mean in H
/// (every reader then composes M) and a lobe keeps H itself.
bool jahRestHasResidual( float v, bool hasH, float N )
{
	return v >= 1.0 && ( !hasH || v >= jahRestHandOverLength( N ) );
}
/// ONE SAMPLE INTO THE REST MEAN: the running mean and moment over vPrev + 1 samples in the denoiser space - exactly
/// from what the stores hold plus what they lost - and the stores' new values and residual.
void jahRestFold( inout JahRestStats s, vec3 sampleLin, float vPrev )
{
	const float v = vPrev + 1.0;
	const vec3 sD = jahToDenoise( clamp( sampleLin, vec3( 0.0 ), vec3( kMaxRadiance ) ) );
	const float sL = jahDenoiseLuma( sD );
	vec3 MD = sD;
	float q = sL * sL;
	if( vPrev > 0.0 )
	{
		MD = jahToDenoise( s.M ) + s.res.xyz;
		q = s.m2 + s.res.w;
		MD += ( sD - MD ) / v;
		q += ( sL * sL - q ) / v;
	}
	MD = max( MD, vec3( 0.0 ) );
	s.M = jahHalf3( clamp( jahFromDenoise( MD ), vec3( 0.0 ), vec3( kMaxRadiance ) ) );
	s.m2 = jahHalf1( max( q, 0.0 ) );
	s.res = vec4( MD - jahToDenoise( s.M ), q - s.m2 );
}
/// THE REST MEAN'S STANDARD ERROR over v samples, in the DENOISER space's luma (the spatial filter's sigma at rest) and
/// carried to LINEAR luma by the space's slope at the mean (dL / dL_D = 1 / ( 1 - E L_D / 10 )^2). 0 for v < 1.
float jahRestErrorD( JahRestStats s, float v )
{
	if( !( v >= 1.0 ) )
		return 0.0;
	const float LD = jahDenoiseLuma( jahToDenoise( s.M ) + s.res.xyz );
	return sqrt( max( s.m2 + s.res.w - LD * LD, 0.0 ) / v );
}
float jahDenoiseSlopeAt( float LD )
{
	const float k = 1.0 - LD * ( JAH_DENOISE_EXPOSURE / kDenoiserTonemapRange );
	return 1.0 / max( k * k, 1e-4 );
}
#ifdef JAH_DISPLAY_LUT
/// THE DISPLAY'S CODE (lever (i)): the host's table (ReflectParams::display, OgreRayQuery.cpp fillDisplaySlope) of
/// d(code) / d(ln x) over the pre-exposed luma x = E L, ln 1e-4 .. ln 100 in 63 steps; below its foot the slope's own
/// d(code) / dx there (the encode's linear segment). A linear luma L's standard error seL in DISPLAY CODES.
const float kDisplayLnMin = -9.210340372;
const float kDisplayLnStep = 0.2192938184;
float jahDisplayLogSlope( float lnx )
{
	const float t = clamp( ( lnx - kDisplayLnMin ) / kDisplayLnStep, 0.0, 63.0 );
	const int i = min( int( t ), 62 );
	return mix( JAH_DISPLAY_LUT( i ), JAH_DISPLAY_LUT( i + 1 ), t - float( i ) );
}
float jahDisplayCodes( float L, float seL )
{
	const float x = JAH_DENOISE_EXPOSURE * max( L, 0.0 );
	const float xMin = exp( kDisplayLnMin );
	// d code / dx = (d code / d ln x) / x, at the table's foot for a value under it
	const float perX = x > xMin ? jahDisplayLogSlope( log( x ) ) / x : JAH_DISPLAY_LUT( 0 ) / xMin;
	return perX * JAH_DENOISE_EXPOSURE * max( seL, 0.0 );
}
#endif   // JAH_DISPLAY_LUT
#endif   // JAH_REFLECT_DENOISE_GLSL

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
