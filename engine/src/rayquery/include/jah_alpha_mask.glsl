// A CUT-OUT'S MASK, READ (ENGINE-POLISH-ATOM-1 stage 3; AlphaMasks.h has the table's layout and the maker).
//
// THE SCENE'S MASK TABLE — one per scene, the GPU scene's (GpuScene::maskTable*), read through its device address —
// names, per shown alpha-tested slot and per submesh, the material's MASK (its albedo texture's alpha through the
// datablock's own alpha test, one bit a texel, Jahshaka/AlphaMask) and its base-map UV transform. Three readers:
// the id pass and the caster cut discard at it (atom_id.frag / atom_caster_masked.frag, JAH_MASKED), the ray
// queries confirm a candidate at it (jah_rq_alpha.glsl).
//
// THE PYRAMID (ATOM-3 stage 2; AlphaMasks.h, JahAlphaMaskPyramid_cs): the mask's levels one after the other, level
// 0 the texture's first level no larger than 1024 on a side, each coarser level a COVERAGE-PRESERVING 1-bit
// downsample of it (its set fraction is level 0's within one texel). A level is read NEAREST texel, wrapped. The
// Atom passes read the level their fragment's UV footprint asks for (jahMaskSolidAtFootprint: Ogre's own mip rule,
// log2 of the footprint's longer side in level-0 texels, rounded to the nearest level), so a minified cut-out keeps
// its coverage; the rays read level 0 (jahMaskSolidAt: a ray carries no footprint). The table's header word 2 caps
// the level (0 = level 0 only: the arm atom.maskPyramid 0, the silhouette rows' reference).
//
// The caller enables GL_EXT_buffer_reference and GL_EXT_buffer_reference_uvec2. No Hlms directive mark anywhere in
// this file.

#ifndef JAH_ALPHA_MASK_GLSL
#define JAH_ALPHA_MASK_GLSL

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahMaskTableRef
{
	uint u[];
};
layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahMaskBitsRef
{
	uint bits[];
};

const uint kJahMaskNone = 0xFFFFFFFFu;
const uint kJahMaskConstant = 1u;
const uint kJahMaskConstantSolid = 2u;
const uint kJahMaskPending = 4u;

/// The word where (slot, submesh)'s record starts in the table, or kJahMaskNone (no table, a slot it does not cover,
/// a submesh past the slot's).
uint jahMaskRecord( uvec2 table, uint slot, uint submesh )
{
	if( ( table.x | table.y ) == 0u )
		return kJahMaskNone;
	JahMaskTableRef T = JahMaskTableRef( table );
	if( slot >= T.u[0] )
		return kJahMaskNone;
	const uint base = T.u[4u + slot];
	if( base == kJahMaskNone || submesh >= T.u[base] )
		return kJahMaskNone;
	return base + 4u + submesh * 12u;
}

/// The record's flags (kJahMaskConstant / kJahMaskConstantSolid / kJahMaskPending).
uint jahMaskFlags( uvec2 table, uint rec )
{
	return JahMaskTableRef( table ).u[rec + 3u];
}

/// THE BASE-MAP UV TRANSFORM (OgreMaterials.cpp) of a record, applied to a coordinate (`bias` 1) or to a
/// derivative (`bias` 0): uv' = R * (uv * s) + bias.
vec2 jahMaskTransform( uvec2 table, uint rec, vec2 uv, float bias )
{
	JahMaskTableRef T = JahMaskTableRef( table );
	const vec4 sb = uintBitsToFloat( uvec4( T.u[rec + 4u], T.u[rec + 5u], T.u[rec + 6u], T.u[rec + 7u] ) );
	const vec4 m = uintBitsToFloat( uvec4( T.u[rec + 8u], T.u[rec + 9u], T.u[rec + 10u], T.u[rec + 11u] ) );
	const vec2 s = uv * sb.xy;
	return vec2( m.x * s.x + m.y * s.y, m.z * s.x + m.w * s.y ) + sb.zw * bias;
}

/// Level 0's size (the record's word 2).
uvec2 jahMaskSize0( uvec2 table, uint rec )
{
	const uint wh = JahMaskTableRef( table ).u[rec + 2u];
	return uvec2( max( wh & 0xFFFFu, 1u ), max( wh >> 16u, 1u ) );
}

