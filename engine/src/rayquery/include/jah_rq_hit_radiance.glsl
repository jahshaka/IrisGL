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
// the sample's footprint at the hit, in world metres, is the ONE parameter
// that decides both reads. The card is read only while it is at most
// JAH_CARD_FOOTPRINT_TEXELS of the picked card's texels (the atlas has no mips
// until SC-2, so a wider footprint on a texel-exact read is speckle); the
// voxels are read at the mip whose texel is that wide. Each CALLER derives its
// own footprint, because each knows what one of its samples stands for:
//   * the REFLECTION (rq_reflect.comp): the spacing between the samples its
//     temporal mean holds over the lobe, 2 t alpha / sqrt(N), N = 1/knobs2.w;
//     a mirror (alpha at or below the mirror cut) reads the card texel-exact
//     and the containing cascade whole at LOD 0 (`mirror`);
//   * the GATHER (rq_probe_gather.comp): its own ray's footprint at the hit,
//     t x sqrt(2/rays) — one ray stands for one octahedral texel's solid angle.
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
/// a mirror's (the card texel-exact, the voxels at LOD 0 with no crossfade).
/// `ok` is false where neither the card nor the cascades can shade the hit.
vec3 jahHitRadiance( uint slot, vec3 hitPos, vec3 hitNormal, vec3 dir, float footprint,
					 bool mirror, out bool ok )
{
	{
		const vec3 card = jahCardRadiance( slot, hitPos, hitNormal, mirror ? 0.0 : footprint, ok );
		if( ok )
			return card;
	}
	return jahVoxelRadiance( hitPos, dir, footprint, mirror, ok );
}

#endif   // JAH_RQ_HIT_RADIANCE_GLSL
