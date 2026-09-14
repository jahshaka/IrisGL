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
//
// THE FINITE GUARD (SMOKE-ENGINE-1 item 1, 2026-09-14). Everything above
// assumes the history holds a radiance. It is a CLOSED LOOP and nothing in it
// guaranteed that: scene colour -> the kSsrPrev history -> `prevFrame` here ->
// jahSsrReflection -> HlmsPbs' envColourS -> scene colour, with the RELATIVE
// firefly clamp below as its only defence. A relative clamp cannot bound a
// runaway (every frame is measured against the previous one, so a gain above 1
// is invisible to it), the history is RGBA16_FLOAT so 65504 is +Inf, and the
// clamp's own `reflected *= ceiling / lum` is Inf * 0 == NaN the moment lum is
// Inf. A NaN tonemaps to BLACK and circulates for the life of the workspace —
// the owner's "black holes in the textures when I move the sun" — and the HDR
// luminance reduction averages it into a 1x1 keep_content history that never
// recovers, which is the same defect's "all white, stuck exposure".
//
// So every value read out of `prevFrame` goes through ssrHistoryTap() first:
// non-finite becomes black and says so, and an absolute ceiling bounds what the
// loop can circulate. The ceiling is ABSOLUTE on purpose — it is the only
// thing a feedback loop cannot argue with. The relative firefly clamp stays
// exactly as it was; a pixel already inside the ceiling comes out of this
// shader bit for bit unchanged, which is why no existing frame moves.
// CONFIDENCE, AND WHAT IT IS FOR (lane SSR-1, 2026-09-14). Two of the fades this
// shader multiplies into `weight` were added because of the owner's Mirror Room:
// a chrome sphere reflected the teapot as green shards and dots, while the same
// sphere with SSR switched off showed a clean probe reflection. Both are
// statements about whether nine neighbouring rays agree — see the block comment
// above each. The two others live in the march (the arrival angle at the
// surface a ray hit, and how marginal the thickness test's crossing was), and
// the whole point of all four is that where a screen-space trace cannot be
// trusted the pixel must fall back to the probe or sky the surface already has,
// which a confidence below 1 does for free through upstream's lerp.

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

// THE LOOP'S CEILING, in linear radiance. Mid-grey is 0.18 and the half-float
// history saturates at 65504, so 1024 is about twelve stops above white and six
// below the format's own ceiling: far above anything a scene legitimately
// reflects, far below the overflow that turns the history into +Inf. A
// reflection clamped here is still white after any exposure the tonemapper
// arrives at, so the ceiling costs no picture and buys a bounded loop.
const float kSsrMaxRadiance = 1024.0;

