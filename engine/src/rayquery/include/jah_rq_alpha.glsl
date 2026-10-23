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

/// The ray flags a query asks with: opaque (the pre-lane fast path, every
/// candidate committed by the hardware) where the scene has no alpha table,
/// none where it has one (only a FORCE_NO_OPAQUE instance then makes candidates).
uint jahAlphaRayFlags( uint extra )
{
	const uvec2 t = JAH_ALPHA_TABLE;
	return ( ( t.x | t.y ) == 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT ) | extra;
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
/// where its mask is solid. JAH_RQ_COUNT counts the query and each candidate - the
/// work a host that defines it measures (the reflection's rest budget); nothing
/// elsewhere.
#ifndef JAH_RQ_COUNT
#define JAH_RQ_COUNT
#endif
#define JAH_RQ_PROCEED( q )                                                                       \
	JAH_RQ_COUNT;                                                                                 \
	while( rayQueryProceedEXT( q ) )                                                              \
	{                                                                                             \
		JAH_RQ_COUNT;                                                                             \
		if( rayQueryGetIntersectionTypeEXT( q, false ) == gl_RayQueryCandidateIntersectionTriangleEXT && \
			jahAlphaSolid( uint( rayQueryGetIntersectionInstanceCustomIndexEXT( q, false ) ),       \
						   uint( rayQueryGetIntersectionInstanceIdEXT( q, false ) ),                \
						   uint( rayQueryGetIntersectionGeometryIndexEXT( q, false ) ),             \
						   uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) ),            \
						   rayQueryGetIntersectionBarycentricsEXT( q, false ) ) )                   \
			rayQueryConfirmIntersectionEXT( q );                                                  \
	}

#endif   // JAH_RQ_ALPHA_GLSL
