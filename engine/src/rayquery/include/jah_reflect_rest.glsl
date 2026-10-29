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
// PER TEXEL until ITS OWN rest mean's standard error (the denoiser space's, the space the filter combines its taps in)
// is within ITS SHARE for kReflectStopFrames frames. A pixel p's displayed SE is sqrt( sum w_i^2 SE_i^2 ) / W_p carried
// to display codes by its slope (the denoiser space's to linear luma, then jahDisplayCodes: the exact slope of the
// view's exposure, grade, film curve and sRGB encode at its value), over the filter's fixed rest taps (W_p = the
// centre's 1 + sum w_i; Q_p = sum of the squared weights of those whose answer has a variance - a centre ray's has
// none): it is under ONE CODE when every such tap's SE is under W_p / sqrt( Q_p ) codes - sqrt( n_eff ), >= 1 as
// ( sum w )^2 >= sum w^2. A texel feeds several pixels, so its share is the MIN over the pixels that read it (the
// filter's atomic min, binding 51): then EVERY pixel's displayed SE is under one code whatever its neighbours' n_eff
// and slopes, and NO TEXEL WAITS ON ANOTHER (the rule (ii) it replaces stopped a texel on its pixel's combined SE, and
// one whose stopped neighbours kept a 4-7 code SE never could: measured on photon-glass, its own SE 0.09 codes at
// v 35,901 under a displayed 1.18).
// Then the texel STOPS tracing (R1), and the view holds when every texel has stopped. There
// is NO SAMPLE CAP (K_max 512 deleted): a texel's count saturates only at its 16-bit field, a CAPPED texel - stopped,
// counted (the hit list's word 3, RayQueryStatus::reflectRestCapped) as the defect reading it is, never a deadline (the
// denoiser bound's guarantee, sigma^2 <= mu (10 - mu), needs 10^4 (10 - mu) / mu samples: 42 s to 38 min at four a
// frame - PREFLIGHT_R section 3).
#ifndef JAH_REFLECT_REST_GLSL
#define JAH_REFLECT_REST_GLSL

