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
//    exactly like fork 3f1ad1110 (was 0011) taught the SSAO shader to.
//
// OUTPUT (PFG_RGBA16_UNORM, so every channel must be [0,1]):
//    xy = the coordinate the ray hit, in the SHOT's uv (the target's without a
//         letterbox; the resolve maps it back — SSR-LETTERBOX-1)
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
	// THE SHOT'S UV MAP (SSR-LETTERBOX-1). Under a letterbox the camera's
	// projection lands in the target's INNER rectangle while this quad covers
	// the whole target, so the march works in the SHOT's uv ([0,1] over the
	// rectangle: `viewToTextureSpaceMatrix` is the shot's own map) and converts
	// to the target's only to read a texture (jahShotToTex). An INSET, so the
	// constant buffer's zeros are the identity: x, y = the rectangle's corner;
	// z, w = 1 - its width, height. chain::updateSsr pushes it.
	uniform vec4 shotInset;
	// The camera's frustum span per unit of uv (view-space x per u, y per v, at
	// z = 1): what corrects the quad's interpolated view vector, which is
	// spread over the whole target, to the shot's uv.
	uniform vec4 cameraSpan;
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

// The shot's uv -> the target's, where every texture of this pass lives. Exact
// in float without a letterbox (0 + uv * 1).
vec2 jahShotToTex( vec2 uv )
{
	return shotInset.xy + uv * ( vec2( 1.0 ) - shotInset.zw );
}

// ...and the depth at a SHOT uv.
float jahSceneDepthAt( vec2 uv )
{
	return texture( vkSampler2D( depthTexture, samplerState ), jahShotToTex( uv ) ).x;
}

// THE MARCH'S CORE IS THE SCREEN TRACE'S (PHOTON-7 stage 6, THE ONE ESTIMATOR): the ray's exact range, the stepped
// crossing and its refinement, the phase jitter and the fades - one text of pure functions shared with the ray tier's
// trace (rq_reflect.comp), staged raw here from irisgl/engine/src/rayquery/include/jah_ssr_march.glsl by
// engine/CMakeLists.txt. This file is its HOST: the depth read in the SHOT's uv (the letterbox map above), the
// projection `viewToTextureSpaceMatrix`, the left-handed view space whose z is the view distance.
#define JAH_SSR_SCENE_DEPTH( uv ) jahSceneDepthAt( uv )
#define JAH_SSR_VIEW_DISTANCE( d ) jahViewDistance( d )
#include "JahSsrMarch.glsl"

// A NEIGHBOURING PIXEL'S RANGE (SSR-EDGE-1: the fade's width). The same
// reconstruction main() makes for its own pixel — the depth, the G-buffer
// normal, the view vector, the reflection — at the SHOT uv `uv` whose view
// vector is `cameraDir`; negative where that pixel has no surface to reflect.
float jahPixelRange( vec2 uv, vec3 cameraDir, float maxDistance )
{
	if( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 )
		return -1.0;
	const float d = jahSceneDepthAt( uv );
	if( d <= 0.0 || d >= 1.0 )
		return -1.0;
	vec3 n = texture( vkSampler2D( gBufNormals, samplerState ), jahShotToTex( uv ) ).xyz * 2.0 - 1.0;
	if( dot( n, n ) < 1e-6 )
		return -1.0;
	n = normalize( n );
	n.z = -n.z;
	vec3 o;
	vec3 v;
	if( orthoParams.x > 0.5 )
	{
		o = vec3( cameraDir.xy * orthoParams.y, jahViewDistance( d ) );
		v = vec3( 0.0, 0.0, 1.0 );
	}
	else
	{
		o = cameraDir * jahViewDistance( d );
		v = normalize( o );
	}
	return jahSsrRayRange( viewToTextureSpaceMatrix, o, reflect( v, n ), maxDistance );
}

