// Jahshaka — HZB seed (hierarchical depth pyramid, mip 0).
//
// Copies the scene depth buffer into mip 0 of the R32F pyramid, 1:1. It exists
// as its own pass because mip 0 is the only level whose SOURCE is a depth
// attachment rather than the level above it. (There used to be a second job
// here, `Jahshaka/HzbSeedMsaa`, reading sample 0 of a multisample depth image;
// the chain renders at 1x by construction and it was unreachable —
// CHAIN-MSAA-CRUD, 2026-09-18.)
//
// An out-of-range invocation needs no guard: imageStore outside the image is
// discarded by the Vulkan specification, and the thread groups are ceil(size/8).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

@property( syntax == glsl )
	#define ogre_t0 binding = 0
	#define ogre_u0 binding = 0
@end

vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D dstMip;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	ivec2 uv = ivec2( gl_GlobalInvocationID.xy );
	float d = texelFetch( depthTexture, uv, 0 ).x;
	OGRE_imageWrite2D1( dstMip, uv, d );
}
