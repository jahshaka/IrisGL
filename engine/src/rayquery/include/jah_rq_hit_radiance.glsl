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
// THE CURRENCY IS THE FOOTPRINT (the lead's decision after the CARDS-2 audit): the sample's footprint at the hit, in
// world metres, decides the VOXEL read (the mip whose texel is that wide), the hit material's read (jahHitMaterial) and
// the record's footprint word. It never gates the CARD (PHOTON-7 stage 6, law L5: the reflection's footprint gate is
// deleted - every caller reads a carded surface's card at every footprint, one estimator of one radiance):
//   * the REFLECTION (rq_reflect.comp): the spacing between the samples its temporal mean holds over the lobe,
//     2 t alpha / sqrt( N ), N = 1 / knobs2.w; a mirror (its centre ray) reads the containing cascade whole at LOD 0
//     (`mirror`);
//   * the GATHER (rq_probe_gather.comp): t x sqrt( 2 / rays ) - one ray stands for one octahedral texel's solid angle.
//
// A VOXEL-ANSWERED HIT'S DIRECT LIGHT IS COMPUTED AT THE HIT (FAR-SUN-2, jah_rq_hit_direct.glsl):
// every light's direct term at the hit from its own material, its own normal and its own shadow
// rays, its emissive, and of the store only the bounce part. The caller describes the hit in
// `jahHitSurf` before asking (a hit it does not describe reads the store whole).
//
// HOW A CALLER BINDS IT: include jah_rq_hit.glsl (the voxel read),
// jah_rq_card.glsl (the card read, behind its own macros), jah_rq_geom.glsl,
// jah_rq_sun_ray.glsl and jah_rq_hit_record.glsl first, and bind
// jah_rq_hit_direct.glsl's names.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_RADIANCE_GLSL
#define JAH_RQ_HIT_RADIANCE_GLSL

#include "jah_rq_hit_direct.glsl"

/// `slot` = the hit instance's item slot (0xFFFFFFFF: no instance the card
/// tables know — the voxels answer); `hitNormal` = the hit triangle's geometric
/// normal facing the ray's origin; `dir` = the ray's direction; `footprint` =
/// the caller's sample footprint at the hit in metres; `mirror` = the sample is
/// a mirror's (the voxels at LOD 0 with no crossfade). `ok` is false where neither the card
/// nor the cascades can shade the hit — and the caller then APPENDS THE HIT TO
/// THE HIT LIST (jah_rq_hit_record.glsl, PHOTON-HIT-SHADE-1), where HlmsAtom's
/// decode shades it with the scene's own lighting text. A hit on a MOVER or a
/// RIGGED item never asks this function at all: the caches cannot hold it.
/// ...and WHICH of the two answered (`source`: 1 the card, 2 the cascades, 0
/// neither) — the photon view's Hits picture (PHOTON-VIEW-1) reads it; the answer
/// itself is the same arithmetic as jahHitRadiance's, which is this function.
// (THE MOVER HIT GATE IS DELETED, PHOTON-7 8A A3: four rays a hit against the movers alone, subtracting their occlusion from
// the store's non-direct share because the store was built without them. Since A1 the store holds every mover at its
// pose - its occlusion and its bounce are in the voxels and the cards - so the gate counted them twice, and its 4 m
// reach could not see a bus 10 m away. The hit reads the store whole.)
vec3 jahHitStoreRead( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					  bool mirror, out bool ok, out uint source )
{
	{
		// The card read restores the diffuse lobe's view term for THIS ray
		// (PHOTON-CARDS-5): the viewer is where the ray came from.
		const vec3 card = jahCardRadiance( slot, hitPos, hitNormal, -dir, ok );
		if( ok )
		{
			source = 1u;
			return card;
		}
	}
	const vec3 voxel = jahVoxelRadiance( hitPos, dir, hitNormal, footprint, mirror, ok );
	source = ok ? 2u : 0u;
	// THE ONE RULE (FAR-SUN-2): the direct light at the hit, the store's bounce part beside it, both toward
	// the ray's viewer (PHOTON-4 stage 2: the bounce's diffuse share by its directional albedo at N.V,
	// jahHitBounceToward).
	vec3 albedo, emissive;
	float roughness;
	vec4 metal;
	if( ok && jahHitMaterial( footprint, albedo, emissive, roughness, metal ) )
	{
		const vec3 N = normalize( hitNormal ), V = -normalize( dir );
		const vec3 r = jahHitDirect( hitPos, N, V, albedo, roughness, metal ) + emissive +
					   jahHitBounceToward( jahVoxelBounceOut, albedo, roughness, metal, clamp( dot( N, V ), 0.0, 1.0 ) );
		return finite3( r ) ? clamp( r, vec3( 0.0 ), vec3( kMaxRadiance ) ) : voxel;
	}
	return voxel;
}

