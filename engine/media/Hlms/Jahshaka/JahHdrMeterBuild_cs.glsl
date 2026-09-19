// Jahshaka — THE HDR METER, part 1: a weighted log-luminance HISTOGRAM of the
// frame (EXPOSURE-2). This replaces the pin's five-quad 64/16/4/1 reduction
// ladder, which was a MEAN OF LOGS over a sparse grid.
//
// WHY A HISTOGRAM AND NOT A MEAN. A mean has no resistance: every sample moves
// it, in proportion to how extreme it is. Three consequences, all of them the
// owner's or the render audit's words:
//
//   * ONE UNUSABLE SAMPLE POISONED THE FRAME. The scene target is RGBA16F and a
//     punctual specular lobe goes as 1/(pi*alpha^2), so a near-mirror surface
//     stores +Inf; log() of that, or of a filtered fetch that came back below
//     zero, is a NaN, and a NaN in a mean is the whole frame's answer (lane
//     HDR-1, 2026-09-15; ogre-patches 0034 and 0042 bounded it by CLAMPING the
//     sample, because a mean cannot drop one). A HISTOGRAM CAN DROP IT: an
//     unusable sample is simply NOT BINNED. It does not vote. Nothing else in
//     the frame moves because of it, and no clamp constant has to be invented.
//
//   * A BRIGHT SKY OR A BLOWN WINDOW DECIDED THE EXPOSURE OF THE ROOM. A mean
//     has no notion of "most of the picture"; a histogram does, and the
//     PERCENTILE CLIPS in the resolve pass (JahHdrMeterResolve_cs) are exactly
//     that notion: throw away the darkest and brightest slices of the metered
//     weight and average what is left.
//
//   * A WHITE FLOOR FILLING THE FRAME TOOK EVERYTHING ELSE DOWN. That one is
//     NOT a percentile problem (a surface covering 60-90 % of the frame is
//     inside any 5-95 band) and it is not this pass's to fix either - what
//     bounds it is the METERING PATTERN below, and, for an authoring tool, the
//     fact that Manual exposure is the editor default (EXPOSURE-1).
//
// THE SAMPLING. One texelFetch per thread on a grid of one sample per 4x4
// pixels of the HDR target - 129,600 binned samples at 1920x1080 (a 480x272
// invocation grid; the two rows past 1080 return at the bounds test), twice the ladder's
// 65,536 (64x64 output texels x 16 taps). Unfiltered, deliberately: the
// ladder's samples were BILINEAR, and a bilinear fetch that weights a +Inf
// texel by zero is where HDR-1's `0 * Inf` NaNs came from. A texelFetch cannot
// manufacture one; the only unusable values left are the ones the shading
// really produced.
//
// THE METERING PATTERN is a per-pixel WEIGHT applied as the sample is binned
// (Scene::exposureMetering; PostFxDesc::meterPattern). The geometry is a circle
// IN PIXELS: q is the offset from the frame centre in units of half the frame
// HEIGHT, corrected for aspect, so q = 1 is the top or bottom edge and a spot
// is round on any window shape.
//
//   average         w = 1 everywhere. The whole frame, equally - the pin's
//                   behaviour, and the reference the other two are judged
//                   against.
//   centreWeighted  w = P + (1-P) * exp( -K q^2 ), the classic camera default.
//                   K is set from "half weight at half the frame height from
//                   the centre" (K = ln 2 / 0.25 = 2.7726) and P = 0.05 is a
//                   PEDESTAL so the corners still count for something: a meter
//                   that ignores the edges of the frame outright cannot see a
//                   window opening behind the subject. Integrated over the
//                   16:9 rectangle that puts 41 % of the sensitivity in the
//                   central 11 % of the picture and 83 % inside the inscribed
//                   full-height circle (44 % of the picture); the infinite
//                   plane's Gaussian would say 40/81, but a picture has corners.
//   spot            a disc whose HALF-WEIGHT AREA IS EXACTLY the authored
//                   fraction of the frame (2.5 %), with a +/-15 % feather in
//                   radius so the boundary is not a step. The radius is derived
//                   here from the area and the aspect, so the same 2.5 % holds
//                   on any window: rho = sqrt( f * 4 * aspect / pi ), which on
//                   16:9 is 0.238 - a disc 23.8 % of the frame height across.
//
// THE FIXED POINT. imageAtomicAdd is integer, so both counters are scaled:
//   row 0  the metered WEIGHT, as round( w * 256 ).
//   row 1  the weighted log-luminance OFFSET WITHIN THE BIN, as
//          round( w * (L - binMin) * 4096 ).
// Storing the offset inside the bin rather than the absolute log is what makes
// the answer independent of the bin width: the resolve reconstructs each bin's
// true weighted mean, so 128 bins over 30 stops quantise NOTHING except the two
// bins the percentile window cuts through. (The absolute form overflows: a
// single bin holding every sample would need 33 bits at 1080p.) Rounding is
// to nearest, so the per-sample error is +/-1/8192 nats and averages to zero
// instead of biasing the grade.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