/// One history tap, with its own verdict attached: .xyz is a radiance this
/// shader can do arithmetic on, and .w is 1 only when the texel really was
/// finite and inside the ceiling.
///
/// WHAT IS OUT THERE, and why .w has to exist. The scene target is
/// RGBA16_FLOAT, and a punctual light's specular lobe on a near-mirror surface
/// is very nearly a delta — the GGX D term goes as 1 / (pi * alpha^2), so a
/// roughness clamped at 1e-4 reaches 1e7 before anything else is applied. That
/// texel is stored as +Inf, and it is a COLOURED Inf when the light is
/// coloured: the warm spot overflows red a stop before it overflows blue. A
/// clamp — per channel or hue-preserving, it makes no difference — turns such a
/// texel into a saturated primary at the ceiling, which is the red/green/blue
/// confetti this shader used to scatter over the Shadow Maps port's floor. So
/// the clamp is only ever a SAFE INTERMEDIATE: the caller reads .w and uses the
/// neighbourhood instead, because a single sample of a value the buffer could
/// not represent carries no information about the lobe, and the neighbourhood
/// does.
vec4 ssrHistoryTap( vec2 uv )
{
	const vec3 c = texture( vkSampler2D( prevFrame, linearSampler ), uv ).xyz;
	// PER-CHANNEL, ORDERED COMPARISONS, AND NOT max(). GLSL and SPIR-V leave
	// `max` with a NaN operand UNDEFINED, and the hardware instruction it maps
	// to is a maxNum on the platforms we ship (Metal/MoltenVK documents it,
	// NVIDIA's FMNMX does it): maxNum returns the NON-NaN operand, so
	// max(abs(NaN), max(abs(0), abs(0))) is 0 and a single-channel NaN reads as
	// finite and in range. That is not hypothetical here — it is this shader's
	// own mechanism, since a COLOURED overflow reaches (Inf, 0, 0) and the
	// arithmetic below turns it into (NaN, 0, 0).
	//
	// lessThan/lessThanEqual are ORDERED (OpFOrdLessThan): every comparison is
	// FALSE for a NaN, so `all(...)` is false for a NaN in any channel and
	// false for an Inf, and a value that passes both really is finite and in
	// range. A pixel that passes comes out untouched, bit for bit.
	if( all( lessThanEqual( abs( c ), vec3( kSsrMaxRadiance ) ) ) )
		return vec4( c, 1.0 );							// the ordinary texel, untouched
	if( all( lessThan( abs( c ), vec3( 3.0e38 ) ) ) )
		// clamp, not min: a channel BELOW -kSsrMaxRadiance must come back too.
		return vec4( clamp( c, vec3( -kSsrMaxRadiance ), vec3( kSsrMaxRadiance ) ), 0.0 );
	// NOT A COLOUR AT ALL, and it contributes NOTHING — neither to this pixel
	// (.w says so, and the caller takes the neighbourhood instead) nor to the
	// neighbourhood mean. MEASURED, because the alternative is tempting and
	// wrong: developing a non-finite tap as the CEILING instead — "it was too
	// bright to store, so call it white" — removes the same black holes but
	// scatters white sparkle where they were, and on the Shadow Maps port that
	// is 20.5 % of the viewport against 3.3 % for this line (SMOKE-ENGINE-1,
	// same drag, same frame). A value nobody can read is worth zero, and the
	// four taps that ARE readable carry the pixel.
	return vec4( 0.0, 0.0, 0.0, 0.0 );
}

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

	// COHERENCE: DO THE NINE RAYS AGREE? (lane SSR-1, the Mirror Room's shredded
	// chrome sphere.)
	//
	// Everything above is written for a neighbourhood that agrees — a flat floor,
	// where nine neighbouring rays leave in nine nearly identical directions and
	// land nine nearly identical places. On a CURVED mirror they do not: the
	// sphere's normal turns under every pixel, the rays fan out, and the nine
	// hits are nine unrelated places in the frame. Each pixel then paints
	// whatever its own ray happened to graze, and the result is the shredded
	// green confetti the owner photographed where the sphere should have shown
	// the teapot.
	//
	// The measure is free, because the loop above already computed it: `sumUvW`
	// is the weight of the taps that AGREE with the reference hit. Measured
	// against the WHOLE NEIGHBOURHOOD — nine, not `sumW` — and deliberately:
	// against sumW a single lucky ray surrounded by eight misses scores a
	// perfect 1.0, because it agrees with itself. That degenerate case IS the
	// artefact (one ray in nine painting a dot), so the denominator has to be
	// the nine rays that were fired, not the ones that came back.
	//
	// THE RAMP IS DELIBERATELY LOW (full confidence from 55 % of the
	// neighbourhood agreeing) because a legitimate reflection edge — the
	// silhouette of the thing being reflected — has a disagreeing neighbourhood
	// by construction and must not vanish. A flat floor scores 1.0 everywhere
	// except across such an edge, which is why the flat-floor frame does not
	// move.
	const float agreement = sumUvW * ( 1.0 / 9.0 );
	const float cohFade	  = smoothstep( 0.35, 0.7, agreement );

	// AND THE BORROW HAS TO EARN IT. When this pixel's OWN ray missed, the block
	// above hands it the most confident NEIGHBOUR's hit. That is a sound
	// interpolation inside a coherent reflection (it is what stops the
	// half-resolution ray buffer from checkerboarding) and pure invention when
	// the neighbourhood is mostly misses: one lucky ray in nine then paints a
	// dot on eight pixels that never hit anything. So a borrowed hit fades with
	// how much of the neighbourhood stands behind it.
	const float borrow = taps[4].w > 0.0 ? 1.0 : smoothstep( 2.0, 5.0, sumW );

	// Full-resolution roughness, undoing HlmsPbs' prepass packing. The ramp
	// below the cutoff is what stops the reflection from appearing and
	// vanishing as a hard boundary across a floor whose roughness varies.
	const float roughness =
		texture( vkSampler2D( gBufShadowRoughness, pointSampler ), inPs.uv0 ).y * 0.98 + 0.02;
	const float cutoff	  = resolveParams.x;
	const float roughFade = 1.0 - smoothstep( cutoff * 0.5, cutoff, roughness );

	const float weight =
		clamp( ray.w * ray.z * roughFade * cohFade * borrow * resolveParams.y, 0.0, 1.0 );
	if( weight <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	const vec4 centreTap = ssrHistoryTap( ray.xy );
	vec3	   reflected  = centreTap.xyz;

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
		// Each tap goes through ssrHistoryTap on its OWN, not after the
		// average: one +Inf neighbour must not drag the other three to zero and
		// collapse the ceiling this pixel is measured against.
		const vec3	nbr = ( ssrHistoryTap( ray.xy + vec2( -t.x, -t.y ) ).xyz +
							ssrHistoryTap( ray.xy + vec2( t.x, -t.y ) ).xyz +
							ssrHistoryTap( ray.xy + vec2( -t.x, t.y ) ).xyz +
							ssrHistoryTap( ray.xy + vec2( t.x, t.y ) ).xyz ) *
						  0.25;
		// THE UNREPRESENTABLE SAMPLE TAKES THE NEIGHBOURHOOD, not a clamped
		// version of itself (see ssrHistoryTap): the lobe stand-in the firefly
		// clamp already computes is the best answer available for a texel the
		// history could not hold, and it is bounded by construction, since
		// every tap that went into it is. A pixel whose centre tap WAS in range
		// skips this line entirely and comes out of the shader exactly as it
		// did before the guard existed.
		if( centreTap.w < 1.0 )
			reflected = nbr;
		const float lum		= dot( reflected, kLum );
		const float ceiling = max( dot( nbr, kLum ) * kFireflyRatio, kFireflyFloor );
		if( lum > ceiling )
			reflected *= ceiling / lum;
	}

	fragColour = vec4( reflected, weight );
}
