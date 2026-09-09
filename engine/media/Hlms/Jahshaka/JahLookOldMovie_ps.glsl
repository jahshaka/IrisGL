// LOOK: OLD MOVIE (POST_LOOKS_SPEC.md §2 row "Old Movie").
//
// Sepia, dirt, gate flicker and frame jitter — the four things that say
// "projected film" — all driven by time. Upstream's OldMovie_ps.glsl is the
// reference for the RECIPE and for nothing else: it reaches for four textures,
// two of them 1D (8x8PagesSplotches2.png, 1D_Noise.png, Sepia1D.tga), from
// Ogre's 1.x media folder that this engine does not stage and that we would
// then own a copy of. Every one of them is replaced here by arithmetic:
//
//   * the sepia ramp is a tint of the pixel's own luma, so it needs no LUT;
//   * the dirt is a value-noise hash of the texel's cell and the frame number;
//   * the flicker and the jitter are the same hash of the frame number alone.
//
// DETERMINISM. The animation rides Ogre's `time_0_x` auto-parameter, which
// under the engine's fixed frame delta advances by a known amount per frame —
// so a gate that steps frames sees the same picture every run. The suite does
// not rely on that: its assertions are STRUCTURAL (sepia means R > G > B), which
// hold at every phase of the flicker. That is the R3 rule in the spec, and it
// is the difference between a test and a tripwire.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D sourceTexture;

vulkan( layout( ogre_s0 ) uniform sampler srcSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4  lookParams0;	// x = amount, y = flicker, z = dirt, w = jitter
	uniform float lookTime;		// auto time_0_x: seconds, cycling
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

float hash11( float n )
{
	return fract( sin( n * 12.9898 ) * 43758.5453 );
}

float hash21( vec2 p )
{
	return fract( sin( dot( p, vec2( 12.9898, 78.233 ) ) ) * 43758.5453 );
}

void main()
{
	const ivec2 texSize = textureSize( vkSampler2D( sourceTexture, srcSampler ), 0 );
	const ivec2 srcCoord = clamp( ivec2( inPs.uv0 * vec2( texSize ) ),
								  ivec2( 0 ), texSize - ivec2( 1 ) );
	const vec4  src = texelFetch( vkSampler2D( sourceTexture, srcSampler ), srcCoord, 0 );

	const float amount  = clamp( lookParams0.x, 0.0, 1.0 );
	const float flicker = clamp( lookParams0.y, 0.0, 1.0 );
	const float dirt    = clamp( lookParams0.z, 0.0, 1.0 );
	const float jitter  = clamp( lookParams0.w, 0.0, 1.0 );

	// THE FRAME NUMBER, at 24 fps — film's own cadence, and the reason the
	// jitter reads as a projector rather than as a wobble: it must hold still
	// for a whole frame and then move.
	const float frame = floor( lookTime * 24.0 );

	// The gate weave: the whole image slides vertically by up to half a percent.
	const vec2 juv = inPs.uv0 + vec2( 0.0, ( hash11( frame ) - 0.5 ) * 0.01 * jitter );
	const vec3 base = texture( vkSampler2D( sourceTexture, srcSampler ),
							   clamp( juv, vec2( 0.0 ), vec2( 1.0 ) ) ).rgb;

	// Sepia: the pixel's luma through a warm tint. The three multipliers ARE the
	// look — anything with r > g > b reads as aged film — and they are
	// normalised so a white pixel stays at full brightness rather than clipping.
	const float lumi = dot( base, vec3( 0.3, 0.59, 0.11 ) );
	vec3 toned = vec3( lumi ) * vec3( 1.15, 0.98, 0.72 );

	// Dirt: sparse dark splotches and brighter specks, re-drawn every frame.
	const float cell = hash21( floor( juv * 96.0 ) + vec2( frame * 7.13, frame * 3.71 ) );
	const float splotch = 1.0 - dirt * 0.55 * step( 0.985, cell );
	const float speck   = dirt * 0.35 * step( 0.9975, hash21( floor( juv * 220.0 ) -
															  vec2( frame * 1.7 ) ) );

	// The gate flicker: exposure wandering between 60% and 100%, at a rate that
	// is not a multiple of the frame rate so it never locks to the dirt.
	const float darken = mix( 1.0, 0.6 + 0.4 * abs( fract( lookTime * 9.0 ) - 0.5 ) * 2.0,
							  flicker );

	const vec3 effect = clamp( toned * splotch * darken + vec3( speck ),
							   vec3( 0.0 ), vec3( 1.0 ) );
	fragColour = vec4( mix( src.rgb, effect, amount ), src.a );
}
