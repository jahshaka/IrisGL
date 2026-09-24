// Jahshaka SSR, pass 1 of 2: THE RAY MARCH.
//
// Screen-space reflections, POST_CHAIN_SPEC.md §4.1 row "SSR" and §8 phase 6.
// This is ours rather than a staged copy of
// Ogre's sample (Samples/Media/2.0/scripts/materials/ScreenSpaceReflections):
// the sample's marcher is the McGuire/Mara DDA plus a frame-to-frame
// reprojection and a compute + mipmap colour history, and it mutates its own
// compositor node definition from C++ to switch resolution — none of which
// composes with a graph this backend ASSEMBLES (OgreChain.cpp §3). What is NOT
// ours is the thing that matters: the composite. The reflection this pass
// eventually produces is handed to HlmsPbs as the pass' `ssrTexture`, and
// upstream's own shader lerps it into the specular env term
// (Hlms/Pbs/Any/Main/800.PixelShader_piece_ps.any: `hlms_use_ssr` →
// `envColourS = lerp( envColourS, ssrReflection.rgb, ssrReflection.w )`),
// BEFORE planar reflections and ambient. We never re-implement that.
//
// CONVENTIONS, all three load-bearing and all three inherited from the pin:
//
//  * VIEW SPACE IS LEFT-HANDED HERE. `inPs.cameraDir` arrives from
//    CompositorPassQuadDef::VIEW_SPACE_CORNERS_NORMALIZED_LH, i.e. the far
//    corner divided by the far plane, so its z is always +1 and
//    `cameraDir * linearDepth` is the view-space position with z GROWING away
//    from the eye. `viewToTextureSpaceMatrix` (built in OgreChain.cpp's
//    updateSsr, verbatim from Ogre's own ScreenSpaceReflections::update) has
//    the right-handed→left-handed column flip baked in, so it expects points in
//    exactly this space and returns TEXTURE space directly — xy/w is already
//    [0,1] with y down, no *0.5+0.5 anywhere in this file.
//  * The G-buffer normal is right-handed (HlmsPbs writes `pixelData.normal`
//    straight out), so its z is negated on the way in. Same line, same reason,
//    as the sample's.
//  * THE PROJECTION IS NOT ASSUMED PERSPECTIVE (orthoview lane). Everything
//    above describes a perspective frustum, where the interpolated corner is a
//    RAY and depth is a reciprocal. Under an ORTHOGRAPHIC camera neither is
//    true: the corner is a constant view-space offset and depth is linear in
//    distance. `orthoParams.x` selects between the two, and the two places it
//    matters are the depth helper and the origin. Without the branch the ray
//    origin collapses and the whole reflection slides with the camera in a
//    top/front/side view — see the block comments at each site.
//  * DEPTH LINEARIZATION IS REVERSE-Z SAFE. `projectionParams` comes from
//    Camera::getProjectionParamsAB(), which branches on
//    RenderSystem::isReverseDepth() (OgreFrustum.cpp:108) — Vulkan's default
//    here is reverse, far == 0. `B / (d - A)` is metres either way, and the
//    "nothing was rendered here" test therefore has to reject BOTH extremes,
//    exactly like ogre-patch 0011 taught the SSAO shader to.
//
// OUTPUT (PFG_RGBA16_UNORM, so every channel must be [0,1]):
//    xy = the texture-space coordinate the ray hit
//    z  = the ENVELOPE: how gracefully the technique has to stop here. The
//         distance fade (1 at the origin falling to 0 at maxDistance), the
//         SCREEN EDGE ramp, the "this reflection points back at the camera"
//         ramp, and SSR-EDGE-1's two: the END OF THE RAY'S RANGE (the last
//         steps before it would leave the screen, pass the eye or run out of
//         distance) and the THICKNESS MARGIN (how much of the gap between the
//         ray and the surface the march's own step cannot explain), multiplied.
//         None of them is a doubt about the hit: they are the places where a
//         screen-space trace runs out of data and the probe has to take over
//         without a seam.
//    w  = the TRUST: 0 for a miss, otherwise the ARRIVAL ANGLE at the surface
//         the ray hit (a backface or a grazing arrival is a hit the depth
//         buffer cannot vouch for) — lane SSR-1's; see the block comment where
//         it is computed.
//
// THE SPLIT IS LANE SSR-2's AND IT CARRIES A DECISION (the owner's dual image
// on the Mirror Room's chrome sphere). The four fades used to be one product in
// w, which forced the resolve to treat them all as one number: "what fraction
// of the probe's answer does the screen replace". On a MIRROR that fraction is
// not a physical quantity — the lobe is a delta, the screen either answered it
// or it did not, and a fraction paints the screen's image over the probe's
// misregistered one at partial strength, which is two images and reads as a
// dither. The ENVELOPE terms may still be fractions there (they are the ramps
// that keep the frame's edge from being a moving hard line), the TRUST terms
// must become a verdict. Nothing about the arithmetic of the old weight changes
// — the resolve still multiplies z by w, in the same order, so the terms'
// product is the number it always was; what changes is that the resolve can now
// ASK THE TWO QUESTIONS SEPARATELY.
// The ROUGHNESS mask is deliberately NOT folded in here: this pass may run at
// half resolution, and a roughness cutoff evaluated at half res has visibly
// blocky edges. The resolve pass re-reads roughness at FULL resolution. The
// only thing roughness does here is skip the march entirely for a surface that
// could never show a sharp reflection — a pure cost saving with no visual term.
// THE CUTOFF ITSELF IS THE PROJECT'S (lane SSR-3): `rayParams.w` is the World
// panel's "Roughness Cutoff" row, in PERCEPTUAL roughness, the same one number
// the ray-traced half gates on — below it the reflection is marched here and,
// on a ray-capable machine, traced for whatever the screen cannot see; above it
// the probe's own prefiltered photograph answers. See where it is decoded.
#version ogre_glsl_ver_330

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
vulkan_layout( ogre_t1 ) uniform texture2D gBufNormals;
vulkan_layout( ogre_t2 ) uniform texture2D gBufShadowRoughness;

