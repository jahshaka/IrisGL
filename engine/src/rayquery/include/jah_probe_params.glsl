// THE SCREEN-PROBE GATHER'S PARAMETERS — one uniform block, three jobs
// (GATHER-1a, 2026-09-21).
//
// The placement job, the trace and the integrate all read the SAME buffer, so
// the block lives in one file and the C++ mirror (`GatherParams` in
// OgreScreenProbeGather.cpp) is written against one declaration. std140 over
// vec4s only, so the C++ layout is the GLSL layout by construction.
//
// A CALLER DEFINES `JAH_PROBE_PARAMS_BINDING` before including this.

#ifndef JAH_PROBE_PARAMS_GLSL
#define JAH_PROBE_PARAMS_GLSL

#include "jah_rq_finite.glsl"

const int kMaxCascades = 4;
/// One probe's threadgroup is 8x8; the octahedral map is at most that, so a
/// probe traces at most 64 rays, one per thread. The C++ constant
/// `kGatherOctResMax` and this must agree.
const int kOctResMax = 8;

layout( set = 0, binding = JAH_PROBE_PARAMS_BINDING ) uniform ProbeParams
{
	/// xyz = the camera in world space, w = 1 perspective / 0 orthographic.
	vec4 camPos;
	/// The world-space ray basis of the 0..1 image (rq_reflect.comp's).
	vec4 rayTL;
	vec4 rayRight;
	vec4 rayDown;
	vec4 fwd;
	/// x = A, y = B of Camera::getProjectionParamsAB() (reverse-Z safe),
	/// z = the far clip.
	vec4 projParams;
	/// xy = the PROBE GRID in probes, zw = the full-resolution target size.
	vec4 resolution;
	/// x = the probe stride in pixels, y = the ray's maximum length in world
	/// units, z = the frame index (the sample sequence's only input),
	/// w = how many cascades are bound.
	vec4 knobs;
	/// x = 1 when the voxel textures carry the anisotropic slots, y = the
	/// surface bias in world units, z = 1 when a sky cubemap is bound,
	/// w = the octahedral map's resolution (rays per probe = w*w).
	vec4 knobs2;
	/// x = how many UNIFORM probes there are (the grid), y = how many ADAPTIVE
	/// probes a frame may add, z = how many probes a row of the atlas holds,
	/// w = unused.
	vec4 knobs3;
	/// THE ADAPTIVE TEST and the two arms. x = how far a cell's pixel may lie
	/// off its probe's plane before the cell wants a second probe, as a
	/// FRACTION OF THE PIXEL'S VIEW DISTANCE (an angular tolerance: a wall
	/// fifty metres away is allowed to be fifty times further off the plane
	/// than one at a metre, which is what makes one number work at every
	/// depth); y = the smallest normal agreement (a dot product) that still
	/// counts as the same surface; z = 1 stands the FAR TERM down (a ray with
	/// no hit inside tMax reads the sky directly); w = 1 puts the probe at its
	/// cell's centre instead of jittering it inside the cell.
	vec4 plane;
	/// The colour an escaping ray reads when no sky cubemap is bound.
	vec4 skyColour;
	/// The VIEW space's three axes in world space.
	vec4 viewAxisX;
	vec4 viewAxisY;
	vec4 viewAxisZ;
	/// Per cascade: xyz = the voxel box's world origin, w = the cascade's
	/// radiance multiplier (the bake's normalisation undone — DRAG-1).
	vec4 voxelOrigin[kMaxCascades];
	/// Per cascade: xyz = 1 / the box's world size, w = one cell in world units.
	vec4 voxelInvSize[kMaxCascades];
} p;

/// ONE PROBE'S RECORD — where it sits, what it faces, what it integrated.
/// std430: three vec4s, 48 bytes, and the C++ mirror is `ProbeRecord`.
struct JahProbeRecord
{
	/// xyz = the probe's world position (already lifted off the surface by
	/// nothing — the bias is applied per ray), w = 1 when it sits on a surface.
	vec4 posW;
	/// xyz = its world normal, w = the index of this cell's ADAPTIVE probe plus
	/// one (0 = the cell has none).
	vec4 normalW;
	/// rgb = the mean radiance over the cosine-weighted hemisphere, i.e. E/pi,
	/// which is exactly what HlmsPbs's `envColourD` is; w = 1 when the probe
	/// answered at all.
	vec4 irradiance;
};

