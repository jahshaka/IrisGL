// THE SCREEN-PROBE GATHER'S PARAMETERS — one uniform block, four jobs
// (GATHER-1a, 2026-09-21; the filter PHOTON-GATHER-1b).
//
// The placement job, the trace, the filter and the integrate all read the SAME buffer, so
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
	/// ray start's floor off the surface in world units (an epsilon; the trace
	/// takes the larger of it and 1e-4 of the probe's view distance), z = 1
	/// when a sky cubemap is bound,
	/// w = the octahedral map's resolution (rays per probe = w*w).
	vec4 knobs2;
	/// x = how many UNIFORM probes there are (the grid), y = how many ADAPTIVE
	/// probes a frame may add, z = how many probes a row of the atlas holds,
	/// w = THE FAR QUERY's end (the far plane, world units; ATOM-FARBLAS-1) --
	/// 0, or anything not beyond knobs.y, is the far query off.
	vec4 knobs3;
	/// THE ADAPTIVE TEST and the two arms. x = how far a cell's pixel may lie
	/// off its probe's plane before the cell wants a second probe, as a
	/// FRACTION OF THE PIXEL'S VIEW DISTANCE (an angular tolerance: a wall
	/// fifty metres away is allowed to be fifty times further off the plane
	/// than one at a metre, which is what makes one number work at every
	/// depth); y = the smallest normal agreement (a dot product) that still
	/// counts as the same surface; z = THE FAR QUERY'S tMin (the near length
	/// less the widest coarse-vs-fine gap, ATOM-FARBLAS-1 audit F2); w = 1 puts the probe at its
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
	/// PHOTON-GATHER-1b. x = the SH bands the integrate evaluates (9 = L0..L2,
	/// 4 = L0..L1: the measurement arm, GatherTuning::shBands), y = the weight
	/// floor under which a pixel is left to the fallback (w = 0), z = 1 runs the
	/// filter in probe space (0 = the filtered map is the raw one: the A/B that
	/// prices the filter), w = unused.
	vec4 knobs4;
	/// THE PREVIOUS FRAME'S CAMERA (PHOTON-GATHER-1c), in the same five numbers
	/// as this frame's (camPos .. fwd above) — what the integrate's pixel
	/// history inverts (jah_reproject.glsl). On a view's first frame the C++
	/// side writes this frame's own basis here, and nothing reads it
	/// (knobs5.x = 0).
	vec4 prevCamPos;
	vec4 prevRayTL;
	vec4 prevRayRight;
	vec4 prevRayDown;
	vec4 prevFwd;
	/// THE PIXEL HISTORY (PHOTON-GATHER-1c). x = THE VIEW'S AGE: how many
	/// consecutive frames this view's history has been written (0 = none yet —
	/// a first frame, a resize, a scene bind, a tuning change, the history
	/// switched back on — and then the previous images hold nothing and are
	/// never read); y = the history's blend FLOOR (the smallest weight a new
	/// frame takes: 1 / the frames it remembers); z = 1 runs the history (0 =
	/// `JAHSHAKA_GATHER_NO_TEMPORAL`, the measurement lever); w = 1 accepts every
	/// reprojected texel (the distance and normal tests off — the test door
	/// GatherTuning::historyValidationOff; 0 shipped).
	vec4 knobs5;
	/// THE SURFACE CACHE THE HITS READ FIRST (PHOTON-GATHER-1d, GA-1e — the
	/// reflection's `p.cards`, word for word): x = the instance-table entries
	/// bound (0 = the scene holds no cache: every hit reads the voxels), y = the
	/// card records bound, z = the footprint gate in card texels
	/// (Types.h kCardFootprintTexels), w = the per-slot geometry-row entries
	/// bound (the hit's geometric normal; 0 = none — the reversed ray faces).
	/// Read by the trace alone.
	vec4 cards;
	/// THE REST MEAN (PHOTON-GATHER-1d, rq_probe_integrate.comp): x = the rest
	/// frame k (0 = the camera, the lighting or the scene moved this frame — not
	/// at rest; k >= 1 = the k-th consecutive frame at rest), y = K, the rest
	/// frames after which the answer IS the rest mean and the host holds.
	vec4 knobs6;
	/// THE HIT RECORD (PHOTON-HIT-SHADE-1; the trace's, jah_rq_hit_record.glsl):
	/// hitList: x = the list's capacity, y = its grid width, z = the GPU scene
	/// instance entries bound, w = 1 when the list is bound. hitSun: xyz = towards
	/// the sun, w = 1 when the pass has one. hitSun2: x = the sun ray's TLAS mask,
	/// y = its minimum lift, z = its length, w = the far copies' lift (farOverlap).
	vec4 hitList;
	vec4 hitSun;
	vec4 hitSun2;
} p;

/// ONE PROBE'S RECORD — where it sits, what it faces, what it integrated.
/// std430: ten vec4s, 160 bytes, and the C++ mirror is `kRecordBytes`
/// (OgreScreenProbeGather.cpp).
struct JahProbeRecord
{
	/// xyz = the probe's world position (already lifted off the surface by
	/// nothing — the bias is applied per ray), w = 1 when it sits on a surface.
	vec4 posW;
	/// xyz = its world normal, w = the index of this cell's ADAPTIVE probe plus
	/// one (0 = the cell has none); on an ADAPTIVE probe, its own cell's index
	/// plus one.
	vec4 normalW;
	/// rgb = the irradiance at the probe's OWN normal as E/pi (the SH below,
	/// evaluated there — a reading for status and suites), w = 1 when the probe
	/// answered at all.
	vec4 irradiance;
	/// THE SH9 RECORD (PHOTON-GATHER-1b item 3): the filtered radiance map
	/// projected onto the nine real spherical harmonics L0..L2 over WORLD axes,
	/// already CONVOLVED with the clamped cosine and divided by pi, its DC
	/// coefficient ANCHORED so that the SH at the probe's own normal is the exact
	/// quadrature there (rq_probe_filter.comp says why) — so
	/// jahProbeShEval( record, n ) IS E(n)/pi, the unit `envColourD` takes, at
	/// the normal n. Coefficient k, channel c lives at flat float 3k + c of the
	/// seven vec4s (27 floats; the 28th is unused).
	vec4 sh[7];
};

