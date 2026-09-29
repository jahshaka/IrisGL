// THE CLOUD FIELD'S EDGE BLUR (CLOUDS-2D-3, the bake's second and third
// passes): a separable Gaussian of the footprint (JahCloudShape_ps.glsl), sigma
// kSigmaKm, along x (JAH_BLUR_V undefined) or y, wrapped with the tile. The
// blurred footprint IS the column's thickness profile: it rises over
// kilometres around every outline (the density ramp), a small cloud stays thin
// (its footprint never fills the kernel), and a large one reaches its core.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D cloudIn;
vulkan( layout( ogre_s0 ) uniform sampler cloudInSampler );

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = texels per km of the source grid
	uniform vec4 blurParams;
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
	const float kSigmaKm = 0.6;
	const vec2 size = vec2( textureSize( vkSampler2D( cloudIn, cloudInSampler ), 0 ) );
	const float sigma = kSigmaKm * blurParams.x;   // texels
	const int radius = int( ceil( 3.0 * sigma ) );
#ifdef JAH_BLUR_V
	const vec2 step = vec2( 0.0, 1.0 / size.y );
#else
	const vec2 step = vec2( 1.0 / size.x, 0.0 );
#endif
	float sum = 0.0, norm = 0.0;
	for( int i = -radius; i <= radius; ++i )
	{
		const float w = exp( -0.5 * float( i * i ) / ( sigma * sigma ) );
		sum += w * textureLod( vkSampler2D( cloudIn, cloudInSampler ), inPs.uv0 + step * float( i ), 0.0 ).x;
		norm += w;
	}
	fragColour = sum / norm;
}
