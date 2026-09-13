// Jahshaka — the PRODUCER half of the indirect-dispatch proof (ogre-patch 0032,
// suite compute.indirect_dispatch). It is deliberately the smallest useful shape
// of what every Lumen-style pass does: walk a list, count what survives, and
// write the thread-group count the NEXT pass must run.
//
// ogre_U0: the input list (one uint per item; non-zero = the item survives).
// ogre_U1: the dispatch argument buffer — [0] = groupsX (counted here),
//          [1] = groupsY, [2] = groupsZ (both left at the 1 the CPU seeded),
//          [3] = a copy of the count, so a CPU readback can see what the GPU
//          decided without reading the argument itself.
@insertpiece( SetCrossPlatformSettings )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
	#define ogre_U1 binding = 1
@end

layout( std430, ogre_U0 ) readonly restrict buffer srcListLayout
{
	uint srcList[];
};

layout( std430, ogre_U1 ) restrict buffer dispatchArgsLayout
{
	uint dispatchArgs[];
};

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	if( srcList[gl_GlobalInvocationID.x] != 0u )
	{
		atomicAdd( dispatchArgs[0], 1u );
		atomicAdd( dispatchArgs[3], 1u );
	}
}
