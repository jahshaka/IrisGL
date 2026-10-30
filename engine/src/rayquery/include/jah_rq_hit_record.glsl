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
//   jahHitIds  RGBA32UI  x = the item slot (bits 0-23; bit 27: "the far copy —
//                            the mesh's coarsest level", which the decode resolves,
//                            else the near copy, level 0 since PHOTON-7 stage 6; bit 28:
//                            "the DIFFUSE response only" — every gather record,
//                            set by jahHitAppend from the destination), y = the
//                            primitive (the triangle within that level), z = the
//                            barycentrics u, v (16-bit normalised: 1.5e-5 of an
//                            edge), w = the ray's direction, octahedral, snorm16
//   jahHitDest R32UI     where the shaded radiance goes back: x in bits 0-14, y
//                        in bits 15-29, bit 31 = the gather's atlas (else the
//                        reflection's history)
//   jahHitBuf[] (uint)   [0] = records appended (may pass the capacity), [1] =
//                        records DROPPED because the list was full (a stat, never
//                        a crash), [2] = the reflection's active resting texels,
//                        [3] = its CAPPED texels, [4] = its BATCHING texels
//                        (the rest's tail budget), [5] = the exposure its
//                        denoiser space ran at and [6] its hand-over length N
//                        (float bits, the trace's: the write-back folds a
//                        texel's RECORD RUN in the same space), [7] = the run
//                        records the decode left unshaded (their index lost),
//                        [8] / [9] the reflection's rest BOUND's two (PHOTON-7
//                        11-1c), [10] the reflection's NEE rays and [11] those
//                        whose first hit was the target drawn (11-1b: the cone's
//                        hit rate); then kJahHitAuxWords (NINE) WORDS PER RECORD
//                        from word kJahHitAuxBase (12):
//                        [0] packHalf2x16( the SUN'S VISIBILITY at the hit (a
//                        shadow ray, jah_rq_sun_ray.glsl — replacing the shadow
//                        map, which covers the camera's frustum only), the ray's
//                        FOOTPRINT at the hit in metres (the decode's texture
//                        LOD) ), [1] the write-back weight's float bits (the
//                        reflection's sample weight — the trace's blend floor and
//                        its surface-only flag, PHOTON-2 1b; the gather's texel
//                        re-weight; NEGATIVE marks a reflection RUN record,
//                        magnitude 1), [2] the ray's
//                        ORIGIN HEIGHT's float bits (world Y) and [3]
//                        packHalf2x16( the ray's LENGTH to the hit, the EYE'S
//                        DISTANCE to the ray's origin ), both in metres and held
//                        under the half's range — what the write-back fogs a
//                        reflection's decoded light along (PHOTON-I-1: the
//                        record, not a full-resolution image, carries them; the
//                        gather's records write them too and nothing reads them),
//                        [8] (PHOTON-7 11-2 L9 (b1)) the decoded hit's AREA LIGHTS' VISIBILITY, byte i that of the
//                        pass's forward LTC light i (jahHitAreaVisWord: one traced stratum, 255 seen),
//                        (the screen's share of a blended sample, words 4-5, is deleted with the Both estimator,
//                        PHOTON-7-9A: 8 bytes a record, 2.3 MB at the list's 1080p capacity)
//                        [4] packHalf2x16( THE NEE DELTA's r, g ) and [5]
//                        packHalf2x16( its b, THE ESTIMATOR's GAIN - the
//                        reflection's factor on the decoded ray share,
//                        PHOTON-7 11-2 (1); 1 on the gather's records )
//                        (PHOTON-7 11-1b, the preflight's gap G3): what the
//                        write-back adds to the shaded sample - the NEE part less
//                        the BRDF's share ( 1 - v ) ( 1 - w_b ) E T of the
//                        emission its hit holds, signed; 0 on every other record;
//                        [6] packHalf2x16( the crossed PANES' own light r, g )
//                        and [7] packHalf2x16( ...its b, the panes' THROUGHPUT ):
//                        a REFLECTION's record seen through panes (the one
//                        read's F6) - the write-back's ray share is the panes'
//                        light plus the throughput x the decoded light, and the
//                        throughput attenuates the NEE delta too (it lives at
//                        the hit, beyond the panes); (0, 1) on every other
//                        record (the gather carries its throughput in its weight
//                        and writes its panes' light).
//                        Words 4-7 = 16 B a record; 36 B a record in all (10.5 MB at
//                        the list's capacity at 1080p, 291,840 records).
// WHY A BUFFER AND NOT A THIRD IMAGE: the decode is a PBS pixel shader, and its
// pass textures number into the fork's absolute-index table (NUM_BIND_TEXTURES;
// the budget per family is OgreFog.cpp's). The read-only buffer slot it takes
// numbers the textures one later too, but a record needs no sampler.
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
//   JAH_HIT_FAR_LIFT      float  the shadow ray's surface term off a FAR copy's hit: the
//                                traced set's coarsest level's gap from level 0 (farOverlap,
//                                the drift law's: Types.h rayOriginLevelLift at its source);
//                                a near hit is ON level 0, so its surface term is 0
// ...the images `jahHitIds`, `jahHitDest` and the buffer member `jahHitBuf[]`, and jah_rq_geom.glsl + jah_rq_sun_ray.glsl included before.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_HIT_RECORD_GLSL
#define JAH_RQ_HIT_RECORD_GLSL
#include "jah_ray_origin.glsl"

