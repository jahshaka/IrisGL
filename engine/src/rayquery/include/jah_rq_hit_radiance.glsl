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
vec3 jahHitRadianceStore( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
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

// ---- C2: THE HIT's OWN SPECULAR (PHOTON-7 11-2 (3); ONE_PICTURE C2; the lead's rulings D1-D6) ---------------------------
// The store holds a surface's INDIRECT specular only as its lobe's INTEGRAL, re-emitted alike in every direction: a
// polished hit seen in a reflection showed none of what it reflects (a glossy slab under an emitter, a lacquered mover
// against the same object still, the crease where a block meets a mirror floor). A reader that traces the hit's lobe
// itself sets jahHitTracedLobe around its store read (the store then leaves the integral out) and adds this:
// ONE SECONDARY RAY - the hit's own VNDF draw at its alpha (the centre ray at the packing floor: a sharp metal),
// weighed by the hit's split DFG ( F0 A( mu, r ) + w B( mu, r ) ) from its material row and the draw's gain (G2/G1 over
// the F-free A + B, jahReflectGain), the hit at its end read by the STORE - the card, the cascades, the direct - whose
// own specular is its lobe's integral: NO RECURSION (the store IS the environment lobe; a sky with no visibility would
// be light through a wall, L9). An escape past the world's exit is the sky (JAH_HIT_LOBE_SKY, the includer's), a back
// face of a one-sided surface none, a hit only a record could shade (a rigged item, a planar mirror) none (the store does not
// hold it). ALWAYS TRACED (the lead's ruling (3b)): a Russian roulette on the weight was unbiased in linear radiance but
// the reflection's denoiser averages in Lumen's compressive space, where the mean of a 1 / p-spiky sample is biased low
// (Jensen; measured: a roughness-0.12 mirror's slab 20 % dark) - REFUSED. The lights' direct lobe stays the store's (a
// light is not geometry).
// `P` the hit, `N` its geometric normal facing the ray's origin, `V` toward that origin, `carried` the distance P was
// carried along (its float bound), `roughness` and `metal` the hit's (jahHitMaterial), `u` two uniforms (the draw).
#ifndef JAH_HIT_LOBE_SKY
#define JAH_HIT_LOBE_SKY( d, alpha, centre ) vec3( 0.0 )
#endif
#ifndef JAH_HIT_LOBE_MASK
#define JAH_HIT_LOBE_MASK 0x0Fu   // (the near copies, kRayMaskNearField; a cache's gather passes its still mask)
#endif
#include "jah_world_exit.glsl"
#include "jah_reflect_draw.glsl"
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
	// THE RAY: the one origin law off a hit on level 0 (no drawn surface's bound), to the world's exit
	const vec3 o = jahRayOrigin( P, N, carried, 0.0 );
	rayQueryEXT q;
	rayQueryInitializeEXT( q, tlas, jahAlphaRayFlags( gl_RayFlagsNoneEXT ), JAH_HIT_LOBE_MASK, o, 0.0, L2,
						   jahWorldExitT( o, L2, 1.0e4 ) );
	JAH_RQ_PROCEED( q )
	vec3 rad = vec3( 0.0 );
	if( rayQueryGetIntersectionTypeEXT( q, true ) != gl_RayQueryCommittedIntersectionTriangleEXT )
		rad = JAH_HIT_LOBE_SKY( L2, alpha, centre );   // an exit: the sky
	else
	{
		const float t2 = rayQueryGetIntersectionTEXT( q, true );
		const uint slot2 = uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, true ) );
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
			const vec3 r2 = jahHitRadianceStore( slot2, o + L2 * t2, n2, L2, max( 2.0 * t2 * sqrt( alpha ), 1e-4 ),
												 centre, ok2, src2 );
			jahHitSurf = first;
			jahHitTracedLobe = tracedWas;
			rad = ok2 && finite3( r2 ) ? r2 : vec3( 0.0 );
		}
	}
	const vec3 c = w * rad * gain;
	return finite3( c ) ? clamp( c, vec3( 0.0 ), vec3( kMaxRadiance ) ) : vec3( 0.0 );
}

vec3 jahHitRadiance( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					 bool mirror, out bool ok )
{
	uint source;
	return jahHitRadianceStore( slot, hitPos, hitNormal, dir, footprint, mirror, ok, source );
}

#endif   // JAH_RQ_HIT_RADIANCE_GLSL
