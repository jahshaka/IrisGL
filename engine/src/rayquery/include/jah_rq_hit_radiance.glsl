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
// card's radiance whole; the voxels answer where no card does (an instance with
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
vec3 jahHitRadiance( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					 bool mirror, bool cardGated, out bool ok )
{
	{
		const vec3 card = jahCardRadiance( slot, hitPos, hitNormal,
										   ( mirror || !cardGated ) ? 0.0 : footprint, ok );
		if( ok )
			return card;
	}
	return jahVoxelRadiance( hitPos, dir, footprint, mirror, ok );
}

#endif   // JAH_RQ_HIT_RADIANCE_GLSL
