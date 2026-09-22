// Jahshaka — ATOM P3's CULL, job 3 of 3: DRAW COMMANDS (design section 1.2).
// ONE THREAD PER SURVIVOR, and the dispatch itself is sized by the GPU: job 2
// wrote this job's thread-group count into the same buffer that carries the
// survivor count, and ogre-patch 0032's `_dispatchIndirect` reads it. From job
// 1's dispatch onwards no host ever learns how much work there is.
//
// WHAT IT WRITES is exactly a VkDrawIndexedIndirectCommand, five uints:
//   indexCount, instanceCount = 1, firstIndex, vertexOffset, firstInstance
// with the index range taken from the (mesh, level) table and `firstInstance`
// set to the INSTANCE SLOT, which is how the vertex shader of a draw that uses
// these commands finds its own row of the GPU scene.
//
// TWO FIELDS ARE HONESTLY ZERO AT THIS PHASE, and both are the table's doing
// rather than this job's (reported by ATOM-SUBSTRATE-1, 2026-09-22):
//   * vertexOffset — the level's first vertex in a POOLED vertex buffer. The
//     mesh table carries device ADDRESSES for that pool and nothing writes them
//     until Atom P4, so 0 is right for a mesh drawn from its own buffers and is
//     the only answer available for one that is not.
//   * the SUBMESH dimension — `GpuMeshLevel` holds submesh 0's ranges only
//     (GpuScene.h says so), so a mesh with several submeshes needs one command
//     per submesh and this table cannot say where they are. The engine counts
//     such instances and the request reports the number rather than writing a
//     command that would draw the wrong triangles.
@insertpiece( SetCrossPlatformSettings )

struct GpuMeshLevel
{
	uint  firstIndex;
	uint  indexCount;
	float bound;
	uint  reserved;
};

layout( std430, ogre_U0 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U1 ) readonly restrict buffer levelLayout { GpuMeshLevel levels[]; };
layout( std430, ogre_U2 ) readonly restrict buffer survLayout { uint survivors[]; };
layout( std430, ogre_U3 ) readonly restrict buffer lvlLayout { uint outLevel[]; };
layout( std430, ogre_U4 ) writeonly restrict buffer drawLayout { uint draws[]; };
layout( std430, ogre_U5 ) readonly restrict buffer cntLayout { uint counter[]; };

// The instance table is read as raw vec4 LANES here: this job wants one number
// out of an entry (the mesh index, bit-cast into boundsMin.w = lane 6's w) and
// declaring the whole 160-byte struct again to reach it would be a second copy
// of a contract to keep in step. 160 bytes = 10 lanes; boundsMin is lane 6.
#define JAH_INSTANCE_LANES 10u
#define JAH_LANE_BOUNDS_MIN 6u
#define JAH_LEVELS_PER_MESH 8u

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	uint i = gl_GlobalInvocationID.x;
	if( i >= counter[0] )
		return;
	uint slot = survivors[i];
	uint meshIndex =
		floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + JAH_LANE_BOUNDS_MIN].w );
	uint level = outLevel[slot];
	GpuMeshLevel row = levels[meshIndex * JAH_LEVELS_PER_MESH + level];

	uint o = i * 5u;
	draws[o + 0u] = row.indexCount;
	draws[o + 1u] = 1u;
	draws[o + 2u] = row.firstIndex;
	draws[o + 3u] = 0u;      // vertexOffset — see the header
	draws[o + 4u] = slot;    // firstInstance: the row of the table this draw is
}
