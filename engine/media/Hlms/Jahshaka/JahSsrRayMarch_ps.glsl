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
//    z  = distance fade, 1 at the origin falling to 0 at maxDistance
//    w  = geometric confidence: 0 for a miss, otherwise screen-edge fade times
//         the "this reflection points back at the camera" fade
// The ROUGHNESS mask is deliberately NOT folded in here: this pass may run at
// half resolution, and a roughness cutoff evaluated at half res has visibly
// blocky edges. The resolve pass re-reads roughness at FULL resolution. The
// only thing roughness does here is skip the march entirely for a surface that
// could never show a sharp reflection — a pure cost saving with no visual term.
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

	// HlmsPbs' prepass packs roughness as (r - 0.02) * 1.02040816; this is the
	// inverse. Anything rougher than the cutoff cannot show a screen-space
	// reflection worth marching for — see the note at the top of the file.
	const float roughness =
		texture( vkSampler2D( gBufShadowRoughness, samplerState ), inPs.uv0 ).y * 0.98 + 0.02;
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
	const float jitter = fract( ( floor( pixel.x ) + floor( pixel.y ) ) * 0.5 );

	float hit		= 0.0;
	float travelled = maxDistance;
	vec2  hitUv		= vec2( 0.0 );
	float prevT		= 0.0;

	// The loop bound must be a COMPILE-TIME constant for the shader to unroll
	// sanely on every driver; `steps` is the runtime budget inside it.
	for( int i = 1; i <= 128; ++i )
	{
		if( i > steps )
			break;

		const float t = stepLen * ( float( i ) + jitter );
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
			if( diff > 0.0 && diff < thickness + stepLen * 0.5 )
			{
				// Binary refinement between the last miss and this hit. Five
				// iterations take the hit to 1/32 of a step, which is what
				// stops the reflection from looking quantised along the ray.
				float lo = prevT;
				float hi = t;
				vec2  refined = uv;
				for( int k = 0; k < 5; ++k )
				{
					const float mid = ( lo + hi ) * 0.5;
					const vec3	q	= origin + rayDir * mid;
					const vec4	hq	= viewToTextureSpaceMatrix * vec4( q, 1.0 );
					const vec2	quv = hq.xy / hq.w;
					const float qz	= jahViewDistance( jahSceneDepthAt( quv ) );
					if( q.z > qz )
					{
						hi = mid;
						refined = quv;
					}
					else
					{
						lo = mid;
					}
				}
				hitUv	  = refined;
				travelled = hi;
				hit		  = 1.0;
				break;
			}
		}
		prevT = t;
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

	fragColour = vec4( hitUv, distFade, edgeFade * camFade );
}
