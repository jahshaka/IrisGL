// THE CLOUD FIELD BAKE (CLOUDS-2D-1; its shape CLOUDS-2D-3): one tile of the
// layer's VERTICAL OPTICAL DEPTH, rendered into a tiling 2048^2 R16F target on a
// change of coverage, density or weather map -- never per frame. The layer, the
// clouded sun disc and the ground shadow (JahFog_piece_vs_piece_ps.any), the
// voxels' light injection and the cards' relight all read THIS texture, so the
// sheet in the sky and its shadow on the ground are one field by construction.
//
// THE SHAPE, and why each part is there (the owner 2026-09-28: "they look all
// the same, hard edges"). Four passes (JahshakaClouds.compositor):
//   1. THE FOOTPRINT (JahCloudShape_ps.glsl, 512^2): a DOMAIN WARP (every
//      lattice read at the texel moved by a smooth few-kilometre noise, so no
//      lattice's rows or period line up into a pattern), CLOUDS OF THEIR OWN
//      (two cell lattices, ~5 km and ~1.6 km cells, one cloud or none per cell,
//      each at its own place with its own radius and its own THICKNESS), an
//      IRREGULAR OUTLINE (a three-octave noise) and WEATHER (a twenty-kilometre
//      field of more and less cloud), against the coverage's threshold, times
//      the owning cloud's thickness;
//   2-3. THE EDGE (JahCloudBlur_ps.glsl): a Gaussian of the footprint, 0.6 km
//      sigma -- the column's thickness profile rises over kilometres around
//      every outline, never within a texel; a small cloud stays thin;
//   4. THIS PASS: the depth is the blurred thickness SQUARED (a thin medium
//      over the margin, the core where the footprint fills the kernel) times a
//      full column, with a fine noise ERODING the margin into wisps (never the
//      core), and the OVERCAST DECK a coverage above three quarters closes
//      into, the weather still showing through it.
// COVERAGE 0 leaves the field EXACTLY zero, which is what makes a clear layer
// invisible to the last bit.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudBlurred;
vulkan_layout( ogre_t1 ) uniform texture2D cloudWeather;
vulkan( layout( ogre_s0 ) uniform sampler cloudBlurredSampler );
vulkan( layout( ogre_s1 ) uniform sampler weatherSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = coverage 0..1, y = the optical depth of a full column (density x the
	// layer's constant), z = 1 with a weather map, 0 without, w = the tile in km
	uniform vec4 bakeParams;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float fragColour;

#include "JahCloudNoise.glsl"

void main()
{
	const vec2 uv = inPs.uv0;
	// THE WEATHER MAP SCALES THE COVERAGE HERE TOO (the footprint pass applies it
	// to the clouds; the deck below is this pass's own): a black region of the map
	// is clear sky at any dial, the deck included.
	const float weatherMap = bakeParams.z > 0.0
		? texture( vkSampler2D( cloudWeather, weatherSampler ), uv ).x : 1.0;
	const float coverage = clamp( bakeParams.x * weatherMap, 0.0, 1.0 );
	if( coverage <= 0.0 )
	{
		fragColour = 0.0;
		return;
	}
	const float thick = texture( vkSampler2D( cloudBlurred, cloudBlurredSampler ), uv ).x;
	// THE EROSION: a fine noise (~400 m down to 100 m) thins the margin where
	// the profile is low; the core keeps its density.
	const float fine = clamp( 0.5 + 0.5 * jahFbm( uv, 160, 3, 0.55, 0x3579u ) / 0.7, 0.0, 1.0 );
	const float erode = mix( 0.25 + 0.75 * fine, 1.0, smoothstep( 0.2, 0.8, thick ) );
	float d = thick * thick * erode * ( 0.9 + 0.1 * fine );

	// THE OVERCAST DECK a coverage above three quarters closes into.
	const float deck = smoothstep( 0.75, 1.0, coverage );
	if( deck > 0.0 )
	{
		const vec2 w = jahWarp( uv );
		const float weather = jahFbm( w, 3, 2, 0.5, 0x1357u );
		const float outline = jahFbm( w, 24, 3, 0.5, 0x2468u );
		d = max( d, deck * ( 0.45 + 0.3 * ( weather + 0.5 ) + 0.25 * ( outline + 0.5 ) ) );
	}
	fragColour = clamp( d, 0.0, 1.0 ) * bakeParams.y;
}