/// The pyramid's level count: until both sides are 1 (AlphaMaskStore::levelCount).
uint jahMaskLevels( uvec2 s0 )
{
	return uint( findMSB( max( max( s0.x, s0.y ), 1u ) ) ) + 1u;
}

/// The bit at the TRANSFORMED coordinate `tuv`, at pyramid level `level` (clamped to the pyramid), nearest, wrapped.
bool jahMaskBit( uvec2 table, uint rec, vec2 tuv, uint level )
{
	JahMaskTableRef T = JahMaskTableRef( table );
	const uvec2 s0 = jahMaskSize0( table, rec );
	level = min( level, jahMaskLevels( s0 ) - 1u );
	uint offset = 0u;
	for( uint j = 0u; j < level; ++j )
	{
		const uvec2 sj = uvec2( max( s0.x >> j, 1u ), max( s0.y >> j, 1u ) );
		offset += ( sj.x * sj.y + 31u ) / 32u;
	}
	const uvec2 sk = uvec2( max( s0.x >> level, 1u ), max( s0.y >> level, 1u ) );
	const vec2 f = fract( tuv );
	const uint x = min( uint( f.x * float( sk.x ) ), sk.x - 1u );
	const uint y = min( uint( f.y * float( sk.y ) ), sk.y - 1u );
	const uint i = y * sk.x + x;
	JahMaskBitsRef M = JahMaskBitsRef( uvec2( T.u[rec], T.u[rec + 1u] ) );
	return ( ( M.bits[offset + ( i >> 5u )] >> ( i & 31u ) ) & 1u ) != 0u;
}

/// A constant or pending record's answer: 1 solid, 0 not, 2 = it has a mask (read it).
uint jahMaskConstantAnswer( uvec2 table, uint rec )
{
	const uint flags = JahMaskTableRef( table ).u[rec + 3u];
	if( ( flags & kJahMaskPending ) != 0u )
		return 0u;
	if( ( flags & kJahMaskConstant ) != 0u )
		return ( flags & kJahMaskConstantSolid ) != 0u ? 1u : 0u;
	return 2u;
}

/// Is the record's surface SOLID at the mesh's texture coordinate `uv` (UV0, before the material's transform), at
/// LEVEL 0 (a ray's read)? A constant record answers its constant; a pending one is NOT solid (the caller decides
/// what pending means: a ray refuses the candidate; the Atom passes never draw a pending cut-out).
bool jahMaskSolidAt( uvec2 table, uint rec, vec2 uv )
{
	const uint c = jahMaskConstantAnswer( table, rec );
	if( c != 2u )
		return c == 1u;
	return jahMaskBit( table, rec, jahMaskTransform( table, rec, uv, 1.0 ), 0u );
}

/// ...at the level a FRAGMENT's footprint asks for: `duvdx` / `duvdy` are the UV0 derivatives the caller took in
/// uniform control flow (dFdx / dFdy before any branch). The footprint's longer side in level-0 texels, rho; the
/// level is round(log2(rho)) (0 under magnification), capped by the table's header word 2.
bool jahMaskSolidAtFootprint( uvec2 table, uint rec, vec2 uv, vec2 duvdx, vec2 duvdy )
{
	const uint c = jahMaskConstantAnswer( table, rec );
	if( c != 2u )
		return c == 1u;
	const vec2 s0 = vec2( jahMaskSize0( table, rec ) );
	const vec2 dx = jahMaskTransform( table, rec, duvdx, 0.0 ) * s0;
	const vec2 dy = jahMaskTransform( table, rec, duvdy, 0.0 ) * s0;
	const float rho = max( length( dx ), length( dy ) );
	uint level = 0u;
	if( rho > 1.0 )
		level = uint( floor( log2( rho ) + 0.5 ) );
	level = min( level, JahMaskTableRef( table ).u[2] );
	return jahMaskBit( table, rec, jahMaskTransform( table, rec, uv, 1.0 ), level );
}

#endif   // JAH_ALPHA_MASK_GLSL
