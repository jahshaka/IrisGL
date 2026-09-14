// Jahshaka SSR, pass 2 of 2: THE RESOLVE.
//
// Turns the ray march's hit coordinates into the texture HlmsPbs samples as
// `ssrTexture`: rgb = the reflected radiance, w = confidence. Upstream's pixel
// shader then does the composite for us —
//     envColourS = lerp( envColourS, ssrReflection.rgb, ssrReflection.w )
// (Hlms/Pbs/Any/Main/800.PixelShader_piece_ps.any, `hlms_use_ssr`, with
// ogre-patch 0036 making that lerp the only spelling) — so w is literally "how
// much of the probe/sky answer does the screen replace", and a zero here is
// EXACTLY today's picture. That is what makes the roughness cutoff and the edge
// fades safe: every one of them just hands the pixel back to the IBL cube.
//
// AND ON A MIRROR, w IS 0 OR 1 (lane SSR-2 — the rule lives at the end of
// main()). "How much does the screen replace" is a fraction only where the
// surface's own lobe is wide enough to make the two sources one blurred
// answer. Below a roughness threshold derived from the probe's blur it is a
// VERDICT, because the probe's image and the screen's are the same object in
// two PLACES there — parallax-corrected onto a box against the true position —
// and a fraction of two places is two images. The confidence terms below all
// still run; they decide VALID vs NONE instead of scaling a blend.
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
	// THE MIRROR MASK RIDES THE SAME NINE TAPS (lane SSR-2; the rule itself is
	// the block below the roughness ramp). It is a COVERAGE: how much of this
	// pixel's neighbourhood is a hit the march TRUSTS and that is looking at the
	// same thing the reference tap is. Two details, both deliberate:
	//
	//  * THE WEIGHTS ARE A TENT over the pixel's position INSIDE the ray texel,
	//    not a box. The nine taps are fetched at integer ray coordinates, so a
	//    box-filtered count is constant across a whole ray texel and its ramp is
	//    a staircase two texels wide with one step — at Half-Res that is a 2 px
	//    hard edge. Weighting each tap by its distance to the pixel's true
	//    (fractional) position instead makes the coverage a continuous function
	//    of the SCREEN pixel, so the mask's boundary is a smooth ramp ~1.5 ray
	//    texels wide — 3 screen pixels on the Half-Res row, 1.5 on Full-Res —
	//    for no extra fetch. That ramp IS the feather: the hit region's edge is
	//    dilated by it, rather than the confidence being lerped.
	//  * THE DENOMINATOR IS ALL NINE TAPS, misses included, for the same reason
	//    the coherence count's is: a single lucky ray surrounded by misses
	//    agrees with itself, and that degenerate case is the artefact.
	const float kMirrorTrust = 0.5;			// "more likely than not", per ray
	const vec2	tentF	 = inPs.uv0 * rayBufferRes.xy - ( vec2( rayCoord ) + 0.5 );
	float		covHit	 = 0.0;
	float		covAll	 = 0.0;
	int			nTrust	 = 0;			// rays the march TRUSTS (the mirror rule's "valid")
	int			nTrustAgree = 0;		// ...of those, the ones looking at one thing
	vec2  sumUv	 = vec2( 0.0 );
	float sumUvW = 0.0;
	int	  nHit	 = 0;				// rays that came back at all
	int	  nAgree = 0;				// ...of those, the ones looking at one thing
	for( int i = 0; i < 9; ++i )
	{
		const vec2	tentD = vec2( float( i % 3 - 1 ), float( i / 3 - 1 ) ) - tentF;
		const float tentW = max( 0.0, 1.5 - abs( tentD.x ) ) * max( 0.0, 1.5 - abs( tentD.y ) );
		covAll += tentW;
		if( taps[i].w <= 0.0 )
			continue;
		++nHit;
		const bool trusted = taps[i].w >= kMirrorTrust;
		if( trusted )
		{
			covHit += tentW;
			++nTrust;
		}
		const vec2 d = abs( taps[i].xy - refUv ) * rayBufferRes.xy;
		if( max( d.x, d.y ) > kCoordSpreadTexels )
			continue;
		++nAgree;
		if( trusted )
			++nTrustAgree;
		sumUv  += taps[i].xy * taps[i].w;
		sumUvW += taps[i].w;
	}
	// NOTE ON THE HALF-RESOLUTION ROW: where the neighbourhood agrees (which is
	// everywhere except a silhouette) every tap passes, the sum is the same sum
	// in the same order as before this fix, and the frame is bit-identical. The
	// picture only moves where the old mean was inventing a coordinate.
	const vec2 hitUv = sumUvW > 0.0 ? sumUv / sumUvW : refUv;
	// .z = the ENVELOPE (distance, screen edge, away-from-camera), averaged over
	// the taps that hit and weighted by how much each is trusted; .w = the mean
	// TRUST over the nine rays fired. Their PRODUCT is sum( z_i * w_i ) / 9 —
	// the same sum, in the same order, as before lane SSR-2 moved two of the
	// march's fades from the w channel to the z channel, which is why the blend
	// path below is arithmetically untouched.
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
	// THE MEASURE IS A COUNT, and it has to be. The obvious spelling —
	// `sumUvW / 9`, the agreeing taps' CONFIDENCE over the neighbourhood — is
	// not a coherence measure at all: on a perfectly coherent floor every tap
	// agrees, so it evaluates to the mean confidence and the weight becomes
	// w * smoothstep(0.35, 0.7, w), which re-shapes every march-side ramp (edge,
	// camera, arrival, thickness) and drives their tails to zero at w = 0.35
	// instead of at 0. Measured: it cost 2.9 points of the flat floor's
	// footprint, all of it real reflection inside those ramps (round 2). Counting
	// instead asks the question that was meant — HOW MANY of the nine rays are
	// looking at one thing — and is exactly orthogonal to how confident they are.
	//
	// The denominator is the NINE RAYS FIRED, not the ones that came back: a
	// single lucky ray surrounded by eight misses agrees with itself, and that
	// degenerate case IS the artefact.
	//
	// WHAT IT MEANS IN ONE SENTENCE, because it is a design decision and not a
	// tuning constant: SSR is off wherever the reflected image is magnified by
	// more than about four ray-buffer texels per pixel — which is what
	// `kCoordSpreadTexels` measures — because a screen-space trace samples that
	// image at one sample per pixel and cannot describe it any finer.
	//
	// THE RAMP IS DELIBERATELY LOW (full confidence from 55 % of the
	// neighbourhood agreeing) because a legitimate reflection edge — the
	// silhouette of the thing being reflected — has a disagreeing neighbourhood
	// by construction and must not vanish. A flat floor scores 1.0 everywhere
	// except across such an edge, which is why the flat-floor frame does not
	// move.
	const float agreement = float( nAgree ) * ( 1.0 / 9.0 );
	const float cohFade	  = smoothstep( 0.35, 0.7, agreement );

	// AND THE BORROW HAS TO EARN IT. When this pixel's OWN ray missed, the block
	// above hands it the most confident NEIGHBOUR's hit. That is a sound
	// interpolation inside a coherent reflection (it is what stops the
	// half-resolution ray buffer from checkerboarding) and pure invention when
	// the neighbourhood is mostly misses: one lucky ray in nine then paints a
	// dot on eight pixels that never hit anything. So a borrowed hit fades with
	// HOW MANY neighbours stand behind it — a count again, for the same reason.
	const float borrow = taps[4].w > 0.0 ? 1.0 : smoothstep( 2.0, 5.0, float( nHit ) );

	// Full-resolution roughness, undoing HlmsPbs' prepass packing. The ramp
	// below the cutoff is what stops the reflection from appearing and
	// vanishing as a hard boundary across a floor whose roughness varies.
	const float roughness =
		texture( vkSampler2D( gBufShadowRoughness, pointSampler ), inPs.uv0 ).y * 0.98 + 0.02;
	const float cutoff	  = resolveParams.x;
	const float roughFade = 1.0 - smoothstep( cutoff * 0.5, cutoff, roughness );

	// ---- THE RULE ON A MIRROR (lane SSR-2, the owner's dual image) ----------
	//
	// THE MEASUREMENT. On the Mirror Room's chrome sphere the confidence terms
	// above leave a field of PARTIAL weights, and upstream's composite lerps the
	// screen's answer over the probe's by it. The two answers are not two
	// samples of one thing there: the probe is parallax-corrected onto the
	// room's box and the trace is at the true position, so they are the same
	// object drawn in two places — and a lerp of two places is both of them, at
	// a weight that changes from pixel to pixel because the counts and the
	// margins do. That is the stippled ghost the rig photographed over the
	// smooth probe image.
	//
	// THE RULE. Below `kMirrorRoughLo` a VALID hit WINS OUTRIGHT and the probe
	// fills only where there is none. The confidence still decides VALID vs
	// NONE — the arrival angle, the thickness margin, the agreement and the
	// quorum all still reject back-faces, thin-object leaks, undersampled fans
	// and lone rays, through `covHit` — it just no longer SCALES the blend.
	// What survives as a fraction is the ENVELOPE (`ray.z`: the distance fade,
	// the screen-edge ramp, the away-from-the-camera ramp) and the roughness
	// ramp, because those are not doubts about the hit, they are the places
	// where the technique runs out of data and the probe must take over without
	// a seam. The mask's own boundary is feathered by the tent above.
	//
	// WHERE THE THRESHOLD COMES FROM, since it is the one number here that is
	// not a decision but a measurement. The two sources disagree by an angle
	// Dtheta in the reflected direction (the probe's box intersection is not the
	// real hit point, and its capture is not this frame). The probe's answer is
	// PREFILTERED: a GGX lobe of perceptual roughness r has alpha = r^2 and a
	// reflected-lobe half-width of about 2*alpha, because a normal perturbed by
	// an angle turns the reflected ray by twice it. The screen's answer carries
	// no roughness blur at all in this v1 — it is one sharp ray per pixel at
	// every roughness below the cutoff. So the two images are DISTINGUISHABLE
	// exactly while the probe's blur is narrower than the disagreement,
	//
	//     2 * r^2  <  Dtheta       ->      r  <  sqrt( Dtheta / 2 )
	//
	// and only above that is the probe's copy a wash the sharp copy merely sits
	// on, which is the one case a lerp is honest in.
	//
	// MEASURED (lane SSR-2, the Mirror Room at the owner's pose, 1600x900; the
	// pictures are in spikes/ssr-2/): the two sources were rendered ALONE — the
	// probe's by switching SSR off, the screen's by forcing the rule's mask to
	// 1 wherever the march has a trusted hit — and compared over the chrome
	// sphere. Where the trace is well sampled (the sphere's face-on middle, the
	// reflected teapot) they AGREE: best cross-correlation shift (0,0), rmse
	// 2.6 of 255. Where they disagree they do not disagree by a shift at all —
	// they show DIFFERENT OBJECTS (at the left limb the probe answers with the
	// blue wall and the screen with the west wall, the gold torus and a red
	// patch), and those two directions are of order 90 degrees apart as seen
	// from the sphere. Taking a deliberately conservative 30 degrees (0.52 rad)
	// for the disagreement puts the crossover at r = sqrt(0.52/2) = 0.51, so
	// the ramp below straddles it. That is ABOVE `ssrRoughnessCutoff`'s default
	// of 0.35, which is the honest conclusion and worth saying plainly: at the
	// shipped cutoff there is NO roughness at which this renderer draws a
	// screen-space reflection and the probe's blur is wide enough to hide the
	// disagreement — so the rule covers every surface SSR draws on, and the
	// lerp survives for a user who raises the cutoff past the band.
	//
	// THE ONE THING THIS DOES NOT FIX, said here because the constant looks
	// like it should: between about 0.2 and the cutoff the screen's image is
	// too SHARP for the surface (v1 has no roughness-varying blur). Winning
	// outright does not make that worse — the old lerp drew the same sharp
	// image, with a second copy behind it — but a roughness-aware blur on the
	// resolve is the follow-up that would.
	const float kMirrorRoughLo = 0.40;
	const float kMirrorRoughHi = 0.55;
	// THE MASK IS THREE FACTORS, AND WHICH QUESTION EACH ANSWERS IS THE WHOLE
	// DESIGN — the first round of this lane got it wrong by asking one question
	// with one number, and paid for it on BOTH sides (the sphere's dither
	// survived at a loose threshold; a flat floor's reflection lost 8 % of its
	// mass at a tight one, all of it the legitimate silhouette of the thing
	// being reflected).
	//
	//  1. COVERAGE — how much of this pixel's neighbourhood is a trusted hit.
	//     This is the ANTI-ALIASED HIT MASK and it is what the blend path's
	//     `ray.w` already was: 1 inside a reflection, 0 outside it, a ramp
	//     across the boundary. It stays a fraction, because a hit mask's edge
	//     is a coverage and not a doubt — it is the feather the rule needs, now
	//     tent-weighted so it is smooth in SCREEN pixels rather than a
	//     staircase in ray texels.
	//  2. THE QUORUM — a lone trusted ray with no neighbours behind it is the
	//     degenerate case the blend path's `borrow` term exists for, and the
	//     tent gives a lone centre tap a third of the coverage, which is a
	//     visible dot. Same counts, same ramp as `borrow`.
	//  3. THE SAMPLING VERDICT — of the rays that DID come back, how many are
	//     looking at one thing. This is the fan test, and the denominator is
	//     what makes it one: measured against the nine rays FIRED it cannot
	//     tell a fan from the edge of a reflected object (both leave about half
	//     the neighbourhood disagreeing, which is why the strict version ate
	//     the flat floor's silhouettes), while measured against the rays that
	//     RETURNED it separates them cleanly — at a silhouette the returning
	//     rays are one cluster and the ratio is ~1, in a fan they are nine
	//     unrelated places and it collapses. This is the term that becomes a
	//     VERDICT on a mirror: the screen may overrule the probe only where the
	//     reflected image is sampled well enough to be described at one sample
	//     per pixel.
	const float kFanLo = 0.55;
	const float kFanHi = 0.85;
	const float mirrorCov = covAll > 0.0 ? covHit / covAll : 0.0;
	const float quorum	  = smoothstep( 1.5, 4.0, float( nTrust ) );
	const float fanGate	  = nTrust > 0
								? smoothstep( kFanLo, kFanHi,
											  float( nTrustAgree ) / float( nTrust ) )
								: 0.0;
	const float mirrorW	  = ray.z * roughFade * mirrorCov * quorum * fanGate;
	const float blendW	   = ray.w * ray.z * roughFade * cohFade * borrow;
	const float mirrorFade = 1.0 - smoothstep( kMirrorRoughLo, kMirrorRoughHi, roughness );
	// A FLAT MIRROR IS THE SAME FRAME EITHER WAY, by construction and not by
	// tuning: every tap trusts (face-on arrival, well inside the thickness),
	// every tap agrees, so mirrorCov is 1 and mirrorMask, cohFade, borrow and
	// ray.w are all 1 — and both branches evaluate to ray.z * roughFade.
	const float weight =
		clamp( mix( blendW, mirrorW, mirrorFade ) * resolveParams.y, 0.0, 1.0 );
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
