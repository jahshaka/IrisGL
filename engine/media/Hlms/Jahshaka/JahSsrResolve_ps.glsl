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
//
// THE FIREFLY FIX (lane-whitedots, 2026-09-09). What the 3x3 above must NOT do
// is average the hit COORDINATES across a silhouette. Two neighbouring rays on
// either side of an edge hit two unrelated places; their mean is a third place
// that neither ray ever touched, and one bright texel there is a lone white dot
// that follows the camera around — the "random white dots on the Showroom
// floor". It is a FULL-RESOLUTION symptom because at half resolution the nine
// taps come from four times fewer, far more coherent rays; the bug was always
// there, only sub-visible. The rule now is: never average coordinates that
// disagree. The confidence and distance-fade averages — the anti-checkerboard
// half of the filter the header above describes — are untouched, so a
// neighbourhood that agrees renders bit-for-bit what it rendered before.
// A luminance clamp catches the rest (a valid coordinate that lands on one
// texel of a highlight is a firefly too), and it too is a no-op on any pixel
// that is not one.
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
	uniform vec4 prevFrameRes;		// auto texture_size 2: xy = the colour history's pixels
	uniform vec4 resolveParams;		// x roughness cutoff, y intensity, zw unused
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

void main()
{
	const ivec2 rayCoord = ivec2( inPs.uv0 * rayBufferRes.xy );
	const ivec2 rayMax	 = ivec2( rayBufferRes.xy ) - ivec2( 1 );

	// Confidence-weighted 3x3 gather (see the header). sumW is the mask, and
	// the coordinate comes out of the second loop below.
	vec4  taps[9];
	int	  n		  = 0;
	float sumFade = 0.0;
	float sumW	  = 0.0;
	for( int dy = -1; dy <= 1; ++dy )
	{
		for( int dx = -1; dx <= 1; ++dx )
		{
			const ivec2 c = clamp( rayCoord + ivec2( dx, dy ), ivec2( 0 ), rayMax );
			const vec4	r = texelFetch( vkSampler2D( rayTexture, pointSampler ), c, 0 );
			taps[n++] = r;
			sumFade += r.z * r.w;
			sumW	+= r.w;
		}
	}
	if( sumW <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	// THE REFERENCE COORDINATE. The centre tap is this pixel's OWN ray, so
	// when it hit anything its hit is by definition the right answer for this
	// pixel; only when it missed does a neighbour stand in, and then the most
	// confident one does.
	vec2  refUv = taps[4].xy;
	if( taps[4].w <= 0.0 )
	{
		float bestW = 0.0;
		for( int i = 0; i < 9; ++i )
		{
			if( taps[i].w > bestW )
			{
				bestW = taps[i].w;
				refUv = taps[i].xy;
			}
		}
	}

	// ...and the mean over the taps that AGREE with it. A tap whose hit is far
	// from the reference hit something else — the two sides of a silhouette —
	// and its coordinate is not a smaller or larger version of this pixel's
	// answer, it is a different answer. Averaging the two lands between them,
	// on a pixel neither ray ever touched, and if that pixel is bright the
	// result is a lone white dot that moves with the camera. The threshold is
	// a heuristic and its failure modes are both benign: too tight and the
	// coordinate falls back to the centre tap's own hit, which is always
	// geometrically correct for this pixel and merely less smooth; too loose
	// and the luminance clamp further down is the second line of defence.
	const float kCoordSpreadTexels = 4.0;
	vec2  sumUv	 = vec2( 0.0 );
	float sumUvW = 0.0;
	for( int i = 0; i < 9; ++i )
	{
		if( taps[i].w <= 0.0 )
			continue;
		const vec2 d = abs( taps[i].xy - refUv ) * rayBufferRes.xy;
		if( max( d.x, d.y ) > kCoordSpreadTexels )
			continue;
		sumUv  += taps[i].xy * taps[i].w;
		sumUvW += taps[i].w;
	}
	// NOTE ON THE HALF-RESOLUTION ROW: where the neighbourhood agrees (which is
	// everywhere except a silhouette) every tap passes, the sum is the same sum
	// in the same order as before this fix, and the frame is bit-identical. The
	// picture only moves where the old mean was inventing a coordinate.
	const vec2 hitUv = sumUvW > 0.0 ? sumUv / sumUvW : refUv;
	const vec4 ray	 = vec4( hitUv, sumFade / sumW, sumW * ( 1.0 / 9.0 ) );

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

	vec3 reflected = texture( vkSampler2D( prevFrame, linearSampler ), ray.xy ).xyz;

	// THE FIREFLY CLAMP, the second line of defence and the only one that also
	// covers a coordinate that is perfectly valid and simply unlucky — a ray
	// that lands on the one texel of a specular highlight, or on a pixel the
	// previous frame left bright and this one did not. A reflection is an
	// integral over a lobe; a single texel of it is not, and one sample of a
	// high-variance signal is exactly how a firefly is born.
	//
	// The neighbourhood is four bilinear taps at 1.5 prevFrame texels, which is
	// a cheap stand-in for the lobe the resolve does not have the budget to
	// integrate. Anything brighter than kFireflyRatio times that mean is pulled
	// back to the ceiling — RELATIVE, so a bright scene keeps bright
	// reflections, plus an absolute floor so a reflection surrounded by black
	// (a lamp against a night sky) is not crushed to nothing.
	//
	// It is a CONDITIONAL, so a pixel that is not a firefly comes out of this
	// shader bit-for-bit unchanged, at either resolution row.
	{
		const vec3	kLum		  = vec3( 0.2126, 0.7152, 0.0722 );
		const float kFireflyRatio = 4.0;
		const float kFireflyFloor = 1.0;	// linear HDR: "as bright as white"
		const vec2	t			  = 1.5 / prevFrameRes.xy;
		const vec3	nbr =
			( texture( vkSampler2D( prevFrame, linearSampler ), ray.xy + vec2( -t.x, -t.y ) ).xyz +
			  texture( vkSampler2D( prevFrame, linearSampler ), ray.xy + vec2( t.x, -t.y ) ).xyz +
			  texture( vkSampler2D( prevFrame, linearSampler ), ray.xy + vec2( -t.x, t.y ) ).xyz +
			  texture( vkSampler2D( prevFrame, linearSampler ), ray.xy + vec2( t.x, t.y ) ).xyz ) *
			0.25;
		const float lum		= dot( reflected, kLum );
		const float ceiling = max( dot( nbr, kLum ) * kFireflyRatio, kFireflyFloor );
		if( lum > ceiling )
			reflected *= ceiling / lum;
	}

	fragColour = vec4( reflected, weight );
}
