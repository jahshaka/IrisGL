// THE ATOM VIEW'S ONE SHADER (JahAtomView.material has the contract).
//
// Every read is a texelFetch clamped into its image (no filtering, no derivatives:
// a discarded lane stops nothing, DOCS/traps/ENGINE.md), and the pixel coordinate
// comes from uv0 - the quad's own mapping onto the target - never gl_FragCoord.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform utexture2D atomIds;
vulkan_layout( ogre_t1 ) uniform texture2D atomIdDepth;
vulkan_layout( ogre_t2 ) uniform texture2D sceneDepth;
vulkan_layout( ogre_t3 ) uniform utexture2D bucketTable;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 atomViewParams;	// x = the mode (0 = draw nothing)
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

// A 32-bit integer hash (Chris Wellons' lowbias32).
uint atomHash( uint x )
{
	x ^= x >> 16u;
	x *= 0x7feb352du;
	x ^= x >> 15u;
	x *= 0x846ca68bu;
	x ^= x >> 16u;
	return x;
}

vec3 atomHsv( float h, float s, float v )
{
	vec3 k = clamp( abs( fract( vec3( h ) + vec3( 1.0, 2.0 / 3.0, 1.0 / 3.0 ) ) * 6.0 - 3.0 ) - 1.0,
					0.0, 1.0 );
	return v * mix( vec3( 1.0 ), k, s );
}

// A bright random colour: any hue, strong saturation, high value.
vec3 atomPalette( uint h )
{
	float hue = float( h & 0xFFFFu ) / 65535.0;
	float sat = 0.6 + 0.4 * float( ( h >> 16u ) & 0xFFu ) / 255.0;
	float val = 0.75 + 0.25 * float( ( h >> 24u ) & 0xFFu ) / 255.0;
	return atomHsv( hue, sat, val );
}

void main()
{
	ivec2 size = textureSize( atomIds, 0 );
	ivec2 xy = clamp( ivec2( inPs.uv0 * vec2( size ) ), ivec2( 0 ), size - ivec2( 1 ) );
	uvec2 id = texelFetch( atomIds, xy, 0 ).xy;
	ivec2 dSize = textureSize( atomIdDepth, 0 );
	ivec2 dxy = clamp( xy, ivec2( 0 ), dSize - ivec2( 1 ) );
	ivec2 sSize = textureSize( sceneDepth, 0 );
	ivec2 sxy = clamp( xy, ivec2( 0 ), sSize - ivec2( 1 ) );
	float idDepth = texelFetch( atomIdDepth, dxy, 0 ).x;
	float finalDepth = texelFetch( sceneDepth, sxy, 0 ).x;

	uint mode = uint( atomViewParams.x + 0.5 );
	uint slot = id.x & 0x00FFFFFFu;
	uint level = ( id.x >> 24u ) & 0x7u;

	vec3 colour = vec3( 0.0 );
	if( mode == 1u )
	{
		colour = atomPalette( atomHash( id.x ^ atomHash( id.y + 0x9E3779B9u ) ) );
	}
	else if( mode == 2u )
	{
		// Level 0 warm, the coarsest cold (the verb's legend).
		const vec3 ramp[8] = vec3[8]( vec3( 1.0, 0.15, 0.1 ), vec3( 1.0, 0.55, 0.0 ),
									  vec3( 1.0, 0.95, 0.1 ), vec3( 0.6, 1.0, 0.1 ),
									  vec3( 0.1, 0.85, 0.2 ), vec3( 0.1, 0.9, 0.95 ),
									  vec3( 0.15, 0.35, 1.0 ), vec3( 0.6, 0.2, 1.0 ) );
		colour = ramp[level];
	}
	else if( mode == 3u )
	{
		ivec2 tSize = textureSize( bucketTable, 0 );
		ivec2 txy = ivec2( int( slot & 1023u ), int( slot >> 10u ) );
		bool inTable = txy.y < tSize.y;
		uint bucket = texelFetch( bucketTable, clamp( txy, ivec2( 0 ), tSize - ivec2( 1 ) ), 0 ).x;
		bucket = inTable ? bucket : 0u;
		// Bucket ids are SERIAL (1, 2, 3, ...): a golden-ratio walk round the hue
		// circle keeps neighbouring ids far apart, which a hash does not promise.
		colour = bucket == 0u ? vec3( 0.3 )
							  : atomHsv( fract( float( bucket ) * 0.61803398875 ), 0.85, 0.95 );
	}
	else
	{
		colour = atomPalette( atomHash( slot + 0x68E31DA4u ) );
	}

	// LAST (the helper-lane rule): nothing named, a mode of zero, or a surface
	// that is not the atom item the id names (a stock-PBR object in front of it).
	if( id.x == 0xFFFFFFFFu || mode == 0u || mode > 4u ||
		floatBitsToUint( idDepth ) != floatBitsToUint( finalDepth ) )
		discard;
	fragColour = vec4( colour, 1.0 );
}
