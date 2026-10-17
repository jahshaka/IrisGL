@insertpiece( SetCrossPlatformSettings )

// PHOTON'S SCROLL COPY (ENGINE-UPGRADE-U5 stage 1): ONE BOX OF ONE VOLUME'S LEVEL 0, COPIED TO
// ANOTHER VOLUME AT AN OFFSET - texel for texel, on compute (an image copy is a transfer command,
// which the GI region may not record: gi.region_compute_only). A cascade that scrolls by k cells
// keeps the voxels it shares with its new box: they are copied, shifted, into the scroll's target,
// and the target is copied back into the live volumes when the entering slab is built.
//
// THE COPY IS EXACT. A load from a normalised or a half-float format and a store back into the
// SAME format is the identity on every code (c / (2^b - 1) times 2^b - 1 rounds back to c); the
// integer formats are copied as integers (copy_uint). The store qualifier is the bound volume's
// own (uav0_pf_type, uav1_pf_type): source and destination always share a format.

@property( copy_uint )
	layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
	uniform restrict readonly uimage3D srcVolume;
	layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
	uniform restrict writeonly uimage3D dstVolume;
@else
	layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
	uniform restrict readonly image3D srcVolume;
	layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
	uniform restrict writeonly image3D dstVolume;
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

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
	imageStore( dstVolume, int3( dstOrigin + id ), imageLoad( srcVolume, int3( srcOrigin + id ) ) );
}