#ifndef JAH_HIT_LOBE_SKY
// (an escape's sky: the ONE environment (jah_rq_hit.glsl includes it) at the lobe's cone - the card gather's own; the
// reflection's includer reads its own sky; an earned exit, a TLAS miss to the world's exit, never a sky past a wall)
#define JAH_HIT_LOBE_SKY( d, alpha, centre ) jahEnvCone( d, max( alpha, 1e-3 ) )
#endif
#ifndef JAH_HIT_LOBE_MASK
#define JAH_HIT_LOBE_MASK 0x0Fu   // (the near copies, kRayMaskNearField; a cache's gather passes its still mask)
#endif
#include "jah_world_exit.glsl"
#include "jah_reflect_draw.glsl"

/// THE NEAR-MIRROR CONTINUATION (PHOTON-7 11-2 (7), the lead's rulings). A card texel is a view-independent cache, so a
/// near-mirror's store holds no specular (JahCardLight_cs.glsl; its bounce share none, jahHitBounceToward): THE LAW IS
/// ONE - a ray that meets a near-mirror, whoever shot it (a gather's first hit, the reflection trace's, C2's secondary),
/// continues by ONE ray along the mirror direction (a near-delta lobe needs no sampling), DEPTH 1: its hit read by the
/// store (jahHitStoreRead: the card, the cascades, the direct - no further continuation: a mirror-to-mirror chain stops
/// at 1 and reads the store there, the named limit), an exit the sky, weighed by the mirror's split DFG
/// ( F0 A( mu, r ) + w B( mu, r ) ) at its own N.V. A rough surface (past the bound) keeps its store answer. Taken in
/// jahHitRadianceStore, the one shared hit read; a reader that traces the hit's lobe itself (jahHitTracedLobe: C2's
/// centre ray at a mirror IS this ray) is not continued twice. The ray tiers only: the no-ray tier has no TLAS, and its
/// store's answer for a metal is what it is. jahHitSurf is the mirror hit on entry (its material is read here) and on
/// exit. `P2` the mirror hit, `n2` its normal facing the ray, `L2` the ray that met it, `footprint` the caller's at P2;
/// `answered` false where nothing was traced (not a near-mirror, a grazing hit).
uint jahHitMirrorSlot = 0xFFFFFFFFu;   // (THE PICK: the last continuation's hit slot, 0xFFFFFFFF none or an exit)
vec3 jahHitMirrorPart = vec3( 0.0 );   // (...and its part, weighed)
vec3 jahHitMirrorContinue( vec3 P2, vec3 n2, vec3 L2, float footprint, out bool answered )
{
	answered = false;
	vec3 alb2, emi2;
	float rough2;
	vec4 metal2;
	if( !jahHitMaterial( footprint, alb2, emi2, rough2, metal2 ) || rough2 * rough2 > kMirrorPackAlpha ||
		!( metal2.w > 0.0 || any( greaterThan( metal2.xyz, vec3( 0.0 ) ) ) ) )
		return vec3( 0.0 );
	const float carried = jahHitSurf.t;
	const float alpha = max( rough2 * rough2, 1e-6 );
	const float mu2 = clamp( dot( n2, -L2 ), 0.0, 1.0 );
	if( !( mu2 > 0.0 ) )
		return vec3( 0.0 );
	float sa2, sb2;
	jahSpecularAlbedo( mu2, rough2, sa2, sb2 );
	const vec3 w2 = metal2.xyz * sa2 + metal2.w * sb2;
	if( !( max( w2.x, max( w2.y, w2.z ) ) > 0.0 ) )
		return vec3( 0.0 );
	answered = true;
	const vec3 L3 = reflect( L2, n2 );
	const vec3 o3 = jahRayOrigin( P2, n2, carried, 0.0 );
	rayQueryEXT q3;
	rayQueryInitializeEXT( q3, tlas, jahAlphaRayFlags( gl_RayFlagsNoneEXT ), JAH_HIT_LOBE_MASK, o3, 0.0, L3,
						   jahWorldExitT( o3, L3, 1.0e4 ) );
	JAH_RQ_PROCEED( q3 )
	if( rayQueryGetIntersectionTypeEXT( q3, true ) != gl_RayQueryCommittedIntersectionTriangleEXT )
		return w2 * JAH_HIT_LOBE_SKY( L3, alpha, true );   // an exit: the sky
	const float t3 = rayQueryGetIntersectionTEXT( q3, true );
	const uint slot3 = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q3, true ) );
	jahHitMirrorSlot = slot3;
	const uint prim3 = uint( rayQueryGetIntersectionPrimitiveIndexEXT( q3, true ) );
	const uint geom3 = uint( rayQueryGetIntersectionGeometryIndexEXT( q3, true ) );
	const vec2 bary3 = rayQueryGetIntersectionBarycentricsEXT( q3, true );
	const mat4x3 toWorld3 = rayQueryGetIntersectionObjectToWorldEXT( q3, true );
	vec3 wound3;
	const bool known3 = jahHitWoundNormal( slot3, geom3, prim3, toWorld3, wound3 );
	const bool back3 = known3 && dot( wound3, L3 ) > 0.0 && ( jahHitFlags( slot3 ) & kJahHitGpuTwoSided ) == 0u;
	if( back3 || ( jahHitFlags( slot3 ) & ( kJahHitGpuSkinned | kJahHitGpuPlanar ) ) != 0u )
		return vec3( 0.0 );   // (the secondary's own rules: a back face, a rigged item, a planar mirror answer none)
	const vec3 n3 = known3 ? ( dot( wound3, L3 ) > 0.0 ? -wound3 : wound3 ) : -L3;
	const JahHitSurface mirrorHit = jahHitSurf;
	jahHitSurf = JahHitSurface( slot3, geom3, prim3, bary3, toWorld3, carried + t3, false, known3 );
	bool ok3 = false;
	uint src3 = 0u;
	// (the footprint spreads on along the mirror ray as it did to P2: a mirror widens nothing)
	const vec3 r3 = jahHitStoreRead( slot3, o3 + L3 * t3, n3, L3, footprint * ( 1.0 + t3 / max( carried, 1e-3 ) ), true,
									 ok3, src3 );
	jahHitSurf = mirrorHit;
	return ok3 && finite3( r3 ) ? w2 * r3 : vec3( 0.0 );
}

