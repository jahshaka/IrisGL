// Jahshaka SSR, pass 2 of 2: THE RESOLVE.
//
// Turns the ray march's hit coordinates into the texture HlmsPbs samples as
// `ssrTexture`: rgb = the reflected radiance, w = confidence. Upstream's pixel
// shader then does the composite for us —
//     envColourS = lerp( envColourS, ssrReflection.rgb, ssrReflection.w )
// (Hlms/Pbs/Any/Main/800.PixelShader_piece_ps.any, `hlms_use_ssr`) — so w is
// literally "how much of the probe/sky answer does the screen replace", and a
// zero here is EXACTLY today's picture. That is what makes the roughness cutoff
// and the edge fades safe: every one of them just hands the pixel back to the
// IBL cube.
//
// THE COLOUR IS THE PREVIOUS FRAME'S, AND IT HAS TO BE. HlmsPbs consumes the
// reflection while it shades, so the reflection must exist BEFORE the colour
// pass that would produce it — there is no ordering in which this frame's
// colour is available. That is the one-frame lag every prepass-architecture SSR
// carries, including Ogre's own sample's. Ours is the plainer version of it: no
// reprojection matrix, so a moving CAMERA smears the reflection by a frame
// rather than rejecting the disoccluded texels. Reflections of moving OBJECTS
// are still per-frame, because the hit coordinates come from this frame's depth
// and normals — which is the whole point of the technique and the thing no
// baked probe can do at any cadence.
//
// WHY THE RAY BUFFER IS POINT-FETCHED, NINE TIMES. At half resolution it holds
// texture COORDINATES, not a colour: hardware bilinear across a hit/miss
// boundary invents a coordinate that is neither, and the invented one usually
// lands somewhere bright. So the taps are integer fetches — but a 3x3 of them,
// combined with the CONFIDENCE as the weight, which is a filter that only
// averages coordinates that actually hit something.
//
// The 3x3 is not decoration, it is the other half of the march's jitter. The
// march offsets alternate pixels by half a step so a fixed step cannot band;
// the price is that at the far edge of a reflection — where a ray either just
// catches the object or just overshoots it — neighbouring pixels disagree and
// the boundary comes out as a CHECKERBOARD. Averaging the confidence over the
// neighbourhood turns that back into a ramp. It also hides most of the
// half-resolution blockiness, which is why v1 needs no separate blur pass.
// (Measured on the suite's fixture: the dither along the reflection's leading
// edge is visible in a 256x256 readback without this and gone with it.)
//
// The ROUGHNESS ramp stays at FULL resolution, deliberately: it is a property
// of the receiving surface, not of the rays, and blurring it would smear the
// cutoff across the boundary between a polished and a matte material.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D rayTexture;
vulkan_layout( ogre_t1 ) uniform texture2D gBufShadowRoughness;
vulkan_layout( ogre_t2 ) uniform texture2D prevFrame;

vulkan( layout( ogre_s0 ) uniform sampler pointSampler );
vulkan( layout( ogre_s2 ) uniform sampler linearSampler );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 rayBufferRes;		// auto texture_size 0: xy = the ray buffer's pixels
	uniform vec4 resolveParams;		// x roughness cutoff, y intensity, zw unused
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const ivec2 rayCoord = ivec2( inPs.uv0 * rayBufferRes.xy );
	const ivec2 rayMax	 = ivec2( rayBufferRes.xy ) - ivec2( 1 );

	// Confidence-weighted 3x3 gather (see the header). sumW is the mask, the
	// weighted mean of the hit coordinates is where to look.
	vec2  sumUv	  = vec2( 0.0 );
	float sumFade = 0.0;
	float sumW	  = 0.0;
	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			const ivec2 c = clamp( rayCoord + ivec2( dx, dy ), ivec2( 0 ), rayMax );
			const vec4	r = texelFetch( vkSampler2D( rayTexture, pointSampler ), c, 0 );
			sumUv	+= r.xy * r.w;
			sumFade += r.z * r.w;
			sumW	+= r.w;
		}
	}
	if( sumW <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}
	const vec4 ray = vec4( sumUv / sumW, sumFade / sumW, sumW * ( 1.0 / 9.0 ) );

	// Full-resolution roughness, undoing HlmsPbs' prepass packing. The ramp
	// below the cutoff is what stops the reflection from appearing and
	// vanishing as a hard boundary across a floor whose roughness varies.
	const float roughness =
		texture( vkSampler2D( gBufShadowRoughness, pointSampler ), inPs.uv0 ).y * 0.98 + 0.02;
	const float cutoff	  = resolveParams.x;
	const float roughFade = 1.0 - smoothstep( cutoff * 0.5, cutoff, roughness );

	const float weight = clamp( ray.w * ray.z * roughFade * resolveParams.y, 0.0, 1.0 );
	if( weight <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	const vec3 reflected = texture( vkSampler2D( prevFrame, linearSampler ), ray.xy ).xyz;
	fragColour = vec4( reflected, weight );
}
