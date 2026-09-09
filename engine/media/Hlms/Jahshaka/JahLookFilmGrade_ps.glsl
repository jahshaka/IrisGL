// LOOK: FILM GRADE (POST_LOOKS_SPEC.md §2, and CAMERA_LENS_SPEC.md §6's
// unbuilt "Film pass" P5, which this program absorbs).
//
// The four controls a colourist reaches for first, in the order they belong in:
// saturation, contrast, tint, vignette. There is no upstream reference for this
// one — the sample folder has no grade — and it is the cheapest look in the
// catalogue: no taps beyond its own pixel and one distance.
//
// It is deliberately NOT a LUT. A colour lookup table is the general form and
// it needs an asset pipeline, a file format and a picker; these four numbers
// are what a scene actually asks for, they serialize as four floats, and they
// are scrubbable in a panel row. LUTs are out of scope by §0.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 lookParams0;	// x = amount, y = saturation, z = contrast, w = vignette
	uniform vec4 lookParams1;	// xyz = tint
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const ivec2 texSize = textureSize( vkSampler2D( sourceTexture, srcSampler ), 0 );
	const ivec2 srcCoord = clamp( ivec2( inPs.uv0 * vec2( texSize ) ),
								  ivec2( 0 ), texSize - ivec2( 1 ) );
	const vec4  src = texelFetch( vkSampler2D( sourceTexture, srcSampler ), srcCoord, 0 );

	const float amount     = clamp( lookParams0.x, 0.0, 1.0 );
	const float saturation = max( lookParams0.y, 0.0 );
	const float contrast   = max( lookParams0.z, 0.0 );
	const float vignette   = clamp( lookParams0.w, 0.0, 1.0 );
	const vec3  tint       = max( lookParams1.rgb, vec3( 0.0 ) );

	vec3 c = src.rgb;

	// Saturation about the pixel's own luma (Rec.601, the same weights the
	// Desaturate look uses — one definition of grey in this catalogue).
	const float luma = dot( c, vec3( 0.3, 0.59, 0.11 ) );
	c = mix( vec3( luma ), c, saturation );

	// Contrast about mid grey. 0.5 and not 0.18: this runs on the TONEMAPPED,
	// display-referred image, where mid grey is the middle of the range.
	c = ( c - vec3( 0.5 ) ) * contrast + vec3( 0.5 );

	c *= tint;

	// Vignette: a quadratic falloff from the centre, normalised so the corner
	// is exactly the full strength and the centre is untouched.
	const float d = distance( inPs.uv0, vec2( 0.5 ) ) * 1.41421356;
	c *= mix( 1.0, 1.0 - d * d, vignette );

	fragColour = vec4( mix( src.rgb, clamp( c, vec3( 0.0 ), vec3( 1.0 ) ), amount ), src.a );
}