/// THE ONE SHARED HIT READ (every TLAS hit reader's): the store's answer at the hit (jahHitStoreRead), and at a
/// NEAR-MIRROR the reflection it owes, continued by one ray (jahHitMirrorContinue) - unless the reader traces the hit's
/// lobe itself (C2 sets jahHitTracedLobe: its centre ray at a mirror is the same ray). `ok` true where either answered.
vec3 jahHitRadianceStore( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
						  bool mirror, out bool ok, out uint source )
{
	vec3 r = jahHitStoreRead( slot, hitPos, hitNormal, dir, footprint, mirror, ok, source );
	jahHitMirrorSlot = 0xFFFFFFFFu;
	jahHitMirrorPart = vec3( 0.0 );
	if( !jahHitTracedLobe && !( JAH_HIT_PARAMS.counts.w > 0.5 ) )   // (MEASURING ARM, reverted)
	{
		bool answered;
		const vec3 m = jahHitMirrorContinue( hitPos, normalize( hitNormal ), normalize( dir ), footprint, answered );
		if( answered )
		{
			jahHitMirrorPart = m;
			r = ( ok && finite3( r ) ? r : vec3( 0.0 ) ) + m;
			ok = true;
		}
	}
	return r;
}

// ---- C2: THE HIT's OWN SPECULAR (PHOTON-7 11-2 (3); ONE_PICTURE C2; the lead's rulings D1-D6) ---------------------------
// The store holds a surface's INDIRECT specular only as its lobe's INTEGRAL, re-emitted alike in every direction: a
// polished hit seen in a reflection showed none of what it reflects (a glossy slab under an emitter, a lacquered mover
// against the same object still, the crease where a block meets a mirror floor). A reader that traces the hit's lobe
// itself sets jahHitTracedLobe around its store read (the store then leaves the integral out) and adds this:
// ONE SECONDARY RAY - the hit's own VNDF draw at its alpha (the centre ray at the packing floor: a sharp metal),
// weighed by the hit's split DFG ( F0 A( mu, r ) + w B( mu, r ) ) from its material row and the draw's gain (G2/G1 over
// the F-free A + B, jahReflectGain), the hit at its end read by the STORE - the card, the cascades, the direct - whose
// own specular is its lobe's integral: NO RECURSION (the store IS the environment lobe; a sky with no visibility would
// be light through a wall, L9) - save a NEAR-MIRROR's, which the store does not hold: that hit continues by ONE
// mirror ray, depth 1 (jahHitMirrorContinue, 11-2 (7)). An escape past the world's exit is the sky (JAH_HIT_LOBE_SKY, the includer's), a back
// face of a one-sided surface none, a hit only a record could shade (a rigged item, a planar mirror) none (the store does not
// hold it). ALWAYS TRACED (the lead's ruling (3b)): a Russian roulette on the weight was unbiased in linear radiance but
// the reflection's denoiser averages in Lumen's compressive space, where the mean of a 1 / p-spiky sample is biased low
// (Jensen; measured: a roughness-0.12 mirror's slab 20 % dark) - REFUSED. The lights' direct lobe stays the store's (a
// light is not geometry).
// `P` the hit, `N` its geometric normal facing the ray's origin, `V` toward that origin, `carried` the distance P was
// carried along (its float bound), `roughness` and `metal` the hit's (jahHitMaterial), `u` two uniforms (the draw).
#include "jah_rq_emitters.glsl"
/// THE PICK's VIEW OF C2 (the reds' instrument, rq_reflect.comp jahPickSample): the last secondary ray's - its hit slot
/// (0xFFFFFFFF an exit), its distance, the BRDF draw's part and the portal draw's part (every caller may ignore them)
uint jahC2PickSlot = 0xFFFFFFFFu;
float jahC2PickT = 0.0;
vec3 jahC2PickBrdf = vec3( 0.0 ), jahC2PickPortal = vec3( 0.0 ), jahC2PickSpec = vec3( 0.0 );
/// ...and the near-mirror continuation's part (11-2 (7)) and its hit slot (0xFFFFFFFF none or an exit)
vec3 jahC2PickMirror = vec3( 0.0 );
uint jahC2PickMirrorSlot = 0xFFFFFFFFu;

