// THE RADIANCE LEAVING A RAY'S HIT — ONE FUNCTION, TWO CALLERS (PHOTON-GATHER-1d,
// GA-1e; PHOTON-CARDS-2's `hitRadiance`, moved here out of rq_reflect.comp).
//
// THE CARD FIRST, THE VOXELS WHERE NO CARD ANSWERS. The surface cache holds, for
// every carded instance near the camera, the lit radiance of its surface at the
// atlas's texel pitch — a 2 m panel on a 128 page is 1.6 cm a texel, where the
// finest voxel a ray can read is two 7.8 cm cells (15.6 cm) wide. So a hit asks
// the card of its own instance first (`jahCardRadiance`, jah_rq_card.glsl: the
// instance by the TLAS's instanceCustomIndex = the item slot, the pick facing
// the hit's GEOMETRIC normal, the through-the-wall rule, the texel) and takes the
// card's radiance, its diffuse view term restored for the ray (PHOTON-CARDS-5,
// jah_card_view.glsl); the voxels answer where no card does (an instance with
// no cards, a point no card of it describes, a scene without a cache, a card
// whose indirect half has not been marched yet).
//
// THE CURRENCY IS THE FOOTPRINT (the lead's decision after the CARDS-2 audit):
// the sample's footprint at the hit, in world metres, decides the VOXEL read —
// the mip whose texel is that wide. Whether it also GATES THE CARD is the
// caller's (`cardGated`), because each caller knows what one of its samples
// stands for:
//   * the REFLECTION (rq_reflect.comp) gates: its footprint is the spacing
//     between the samples its temporal mean holds over the lobe,
//     2 t alpha / sqrt(N), N = 1/knobs2.w, and the card is read only while that
//     is at most JAH_CARD_FOOTPRINT_TEXELS of the picked card's texels (the
//     atlas has no mips until SC-2, so a glossy lobe wider than that on a
//     texel-exact read is speckle — measured on reflections). A mirror (alpha
//     at or below the mirror cut) reads the card texel-exact and the containing
//     cascade whole at LOD 0 (`mirror`).
//   * the GATHER (rq_probe_gather.comp) passes NO gate (the fix round's
//     decision, C1's "each caller derives its footprint"): its rays are 64 (36
//     at Medium) STRATIFIED directions per probe, re-jittered every frame and
//     averaged by the SH projection and the pixel history, so a texel-exact card
//     read at each ray's hit is an UNBIASED sample of the surface's radiance and
//     the finer representation wins until SC-2's mips give the read a floor. Its
//     footprint, t x sqrt(2/rays) — one ray stands for one octahedral texel's
//     solid angle, 0.177 t at 64 rays and 0.236 t at 36 — still picks the voxel
//     mip where no card answers.
//
// HOW A CALLER BINDS IT: include jah_rq_hit.glsl (the voxel read) and
// jah_rq_card.glsl (the card read, behind its own macros) first.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_RADIANCE_GLSL
#define JAH_RQ_HIT_RADIANCE_GLSL

/// `slot` = the hit instance's item slot (0xFFFFFFFF: no instance the card
/// tables know — the voxels answer); `hitNormal` = the hit triangle's geometric
/// normal facing the ray's origin; `dir` = the ray's direction; `footprint` =
/// the caller's sample footprint at the hit in metres; `mirror` = the sample is
/// a mirror's (the card texel-exact, the voxels at LOD 0 with no crossfade);
/// `cardGated` = the footprint also gates the card read (the reflection's;
/// the gather's is false — see above). `ok` is false where neither the card
/// nor the cascades can shade the hit — and the caller then APPENDS THE HIT TO
/// THE HIT LIST (jah_rq_hit_record.glsl, PHOTON-HIT-SHADE-1), where HlmsAtom's
/// decode shades it with the scene's own lighting text. A hit on a MOVER or a
/// RIGGED item never asks this function at all: the caches cannot hold it.
/// ...and WHICH of the two answered (`source`: 1 the card, 2 the cascades, 0
/// neither) — the photon view's Hits picture (PHOTON-VIEW-1) reads it; the answer
/// itself is the same arithmetic as jahHitRadiance's, which is this function.
#ifndef JAH_MOVER_SKY_LENGTH
/// How far a hit looks for a MOVER above it (metres): movers are objects at hand's to
/// room scale, and a mover further than this subtends too little of the hemisphere to
/// read in the share (a 2 m cube 4 m up covers 6 % of a cosine hemisphere).
#define JAH_MOVER_SKY_LENGTH 4.0
#endif
/// THE ROTATION OF THE FOUR RAYS (MOVER-OCCLUSION-1 fix round): the caller sets it per
/// hit before the read — a hash of the pixel (or the probe's cell and ray) and the frame —
/// so the four directions turn from hit to hit and frame to frame and the histories the
/// reads land in average the estimate to the true fraction. 0 = unrotated.
uint jahHitSeed = 0u;
#ifndef JAH_HIT_MOVERS_ON
/// A ray-traced mover exists in the scene (the host's flag); none = no rays at all.
#define JAH_HIT_MOVERS_ON true
#endif

