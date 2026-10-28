// THE LAMP'S VISIBILITY AT A RASTERISED POINT (PHOTON-7 10-4 R9; L9: a lamp is blocked by geometry on every path).
// A Forward+ lamp - every point or spot light past the shadow-map budget, and every one that casts no shadow - had no
// visibility at all in the pixel: its light reached every pixel in its range through any wall. A point lamp is a
// point, so ONE ray from the surface to it is the exact answer (no strata, no history, no noise): 0 behind an opaque
// caster, the product of the panes' transmittance between (Ogre's blend: 1 - alpha a pane, jah_rq_alpha.glsl's
// JAH_RQ_PROCEED_TRANSMIT: every pane met, no cap), 1 in the open. The ray tier's TLAS, reached by its device address
// (the pass buffer carries it:
// FogHlmsListener, OgreFog.cpp - no descriptor; the fork's Forward+ hook custom_ps_fwdplus_lamp multiplies the lamp's
// attenuation by it). Only the shadow casters stop it (kRayMaskCaster), as every lamp's shadow ray (the cards' lamps).
// The origin is lifted along the geometric normal on the lamp's side by its float bound (jahRayOrigin, ONE FORM AT EVERY
// RAY) and the segment ends ON the lamp: a lamp inside an opaque fixture lights nothing outside it (a glass shade is a
// pane and passes its 1 - alpha).
// Needs jah_rq_finite.glsl, jah_rq_alpha.glsl (with JAH_ALPHA_TABLE defined) and jah_ray_origin.glsl before it; as an
// Hlms piece (JahLampRayLib) it is concatenated after them, its includes stripped. No Hlms directive mark in this file.

#ifndef JAH_LAMP_RAY_GLSL
#define JAH_LAMP_RAY_GLSL

/// World space: what reaches `surface` of the lamp at `lamp`. `geomN` the surface's unit geometric normal (either side),
/// `carried` the distance the point was reconstructed along (the eye's), `tlasAddress` the TLAS's device address.
float jahLampRay( uvec2 tlasAddress, vec3 surface, vec3 geomN, float carried, vec3 lamp )
{
	if( ( tlasAddress.x | tlasAddress.y ) == 0u || !finite3( surface ) || !finite3( lamp ) || !finite3( geomN ) )
		return 1.0;
	const vec3 n = dot( geomN, lamp - surface ) < 0.0 ? -geomN : geomN;
	const vec3 origin = jahRayOrigin( surface, n, carried, 0.0 );
	const vec3 d = lamp - origin;
	const float len = length( d );
	if( !( len > 1e-4 ) )
		return 1.0;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq, accelerationStructureEXT( tlasAddress ), jahAlphaRayFlags( gl_RayFlagsTerminateOnFirstHitEXT ),
						   0x01u, origin, 0.0, d * ( 1.0 / len ), len );
	float T = 1.0;
	JAH_RQ_PROCEED_TRANSMIT( rq, T )
	if( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT )
		return 0.0;
	return T;
}

/// THE SUN AT A POINT OF THE MEDIUM (the fog's march past its last shadow split, the one read's F9): one ray from the
/// world point `p` toward the sun (`toSun`, world) - a point of the medium has no surface, so no lift - against the
/// shadow casters (kRayMaskCaster), through the panes: 0 behind an opaque caster, the panes' transmittance, 1 open.
float jahSunMediumRay( uvec2 tlasAddress, vec3 p, vec3 toSun )
{
	if( ( tlasAddress.x | tlasAddress.y ) == 0u || !finite3( p ) || !finite3( toSun ) )
		return 1.0;
	rayQueryEXT rq;
	rayQueryInitializeEXT( rq, accelerationStructureEXT( tlasAddress ), jahAlphaRayFlags( gl_RayFlagsTerminateOnFirstHitEXT ),
						   0x01u, p, 0.0, normalize( toSun ), 1e6 );
	float T = 1.0;
	JAH_RQ_PROCEED_TRANSMIT( rq, T )
	if( rayQueryGetIntersectionTypeEXT( rq, true ) == gl_RayQueryCommittedIntersectionTriangleEXT )
		return 0.0;
	return T;
}

#endif   // JAH_LAMP_RAY_GLSL
