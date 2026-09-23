// THE CLOUD LAYER (CLOUDS-2D-1; SPECS/CLOUDS_ASSESSMENT.md option C0 and its
// section 4). A full-screen quad at render queue 0 drawn OVER the sky, blended
// premultiplied: the sky behind is multiplied by the sheet's transmittance
// along the view ray and the light the sheet scatters towards the eye is
// added. It is inside the environment capture's range, so the ambient SH and
// every reflection see what this draws.
//
// THE LIGHT, per pixel of sheet (a medium, nothing about an enclosure):
//   * SUN, DIFFUSELY TRANSMITTED -- the Eddington two-stream answer for a
//     conservative slab of optical depth tau lit by a collimated beam at
//     cosine mu_s (Meador and Weaver 1980; asymmetry g folded in as the
//     similarity factor 1 - g):
//         T_total  = [ (1/2 + 3/4 mu_s) + (1/2 - 3/4 mu_s) exp(-tau / mu_s) ]
//                    / ( 1 + 3/4 (1 - g) tau )
//         T_direct = exp( -tau / mu_s )
//     and the base glows with the diffusely transmitted part,
//         L = E mu_s ( T_total - T_direct ) / pi
//     (a thick deck's base under a high sun is about a third as bright as a
//     white card facing the same sun, and a low sun reaches it far less --
//     which is what an overcast base looks like).
//   * SUN, SINGLY SCATTERED towards the eye through the thin parts -- a two-lobe
//     Henyey-Greenstein phase (a strong forward lobe: the bright rim of a cloud
//     around the sun; a weak back lobe), attenuated by the unscattered beam.
//   * SELF-SHADOW: N steps through the field TOWARDS THE SUN across one slab
//     thickness; where the neighbours on the sun's side are thicker than this
//     column, this column sits in their shadow (the low-sun case -- at a high
//     sun the steps collapse onto the column itself and change nothing).
//   * SKY LIGHT: the CLEAR sky's mean radiance (SH band 0 of a capture taken
//     with the sheet hidden -- never of the capture the sheet is in, which
//     would light the sheet by itself), diffusely transmitted through it:
//     a thin sheet in that light returns it (a conservative scatterer in a
//     uniform field), a thick one passes 1 / ( 1 + 3/4 (1 - g) tau ) of it.
//   * DISTANCE: a sheet 100 km away is seen through that much air. The sky
//     model draws no aerial perspective for us to composite into, so the far
//     sheet fades into the sky itself with distance (a stated proxy).
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
	// rgb = the sky's mean radiance (SH band 0), w = the distance fade (m)
	uniform vec4 cloudAmbient;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec3 cameraDir;
} inPs;

vulkan_layout( location = 0 )
out vec4 fragColour;

#define JAH_CLOUD_TAU( uv ) texture( vkSampler2D( cloudField, cloudSampler ), uv ).x
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
	const float tau = JAH_CLOUD_TAU( hit.uv );
	const float tView = jahCloudViewTransmittance( tau, hit );
	const float opacity = 1.0 - tView;

	const float kG = 0.85;
	const float similarity = 1.0 + 0.75 * ( 1.0 - kG ) * tau;
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
		const float tTotal = ( ( 0.5 + 0.75 * muS ) + ( 0.5 - 0.75 * muS ) * tDirect ) / similarity;
		const float tDiffuse = max( tTotal - tDirect, 0.0 );
		const float cosTheta = dot( dir, cloudSun.xyz );
		const float phase = 0.8 * jahHenyeyGreenstein( cosTheta, 0.7 ) +
							0.2 * jahHenyeyGreenstein( cosTheta, -0.2 );
		radiance += sunE * ( muS * tDiffuse * ( 1.0 / 3.14159265 ) + phase * opacity * tDirect );
	}

	const float fade = exp( -hit.dist / max( cloudAmbient.w, 1.0 ) );
	vec4 outColour = vec4( radiance * fade, opacity * fade );
	if( !( jahFinite( outColour.x ) && jahFinite( outColour.y ) && jahFinite( outColour.z ) &&
		   jahFinite( outColour.w ) ) )
		outColour = vec4( 0.0 );
	fragColour = outColour;
}
