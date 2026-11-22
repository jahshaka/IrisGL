// A CUT-OUT IS GEOMETRY WITH HOLES — THE RAY QUERIES' CANDIDATE TEST
// (REFLECT-MOVERS-2; audit C-16 answered).
//
// Every bottom-level structure is VK_GEOMETRY_OPAQUE_BIT_KHR, so an opaque
// triangle commits inside the traversal and never reaches this file: the fast
// path is the hardware's and unchanged. An ALPHA-TESTED item's instance carries
// VK_GEOMETRY_INSTANCE_FORCE_NO_OPAQUE_BIT_KHR (rq_tlas_write.comp), so its
// triangles come back to the query as CANDIDATES; the loop below takes each one's
// texture coordinate (the geometry row's UVs at the candidate's barycentrics),
// applies the material's base-map UV transform (the datablock's user values 0-1,
// OgreMaterials.cpp) and reads the material's MASK — its albedo texture's alpha
// thresholded at the cutoff, one bit a texel (the engine's store, AlphaMasks.h:
// level 0 of the mask's pyramid) — and confirms the candidate only where the
// texel is solid.
// A ray that asked for the first hit (the sun's) stops at the first solid one.
//
// THE TABLE (one per scene, the TLAS's half; rebuilt on the host when the traced
// alpha-tested set, a row or the GPU scene's mask table changes), read through
// its device address (the caller defines JAH_ALPHA_TABLE, a uvec2; zero = no
// table: the ray flags stay opaque and this loop never sees a candidate):
//   uint[0] = N, the slots the per-slot index covers; uint[1] = entries;
//   uint[2..3] = the GPU scene's GEOMETRY-ROW buffer's device address (three
//   uvec4 lanes a row, GpuScene::geomBuffer); uint[4..5] = the GPU scene's
//   MASK TABLE's device address (jah_alpha_mask.glsl: per slot and submesh, the
//   material's mask and its UV transform - ENGINE-POLISH-ATOM-1 stage 3, the
//   masks the engine's); uint[6..7] = 0
//   uint[8 + s] = the word where slot s's entry starts, 0xFFFFFFFF none (s < N)
//   an entry: (near row base, far row base, 0, 0) — the row of submesh 0 of the
//   level the near/far copy was built from (a rigged item's skin row for both);
//   submesh g's row is base + g (GpuScene::geomRowIndex).
// A candidate is REFUSED (not confirmed) while its mask is pending and while its
// row is not known yet (a rigged item's first ready frame): before REFLECT-MOVERS-2
// the item was not traced at all, and a solid quad is the one wrong answer. A
// geometry past the mask table's submeshes for the slot (a mesh of more than
// GpuScene::kSubmeshesPerMesh = 8 submeshes: those carry no row) is SOLID — its
// own datablock is not in the table; so is an opaque submesh of a cut-out item
// (the tree's trunk: a constant solid record).
//
// A ray reads the mask's LEVEL 0 — NEAREST texel at the texture's first level no
// larger than AlphaMaskStore::kMaxSide on a side, wrapped (a ray carries no
// footprint; the Atom passes read the pyramid level theirs asks for): a ray and a
// pixel can disagree within half a mask texel of an edge — the bar in
// gi.rt_alpha_tested is the coverage, never the edge.
//
// The caller enables GL_EXT_ray_query, GL_EXT_buffer_reference and
// GL_EXT_buffer_reference_uvec2 and defines JAH_ALPHA_TABLE (the geometry rows'
// decode, jah_geom_rows.glsl, is included here). No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_ALPHA_GLSL
#define JAH_RQ_ALPHA_GLSL

#include "jah_rq.glsl"
#include "jah_rq_finite.glsl"
#include "jah_geom_rows.glsl"
#include "jah_alpha_mask.glsl"

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahAlphaTableRef
{
	uint u[];
};
layout( buffer_reference, std430, buffer_reference_align = 16 ) readonly buffer JahAlphaRowsRef
{
	uvec4 lane[];
};

const uint kJahAlphaNone = 0xFFFFFFFFu;

