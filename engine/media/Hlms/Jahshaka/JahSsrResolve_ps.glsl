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
// AND w IS A VERDICT, NOT A FRACTION (lane SSR-2 — the rule lives at the end
// of main()). The probe's image and the screen's are the same objects in two
// PLACES — parallax-corrected onto a box, captured from a grid point, against
// the true position this frame — and a fraction of two places is two images,
// which is what the owner photographed on the Mirror Room's chrome sphere. So
// a valid hit wins outright and the probe fills only where there is none. The
// confidence terms below all still run; they decide VALID vs NONE instead of
// scaling a blend. The fractions that remain are the ENVELOPE (where the
// technique runs out of reach), the ROUGHNESS ramp and the mask's own
// COVERAGE — a hit region's antialiased edge — and never a doubt about a hit.
// There is no lerp branch: a lerp would only be honest above the roughness at
// which the probe's own blur hides its parallax error, and this renderer stops
// drawing screen-space reflections well below it (the derivation is with the
// rule).
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
	uniform vec4 resolveParams;		// x roughness cutoff, y intensity,
									// z the cutoff's feather, w unused
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
	//  * THE TRUST TEST IS A RAMP, NOT A STEP, and that is a defect this lane
	//    shipped in its first round. `taps[i].w` is the product of two SMOOTH
	//    fields (the arrival angle and the thickness margin); admitting a tap
	//    at exactly 0.5 draws the ISO-LINE of that field into the picture — the
	//    coverage jumps by one ninth wherever the field crosses the threshold,
	//    which on the ssr.engine fixture came out as three horizontal contour
	//    stripes across the reflected cube's underside, inside a real reflected
	//    surface. A tap's contribution to the coverage therefore ramps over
	//    0.35..0.65 of its own trust. The DECISION the rule makes is still a
	//    decision — a tap below 0.35 contributes nothing at all, and the mask
	//    is still a coverage and not a confidence — it is only the admission
	//    that is continuous, which is what turns a contour back into the short
	//    ramp the underlying field actually has. The COUNTS (`nTrust`,
	//    `nTrustAgree`) stay integer at the same midpoint: they are a quorum
	//    and a ratio, and half a ray is not a ray.
	const float kMirrorTrust   = 0.5;		// "more likely than not", per ray
	const float kMirrorTrustLo = 0.35;		// ...and the ramp the coverage admits it over
	const float kMirrorTrustHi = 0.65;
	const vec2	tentF	 = inPs.uv0 * rayBufferRes.xy - ( vec2( rayCoord ) + 0.5 );
	float		covHit	 = 0.0;
	float		covAll	 = 0.0;
	int			nTrust	 = 0;			// rays the march TRUSTS (the mirror rule's "valid")
	int			nTrustAgree = 0;		// ...of those, the ones looking at one thing
	vec2  sumUv	 = vec2( 0.0 );
	float sumUvW = 0.0;
	for( int i = 0; i < 9; ++i )
	{
		const vec2	tentD = vec2( float( i % 3 - 1 ), float( i / 3 - 1 ) ) - tentF;
		const float tentW = max( 0.0, 1.5 - abs( tentD.x ) ) * max( 0.0, 1.5 - abs( tentD.y ) );
		covAll += tentW;
		if( taps[i].w <= 0.0 )
			continue;
		const bool trusted = taps[i].w >= kMirrorTrust;
		covHit += tentW * smoothstep( kMirrorTrustLo, kMirrorTrustHi, taps[i].w );
		if( trusted )
			++nTrust;
		const vec2 d = abs( taps[i].xy - refUv ) * rayBufferRes.xy;
		if( max( d.x, d.y ) > kCoordSpreadTexels )
			continue;
		if( trusted )
			++nTrustAgree;
		sumUv  += taps[i].xy * taps[i].w;
		sumUvW += taps[i].w;
	}
	// NOTE ON THE HALF-RESOLUTION ROW: where the neighbourhood agrees (which is
	// everywhere except a silhouette) every tap passes, the sum is the same sum
	// in the same order as before the firefly fix, and the frame is
	// bit-identical. The picture only moves where the old mean was inventing a
	// coordinate.
	//
	// AND ONE THING LANE SSR-2's REPACK DID CHANGE HERE, which "the product is
	// unchanged" does NOT cover (second reader, round 2): `w > 0` now means a
	// TRUSTED hit rather than a trusted hit with a non-zero envelope, so a hit
	// whose envelope has gone to zero — the last-step ring at travelled >=
	// maxDistance, a reflection pointing back at the camera past towardEye 0.85
	// — is a HIT to everything downstream: it can be the reference coordinate,
	// it enters the coordinate mean, and it counts towards the quorum and the
	// sampling verdict (it counted towards the coherence and borrow terms too,
	// while those existed). That is the more
	// correct reading: those rays did find geometry, and it is the ENVELOPE
	// that is refusing to draw it, not the hit that is in doubt. It cannot
	// brighten anything either way — every path out of here is multiplied by
	// that same zero envelope — and the fixture's reflection interior is
	// byte-identical across the repack, which is the evidence.
	const vec2 hitUv = sumUvW > 0.0 ? sumUv / sumUvW : refUv;
	// .z = the ENVELOPE (distance, screen edge, away-from-camera), averaged over
	// the taps that hit and weighted by how much each is trusted; .w = the mean
	// TRUST over the nine rays fired. Their PRODUCT is sum( z_i * w_i ) / 9 —
	// the same five factors per tap as before lane SSR-2 moved two of the
	// march's fades from the w channel to the z channel. Not bit-identical,
	// because the ray buffer is RGBA16_UNORM and the two channels are now
	// quantised at different places in the product (about 2 parts in 65535 per
	// term); the evidence that it does not reach a picture is the fixture's
	// byte-identical reflection interior.
	const vec4 ray	 = vec4( hitUv, sumFade / sumW, sumW * ( 1.0 / 9.0 ) );

	// Full-resolution roughness, undoing HlmsPbs' prepass packing. The ramp
	// below the cutoff is what stops the reflection from appearing and
	// vanishing as a hard boundary across a floor whose roughness varies.
	//
	// THE DECODE IS THE MARCH'S (lane SSR-3, and the same defect was here): the
	// channel is the GGX ALPHA packed by ogre-patch 0043 over [0.001, 1], so the
	// perceptual roughness the cutoff is stated in is its square root. Read with
	// the pre-0043 range and no square root, this ramp ran over perceptual
	// 0.399 -> 0.581 for a cutoff of 0.35.
	//
	// AND THE RAMP IS THE FEATHER THE TRACED HALF USES (lane SSR-3 round 2, the
	// lead's call on the measurement). It was `cutoff/2 -> cutoff`, a width that
	// was never chosen -- it was half of a number in the WRONG UNIT -- and read
	// in perceptual roughness it took half the screen reflection off a satin
	// surface at 0.30 under the default cutoff of 0.40 (measured, tests/ssr
	// section 13: red excess 0.847 -> 0.424). It is now
	// `cutoff - feather -> cutoff` with the feather the ray tier fades over,
	// `kRayReflectFeather` = 0.1, arriving in resolveParams.z from
	// EnginePrivate.h through OgreChain::updateSsr. A surface more than a
	// feather below the cutoff keeps its reflection WHOLE, one authored inside
	// the feather crossfades to the probe, and the screen and the ray now hand
	// over on one number instead of two.
	//
	// The ray fades to zero at `cutoff + feather` and this fades to zero AT the
	// cutoff, and that asymmetry is not an oversight: a ray may be spent above
	// the cutoff and its answer crossfaded, while the march is SKIPPED there
	// (JahSsrRayMarch_ps.glsl returns before it starts), so above the cutoff
	// there is no screen answer to fade.
	const float alpha = max(
		texture( vkSampler2D( gBufShadowRoughness, pointSampler ), inPs.uv0 ).y * 0.999 + 0.001,
		1e-6 );
	const float roughness = sqrt( alpha );
	const float cutoff	  = resolveParams.x;
	const float feather	  = resolveParams.z;
	const float roughFade =
		1.0 - smoothstep( max( cutoff - feather, 0.0 ), cutoff, roughness );

	// ---- THE RULE ON A MIRROR (lane SSR-2, the owner's dual image) ----------
	//
	// THE DEFECT. On the Mirror Room's chrome sphere the confidence terms left a
	// field of PARTIAL weights and upstream's composite lerped the screen's
	// answer over the probe's by it. The two answers are not two samples of one
	// thing there: the probe is parallax-corrected onto the room's box and
	// captured from a grid point, the trace is at the true position and from
	// this frame, so they are the same objects drawn in two PLACES. A lerp of
	// two places is both of them, at a weight that changes from pixel to pixel
	// because the counts and the margins do — the stippled ghost the rig
	// photographed over the smooth probe image.
	//
	// THE RULE, and it is the ONLY rule this shader has below the cutoff: a
	// VALID hit WINS OUTRIGHT and the probe fills only where there is none. The
	// confidence decides VALID vs NONE — the arrival angle, the thickness
	// margin, the sampling verdict and the quorum still reject back-faces,
	// thin-object leaks, undersampled fans and lone rays — it never SCALES the
	// composite. What remains a fraction is the ENVELOPE (`ray.z`: distance,
	// screen edge, away-from-the-camera), the roughness ramp and the mask's own
	// COVERAGE, because none of those is a doubt about the hit: two are where
	// the technique runs out of data and the probe must take over without a
	// seam, and the third is a hit region's antialiased edge.
	//
	// WHY THERE IS NO LERP BRANCH LEFT (round 2, and it is a CRUD deletion).
	// A lerp is honest only where the probe's own blur is wide enough to make
	// the two answers one: the probe's cube is prefiltered, so a GGX lobe of
	// perceptual roughness r (alpha = r^2) has a reflected-lobe half-width of
	// about 2*alpha — a normal perturbed by an angle turns the reflected ray by
	// twice it — while the screen's answer carries NO roughness blur at all in
	// this v1. The two are therefore distinguishable exactly while
	//
	//     2 * r^2  <  Dtheta       ->      r  <  sqrt( Dtheta / 2 )
	//
	// where Dtheta is not a renderer constant but THE PROBE'S OWN PARALLAX
	// ERROR AT THIS PIXEL: the angle between the direction the surface really
	// reflects and the direction the probe was asked for, which grows with how
	// far the probe stands from the shading point and shrinks with how far away
	// the reflected object is. Measured on the Mirror Room's sphere (spikes/
	// ssr-2/): where the trace is well sampled the two AGREE (cross-correlation
	// shift (0,0), rmse 2.6 of 255 on the reflected teapot); where they disagree
	// they are not a shifted copy at all but DIFFERENT OBJECTS — the probe's
	// blue wall against the screen's west wall, gold torus and red patch at the
	// limb — tens of degrees apart. At a deliberately conservative 30 degrees
	// (0.52 rad) the crossover is r = 0.51, and the cutoff is the World panel's
	// "Roughness Cutoff" row, in PERCEPTUAL roughness, default 0.40 (lane SSR-3
	// wired it; it used to be a renderer constant nothing in the document could
	// write, and a constant the shader then compared against an un-square-rooted
	// alpha, so the band the frame applied was perceptual 0.581). At the default
	// the lerp's honest range therefore still begins above the roughness at
	// which this renderer stops drawing a screen-space reflection at all.
	//
	// WHAT IS NEW SINCE SSR-3, stated honestly: the row reaches 100 %, so a
	// project CAN now set a cutoff above that crossover, which the old constant
	// could not. The answer is still not a lerp branch — the screen's image is a
	// sharp mirror image at EVERY roughness, so a raised cutoff paints a sharp
	// reflection on a rough surface whether it wins or blends, and lerping two
	// answers that sit in two PLACES is the dither this rule exists to remove.
	// The fix for a raised cutoff is a roughness-aware blur on the resolve, and
	// it is a different piece of work.
	//
	// THE MASK IS THREE FACTORS, AND WHICH QUESTION EACH ANSWERS IS THE WHOLE
	// DESIGN — the first round of this lane got it wrong by asking one question
	// with one number, and paid for it on BOTH sides (the sphere's dither
	// survived at a loose threshold; a flat floor's reflection lost 8 % of its
	// mass at a tight one, all of it the legitimate silhouette of the thing
	// being reflected).
	//
	//  1. COVERAGE — how much of this pixel's neighbourhood is a trusted hit.
	//     This is the ANTI-ALIASED HIT MASK: 1 inside a reflection, 0 outside
	//     it, a ramp across the boundary. It stays a fraction, because a hit
	//     mask's edge is a coverage and not a doubt — it is the feather the
	//     rule needs, tent-weighted so it is smooth in SCREEN pixels rather
	//     than a staircase in ray texels.
	//     ITS RAMP STAYS A RAMP, measured (round 2). A HARD quorum (nTrust >= 3
	//     or nothing) was tried because a count that is 0.07 at two rays and
	//     0.43 at three is itself a fraction born of doubt: it removed the
	//     half-resolution row's faint marginal bands but DOUBLED the
	//     full-resolution row's (the same bands then passed at full quorum),
	//     and the flat floor's moved-pixel count only fell from 958 to 784 for
	//     it. The soft ramp is the better of the two measured answers.
	//  2. THE QUORUM — a lone trusted ray with no neighbours behind it paints a
	//     dot on a neighbourhood that never hit anything, and the tent gives a
	//     lone centre tap a third of the coverage. Same counts and the same
	//     ramp as the borrow term this replaced.
	//  3. THE SAMPLING VERDICT — of the TRUSTED rays, how many are looking at
	//     one thing. This is the fan test, and the denominator is what makes it
	//     one: measured against the nine rays FIRED it cannot tell a fan from
	//     the edge of a reflected object (both leave about half the
	//     neighbourhood disagreeing, which is why the strict version ate the
	//     flat floor's silhouettes), while measured against the rays that came
	//     back and are trusted it separates them cleanly — at a silhouette
	//     those rays are one cluster and the ratio is ~1, in a fan they are
	//     unrelated places and it collapses. This is the term that makes the
	//     rule a VERDICT: the screen may overrule the probe only where the
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
	// A FLAT MIRROR IS THE FRAME IT WAS, by construction and not by tuning:
	// every tap trusts (face-on arrival, well inside the thickness) and every
	// tap agrees, so mirrorCov, quorum and fanGate are all 1 and the weight is
	// the envelope times the roughness ramp — which is what the old
	// ray.w * ray.z * roughFade * cohFade * borrow evaluated to there.
	const float weight =
		clamp( ray.z * roughFade * mirrorCov * quorum * fanGate * resolveParams.y, 0.0, 1.0 );
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
