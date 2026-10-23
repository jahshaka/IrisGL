// THE REFLECTION'S DENOISER SPACE (PHOTON-7 stage 6, commit 6 - Lumen's, LUMEN_VR_FACTS C7.1) - one text for the
// resolve, the temporal pass and the spatial filter: a luminance-Reinhard space on PRE-EXPOSED colour,
//   D( c ) = c / ( 1 + E L( c ) / T ),   c = D / ( 1 - E L( D ) / T ),
// E the view's exposure multiplier (the tonemapper's - the chain's measured history, or its fixed constant), T Lumen's
// tonemap range 10, L the Rec. 709 luminance. Every average a pass takes - the resolve's reconstruction, the box, the
// history's blend - is a mean of D( c ), so one firefly weighs at most T / E against the rest; the stored history is
// linear (converted back), as Lumen's. The paired run on gi.reflect_mover's floor (spikes/photon-7/stage6/c6-paired.txt)
// measured this form against the linear box and the own-sample extension it replaces: -0.025 against -0.612 codes of
// bias, |.| 2.49 against 2.64.
// The includer defines JAH_DENOISE_EXPOSURE (float, > 0) before this file.
#ifndef JAH_REFLECT_DENOISE_GLSL
#define JAH_REFLECT_DENOISE_GLSL

/// Lumen's r.Lumen.Reflections.DenoiserTonemapRange.
const float kDenoiserTonemapRange = 10.0;
const vec3 kLuma709 = vec3( 0.2126390059, 0.7151686788, 0.0721923154 );

float jahDenoiseLuma( vec3 c ) { return dot( c, kLuma709 ); }
vec3 jahToDenoise( vec3 c )
{
	return c / ( 1.0 + jahDenoiseLuma( c ) * ( JAH_DENOISE_EXPOSURE / kDenoiserTonemapRange ) );
}
vec3 jahFromDenoise( vec3 d )
{
	return d / max( 1.0 - jahDenoiseLuma( d ) * ( JAH_DENOISE_EXPOSURE / kDenoiserTonemapRange ), 1e-4 );
}
/// YCoCg (non-normalised, Lumen's StochasticLightingCommon) and back.
vec3 jahToYCoCg( vec3 c )
{
	return vec3( dot( c, vec3( 0.25, 0.5, 0.25 ) ), dot( c, vec3( 0.5, 0.0, -0.5 ) ), dot( c, vec3( -0.25, 0.5, -0.25 ) ) );
}
vec3 jahFromYCoCg( vec3 y )
{
	return vec3( y.x + y.y - y.z, y.x + y.z, y.x - y.y - y.z );
}

#endif
