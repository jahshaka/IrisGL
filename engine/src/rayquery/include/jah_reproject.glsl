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

// ---- A MOVING OBJECT (REFLECT-MOVERS-1) ------------------------------------
// The camera path above is exact for everything that stood still. A point ON A
// MOVER was somewhere else last frame, so the camera alone sends it to the
// texel where the object USED to be — the distance test rejects that texel and
// the temporal mean restarts from one sample, every frame the object moves: the
// dither the owner saw on a moving glossy sphere. The GPU scene already keeps
// both poses of every slot (GpuScene.h, GpuInstance::world / prevWorld: the
// previous frame's EXACTLY, and prevWorld == world bit for bit for anything
// that did not move this frame), so the point's previous position is the
// inverse of this pose followed by the last one.
//
// A CALLER hands over the slot's six rows as read from the instance table (row
// i of `world` is `w[i]`, of `prevWorld` is `q[i]`: ROW-MAJOR 3x4, the top three
// rows of Ogre's Matrix4 — GpuScene.h's warning against reading them as a
// mat3x4) and its flags word.

/// GpuScene.h GpuInstanceFlag: kGpuMover (the document says it moves) and
/// kGpuDragMover (the user holds it). Anything else never takes the branch.
const uint kJahReprojectMoverFlags = ( 1u << 2u ) | ( 1u << 8u );

/// Did this slot move between the last frame and this one? Only a mover can,
/// and only when its two poses differ in some bit — a mover at rest takes the
/// camera path exactly as a still object does, so no still pixel moves.
bool jahInstanceMoved( uint flags, uvec4 w0, uvec4 w1, uvec4 w2, uvec4 q0, uvec4 q1, uvec4 q2 )
{
	if( ( flags & kJahReprojectMoverFlags ) == 0u )
		return false;
	return any( notEqual( w0, q0 ) ) || any( notEqual( w1, q1 ) ) || any( notEqual( w2, q2 ) );
}

/// WHERE A POINT OF THE SLOT WAS LAST FRAME: this pose inverted, the last one
/// applied. The 3x3 is inverted whole (a scale, a shear from a scaled parent),
/// never assumed to be a rotation.
vec3 jahInstancePrevPosition( uvec4 w0, uvec4 w1, uvec4 w2, uvec4 q0, uvec4 q1, uvec4 q2,
							  vec3 x )
{
	const vec4 a0 = uintBitsToFloat( w0 ), a1 = uintBitsToFloat( w1 ), a2 = uintBitsToFloat( w2 );
	const vec4 b0 = uintBitsToFloat( q0 ), b1 = uintBitsToFloat( q1 ), b2 = uintBitsToFloat( q2 );
	// GLSL matrices are built from COLUMNS: the transpose of the three rows.
	const mat3 m = transpose( mat3( a0.xyz, a1.xyz, a2.xyz ) );
	const vec3 local = inverse( m ) * ( x - vec3( a0.w, a1.w, a2.w ) );
	const vec4 l4 = vec4( local, 1.0 );
	return vec3( dot( b0, l4 ), dot( b1, l4 ), dot( b2, l4 ) );
}

/// ...and a DIRECTION of the slot (a normal: the inverse transpose), last frame.
vec3 jahInstancePrevNormal( uvec4 w0, uvec4 w1, uvec4 w2, uvec4 q0, uvec4 q1, uvec4 q2, vec3 n )
{
	const vec4 a0 = uintBitsToFloat( w0 ), a1 = uintBitsToFloat( w1 ), a2 = uintBitsToFloat( w2 );
	const vec4 b0 = uintBitsToFloat( q0 ), b1 = uintBitsToFloat( q1 ), b2 = uintBitsToFloat( q2 );
	const mat3 m = transpose( mat3( a0.xyz, a1.xyz, a2.xyz ) );
	const mat3 pm = transpose( mat3( b0.xyz, b1.xyz, b2.xyz ) );
	// object-space normal = transpose(m) * n (up to scale); back out through pm's inverse transpose
	const vec3 local = transpose( m ) * n;
	const vec3 was = transpose( inverse( pm ) ) * local;
	const float l2 = dot( was, was );
	return l2 > 1e-20 ? was * inversesqrt( l2 ) : n;
}

#endif   // JAH_REPROJECT_GLSL