const uint kReflectStopFrames = 8u;
/// THE STOP's SAMPLES (PHOTON-7 8B, B2's (e) attribution - a LAW CONSTANT): a texel stops only when its standard error
/// has stayed within its share across a window of at least kReflectStopSamples SAMPLES as well as kReflectStopFrames
/// frames. A stop's confidence is the samples', not the frames': counted in frames, the same lobe stopped at v = 68 at 4
/// samples a frame (32 in its window) and ran on to v = 1,296 at 64 a frame, where one bright sample in ~100 lifted its
/// mean 34 % - the held pictures 9 codes apart (rtreflect.lamp_step_held (e)). S = ln( 1 / delta ) / p, the samples that
/// see an event of probability p with confidence 1 - delta: delta 0.05, p 0.01 (the measured tail) -> S = 300.
/// THE RESIDUAL, named: a tail event of probability p is missed by a fraction delta of such texels - 5 % at p = 0.01,
/// more for rarer paths - which hold a biased mean until a region resumes them: a limit of any standard-error stop.
const float kReflectStopSamples = 300.0;
/// THE STOP's DRIFT TERM (B2, the gather's rest predicate's form): the window's rest mean at its start against now -
/// apart by more than K_DRIFT times their difference's own standard error (sqrt( e_start^2 - e_now^2 ): the later mean
/// holds the earlier's samples) is a mean still MOVING (a light still rising, a tail still being found), which the plain
/// error test reads as a small variance: the window restarts there. K 3: a stationary mean trips it 0.3 % a frame.
const float kReflectDriftK = 1.0e30;   // MEASURING ARM (8B B2, to be reverted): the drift term off - (A) alone
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
/// THE NO-POP FOLLOW OF A CACHE REFINEMENT (PHOTON-7 8B, B2 - the gather's B3 in the reflection's form; the set's binding
/// 62, OgreRayQuery.cpp kReflectRefineBinding: an rgba32ui a trace texel and eye - x, y.lo the SHOWN answer D in halves,
/// y.hi the stop window's starting mean (its denoiser-space luma), z the window's start v (lo 16) and its starting
/// standard error (a half, hi 16), w
/// the word). A texel reached at rest by an ESTIMATE CONVERGING alone (rq_reflect.comp restHitKind 1: a card's capture,
/// relight or card-to-card round, a scroll's slab, the store's bricks relighting on a close that lists no scene change -
/// the caches refined what its hit reads, the world did not change), stopped or still converging, RE-RESTS AT A CODE A
/// FRAME (a scene change - a mover, an edit, a lamp, a lamp's shadow range - resumes it at once): its
/// rest restarts as a resume's, and every reader SHOWS D, which the temporal pass SLEWS each frame toward the texel's
/// COMPOSED answer T (the accumulated estimate, never a frame's raw sample: D does not random-walk on the estimator's
/// noise) by at most ONE DISPLAY CODE per channel - the code distance integrated along the step as the delta is, the
/// same curve at every brightness. The picture follows the re-convergence and never steps by more than a code. (The
/// first build HELD the old answer until the new rest stopped and then walked: a glossy texel's ~500-frame re-convergence
/// frozen - the instrument's read, 2026-10-29.) The texel counts active until D has reached T on a stopped texel, when
/// the word clears. A real change (a mover, a light, a slab, an edit, a brick) resumes it at once from what it shows.
/// The word: kRefineReach = the trace found a card-only reach THIS frame (the temporal pass consumes it), kRefineRefining
/// = D is shown.
/// kRefineWindow = the stop window's start (y.hi, z) is held.
const uint kRefineReach = 1u, kRefineRefining = 2u, kRefineWindow = 4u;
vec3 jahRefineD( uvec4 r ) { return vec3( unpackHalf2x16( r.x ), unpackHalf2x16( r.y ).x ); }
uvec4 jahRefineWithD( uvec4 r, vec3 D )
{
	r.x = packHalf2x16( D.xy );
	r.y = ( r.y & 0xFFFF0000u ) | ( packHalf2x16( vec2( D.z, 0.0 ) ) & 0xFFFFu );
	return r;
}
/// What a reader SHOWS of a texel's composed answer under its refinement word `r`.
vec3 jahRefineShown( vec3 answer, uvec4 r )
{
	return ( r.w & kRefineRefining ) != 0u ? jahRefineD( r ) : answer;
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
/// WHAT A 16-BIT STORE KEEPS OF x, built from the float's bits: the nearest fp16 value (ties to even), as an fp32 the
/// rgba16f store holds exactly - so the store rounds nothing whatever its own rounding, and the residual below carries
/// the whole error. MEASURED (PHOTON-7 stage 6, the reference diagnostic): unpackHalf2x16( packHalf2x16( x ) ) did not
/// reproduce what the store kept - the residual read back ~0 (max 2^-24) and the rest mean drifted DOWN against a
/// 32-bit mean of the same samples, -4.4 / -13.5 / -27.3 % at v 300 / 1,200 / 4,096. x finite, |x| under 65,520 (the
/// callers clamp to kMaxRadiance).
float jahHalf1( float x )
{
	const float a = abs( x );
	if( a < 6.103515625e-05 )   // under fp16's smallest normal (2^-14): its subnormal grid, 2^-24 apart
		return sign( x ) * roundEven( a * 16777216.0 ) * 5.9604644775390625e-08;
	const uint u = floatBitsToUint( a );
	return sign( x ) * uintBitsToFloat( ( u + 0x00000FFFu + ( ( u >> 13u ) & 1u ) ) & 0xFFFFE000u );
}
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
	// (the residual is itself stored in fp16: rounded the same way, so its own store is exact too - its rounding is
	// unbiased, the next order, and not carried)
	s.res = vec4( jahHalf3( MD - jahToDenoise( s.M ) ), jahHalf1( q - s.m2 ) );
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
