@insertpiece( SetCrossPlatformSettings )

// PHOTON'S SCROLL COPY (ENGINE-UPGRADE-U5 stage 1; PHOTON-7 stage 2): ONE BOX OF ONE VOLUME'S LEVEL 0, COPIED TO
// ANOTHER VOLUME AT AN OFFSET - texel for texel, on compute (an image copy is a transfer command, which the GI region
// may not record: gi.region_compute_only). Two uses: a scroll's entering slab, built in the cascade's SLAB-SIZED
// scratch, copied into the live store's slab region at the commit (after the store's in-place shift,
// PhotonScrollShift_cs.glsl); and a bounce pass's region copied back into the light's total (its scratch target,
// PhotonVoxelLighting::runBounce - the texture identities never change).
//
// THE SOURCE'S LAYOUT (permAxis): the scratch lays every slab along its first axis, so a slab of axis a stores the
// destination's cell (x, y, z) at (x, y, z) for a = 0, (y, x, z) for a = 1 and (z, x, y) for a = 2 - its own axis
// first, the two others in increasing order (PhotonComponent::planScroll). 0 is the identity.
//
// THE CLEAR (PHOTON-7 stage 8A, the region rebuild): `copyClear.x` 1 writes the box with the CLEAR VALUE instead of a
// copy - `clearU` for an integer volume, `clearF` otherwise - what a whole build's clear writes (every value zero but
// the normal's 0.5 bias): the cells a region rebuild re-voxelises in place start where a whole build's start. Every
// dispatch sets it (PhotonVoxelScroll::copyBox 0, clearBox 1): a clone keeps its last parameters.
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
	uniform uint permAxis;
	uniform uint4 copyClear;
	uniform float4 clearF;
	uniform uint4 clearU;
vulkan( }; )

void main()
{
	uint3 id = gl_GlobalInvocationID.xyz;
	if( id.x >= copySize.x || id.y >= copySize.y || id.z >= copySize.z )
		return;
	if( copyClear.x != 0u )
	{
@property( copy_uint )
		imageStore( dstVolume, int3( dstOrigin + id ), clearU );
@else
		imageStore( dstVolume, int3( dstOrigin + id ), clearF );
@end
		return;
	}
	const uint3 src = permAxis == 1u ? id.yxz : ( permAxis == 2u ? id.zxy : id );
	imageStore( dstVolume, int3( dstOrigin + id ), imageLoad( srcVolume, int3( srcOrigin + src ) ) );
}
