// THE CLOUD LAYER (CLOUDS-2D-1; SPECS/CLOUDS_ASSESSMENT.md option C0 and its
// section 4). A full-screen quad at render queue 0 drawn OVER the sky, blended
// premultiplied: the sky behind is multiplied by the sheet's transmittance
// along the view ray and the light the sheet scatters towards the eye is
// added. It is inside the environment capture's range, so the ambient SH and
// every reflection see what this draws.
//
// THE LIGHT, per pixel of sheet (a medium, nothing about an enclosure). The
// sheet CONSERVES ENERGY (CLOUDS-2D-3): what it sends down is never more than
// the sun's beam loses crossing it, and its base is never brighter than the
// light arriving on its top.
//   * SUN, TRANSMITTED -- the Eddington two-stream answer for a conservative
//     slab lit by a collimated beam at cosine mu_s (Meador and Weaver 1980),
//     in its SIMILARITY form: the strongly forward phase function is a
//     forward peak plus an isotropic rest, so the slab the two-stream solves
//     has the scaled depth tau' = (1 - g) tau, and EVERY term uses tau'
//         T_total = [ (1/2 + 3/4 mu_s) + (1/2 - 3/4 mu_s) exp(-tau' / mu_s) ]
//                   / ( 1 + 3/4 tau' )
//     which is at most 1 for every tau' and mu_s (for mu_s >= 2/3 the
//     numerator's excess over 1 is below 3/4 tau'; below 2/3 it is negative).
//     The earlier sheet divided by the scaled depth but decayed the numerator
//     with the UNSCALED one, which returned up to 1.04 of the beam at a high
//     sun (tau ~ 1) -- and added a single-scatter lobe ON TOP of a
//     transmission that already held every scattered photon: the thin
//     margins sent down up to twice what they took out of the beam.
//     The beam that crosses splits three ways, and they sum to T_total:
//       T_direct  = exp( -tau / mu_s )         unscattered -- the ground
//                                              shadow's own factor, drawn
//                                              by the sun disc, not here
//       T_forward = exp( -tau' / mu_s ) - T_direct
//                                              the forward peak: seen in a
//                                              Henyey-Greenstein lobe of the
//                                              same g around the sun, scaled
//                                              so its flux through the base
//                                              is exactly T_forward (the
//                                              lobe's integral over the sky
//                                              below the sheet is taken on
//                                              the CPU, cloudPhase.x)
//       T_diffuse = T_total - exp( -tau' / mu_s )
//                                              the multiply scattered rest,
//                                              the same radiance every way
//     so the radiance the base shows is E mu_s ( T_diffuse / pi +
//     T_forward HG(cos) / norm ), and its flux is E mu_s ( T_total - T_direct ):
//     exactly what the beam lost, less what the top reflects.
//   * SELF-SHADOW: N steps through the field TOWARDS THE SUN across one slab
//     thickness; where the neighbours on the sun's side are thicker than this
//     column, this column sits in their shadow (the low-sun case -- at a high
//     sun the steps collapse onto the column itself and change nothing).
//     It only ever removes light.
//   * SKY LIGHT: the light the CLEAR sky puts on the sheet's top -- its
//     irradiance on an up-facing plate over pi, from a capture taken with the
//     sheet hidden (never the capture the sheet is in, which would light the
//     sheet by itself; never the SPHERE's mean, whose horizon glow and lower
//     half lit a thin deck with 2.6x the sky light a plate receives) --
//     diffusely transmitted through it:
//     the sky behind keeps exp( -tau / mu ) of itself and the sheet adds
//     ( 1 - exp( -tau / mu ) ) / ( 1 + 3/4 tau' ) of the mean, so a pixel of
//     sheet never shows more sky light than the clear sky did.
//   * DISTANCE: a sheet 100 km away is seen through that much air. The sky
//     model draws no aerial perspective for us to composite into, so the far
//     sheet fades into the sky itself with distance (a stated proxy), and is
//     read through a coarser mip the more grazing the view (below).
//
// THE SUN'S IRRADIANCE arrives in the renderer's own units -- what a white
// Lambert card facing the sun would reflect times pi -- so the sheet and a
// lit surface are on one scale (OgreSky.cpp applyCloudLayer).
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudField;
vulkan( layout( ogre_s0 ) uniform sampler cloudSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 cameraPos;
	// x = altitude (m), y = 1 / tile (1/m), zw = scroll (m)
	uniform vec4 cloudLayer;
	// xyz = unit vector towards the sun, w = 1 with a sun, 0 without
	uniform vec4 cloudSun;
	// rgb = the sun's irradiance, w = the slab thickness the self-shadow crosses (m)
	uniform vec4 cloudSunE;
	// rgb = the clear sky's irradiance on the sheet's top / pi, w = the distance fade (m)
	uniform vec4 cloudAmbient;
	// x = 1 / the forward lobe's flux through the base (the HG lobe of g
	// around the sun integrated over the sky below the sheet, cosine
	// weighted; OgreSky.cpp cloudForwardLobeFlux), yzw unused
	uniform vec4 cloudPhase;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

// THE FAR EDGE, SOFTENED BY THE VIEW'S GRAZING ANGLE (CLOUDS-2D-3): towards the
// horizon a pixel covers kilometres of sheet and the slant multiplies every
// depth by 1 / mu, so the field is read through a coarser mip the more grazing
// the ray -- half a mip per halving of mu -- and a far cloud's edge is its
// footprint's average, not a texel's step magnified into a wall.
float jahCloudBias = 0.0;
#define JAH_CLOUD_TAU( uv ) texture( vkSampler2D( cloudField, cloudSampler ), uv, jahCloudBias ).x
#include "JahCloudLayer.glsl"

float jahHenyeyGreenstein( float cosTheta, float g )
{
	const float g2 = g * g;
	const float denom = max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-4 );
	return ( 1.0 - g2 ) / ( 4.0 * 3.14159265 * denom * sqrt( denom ) );
}

