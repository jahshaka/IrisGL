@insertpiece( SetCrossPlatformSettings )

@piece( CustomGlslExtensions )
	// Jahshaka (ATOM P4): see the JahGeomRows piece (jah_geom_rows.glsl).
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end

#define __sharedOnlyBarrier memoryBarrierShared();barrier();

@insertpiece( PreBindingsHeaderCS )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
	#define ogre_U1 binding = 1
@end

// Jahshaka (ATOM P4): the geometry table where the private vertex and index copies
// used to be; the output AABBs move down one slot with them.
layout(std430, ogre_U0) readonly restrict buffer geometryTableLayout
{
	GeometryRow geometryTable[];
};
layout(std430, ogre_U1) writeonly restrict buffer outMeshAabbLayout
{
	MeshAabb outMeshAabb[];
};

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vulkan_layout( ogre_T0 ) uniform usamplerBuffer inMeshBuffer;

shared float3 g_minAabb[@value( threads_per_group_x )];
shared float3 g_maxAabb[@value( threads_per_group_x )];

@insertpiece( HeaderCS )

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint2 meshStart_meshEnd;
vulkan( }; )

#define p_meshStart meshStart_meshEnd.x
#define p_meshEnd meshStart_meshEnd.y

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
