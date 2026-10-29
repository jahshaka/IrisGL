// THE PIXEL'S AREA LIGHT VISIBILITY'S PARAMETERS (PHOTON-7 stage 7, 7-A) - one block for its two jobs on both
// producers (the ray tier's rq_area_vis.comp / rq_area_vis_resolve.comp, a uniform block; the no-ray route's
// Jahshaka/ScreenAreaVis and its resolve, 7-6, a read-only storage block: the includer defines
// JAH_AREA_VIS_PARAMS_BLOCK first), mirrored member for member by AreaVisShared.h's AreaVisParams. Every member a vec4
// (std140 and std430 lay them out alike).
#ifndef JAH_AREA_VIS_PARAMS_GLSL
#define JAH_AREA_VIS_PARAMS_GLSL

/// The forward LTC set's ceiling (HlmsPbs's area LTC limit, EnginePrivate.h kAreaLtcLimit): a channel a light.
const int kAreaVisLights = 4;
/// THE GEOMETRY BOXES A FRAME HANDS THE TRACE (PHOTON-7 8B, B2; EnginePrivate.h kAreaVisRegions): the changed-region
/// list's movers and edits, merged on the host to fit - never dropped.
const int kAreaVisRegions = 32;

/// ONE AREA LIGHT as the trace reads it, in world space: the rectangle's corner (w = 1 two-sided), its two edges (eu.w
/// = the light's range), and the direction it shines (its -Z; the cards' `emit`).
struct JahAreaVisLight
{
	vec4 c0;
	vec4 eu;
	vec4 ev;
	vec4 emit;
};

#ifndef JAH_AREA_VIS_PARAMS_BLOCK
#define JAH_AREA_VIS_PARAMS_BLOCK layout( set = 0, binding = JAH_AREA_VIS_PARAMS_BINDING ) uniform
#endif
JAH_AREA_VIS_PARAMS_BLOCK AreaVisParams
{
	/// THE CAMERA'S IMAGE (rq_sun_contact.comp's five numbers: camPos.w 1 perspective / 0 orthographic; the image ray
	/// rayTL + rayRight u + rayDown v over the whole target, its component along fwd 1), and the previous frame's.
	vec4 camPos;
	vec4 rayTL;
	vec4 rayRight;
	vec4 rayDown;
	vec4 fwd;
	vec4 prevCamPos;
	vec4 prevRayTL;
	vec4 prevRayRight;
	vec4 prevRayDown;
	vec4 prevFwd;
	/// x = A, y = B of Camera::getProjectionParamsAB() (reverse-Z safe), z = the far plane.
	vec4 projParams;
	/// The view space's axes in world space (the G-buffer normal's rotation back).
	vec4 viewAxisX;
	vec4 viewAxisY;
	vec4 viewAxisZ;
	/// xy = the trace grid, zw = the full target.
	vec4 resolution;
	/// x, y = the clearance's terms, the producer's (the ray tier's drawn footprints and sub-pixel snap - the ray
	/// origin's one law; the no-ray route's 0 and the voxel cell), z = the TLAS mask (the shadow casters), w = the
	/// trace's divisor (pixels a texel on each axis).
	vec4 knobs;
	/// x = n (the strata a side), y = the stratum sequence's frame (the rest frame's own index at rest), z = the history
	/// blend's floor (1 / n^2), w = the view's age (0: the previous images hold nothing - never read).
	vec4 strata;
	/// x = the rest frame k (0: not at rest), y = N, the rest frames to the hold (n^2).
	vec4 rest;
	/// The alpha table's device address (float bits; 0 = none: the rays stay opaque).
	vec4 alpha;
	/// x = how many lights (<= kAreaVisLights).
	vec4 lights;
	JahAreaVisLight light[kAreaVisLights];
	/// THE GEOMETRY THAT CHANGED THIS FRAME (PHOTON-7 8B, B2: the rest breaks PER TEXEL, as the gather's cells do): x =
	/// how many boxes follow (the changed-region list's movers' swept boxes and edits), y = 1 = every texel is reached
	/// (the list overflowed or restarted). A resting texel whose light rays one meets leaves its rest for its moving
	/// history and rests again from the next frame (jah_area_vis_law.glsl); box i = [2i] its lower corner, [2i + 1] upper.
	vec4 regions;
	vec4 regionBox[2 * kAreaVisRegions];
} p;

/// The view distance a depth texel stands for (reverse-Z safe, both projections) - rq_sun_contact.comp's.
float jahAreaViewDistance( float rawDepth )
{
	if( p.camPos.w < 0.5 )
		return abs( ( rawDepth - p.projParams.x ) / p.projParams.y );
	return p.projParams.y / ( rawDepth - p.projParams.x );
}

/// The world point at image coordinate uv and view distance dist - through this frame's camera, or (prev) the last's.
vec3 jahAreaWorldAt( vec2 uv, float dist )
{
	const vec3 plane = p.rayTL.xyz + p.rayRight.xyz * uv.x + p.rayDown.xyz * uv.y;
	if( p.camPos.w < 0.5 )
		return p.camPos.xyz + plane + p.fwd.xyz * dist;
	return p.camPos.xyz + plane * dist;
}
vec3 jahAreaPrevWorldAt( vec2 uv, float dist )
{
	const vec3 plane = p.prevRayTL.xyz + p.prevRayRight.xyz * uv.x + p.prevRayDown.xyz * uv.y;
	if( p.prevCamPos.w < 0.5 )
		return p.prevCamPos.xyz + plane + p.prevFwd.xyz * dist;
	return p.prevCamPos.xyz + plane * dist;
}

/// A unit normal as one word (octahedral, two snorm16 halves) and back - the trace texel's surface word.
uint jahAreaPackNormal( vec3 n )
{
	vec2 o = n.xy / max( abs( n.x ) + abs( n.y ) + abs( n.z ), 1e-6 );
	if( n.z < 0.0 )
		o = ( 1.0 - abs( o.yx ) ) * vec2( o.x >= 0.0 ? 1.0 : -1.0, o.y >= 0.0 ? 1.0 : -1.0 );
	return packSnorm2x16( o );
}
vec3 jahAreaUnpackNormal( uint w )
{
	const vec2 o = unpackSnorm2x16( w );
	vec3 n = vec3( o, 1.0 - abs( o.x ) - abs( o.y ) );
	if( n.z < 0.0 )
		n.xy = ( 1.0 - abs( n.yx ) ) * vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
	return normalize( n );
}

/// THE SAME SURFACE, as a weight (the gather's plane test, its two tolerances - kPlaneTolerance 1 % of the view
/// distance off the plane, kNormalTolerance a dot of 0.9 - each a linear ramp): how well a point x (normal nx, at view
/// distance dist) lies on the plane of a texel's point q (normal nq). A grazing floor's depth changes by several per
/// cent between neighbouring texels; its plane does not.
float jahAreaSameSurface( vec3 q, vec3 nq, vec3 x, vec3 nx, float dist )
{
	const float planeTol = 0.01 * max( dist, 1e-3 );
	const float off = abs( dot( nq, x - q ) );
	const float wPlane = clamp( 1.0 - off / planeTol, 0.0, 1.0 );
	const float wNormal = clamp( ( dot( nq, nx ) - 0.9 ) / 0.1, 0.0, 1.0 );
	return wPlane * wNormal;
}

#endif
