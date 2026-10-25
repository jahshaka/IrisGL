// THE WORLD CACHES' RAY SET AND ITS FRAME (PHOTON-7 stage 10, 10-2a): the ONE text of the directions every cache ray
// takes - the cards' and the voxel sides' rays through the voxel chain (jah_voxel_ray.glsl, jah_voxel_cones.glsl's
// jahRayDiffuse) and through the TLAS (rayquery/rq_card_gather.comp) - and of the frame they are lifted in, which the
// cone sets that remain share. Moved here out of the two files so a ray job that traces the TLAS takes the same
// directions without the march: the Hlms consumers insert it as the head of the piece JahVoxelRay (the build WRAPS
// this file before jah_voxel_ray.glsl, irisgl/engine/CMakeLists.txt); a ray job includes it through glslang.
//
// THE AT-SIGN RULE: this file becomes Hlms input, so no character of it may be the Hlms directive mark, comments
// included (the wrapper fails the build).

#ifndef JAH_RAY_SET_GLSL
#define JAH_RAY_SET_GLSL

/// THE CONE FRAME IS A FUNCTION OF THE NORMAL AND OF NOTHING ELSE (fork change
/// 0083). An orthonormal frame whose third axis is `n`, by Frisvad's method
/// (JGT 2012) — continuous over the whole sphere but for ONE direction, where
/// the branch hands back a fixed frame instead of dividing by zero.
///
/// `n` is in a CAMERA-INDEPENDENT space — the cubemap frame (world with z
/// negated, the frame HlmsPbs's invViewMatCubemap takes a view direction to) —
/// because a frame is only worth building once it cannot turn with the head.
///
/// AND IT IS BUILT ABOUT THAT SPACE'S OWN AXES, a decision measured rather than
/// argued: Frisvad's special direction is the antipode of (0,0,1), and a fixed
/// pre-rotation (onto a body diagonal, which no axis-aligned surface has) turns
/// the cones of every axis-aligned surface to odd angles, where each sample
/// is a three-way blend of the anisotropic volumes, and the bounce comes out
/// dimmer — gi.field_follows' red-wall bounce read 0.0174 with the axes and
/// 0.0105 with the body diagonal. The price: the special direction is a world
/// axis, where a surface within a hair of it takes the fixed frame while its
/// neighbours take a rapidly turning one. "The frame is axis-aligned for axis
/// normals" and "the reference direction is an axis" are the same statement.
///
/// The two frames this replaced were both unstable against an anisotropic
/// voxel field (the diffuse set is not azimuthally symmetric, so turning the
/// frame about the normal reads different axis volumes): the material's
/// per-vertex TANGENT frame (a kink along every quad's diagonal) and the screen
/// derivative fallback.
mat3 jahConeBasis( vec3 n )
{
	vec3 t;
	vec3 b;
	if( n.z < -0.9999999 )
	{
		t = vec3( 0.0, -1.0, 0.0 );
		b = vec3( -1.0, 0.0, 0.0 );
	}
	else
	{
		float a = 1.0 / ( 1.0 + n.z );
		float c = -n.x * n.y * a;
		t = vec3( 1.0 - n.x * n.x * a, c, -n.x );
		b = vec3( c, 1.0 - n.y * n.y * a, -n.y );
	}
	return mat3( t, b, n );
}

/// The same frame for a WORLD normal, returned in world axes: built in the
/// cubemap frame (z negated) exactly as the pixel builds it, and brought back
/// by negating z again (exact in floating point, so a compute job's cone
/// directions are the pixel's to the bit, before the pixel's view rotation).
mat3 jahConeBasisWorld( vec3 nWorld )
{
	mat3 c = jahConeBasis( vec3( nWorld.x, nWorld.y, -nWorld.z ) );
	return mat3( vec3( c[0].x, c[0].y, -c[0].z ), vec3( c[1].x, c[1].y, -c[1].z ),
				 vec3( c[2].x, c[2].y, -c[2].z ) );
}

/// THE WORLD CACHES' DIRECTIONS (the spec's "N fixed, world-keyed, cosine-distributed rays"): direction `i` of `n` in
/// the frame whose third axis is the normal (jahConeBasis above: a function of the WORLD normal alone) -
/// a Vogel spiral on the unit disc (radius sqrt((i + 1/2) / n), the golden angle between neighbours: equal areas, so
/// equal projected solid angles once lifted onto the hemisphere, Malley's method). ONE SET FOR EVERY SURFACE POINT,
/// never turned per texel or voxel: a cache is READ - by a reflection, by the gather's hits - texel by texel, and a set
/// turned by a world key per texel stored the set's own sampling noise into the cache, a speckle of codes the readers
/// show (rtreflect.contact: the crease's floor rows 91-98 a row apart, 36 columns over its law); the one set is smooth
/// across a surface, deterministic, and a function of the world (a cache's rest, "replace at once", walk = teleport).
vec3 jahRayCosineDir( int i, int n )
{
	const float u = ( float( i ) + 0.5 ) / float( n );
	const float rr = sqrt( u );
	const float phi = 6.2831853 * fract( float( i ) * 0.38196601 );
	return vec3( rr * cos( phi ), rr * sin( phi ), sqrt( max( 1.0 - u, 0.0 ) ) );
}

#endif   // JAH_RAY_SET_GLSL