@property( syntax == glsl )
	#define ogre_t0 binding = 0
	#define ogre_u0 binding = 0
@end

vulkan_layout( ogre_t0 ) uniform texture2D hdrTexture;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict uimage2D histogram;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 meterParams;
	uniform vec4 meterEyes;
vulkan( }; )

#define p_pattern       meterParams.x
#define p_gaussK        meterParams.y
#define p_spotArea      meterParams.z
#define p_pedestal      meterParams.w
// HOW MANY EYES ARE IN THIS TARGET (lane EYE-GRADE-1): 1 for every ordinary
// view, 2 for the VR session's pair, which renders the two eyes side by side
// into ONE texture. See jahMeterWeight.
#define p_eyes          meterEyes.x

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// THE AXIS. The chain measures ln( 1024 * Y ) - 1024 is the pin's own scale
// (HdrUtils::setExposure builds 1024 * e^(E-2) against it) - so the histogram
// spans luminance 2^-14 .. 2^16, i.e. ln(1024*2^-14) .. ln(1024*2^16). Thirty
// stops: the whole range an RGBA16F target can say anything about (the largest
// half float is 65504, ln(65504*1024) = 18.0) against the darkest value the
// floor below admits.
const float c_histMinLog  = -2.7725887;     // ln( 1024 ) - 14 * ln 2
const float c_histSpanLog = 20.7944154;     // 30 * ln 2
// THE FLOOR, applied BEFORE the log (CLAUDE.md, HDR-1): the meter is a mean of
// logs and log(0) is -Inf, so a black texel needs a floor or it is not a
// measurement. 1e-4 is the pin's ladder's constant, but applied as a FLOOR
// (max) where the ladder ADDED it before the log: the HDR-1 rule. The two
// differ by 1e-4/Y nats - 0.005 at Y = 0.02, 0.095 (0.14 stops) at Y = 0.001 -
// so an Auto scene regrades by that much against the ladder, more in the dark.
const float c_minLuminance = 0.0001;
// Luminance coefficients for LINEAR RGB - the same vector the pin's ladder used
// (from the DX SDK docs), kept so the two meters measure the same quantity.
const vec3  c_luminanceCoeffs = vec3( 0.2125, 0.7154, 0.0721 );
const float c_weightScale = 256.0;
const float c_logScale    = 4096.0;
const float c_spotFeather = 0.15;

// THE PATTERN IS PER EYE, AND THE MEASUREMENT IS ONE (lane EYE-GRADE-1).
//
// A stereo target carries the two eyes side by side, so its geometric centre is
// the pair's INNER EDGE and is nowhere in either picture: a centre-weighted
// meter would have weighted each eye's nasal edge and a spot would have metered
// the wearer's nose. `uv` is therefore mapped into the EYE's own frame before
// the pattern is evaluated (and the aspect is the eye's, not the pair's), so
// the same circle-in-pixels rule holds per eye.
//
// BOTH EYES STILL BIN INTO ONE HISTOGRAM, deliberately: the two pictures differ
// by an interpupillary distance, and a per-eye exposure — two chains converging
// separately — is binocular rivalry in the one dimension the visual system is
// least forgiving about. One measurement, two patterns.
float jahMeterWeight( vec2 uv, float aspect, float eyes )
{
	const float n = max( eyes, 1.0 );
	// The eye-local u: 0..1 inside whichever half (or whole) this texel is in,
	// and the EYE's own aspect, which is the target's divided by the eye count.
	const vec2 eyeUv = vec2( fract( uv.x * n ), uv.y );
	const float a = aspect / n;
	// q: the offset from the centre in units of half the frame HEIGHT, so the
	// pattern is a circle in pixels on any window shape.
	const vec2 q = ( eyeUv - 0.5 ) * 2.0 * vec2( a, 1.0 );
	const float d = length( q );

	if( p_pattern < 0.5 )
		return 1.0;										// average
	if( p_pattern < 1.5 )
		return p_pedestal + ( 1.0 - p_pedestal ) * exp( -p_gaussK * d * d );	// centreWeighted

	// spot: the radius that makes the disc's area exactly p_spotArea of the
	// frame. Frame area in q units is 2*a x 2 = 4*a; pi*rho^2 = f*4*a.
	const float rho = sqrt( max( p_spotArea, 0.0 ) * 4.0 * a * 0.31830989 );
	return 1.0 - smoothstep( rho * ( 1.0 - c_spotFeather ), rho * ( 1.0 + c_spotFeather ), d );
}

