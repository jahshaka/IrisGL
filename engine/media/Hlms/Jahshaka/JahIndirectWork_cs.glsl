// Jahshaka — the CONSUMER half of the indirect-dispatch proof (ogre-patch 0032,
// suite compute.indirect_dispatch). One group per surviving item; each group
// stamps its own slot with the total group count the GPU was given.
//
// Writing gl_NumWorkGroups.x rather than a constant is what makes the readback a
// two-sided assertion: the number of written slots says how many groups RAN, and
// their value says what count the dispatch was actually issued with. A CPU-sized
// dispatch of the same job must produce the identical buffer.
@insertpiece( SetCrossPlatformSettings )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
@end

layout( std430, ogre_U0 ) writeonly restrict buffer outStampsLayout
{
	uint outStamps[];
};

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	outStamps[gl_WorkGroupID.x] = gl_NumWorkGroups.x;
}