void main()
{
	// THIS PIXEL IN THE SHOT (SSR-LETTERBOX-1): outside the letterbox's
	// rectangle there is no shot, and nothing to march. Without a letterbox
	// shotUv IS inPs.uv0, bit for bit ((uv - 0) / 1).
	const vec2 shotUv = ( inPs.uv0 - shotInset.xy ) / ( vec2( 1.0 ) - shotInset.zw );
	if( shotUv.x < 0.0 || shotUv.x > 1.0 || shotUv.y < 0.0 || shotUv.y > 1.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}
	// ...and the view vector through it: the quad's corners interpolated at the
	// target's uv, corrected to the shot's (exactly zero without a letterbox).
	const vec3 cameraDir = inPs.cameraDir + vec3( ( shotUv - inPs.uv0 ) * cameraSpan.xy, 0.0 );

	const float rawDepth = jahSceneDepthAt( shotUv );
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
	// value SQUARED, and fork 16d8e29d4 (was 0043) packs it over [0.001, 1] — the shader's
	// own alpha floor — not over upstream's old [0.02, 1].
	//
	// This line used the PRE-0043 range and then compared the result to the
	// cutoff as though it were perceptual, so the band the frame actually applied
	// was `0.980981 * alpha + 0.019019 > 0.35`, i.e. alpha 0.3374, i.e.
	// PERCEPTUAL 0.581 — a number nobody chose and nobody could read anywhere.
	//
	// So: undo the range fork 16d8e29d4 (was 0043) wrote, then take the square root, because the
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
		origin	  = vec3( cameraDir.xy * orthoParams.y, jahViewDistance( rawDepth ) );
		toSurface = vec3( 0.0, 0.0, 1.0 );
	}
	else
	{
		origin	  = cameraDir * jahViewDistance( rawDepth );
		toSurface = normalize( origin );
	}
	const vec3 rayDir	 = reflect( toSurface, normalVS );

	const float maxDistance = rayParams.x;
	const float thickness	= rayParams.y;

	// THE STEP'S PHASE (SSR-RINGS-1; jah_ssr_march.glsl's jahSsrJitter): the two-value checkerboard keyed on this
	// pixel, or sixteen phases under marchParams.x == 2.
	const vec2	pixel  = inPs.uv0 * rayBufferRes.xy;
	const float jitter = jahSsrJitter( pixel, marchParams.x );

	// THE MARCH (jah_ssr_march.glsl): the ray's range found once (SSR-EDGE-1), fixed steps of maxDistance / steps,
	// the crossing (marchParams.x: the phase rule, SSR-RINGS-1) and its five-bisection refinement.
	const JahSsrMarch m = jahSsrMarch( viewToTextureSpaceMatrix, origin, rayDir, maxDistance, thickness, rayParams.z,
									   jitter, marchParams.x );
	if( m.hit <= 0.0 )
	{
		fragColour = vec4( 0.0 );
		return;
	}

	// THE ARRIVAL ANGLE (lane SSR-1, the Mirror Room's shredded chrome sphere): a depth buffer records the FRONT of each
	// surface, so a ray that "crossed" a surface whose normal points the way the ray travels arrived at its BACK, and a
	// grazing arrival is one texel either way from another surface (jahSsrFaceFade).
	vec3 hitNormal = texture( vkSampler2D( gBufNormals, samplerState ), jahShotToTex( m.hitUv ) ).xyz * 2.0 - 1.0;
	float arrival = 0.0;
	if( dot( hitNormal, hitNormal ) > 1e-6 )
	{
		hitNormal = normalize( hitNormal );
		hitNormal.z = -hitNormal.z;				// right-handed G-buffer -> left-handed march
		arrival = -dot( rayDir, hitNormal );
	}
	const float faceFade  = jahSsrFaceFade( arrival );

	// THE RIM'S WIDTH (SSR-EDGE-1): the resolve lets a TRUSTED hit win outright (SSR-2's mirror rule), so the end of
	// the ray's range fades over the range's own SCREEN-SPACE GRADIENT, in steps, floored at 2 (jahSsrEnvelope) - where
	// neighbouring pixels' rays end many steps apart (a curved reflector's limb, where the rays fan) a two-step fade is
	// narrower than one pixel and the edge stays a rim. Read on BOTH sides along each axis and the SMALLER taken, so a
	// neighbour across a silhouette (another surface, whose range has nothing to do with this one's) cannot widen it:
	// a one-sided jump is an edge of the receiver, a two-sided one is the rays fanning.
	const vec2 onePixel = ( vec2( 1.0 ) / rayBufferRes.xy ) / ( vec2( 1.0 ) - shotInset.zw );
	float rangeGrad = 0.0;
	for( int axis = 0; axis < 2; ++axis )
	{
		const vec2 d = axis == 0 ? vec2( onePixel.x, 0.0 ) : vec2( 0.0, onePixel.y );
		const float ra = jahPixelRange( shotUv + d, cameraDir + vec3( d * cameraSpan.xy, 0.0 ), maxDistance );
		const float rb = jahPixelRange( shotUv - d, cameraDir - vec3( d * cameraSpan.xy, 0.0 ), maxDistance );
		float g = 1.0e30;
		if( ra >= 0.0 )
			g = min( g, abs( ra - m.range ) );
		if( rb >= 0.0 )
			g = min( g, abs( rb - m.range ) );
		if( g < 1.0e30 )
			rangeGrad = max( rangeGrad, g );
	}

	// THE TWO GROUPS GO IN SEPARATE CHANNELS (lane SSR-2 - see OUTPUT at the top): z carries the ENVELOPE ramps
	// (distance, screen edge, away from the camera, and SSR-EDGE-1's range end and thickness margin), w the TRUST (the
	// arrival angle). The resolve forms their product, and needs them apart to tell "the data runs out here" from
	// "this hit means nothing", which are opposite decisions on a mirror.
	fragColour = vec4( m.hitUv, jahSsrEnvelope( m, rayDir, toSurface, maxDistance, rangeGrad ), faceFade );
}
