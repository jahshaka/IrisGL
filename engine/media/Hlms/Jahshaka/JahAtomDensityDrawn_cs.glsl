// Jahshaka — ATOM-8 stage 0: THE DRAWN HALF OF THE DENSITY COUNT, what one list's cut DREW per object this frame.
//
// After the density count (JahAtomDensityCount_cs.glsl: the visible pixels per slot), once per list that drew this
// frame (the first list's, then the late list's): one thread a drawn-cluster RECORD of that list (the cut's work
// list, JahCullCut_cs.glsl pass 2: slot, the cluster's global index, its first index in the stream, its depth), the
// dispatch the list's own count[26..28] (the cut's lane 0 keeps x = ceil(records reserved / 64)). Per record, into
// the count words of its slot:
//   16 += the cluster's triangles, 17 += 1 (the triangles and clusters the cut drew for the object - both lists);
//   header 3 += the triangles (the frame's drawn total), and header 2 += them when the slot has NO visible pixel
//   (an object the picture does not show at all: the hidden share);
//   18 += 1 when the bitmap is on (the probe's frame) and the (slot, cluster) bit the count set is there - the
//   clusters of the object that show at least one pixel.
// A two-sided or masked survivor's draw command sits in another range at an atomic index (JahCullCut_cs.glsl): the
// records are the one per-slot account of what was drawn. No Hlms directive mark appears in any comment here.
@insertpiece( SetCrossPlatformSettings )

struct CullParams
{
	vec4  planes[6];
	vec4  viewProjRow[4];
	vec4  eye;
	vec4  lod;
	uvec4 counts;
	uvec4 hzb;
	uvec4 cut;              // y the record capacity
};

struct GpuMesh
{
	uvec4 counts;
	vec4  localBoundsMin;
	vec4  localBoundsMax;
	uvec4 dag;              // x first cluster
};

struct GpuCluster
{
	uvec4 range;            // y index count
	vec4  sphere;
};

layout( std430, ogre_U0 ) restrict buffer countLayout { uint counts[]; };
layout( std430, ogre_U1 ) readonly restrict buffer paramsLayout { CullParams params; };
layout( std430, ogre_U2 ) readonly restrict buffer recordLayout { uvec4 records[]; };
layout( std430, ogre_U3 ) readonly restrict buffer cntLayout { uint counter[]; };
layout( std430, ogre_U4 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U5 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( std430, ogre_U6 ) readonly restrict buffer clusterLayout { GpuCluster clusters[]; };
layout( std430, ogre_U7 ) readonly restrict buffer clusterBaseLayout { uint clusterBase[]; };
layout( std430, ogre_U8 ) readonly restrict buffer seenLayout { uint seen[]; };

#define JAH_INSTANCE_LANES 10u
#define JAH_SLOT_WORDS 20u
#define JAH_COUNT_HEADER 4u

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	uint r = gl_WorkGroupID.x * @value( threads_per_group_x )u + gl_LocalInvocationID.x;
	if( r >= min( counter[8], params.cut.y ) )
		return;
	uvec4 rec = records[r];
	uint slot = rec.x;
	if( slot == 0xFFFFFFFFu || slot >= counts[0] )
		return;   // a skip (its instance did not fit the budget), or a slot past the count's capacity
	uint tris = clusters[rec.y].range.y / 3u;
	uint base = JAH_COUNT_HEADER + slot * JAH_SLOT_WORDS;
	atomicAdd( counts[base + 16u], tris );
	atomicAdd( counts[base + 17u], 1u );
	atomicAdd( counts[3], tris );
	uint pixels = 0u;
	for( uint d = 0u; d < 16u; ++d )
		pixels += counts[base + d];
	if( pixels == 0u )
		atomicAdd( counts[2], tris );
	if( counts[1] != 0u )
	{
		uint meshIndex = floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + 6u].w );
		uint b = clusterBase[slot] + ( rec.y - meshes[meshIndex].dag.x );
		if( b < counts[1] && ( seen[b >> 5u] & ( 1u << ( b & 31u ) ) ) != 0u )
			atomicAdd( counts[base + 18u], 1u );
	}
}