/// The ray flags a query asks with: NONE - every bottom-level structure is opaque, so
/// only a FORCE_NO_OPAQUE instance (a cut-out, or a PANE: PHOTON-7 10-4 R7) makes a
/// candidate, and every other triangle commits inside the traversal as before. (The
/// opaque flag this used to ask where the scene had no alpha table would make a pane
/// opaque.)
uint jahAlphaRayFlags( uint extra )
{
	return JAH_RQ_FLAGS_NONE | extra;
}

/// Is the candidate triangle SOLID at its barycentrics? `instanceId` is the TLAS
/// instance's index (two a slot: even the near copy, odd the far one), `geom` the
/// candidate's geometry within its BLAS (= its submesh).
bool jahAlphaSolid( uint slot, uint instanceId, uint geom, uint prim, vec2 bary )
{
	const uvec2 t = JAH_ALPHA_TABLE;
	if( ( t.x | t.y ) == 0u )
		return true;
	JahAlphaTableRef T = JahAlphaTableRef( t );
	const uint n = T.u[0];
	if( slot >= n )
		return true;
	const uint base = T.u[8u + slot];
	if( base == kJahAlphaNone )
		return true;
	const uvec2 masks = uvec2( T.u[4], T.u[5] );
	const uint rec = jahMaskRecord( masks, slot, geom );
	if( rec == kJahMaskNone )
		return true;
	const uint flags = jahMaskFlags( masks, rec );
	if( ( flags & kJahMaskPending ) != 0u )
		return false;
	if( ( flags & kJahMaskConstant ) != 0u )
		return ( flags & kJahMaskConstantSolid ) != 0u;
	const uint rowBase = T.u[base + ( ( instanceId & 1u ) != 0u ? 1u : 0u )];
	if( rowBase == kJahAlphaNone )
		return false;
	JahAlphaRowsRef R = JahAlphaRowsRef( uvec2( T.u[2], T.u[3] ) );
	const uint row = ( rowBase + geom ) * 3u;
	GeometryRow g;
	g.addresses = R.lane[row];
	g.layout0 = R.lane[row + 1u];
	g.layout1 = R.lane[row + 2u];
	if( ( g.addresses.x | g.addresses.y ) == 0u || ( g.addresses.z | g.addresses.w ) == 0u )
		return false;
	if( ( g.layout1.x & GeomUvMask ) == GeomUvNone )
		return true;
	const vec3 b = vec3( 1.0 - bary.x - bary.y, bary.x, bary.y );
	vec2 uv = vec2( 0.0 );
	for( uint k = 0u; k < 3u; ++k )
		uv += geomUv( g, geomIndex( g, prim * 3u + k ) ) * b[k];
	return jahMaskSolidAt( masks, rec, uv );
}

