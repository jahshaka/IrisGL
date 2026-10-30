// THE SCREEN MARCH'S CORE (SSR-1, PHOTON-7 stage 6) - pure functions, for its one host since PHOTON-7-9A.
//
// The march walks a reflected ray through the DEPTH BUFFER: its range on screen solved exactly, fixed steps with a
// phase jitter, the crossing test against the surface's front, the bisection that refines a crossing, and the fades
// that say how much the screen can vouch for what it found. Its host is the NO-RAY chain's raster march
// (JahSsrRayMarch_ps.glsl, staged raw into media as JahSsrMarch.glsl by engine/CMakeLists.txt, the fog law's pattern),
// which marches the mirror direction for its resolve. (The ray tier's screen trace - the "Both" estimator's first
// source, rq_reflect.comp - is deleted with its secant and its vouch: a ray chain reflects by its rays alone.)
// Extracted VERBATIM from the raster march (SSR-1, SSR-EDGE-1, SSR-RINGS-1: every rule's reasoning is in that file's
// history and in the comments kept below); only the projection became an argument and the depth read a macro.
//
// THE MARCH'S SPACE: any orthonormal frame whose third axis is the camera's forward axis and whose z is the VIEW
// DISTANCE along it (the raster's left-handed view space; the trace's right/down/forward frame) - every test below is
// a dot product or a z, so the handedness never enters. `viewToTex` maps a point of that space to homogeneous TEXTURE
// space: xy / w is the [0,1] image coordinate, w > 0 in front of the eye (affine in the point, so every bound below
// is a linear one along the ray).
//
// THE HOST DEFINES, before including this file:
//   JAH_SSR_SCENE_DEPTH( uv )    the depth buffer's raw value at an image coordinate (a point read)
//   JAH_SSR_VIEW_DISTANCE( d )   that raw value's view distance (reverse-Z safe; both projections)
// The fog section below needs the fog law (jah_fog_along.glsl) included first, which defines JAH_FOG_P.
// No at-sign anywhere in this file: the raster copy is Hlms media (trap 2; WrapHlmsPiece.cmake refuses one).
#ifndef JAH_SSR_MARCH_GLSL
#define JAH_SSR_MARCH_GLSL

#ifdef JAH_SSR_SCENE_DEPTH