/// THE HEMISPHERE ABOVE A HIT, AS THE MOVERS LEAVE IT (MOVER-OCCLUSION-1): the store
/// (voxels, cards) is built without movers, so the sky and the bounce it holds at a
/// point a mover hangs over are too bright on every hit read. Four cosine-distributed
/// rays — two rings of the cosine hemisphere's equal-weight quadrature (u = 1/4 and 3/4:
/// 30 and 60 degrees off the normal), two rays each, opposite in azimuth, the rings a
/// quarter turn apart, the whole set turned by the hit's seed (the golden angle per
/// frame on a per-pixel hash) — against the MOVERS' near copies alone (kRayMaskMover; the
/// still world is in the store already), cut-outs honoured: the fraction that escapes.
float jahMoverSkyVisibility( vec3 p, vec3 n )
{
	if( !( JAH_HIT_MOVERS_ON ) )
		return 1.0;
	const vec3 t = abs( n.z ) < 0.999 ? normalize( cross( n, vec3( 0.0, 0.0, 1.0 ) ) )
									  : vec3( 1.0, 0.0, 0.0 );
	const vec3 b = cross( n, t );
	uint h = jahHitSeed * 747796405u + 2891336453u;
	h = ( ( h >> ( ( h >> 28u ) + 4u ) ) ^ h ) * 277803737u;
	h = ( h >> 22u ) ^ h;
	const float phi0 = 6.2831853 * ( float( h ) * ( 1.0 / 4294967296.0 ) );
	float open = 0.0;
	for( int k = 0; k < 4; ++k )
	{
		const float u = ( k < 2 ) ? 0.25 : 0.75;
		const float r = sqrt( u );
		const float z = sqrt( 1.0 - u );
		const float phi = phi0 + 3.1415927 * float( k & 1 ) + 1.5707963 * float( k >> 1 );
		const vec3 d = normalize( n * z + ( t * cos( phi ) + b * sin( phi ) ) * r );
		rayQueryEXT q;
		rayQueryInitializeEXT( q, tlas, jahAlphaRayFlags( gl_RayFlagsTerminateOnFirstHitEXT ), 0x02u,
							   p + n * 0.02, 0.0, d, JAH_MOVER_SKY_LENGTH );
		JAH_RQ_PROCEED( q )
		if( rayQueryGetIntersectionTypeEXT( q, true ) != gl_RayQueryCommittedIntersectionTriangleEXT )
			open += 0.25;
	}
	return open;
}

vec3 jahHitRadianceStore( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
						  bool mirror, bool cardGated, out bool ok, out uint source );

vec3 jahHitRadianceSourced( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
							bool mirror, bool cardGated, out bool ok, out uint source )
{
	jahHitIndirectGate = jahMoverSkyVisibility( hitPos, normalize( hitNormal ) );
	const vec3 r = jahHitRadianceStore( slot, hitPos, hitNormal, dir, footprint, mirror, cardGated, ok, source );
	jahHitIndirectGate = 1.0;
	return r;
}

vec3 jahHitRadianceStore( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
						  bool mirror, bool cardGated, out bool ok, out uint source )
{
	{
		// The card read restores the diffuse lobe's view term for THIS ray
		// (PHOTON-CARDS-5): the viewer is where the ray came from.
		const vec3 card = jahCardRadiance( slot, hitPos, hitNormal, -dir,
										   ( mirror || !cardGated ) ? 0.0 : footprint, ok );
		if( ok )
		{
			source = 1u;
			return card;
		}
	}
	const vec3 voxel = jahVoxelRadiance( hitPos, dir, footprint, mirror, ok );
	source = ok ? 2u : 0u;
	return voxel;
}

vec3 jahHitRadiance( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					 bool mirror, bool cardGated, out bool ok )
{
	uint source;
	return jahHitRadianceSourced( slot, hitPos, hitNormal, dir, footprint, mirror, cardGated, ok, source );
}

#endif   // JAH_RQ_HIT_RADIANCE_GLSL
