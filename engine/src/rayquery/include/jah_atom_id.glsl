// THE ID IMAGE'S TWO WORDS — the one decode (ATOM-CLUSTER-CUT; SPECS/v2/
// CLUSTER_CUT_DESIGN.md D3). Written by the id pass (src/atomdraw/atom_id.frag,
// through GL_GOOGLE_include_directive) and read by the screen decode
// (800.Atom_piece_ps.any, which inserts the piece JahAtomId the build wraps this
// file into); the Atom view's low-level shader (JahAtomView_ps.glsl) spells the same
// three shifts and names this file. HlmsAtom.h's AtomId is the C++ statement.
//
//   x = the GPU scene's ITEM SLOT (bits 0-23) | the drawn cluster's DAG DEPTH
//       (bits 24-31: 0 = a leaf, the level-0 surface; the Levels view paints it)
//   y = the CLUSTER, its index in ITS MESH's DAG (bits 8-31: 16 M clusters a mesh)
//       | the TRIANGLE inside the cluster (bits 0-7: a cluster holds at most 256;
//       the bake's leaf is 128)
//   0xFFFFFFFF in x = nothing covers the pixel.
// The decode resolves the triangle through the cluster table: the cluster's first
// index + 3 x the local triangle, in the mesh's cluster stream, read through the
// mesh's CLUSTER ROW (GpuMesh::dag.w).
//
// THE HIT LIST'S RECORDS ARE NOT THIS ENCODING: a ray hits a bottom-level structure
// built from a LEVEL, so jah_rq_hit_record.glsl keeps (slot | level, triangle of the
// level) and the decode's hit mode reads the level tables.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_ATOM_ID_GLSL
#define JAH_ATOM_ID_GLSL

#define JAH_ATOM_ID_EMPTY     0xFFFFFFFFu
#define JAH_ATOM_ID_SLOT_MASK 0x00FFFFFFu

uint jahAtomIdPackX( uint slot, uint depth )
{
	return ( slot & JAH_ATOM_ID_SLOT_MASK ) | ( min( depth, 254u ) << 24u );
}

uint jahAtomIdPackY( uint cluster, uint localTriangle )
{
	return ( cluster << 8u ) | ( localTriangle & 0xFFu );
}

// slot, depth, cluster, triangle-in-cluster.
uvec4 jahAtomIdDecode( uvec2 id )
{
	return uvec4( id.x & JAH_ATOM_ID_SLOT_MASK, id.x >> 24u, id.y >> 8u, id.y & 0xFFu );
}

/// IS THE PIXEL'S SURFACE NOT THE ONE THE ID PASS DREW? (ID-DEPTH-1) The scene's final
/// depth against the id pass's own, as view distances: nearer by more than the depth's
/// own float spread means a stock-drawn surface (a character, a front-culled twin) was
/// added in front after the id pass, and the id names what lies BEHIND it. The one test
/// the id's readers take: rq_motion.comp, rq_motion_skin.comp and rq_reflect.comp.
bool jahNotIdSurface( float sceneDist, float idDist )
{
	return sceneDist < idDist * ( 1.0 - 1e-4 );
}

#endif   // JAH_ATOM_ID_GLSL
