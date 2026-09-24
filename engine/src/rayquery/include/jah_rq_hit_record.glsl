// THE HIT RECORD — a ray hit the caches cannot shade becomes a pixel of a second
// visibility buffer (PHOTON-HIT-SHADE-1; SPECS/atom/D2_HIT_SHADING_DESIGN.md with
// the lead's decisions F1-F8).
//
// WHICH HITS: a hit on a MOVER or a RIGGED item ALWAYS (the voxels exclude movers
// and hold a rig's bind pose, so a voxel read at such a hit is a neighbour's light
// or a ghost's), and a static hit that neither its card nor a cascade answers
// (outside every cascade; the gather's far copies). Everything else is shaded from
// the caches exactly as before (jah_rq_hit_radiance.glsl).
//
// WHAT A RECORD IS: one entry of a COMPACTED LIST — the ray jobs append with an
// atomic counter, so the decode that shades the list (HlmsAtom's hit mode, one
// fragment per record) costs what the records cost and nothing per screen pixel.
// The list is two images of one layout, a grid LIST_W wide, record i at
// (i % LIST_W, i / LIST_W), and one buffer:
//   jahHitIds  RGBA32UI  x = the item slot | the LEVEL the hit BLAS was built
//                            from << 24 (bit 27: "the far copy — the mesh's
//                            coarsest level", which the decode resolves), y = the
//                            primitive (the triangle within that level), z = the
//                            barycentrics u, v (16-bit normalised: 1.5e-5 of an
//                            edge), w = the ray's direction, octahedral, snorm16
//   jahHitDest R32UI     where the shaded radiance goes back: x in bits 0-14, y
//                        in bits 15-29, bit 31 = the gather's atlas (else the
//                        reflection's history)
//   jahHitBuf[] (uint)   [0] = records appended (may pass the capacity), [1] =
//                        records DROPPED because the list was full (a stat, never
//                        a crash), [2..3] unused; then TWO WORDS PER RECORD from
//                        word kJahHitAuxBase: packHalf2x16( the SUN'S VISIBILITY
//                        at the hit (a shadow ray, jah_rq_sun_ray.glsl — replacing
//                        the shadow map, which covers the camera's frustum only),
//                        the ray's FOOTPRINT at the hit in metres (the decode's
//                        texture LOD) ), and the write-back weight's float bits
//                        (the reflection's temporal blend, negated when the
//                        history is warm-held; the gather's texel re-weight).
// WHY A BUFFER AND NOT A THIRD IMAGE: the decode is a PBS pixel shader, and the
// pin's Vulkan table of PASS textures holds 32 slots (NUM_BIND_TEXTURES, its
// bounds assert compiled out; a datablock's own textures are a baked set and do
// not count). A second decode image put the hit mode's last pass texture at
// slot 31, the table's last entry — one more pass texture (decals, an area-light
// mask) and the bind reads past it. The read-only buffer table is separate.
//
// HOW A CALLER BINDS IT (macros defined first):
//   JAH_HIT_ON            bool   the list is bound this dispatch
//   JAH_HIT_CAPACITY      uint   records the list holds
//   JAH_HIT_LIST_W        uint   the grid's width
//   JAH_HIT_INSTANCES     uint   GPU scene instance entries bound (0 = none)
//   JAH_HIT_INSTANCE( s, k ) uvec4, lane k of entry s (10 lanes, GpuScene.h)
//   JAH_HIT_SUN           vec4   xyz = towards the sun, w = 1 when the pass has one
//   JAH_HIT_SUN_MASK      uint   the TLAS mask the sun ray asks for (the casters)
//   JAH_HIT_SUN_RANGE     float  the sun ray's length
//   JAH_HIT_LIFT          float  the shadow ray's minimum lift off the hit, metres
//   JAH_HIT_FAR_LIFT      float  ...and off a FAR copy's hit (the widest coarse-vs-
//                                fine gap of the traced set, the gather's farOverlap)
// ...the images `jahHitIds`, `jahHitDest` and the buffer member `jahHitBuf[]`, and jah_rq_geom.glsl + jah_rq_sun_ray.glsl included before.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_RECORD_GLSL
#define JAH_RQ_HIT_RECORD_GLSL

const uint kJahHitEmpty = 0xFFFFFFFFu;
const uint kJahHitFarBit = 1u << 27u;
const uint kJahHitDestGather = 0x80000000u;
/// The first per-record word of `jahHitBuf` (two per record).
const uint kJahHitAuxBase = 4u;
/// GpuScene.h GpuInstanceFlag: kGpuMover | kGpuSkinned.
const uint kJahHitGpuMover = 1u << 2u;
const uint kJahHitGpuSkinned = 1u << 5u;

