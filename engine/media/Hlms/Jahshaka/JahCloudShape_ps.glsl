// THE CLOUD FIELD'S FOOTPRINT (CLOUDS-2D-3, the bake's first pass): where the
// clouds are and how thick each one is, on a 512^2 grid over the tile (125 m a
// texel) -- the outline's potential (JahCloudNoise.glsl: a domain warp, two
// lattices of clouds of their own sizes and thicknesses, an irregular outline
// and a weather field) against the coverage's threshold, times the owning
// cloud's thickness. The next two passes blur it (JahCloudBlur_ps.glsl); the
// last composes the field from the blur (JahCloudBake_ps.glsl).
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudWeather;
vulkan( layout( ogre_s0 ) uniform sampler weatherSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = coverage 0..1, y = the optical depth of a full column, z = 1 with a
	// weather map, 0 without, w = the tile in km
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
	const float weatherMap = bakeParams.z > 0.0
		? texture( vkSampler2D( cloudWeather, weatherSampler ), uv ).x : 1.0;
	const float coverage = clamp( bakeParams.x * weatherMap, 0.0, 1.0 );
	if( coverage <= 0.0 )
	{
		fragColour = 0.0;
		return;
	}
	// THE THRESHOLD a coverage puts on the potential (about -0.55 .. 1.45),
	// calibrated so the covered fraction follows the dial (cloud_2d.shape S0).
	const float threshold = 1.25 - 1.25 * pow( coverage, 0.6 );
	float core;
	const float pot = jahPotential( jahWarp( uv ), core );
	// One texel of soft edge (the blur does the rest), times the cloud's own
	// thickness: a third to all of a full column.
	fragColour = smoothstep( threshold - 0.02, threshold + 0.02, pot ) * mix( 0.3, 1.0, core );
}
