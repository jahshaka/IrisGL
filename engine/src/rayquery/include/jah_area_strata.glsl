// THE AREA LIGHT'S STRATIFIED VISIBILITY - one text for every reader that shadows an area light by rays (PHOTON-7
// stage 7, 7-A, extracted from the cards' lamps mode, stage 5's 5-3 law): the cards (rq_card_movers.comp), the pixel's
// trace (rq_area_vis.comp) and, after the photon-7r merge, the hits (7-H).
//
// THE LAW (Heitz et al. 2018, the ratio estimator): the rectangle (its corner c0, its edges eu and ev, emitting down
// `emit` - the light's -Z - or both ways when two-sided) is cut into n x n strata; stratum k's sample point is jittered
// inside it by a hash of the RECEIVER'S WORLD POINT (jahAreaStrataKey: never the frame - a receiver's strata are one
// fixed set, so a reader that traces them a few a frame and averages converges to the same set's answer); its weight
// is the form factor f = cos_r cos_l / r^2 at the receiver; its visibility one finite ray toward it from the receiver's
// lifted start, stopping `lampEnd` short of the light. The ratio sum( V f ) / sum( f ) is the
// visibility a reader multiplies the analytic LTC form factor by: the unshadowed energy stays exact, only the
// visibility is sampled.
// THE START (jahAreaRayOrigin): the ray origin's one law (jah_ray_origin.glsl, jahRayStart - this include is also an Hlms
// piece, which cannot include that file, so the law's point-target case is written here once): the receiver lifted
// along its normal by the clearance, and the ray AIMED FROM THERE at the stratum's point - so the segment tested ends
// on the light whatever the lift, and no start runs past an occluder at a grazing angle.
// THE STRATUM'S GEOMETRY (jahAreaStratumAt: its point, form factor, ray start and reach) is plain GLSL, read by every
// producer - the ray tier through glslang, the no-ray route's screen job (Jahshaka/ScreenAreaVis, PHOTON-7 stage 7,
// 7-6) as the Hlms piece JahAreaStrata - and its VISIBILITY is the producer's: the ray below (jahAreaStratum, compiled
// only for an includer that defines JAH_AREA_STRATA_RAY - glslang predefines every extension's macro it SUPPORTS, so
// GL_EXT_ray_query cannot tell), the voxel chain's march on the no-ray route.
// Needs of the ray's includer: `tlas` (accelerationStructureEXT), jahAlphaRayFlags() and JAH_RQ_PROCEED
// (jah_rq_alpha.glsl).
#ifndef JAH_AREA_STRATA_GLSL
#define JAH_AREA_STRATA_GLSL

/// The PCG integer hash (jah_probe_params.glsl's jahPcg, the gather's): the strata's jitter.
uint jahAreaPcg( uint v )
{
	const uint s = v * 747796405u + 2891336453u;
	const uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

/// THE STRATA A SIDE, BOUNDED WHERE A SHADER READS THEM (the loop law: a bound read from a buffer is clamped in the
/// shader): the largest any tier asks (GiQualityFacts::cardAreaStrata, Types.h: 8 - Low 4, Medium 6).
const uint kAreaStrataMax = 8u;

/// THE STRATA'S KEY: a function of the receiver's world point alone.
uint jahAreaStrataKey( vec3 P )
{
	return jahAreaPcg( floatBitsToUint( P.x ) ^ jahAreaPcg( floatBitsToUint( P.y ) ^ jahAreaPcg( floatBitsToUint( P.z ) ) ) );
}

/// THE START: where a ray from P (normal N) toward a point in direction w starts, the clearance `lift` off the surface.
vec3 jahAreaRayOrigin( vec3 P, vec3 N, vec3 w, float lift )
{
	return P + N * lift;   // (w: the ray it starts - aimed by the caller from here at its point)
}

/// STRATUM k (0 .. n^2 - 1) of the rectangle as the receiver P (normal N; Ncos the normal its cosine is taken against)
/// sees it - ITS GEOMETRY: returns the stratum's form factor f (0 - it lights nothing here: no ray), and, where f > 0,
/// the direction to its sample point, the emitter's cosine over the squared distance (a reader that weighs the stratum
/// by another lobe - the pixel's specular - takes them) and the visibility ray: its start (jahAreaRayOrigin), its
/// direction from there to the sample point and its reach (stopping `lampEnd` short of the light: <= 0, nothing to
/// test, V = 1).
float jahAreaStratumAt( vec3 P, vec3 N, vec3 Ncos, float lift, vec3 c0, vec3 eu, vec3 ev, vec3 emit, bool twoSided,
						uint n, uint k, uint key, float lampEnd, out vec3 dirQ, out float clOverR2, out vec3 originQ,
						out vec3 rayQ, out float reachQ )
{
	dirQ = vec3( 0.0, 0.0, 1.0 );
	clOverR2 = 0.0;
	originQ = P;
	rayQ = dirQ;
	reachQ = 0.0;
	const float xu = ( float( k % n ) + float( jahAreaPcg( key + 2u * k ) ) * 2.3283064e-10 ) / float( n );
	const float xv = ( float( k / n ) + float( jahAreaPcg( key + 2u * k + 1u ) ) * 2.3283064e-10 ) / float( n );
	const vec3 Q = c0 + eu * xu + ev * xv;
	const vec3 dq = Q - P;
	const float r2 = dot( dq, dq );
	if( !( r2 > 0.0 ) )
		return 0.0;
	const vec3 wq = dq * inversesqrt( r2 );
	const float cr = max( dot( Ncos, wq ), 0.0 );
	const float cl = twoSided ? abs( dot( emit, wq ) ) : max( -dot( emit, wq ), 0.0 );
	const float f = cr * cl / r2;
	if( !( f > 0.0 ) )
		return 0.0;
	dirQ = wq;
	clOverR2 = cl / r2;
	originQ = jahAreaRayOrigin( P, N, wq, lift );
	const vec3 toQ = Q - originQ;
	const float lenQ = length( toQ );
	rayQ = lenQ > 0.0 ? toQ / lenQ : wq;
	reachQ = lenQ - lampEnd;
	return f;
}

#ifdef JAH_AREA_STRATA_RAY
/// ...AND ITS VISIBILITY BY ONE RAY (the ray tier's readers): V (0 or 1) against the TLAS under `mask`.
float jahAreaStratum( vec3 P, vec3 N, vec3 Ncos, float lift, vec3 c0, vec3 eu, vec3 ev, vec3 emit, bool twoSided, uint n,
					  uint k, uint key, uint mask, float lampEnd, out float V, out vec3 dirQ, out float clOverR2 )
{
	V = 0.0;
	vec3 originQ, rayQ;
	float reachQ;
	const float f = jahAreaStratumAt( P, N, Ncos, lift, c0, eu, ev, emit, twoSided, n, k, key, lampEnd, dirQ, clOverR2,
									  originQ, rayQ, reachQ );
	if( !( f > 0.0 ) )
		return 0.0;
	const vec3 wq = rayQ;
	V = 1.0;
	if( reachQ > 0.0 )
	{
		rayQueryEXT rqa;
		rayQueryInitializeEXT( rqa, tlas, jahAlphaRayFlags( gl_RayFlagsTerminateOnFirstHitEXT ), mask, originQ, 0.0, wq,
							   reachQ );
		JAH_RQ_PROCEED( rqa )
		if( rayQueryGetIntersectionTypeEXT( rqa, true ) == gl_RayQueryCommittedIntersectionTriangleEXT )
			V = 0.0;
	}
	return f;
}
#endif

#endif
