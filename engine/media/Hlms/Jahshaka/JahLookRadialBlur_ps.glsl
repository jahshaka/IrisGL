// LOOK: RADIAL BLUR (POST_LOOKS_SPEC.md §2 row "Radial Blur").
//
// A zoom blur about a point: seven taps along the ray from the centre through
// this pixel, averaged. Upstream's RadialBlur_ps.glsl is the reference,
// including its distance attenuation — the blur fades in past a minimum radius
// so the middle of the frame stays sharp. What it does not have is an AMOUNT:
// its tap multipliers are compile-time constants, so the sample's blur is
// exactly one strength for ever.
//
// Here the SPREAD is the amount: at amount 1 the taps are upstream's, at 0.25 a
// quarter of the way out, at 0 all seven coincide. The output is then
// mix(src, blurred, amount * attenuation) so the identity at 0 is exact — the
// coinciding taps would already very nearly give it, and "very nearly" is not
// the contract.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount, yz = centre in UV, w = falloff exponent
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

#define NUM_SAMPLES 7

void main()
{
	const ivec2 texSize = textureSize( vkSampler2D( sourceTexture, srcSampler ), 0 );
	const ivec2 srcCoord = clamp( ivec2( inPs.uv0 * vec2( texSize ) ),
								  ivec2( 0 ), texSize - ivec2( 1 ) );
	const vec4  src = texelFetch( vkSampler2D( sourceTexture, srcSampler ), srcCoord, 0 );

	const float amount   = clamp( lookParams0.x, 0.0, 1.0 );
	const vec2  centre   = lookParams0.yz;
	const float exponent = max( lookParams0.w, 0.0 );

	// Upstream's seven multipliers. Scaled TOWARDS 1 by the amount, so the
	// spread is the knob and the shape of the falloff is theirs.
	const float multipliers[NUM_SAMPLES] =
		float[NUM_SAMPLES]( 1.00, 0.99, 0.98, 0.97, 0.96, 0.94, 0.93 );

	// The sample's attenuation, with its min distance and falloff written out:
	// the blur starts a fifth of the frame from the centre and reaches full
	// strength at the corner, so the subject stays sharp.
	float atten = ( distance( inPs.uv0, centre ) - 0.2 ) * ( 1.0 / 0.55 );
	atten = clamp( atten, 0.0, 1.0 );
	atten = pow( atten, exponent );

	vec3 acc = vec3( 0.0 );
	for( int i = 0; i < NUM_SAMPLES; ++i )
	{
		const float m = mix( 1.0, multipliers[i], amount );
		acc += texture( vkSampler2D( sourceTexture, srcSampler ),
						( inPs.uv0 - centre ) * m + centre ).rgb;
	}
	const vec3 blurred = acc * ( 1.0 / float( NUM_SAMPLES ) );

	fragColour = vec4( mix( src.rgb, blurred, amount * atten ), src.a );
}
