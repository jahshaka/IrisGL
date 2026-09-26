// Jahshaka — ATOM-CLUSTER-CUT: THE EMIT, the cull's last mode-3 job (SPECS/v2/
// CLUSTER_CUT_DESIGN.md D2 and D3). ONE WORKGROUP PER DRAWN CLUSTER (the cut job's
// records; count[8] is the dispatch, ogre-patch 0032), its 64 threads copying the
// cluster's corners from the mesh's CLUSTER STREAM into the view's compacted stream
// at the record's offset, and writing each triangle's two id words.
//
// THE STREAM HOLDS REAL VERTEX INDICES (mesh-local): the id pass binds it as its
// index buffer, so the vertex stage's gl_VertexIndex is a vertex again and the
// post-transform cache sees the reuse the identity buffer hid. The corners are read
// through the mesh's CLUSTER ROW (GpuMesh::dag.w: level 0's vertices, the stream's
// indices) with the one row decode every reader uses (JahGeomRows).
//
// THE TRIANGLE'S WORDS, two per stream triangle: [0] = the id image's y, the
// cluster's index IN ITS MESH << 8 | the triangle inside the cluster (a cluster
// holds at most 256; the bake's leaf is 128); [1] = the cluster's DAG depth (the x
// word's top byte, what the Levels view paints).
//
// No Hlms directive mark appears in any comment of this file.
@insertpiece( SetCrossPlatformSettings )

@piece( CustomGlslExtensions )
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end

@insertpiece( JahGeomRows )

struct CullParams
{
	vec4  planes[6];
	vec4  viewProjRow[4];
	vec4  eye;
	vec4  lod;
	uvec4 counts;
	uvec4 hzb;
	uvec4 cut;              // x stream capacity (indices), y record capacity
};

struct GpuMesh
{
	uvec4 counts;
	vec4  localBoundsMin;
	vec4  localBoundsMax;
	uvec4 dag;              // x first cluster, y cluster count, z first group, w the cluster row
};

struct GpuCluster
{
	uvec4 range;            // x first index (the mesh's stream), y index count, z group, w refined
	vec4  sphere;
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { CullParams params; };
layout( std430, ogre_U1 ) readonly restrict buffer recordLayout { uvec4 records[]; };
layout( std430, ogre_U2 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U3 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( std430, ogre_U4 ) readonly restrict buffer clusterLayout { GpuCluster clusters[]; };
layout( std430, ogre_U5 ) readonly restrict buffer geomLayout { GeometryRow geometryTable[]; };
layout( std430, ogre_U6 ) writeonly restrict buffer streamLayout { uint stream[]; };
layout( std430, ogre_U7 ) writeonly restrict buffer triLayout { uint triWords[]; };
layout( std430, ogre_U8 ) readonly restrict buffer cntLayout { uint counter[]; };

#define JAH_INSTANCE_LANES 10u
#define JAH_EMIT_WIDTH @value( threads_per_group_x )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	uint r = gl_WorkGroupID.x;
	if( r >= min( counter[8], params.cut.y ) )
		return;
	uvec4 rec = records[r];
	if( rec.x == 0xFFFFFFFFu )
		return;   // a skip: its instance did not fit the budget
	uint lane = gl_LocalInvocationID.x;
	uint meshIndex = floatBitsToUint( instanceWords[rec.x * JAH_INSTANCE_LANES + 6u].w );
	uvec4 dag = meshes[meshIndex].dag;
	uvec4 range = clusters[rec.y].range;
	uint dst = rec.z;
	uint n = range.y;
	// The run was reserved inside the capacity by the cut job; this is the second
	// lock on that door (an index past the stream is a device fault, not a message).
	if( dst + n > params.cut.x )
		return;
	GeometryRow row = geometryTable[dag.w];
	bool readable = ( row.addresses.x | row.addresses.y ) != 0u && ( row.addresses.z | row.addresses.w ) != 0u;
	for( uint k = lane; k < n; k += JAH_EMIT_WIDTH )
		stream[dst + k] = readable ? geomIndex( row, range.x + k ) : 0u;
	uint local = rec.y - dag.x;
	uint firstTri = dst / 3u;
	for( uint t = lane; t < n / 3u; t += JAH_EMIT_WIDTH )
	{
		triWords[( firstTri + t ) * 2u] = ( local << 8u ) | t;
		triWords[( firstTri + t ) * 2u + 1u] = rec.w;
	}
}
