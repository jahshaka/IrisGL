// LOOK: GLASS WARP (POST_LOOKS_SPEC.md §2 row "Glass").
//
// Refraction through rippled glass: the image is resampled through a displaced
// coordinate. Upstream's Glass_ps.glsl is the reference — the same idea, one
// texture fetch through an offset UV — with two differences that are the whole
// reason we author our own (§3 decision D2):
//
//   1. THE STRENGTH IS A PARAMETER. The sample multiplies its normal by a
//      hard-coded 0.05. There is no way to ask it for less.
//   2. THE RIPPLE IS PROCEDURAL. The sample samples WaterNormal1.tga from
//      Ogre's 1.x media folder, which we do not stage and would then own a copy
//      of. Two sinusoids at right angles are a ripple field with a SCALE knob
//      and no asset at all.
//
// At amount 0 the displacement is exactly zero AND the source pixel is taken
// with texelFetch (see JahLookCommon_piece_ps.glsl), so the look is an exact
// identity — not "almost", at any resolution.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount, y = ripple scale
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
	const float scale  = max( lookParams0.y, 0.0 );

	// Two sinusoids at right angles, each phase-shifted by the other axis so the
	// field is not separable and the ripple reads as glass rather than as a
	// grid. The 0.02 is the maximum displacement in UV at amount 1 — a fifth of
	// the frame's width would be a funhouse mirror, not a window.
	const vec2 ripple = vec2( sin( ( inPs.uv0.y * scale + inPs.uv0.x * 0.7 ) * 6.2831853 ),
							  cos( ( inPs.uv0.x * scale + inPs.uv0.y * 0.3 ) * 6.2831853 ) );
	const vec2 warpedUv = inPs.uv0 + ripple * ( amount * 0.02 );

	const vec4 warped = texture( vkSampler2D( sourceTexture, srcSampler ),
								 clamp( warpedUv, vec2( 0.0 ), vec2( 1.0 ) ) );

	// A SELECT, not a mix: cross-fading the warped image with the unwarped one
	// would ghost at every amount between 0 and 1, and the amount already lives
	// in the displacement. The branch is uniform over the whole quad.
	fragColour = amount > 0.0 ? warped : src;
}