/// THE BOUND OF EVERY RAY QUERY LOOP (GATHER-HANG-1 stage 2; CODING_RULES section 2: every data-driven GPU loop is bounded
/// in the shader, its count clamped at its law). A loop's trip count is not ours: jahRqProceed hands back one CANDIDATE a
/// trip, and a candidate is one non-opaque triangle (a cut-out's or a pane's: every bottom level is opaque, only a
/// FORCE_NO_OPAQUE instance makes one) that the ray crosses inside its live interval - an opaque triangle commits inside
/// the hardware and never reaches the shader. NO STRUCTURE BOUNDS IT: a mesh may stack any number of cut-out quads along
/// one ray, and a pane is never committed, so every pane crossed is a trip. THE LAW IS THEREFORE A BUDGET,
/// kJahRqStepLaw = 1024 trips: sixteen times the deepest harness scene's 64 (stage 0 measured 14 at most over 3.3 M loops a
/// frame); THE BOUND is four times it, kJahRqStepBound = 4096, about 4096 mask tests for one ray - far below a TDR. A loop
/// that reaches the bound TERMINATES its query and raises its `cut` flag. WHAT A CUT RAY ANSWERS (the lead's ruling (B),
/// 2026-11-22): the hardware hands candidates and commits opaque triangles in a FREE order, so what a terminated query
/// holds is not a function of the scene - it is never read. A CUT RAY ANSWERS AS OCCLUDED: a LIGHT or visibility ray
/// returns 0 (blocked), a READING ray (the reflection, the gather, a card's or a side's gather) a BLACK HIT at the ray's
/// end (no radiance, no sky, no pane light) - so a cut can never ADD light: the picture under a cut is darker than the
/// truth, never brighter, and never the sky through a wall. A CUT IS A BIAS of ONE sign, and the counter exists to make
/// it VISIBLE, never silent. JAH_RQ_PROCEED_TRANSMIT answers it itself (its T set to 0); every caller of the other three
/// forms reads its `cut` flag before the committed intersection.
/// PUBLISHED by the five jobs with the hit params (the probe gather, the reflection, the glass escape, the cards' gather,
/// the voxel sides; the others are bounded the same and publish nothing - a named residue): a loop whose trips reach the
/// threshold atomicMax-es them into the tier's counter [1]. In every real scene no loop reaches 1024, so the cost is one
/// compare a loop and no write: the counter reads 0 = no loop reached the law, 1024 to 4095 how deep the worst went, the
/// bound = a loop was CUT (RayQueryStatus::proceedStepsMax; the tier's close logs it). The bound and the counter are per
/// query: order-free across views and eyes.
/// THE LOOP WORD (JAH_HIT_PARAMS.counts.w, its float bits; ScreenProbeGather.h HitSetSources): bits 0-12 the publishing
/// threshold (0 = the law), bits 13-25 a lowered bound (0 = kJahRqStepBound; never above it), bit 26 the NaN door (THE
/// RAY's INPUTS, below). All 0 in the shipped picture; the test arms rayquery.proceedPublishAll (threshold 1: every loop
/// publishes its true small maximum - the negative control), rayquery.stepBound (the bound lowered: a loop is cut on
/// purpose) and rayquery.nanRay set them. The threshold never exceeds the bound, so a cut is always published.
const uint kJahRqStepLaw = 1024u;
const uint kJahRqStepBound = 4u * kJahRqStepLaw;
#if defined( JAH_HIT_PARAMS ) && defined( GL_EXT_buffer_reference )
/// THE TIER'S COUNTERS (the hit set's surface.zw: a host-visible buffer's address, OgreRayQuery.cpp mTierCounters):
/// [0] the reading rays' pane-list overflows; [1] the deepest ray query loop published (THE BOUND, above); [2] the rays
/// refused for their inputs (THE RAY's INPUTS, below)
layout( buffer_reference, std430, buffer_reference_align = 4 ) buffer JahTierCounters { uint c[]; };
void jahPanesOverflowCount()
{
	const uvec2 a = floatBitsToUint( JAH_HIT_PARAMS.surface.zw );
	if( ( a.x | a.y ) != 0u )
		atomicAdd( JahTierCounters( a ).c[0], 1u );
}
uint jahRqLoopWord() { return floatBitsToUint( JAH_HIT_PARAMS.counts.w ); }
uint jahRqStepBoundNow()
{
	const uint b = ( jahRqLoopWord() >> 13u ) & 8191u;
	return b == 0u ? kJahRqStepBound : min( b, kJahRqStepBound );
}
void jahRqStepsPublish( uint n, uint bound )
{
	const uint t = jahRqLoopWord() & 8191u;
	if( n < min( t == 0u ? kJahRqStepLaw : t, bound ) )
		return;
	const uvec2 a = floatBitsToUint( JAH_HIT_PARAMS.surface.zw );
	if( ( a.x | a.y ) != 0u )
		atomicMax( JahTierCounters( a ).c[1], n );
}
void jahRqRefusedCount()
{
	const uvec2 a = floatBitsToUint( JAH_HIT_PARAMS.surface.zw );
	if( ( a.x | a.y ) != 0u )
		atomicAdd( JahTierCounters( a ).c[2], 1u );
}
/// THE NaN DOOR (rayquery.nanRay, a TEST door): invocation 0 of the job traces its rays from a NaN origin (by its bits) -
/// the guard below must refuse them; never an unguarded NaN ray (undefined in the API)
vec3 jahRqDoorOrigin( vec3 o )
{
	return ( jahRqLoopWord() & ( 1u << 26u ) ) != 0u && gl_GlobalInvocationID == uvec3( 0u )
			   ? vec3( uintBitsToFloat( 0x7FC00000u ) ) : o;
}
#else
void jahPanesOverflowCount() {}
uint jahRqStepBoundNow() { return kJahRqStepBound; }
void jahRqStepsPublish( uint n, uint bound ) {}
void jahRqRefusedCount() {}
vec3 jahRqDoorOrigin( vec3 o ) { return o; }
#endif
/// THE RAY's INPUTS ARE DEFINED (GATHER-HANG-1 stage 2, the lead's calls 2-3; CODING_RULES section 2: a loop's float
/// inputs bit-tested). The API leaves a ray with a non-finite origin, direction, tMin or tMax, a negative tMin or a tMin
/// past its tMax UNDEFINED - the traversal's own loop is then not ours to bound. Every ray query starts through
/// JAH_RQ_INIT, OUTSIDE the seam (a software tracer that replaces jah_rq.glsl must not drop it): an undefined ray (tested
/// by the BITS, never x == x) initialises an EMPTY query instead - cull mask 0, from the origin along +z over [0, 0]: no
/// instance can answer it, so it is a MISS - and is COUNTED in the tier's counter [2] (the jobs with the hit params;
/// RayQueryStatus::raysRefused). A non-zero count in a healthy scene is a red UPSTREAM of the ray, never hidden here.
bool jahRqRayDefined( vec3 origin, float tMin, vec3 dir, float tMax )
{
	return finite3( origin ) && finite3( dir ) && finite1( tMin ) && finite1( tMax ) && tMin >= 0.0 && tMin <= tMax;
}
#define JAH_RQ_INIT( q, tlas, flags, mask, origin, tMin, dir, tMax )                                         \
	{                                                                                                         \
		const vec3 jahRqO_ = jahRqDoorOrigin( origin );                                                       \
		const vec3 jahRqD_ = ( dir );                                                                         \
		const float jahRqT0_ = float( tMin );                                                                 \
		const float jahRqT1_ = float( tMax );                                                                 \
		const bool jahRqOk_ = jahRqRayDefined( jahRqO_, jahRqT0_, jahRqD_, jahRqT1_ );                        \
		if( !jahRqOk_ )                                                                                       \
			jahRqRefusedCount();                                                                              \
		jahRqInit( q, tlas, flags, jahRqOk_ ? uint( mask ) : 0u, jahRqOk_ ? jahRqO_ : vec3( 0.0 ),            \
				   jahRqOk_ ? jahRqT0_ : 0.0, jahRqOk_ ? jahRqD_ : vec3( 0.0, 0.0, 1.0 ), jahRqOk_ ? jahRqT1_ : 0.0 ); \
	}
