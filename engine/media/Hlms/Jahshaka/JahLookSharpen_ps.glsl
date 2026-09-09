// LOOK: SHARPEN (POST_LOOKS_SPEC.md §2 row "Sharpen Edges").
//
// An unsharp mask: the pixel plus its own difference from a 3x3 box blur of its
// neighbourhood. Upstream's SharpenEdges_ps.glsl is the reference and it is the
// same kernel written the other way round (9x the centre minus the eight
// neighbours) — which is this at a FIXED strength of 1 with no way to ask for
// less, and which also brightens the image, because its weights sum to one only
// on a flat region.
//
// The form here is the ordinary one and it has the property the catalogue's
// contract needs for free: at strength 0 the correction term is exactly zero,
// so the output is exactly the input, with no mix and no branch.
//
// The nine taps are texelFetch, not offset texture() reads: an integer
// neighbourhood is what a 3x3 kernel means, and it is exact at any resolution.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount
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

	vec3 box = vec3( 0.0 );
	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			const ivec2 c = clamp( srcCoord + ivec2( dx, dy ), ivec2( 0 ), texSize - ivec2( 1 ) );
			box += texelFetch( vkSampler2D( sourceTexture, srcSampler ), c, 0 ).rgb;
		}
	}
	box *= 1.0 / 9.0;

	// 3 at amount 1: enough to be unmistakably sharper without ringing into
	// black on the fixture's hard silhouettes.
	const vec3 sharpened = src.rgb + ( src.rgb - box ) * ( amount * 3.0 );
	fragColour = vec4( clamp( sharpened, vec3( 0.0 ), vec3( 1.0 ) ), src.a );
}