// NaN AND INF ON THE BITS (DOCS/traps/ENGINE.md: x == x is folded to true here).
bool jahFinite( float x ) { return ( floatBitsToUint( x ) & 0x7FFFFFFFu ) < 0x7F800000u; }

void main()
{
	const vec3 dir = normalize( inPs.cameraDir );
	const JahCloudHit hit = jahCloudIntersect( dir, cameraPos.xyz, cloudLayer );
	if( hit.above <= 0.0 )
	{
		fragColour = vec4( 0.0, 0.0, 0.0, 0.0 );
		return;
	}
	jahCloudBias = 0.5 * log2( 1.0 / max( hit.mu, 0.05 ) );
	const float tau = JAH_CLOUD_TAU( hit.uv );
	const float tView = jahCloudViewTransmittance( tau, hit );
	const float opacity = 1.0 - tView;

	// THE SIMILARITY SCALING (the header): one g, one scaled depth, every term.
	const float kG = 0.85;
	const float tauScaled = ( 1.0 - kG ) * tau;
	const float similarity = 1.0 + 0.75 * tauScaled;
	vec3 radiance = cloudAmbient.rgb * ( opacity / similarity );
	if( cloudSun.w > 0.0 && cloudSun.y > 0.0 )
	{
		const float muS = max( cloudSun.y, 0.02 );
		// SELF-SHADOW: the mean optical depth of the column's sun-side
		// neighbours across one slab thickness, against its own.
		const int kSteps = 6;
		const vec2 throwUv = cloudSun.xz / muS * cloudSunE.w * cloudLayer.y;
		float tauSun = 0.0;
		for( int i = 0; i < kSteps; ++i )
			tauSun += JAH_CLOUD_TAU( hit.uv + throwUv * ( ( float( i ) + 0.5 ) / float( kSteps ) ) );
		tauSun /= float( kSteps );
		const float shadowed = exp( -0.5 * max( tauSun - tau, 0.0 ) / muS );
		const vec3 sunE = cloudSunE.rgb * shadowed;

		const float tDirect = exp( -tau / muS );
		const float tPeak = exp( -tauScaled / muS );
		const float tTotal = min( ( ( 0.5 + 0.75 * muS ) + ( 0.5 - 0.75 * muS ) * tPeak ) / similarity,
								  1.0 );
		// The three shares of the crossing beam sum to max( T_total, T_direct )
		// whatever the two-stream's own small-depth error does.
		const float tForward = max( min( tPeak, tTotal ) - tDirect, 0.0 );
		const float tDiffuse = max( tTotal - tPeak, 0.0 );
		const float cosTheta = dot( dir, cloudSun.xyz );
		radiance += sunE * muS * ( tDiffuse * ( 1.0 / 3.14159265 ) +
								   tForward * jahHenyeyGreenstein( cosTheta, kG ) * cloudPhase.x );
	}

	const float fade = exp( -hit.dist / max( cloudAmbient.w, 1.0 ) );
	vec4 outColour = vec4( radiance * fade, opacity * fade );
	if( !( jahFinite( outColour.x ) && jahFinite( outColour.y ) && jahFinite( outColour.z ) &&
		   jahFinite( outColour.w ) ) )
		outColour = vec4( 0.0 );
	fragColour = outColour;
}