const uint kJahHitEmpty = 0xFFFFFFFFu;
const uint kJahHitFarBit = 1u << 27u;
/// THE GATHER'S HITS ARE DIFFUSE-ONLY (SPECKLE-FIX-1, the owner's option 1 — the
/// Lumen choice): a screen-probe ray estimates the IRRADIANCE at its origin, and the
/// caches that answer most of its hits (the cards, the voxels) store the diffuse
/// response alone (albedo x irradiance, plus the emissive). A decoded hit shaded
/// with its specular lobe answered the same ray with a different quantity — on a
/// glossy metal under the sun, the highlight at hundreds x the sky — so a probe
/// whose hit changed route (a moved object's cards re-allocate) flashed a disc on
/// the floor. The decode reads this bit and drops every specular term (the
/// lights', the environment's, the clear coat's). The REFLECTION's records keep
/// the full lobe: a mirror shows the highlight.
const uint kJahHitDiffuseOnlyBit = 1u << 28u;
/// A REFLECTION RECORD WHOSE LOBE THE TRACE TRACED (C2, PHOTON-7 11-2 (3)): the record's environment specular is the
/// traced secondary ray's, riding its delta; the decode zeroes its own (HlmsAtom custom_ps_preEnvBrdf) and keeps the
/// lights' lobe.
const uint kJahHitTracedLobeBit = 1u << 29u;
const uint kJahHitDestGather = 0x80000000u;
/// The first per-record word of `jahHitBuf`, and the words per record.
const uint kJahHitAuxBase = 12u;
const uint kJahHitAuxWords = 9u;
/// GpuScene.h GpuInstanceFlag: kGpuSkinned | kGpuPlanar.
const uint kJahHitGpuSkinned = 1u << 5u;
const uint kJahHitGpuPlanar = 1u << 10u;
/// GpuScene.h kGpuTwoSided (a CULL_NONE macroblock: both faces are the surface) - a cache ray's back-face rule
/// (rq_card_gather.comp).
const uint kJahHitGpuTwoSided = 1u << 11u;

uint jahHitFlags( uint slot )
{
	if( slot >= JAH_HIT_INSTANCES )
		return 0u;
	return JAH_HIT_INSTANCE( slot, 7u ).w;   // boundsMax.w: the flags word
}

/// A RIGGED item: the caches hold its bind pose (the ray tier's skin cache is not theirs), the decode always shades it.
/// A PLANAR MIRROR (D3-HIT-SHADE-2): the caches hold only its diffuse (black for a
/// metal mirror), and a second ray from every such hit is a second bounce; the
/// decode shades its own material with the pass' specular environment (the voxel
/// cone, the sky) — a mirror seen in a reflection, never a black one.
/// (A MOVER is not decoded: it answers from the store like any hit since PHOTON-7 8A A1 - the store holds it at its pose.)
bool jahHitAlwaysDecodes( uint slot )
{
	return ( jahHitFlags( slot ) & ( kJahHitGpuSkinned | kJahHitGpuPlanar ) ) != 0u;
}


#include "jah_oct_dir.glsl"

/// A destination: x in bits 0-14, y in 15-29, THE EYE'S LAYER in bit 30 (a
/// stereo target is a two-layer array, LAYERED-STEREO-1; a probe-atlas
/// destination has none), the gather flag in bit 31.
uint jahHitDestPack( ivec2 xy, bool gather, int layer )
{
	return ( uint( xy.x ) & 0x7FFFu ) | ( ( uint( xy.y ) & 0x7FFFu ) << 15u ) |
		   ( uint( layer & 1 ) << 30u ) | ( gather ? kJahHitDestGather : 0u );
}

