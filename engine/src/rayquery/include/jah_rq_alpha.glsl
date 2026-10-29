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
	return gl_RayFlagsNoneEXT | extra;
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

/// THE LOOP every ray query runs in place of `while( rayQueryProceedEXT( q ) ) {}`:
/// a triangle candidate (only an alpha-tested instance makes one) is confirmed
/// where its mask is solid.
#define JAH_RQ_PROCEED( q )                                                                       \
	while( rayQueryProceedEXT( q ) )                                                              \
	{                                                                                             \
		if( rayQueryGetIntersectionTypeEXT( q, false ) == gl_RayQueryCandidateIntersectionTriangleEXT && \
			jahAlphaSolid( uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, false ) ),       \
						   uint( rayQueryGetIntersectionInstanceIdEXT( q, false ) ),                \
						   uint( rayQueryGetIntersectionGeometryIndexEXT( q, false ) ),             \
						   uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) ),            \
						   rayQueryGetIntersectionBarycentricsEXT( q, false ) ) )                   \
			rayQueryConfirmIntersectionEXT( q );                                                  \
	}

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

#if defined( JAH_HIT_PARAMS ) && defined( GL_EXT_buffer_reference )
/// THE TIER'S COUNTERS (the hit set's surface.zw: a host-visible buffer's address, OgreRayQuery.cpp mTierCounters):
/// [0] the reading rays' pane-list overflows
layout( buffer_reference, std430, buffer_reference_align = 4 ) buffer JahTierCounters { uint c[]; };
void jahPanesOverflowCount()
{
	const uvec2 a = floatBitsToUint( JAH_HIT_PARAMS.surface.zw );
	if( ( a.x | a.y ) != 0u )
		atomicAdd( JahTierCounters( a ).c[0], 1u );
}
/// MEASURING ARM (11-2, the sealed_sky teleport counter; reverted): [1] C2's secondary rays answered as the sky, [2] of
/// them the ones a ray past the world's exit (to 1e4) would have hit, [3] C2's secondary rays, [4] the probe integrate's
/// sky answers, [5] of them the ones past the exit that would have hit
void jahTierCount( uint i )
{
	const uvec2 a = floatBitsToUint( JAH_HIT_PARAMS.surface.zw );
	if( ( a.x | a.y ) != 0u )
		atomicAdd( JahTierCounters( a ).c[i], 1u );
}
#else
void jahPanesOverflowCount() {}
void jahTierCount( uint i ) {}
#endif
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
		if( ( ( w ) & kJahPaneTwoSided ) != 0u || rayQueryGetIntersectionFrontFaceEXT( q, false ) )   \
		{                                                                                             \
			const float ct_ = rayQueryGetIntersectionTEXT( q, false );                               \
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
				( panes ).slotFar[k_] = ( uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, false ) ) & 0xFFFFFFu ) | \
					( ( uint( rayQueryGetIntersectionGeometryIndexEXT( q, false ) ) & 127u ) << 24u ) |       \
					( ( uint( rayQueryGetIntersectionInstanceIdEXT( q, false ) ) & 1u ) << 31u );            \
				( panes ).prim[k_] = uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) );    \
				( panes ).bary[k_] = packHalf2x16( rayQueryGetIntersectionBarycentricsEXT( q, false ) ); \
			}                                                                                         \
		}                                                                                             \
	}

/// THE LOOP of a LIGHT ray (a lamp's, the sun's, an area stratum's, a card's lamp and sun, the mover sky gate): every
/// pane's T multiplied as it is met into the float `T` (order-free: no list, no cap); an alpha-tested candidate
/// confirmed where its mask is solid. The caller answers 0 on an opaque commit, else T.
#define JAH_RQ_PROCEED_TRANSMIT( q, T )                                                               \
	while( rayQueryProceedEXT( q ) )                                                                  \
	{                                                                                                 \
		if( rayQueryGetIntersectionTypeEXT( q, false ) != gl_RayQueryCandidateIntersectionTriangleEXT ) \
			continue;                                                                                 \
		const uint w_ = uint( rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT( q, false ) ); \
		if( ( w_ & kJahPaneOn ) != 0u )                                                               \
		{                                                                                             \
			if( ( w_ & kJahPaneTwoSided ) != 0u || rayQueryGetIntersectionFrontFaceEXT( q, false ) )  \
				( T ) *= jahPaneT( w_ );                                                              \
		}                                                                                             \
		else if( jahAlphaSolid( uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, false ) ),    \
								uint( rayQueryGetIntersectionInstanceIdEXT( q, false ) ),             \
								uint( rayQueryGetIntersectionGeometryIndexEXT( q, false ) ),          \
								uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) ),         \
								rayQueryGetIntersectionBarycentricsEXT( q, false ) ) )                \
			rayQueryConfirmIntersectionEXT( q );                                                      \
	}

/// THE LOOP of a ray that CROSSES panes (the caches' gathers, the screen gather, the reflection; R8 the light rays):
/// JAH_RQ_PROCEED's, with the pane branch first. `panes` must be a JahPanes with n = 0.
#define JAH_RQ_PROCEED_PANES( q, panes )                                                              \
	while( rayQueryProceedEXT( q ) )                                                                  \
	{                                                                                                 \
		if( rayQueryGetIntersectionTypeEXT( q, false ) != gl_RayQueryCandidateIntersectionTriangleEXT ) \
			continue;                                                                                 \
		const uint w_ = uint( rayQueryGetIntersectionInstanceShaderBindingTableRecordOffsetEXT( q, false ) ); \
		if( ( w_ & kJahPaneOn ) != 0u )                                                               \
			JAH_RQ_PANE_CANDIDATE( q, panes, w_ )                                                     \
		else if( jahAlphaSolid( uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, false ) ),    \
								uint( rayQueryGetIntersectionInstanceIdEXT( q, false ) ),             \
								uint( rayQueryGetIntersectionGeometryIndexEXT( q, false ) ),          \
								uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) ),         \
								rayQueryGetIntersectionBarycentricsEXT( q, false ) ) )                \
			rayQueryConfirmIntersectionEXT( q );                                                      \
	}

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