/// ...the loop's head (a trip counted; at the bound the query terminated) and its end (the trips published)
#define JAH_RQ_LOOP_BEGIN_( q, cut )                                                              \
	{                                                                                             \
		const uint jahRqBound_ = jahRqStepBoundNow();                                             \
		uint jahRqTrips_ = 0u;                                                                    \
		( cut ) = false;                                                                          \
		while( jahRqProceed( q ) )                                                                \
		{                                                                                         \
			if( ++jahRqTrips_ >= jahRqBound_ )                                                    \
			{                                                                                     \
				jahRqTerminate( q );                                                              \
				( cut ) = true;                                                                   \
				break;                                                                            \
			}
#define JAH_RQ_LOOP_END_                                                                          \
		}                                                                                         \
		jahRqStepsPublish( jahRqTrips_, jahRqBound_ );                                            \
	}

/// THE LOOP of an OPAQUE query (JAH_RQ_FLAGS_OPAQUE: the hardware commits every triangle, so it hands back no candidate)
/// - bounded like every other; `cut` a bool the caller declares (a cut query identifies nothing).
#define JAH_RQ_PROCEED_OPAQUE( q, cut )                                                           \
	JAH_RQ_LOOP_BEGIN_( q, cut )                                                                  \
	JAH_RQ_LOOP_END_

