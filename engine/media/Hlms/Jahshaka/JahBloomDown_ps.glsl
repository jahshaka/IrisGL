#version ogre_glsl_ver_330

#include "JahScreen.glsl"

// THE BLOOM LADDER'S DOWNSAMPLE (BLOOM-1): one step of the progressive mip
// ladder, the source level to a target half its size in each axis. The filter
// is the 13-tap downsample of Jimenez, "Next Generation Post Processing in
// Call of Duty: Advanced Warfare" (SIGGRAPH 2014): thirteen bilinear taps
// that together cover a 6x6 source footprint, read as FIVE overlapping 2x2
// boxes - the inner box (the four taps one source texel from the centre,
// weight 0.5) and the four corner boxes (weight 0.125 each). It is isotropic
// by construction: every step halves BOTH axes of a ladder sized from the
// view (OgreChain.cpp, kBloomLevel), so a highlight spreads by the same
// number of pixels across and down at any aspect.
//
// JAH_BLOOM_PREFILTER is the FIRST step only, from the scene's radiance:
//   * the exposure, from the 1x1 the tonemapper reads (the threshold is in the
//     tonemapper's units, as it always was),
//   * the soft knee per tap - below brightThreshold.x nothing blooms, above
//     brightThreshold.x + 1 / brightThreshold.y everything does, a smoothstep
//     in between (the old bright pass's exact ramp),
//   * the Karis average across the five boxes: each box weighted by
//     1 / (1 + luma), so one firefly pixel cannot flash a whole halo on and
//     off as it crawls (Jimenez's partial Karis average; the later steps are
//     plain, which keeps the energy).
// The ladder stores LINEAR radiance in R11G11B10_FLOAT; the encode the
// tonemapper expects happens once, at the end (JahBloomResolve_ps.glsl).

vulkan_layout( ogre_t0 ) uniform JahScreenTexture sourceTexture;
vulkan( layout( ogre_s0 ) uniform sampler bilinearSampler );

#ifdef JAH_BLOOM_PREFILTER
vulkan_layout( ogre_t1 ) uniform texture2D lumRt;
vulkan( layout( ogre_s1 ) uniform sampler pointSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = the threshold, y = 1 / knee width (chain::setBloomThreshold).
	uniform vec2 brightThreshold;
vulkan( }; )
#endif

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#ifdef JAH_BLOOM_PREFILTER
float gInvLum;
#endif

vec3 tap( vec2 uv )
{
	vec3 c = texture( jahScreenSampler( sourceTexture, bilinearSampler ), jahScreenUv( uv ) ).xyz;
#ifdef JAH_BLOOM_PREFILTER
	// Finite and inside the ladder's format (R11G11B10_FLOAT tops out at
	// 65024): a scene value beyond it would store as infinity and smear.
	c = min( max( c * gInvLum, vec3( 0.0 ) ), vec3( 65000.0 ) );
	const vec3 w = clamp( ( c - brightThreshold.xxx ) * brightThreshold.yyy, 0.0, 1.0 );
	c *= w * w * ( 3.0 - 2.0 * w );
#endif
	return c;
}

#ifdef JAH_BLOOM_PREFILTER
float karis( vec3 c )
{
	return 1.0 / ( 1.0 + dot( c, vec3( 0.2126, 0.7152, 0.0722 ) ) );
}
#endif

void main()
{
#ifdef JAH_BLOOM_PREFILTER
	gInvLum = texture( vkSampler2D( lumRt, pointSampler ), vec2( 0.0, 0.0 ) ).x;
#endif
	const vec2 t = vec2( 1.0 ) / vec2( jahScreenSize( jahScreenSampler( sourceTexture, bilinearSampler ), 0 ) );
	const vec2 uv = inPs.uv0;

	const vec3 a = tap( uv + t * vec2( -2.0, -2.0 ) );
	const vec3 b = tap( uv + t * vec2(  0.0, -2.0 ) );
	const vec3 c = tap( uv + t * vec2(  2.0, -2.0 ) );
	const vec3 d = tap( uv + t * vec2( -1.0, -1.0 ) );
	const vec3 e = tap( uv + t * vec2(  1.0, -1.0 ) );
	const vec3 f = tap( uv + t * vec2( -2.0,  0.0 ) );
	const vec3 g = tap( uv );
	const vec3 h = tap( uv + t * vec2(  2.0,  0.0 ) );
	const vec3 i = tap( uv + t * vec2( -1.0,  1.0 ) );
	const vec3 j = tap( uv + t * vec2(  1.0,  1.0 ) );
	const vec3 k = tap( uv + t * vec2( -2.0,  2.0 ) );
	const vec3 l = tap( uv + t * vec2(  0.0,  2.0 ) );
	const vec3 m = tap( uv + t * vec2(  2.0,  2.0 ) );

	const vec3 inner = ( d + e + i + j ) * 0.25;
	const vec3 tl = ( a + b + f + g ) * 0.25;
	const vec3 tr = ( b + c + g + h ) * 0.25;
	const vec3 bl = ( f + g + k + l ) * 0.25;
	const vec3 br = ( g + h + l + m ) * 0.25;

#ifdef JAH_BLOOM_PREFILTER
	const float wi = 0.5 * karis( inner );
	const float w0 = 0.125 * karis( tl );
	const float w1 = 0.125 * karis( tr );
	const float w2 = 0.125 * karis( bl );
	const float w3 = 0.125 * karis( br );
	const vec3 sum = ( inner * wi + tl * w0 + tr * w1 + bl * w2 + br * w3 ) /
					 ( wi + w0 + w1 + w2 + w3 );
#else
	const vec3 sum = inner * 0.5 + ( tl + tr + bl + br ) * 0.125;
#endif
	fragColour = vec4( sum, 1.0 );
}
