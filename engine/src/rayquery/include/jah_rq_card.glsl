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
// NO FOOTPRINT GATE (PHOTON-7 stage 6, law L5): every caller reads a carded surface's card at every footprint - the
// reflection's gate, which read the voxels for the same surface wherever its lobe passed four card texels, is deleted
// (one estimator of one radiance; at the gated hits the card was the nearer to the screen's answer).
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

#ifdef JAH_CARD_DEMAND
/// THE READERS' DEMAND (PHOTON-7 stage 5, 5-2; jah_rq_card_bindings.glsl's layout): the hit's read stamps the card it
/// read, and - EVERY hit, answered or not (PHOTON-7 stage 10-3, F3) - the instance's face bucket the hit faces, with the frame's stamp
/// (a plain store of one value: every writer writes the same, so the race is benign, and the read first keeps a hot
/// card to one store a frame). The host reads them back and makes resident what the readers read.
uint jahCardBucket( vec3 n )
{
	const vec3 a = abs( n );
	const uint axis = ( a.x >= a.y && a.x >= a.z ) ? 0u : ( a.y >= a.z ? 1u : 2u );
	return 2u * axis + ( n[axis] < 0.0 ? 1u : 0u );
}
void jahCardDemandStamp( uint e, uint k, uint stamp )
{
	if( jahCardInstances[e][k] != stamp )
		jahCardInstances[e][k] = stamp;
}
void jahCardDemand( uint slot, JahCardPick pick, vec3 facing )
{
	const uint S = JAH_CARD_SLOTS;
	if( S == 0u || slot >= S )
		return;
	const uvec4 head = jahCardInstances[S];
	const uint reader = uint( JAH_CARD_DEMAND );
	// THE DEMAND SPAN (PHOTON-7 stage 10, the per-reader snapshot): a reader past its rest indices [1, N + the due]
	// stamps nothing - its demand is that span in every run (the face hits below are counted all the same)
	const bool stamps = JAH_HIT_PARAMS.cards.z > 0.5;
	if( stamps && pick.ok )
	{
		const uvec4 at = jahCards[pick.card].atlas;
		const uint w = 2u * ( ( at.y >> 4u ) * 128u + ( at.x >> 4u ) ) + reader;
		jahCardDemandStamp( S + 2u + ( w >> 2u ), w & 3u, head.x );
	}
	// THE FACE BUCKET ON EVERY HIT, ANSWERED OR NOT (PHOTON-7 stage 10-3, F3): the rest's demand is the faces the rays
	// HIT. A mark written only when no card answered made it follow the cache's state - a resident card not yet
	// (re)captured answers nothing, so whether a face was marked followed the capture queue's history.
	if( stamps )
	{
		const uint w = 8u * reader + jahCardBucket( facing );
		jahCardDemandStamp( S + 8194u + 4u * slot + ( w >> 2u ), w & 3u, head.x );
	}
	if( head.y != 0u )
	{
		// THE FACE'S HITS (the measuring arm): EVERY read of the face, its card's or none's - the settle law's lambda
		const uint w = 8u * reader + jahCardBucket( facing );
		const uint e = S + 8194u + 4u * S + 4u * slot + ( w >> 2u );
		const uint k = w & 3u;
		if( k == 0u )
			atomicAdd( jahCardInstances[e].x, 1u );
		else if( k == 1u )
			atomicAdd( jahCardInstances[e].y, 1u );
		else if( k == 2u )
			atomicAdd( jahCardInstances[e].z, 1u );
		else
			atomicAdd( jahCardInstances[e].w, 1u );
	}
	if( head.y != 0u )
	{
		const uint w = 2u * uint( JAH_CARD_DEMAND ) + ( pick.ok ? 0u : 1u );
		if( w == 0u )
			atomicAdd( jahCardInstances[S + 1u].x, 1u );
		else if( w == 1u )
			atomicAdd( jahCardInstances[S + 1u].y, 1u );
		else if( w == 2u )
			atomicAdd( jahCardInstances[S + 1u].z, 1u );
		else
			atomicAdd( jahCardInstances[S + 1u].w, 1u );
	}
}
#endif

/// THE RADIANCE LEAVING A HIT TOWARDS `viewDir` (unit, from the hit to the
/// viewer: the reversed ray), FROM ITS CARD. `ok` is false when no card of
/// the instance describes the point, or when the one that does has not had its
/// indirect half marched yet — the caller then reads the voxels. The atlas is read texel-exact (no mips until SC-2).
#ifndef JAH_HIT_GATE_DECLARED
#define JAH_HIT_GATE_DECLARED
/// C2, THE HIT's OWN SPECULAR TRACED (PHOTON-7 11-2 (3)): set by a reader that traces the hit's lobe itself
/// (jahHitSpecularLobeTraced, jah_rq_hit_radiance.glsl) around the store's read - the store then leaves out the INDIRECT
/// specular it holds only as the lobe's integral (the card's lobe share, the voxels' bounce share); the lights' direct
/// lobe stays (a light is not geometry: no ray meets it). False everywhere else.
bool jahHitTracedLobe = false;
#endif

/// THE LOBE'S HEMISPHERICAL INTEGRALS ( A-bar, B-bar ) at roughness r - the store's table. The
/// INCLUDER defines it after this file (the ray jobs: jahSpecularAlbedoHemi through the hit table).
void jahCardLobeHemi( float r, out float a, out float b );
/// THE LIGHTS' SPECULAR LOBE AT A CARD TEXEL TOWARD V (PHOTON-3 stage 2): E mu vis f_s( L, V ) summed over
/// the lights the relight shades, the sun weighted by the texel's stored visibility `sunVis`, each lamp by its own bit
/// of the texel's lamp mask (`lampBits`: the SunVis texel; `lampIds` the card's lamp table - PHOTON-7 stage 5, 5-3).
/// The INCLUDER defines it (jah_rq_card_lights.glsl, from the ray tier's light table).
vec3 jahCardLightsLobe( vec3 P, vec3 N, vec3 V, float r, vec3 F0, float w, float sunVis, uvec4 lampBits,
						uvec4 lampIds[12] );