/// THE SUN AT A HIT: a shadow ray from the hit under THE ONE ORIGIN LAW (jah_ray_origin.glsl; the lead's round-7
/// ruling: the 1 mm / 1e-4 t floor is deleted) - lifted along `liftDir` (the hit triangle's geometric normal facing
/// the ray's origin, or the reversed ray where there is none) by the hit's own float bound (its position and the
/// distance `t` it was carried) plus its surface term (0 on level 0, which the casters' near copies are; a far copy's
/// JAH_HIT_FAR_LIFT), towards the pass' sun, the whole sun range with no fade band. 1 where the pass has no sun.
float jahHitSunVisibility( vec3 hitPos, vec3 liftDir, float t, bool far )
{
	if( JAH_HIT_SUN.w < 0.5 )
		return 1.0;
	return jahSunRay( jahRayOrigin( hitPos, liftDir, t, far ? JAH_HIT_FAR_LIFT : 0.0 ), JAH_HIT_SUN.xyz,
					  JAH_HIT_SUN_MASK, JAH_HIT_SUN_RANGE, JAH_HIT_SUN_RANGE );
}

/// THE RECORD'S WORDS at list slot i (reserved by the caller, < the capacity). A
/// NEGATIVE weight marks a reflection FOLD record (rq_reflect.comp's record runs:
/// its texel's rest mean takes it in the write-back, in its run's order), its
/// magnitude 1; `pane` = a reflection's crossed panes (their own light, their throughput; (0, 1) none).
void jahHitWrite( uint i, uint slot, bool far, uint prim, vec2 bary, vec3 dir, float sunVis,
				  float footprint, float weight, uint dest, float originY, float rayLength, float eyeDistance,
				  vec3 delta, float gain, vec4 pane, bool tracedLobe, uint areaVis )
{
	const ivec2 at = ivec2( int( i % JAH_HIT_LIST_W ), int( i / JAH_HIT_LIST_W ) );
	const uint slotLevel = ( slot & 0x00FFFFFFu ) | ( far ? kJahHitFarBit : 0u ) |
						   ( ( dest & kJahHitDestGather ) != 0u ? kJahHitDiffuseOnlyBit : 0u ) |
						   ( tracedLobe ? kJahHitTracedLobeBit : 0u );
	imageStore( jahHitIds, at,
				uvec4( slotLevel, prim, packUnorm2x16( clamp( bary, vec2( 0.0 ), vec2( 1.0 ) ) ),
					   packSnorm2x16( jahOctEncode( dir ) ) ) );
	const uint aux = kJahHitAuxBase + kJahHitAuxWords * i;
	jahHitBuf[aux] = packHalf2x16( vec2( sunVis, footprint ) );
	jahHitBuf[aux + 1u] = floatBitsToUint( weight );
	jahHitBuf[aux + 2u] = floatBitsToUint( originY );
	jahHitBuf[aux + 3u] = packHalf2x16( clamp( vec2( rayLength, eyeDistance ), vec2( 0.0 ), vec2( 65504.0 ) ) );
	jahHitBuf[aux + 4u] = packHalf2x16( clamp( delta.xy, vec2( -65504.0 ), vec2( 65504.0 ) ) );
	jahHitBuf[aux + 5u] = packHalf2x16( vec2( clamp( delta.z, -65504.0, 65504.0 ), clamp( gain, 0.0, 65504.0 ) ) );
	jahHitBuf[aux + 6u] = packHalf2x16( clamp( pane.xy, vec2( 0.0 ), vec2( 65504.0 ) ) );
	jahHitBuf[aux + 7u] = packHalf2x16( clamp( pane.zw, vec2( 0.0 ), vec2( 65504.0 ) ) );
	jahHitBuf[aux + 8u] = areaVis;   // (L9 (b1): the decoded hit's area lights' visibility, byte i the LTC set's light i)
	imageStore( jahHitDest, at, uvec4( dest, 0u, 0u, 0u ) );
}

/// Appends one record. False when the list is full (the record is dropped and
/// counted) or not bound this dispatch — the caller then treats the hit as the
/// write-back's "unshaded" case.
bool jahHitAppend( uint slot, bool far, uint prim, vec2 bary, vec3 dir, float sunVis,
				   float footprint, float weight, uint dest, float originY, float rayLength, float eyeDistance,
				   vec3 delta, float gain, vec4 pane, bool tracedLobe, uint areaVis )
{
	if( !( JAH_HIT_ON ) || JAH_HIT_CAPACITY == 0u )
		return false;
	const uint i = atomicAdd( jahHitBuf[0], 1u );
	if( i >= JAH_HIT_CAPACITY )
	{
		atomicAdd( jahHitBuf[1], 1u );
		return false;
	}
	jahHitWrite( i, slot, far, prim, bary, dir, sunVis, footprint, weight, dest, originY, rayLength, eyeDistance,
				 delta, gain, pane, tracedLobe, areaVis );
	return true;
}

#endif   // JAH_RQ_HIT_RECORD_GLSL
