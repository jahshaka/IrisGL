// THE DISTORTION COMPOSE QUAD (SPECS/POST_LOOKS_SPEC.md §5.3).
//
// Warps the scene image by the screen-space displacement field the distortion
// objects rendered into their own target a pass earlier. Upstream's
// Distortion_ps.glsl is the reference and the arithmetic is the same:
//
//   offset = (field.rg - 0.5) * 2 * field.a * globalStrength
//
// The field's target is CLEARED to (0.5, 0.5, 0, 0), so a pixel no emitter
// covered decodes to a zero offset AND a zero strength — untouched twice over.
// That is what makes "no distortion object on screen" and "strength 0" both
// byte-identical to a frame with the feature off: the fetch below lands on the
// source texel and a bilinear fetch at a texel centre is exact.
//
// THE CHROMATIC FRINGE is upstream's too, and it is the reason this is not a
// single fetch: the three channels are sampled at 80%, 90% and 100% of the
// offset, so a strong warp separates colour the way a real refracting medium
// does. At small offsets the three land on the same texel and it costs nothing
// visible.
//
// THIS RUNS IN LINEAR HDR, before the SSR history copy, before SSAO and long
// before the tonemap — heat haze is a phenomenon in front of the LENS, so the
// bloom and the exposure should see the warped radiance. It is deliberately NOT
// one of the LDR looks.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sceneTexture;
vulkan_layout( ogre_t1 ) uniform texture2D distortionTexture;

vulkan( layout( ogre_s0 ) uniform sampler sceneSampler );
vulkan( layout( ogre_s1 ) uniform sampler fieldSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x = the world's global strength (PostFxDesc::distortionStrength).
	// The safe value is 0 = no warp, so a frame rendered before
	// chain::updateDistortion has run looks like the feature is off.
	uniform vec4 distortionParams;
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const vec4 field = texture( vkSampler2D( distortionTexture, fieldSampler ), inPs.uv0 );

	// rg: [0,1] -> [-1,1]; a: the emitter's own strength, already multiplied in
	// by the alpha blend that wrote it. 0.05 is the maximum displacement in UV
	// at full strength — the sample's own scale, and about a twentieth of the
	// frame, which is a lot of haze.
	const vec2 offset = ( field.rg - vec2( 0.5 ) ) * 2.0 * field.a *
						( distortionParams.x * 0.05 );

	// Three fetches at 80 / 90 / 100 % for the chromatic fringe.
	fragColour = vec4(
		texture( vkSampler2D( sceneTexture, sceneSampler ),
				 clamp( inPs.uv0 + offset * 0.80, vec2( 0.0 ), vec2( 1.0 ) ) ).r,
		texture( vkSampler2D( sceneTexture, sceneSampler ),
				 clamp( inPs.uv0 + offset * 0.90, vec2( 0.0 ), vec2( 1.0 ) ) ).g,
		texture( vkSampler2D( sceneTexture, sceneSampler ),
				 clamp( inPs.uv0 + offset, vec2( 0.0 ), vec2( 1.0 ) ) ).b,
		1.0 );
}
