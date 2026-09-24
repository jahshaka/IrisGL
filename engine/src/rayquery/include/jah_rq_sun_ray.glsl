// THE SUN RAY — ONE TEXT, TWO CALLERS (PHOTON-HIT-SHADE-1; RAYS-1's contact ray,
// factored out of rq_sun_contact.comp).
//
// Is the sun visible from `origin`? One inline query towards it against the
// scene's TLAS, geometry only, OPAQUE only (alpha-tested casters are not in the
// structure, audit C-16). Two queries keep the common case cheap: the hard part
// of the range asks for ANY hit (terminate on the first) and answers 0 on one;
// only a ray that crossed it clean asks the fade band [hardEnd, range] for its
// NEAREST hit, whose distance hands the answer over linearly from 0 at hardEnd to
// 1 at range (the contact job's finisher: where a long low-sun shadow outruns the
// range the ray's exact edge hands over to the shadow map instead of stopping in
// a step). A caller with no fade band passes hardEnd == range.
//
// The callers:
//   * rq_sun_contact.comp — one ray per screen texel from the prepass' surface,
//     lifted by two pixel footprints, out to the contact range with its fade;
//   * the ray jobs' HIT RECORD (jah_rq_hit_record.glsl) — one ray from a hit the
//     visibility-buffer decode will shade, whose answer REPLACES the shadow map's
//     for the sun at that hit (a hit may lie anywhere, on screen or not, and the
//     map covers the camera's frustum only).
//
// The caller enables GL_EXT_ray_query and declares `tlas`.
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_SUN_RAY_GLSL
#define JAH_RQ_SUN_RAY_GLSL

float jahSunRay( vec3 origin, vec3 toSun, uint mask, float hardEnd, float range )
{
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq, tlas, gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
						   mask, origin, 0.0, toSun, hardEnd );
	while( rayQueryProceedEXT( rq ) ) {}
	if( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT )
		return 0.0;
	float vis = 1.0;
	if( range > hardEnd )
	{
		rayQueryEXT rqFade;
		rayQueryInitializeEXT( rqFade, tlas, gl_RayFlagsOpaqueEXT, mask, origin, hardEnd, toSun,
							   range );
		while( rayQueryProceedEXT( rqFade ) ) {}
		if( rayQueryGetIntersectionTypeEXT( rqFade, true ) ==
			gl_RayQueryCommittedIntersectionTriangleEXT )
		{
			const float t = rayQueryGetIntersectionTEXT( rqFade, true );
			vis = clamp( ( t - hardEnd ) / ( range - hardEnd ), 0.0, 1.0 );
		}
	}
	return vis;
}

#endif   // JAH_RQ_SUN_RAY_GLSL
