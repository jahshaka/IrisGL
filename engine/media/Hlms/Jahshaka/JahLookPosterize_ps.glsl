// LOOK: POSTERIZE (POST_LOOKS_SPEC.md §2 row "Posterize").
//
// Colour quantization, in a gamma-bent space so the bands fall where the eye
// puts them rather than where the numbers do. Upstream's Posterize_ps.glsl is
// the reference and it is four lines long; the two numbers that decide what it
// LOOKS like — eight levels and a gamma of 0.6 — are literals in the shader
// body, which is the single clearest example of why this program authors its
// own media (§3 decision D2).
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount, y = levels, z = gamma
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const ivec2 texSize = textureSize( vkSampler2D( sourceTexture, srcSampler ), 0 );
	const ivec2 srcCoord = clamp( ivec2( inPs.uv0 * vec2( texSize ) ),
								  ivec2( 0 ), texSize - ivec2( 1 ) );
	const vec4  src = texelFetch( vkSampler2D( sourceTexture, srcSampler ), srcCoord, 0 );

	const float amount = clamp( lookParams0.x, 0.0, 1.0 );
	const float levels = max( lookParams0.y, 2.0 );
	const float gamma  = max( lookParams0.z, 0.05 );

	vec3 tc = pow( max( src.rgb, vec3( 0.0 ) ), vec3( gamma ) );
	tc = floor( tc * levels ) / levels;
	tc = pow( tc, vec3( 1.0 / gamma ) );

	fragColour = vec4( mix( src.rgb, tc, amount ), src.a );
}
