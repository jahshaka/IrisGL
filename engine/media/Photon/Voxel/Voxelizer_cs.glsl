// Heavily modified version based on voxelization shader written by mattatz
//	https://github.com/mattatz/unity-voxel
// Original work Copyright (c) 2018 mattatz under MIT license.
//
// mattatz's work is a full voxelization algorithm.
// We only need to voxelize the shell (aka the contour),
// not to fill the entire voxel.
//
// Adapted for Ogre and for use for Voxel Cone Tracing by
// Matias N. Goldberg Copyright (c) 2019

@insertpiece( SetCrossPlatformSettings )

@insertpiece( DeclUavCrossPlatform )

@piece( CustomGlslExtensions )
	#extension GL_ARB_shader_group_vote: require
	// Jahshaka (ATOM P4): the geometry is read where the raster keeps it, through
	// buffer device addresses - see the JahGeomRows piece. The uvec2 form is
	// deliberate: shaderInt64 is not an enabled device feature.
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end

@property( !vendor_shader_extension )
	// We are emulating anyInvocationARB because for it to work
	// correctly we need the following guarantees:
	//	1. gl_SubGroupSizeARB is known beforehand and all lanes [0; gl_SubGroupSizeARB) are always active
	//	2. VK_EXT_subgroup_size_control to enforce 1
	//	3. REQUIRE_FULL_SUBGROUPS_BIT_EXT to ensure all lanes are occupied the way we need
	//     (perfect lockstep)
	//
	// See https://github.com/KhronosGroup/GLSL/issues/19#issuecomment-881931582
	// See https://forums.ogre3d.org/viewtopic.php?p=551000#p551000
	shared bool g_emulatedGroupVote[64];

	bool emulatedAnyInvocationARB( bool value )
	{
		g_emulatedGroupVote[gl_LocalInvocationIndex] = value;

		for( uint i=0u; i<6u; ++i )
		{
			__sharedOnlyBarrier;
			uint nextIdx = gl_LocalInvocationIndex + (1u << i);
			uint mask = (1u << (i+1u)) - 1u;
			if( ( gl_LocalInvocationIndex & mask ) == 0u )
			{
				g_emulatedGroupVote[gl_LocalInvocationIndex] =
					g_emulatedGroupVote[gl_LocalInvocationIndex] || g_emulatedGroupVote[nextIdx];
			}
		}

		__sharedOnlyBarrier;
		return g_emulatedGroupVote[0];
	}

	#define anyInvocationARB( value ) emulatedAnyInvocationARB( value )
@end

@insertpiece( PreBindingsHeaderCS )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
	#define ogre_U1 binding = 1
@end

// Jahshaka (ATOM P4): U0 IS THE GEOMETRY TABLE. It replaces the two slots that used
// to bind a private vertex copy and a private index copy - and with them the
// `compressed_vertex_format` and `index_32bit` shader properties, since a dispatch no
// longer binds a format. Every image below therefore moved down a slot (two, with U1).
layout( std430, ogre_U0 ) readonly restrict buffer geometryTableLayout
{
	GeometryRow geometryTable[];
};

// Jahshaka (ATOM P4b): THE RANGES - one (start, count) per (octant, bucket), written
// on the device by the host's gather. This dispatch loops over ranges[rangeIdx]; the
// count is only a LOOP BOUND, because the dispatch's thread count is the octant's and
// never the instance count's, so no indirect dispatch is needed to take it from the GPU.
// U1 and not after the images: the root layout packs UAV buffers and UAV images as
// two contiguous ranges.
layout( std430, ogre_U1 ) readonly restrict buffer rangesLayout
{
	uvec2 ranges[];
};

layout( vulkan( ogre_u2 ) vk_comma @insertpiece(uav2_pf_type) )
uniform restrict image3D voxelAlbedoTex;
layout( vulkan( ogre_u3 ) vk_comma @insertpiece(uav3_pf_type) )
uniform restrict image3D voxelNormalTex;
layout( vulkan( ogre_u4 ) vk_comma @insertpiece(uav4_pf_type) )
uniform restrict image3D voxelEmissiveTex;
layout( vulkan( ogre_u5 ) vk_comma @insertpiece(uav5_pf_type) )
uniform restrict uimage3D voxelAccumVal;
// Jahshaka fork ad452604a+155a56bf8+0338ca7f2+c4c80b5f7 (was 0065): the per-voxel INTEGER ACCUMULATOR the merge sums into.
layout( vulkan( ogre_u6 ) vk_comma @insertpiece(uav6_pf_type) )
uniform restrict uimage3D voxelMergeAccum;
// Jahshaka (PHOTON-VOXEL-3/-4): THE PER-HALF-AXIS COVERAGE - O_a+ (the faces looking +a)
// and O_a- (looking -a), written by the resolve (VoxelMerge_piece_cs.any, THE DIRECTIONAL
// COVERAGE); the light injection, the anisotropic mip step 0 and every read sample it.
layout( vulkan( ogre_u7 ) vk_comma @insertpiece(uav7_pf_type) )
uniform restrict writeonly image3D voxelCoveragePTex;
layout( vulkan( ogre_u8 ) vk_comma @insertpiece(uav8_pf_type) )
uniform restrict writeonly image3D voxelCoverageNTex;
// Jahshaka (PHOTON-VOXEL-4): THE SURFACE POSITION per half-axis, O-premultiplied, absolute
// in the volume's normalised coordinate (VoxelMerge_piece_cs.any, THE SURFACE POSITION).
layout( vulkan( ogre_u9 ) vk_comma @insertpiece(uav9_pf_type) )
uniform restrict writeonly image3D voxelPositionPTex;
layout( vulkan( ogre_u10 ) vk_comma @insertpiece(uav10_pf_type) )
uniform restrict writeonly image3D voxelPositionNTex;



layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

//layout (rgba8) uniform restrict writeonly image3D voxelAlbedoTex;
//layout (rgb10_a2) uniform restrict writeonly image3D voxelNormalTex;
//layout (rgba8) uniform restrict writeonly image3D voxelEmissiveTex;

//layout( local_size_x = 4,
//		local_size_y = 4,
//		local_size_z = 4 ) in;

@property( syntax == glsl )
	ReadOnlyBufferF( 7, InstanceBuffer, instanceBuffer );
@else
	ReadOnlyBufferF( 0, InstanceBuffer, instanceBuffer );
@end

@property( has_diffuse_tex || has_emissive_tex )
	vulkan_layout( ogre_t1 ) uniform texture2DArray texturePool;
	vulkan( layout( ogre_s1 ) uniform sampler poolSampler );
@end


@insertpiece( DeclVoxelMerge )

@insertpiece( HeaderCS )


vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint2 rangeIdx_pad;
	uniform float3 voxelOrigin;
	uniform float3 voxelCellSize;
	uniform uint3 voxelPixelOrigin;
vulkan( }; )

#define p_rangeIdx rangeIdx_pad.x
#define p_voxelOrigin voxelOrigin
#define p_voxelCellSize voxelCellSize
#define p_voxelPixelOrigin voxelPixelOrigin

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
