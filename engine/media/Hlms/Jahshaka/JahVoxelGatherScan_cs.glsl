// Jahshaka - ATOM P4b's VOXEL GATHER, job 2 of 3 (GpuVoxelGather.h): the counts
// become ranges. ONE THREAD, SERIAL, and deliberately so: there are only octants
// times material pools of them (one to a few dozen), a serial walk is
// deterministic, and it clamps every range to the record capacity so neither the
// write pass nor the voxelize shader can reach past the buffer.
@insertpiece( SetCrossPlatformSettings )

struct VoxelGatherParams
{
	uvec4 counts;
	uvec4 caps;         // x record capacity, y range count
	vec4  lod;
	vec4  octMin[8];
	vec4  octMax[8];
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { VoxelGatherParams params; };
layout( std430, ogre_U1 ) restrict buffer workLayout { uint work[]; };
layout( std430, ogre_U2 ) writeonly restrict buffer rangesLayout { uvec2 ranges[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	if( gl_GlobalInvocationID.x != 0u )
		return;
	uint rangeCount = params.caps.y;
	uint capacity = params.caps.x;
	uint start = 0u;
	for( uint p = 0u; p < rangeCount; ++p )
	{
		uint wanted = work[p];
		uint take = start < capacity ? min( wanted, capacity - start ) : 0u;
		ranges[p] = uvec2( start, take );
		start += take;
		work[rangeCount + p] = 0u;          // the write pass's cursors start at zero
	}
}
