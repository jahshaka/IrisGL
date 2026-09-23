// A RAY'S HIT READ FROM THE SURFACE CACHE — the one copy (PHOTON-CARDS-2 part
// B, SC-1d). `SurfaceCache::readAt` (OgreSurfaceCache.cpp) is the same
// algorithm on the CPU and stays as the TEST reference: gi.card_read_parity
// asserts the two pick the same card and the same texel.
//
// WHAT A CARD IS (SurfaceCache.h, CardRec): an axis-aligned rectangle in WORLD
// space around one instance, captured orthographically along its outward axis
// into the atlas — depth from the card's near plane, and (the CardLight job)
// the lit RADIANCE: direct + the voxel march's indirect + emissive, at V = N.
// Reading one at a world point is three dot products and a texel fetch; no
// per-instance matrix exists anywhere in the read.
//
// THE PICK (Lumen's card selection), over the cards of the HIT INSTANCE only —
// the instance table is indexed by the TLAS instance's `instanceCustomIndex`,
// which the ray tier writes as the object's item slot:
//   * the card must have been captured, and must FACE the given direction by
//     more than 0.05 (a card edge-on to the surface says nothing);
//   * the point projected with three dot products must land inside it;
//   * THE THROUGH-THE-WALL RULE: the depth the capture stored at that texel
//     must agree with the point's own distance from the card's near plane
//     within two card texels (plus a millimetre) — the capture saw the FIRST
//     surface along its axis, so a point further than that is hidden behind
//     something and this card does not describe it;
//   * of the cards that pass, the one facing most squarely wins.
// The texel is the one the CPU reference picks: the card parameter times the
// card's size, floored and clamped, v mirrored (image row 0 is the card's +v
// side). THE ATLAS HAS NO MIPS (SurfaceCache::makeAtlas creates every layer
// with one level), so the read is TEXEL-EXACT whatever the footprint: the
// page's own mip chain is SC-2's.
//
// WHICH DIRECTION "FACES". The CPU reference takes the surface normal. A ray
// query's committed hit carries no normal — the triangle's vertices are not
// readable from the acceleration structure without VK_KHR_ray_tracing_position_fetch
// (a device feature this pin does not enable) — so the reflection passes the
// REVERSED RAY DIRECTION. That is the side of the surface the ray sees: a card
// facing away from the ray can never be the one it hit, and among the cards
// that face it the depth rule leaves only those that captured the hit surface
// itself (a point on a box's top is not the first surface along any side
// card's axis, except within two texels of the edge).
//
// TWO LIMITS OF THE CARD'S RADIANCE, stated (the CARDS-1 audit's F7 and F8):
//   * a card whose indirect half has not been marched yet (freshly captured —
//     the capture budget exceeds the indirect budget at every tier) holds a
//     BLACK indirect: the pick reports it (`lit` false) and the caller reads the
//     voxels for that hit instead, never a black bounce;
//   * the stored radiance carries no diffuse fresnel (fresnelD: a
//     SeparateDiffuseFresnel BRDF's card is brighter by 1 - F), and its cone
//     frame is built on the stored SHADING normal (the pixel builds it on the
//     geometric one; they differ under a normal map).
//
// HOW A CALLER BINDS IT (the caller's own bindings, through macros defined
// FIRST, as jah_rq_hit.glsl does):
//
//     JAH_CARD_SLOTS              uint   instance-table entries bound (0 = no
//                                        cache: every pick fails)
//     JAH_CARD_RECORDS            uint   card records bound
//     JAH_CARD_INSTANCE(slot)     uvec4  (firstCard, cardCount, 0, 0)
//     JAH_CARD_DEPTH(texel)       float  the Depth layer at an atlas texel
//     JAH_CARD_RADIANCE(texel)    vec3   the Radiance layer at an atlas texel
// and the card table as `jahCards[]` — jah_rq_card_bindings.glsl declares all
// of it at the caller's JAH_CARD_BINDING_BASE.
//
// No Hlms directive mark anywhere in this file (it may be wrapped into a piece).

