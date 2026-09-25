// THE CLOUD FIELD BAKE (CLOUDS-2D-1): one tile of the layer's VERTICAL OPTICAL
// DEPTH, rendered into a tiling R16F target on a change of coverage, density
// or weather map -- never per frame. The layer, the clouded sun disc and the
// ground shadow (JahFog_piece_vs_piece_ps.any) all read THIS texture, so the
// sheet in the sky and its shadow on the ground are one field by construction.
//
// THE NOISE is generated once on the CPU with a fixed seed (OgreSky.cpp,
// cloudNoise): R = a Perlin-Worley cumulus field, G = a finer Worley field (the
// edges' erosion), B = a low-frequency Perlin field (the large-scale patchiness
// that makes a coverage of 0.5 look like weather rather than a pattern). Every
// sample below is taken at an INTEGER multiple of the tile, so the field tiles.
//
// COVERAGE is a threshold on the cumulus field: 0 leaves the field EXACTLY
// zero (smoothstep below its first edge is 0), which is what makes a clear
// layer invisible to the last bit; 1 closes it into an overcast deck.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudNoise;
vulkan_layout( ogre_t1 ) uniform texture2D cloudWeather;
vulkan( layout( ogre_s0 ) uniform sampler noiseSampler );
vulkan( layout( ogre_s1 ) uniform sampler weatherSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = coverage 0..1, y = the optical depth of a full column (density x the
	// layer's constant), z = 1 with a weather map, 0 without, w unused
	uniform vec4 bakeParams;
vulkan( }; )

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan_layout( location = 0 )
out float fragColour;

void main()
{
	const vec2 uv = inPs.uv0;
	const float cumulus = texture( vkSampler2D( cloudNoise, noiseSampler ), uv * 2.0 ).x;
	const float detail  = texture( vkSampler2D( cloudNoise, noiseSampler ), uv * 8.0 ).y;
	const float large   = texture( vkSampler2D( cloudNoise, noiseSampler ), uv ).z;
	const float weather = bakeParams.z > 0.0
		? texture( vkSampler2D( cloudWeather, weatherSampler ), uv ).x : 1.0;
	const float coverage = clamp( bakeParams.x * weather, 0.0, 1.0 );

	const float shape = clamp( cumulus * 0.75 + large * 0.35 - 0.05, 0.0, 1.0 );
	float d = smoothstep( 1.0 - coverage, 1.0 - coverage + 0.4, shape );
	// The overcast deck a full coverage closes into, with the cumulus field
	// still showing through it (a deck is never a flat grey).
	d = max( d, smoothstep( 0.75, 1.0, coverage ) * ( 0.45 + 0.3 * large + 0.25 * cumulus ) );
	// Erode the EDGES with the finer field; the cores keep their density.
	d = clamp( d - ( 1.0 - detail ) * 0.35 * ( 1.0 - d ), 0.0, 1.0 );
	// THE DENSITY RISES QUADRATICALLY from an edge: a cloud's margin is a thin
	// medium over a distance, not a wall, and a linear ramp times a full
	// column's depth made every edge opaque within a texel.
	fragColour = d * d * bakeParams.y;
}