vec3 jahHitSpecularLobeTraced( vec3 P, vec3 N, vec3 V, float carried, float roughness, vec4 metal, vec2 u )
{
	const float mu = clamp( dot( N, V ), 0.0, 1.0 );
	if( !( mu > 0.0 ) || !( metal.w > 0.0 || any( greaterThan( metal.xyz, vec3( 0.0 ) ) ) ) )
		return vec3( 0.0 );
	float sa, sb;
	jahSpecularAlbedo( mu, roughness, sa, sb );
	const vec3 w = metal.xyz * sa + metal.w * sb;
	if( !( max( w.x, max( w.y, w.z ) ) > 0.0 ) )
		return vec3( 0.0 );
	// THE DRAW: the hit's lobe toward V (its geometric normal: the store's own frame)
	const float alpha = max( roughness * roughness, 1e-6 );
	const bool centre = alpha <= kMirrorPackAlpha;
	vec3 L2;
	float gain = 1.0;
	if( centre )
		L2 = reflect( -V, N );
	else
	{
		vec3 T, B;
		jahReflectFrame( N, T, B );
		const vec3 Ve = vec3( dot( V, T ), dot( V, B ), dot( V, N ) );
		const vec3 Ht = jahGgxVndf( Ve, alpha, u );
		L2 = reflect( -V, normalize( Ht.x * T + Ht.y * B + Ht.z * N ) );
		if( !( dot( L2, N ) > 0.0 ) )
			return vec3( 0.0 );   // a draw that left the surface: no light (G2 is 0)
		gain = jahReflectGain( N, N, V, L2, alpha, sa + sb );
	}
	// THE SKY's PORTALS (PHOTON-7 11-2 (6), the lead's ruling): the sky seen through an opening is a small emitter, so its
	// share of this lobe is estimated by BOTH strategies at the balance heuristic - the BRDF draw's escape weighs
	// q / ( q + p_l ), and one NEE draw toward the table's portals (jah_rq_emitters.glsl, kind portals) adds what it sees
	// of the sky at p_l / ( q + p_l ); a geometry hit stays the BRDF draw's at weight 1 (the NEE ray that meets geometry
	// adds 0). Unbiased for ANY portal set; no portal (W 0): the BRDF draw alone, as before.
	JahNeePick sky;
	sky.ok = false;
	sky.pdfB = 0.0;
	if( !centre )
	{
		const uint k0 = jahReflectPcg( floatBitsToUint( u.x ) ^ jahReflectPcg( floatBitsToUint( u.y ) ^ 0x5CA1AB1Eu ) );
		const uint k1 = jahReflectPcg( k0 ^ 0xA54FF53Au );
		const vec3 u3 = vec3( float( k0 & 0xFFFFFFu ) * ( 1.0 / 16777216.0 ), float( k1 & 0xFFFFu ) / 65536.0,
							  float( k1 >> 16u ) / 65536.0 );
		sky = jahNeePick( P, N, reflect( -V, N ), alpha, L2, true, u3, true );
	}
	const float qB = centre ? 0.0 : jahReflectFoldedPdf( N, N, V, L2, alpha );
	const float wSkyB = centre || !( sky.pdfB > 0.0 ) ? 1.0 : qB / max( qB + sky.pdfB, 1e-30 );
	// THE RAY: the one origin law off a hit on level 0 (no drawn surface's bound), to the world's exit
	const vec3 o = jahRayOrigin( P, N, carried, 0.0 );
	rayQueryEXT q;
	rayQueryInitializeEXT( q, tlas, jahAlphaRayFlags( gl_RayFlagsNoneEXT ), JAH_HIT_LOBE_MASK, o, 0.0, L2,
						   jahWorldExitT( o, L2, 1.0e4 ) );
	JAH_RQ_PROCEED( q )
	vec3 rad = vec3( 0.0 );
	jahC2PickSlot = 0xFFFFFFFFu;
	jahC2PickT = 0.0;
	jahC2PickPortal = vec3( 0.0 );
	jahC2PickSpec = vec3( 0.0 );
	jahC2PickMirror = vec3( 0.0 );
	jahC2PickMirrorSlot = 0xFFFFFFFFu;
	if( rayQueryGetIntersectionTypeEXT( q, true ) != gl_RayQueryCommittedIntersectionTriangleEXT )
		rad = JAH_HIT_LOBE_SKY( L2, alpha, centre ) * wSkyB;   // an exit: the sky, at its balance weight (11-2 (6))
	else
	{
		const float t2 = rayQueryGetIntersectionTEXT( q, true );
		const uint slot2 = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, true ) );
		jahC2PickSlot = slot2;
		jahC2PickT = t2;
		const uint prim2 = uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, true ) );
		const uint geom2 = uint( rayQueryGetIntersectionGeometryIndexEXT( q, true ) );
		const vec2 bary2 = rayQueryGetIntersectionBarycentricsEXT( q, true );
		const mat4x3 toWorld2 = rayQueryGetIntersectionObjectToWorldEXT( q, true );
		vec3 wound;
		const bool known = jahHitWoundNormal( slot2, geom2, prim2, toWorld2, wound );
		const bool backFace = known && dot( wound, L2 ) > 0.0 && ( jahHitFlags( slot2 ) & kJahHitGpuTwoSided ) == 0u;
		// (a MOVER answers from the store like any hit since A1 - PHOTON-7 8A, A3/A4: the store holds it at its pose; a
		// rigged item, whose bind pose is what the store holds without the ray tier's skin cache, and a planar mirror,
		// whose picture is a reflection, answer none)
		if( !backFace && ( jahHitFlags( slot2 ) & ( kJahHitGpuSkinned | kJahHitGpuPlanar ) ) == 0u )
		{
			const vec3 n2 = known ? ( dot( wound, L2 ) > 0.0 ? -wound : wound ) : -L2;
			// (the second hit's read: its own store answer whole - its lobe's integral is the environment lobe)
			const JahHitSurface first = jahHitSurf;
			const bool tracedWas = jahHitTracedLobe;
			jahHitTracedLobe = false;
			jahHitSurf = JahHitSurface( slot2, geom2, prim2, bary2, toWorld2, t2, false, known );
			bool ok2 = false;
			uint src2 = 0u;
			jahHitDirectPickSpec = vec3( 0.0 );
			const vec3 r2 = jahHitRadianceStore( slot2, o + L2 * t2, n2, L2, max( 2.0 * t2 * sqrt( alpha ), 1e-4 ),
												 centre, ok2, src2 );
			jahC2PickSpec = jahHitDirectPickSpec;   // (THE PICK: the secondary hit's own lamps' specular, inside r2)
			// (a near-mirror's reflection is continued inside the read: 11-2 (7), the one law)
			jahC2PickMirror = w * jahHitMirrorPart * gain;
			jahC2PickMirrorSlot = jahHitMirrorSlot;
			jahHitSurf = first;
			jahHitTracedLobe = tracedWas;
			rad = ok2 && finite3( r2 ) ? r2 : vec3( 0.0 );
		}
	}
	vec3 c = w * rad * gain;
	jahC2PickBrdf = c;
	// ...AND THE PORTALS' DRAW: its ray to the world's exit - the sky it sees at p_l / ( q + p_l ), 0 on geometry
	if( sky.ok && dot( sky.L, N ) > 0.0 )
	{
		rayQueryEXT qn;
		rayQueryInitializeEXT( qn, tlas, jahAlphaRayFlags( gl_RayFlagsNoneEXT ), JAH_HIT_LOBE_MASK, o, 0.0, sky.L,
							   jahWorldExitT( o, sky.L, 1.0e4 ) );
		JAH_RQ_PROCEED( qn )
		if( rayQueryGetIntersectionTypeEXT( qn, true ) != gl_RayQueryCommittedIntersectionTriangleEXT )
		{
			const float qL = jahReflectFoldedPdf( N, N, V, sky.L, alpha );
			const float ab = sa + sb;
			if( ab > 1e-4 )
			{
				jahC2PickPortal = w * JAH_HIT_LOBE_SKY( sky.L, alpha, false ) *
								  ( jahReflectFoldedTarget( N, N, V, sky.L, alpha ) / ( ab * max( qL + sky.pdfL, 1e-30 ) ) );
				c += jahC2PickPortal;
			}
		}
	}
	return finite3( c ) ? clamp( c, vec3( 0.0 ), vec3( kMaxRadiance ) ) : vec3( 0.0 );
}

vec3 jahHitRadiance( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					 bool mirror, out bool ok )
{
	uint source;
	return jahHitRadianceStore( slot, hitPos, hitNormal, dir, footprint, mirror, ok, source );
}

#endif   // JAH_RQ_HIT_RADIANCE_GLSL
