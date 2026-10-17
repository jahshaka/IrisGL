@insertpiece( SetCrossPlatformSettings )

// PHOTON'S SCROLLED SURFACE POSITION (ENGINE-UPGRADE-U5 stage 1). The surface position is stored
// ABSOLUTE in the volume's normalised coordinate - O_a x ( ( cell_a + p_a ) / res_a ), VoxelMerge_piece_cs.any,
// THE SURFACE POSITION - so a voxel that a scroll moves k cells inside its volume does not keep its
// bits: its position is re-resolved at the cell it moves to, FROM THE MERGE'S OWN INTEGER SUMS (the
// voxeliser keeps the four it resolves the position from, Voxelizer_cs.glsl voxelPositionSums), by the
// voxeliser's own function. That is what makes the scrolled store the store a whole build at the new
// place writes, bit for bit (gi.scroll_slab). The caster share rides in alpha and does not depend on
// the cell: it is copied.

layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict readonly uimage3D srcSums;
layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict readonly image3D srcPositionP;
layout( vulkan( ogre_u2 ) vk_comma @insertpiece(uav2_pf_type) )
uniform restrict readonly image3D srcPositionN;
layout( vulkan( ogre_u3 ) vk_comma @insertpiece(uav3_pf_type) )
uniform restrict writeonly image3D dstPositionP;
layout( vulkan( ogre_u4 ) vk_comma @insertpiece(uav4_pf_type) )
uniform restrict writeonly image3D dstPositionN;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( DeclVoxelMerge )

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint3 srcOrigin;
	uniform uint3 dstOrigin;
	uniform uint3 copySize;
vulkan( }; )

void main()
{
	uint3 id = gl_GlobalInvocationID.xyz;
	if( id.x >= copySize.x || id.y >= copySize.y || id.z >= copySize.z )
		return;
	uint3 src = srcOrigin + id;
	uint3 voxelCel = dstOrigin + id;
	uint sumAxisP = imageLoad( srcSums, int3( src.xy, src.z * 4u + 0u ) ).x;
	uint sumAxisN = imageLoad( srcSums, int3( src.xy, src.z * 4u + 1u ) ).x;
	uint sumPosP = imageLoad( srcSums, int3( src.xy, src.z * 4u + 2u ) ).x;
	uint sumPosN = imageLoad( srcSums, int3( src.xy, src.z * 4u + 3u ) ).x;
	// The voxeliser's own two lines (Voxelizer_piece_cs.any), at the voxel's new cell.
	const float3 invRes = float3( 1.0f, 1.0f, 1.0f ) / float3( imageSize( dstPositionP ) );
	imageStore( dstPositionP, int3( voxelCel ),
				float4( voxelMergePositionResolve( sumPosP, sumAxisP, voxelCel, invRes ),
						imageLoad( srcPositionP, int3( src ) ).w ) );
	imageStore( dstPositionN, int3( voxelCel ),
				float4( voxelMergePositionResolve( sumPosN, sumAxisN, voxelCel, invRes ),
						imageLoad( srcPositionN, int3( src ) ).w ) );
}
