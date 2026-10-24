// THE AREA LIGHT'S STRATIFIED VISIBILITY - one text for every reader that shadows an area light by rays (PHOTON-7
// stage 7, 7-A, extracted from the cards' lamps mode, stage 5's 5-3 law): the cards (rq_card_movers.comp), the pixel's
// trace (rq_area_vis.comp) and, after the photon-7r merge, the hits (7-H).
//
// THE LAW (Heitz et al. 2018, the ratio estimator): the rectangle (its corner c0, its edges eu and ev, emitting down
// `emit` - the light's -Z - or both ways when two-sided) is cut into n x n strata; stratum k's sample point is jittered
// inside it by a hash of the RECEIVER'S WORLD POINT (jahAreaStrataKey: never the frame - a receiver's strata are one
// fixed set, so a reader that traces them a few a frame and averages converges to the same set's answer); its weight
// is the form factor f = cos_r cos_l / r^2 at the receiver; its visibility one finite ray from the receiver toward it,
// the clearance taken ALONG the ray and stopping `lampEnd` short of the light. The ratio sum( V f ) / sum( f ) is the
// visibility a reader multiplies the analytic LTC form factor by: the unshadowed energy stays exact, only the
// visibility is sampled.
// THE ONE ORIGIN FUNCTION (jahAreaRayStart): a lamp is at a finite distance, so an origin lifted along the normal sees
// an occluder from a point the surface does not (the edge moves by the lift's parallax: the 5-3 fix measured -6.4
// codes across a penumbra); each ray starts on its own segment at the lift's height, lift / cos from the receiver. A
// MERGE ITEM (the lead, 2026-10-23): photon-7r's one law (jah_ray_origin.glsl, P + geomN x bound) is the rival form -
// at the merge one body survives here, and every reader of this include takes it.
// Needs of the includer: `tlas` (accelerationStructureEXT), jahAlphaRayFlags() and JAH_RQ_PROCEED (jah_rq_alpha.glsl).
#ifndef JAH_AREA_STRATA_GLSL
#define JAH_AREA_STRATA_GLSL

/// The PCG integer hash (jah_probe_params.glsl's jahPcg, the gather's): the strata's jitter.
uint jahAreaPcg( uint v )
{
	const uint s = v * 747796405u + 2891336453u;
	const uint w = ( ( s >> ( ( s >> 28u ) + 4u ) ) ^ s ) * 277803737u;
	return ( w >> 22u ) ^ w;
}

/// THE STRATA'S KEY: a function of the receiver's world point alone.
uint jahAreaStrataKey( vec3 P )
{
	return jahAreaPcg( floatBitsToUint( P.x ) ^ jahAreaPcg( floatBitsToUint( P.y ) ^ jahAreaPcg( floatBitsToUint( P.z ) ) ) );
}

/// THE ONE ORIGIN FUNCTION: where a ray toward a lamp at a finite distance starts, along its own direction w (the
/// clearance `lift` measured off the surface whose normal is N).
float jahAreaRayStart( vec3 N, vec3 w, float lift )
{
	return lift / max( dot( N, w ), 1e-4 );
}

/// STRATUM k (0 .. n^2 - 1) of the rectangle as the receiver P (normal N; Ncos the normal its cosine is taken against)
/// sees it: returns the stratum's form factor f (0 - it lights nothing here: no ray), and, where f > 0, its visibility
/// V (0 or 1) by one ray against the TLAS under `mask`, the direction to its sample point and the emitter's cosine over
/// the squared distance (a reader that weighs the stratum by another lobe - the pixel's specular - takes them).
float jahAreaStratum( vec3 P, vec3 N, vec3 Ncos, float lift, vec3 c0, vec3 eu, vec3 ev, vec3 emit, bool twoSided, uint n,
					  uint k, uint key, uint mask, float lampEnd, out float V, out vec3 dirQ, out float clOverR2 )
{
	V = 0.0;
	dirQ = vec3( 0.0, 0.0, 1.0 );
	clOverR2 = 0.0;
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
	const float startQ = jahAreaRayStart( N, wq, lift );
	const vec3 originQ = P + wq * startQ;
	const float reachQ = sqrt( r2 ) - startQ - lampEnd;
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