vulkan( layout( ogre_s0 ) uniform sampler samplerState );

vulkan_layout( location = 0 )
in block
{
	vec2 uv0;
	vec3 cameraDir;
} inPs;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec2 projectionParams;			// A, B — Camera::getProjectionParamsAB()
	uniform vec4 rayParams;					// x maxDistance, y thickness, z steps, w roughness cutoff
	uniform vec4 rayBufferRes;				// auto viewport_size: xy = this target's pixels
	uniform mat4 viewToTextureSpaceMatrix;
	// x = 1 for an ORTHOGRAPHIC camera, 0 otherwise; y = the far plane, which
	// is the scale that turns the normalized corner back into view-space xy.
	// chain::updateSsr pushes both every frame; see the ORTHOGRAPHIC note above.
	uniform vec4 orthoParams;
	// THE MARCH'S PHASE RULE (SSR-RINGS-1). x selects it:
	//   0 = CHECKER, the shipped march exactly (the default): the crossing
	//       test and the trust below both read the COARSE sample, and both
	//       therefore carry the step's own phase.
	//   1 = REFINED: a crossing is a SIGN CHANGE, and the thickness test and
	//       the trust are evaluated at the REFINED crossing.
	//   2 = DITHER: the SHIPPED hit rule with the two-value checkerboard
	//       offset replaced by a 4x4 ordered (Bayer) phase — the minimal
	//       candidate, which changes what a ray SAMPLES and nothing about
	//       what counts as a hit.
	// A uniform and not a property, for the reason orthoParams is one: it is
	// uniform-coherent over the quad, and a permutation would recompile the
	// marcher every time a project changed the row.
	uniform vec4 marchParams;
vulkan( }; )

vulkan_layout( location = 0 )
out vec4 fragColour;

// VIEW-SPACE DISTANCE FROM THE DEPTH BUFFER, both projections.
//
// PERSPECTIVE is the reciprocal form documented at the top of this file:
// `B / (d - A)` is metres with and without reverse Z.
//
// ORTHOGRAPHIC needs its own line because an ortho depth buffer is LINEAR in
// distance, and `Frustum::getProjectionParamsAB()` returns a different pair
// entirely for it (OgreFrustum.cpp:128-141): B is -1/(far-near) and the same
// expression evaluates to 1/t, the RECIPROCAL of the distance — and to MINUS
// 1/t without reverse depth. Inverting it recovers t, and the absolute value is
// what makes the line reverse-Z safe without the shader having to know which
// convention the render system chose. No new uniform is needed for the depth:
// the pair the chain already pushes carries it, once it is read the right way
// round.
float jahViewDistance( float d )
{
	if( orthoParams.x > 0.5 )
		return abs( ( d - projectionParams.x ) / projectionParams.y );
	return projectionParams.y / ( d - projectionParams.x );
}

