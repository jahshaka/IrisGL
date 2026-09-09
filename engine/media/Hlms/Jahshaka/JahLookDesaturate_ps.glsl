// LOOK: DESATURATE (POST_LOOKS_SPEC.md §2 row "B&W", built with an amount).
//
// The plumbing proof for the whole looks stage, and the reason it is first:
// there is nothing in it but the two properties every look in the catalogue
// has to have.
//
//   1. AT AMOUNT 0 IT IS AN EXACT IDENTITY. mix(src, grey, 0.0) is
//      src * 1.0 + grey * 0.0 in IEEE arithmetic, i.e. src, bit for bit — and
//      the stage's targets are sRGB, whose decode/encode round trip is exact
//      for all 256 values. That is what makes "a look at zero is
//      byte-identical to no look" an assertion rather than a tolerance.
//   2. AT AMOUNT 1 IT HAS A STRUCTURAL PROPERTY no tolerance is needed for:
//      R == G == B on every pixel.
//
// Upstream's GrayScale_ps.glsl is the reference (Rec.601 weights, the same
// three constants); what it does not have is the amount, because the sample's
// looks are toggles rather than settings. §3 decision D2 is exactly this: the
// knob is the product, and adding one to a sample shader would fork it.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount (0 = identity, 1 = fully grey)
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const vec4 src = texture( vkSampler2D( sourceTexture, pointSampler ), inPs.uv0 );
	// Rec.601 luma, the sample's weights.
	const float luma = dot( src.rgb, vec3( 0.3, 0.59, 0.11 ) );
	const float amount = clamp( lookParams0.x, 0.0, 1.0 );
	fragColour = vec4( mix( src.rgb, vec3( luma ), amount ), src.a );
}
