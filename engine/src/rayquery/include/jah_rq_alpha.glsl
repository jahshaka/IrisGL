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
// thresholded at the cutoff, one bit a texel (RayQueryTier's mask job,
// rq_alpha_mask.comp) — and confirms the candidate only where the texel is solid.
// A ray that asked for the first hit (the sun's) stops at the first solid one.
//
// THE TABLE (one per scene, rebuilt on the host when the alpha-tested set, a
// row or a mask changes), read through its device address (the caller defines
// JAH_ALPHA_TABLE, a uvec2; zero = no table: the ray flags stay opaque and this
// loop never sees a candidate):
//   uint[0] = N, the slots the per-slot index covers; uint[1] = entries
//   uint[4 + s] = slot s's entry, 0xFFFFFFFF none (s < N); the entries start at
//   uint 4 + N rounded up to four, 36 uints (nine uvec4) each:
//     0-2  the NEAR copy's geometry row (the three lanes of jah_geom_rows.glsl)
//     3-5  the FAR copy's row (the mesh's coarsest level; a rigged item's skin row)
//     6    (mask address lo, hi, width | height << 16, flags)
//     7    the UV transform's (scale x, scale y, bias x, bias y), float bits
//     8    its rotation, row-major (m00, m01, m10, m11), float bits
//   flags: bit 0 = no mask, the constant answer is bit 1 (an alpha test with no
//   albedo map: the colour's alpha decides for every texel); bit 2 = the mask is
//   still being made (the candidate is refused: before this lane the item was
//   not traced at all, and a solid quad would be the one wrong answer).
//
// The mask is NEAREST texel at the texture's first level no larger than
// kMaxAlphaMask on a side (RayQueryTier), wrapped: the raster's alpha test reads
// the filtered alpha at the pixel's footprint, so a ray and a pixel can disagree
// within half a mask texel of an edge — the bar in gi.rt_alpha_tested is the
// coverage, never the edge.
//
// The caller enables GL_EXT_ray_query, GL_EXT_buffer_reference and
// GL_EXT_buffer_reference_uvec2 and defines JAH_ALPHA_TABLE (the geometry rows'
// decode, jah_geom_rows.glsl, is included here). No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_ALPHA_GLSL
#define JAH_RQ_ALPHA_GLSL

#include "jah_geom_rows.glsl"

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahAlphaTableRef
{
	uint u[];
};
layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahAlphaMaskRef
{
	uint bits[];
};

const uint kJahAlphaNone = 0xFFFFFFFFu;
const uint kJahAlphaConstant = 1u;
const uint kJahAlphaConstantSolid = 2u;
const uint kJahAlphaPending = 4u;

/// The ray flags a query asks with: opaque (the pre-lane fast path, every
/// candidate committed by the hardware) where the scene has no alpha table,
/// none where it has one (only a FORCE_NO_OPAQUE instance then makes candidates).
uint jahAlphaRayFlags( uint extra )
{
	const uvec2 t = JAH_ALPHA_TABLE;
	return ( ( t.x | t.y ) == 0u ? gl_RayFlagsOpaqueEXT : gl_RayFlagsNoneEXT ) | extra;
}

/// Is the candidate triangle SOLID at its barycentrics? `instanceId` is the TLAS
/// instance's index (two a slot: even the near copy, odd the far one).
bool jahAlphaSolid( uint slot, uint instanceId, uint prim, vec2 bary )
{
	const uvec2 t = JAH_ALPHA_TABLE;
	if( ( t.x | t.y ) == 0u )
		return true;
	JahAlphaTableRef T = JahAlphaTableRef( t );
	const uint n = T.u[0];
	if( slot >= n )
		return true;
	const uint e = T.u[4u + slot];
	if( e == kJahAlphaNone )
		return true;
	const uint base = 4u + ( ( n + 3u ) & ~3u ) + e * 36u;
	const uint flags = T.u[base + 27u];
	if( ( flags & kJahAlphaPending ) != 0u )
		return false;
	if( ( flags & kJahAlphaConstant ) != 0u )
		return ( flags & kJahAlphaConstantSolid ) != 0u;
	const uint r = base + ( ( instanceId & 1u ) != 0u ? 12u : 0u );
	GeometryRow g;
	g.addresses = uvec4( T.u[r], T.u[r + 1u], T.u[r + 2u], T.u[r + 3u] );
	g.layout0 = uvec4( T.u[r + 4u], T.u[r + 5u], T.u[r + 6u], T.u[r + 7u] );
	g.layout1 = uvec4( T.u[r + 8u], T.u[r + 9u], T.u[r + 10u], T.u[r + 11u] );
	if( ( g.addresses.x | g.addresses.y ) == 0u || ( g.addresses.z | g.addresses.w ) == 0u ||
		( g.layout1.x & GeomUvMask ) == GeomUvNone )
		return true;
	const vec3 b = vec3( 1.0 - bary.x - bary.y, bary.x, bary.y );
	vec2 uv = vec2( 0.0 );
	for( uint k = 0u; k < 3u; ++k )
		uv += geomUv( g, geomIndex( g, prim * 3u + k ) ) * b[k];
	// THE BASE-MAP UV TRANSFORM (OgreMaterials.cpp): uv' = R * (uv * s) + bias
	const vec4 sb = uintBitsToFloat( uvec4( T.u[base + 28u], T.u[base + 29u], T.u[base + 30u], T.u[base + 31u] ) );
	const vec4 m = uintBitsToFloat( uvec4( T.u[base + 32u], T.u[base + 33u], T.u[base + 34u], T.u[base + 35u] ) );
	const vec2 s = uv * sb.xy;
	uv = vec2( m.x * s.x + m.y * s.y, m.z * s.x + m.w * s.y ) + sb.zw;
	const uint wh = T.u[base + 26u];
	const uint w = max( wh & 0xFFFFu, 1u ), h = max( wh >> 16u, 1u );
	const vec2 f = fract( uv );
	const uint x = min( uint( f.x * float( w ) ), w - 1u );
	const uint y = min( uint( f.y * float( h ) ), h - 1u );
	const uint i = y * w + x;
	JahAlphaMaskRef M = JahAlphaMaskRef( uvec2( T.u[base + 24u], T.u[base + 25u] ) );
	return ( ( M.bits[i >> 5u] >> ( i & 31u ) ) & 1u ) != 0u;
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
						   uint( rayQueryGetIntersectionPrimitiveIndexEXT( q, false ) ),            \
						   rayQueryGetIntersectionBarycentricsEXT( q, false ) ) )                   \
			rayQueryConfirmIntersectionEXT( q );                                                  \
	}

#endif   // JAH_RQ_ALPHA_GLSL