/// THE OCTAHEDRAL MAP, hemisphere form: the unit square onto the upper half of
/// the sphere in the probe's tangent frame. Returns the UNNORMALISED
/// octahedron point, whose length is what the solid-angle weight needs (see
/// jahOctSolidAngle).
///
/// The mapping is a 45-degree rotation of the square onto the diamond
/// |x| + |y| <= 1 and the octahedron face z = 1 - |x| - |y| over it.
vec3 jahOctPoint( vec2 uv )
{
	const vec2 f = uv * 2.0 - 1.0;
	const vec2 d = vec2( ( f.x + f.y ) * 0.5, ( f.x - f.y ) * 0.5 );
	return vec3( d.x, d.y, 1.0 - abs( d.x ) - abs( d.y ) );
}

/// THE SOLID ANGLE one texel of that map subtends, EXACTLY, and the whole
/// reason the gather's magnitude can be argued rather than tuned.
///
/// The octahedron's face lies on the plane |x| + |y| + z = 1, whose unit normal
/// makes a constant angle with every point of it, so the projected area element
/// has a closed form: dOmega = dA_xy / r^3, where dA_xy is the area element in
/// the (x,y) plane and r the length of the octahedron point. The square-to-
/// diamond map has |det| = 1/2 and the square spans [-1,1]^2, so
/// dA_xy = 2 du dv — and one texel of an NxN map is du dv = 1/N^2. Hence
///
///     dOmega( texel ) = 2 / ( r^3 * N^2 )
///
/// Summed over a jittered NxN set this integrates to 2*pi and, weighted by the
/// cosine, to pi — verified numerically to four figures at N = 512 and to
/// within the stratified estimator's own variance at N = 8 (the gather's grid).
float jahOctSolidAngle( vec3 octPoint, float octRes )
{
	const float r = max( length( octPoint ), 1e-6 );
	return 2.0 / ( r * r * r * octRes * octRes );
}

/// The PCG integer hash every stochastic input of the gather is a pure function
/// of. No clock, no subgroup identity, no atomic whose order is the scheduler's
/// — the idiom rq_reflect.comp already uses, and the reason a still frame is
/// byte-deterministic.
uint jahPcg( uint v )
{
	uint state = v * 747796405u + 2891336453u;
	uint word = ( ( state >> ( ( state >> 28u ) + 4u ) ) ^ state ) * 277803737u;
	return ( word >> 22u ) ^ word;
}
/// Two numbers on [0,1) from (cell, index, frame). The three enter one hash
/// with distinct odd multipliers.
vec2 jahSample2( ivec2 cell, uint index, uint frame )
{
	const uint h = jahPcg( uint( cell.x ) * 1973u + uint( cell.y ) * 9277u + index * 20749u +
						   frame * 26699u );
	return vec2( float( h & 0xFFFFu ) / 65536.0, float( h >> 16u ) / 65536.0 );
}

/// The view distance a depth texel stands for (reverse-Z safe, both
/// projections) and the world point a 0..1 image coordinate at that distance
/// names. rq_reflect.comp's reconstruction, in the same two lines.
float jahViewDistance( float rawDepth )
{
	if( p.camPos.w < 0.5 )
		return abs( ( rawDepth - p.projParams.x ) / p.projParams.y );
	return p.projParams.y / ( rawDepth - p.projParams.x );
}

vec3 jahWorldAt( vec2 uv, float dist )
{
	const vec3 plane = p.rayTL.xyz + p.rayRight.xyz * uv.x + p.rayDown.xyz * uv.y;
	if( p.camPos.w < 0.5 )
		return p.camPos.xyz + plane + p.fwd.xyz * dist;
	return p.camPos.xyz + plane * dist;
}

/// THE PROBE'S TANGENT FRAME around its normal (Duff et al., branchless) — the
/// same one rq_reflect.comp builds, and the frame the octahedral map is
/// expressed in. Both jobs must build it IDENTICALLY or the trace and the
/// integrate would disagree about which direction a texel holds.
void jahTangentFrame( vec3 n, out vec3 t, out vec3 b )
{
	const float sg = n.z >= 0.0 ? 1.0 : -1.0;
	const float a0 = -1.0 / ( sg + n.z );
	const float b0 = n.x * n.y * a0;
	t = vec3( 1.0 + sg * n.x * n.x * a0, sg * b0, -sg * n.x );
	b = vec3( b0, sg + n.y * n.y * a0, -n.y );
}

#endif   // JAH_PROBE_PARAMS_GLSL