void main()
{
	const ivec2 texSize = textureSize( hdrTexture, 0 );
	// ONE SAMPLE PER 4x4 PIXELS. The job's `thread_groups_based_on_texture`
	// divisor is 4, and Ogre reads that as "one INVOCATION per 4 pixels on each
	// axis" before it divides by the 8x8 threads per group
	// (HlmsComputeJob::_calculateNumThreadGroupsBasedOnSetting) — so the grid is
	// ceil( size / 4 ) invocations and the last group on each axis runs off the
	// edge, which the bounds test below is for. (Measured while building this:
	// with the divisor read as "pixels per GROUP" the meter silently binned 256
	// samples instead of 16,384 and still looked plausible. The probe that found
	// it is why the total weight is worth being able to read back.)
	const ivec2 px = ivec2( gl_GlobalInvocationID.xy ) * 4 + ivec2( 2, 2 );
	if( px.x >= texSize.x || px.y >= texSize.y )
		return;

	const vec3 rgb = texelFetch( hdrTexture, px, 0 ).xyz;
	float lum = dot( rgb, c_luminanceCoeffs );

	// AN UNUSABLE SAMPLE IS NOT BINNED - it does not vote, and nothing else in
	// the frame moves because of it. That is the whole reason this is a
	// histogram (see the header).
	//
	// THE TEST IS ON THE BITS, and that is not a style choice: measured on this
	// stack (NVIDIA 595.84, Vulkan/SPIR-V, lane HDR-1) `x == x` is FOLDED TO
	// TRUE by the shader compiler and never fires. A float is a NaN or an Inf
	// exactly when the magnitude of its bit pattern reaches +Inf's, which is
	// integer arithmetic no optimiser may remove.
	const uint mag = floatBitsToUint( lum ) & 0x7FFFFFFFu;
	if( mag >= 0x7F800000u )
		return;											// NaN or +/-Inf
	// A NEGATIVE luminance is not a luminance either. It happens: the scene
	// target is a SUM of shading terms. Zero is a measurement (black) and takes
	// the floor; below zero is not.
	if( lum < 0.0 )
		return;

	lum = max( lum, c_minLuminance );
	const float L = log( lum * 1024.0 );

	const vec2 uv = ( vec2( px ) + 0.5 ) / vec2( texSize );
	const float aspect = float( texSize.x ) / float( texSize.y );
	const float w = clamp( jahMeterWeight( uv, aspect, p_eyes ), 0.0, 1.0 );

	const uint wq = uint( w * c_weightScale + 0.5 );
	if( wq == 0u )
		return;											// outside the pattern entirely
	const float wf = float( wq ) * ( 1.0 / c_weightScale );

	const int bins = imageSize( histogram ).x;
	const float binWidth = c_histSpanLog / float( bins );
	const int bin = clamp( int( floor( ( L - c_histMinLog ) / binWidth ) ), 0, bins - 1 );
	const float binMin = c_histMinLog + float( bin ) * binWidth;
	const float offset = clamp( L - binMin, 0.0, binWidth );

	imageAtomicAdd( histogram, ivec2( bin, 0 ), wq );
	imageAtomicAdd( histogram, ivec2( bin, 1 ),
					uint( wf * offset * c_logScale + 0.5 ) );
}