/// THE LOOP every ray query runs in place of `while( jahRqProceed( q ) ) {}`:
/// a triangle candidate (only an alpha-tested instance makes one) is confirmed
/// where its mask is solid. `cut` a bool the caller declares and reads first (a cut ray answers as occluded).
#define JAH_RQ_PROCEED( q, cut )                                                                  \
	JAH_RQ_LOOP_BEGIN_( q, cut )                                                                  \
		if( jahRqType( q, false ) == JAH_RQ_CANDIDATE_TRIANGLE &&                                        \
			jahAlphaSolid( uint( jahRqCustomIndex( q, false ) ),                                    \
						   uint( jahRqInstanceId( q, false ) ),                                     \
						   uint( jahRqGeometry( q, false ) ),                                       \
						   uint( jahRqPrimitive( q, false ) ),                                      \
						   jahRqBary( q, false ) ) )                                                \
			jahRqConfirm( q );                                                                    \
	JAH_RQ_LOOP_END_

/// THE PANES (PHOTON-7 10-4 R7; L9: a transparent surface transmits a ray, attenuated by its material). A blended
/// item's TLAS instance is FORCE_NO_OPAQUE and carries its PANE WORD in its shader-binding-table offset
/// (rq_tlas_write.comp; GpuScene.h kGpuPaneOn: T and its own light's share on 2047 steps, and whether its material
/// draws both faces). A ray never COMMITS a pane: each pane candidate is listed (its distance, its word, and the
/// surface: slot, near/far, submesh, triangle, barycentrics), and after the traversal the panes NEARER than the
/// committed opaque hit (or than the ray's end, on a miss) are folded in crossing order - the throughput the product
/// of their T, each one's own light weighted by the panes in front of it (jahPanesFold). A pane's FACING decides as
/// the raster does: the back face of a one-sided pane is culled there, so it transmits whole and adds nothing; a
/// two-sided pane takes both faces.
/// NEVER A SILENT CAP (the one read's F4: the 9th pane used to be CONFIRMED - a lamp behind a shelf of wine glasses
/// went black). A LIGHT ray needs no list: its visibility is the order-free product of every pane's T, multiplied as
/// the traversal meets them (JAH_RQ_PROCEED_TRANSMIT; an opaque commit answers 0 whatever). A READING ray keeps the
/// kJahPanesMax NEAREST panes (each one's own light needs its surface), and every pane past them is APPLIED: its T
/// multiplied into the overflow's product (overT), its distance range kept (overMin, overMax); after the traversal the
/// overflow counts when it lies in front of the ray's end - all of it in front, none of it behind; a hit among the
/// overflow's panes takes the whole product (the nearest-list's limit: a pane past the hit dims it) - and its own light
/// is left out. A ray that overflows is counted once (jahPanesOverflowCount: the tier's counter, giStatus
/// rayQuery.panesOverflow).
/// The order the hardware reports candidates in is free, so the list keeps them unsorted and the fold sorts.
const uint kJahPaneOn = 1u << 23u;
const uint kJahPaneTwoSided = 1u << 22u;
const int kJahPanesMax = 8;
struct JahPanes
{
	int n;
	float t[kJahPanesMax];
	uint word[kJahPanesMax];
	uint slotFar[kJahPanesMax];	// slot in bits 0-23, bit 31 the far copy, bits 24-30 the submesh
	uint prim[kJahPanesMax];
	uint bary[kJahPanesMax];	// packHalf2x16
	float overT;				// the overflow's product of T (1: none)
	float overMin, overMax;		// ...its panes' distance range
	float overApplied;			// what the overflow multiplies the throughput by, set by jahPanesBetween
};
float jahPaneT( uint word ) { return float( word & 2047u ) * ( 1.0 / 2047.0 ); }
float jahPaneOwn( uint word ) { return float( ( word >> 11u ) & 2047u ) * ( 1.0 / 2047.0 ); }