uint jahHitFlags( uint slot )
{
	if( slot >= JAH_HIT_INSTANCES )
		return 0u;
	return JAH_HIT_INSTANCE( slot, 7u ).w;   // boundsMax.w: the flags word
}

/// A mover or a rigged item: the caches cannot hold it, the decode always shades it.
bool jahHitAlwaysDecodes( uint slot )
{
	return ( jahHitFlags( slot ) & ( kJahHitGpuMover | kJahHitGpuSkinned ) ) != 0u;
}

/// The LEVEL the NEAR copy of `slot` was built from — the level its per-slot
/// geometry row names (GpuScene::geomRowIndex: mesh * 64 + level * 8 + submesh).
/// A rigged item's near copy is its skin cache, level 0 (the decode reads the
/// instance's skin row).
uint jahHitNearLevel( uint slot )
{
	if( ( jahHitFlags( slot ) & kJahHitGpuSkinned ) != 0u || slot >= JAH_GEOM_SLOTS )
		return 0u;
	const uint row = JAH_GEOM_ROW_OF( slot );
	return row == 0xFFFFFFFFu ? 0u : ( ( row >> 3u ) & 0x7u );
}

vec2 jahOctEncode( vec3 n )
{
	n /= max( abs( n.x ) + abs( n.y ) + abs( n.z ), 1e-20 );
	vec2 e = n.xy;
	if( n.z < 0.0 )
		e = ( 1.0 - abs( n.yx ) ) * vec2( n.x >= 0.0 ? 1.0 : -1.0, n.y >= 0.0 ? 1.0 : -1.0 );
	return e;
}

uint jahHitDestPack( ivec2 xy, bool gather )
{
	return ( uint( xy.x ) & 0x7FFFu ) | ( ( uint( xy.y ) & 0x7FFFu ) << 15u ) |
		   ( gather ? kJahHitDestGather : 0u );
}

/// THE SUN AT A HIT: a shadow ray from the hit, lifted along `liftDir` (the hit
/// triangle's geometric normal facing the ray's origin, or the reversed ray where
/// there is none) by the larger of the floor and 1e-4 of the distance (the hit
/// point's own float precision), towards the pass' sun, against the casters' near
/// copies, the whole sun range with no fade band. 1 where the pass has no sun.
float jahHitSunVisibility( vec3 hitPos, vec3 liftDir, float t, bool far )
{
	if( JAH_HIT_SUN.w < 0.5 )
		return 1.0;
	const float lift = max( far ? JAH_HIT_FAR_LIFT : JAH_HIT_LIFT, 1e-4 * t );
	return jahSunRay( hitPos + liftDir * lift, JAH_HIT_SUN.xyz, JAH_HIT_SUN_MASK,
					  JAH_HIT_SUN_RANGE, JAH_HIT_SUN_RANGE );
}

/// Appends one record. False when the list is full (the record is dropped and
/// counted) or not bound this dispatch — the caller then treats the hit as the
/// write-back's "unshaded" case.
bool jahHitAppend( uint slot, uint level, bool far, uint prim, vec2 bary, vec3 dir, float sunVis,
				   float footprint, float weight, uint dest )
{
	if( !( JAH_HIT_ON ) || JAH_HIT_CAPACITY == 0u )
		return false;
	const uint i = atomicAdd( jahHitBuf[0], 1u );
	if( i >= JAH_HIT_CAPACITY )
	{
		atomicAdd( jahHitBuf[1], 1u );
		return false;
	}
	const ivec2 at = ivec2( int( i % JAH_HIT_LIST_W ), int( i / JAH_HIT_LIST_W ) );
	const uint slotLevel = ( slot & 0x00FFFFFFu ) | ( ( level & 0x7u ) << 24u ) | ( far ? kJahHitFarBit : 0u );
	imageStore( jahHitIds, at,
				uvec4( slotLevel, prim, packUnorm2x16( clamp( bary, vec2( 0.0 ), vec2( 1.0 ) ) ),
					   packSnorm2x16( jahOctEncode( dir ) ) ) );
	jahHitBuf[kJahHitAuxBase + 2u * i] = packHalf2x16( vec2( sunVis, footprint ) );
	jahHitBuf[kJahHitAuxBase + 2u * i + 1u] = floatBitsToUint( weight );
	imageStore( jahHitDest, at, uvec4( dest, 0u, 0u, 0u ) );
	return true;
}

#endif   // JAH_RQ_HIT_RECORD_GLSL
