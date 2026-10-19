// Jahshaka - A CUT-OUT'S MASK (REFLECT-MOVERS-2; the maker moved from the ray tier to the engine's one store by
// ENGINE-POLISH-ATOM-1 stage 3, AlphaMasks.h). One thread per 32 texels: the material's albedo texture at one level
// (the first no larger than 1024 on a side), its ALPHA put through the datablock's own alpha test, one bit a texel,
// row-major, into the mask's buffer. The id pass and the caster cut discard at it (atom_id_masked.frag,
// atom_caster_masked.frag), the ray queries confirm a candidate at it (jah_rq_alpha.glsl); all three read it through
// its device address (jah_alpha_mask.glsl). The test is the raster's, word for word (Hlms/Pbs 800.PixelShader_piece_ps:
// discard when threshold CMP alpha, with CMP the datablock's compare function): a texel is SOLID when the raster
// would keep it. A pooled texture is a 2D array: its slice is the job's parameter.
@insertpiece( SetCrossPlatformSettings )

@property( texture0_texture_type == TextureTypes_Type2DArray )
	vulkan_layout( ogre_t0 ) uniform texture2DArray maskSource;
	#define jahMaskFetch( p, lvl ) texelFetch( maskSource, ivec3( p, int( alphaMaskB.z ) ), lvl )
@else
	vulkan_layout( ogre_t0 ) uniform texture2D maskSource;
	#define jahMaskFetch( p, lvl ) texelFetch( maskSource, p, lvl )
@end

layout( std430, ogre_U0 ) writeonly restrict buffer maskLayout { uint maskBits[]; };

vulkan( layout( ogre_P0 ) uniform Params { )
	// x the level's width, y its height, z the level, w the Ogre::CompareFunction
	uniform uvec4 alphaMaskA;
	// x the threshold's float bits, y the words, z the slice (a 2D array), w unused
	uniform uvec4 alphaMaskB;
vulkan( }; )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// Ogre::CompareFunction: 2 LESS, 3 LESS_EQUAL, 4 EQUAL, 5 NOT_EQUAL, 6 GREATER_EQUAL, 7 GREATER. The raster DISCARDS
// when (threshold CMP alpha).
bool jahMaskKept( float alpha, float threshold, uint cmp )
{
	bool dropped = false;
	if( cmp == 2u ) dropped = threshold < alpha;
	else if( cmp == 3u ) dropped = threshold <= alpha;
	else if( cmp == 4u ) dropped = threshold == alpha;
	else if( cmp == 5u ) dropped = threshold != alpha;
	else if( cmp == 6u ) dropped = threshold >= alpha;
	else if( cmp == 7u ) dropped = threshold > alpha;
	return !dropped;
}

void main()
{
	const uint word = gl_GlobalInvocationID.x;
	if( word >= alphaMaskB.y )
		return;
	const uint w = max( alphaMaskA.x, 1u ), h = max( alphaMaskA.y, 1u );
	const float threshold = uintBitsToFloat( alphaMaskB.x );
	uint bits = 0u;
	for( uint k = 0u; k < 32u; ++k )
	{
		const uint i = word * 32u + k;
		if( i >= w * h )
			break;
		const float alpha = jahMaskFetch( ivec2( int( i % w ), int( i / w ) ), int( alphaMaskA.z ) ).a;
		if( jahMaskKept( alpha, threshold, alphaMaskA.w ) )
			bits |= 1u << k;
	}
	maskBits[word] = bits;
}