/// ...a pane to the overflow
void jahPanesOverflow( inout JahPanes p, float t, uint w )
{
	if( p.overMax < -1e29 )
		jahPanesOverflowCount();	// once a ray
	p.overT *= jahPaneT( w );
	p.overMin = min( p.overMin, t );
	p.overMax = max( p.overMax, t );
}

/// A pane candidate of query q (its word w nonzero): listed, skipped (the back of a one-sided pane), or - the list full
/// - the nearest kept: the farther of it and the list's farthest goes to the overflow (applied, never confirmed).
#define JAH_RQ_PANE_CANDIDATE( q, panes, w )                                                          \
	{                                                                                                 \
		if( ( ( w ) & kJahPaneTwoSided ) != 0u || jahRqFrontFace( q, false ) )                        \
		{                                                                                             \
			const float ct_ = jahRqT( q, false );                                                    \
			int k_ = -1;                                                                              \
			if( ( panes ).n < kJahPanesMax )                                                          \
				k_ = ( panes ).n++;                                                                   \
			else                                                                                      \
			{                                                                                         \
				int f_ = 0;                                                                           \
				for( int i_ = 1; i_ < kJahPanesMax; ++i_ )                                            \
					if( ( panes ).t[i_] > ( panes ).t[f_] ) f_ = i_;                                  \
				if( ct_ < ( panes ).t[f_] )                                                           \
				{                                                                                     \
					jahPanesOverflow( panes, ( panes ).t[f_], ( panes ).word[f_] );                   \
					k_ = f_;                                                                          \
				}                                                                                     \
				else                                                                                  \
					jahPanesOverflow( panes, ct_, ( w ) );                                            \
			}                                                                                         \
			if( k_ >= 0 )                                                                             \
			{                                                                                         \
				( panes ).t[k_] = ct_;                                                                \
				( panes ).word[k_] = ( w );                                                           \
				( panes ).slotFar[k_] = ( uint( jahRqCustomIndex( q, false ) ) & 0xFFFFFFu ) |                              \
					( ( uint( jahRqGeometry( q, false ) ) & 127u ) << 24u ) |                                 \
					( ( uint( jahRqInstanceId( q, false ) ) & 1u ) << 31u );                                 \
				( panes ).prim[k_] = uint( jahRqPrimitive( q, false ) );                              \
				( panes ).bary[k_] = packHalf2x16( jahRqBary( q, false ) );                              \
			}                                                                                         \
		}                                                                                             \
	}

/// THE LOOP of a LIGHT ray (a lamp's, the sun's, an area stratum's, a card's lamp and sun, the mover sky gate): every
/// pane's T multiplied as it is met into the float `T` (order-free: no list, no cap); an alpha-tested candidate
/// confirmed where its mask is solid. The caller answers 0 on an opaque commit, else T - and a CUT ray's T is 0 (blocked).
#define JAH_RQ_PROCEED_TRANSMIT( q, T )                                                               \
	{                                                                                                 \
	bool jahRqCutT_;                                                                                  \
	JAH_RQ_LOOP_BEGIN_( q, jahRqCutT_ )                                                           \
		if( jahRqType( q, false ) != JAH_RQ_CANDIDATE_TRIANGLE )                                        \
			continue;                                                                                 \
		const uint w_ = uint( jahRqPaneWord( q, false ) );                                                    \
		if( ( w_ & kJahPaneOn ) != 0u )                                                               \
		{                                                                                             \
			if( ( w_ & kJahPaneTwoSided ) != 0u || jahRqFrontFace( q, false ) )                       \
				( T ) *= jahPaneT( w_ );                                                              \
		}                                                                                             \
		else if( jahAlphaSolid( uint( jahRqCustomIndex( q, false ) ),                                 \
								uint( jahRqInstanceId( q, false ) ),                                  \
								uint( jahRqGeometry( q, false ) ),                                    \
								uint( jahRqPrimitive( q, false ) ),                                   \
								jahRqBary( q, false ) ) )                                             \
			jahRqConfirm( q );                                                                        \
	JAH_RQ_LOOP_END_                                                                                  \
	if( jahRqCutT_ )                                                                                  \
		( T ) = 0.0;                                                                                  \
	}

