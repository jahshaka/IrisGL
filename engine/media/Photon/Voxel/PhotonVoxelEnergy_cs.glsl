@insertpiece( SetCrossPlatformSettings )

// PHOTON'S BOUNCE ENERGY (PHOTON-5 stage 5): THE LIGHT VOLUME'S SUM, ONE PARTIAL PER WORKGROUP.
// The chain's multi-bounce is a Jacobi series (PhotonVoxelLighting::update: new = direct + rho G( total )), so
// the sum of the volume after pass k minus the sum after pass k - 1 is the energy that pass ADDED - its
// increment E_k - and E_2 / E_1 is the series' measured contraction, from which the next injection of this
// cascade chooses how many passes reach a residual. This job sums rgb over level 0 of the current light
// volume: each thread a 2 x 2 x 2 block, the group's 4 x 4 x 4 threads reduced in shared memory, one float per
// group written to the bound buffer range (the caller binds one range per pass and adds the partials on the
// CPU, a frame or more later - nothing waits for it).
//
// Texture slot 0 is the light volume (level 0); uav slot 0 is this pass's range of partials.

vulkan_layout( ogre_t0 ) uniform texture3D lightVolume;

layout( std430, ogre_U0 ) writeonly restrict buffer partialLayout { float partials[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

shared float groupSum[64];

void main()
{
	const ivec3 size = textureSize( lightVolume, 0 );
	const ivec3 base = ivec3( gl_GlobalInvocationID.xyz ) * 2;
	float s = 0.0;
	for( int k = 0; k < 8; ++k )
	{
		const ivec3 p = base + ivec3( k & 1, ( k >> 1 ) & 1, k >> 2 );
		if( all( lessThan( p, size ) ) )
		{
			const vec3 c = texelFetch( lightVolume, p, 0 ).xyz;
			// a non-finite texel is no energy (the bits, not x == x: CLAUDE.md trap 5)
			const uvec3 b = floatBitsToUint( c ) & 0x7F800000u;
			if( all( notEqual( b, uvec3( 0x7F800000u ) ) ) )
				s += c.x + c.y + c.z;
		}
	}
	const uint li = gl_LocalInvocationIndex;
	groupSum[li] = s;
	barrier();
	for( uint stride = 32u; stride > 0u; stride >>= 1u )
	{
		if( li < stride )
			groupSum[li] += groupSum[li + stride];
		barrier();
	}
	if( li == 0u )
	{
		const uvec3 g = gl_WorkGroupID;
		const uvec3 n = gl_NumWorkGroups;
		partials[( g.z * n.y + g.y ) * n.x + g.x] = groupSum[0];
	}
}