/// THE NINE REAL SPHERICAL HARMONICS L0..L2 at a unit direction, in the
/// standard order (Y00; Y1-1, Y10, Y11; Y2-2, Y2-1, Y20, Y21, Y22).
void jahShBasis9( vec3 d, out float y[9] )
{
	y[0] = 0.282095;
	y[1] = 0.488603 * d.y;
	y[2] = 0.488603 * d.z;
	y[3] = 0.488603 * d.x;
	y[4] = 1.092548 * d.x * d.y;
	y[5] = 1.092548 * d.y * d.z;
	y[6] = 0.315392 * ( 3.0 * d.z * d.z - 1.0 );
	y[7] = 1.092548 * d.x * d.z;
	y[8] = 0.546274 * ( d.x * d.x - d.y * d.y );
}

/// The clamped-cosine convolution per band, divided by pi (Ramamoorthi and
/// Hanrahan's A_l / pi: 1, 2/3, 1/4). A coefficient multiplied by this turns a
/// RADIANCE projection into an E/pi one.
float jahShCosineOverPi( int k )
{
	return k == 0 ? 1.0 : ( k < 4 ? 2.0 / 3.0 : 0.25 );
}

/// Coefficient k of a record's SH (rgb).
vec3 jahProbeShCoeff( JahProbeRecord r, int k )
{
	const int f = 3 * k;
	return vec3( r.sh[( f ) >> 2][( f ) & 3], r.sh[( f + 1 ) >> 2][( f + 1 ) & 3],
				 r.sh[( f + 2 ) >> 2][( f + 2 ) & 3] );
}

/// E(n)/pi from a record's SH at the unit normal n, over the first `bands`
/// coefficients (9 = L0..L2; 4 = L0..L1). Clamped at zero: a band-limited
/// reconstruction rings below zero where the radiance has a hard edge, and a
/// negative irradiance is not light.
vec3 jahProbeShEval( JahProbeRecord r, vec3 n, int bands )
{
	float y[9];
	jahShBasis9( n, y );
	vec3 e = vec3( 0.0 );
	for( int k = 0; k < 9; ++k )
	{
		if( k >= bands )
			break;
		e += jahProbeShCoeff( r, k ) * y[k];
	}
	return max( e, vec3( 0.0 ) );
}

/// ...the same sum WITHOUT the clamp — the anchor needs the raw value the DC
/// coefficient corrects.
vec3 jahProbeShEval9Raw( JahProbeRecord r, vec3 n )
{
	float y[9];
	jahShBasis9( n, y );
	vec3 e = vec3( 0.0 );
	for( int k = 0; k < 9; ++k )
		e += jahProbeShCoeff( r, k ) * y[k];
	return e;
}

/// THE PLANE TEST, as a WEIGHT (PHOTON-GATHER-1b): how well a surface at
/// `x` with normal `nx`, at view distance `dist`, belongs to the plane of a
/// probe at `probePos` with normal `probeN`. The same two tolerances the
/// placement job's adaptive test uses — the distance off the plane as a
/// fraction of the view distance (an angular tolerance), and the normals' dot —
/// each turned from a cut into a linear ramp, so a pixel on the probe's own
/// plane weighs 1 and one at either tolerance weighs 0.
float jahPlaneWeight( vec3 probePos, vec3 probeN, vec3 x, vec3 nx, float dist )
{
	const float planeTol = max( p.plane.x, 1e-4 ) * max( dist, 1e-3 );
	const float normalTol = p.plane.y;
	const float offPlane = abs( dot( probeN, x - probePos ) );
	const float wp = clamp( 1.0 - offPlane / planeTol, 0.0, 1.0 );
	const float wn = clamp( ( dot( probeN, nx ) - normalTol ) / max( 1.0 - normalTol, 1e-3 ), 0.0,
							1.0 );
	return wp * wn;
}

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

/// ...and its INVERSE: the unit square coordinate of a direction in the
/// probe's tangent frame (z >= 0 — the caller rejects the lower hemisphere).
/// The octahedron point is the direction divided by its L1 norm; the 45-degree
/// rotation is its own inverse up to the factor the forward map halved.
vec2 jahOctUv( vec3 dLocal )
{
	const vec3 o = dLocal / max( abs( dLocal.x ) + abs( dLocal.y ) + dLocal.z, 1e-6 );
	const vec2 f = vec2( o.x + o.y, o.x - o.y );
	return clamp( f * 0.5 + 0.5, vec2( 0.0 ), vec2( 1.0 ) );
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

/// The view distance of a WORLD point (the same linear depth jahViewDistance
/// decodes), for a probe whose record carries only its position.
float jahViewDistanceOf( vec3 world )
{
	return max( dot( world - p.camPos.xyz, p.fwd.xyz ), 1e-3 );
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