float jahSceneDepthAt( vec2 uv )
{
	return texture( vkSampler2D( depthTexture, samplerState ), uv ).x;
}

// THE RAY'S OWN RANGE (SSR-EDGE-1): how far along `rayDir` the march can follow
// the ray before it stops being able to — past `maxDistance`, behind the eye,
// or off the screen — solved EXACTLY on the ray's clip-space line rather than
// found by stepping. `viewToTextureSpaceMatrix` returns texture space, so a
// point is on screen while h.x, h.w - h.x, h.y and h.w - h.y are all >= 0 and
// h.w > 0; each is LINEAR in t along the ray (h = h0 + h1 t), so each leaves
// its half-space at one t, and the range is the nearest of them.
float jahRayRange( vec3 origin, vec3 rayDir, float maxDistance )
{
	float range = maxDistance;
	if( rayDir.z < 0.0 )
		range = min( range, -origin.z / rayDir.z );			// the eye's plane
	const vec4 h0 = viewToTextureSpaceMatrix * vec4( origin, 1.0 );
	const vec4 h1 = viewToTextureSpaceMatrix * vec4( rayDir, 0.0 );
	const vec4 f0 = vec4( h0.x, h0.w - h0.x, h0.y, h0.w - h0.y );
	const vec4 f1 = vec4( h1.x, h1.w - h1.x, h1.y, h1.w - h1.y );
	for( int k = 0; k < 4; ++k )
	{
		if( f1[k] < 0.0 )
			range = min( range, -f0[k] / f1[k] );
	}
	if( h1.w < 0.0 )
		range = min( range, -h0.w / h1.w );
	return max( range, 0.0 );
}