// THE RAY'S OWN RANGE (SSR-EDGE-1): how far along `rayDir` the march can follow the ray before it stops being able
// to - past `maxDistance`, behind the eye, or off the screen - solved EXACTLY on the ray's clip-space line rather than
// found by stepping. A point is on screen while h.x, h.w - h.x, h.y and h.w - h.y are all >= 0 and h.w > 0; each is
// LINEAR in t along the ray (h = h0 + h1 t), so each leaves its half-space at one t, and the range is the nearest.
float jahSsrRayRange( mat4 viewToTex, vec3 origin, vec3 rayDir, float maxDistance )
{
	float range = maxDistance;
	if( rayDir.z < 0.0 )
		range = min( range, -origin.z / rayDir.z );			// the eye's plane
	const vec4 h0 = viewToTex * vec4( origin, 1.0 );
	const vec4 h1 = viewToTex * vec4( rayDir, 0.0 );
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

// THE STEP'S PHASE (SSR-RINGS-1): a two-value checkerboard jitter along the ray, keyed on the PIXEL - without it a
// fixed step lands every pixel of a flat floor on the same depth samples and the reflection bands; with it
// neighbouring pixels sample between each other's steps. Or (phaseRule 2) SIXTEEN phases instead of two: a 4x4
// ordered (Bayer) matrix - the arc becomes a ramp rather than an edge, at the price of a grain. The phase is always
// an offset inside ONE step.
float jahSsrJitter( vec2 pixel, float phaseRule )
{
	float jitter = fract( ( floor( pixel.x ) + floor( pixel.y ) ) * 0.5 );
	if( phaseRule > 1.5 )
	{
		const ivec2 ip = ivec2( pixel ) & ivec2( 3 );
		const int bayer[16] = int[16]( 0, 8, 2, 10, 12, 4, 14, 6,
									   3, 11, 1, 9, 15, 7, 13, 5 );
		jitter = float( bayer[ ip.y * 4 + ip.x ] ) * ( 1.0 / 16.0 );
	}
	return jitter;
}

// WHAT A MARCH FOUND: hit 1 (a crossing the depth buffer vouches for) or 0; the image coordinate of the crossing; the
// distance travelled to it along the ray (maxDistance on a miss); the crossing's depth error as a fraction of the
// thickness (the step's own overshoot subtracted); the ray's exact range (jahSsrRayRange); the step's length.
struct JahSsrMarch
{
	float hit;
	vec2  hitUv;
	float travelled;
	float hitDiff;
	float range;
	float stepLen;
};

// THE MARCH. Fixed steps of maxDistance / steps from the origin, the LAST SAMPLE on the end of the range (just inside
// it, so a crossing before the end is found whatever the phase - SSR-EDGE-1). A crossing is a sample BEHIND the
// surface at that pixel by less than the tolerance (diff > 0: it crossed; diff < tolerance: the surface is not so far
// in front that the crossing is a false positive through thin geometry - the depth buffer knows a surface's front,
// never its back). WHICH SAMPLE ANSWERS (phaseRule, SSR-RINGS-1): 0 CHECKER - the coarse sample, the shipped march; 1
// REFINED - a crossing is a SIGN CHANGE and the thickness test is asked at the refined crossing; 2 DITHER - the
// shipped hit rule under sixteen phases (jahSsrJitter). Five bisections take a hit to 1/32 of a step, with the
// validity guards the coarse march has (a midpoint behind the eye, off screen, or on a pixel with no depth falls back
// to the last COARSE hit, which is always a real one).
JahSsrMarch jahSsrMarch( mat4 viewToTex, vec3 origin, vec3 rayDir, float maxDistance, float thickness, float stepsF,
						 float jitter, float phaseRule )
{
	const int	steps		= int( stepsF );
	const float stepLen		= maxDistance / max( 1.0, stepsF );
	const float range	 = jahSsrRayRange( viewToTex, origin, rayDir, maxDistance );
	const float rangeEnd = max( range - 1.0e-3 * stepLen, 0.0 );

	float hit		= 0.0;
	float travelled = maxDistance;
	vec2  hitUv		= vec2( 0.0 );
	float prevT		= 0.0;
	float hitDiff	= 0.0;			// the crossing's depth error, as a fraction of the tolerance

	// The loop bound must be a COMPILE-TIME constant for the shader to unroll sanely on every driver; `steps` is the
	// runtime budget inside it.
	for( int i = 1; i <= 128; ++i )
	{
		if( i > steps )
			break;

		const float tStep = stepLen * ( float( i ) + jitter );
		const bool	last  = tStep >= rangeEnd;
		const float t	  = last ? rangeEnd : tStep;
		if( t <= prevT )
			break;								// the range ended before this step began
		const vec3	pt = origin + rayDir * t;	// (not `p`: a host may name its parameter block so)
		if( pt.z <= 0.0 )
			break;								// the ray went behind the eye

		const vec4 h = viewToTex * vec4( pt, 1.0 );
		if( h.w <= 0.0 )
			break;
		const vec2 uv = h.xy / h.w;
		if( uv.x < 0.0 || uv.x > 1.0 || uv.y < 0.0 || uv.y > 1.0 )
			break;								// left the screen: nothing to reflect

		const float sceneRaw = JAH_SSR_SCENE_DEPTH( uv );
		if( sceneRaw > 0.0 && sceneRaw < 1.0 )
		{
			const float sceneZ = JAH_SSR_VIEW_DISTANCE( sceneRaw );
			const float diff   = pt.z - sceneZ;
			const float crossTol = phaseRule == 1.0 ? 1.0e9 : thickness + stepLen * 0.5;
			if( diff > 0.0 && diff < crossTol )
			{
				// HOW MARGINAL THE CROSSING WAS (lane SSR-1), the step's own overshoot subtracted first - what is
				// left is the part of the gap the STEP cannot explain, normalised by the thickness itself.
				hitDiff = max( 0.0, diff - stepLen * max( rayDir.z, 0.0 ) ) /
						  max( thickness, 1e-6 );
				float lo = prevT;
				float hi = t;
				vec2  refined = uv;
				float refinedDiff = diff;
				for( int k = 0; k < 5; ++k )
				{
					const float mid = ( lo + hi ) * 0.5;
					const vec3	q	= origin + rayDir * mid;
					const vec4	hq	= viewToTex * vec4( q, 1.0 );
					if( hq.w <= 0.0 )
						break;
					const vec2	quv = hq.xy / hq.w;
					if( quv.x < 0.0 || quv.x > 1.0 || quv.y < 0.0 || quv.y > 1.0 )
						break;
					const float qRaw = JAH_SSR_SCENE_DEPTH( quv );
					if( qRaw <= 0.0 || qRaw >= 1.0 )
					{
						lo = mid;					// nothing there: not a crossing
						continue;
					}
					const float qz	= JAH_SSR_VIEW_DISTANCE( qRaw );
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
				// THE THICKNESS TEST, AT THE CROSSING (phaseRule 1): a crossing that fails it is a ray that passed
				// BEHIND the object, and the march goes on looking.
				if( phaseRule == 1.0 )
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
	JahSsrMarch m;
	m.hit = hit;
	m.hitUv = hitUv;
	m.travelled = travelled;
	m.hitDiff = hitDiff;
	m.range = range;
	m.stepLen = stepLen;
	return m;
}

// THE ENVELOPE (lane SSR-2's split; SSR-EDGE-1's two): how gracefully the screen has to stop at a hit - the places
// where a screen-space trace runs out of data. None of them is a doubt about the hit's surface (that is the arrival,
// below):
//  * SCREEN EDGE - the information stops at the border of the frame;
//  * BACK AT THE CAMERA - a ray roughly reversing the view vector asks about geometry behind the eye;
//  * DISTANCE - 1 at the origin falling to 0 at maxDistance;
//  * THE END OF THE RAY'S RANGE - a hit with less than a step of range left is the one its neighbour might have missed
//    (the fade over the last kEdgeSteps steps; the width the range's own screen-space gradient, floored at 2 - a host
//    that has no neighbours' ranges passes 0);
//  * THE THICKNESS MARGIN - a crossing that only qualifies because the thickness guess is generous fades out.
// `toSurface` is the unit view vector at the origin (the march space's), `rayDir` the unit ray. The product is the
// raster march's z, in its order.
float jahSsrEnvelope( JahSsrMarch m, vec3 rayDir, vec3 toSurface, float maxDistance, float rangeGrad )
{
	const vec2	edge	  = min( m.hitUv, vec2( 1.0 ) - m.hitUv );
	const float edgeFade  = smoothstep( 0.0, 0.12, min( edge.x, edge.y ) );
	const float towardEye = clamp( -dot( rayDir, toSurface ), 0.0, 1.0 );
	const float camFade	  = 1.0 - smoothstep( 0.25, 0.85, towardEye );
	const float distFade  = 1.0 - clamp( m.travelled / maxDistance, 0.0, 1.0 );
	const float thickFade = 1.0 - smoothstep( 0.5, 1.0, m.hitDiff );
	const float kEdgeSteps = max( 2.0, rangeGrad / m.stepLen );
	const float rangeLeft  = ( m.range - m.travelled ) / m.stepLen;
	const float rangeFade  = smoothstep( 0.0, kEdgeSteps, rangeLeft );
	return distFade * edgeFade * camFade * rangeFade * thickFade;
}

// THE TRUST (lane SSR-1): the ARRIVAL angle at the surface the ray hit, -dot( ray, the hit's normal ) - a backface or a
// grazing arrival is a hit the depth buffer cannot vouch for (it records the FRONT of each surface; one texel either
// way at grazing is a different surface). Nothing at a backface, full at 0.2 (about 11 degrees off the surface).
float jahSsrFaceFade( float arrival )
{
	return smoothstep( 0.0, 0.2, arrival );
}

#endif   // JAH_SSR_SCENE_DEPTH

#ifdef JAH_FOG_P
// THE SCREEN HIT, RE-FOGGED (PHOTON-I-1: one fog law on every reflection path). The previous picture holds the hit as
// the EYE saw it - the fog law along the eye's ray to the hit (upstream's distance block, then jahFogMedia) - and the
// reflection must carry it as the mirror's virtual eye sees it: the law along the ray from the reflecting surface to
// the hit, the height fog's start spent by the eye's leg (jahFogAlong). The eye's law is affine in the surface's
// radiance but for the breakthrough's luminance, so it is undone by its affine pair (the law at 0 and at 1) and one
// correction step through the law itself; a channel the eye's medium all but hid (transmittance under 1e-3) carries
// what the screen holds. With no fog of either kind the colour is returned as it was, bit for bit.
vec3 jahSsrRefog( vec3 screen, vec3 eye, vec3 surface, vec3 hit )
{
	if( JAH_FOG_P( 3 ).w < 0.5 && JAH_FOG_P( 7 ).w < 0.5 )
		return screen;
	const vec3 toHit = hit - eye;
	const float le = length( toHit );
	const vec3 dirE = toHit / max( le, 1e-6 );
	const vec3 s0 = jahFogAlong( vec3( 0.0 ), eye.y, dirE, le, 0.0 );
	const vec3 t1 = jahFogAlong( vec3( 1.0 ), eye.y, dirE, le, 0.0 ) - s0;
	const bvec3 seen = greaterThan( t1, vec3( 1e-3 ) );
	vec3 c = mix( screen, ( screen - s0 ) / max( t1, vec3( 1e-3 ) ), vec3( seen ) );
	c = max( c, vec3( 0.0 ) );
	c = max( c + mix( vec3( 0.0 ), ( screen - jahFogAlong( c, eye.y, dirE, le, 0.0 ) ) /
										  max( t1, vec3( 1e-3 ) ), vec3( seen ) ),
			 vec3( 0.0 ) );
	const vec3 leg = hit - surface;
	const float l1 = length( leg );
	return jahFogAlong( c, surface.y, leg / max( l1, 1e-6 ), l1, length( surface - eye ) );
}
#endif   // JAH_FOG_P

#endif   // JAH_SSR_MARCH_GLSL
