// THE REPROJECTION — where a world point lay in the PREVIOUS frame's picture,
// and whether the surface found there is the same one. ONE text, two temporal
// histories: the ray-traced reflection's (rq_reflect.comp, `historyAt`) and the
// screen-probe gather's pixel history (rq_probe_integrate.comp,
// PHOTON-GATHER-1c).
//
// WHY A CAMERA BASIS AND NOT A VIEW-PROJECTION MATRIX (rq_reflect.comp's note,
// kept): the image is described by the same five numbers the reconstruction
// uses (the camera position, the world-space ray of the image's top-left
// corner and its right and down spans, the forward axis), so inverting them is
// the same arithmetic forwards and backwards and cannot disagree with the
// reconstruction about a handedness, a y-flip or a depth convention. At rest
// the previous basis IS the current one, bit for bit, and a pixel centre
// reprojects onto itself (the float error is far below half a pixel) — the
// exact identity at rest PAN-SMEAR-1 asks of every history.
//
// A CALLER includes this after its own bindings; it reads no binding itself.

#ifndef JAH_REPROJECT_GLSL
#define JAH_REPROJECT_GLSL

/// The world point's 0..1 coordinate in an image and its depth along that
/// image's forward axis (the view distance the depth buffer decodes to).
/// `perspective` is 1 for a camera whose image is a fan of rays and 0 for an
/// orthographic one, whose `rayTL`/`rayRight`/`rayDown` are an image-plane
/// OFFSET rather than a ray.
///
/// Returns 0 when the point is in the image, 1 when it lies BEHIND the camera,
/// 2 when it lies outside the image. The two failures are kept apart because
/// a history may answer them differently; both leave `uv` meaningless.
int jahReproject( vec3 world, vec3 camPos, float perspective, vec3 rayTL, vec3 rayRight,
				  vec3 rayDown, vec3 fwd, out vec2 uv, out float z )
{
	uv = vec2( 0.0 );
	const vec3 d = world - camPos;
	z = dot( d, fwd );
	if( z <= 0.0 )
		return 1;
	const vec3 plane = perspective < 0.5 ? d - fwd * z : d / z;
	const vec3 q = plane - rayTL;
	uv = vec2( dot( q, rayRight ) / dot( rayRight, rayRight ),
			   dot( q, rayDown ) / dot( rayDown, rayDown ) );
	if( any( lessThan( uv, vec2( 0.0 ) ) ) || any( greaterThanEqual( uv, vec2( 1.0 ) ) ) )
		return 2;
	return 0;
}

/// THE 5 % TEST: is a surface stored `wasDist` away from the previous camera
/// the one this point (`dist` away from it) is? Far enough not to reject a
/// surface the camera merely moved along, tight enough that a silhouette's far
/// side is a different surface; the tolerance never falls below 5 cm (a metre's
/// 5 %), so a close surface is not rejected on the depth buffer's own grain.
/// A stored distance of zero or less is "nothing was here" and never agrees.
bool jahSameDistance( float wasDist, float dist )
{
	return wasDist > 0.0 && abs( wasDist - dist ) <= 0.05 * max( dist, 1.0 );
}

#endif   // JAH_REPROJECT_GLSL