#ifndef JAH_RQ_CARD_GLSL
#define JAH_RQ_CARD_GLSL

// ONE CARD, as SurfaceCache::syncBuffers writes it (std430, 80 bytes;
// JahCardRecordGpu in the bindings file):
//   rowU / rowV   dot( world, xyz ) + w = the card's u / v in [0, 1]
//   rowD          ...the distance from the card's near plane
//   axis          xyz = the outward world axis, w = one card texel, metres
//   atlas         x, y = the rect's corner in the atlas, z = its size, w = flags
const uint kJahCardCaptured = 1u;	///< the card has been captured at least once
const uint kJahCardLit = 2u;		///< its radiance carries a marched indirect half

/// What the pick found: the card (an index into the card table), the atlas
/// texel, and whether the card's radiance is whole (kJahCardLit).
struct JahCardPick
{
	bool ok;
	bool lit;
	uint card;
	ivec2 texel;
};

JahCardPick jahCardPick( uint slot, vec3 hitPos, vec3 facingDir )
{
	JahCardPick best;
	best.ok = false;
	best.lit = false;
	best.card = 0xFFFFFFFFu;
	best.texel = ivec2( 0 );
	if( slot >= JAH_CARD_SLOTS )
		return best;
	const uvec4 inst = JAH_CARD_INSTANCE( slot );
	const vec3 n = normalize( facingDir );
	float bestFacing = 0.05;
	for( uint k = 0u; k < inst.y; ++k )
	{
		const uint i = inst.x + k;
		if( i >= JAH_CARD_RECORDS )
			break;
		const JahCardRecordGpu c = jahCards[i];
		if( ( c.atlas.w & kJahCardCaptured ) == 0u || c.atlas.z == 0u )
			continue;
		const float facing = dot( c.axis.xyz, n );
		if( facing <= bestFacing )
			continue;
		const float u = dot( c.rowU.xyz, hitPos ) + c.rowU.w;
		const float v = dot( c.rowV.xyz, hitPos ) + c.rowV.w;
		if( u < 0.0 || u > 1.0 || v < 0.0 || v > 1.0 )
			continue;
		const uint size = c.atlas.z;
		const ivec2 texel = ivec2( int( c.atlas.x + min( size - 1u, uint( u * float( size ) ) ) ),
								   int( c.atlas.y + min( size - 1u, uint( ( 1.0 - v ) * float( size ) ) ) ) );
		const float stored = JAH_CARD_DEPTH( texel );
		if( !( stored > 0.0 ) )
			continue;					// the clear: nothing captured here
		const float planeToPoint = dot( c.rowD.xyz, hitPos ) + c.rowD.w;
		if( abs( planeToPoint - stored ) > 2.0 * c.axis.w + 1e-3 )
			continue;					// the through-the-wall rule
		best.ok = true;
		best.lit = ( c.atlas.w & kJahCardLit ) != 0u;
		best.card = i;
		best.texel = texel;
		bestFacing = facing;
	}
	return best;
}

/// THE RADIANCE LEAVING A HIT, FROM ITS CARD. `ok` is false when no card of
/// the instance describes the point, or when the one that does has not had its
/// indirect half marched yet — the caller then reads the voxels. `footprint`
/// (world metres) is accepted for the day the pages carry mips (SC-2) and is
/// unused: the atlas is read texel-exact.
vec3 jahCardRadiance( uint slot, vec3 hitPos, vec3 facingDir, float footprint, out bool ok )
{
	const JahCardPick pick = jahCardPick( slot, hitPos, facingDir );
	ok = pick.ok && pick.lit;
	if( !ok )
		return vec3( 0.0 );
	return JAH_CARD_RADIANCE( pick.texel );
}

#endif   // JAH_RQ_CARD_GLSL