void main()
{
	const float rawDepth = jahSceneDepthAt( inPs.uv0 );
	// Cleared depth at either extreme = no geometry (the sky quad writes colour,
	// never depth). Reject both, so this is correct with and without reverse Z.
	if( rawDepth <= 0.0 || rawDepth >= 1.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	// WHAT THE G-BUFFER'S .y ACTUALLY IS, and reading it wrong was worth half
	// again the cutoff the user asked for (lane SSR-3; the finding is the R5
	// readers', ledger 432/440). It is the GGX ALPHA and not a perceptual
	// roughness: HlmsPbs runs with `mPerceptualRoughness` true, so the
	// `pixelData.roughness` the prepass packs into this channel is the perceptual
	// value SQUARED, and ogre-patch 0043 packs it over [0.001, 1] — the shader's
	// own alpha floor — not over upstream's old [0.02, 1].
	//
	// This line used the PRE-0043 range and then compared the result to the
	// cutoff as though it were perceptual, so the band the frame actually applied
	// was `0.980981 * alpha + 0.019019 > 0.35`, i.e. alpha 0.3374, i.e.
	// PERCEPTUAL 0.581 — a number nobody chose and nobody could read anywhere.
	//
	// So: undo the range patch 0043 wrote, then take the square root, because the
	// cutoff is stated in PERCEPTUAL roughness. That is the number the material
	// panel shows, the number the World row's "Roughness Cutoff" is in, and the
	// number the ray-traced half of the same reflection gates on
	// (rq_reflect.comp does exactly this decode, for exactly this reason).
	const float alpha = max(
		texture( vkSampler2D( gBufShadowRoughness, samplerState ), inPs.uv0 ).y * 0.999 + 0.001,
		1e-6 );
	const float roughness = sqrt( alpha );
	if( roughness > rayParams.w )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	vec3 normalVS = texture( vkSampler2D( gBufNormals, samplerState ), inPs.uv0 ).xyz * 2.0 - 1.0;
	if( dot( normalVS, normalVS ) < 1e-6 )
	{
		fragColour = vec4( 0.0 );
		return;
	}
	normalVS = normalize( normalVS );
	normalVS.z = -normalVS.z;					// right-handed G-buffer -> left-handed march

	// THE ORIGIN AND THE VIEW VECTOR, and the whole reason this shader knows
	// what a projection is.
	//
	// PERSPECTIVE: `cameraDir` IS a ray (the far corner over the far plane), so
	// scaling it by the distance lands on the surface, and the direction from
	// the eye to that surface is the same vector normalized.
	//
	// ORTHOGRAPHIC: it is NOT a ray. An ortho frustum's far corners have the
	// same xy as its near ones (OgreFrustum.cpp:884 takes ratio = 1 for
	// PT_ORTHOGRAPHIC), so `cameraDir.xy * farPlane` is this pixel's view-space
	// xy AT EVERY DEPTH and must not be scaled by distance — scaling it is
	// exactly the defect this branch fixes: the reconstructed position slid
	// with the camera, so the reflections slid with a pan of an axis view even
	// though the projection had not moved. And every pixel of an ortho frame
	// looks the same way, so the view vector is a constant, not a per-pixel
	// direction. (In this left-handed space +z is away from the eye.)
	vec3 origin;
	vec3 toSurface;
	if( orthoParams.x > 0.5 )
	{
		origin	  = vec3( inPs.cameraDir.xy * orthoParams.y, jahViewDistance( rawDepth ) );
		toSurface = vec3( 0.0, 0.0, 1.0 );
	}
	else
	{
		origin	  = inPs.cameraDir * jahViewDistance( rawDepth );
		toSurface = normalize( origin );
	}
	const vec3 rayDir	 = reflect( toSurface, normalVS );

	const float maxDistance = rayParams.x;
	const float thickness	= rayParams.y;
	const int	steps		= int( rayParams.z );
	const float stepLen		= maxDistance / max( 1.0, rayParams.z );

	// A two-value checkerboard jitter along the ray. Without it a fixed step
	// lands every pixel of a flat floor on the same depth samples and the
	// reflection bands; with it neighbouring pixels sample between each other's
	// steps and the banding reads as noise the confidence fade hides.
	const vec2	pixel  = inPs.uv0 * rayBufferRes.xy;
	float jitter = fract( ( floor( pixel.x ) + floor( pixel.y ) ) * 0.5 );
	// ...OR SIXTEEN PHASES INSTEAD OF TWO (marchParams.x == 2). The two-value
	// checkerboard is visible in the picture as a two-pixel dither along every
	// boundary the march quantises — measured on the Showroom's glossy sphere,
	// the autocorrelation of the reflection's own deficit peaks at 2 px before
	// anything else. A 4x4 ordered matrix spreads that boundary over sixteen
	// phases, which the resolve's 3x3 gather then averages: the arc becomes a
	// ramp rather than an edge, at the price of a grain the checkerboard did not
	// have. Nothing else changes — the phase is still an offset inside ONE step.
	if( marchParams.x > 1.5 )
	{
		const ivec2 ip = ivec2( pixel ) & ivec2( 3 );
		const int bayer[16] = int[16]( 0, 8, 2, 10, 12, 4, 14, 6,
									   3, 11, 1, 9, 15, 7, 13, 5 );
		jitter = float( bayer[ ip.y * 4 + ip.x ] ) * ( 1.0 / 16.0 );
	}

	// THE RAY'S RANGE, found once (SSR-EDGE-1; jahRayRange). The march used to
	// learn it by stepping past it — a sample off the screen, behind the eye or
	// beyond maxDistance ended the ray — so whether a crossing just before the
	// end was FOUND depended on where the sample after it fell: a ray whose
	// crossing sample landed one step late missed, its neighbour with the other
	// checkerboard phase hit, and the end of every reflection was a speckled
	// line of full-weight hits against misses (ssr.edge's spheres: the floor's
	// reflection on the lower hemisphere, where the rays head toward the camera
	// and one 0.26 m step crosses much of the screen). The LAST SAMPLE now sits
	// on the end of the range, just inside it, so a crossing before the end is
	// found whatever the phase, and hit-or-miss is the geometry's answer.
	const float range	 = jahRayRange( origin, rayDir, maxDistance );
	const float rangeEnd = max( range - 1.0e-3 * stepLen, 0.0 );

	float hit		= 0.0;
	float travelled = maxDistance;
	vec2  hitUv		= vec2( 0.0 );
	float prevT		= 0.0;
	float hitDiff	= 0.0;			// the crossing's depth error, as a fraction of the tolerance

	// The loop bound must be a COMPILE-TIME constant for the shader to unroll
	// sanely on every driver; `steps` is the runtime budget inside it.
	for( int i = 1; i <= 128; ++i )
	{
		if( i > steps )
			break;

		const float tStep = stepLen * ( float( i ) + jitter );
		const bool	last  = tStep >= rangeEnd;
		const float t	  = last ? rangeEnd : tStep;
		if( t <= prevT )
			break;								// the range ended before this step began
		const vec3	p = origin + rayDir * t;
		if( p.z <= 0.0 )
			break;								// the ray went behind the eye

		const vec4 h = viewToTextureSpaceMatrix * vec4( p, 1.0 );
		if( h.w <= 0.0 )
			break;
		const vec2 uv = h.xy / h.w;
		if( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 )
			break;								// left the screen: nothing to reflect

		const float sceneRaw = jahSceneDepthAt( uv );
		if( sceneRaw > 0.0 && sceneRaw < 1.0 )
		{
			const float sceneZ = jahViewDistance( sceneRaw );
			const float diff   = p.z - sceneZ;
			// diff > 0: the ray is BEHIND the surface at that pixel, i.e. it
			// crossed it. diff < thickness: the surface is not so far in front
			// that the crossing is a false positive through thin geometry --
			// the depth buffer knows a surface's front, never its back, which
			// is the whole reason SSR needs a thickness guess at all. The
			// step's own length is added so a coarse march cannot straddle a
			// legitimately thin hit.
			// WHICH SAMPLE ANSWERS THE CROSSING TEST (SSR-RINGS-1).
			//
			// CHECKER (the shipped rule) asks the COARSE sample: the ray must
			// land behind the surface by less than the tolerance, so whether a
			// ray is a hit AT ALL depends on where its samples fell. Measured
			// on the Showroom's glossy sphere: the accepted hit region covers
			// 15.3 % of the crop at a 1.04 m step, 34.3 % at 0.13 m, and 58.4 %
			// when the tolerance is widened instead — the nested crescents ARE
			// that region's boundary, drawn hard because the resolve lets a
			// trusted hit win outright.
			//
			// REFINED asks the crossing itself. `diff > 0` is a SIGN CHANGE: the
			// ray was in front of the surface at the previous sample and is
			// behind it at this one, which is a crossing whatever the phase. The
			// refinement below already finds WHERE; the thickness question —
			// did the ray pass close enough for the depth buffer to vouch for
			// it — is then asked there, of a gap the step cannot explain,
			// instead of at a sample up to a whole step past the surface.
			const float crossTol = marchParams.x == 1.0 ? 1.0e9 : thickness + stepLen * 0.5;
			if( diff > 0.0 && diff < crossTol )
			{
				// HOW MARGINAL THE CROSSING WAS, kept for the confidence below:
				// a ray that passed a hair behind the surface really met it; one
				// that only counts because the tolerance is fat may have passed
				// BEHIND the object entirely, through the part of the world the
				// depth buffer cannot describe.
				//
				// THE STEP'S OWN OVERSHOOT IS SUBTRACTED FIRST, and that
				// subtraction is what makes this a measure of the WORLD rather
				// than of the march's phase. `diff` is read at a coarse step, so
				// even a perfectly face-on crossing lands anywhere in
				// (0, stepLen * rayDir.z] depending on where the checkerboard
				// jitter put this pixel's samples — at the High tier's 48 steps
				// that bound is 0.52 m against a 0.5 m thickness, so a
				// legitimate hit would score "marginal" for about a quarter of
				// the jitter phases and the confidence would checkerboard. What
				// is left after the subtraction is the part of the gap the STEP
				// cannot explain, which is the only part the thickness guess is
				// being asked about, and it is normalised by the thickness
				// itself (round 2, lane SSR-1).
				hitDiff = max( 0.0, diff - stepLen * max( rayDir.z, 0.0 ) ) /
						  max( thickness, 1e-6 );
				// Binary refinement between the last miss and this hit. Five
				// iterations take the hit to 1/32 of a step, which is what
				// stops the reflection from looking quantised along the ray.
				float lo = prevT;
				float hi = t;
				vec2  refined = uv;
				// The depth error AT the best bound found so far. It starts at
				// the coarse sample's, so a refinement that bails out early —
				// off screen, or onto a pixel with no depth — falls back to
				// exactly the number the shipped rule would have used.
				float refinedDiff = diff;
				for( int k = 0; k < 5; ++k )
				{
					const float mid = ( lo + hi ) * 0.5;
					const vec3	q	= origin + rayDir * mid;
					const vec4	hq	= viewToTextureSpaceMatrix * vec4( q, 1.0 );
					// THE VALIDITY GUARDS THE COARSE MARCH HAS AND THIS LOOP DID NOT,
					// which is how a refinement could WORSEN a good hit:
					//  * hq.w <= 0 is a point behind the eye, and hq.xy / hq.w is then
					//    a plausible-looking coordinate on the wrong side of the
					//    projection.
					//  * a midpoint that projects OFF SCREEN, or onto a pixel the
					//    depth buffer never wrote (the sky, at either reverse-Z
					//    extreme), has no depth to compare against: jahLinearDepth of
					//    a cleared depth is not a distance, and `q.z > qz` then reads
					//    as a crossing about half the time. Near a silhouette that is
					//    exactly the bad case — the refinement walks the hit off the
					//    object it found and onto the background, and the resolve
					//    fetches the previous frame there.
					// Both bail out with `refined` still at the last COARSE hit, which
					// is always a real one: the only thing lost is the sub-step
					// polish, and the only thing gained is that the reflection points
					// at something the ray actually met.
					if( hq.w <= 0.0 )
						break;
					const vec2	quv = hq.xy / hq.w;
					if( quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0 )
						break;
					const float qRaw = jahSceneDepthAt( quv );
					if( qRaw <= 0.0 || qRaw >= 1.0 )
					{
						lo = mid;					// nothing there: not a crossing
						continue;
					}
					const float qz	= jahViewDistance( qRaw );
					if( q.z > qz )
					{
						hi = mid;
						refined = quv;
						refinedDiff = q.z - qz;
					}
					else
					{
						lo = mid;
					}
				}
				// THE THICKNESS TEST, AT THE CROSSING (marchParams.x >= 1).
				// Five bisections put `hi` within 1/32 of a step of the true
				// crossing, so `refinedDiff` is the part of the gap that is the
				// WORLD's and not the march's — the only part the thickness
				// guess was ever about. A crossing that fails it is a ray that
				// passed BEHIND the object, through the part of the world no
				// depth buffer describes, and the march goes on looking, exactly
				// as it does today when the coarse tolerance rejects one.
				if( marchParams.x == 1.0 )
				{
					if( refinedDiff >= thickness )
					{
						prevT = t;
						continue;
					}
					hitDiff = refinedDiff / max( thickness, 1e-6 );
				}
				hitUv	  = refined;
				travelled = hi;
				hit		  = 1.0;
				break;
			}
		}
		prevT = t;
		if( last )
			break;								// the sample on the end of the range was the last
	}

	if( hit <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	// The two fades every screen-space reflection needs and cannot avoid:
	//  * SCREEN EDGE. The information simply stops at the border of the frame;
	//    without the ramp a mirror floor gets a hard line where its reflection
	//    is cut off, and the line MOVES with the camera, which is worse than
	//    having no reflection.
	//  * BACK AT THE CAMERA. A ray whose direction is roughly the reverse of
	//    the view vector is asking about geometry behind the eye, which no
	//    depth buffer has. Fading it out is what keeps a wall facing the camera
	//    from reflecting whatever happens to be in front of it.
	const vec2	edge	  = min( hitUv, vec2( 1.0 ) - hitUv );
	const float edgeFade  = smoothstep( 0.0, 0.12, min( edge.x, edge.y ) );
	const float towardEye = clamp( -dot( rayDir, toSurface ), 0.0, 1.0 );
	const float camFade	  = 1.0 - smoothstep( 0.25, 0.85, towardEye );
	const float distFade  = 1.0 - clamp( travelled / maxDistance, 0.0, 1.0 );

	// ...AND THE TWO THAT ASK WHETHER THE HIT ITSELF MEANS ANYTHING (lane SSR-1,
	// the Mirror Room's shredded chrome sphere). Everything above is about the
	// RAY — where it went and where it was pointing. Neither says anything about
	// the SURFACE it landed on, and on a curved mirror that is exactly what goes
	// wrong: the rays leave in every direction, most of them arrive somewhere
	// they cannot be checked against, and the march used to hand every one of
	// them back with full confidence.
	//
	//  * THE ARRIVAL ANGLE. A depth buffer records the FRONT of each surface.
	//    A ray that "crossed" a surface whose normal points the same way the
	//    ray travels arrived at its BACK — the depth buffer has no idea what is
	//    there, and the colour at that pixel is the front face's, which faces
	//    somewhere else entirely. At exactly grazing arrival the answer is just
	//    as meaningless: one texel either way is a different surface. So the
	//    confidence ramps from nothing at a backface or a grazing arrival to
	//    full at 0.2 (about 11 degrees off the surface).
	//  * THE THICKNESS TEST'S OWN MARGIN. `thickness` is a GUESS at how thick
	//    the world is; a crossing that only qualified because the guess is
	//    generous is a maybe, not a hit. Full confidence up to half of it,
	//    falling to nothing at the whole of it — measured on the part of the gap
	//    the march's own step cannot explain (see where hitDiff is computed).
	//
	// Both are ZERO-COST where the trace was already trustworthy — a flat floor
	// reflecting the room in front of it arrives at its hits face-on and well
	// inside the tolerance, so both terms are 1 and the frame does not move.
	vec3 hitNormal = texture( vkSampler2D( gBufNormals, samplerState ), hitUv ).xyz * 2.0 - 1.0;
	float arrival = 0.0;
	if( dot( hitNormal, hitNormal ) > 1e-6 )
	{
		hitNormal = normalize( hitNormal );
		hitNormal.z = -hitNormal.z;				// right-handed G-buffer -> left-handed march
		arrival = -dot( rayDir, hitNormal );
	}
	const float faceFade  = smoothstep( 0.0, 0.2, arrival );
	const float thickFade = 1.0 - smoothstep( 0.5, 1.0, hitDiff );

	// ...AND THE RIM (SSR-EDGE-1). The resolve lets a TRUSTED hit win outright
	// (SSR-2's mirror rule), so whatever decides "hit or no hit" draws a
	// pixel-sharp line into the picture unless the hit's weight has already
	// faded by the time the decision flips. Two things flip it at a boundary
	// that is not an object's silhouette, and both were a one-step flip:
	//
	//  * THE END OF THE RAY'S RANGE. A ray that is about to leave the screen,
	//    pass behind the eye or run out of `maxDistance` hits if its crossing
	//    sample lands before the end and misses if its phase puts that sample
	//    one step later — the checkerboard jitter makes neighbours disagree, and
	//    the boundary is a speckled line of full-weight hits against misses
	//    (measured on ssr.edge's spheres: the floor's reflection on the lower
	//    hemisphere ended in 592 such pixels; the rays head toward the camera,
	//    where one 0.26 m step crosses much of the screen, so the screen exit
	//    and not `maxDistance` is the end almost everywhere). So the weight
	//    fades over the LAST kEdgeSteps STEPS of the ray's own range
	//    (jahRayRange: exact, not stepped) — a hit with less than a step of
	//    range left is the one its neighbour might have missed, and it hands
	//    over at nearly zero.
	//  * THE THICKNESS MARGIN. `thickFade` was a TRUST term, and trust is a
	//    verdict to the resolve (a quorum and a fan count, at 0.5): the margin's
	//    smooth ramp became a step where it crossed the count's threshold. It is
	//    an ENVELOPE term now — the same ramp, in the channel the resolve scales
	//    by instead of counting — so a hit whose crossing only qualifies because
	//    the thickness guess is generous fades out continuously and is still
	//    counted as the hit it is (the fan and the quorum judge its AGREEMENT;
	//    the arrival angle stays the one trust term).
	const float kEdgeSteps = 2.0;
	const float rangeLeft  = ( range - travelled ) / stepLen;
	const float rangeFade  = smoothstep( 0.0, kEdgeSteps, rangeLeft );

	// THE TWO GROUPS GO IN SEPARATE CHANNELS (lane SSR-2 — see OUTPUT at the
	// top): z carries the ENVELOPE ramps (distance, screen edge, away from the
	// camera, and SSR-EDGE-1's range end and thickness margin), w the TRUST
	// (the arrival angle). The resolve forms their product, and needs them apart
	// to tell "the data runs out here" from "this hit means nothing", which are
	// opposite decisions on a mirror.
	fragColour = vec4( hitUv, distFade * edgeFade * camFade * rangeFade * thickFade, faceFade );
}