/// THE LOOP of a ray that CROSSES panes (the caches' gathers, the screen gather, the reflection; R8 the light rays):
/// JAH_RQ_PROCEED's, with the pane branch first. `panes` must be a JahPanes with n = 0; `cut` a bool the caller declares
/// and reads first (a cut ray answers as occluded: its panes are not read).
#define JAH_RQ_PROCEED_PANES( q, panes, cut )                                                         \
	JAH_RQ_LOOP_BEGIN_( q, cut )                                                                  \
		if( jahRqType( q, false ) != JAH_RQ_CANDIDATE_TRIANGLE )                                        \
			continue;                                                                                 \
		const uint w_ = uint( jahRqPaneWord( q, false ) );                                                    \
		if( ( w_ & kJahPaneOn ) != 0u )                                                               \
			JAH_RQ_PANE_CANDIDATE( q, panes, w_ )                                                     \
		else if( jahAlphaSolid( uint( jahRqCustomIndex( q, false ) ),                                 \
								uint( jahRqInstanceId( q, false ) ),                                  \
								uint( jahRqGeometry( q, false ) ),                                    \
								uint( jahRqPrimitive( q, false ) ),                                   \
								jahRqBary( q, false ) ) )                                             \
			jahRqConfirm( q );                                                                        \
	JAH_RQ_LOOP_END_

JahPanes jahPanesNone()
{
	JahPanes p;
	p.n = 0;
	p.overT = 1.0;
	p.overMin = 1e30;
	p.overMax = -1e30;
	p.overApplied = 1.0;
	return p;
}

/// THE PANES BETWEEN tStart AND tEnd (jahPanesInFront: nearer than tEnd), in crossing order (an insertion sort of the list in place; the ones past tEnd dropped):
/// returns how many there are; the throughput past them all is jahPanesThroughput.
int jahPanesBetween( inout JahPanes p, float tStart, float tEnd )
{
	int m = 0;
	for( int i = 0; i < p.n; ++i )
	{
		if( !( p.t[i] < tEnd ) || p.t[i] < tStart )
			continue;
		const float t = p.t[i];
		const uint w = p.word[i], sf = p.slotFar[i], pr = p.prim[i], b = p.bary[i];
		int j = m - 1;
		while( j >= 0 && p.t[j] > t )
		{
			p.t[j + 1] = p.t[j];
			p.word[j + 1] = p.word[j];
			p.slotFar[j + 1] = p.slotFar[j];
			p.prim[j + 1] = p.prim[j];
			p.bary[j + 1] = p.bary[j];
			--j;
		}
		p.t[j + 1] = t;
		p.word[j + 1] = w;
		p.slotFar[j + 1] = sf;
		p.prim[j + 1] = pr;
		p.bary[j + 1] = b;
		++m;
	}
	p.n = m;
	// THE OVERFLOW (the head): wholly in front of tEnd and past tStart - all of it; wholly outside - none; a range an end
	// falls in - all of it (the nearest list's limit)
	p.overApplied = ( p.overMax < tStart || !( p.overMin < tEnd ) ) ? 1.0 : p.overT;
	return m;
}
int jahPanesInFront( inout JahPanes p, float tEnd ) { return jahPanesBetween( p, -1e30, tEnd ); }
/// A light segment's transmittance from a reading list (the sun ray's fade segment, whose opaque end is known only after
/// the traversal): the listed panes nearer than tEnd, the overflow when it starts nearer
float jahPanesTransmit( JahPanes p, float tEnd )
{
	float T = p.overMin < tEnd ? p.overT : 1.0;
	for( int i = 0; i < p.n; ++i )
		if( p.t[i] < tEnd )
			T *= jahPaneT( p.word[i] );
	return T;
}
float jahPanesThroughput( JahPanes p )
{
	float T = p.overApplied;
	for( int i = 0; i < p.n; ++i )
		T *= jahPaneT( p.word[i] );
	return T;
}

#endif   // JAH_RQ_ALPHA_GLSL
