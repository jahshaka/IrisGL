// THE CHANGED-REGION LIST AND THE BRICK MASKS AS A REST READS THEM (PHOTON-7 stage 8B: B1's gather resume and B2's
// reflection resume read the same list, one text). The list is PhotonComponent::closeRegions' one buffer as it closed
// before the render: a header of two vec4s (count, overflow, restart, frame | toSun, sun) then two vec4s a region (lo,
// kind | hi, cascades) - Types.h GiRegionKind and GiChangedRegions. The includer defines JAH_REST_REGIONS_BINDING.
#ifndef JAH_REST_REGIONS_GLSL
#define JAH_REST_REGIONS_GLSL

#ifdef JAH_REST_REGIONS_BINDING
layout( std140, set = 0, binding = JAH_REST_REGIONS_BINDING ) uniform JahRestRegions { vec4 regionWords[2 + 2 * 128]; };
vec4 regionLo( uint k ) { return regionWords[2u + 2u * k]; }
vec4 regionHi( uint k ) { return regionWords[3u + 2u * k]; }
uint regionKind( uint k ) { return floatBitsToUint( regionLo( k ).w ); }
#endif

/// The kinds (Types.h GiRegionKind): GEOMETRY changed in a Mover's (a moved or posed slot's swept box) or an Edit's
/// box - it blocks or unblocks any ray across it; LIGHT changed in the others' - what a HIT inside reads.
const uint kRegionSlab = 1u, kRegionCard = 2u, kRegionEdit = 3u, kRegionMover = 4u, kRegionMoverShadow = 5u,
		   kRegionLampShadow = 6u, kRegionLamp = 7u, kRegionCardScene = 8u;

/// A BRICK CHANGED AT ALL this frame, either of its two words (PhotonVoxelLighting's brick mask: the direct term's and
/// the feedback's relative change; the feedback writes a voxel only when it moves a store step, so a converged store
/// writes nothing). Never a per-frame threshold (rq_probe_resume.comp's kRestBrickCode); a NaN word is a change (the
/// bits are tested, never x == x). The gather's resume, its fast update's moving ray (rq_probe_gather.comp) and the
/// reflection's resume (rq_reflect.comp) read it.
bool jahBrickChangedWords( uint a, uint b )
{
	const bool na = ( a & 0x7F800000u ) == 0x7F800000u, nb = ( b & 0x7F800000u ) == 0x7F800000u;
	return na || nb || uintBitsToFloat( a ) > 0.0 || uintBitsToFloat( b ) > 0.0;
}

#endif
