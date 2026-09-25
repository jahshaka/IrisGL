// A RAY'S HIT READ FROM THE SURFACE CACHE — the one copy (PHOTON-CARDS-2 part
// B, SC-1d). `SurfaceCache::readAt` (OgreSurfaceCache.cpp) is the same
// algorithm on the CPU and stays as the TEST reference: gi.card_read_parity
// asserts the two pick the same card and the same texel.
//
// WHAT A CARD IS (SurfaceCache.h, CardRec): an axis-aligned rectangle in WORLD
// space around one instance, captured orthographically along its outward axis
// into the atlas — depth from the card's near plane, and (the CardLight job)
// the lit RADIANCE: direct + the environment half + emissive, stored HEAD-ON
// (V = N) — and the read restores the diffuse lobe's VIEW term for the ray
// that reads it (jah_card_view.glsl, PHOTON-CARDS-5: at a grazing eye the pixel
// is up to 1.6-1.8x its head-on value, and so is the card read now).
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
// WHICH DIRECTION "FACES": the hit surface's GEOMETRIC normal, as the CPU
// reference takes the surface normal. The reflection rebuilds it from the hit
// triangle (jah_rq_geom.glsl — the GPU scene's geometry rows); the reversed ray
// it used before let a NEIGHBOUR face's card win within two texels of an edge at
// more than 45 degrees of incidence, and rejected the true card past 87.
//
// THE FOOTPRINT GATE (the lead's decision; the currency SC-2's page mips will
// select on): the card is read only when the sample's footprint at the hit —
// the caller's, in metres — is at most JAH_CARD_FOOTPRINT_TEXELS of the picked
// card's texels. A wider footprint is a lobe a texel-exact read would alias
// (one stochastic sample per frame landing on one texel where the voxel read is
// prefiltered at the footprint's mip: speckle in a young view); those samples
// read the voxels until a page carries mips. k is Types.h's
// kCardFootprintTexels, with the measurement that chose it.
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
//     JAH_CARD_FOOTPRINT_TEXELS   float  the footprint gate, in card texels
//     JAH_CARD_INSTANCE(slot)     uvec4  (firstCard, cardCount, 0, 0)
//     JAH_CARD_DEPTH(texel)       float  the Depth layer at an atlas texel
//     JAH_CARD_RADIANCE(texel)    vec3   the Radiance layer at an atlas texel
//     JAH_CARD_INDIRECT / _EMISSIVE / _SHADOW_ROUGH / _ALBEDO / _NORMAL (texel)
//                                        the view term's five layers
// and the card table as `jahCards[]` — jah_rq_card_bindings.glsl declares all
// of it at the caller's JAH_CARD_BINDING_BASE and JAH_CARD_VIEW_BINDING_BASE.
//
// No Hlms directive mark anywhere in this file (it may be wrapped into a piece).

#ifndef JAH_RQ_CARD_GLSL
#define JAH_RQ_CARD_GLSL

// The fork's diffuse lobe (JahBrdf, JahDiffuseAlbedo — unwrapped from its Pbs
// media by the build) and the view term built on it.
#include "jah_fork_brdf.glsl"
#include "jah_card_view.glsl"

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

/// THE RADIANCE LEAVING A HIT TOWARDS `viewDir` (unit, from the hit to the
/// viewer: the reversed ray), FROM ITS CARD. `ok` is false when no card of
/// the instance describes the point, when the one that does has not had its
/// indirect half marched yet, or when `footprint` (world metres) is wider than
/// JAH_CARD_FOOTPRINT_TEXELS of that card's texels — the caller then reads the
/// voxels. The atlas is read texel-exact (no mips until SC-2).
vec3 jahCardRadiance( uint slot, vec3 hitPos, vec3 facingDir, vec3 viewDir, float footprint,
					  out bool ok )
{
	const JahCardPick pick = jahCardPick( slot, hitPos, facingDir );
	ok = pick.ok && pick.lit &&
		 footprint <= float( JAH_CARD_FOOTPRINT_TEXELS ) * jahCards[pick.card].axis.w;
	if( !ok )
		return vec3( 0.0 );
	// THE VIEW TERM (jah_card_view.glsl): the stored normal in the card's own
	// frame (u, v, the outward axis — the relight's decode), the roughness
	// through patch 0043's range (the relight's), the mean light direction from
	// the two alphas.
	const JahCardRecordGpu c = jahCards[pick.card];
	const vec4 albedo = JAH_CARD_ALBEDO( pick.texel );
	const vec4 normal = JAH_CARD_NORMAL( pick.texel );
	const vec3 nV = normal.xyz * 2.0 - 1.0;
	const vec3 Nstored = normalize( normalize( c.rowU.xyz ) * nV.x + normalize( c.rowV.xyz ) * nV.y +
									c.axis.xyz * nV.z );
	// THE CARD'S PLANE, as the relight takes it (JahCardLight_cs.glsl): a stored
	// normal within the 8-bit format's quantum of the card's axis (0 decodes to
	// -0.0039, 0.32 degrees off) IS the plane — at a grazing N.V that tilt alone
	// moved the view term 0.9 % (measured, PHOTON-CARDS-5).
	const vec3 N = dot( Nstored, c.axis.xyz ) > 0.99985 ? c.axis.xyz : Nstored;
	const float alpha = JAH_CARD_SHADOW_ROUGH( pick.texel ).y / 1.001001 + 0.001;
	const float r = sqrt( max( alpha, 0.0 ) );
	const vec3 L = jahCardOctDecode( vec2( albedo.w, normal.w ) );
	return jahCardViewRadiance( JAH_CARD_RADIANCE( pick.texel ), JAH_CARD_INDIRECT( pick.texel ),
								JAH_CARD_EMISSIVE( pick.texel ), N, L, r, normalize( viewDir ) );
}

#endif   // JAH_RQ_CARD_GLSL
