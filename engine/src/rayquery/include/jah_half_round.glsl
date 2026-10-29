// THE fp16 STORE, ROUNDED TO NEAREST (REDS-1 item 2, measured on NVIDIA 595.91): an rgba16f image store CONVERTS BY
// TRUNCATION - toward zero, not to nearest. Measured on gi.card_truth's lamp-lit wall: 1,086 stored card texels against
// the analytic at V = N lay in ( -1.25, 0 ] half-float steps, none above, a mean of -1/2 step = -0.024 .. -0.049 % of
// every value by its binade (round to nearest would sit in [ -1/2, +1/2 ]). Vulkan leaves the conversion's rounding to
// the implementation. A value about to be stored in a half-float image takes half a half-float step on its float bits
// first (the 10-bit mantissa's half unit: bit 12 of the float's 23), so the truncating store rounds half away from
// zero: to nearest (a tie is 1 input in 2^13 of a step; radiance is non-negative). The same trick as the R11G11B10F layers' jahCardRound (JahCardLight_cs.glsl). Never through
// unpackHalf2x16( packHalf2x16( x ) ), which the compiler folds to x. Clamped to the half's largest finite value first
// (65504: +Inf's bits plus the half step are a NaN pattern); an Inf or a NaN is returned as it came, never rounded.
// WRONG IN THE fp16 DENORMAL RANGE, stated: below the half's smallest normal (|x| < 2^-14) the half's half step is the
// absolute 2^-25 and the float add is smaller, so the store still truncates there - a residue under 3e-8, no guard
// (radiance). One text for the ray
// jobs (glslang) and the Hlms compute jobs (the piece JahHalfRound). No Hlms directive mark in this file.
#ifndef JAH_HALF_ROUND_GLSL
#define JAH_HALF_ROUND_GLSL
vec3 jahRoundHalf( vec3 v )
{
	// finite BY THE INPUT'S BITS (trap 5: the driver's clamp of a NaN returns a number), then clamped and rounded
	const bvec3 finite = lessThan( floatBitsToUint( v ) & uvec3( 0x7FFFFFFFu ), uvec3( 0x7F800000u ) );
	const uvec3 b = floatBitsToUint( clamp( v, vec3( -65504.0 ), vec3( 65504.0 ) ) );
	return mix( v, uintBitsToFloat( b + uvec3( 0x1000u ) ), finite );
}
vec4 jahRoundHalf( vec4 v )
{
	return vec4( jahRoundHalf( v.xyz ), jahRoundHalf( vec3( v.w ) ).x );
}
float jahRoundHalf( float v )
{
	return jahRoundHalf( vec3( v ) ).x;
}
#endif   // JAH_HALF_ROUND_GLSL