/// THE CARD THE LAST READ ANSWERED FROM (its record in the card table; 0xFFFFFFFF none): the cards' TLAS gather records
/// which cards each texel's rays read (rq_card_gather.comp: the card-to-card re-listing, PHOTON-7 stage 10).
uint jahCardPicked = 0xFFFFFFFFu;
vec3 jahCardPickCut = vec3( 0.0 );   // THE PICK (the reds' instrument): the last read's cutLobe (C2's share left out)

/// THE NEAR-MIRROR BOUND (PHOTON-7 11-2 (7)): the G-buffer's floor, jah_reflect_draw.glsl's kMirrorPackAlpha - at or
/// under it the relight stores no specular (JahCardLight_cs.glsl takes the same 0.0011).
const float kJahCardNearMirrorAlpha = 0.0011;
vec3 jahCardRadiance( uint slot, vec3 hitPos, vec3 facingDir, vec3 viewDir, out bool ok )
{
	const JahCardPick pick = jahCardPick( slot, hitPos, facingDir );
#ifdef JAH_CARD_DEMAND
	jahCardDemand( slot, pick, facingDir );
#endif
	ok = pick.ok && pick.lit;
	jahCardPicked = ok ? pick.card : 0xFFFFFFFFu;
	jahCardPickCut = vec3( 0.0 );
	if( !ok )
		return vec3( 0.0 );
	// THE VIEW TERM (jah_card_view.glsl): the stored normal in the card's own
	// frame (u, v, the outward axis — the relight's decode), the roughness
	// through fork 16d8e29d4 (was 0043)'s range (the relight's), the mean light direction from
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
	// THE INDIRECT LAYER (ENGINE-POLISH-PHOTON-1 stage 3) holds the diffuse half where the channel has a
	// diffuse albedo and the specular lobe's share where it has none (JahCardLight_cs.glsl's cache): the
	// diffuse half is the layer where kD > 0, else 0. The EMISSIVE binding is the relight's FLAT layer
	// (the emissive and the specular lobe's re-emission), added outside the diffuse view restore.
	const vec3 stored = JAH_CARD_INDIRECT( pick.texel );
	const bvec3 hasDiffuse = greaterThan( albedo.xyz, vec3( 0.0 ) );
	const vec3 indirect = mix( vec3( 0.0 ), stored, hasDiffuse );
	// THE LOBE'S INDIRECT SHARE, LEFT OUT WHERE THE READER TRACES THE LOBE ITSELF (C2: the traced lobe replaces the
	// integral): where kD = 0 the cached texel IS that share; where kD > 0 it is the cached diffuse half times the lobes'
	// ratio ( F0 A-bar + w B-bar ) / ( kD pi Dd-bar ), the relight's own recovery. Cut from the radiance and the flat term
	// alike, so the direct half is untouched. (The movers' gate's half of this cut is deleted: PHOTON-7 8A A3.)
	// (A NEAR-MIRROR's texel holds no lobe share - JahCardLight_cs.glsl, PHOTON-7 11-2 (7): nothing to cut. The lights'
	// lobe below is not a cache: it is evaluated toward this ray, a near-mirror's glint included.)
	vec3 cutLobe = vec3( 0.0 );
	if( jahHitTracedLobe && alpha > kJahCardNearMirrorAlpha )
	{
		const vec4 spec = JAH_CARD_SPECULAR( pick.texel );
		if( spec.w > 0.0 )
		{
			float sa, sb;
			jahCardLobeHemi( r, sa, sb );
			const vec3 specLobe = ( spec.www * 0.04 + spec.xyz * 0.96 ) * sa + spec.w * sb;
			const vec3 diffuseLobe = albedo.xyz * 3.141592654 * jahDiffuseAlbedoHemi( r );
			const vec3 lobeShare = mix( stored, stored * specLobe / max( diffuseLobe, vec3( 1e-12 ) ), hasDiffuse );
			cutLobe = lobeShare;
		}
	}
	// THE LIGHTS' SPECULAR LOBE TOWARD THIS RAY (PHOTON-3 stage 2): where the relight re-emitted the lobe's
	// integral the same way to every direction, the read evaluates BRDF_Default's own lobe toward viewDir for
	// every light it knows, E mu vis f_s( L, V ) with the texel's F0 and w (jahCardLightsLobe) - exact per light,
	// the sun's visibility the texel's own (the SunVis layer). Its V-integral is what the store re-emits: the
	// energy is unchanged, the direction is the surface's.
	const vec3 V = normalize( viewDir );
	vec3 lobe = vec3( 0.0 );
	{
		const vec4 spec = JAH_CARD_SPECULAR( pick.texel );
		if( spec.w > 0.0 )
			lobe = jahCardLightsLobe( hitPos, N, V, r, spec.www * 0.04 + spec.xyz * 0.96, spec.w,
									  JAH_CARD_SUN_VIS( pick.texel ), JAH_CARD_LAMP_BITS( pick.texel ), c.lamps );
	}
	jahCardPickCut = cutLobe;
	return jahCardViewRadiance( JAH_CARD_RADIANCE( pick.texel ) - cutLobe, indirect,
								JAH_CARD_EMISSIVE( pick.texel ) - cutLobe, N, L, r, V ) + lobe;
}

#endif   // JAH_RQ_CARD_GLSL

