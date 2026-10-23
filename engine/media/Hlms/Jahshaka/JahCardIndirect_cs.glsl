// Jahshaka/CardAccumulate and Jahshaka/CardSensitivity (PHOTON-7 stage 5, 5-1) - A CARD'S INDIRECT IS STALE WHEN THE
// LIGHT IT GATHERS CHANGED BY A CODE SINCE ITS LAST MARCH, the change accumulated per card in absolute terms (the
// voxel feedback's resume rule, PhotonBrickFeedback_cs.glsl's ACCUM, with the card as the receiver).
//
// THE STATE, 4 words per atlas SLOT (the card's corner in 16-texel cells of the atlas, y x 128 + x: a card keeps its
// slot while it is resident, and a new card in a freed slot is captured, so marched, so its state is reset):
//   [0] the card's SENSITIVITY: codes of its radiance per unit of gathered radiance, float bits - 1 / its darkest lit
//       texel's code (a code is 1 % of the texel's radiance over the store's floor, Types.h kGiBrickFloor), the
//       re-emission bounded by one (a surface re-emits at most what it gathers);
//   [1] the would-move ACCUMULATED since its last march, fixed point (65536 units a code, saturating);
//   [2] the fraction of a unit the last accumulation's rounding left (float bits; carried: unbiased);
//   [3] REPORTED: the card is listed stale and waits for its march (no second listing).
// SENSITIVITY (jah_card_sens): one workgroup a card of the relight list, after the relight - its lit texels' lowest
// radiance, and on a MARCHED relight (mode bit 1) the accumulator restarts.
// REDUCE (mode 2): per CLUSTER of 4^3 bricks of a cascade that moved this frame, its bricks' largest move - a pass's
// absolute move (state word 4) where the brick was passed this frame, an injection's (word 7) - and the box of the bricks
// that moved (a brick inside the next finer cascade's box skipped: the cones read that one there). No atomics:
// deterministic.
// ACCUMULATE (mode 0): one thread a resident card - each cluster's largest move weighted by a BOUND of the share of the
// card's gather the moved bricks can fill, min( 1, S / ( pi d^2 ) ): S the moved box's largest silhouette (a box of sides
// x, y, z shows at most xy + yz + zx), d the card box's nearest distance to it (1 for boxes that touch; occlusion
// ignored) - the moved bricks' shares are solid angles inside the box's, so their sum is bounded by it and the largest
// move times it bounds the change - times the card's sensitivity. A card at a code or more is LISTED (the report, read
// back by the host, which relights its indirect).
// CLEAR (mode 1): the report's count, once a frame before the first accumulation.
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )

@property( jah_card_sens )
	vulkan_layout( ogre_t0 ) uniform texture2D cardRadiance;
	vulkan_layout( ogre_t1 ) uniform texture2D cardDepth;
	// THE RELIGHT LIST (OgreSurfaceCache.cpp relightCards): nine vec4 a card, the first (atlasX, atlasY, size, mode)
	layout( std430, ogre_U0 ) restrict readonly buffer csRelightLayout { vec4 csRelights[]; };
	layout( std430, ogre_U1 ) restrict coherent buffer csSlotLayout { uint csSlot[]; };
@else
	layout( std430, ogre_U0 ) restrict readonly buffer caStateLayout { uint caState[]; };
	// THE RESIDENT CARDS: two vec4 a card - ( centre.xyz, the slot as uint bits ), ( half extents.xyz, 0 )
	layout( std430, ogre_U1 ) restrict readonly buffer caCardLayout { vec4 caCards[]; };
	layout( std430, ogre_U2 ) restrict coherent buffer caSlotLayout { uint caSlot[]; };
	layout( std430, ogre_U3 ) restrict coherent buffer caReportLayout { uint caReport[]; };
	// THE CLUSTERS, the reduce's output: two words each - the largest move (float bits) and the moved bricks' box inside
	// the cluster (lo xyz, hi xyz: 3 bits each, bricks, hi exclusive)
	layout( std430, ogre_U4 ) restrict coherent buffer caClusterLayout { uint caCluster[]; };
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x: the mode (0 accumulate, 1 clear, 2 reduce); y: the cards (accumulate) or the relight entries (sensitivity); z:
	// the report's capacity
	uniform uint4 caMode;
	// the cascade's box corner (world, xyz) and brick side (metres, w)
	uniform float4 caSelf;
	// its bricks per axis (xyz) and their count (w)
	uniform uint4 caDims;
	// the next finer cascade's box (world, xyz; w = 1 when there is one)
	uniform float4 caInnerLo;
	uniform float4 caInnerHi;
vulkan( }; )

#define CA_PI 3.14159265358979
#define CA_ONE 65536.0
#define CA_RAN 512u

@property( jah_card_sens )
shared uint csMinBits;
@end

void main()
{
@property( jah_card_sens )
	// SENSITIVITY: the workgroup is the relight entry
	const uint e = gl_WorkGroupID.x;
	if( e >= caMode.y )
		return;
	const vec4 rect = csRelights[9u * e];
	const uint ax = uint( rect.x ), ay = uint( rect.y ), size = uint( rect.z ), mode = uint( rect.w );
	const uint lid = gl_LocalInvocationIndex;
	if( lid == 0u )
		csMinBits = 0x7F7FFFFFu;   // the largest finite float: no lit texel yet
	memoryBarrierShared();
	barrier();
	uint lowest = 0x7F7FFFFFu;
	for( uint k = lid; k < size * size; k += 64u )
	{
		const ivec2 at = ivec2( int( ax + k % size ), int( ay + k / size ) );
		if( !( texelFetch( cardDepth, at, 0 ).x > 0.0 ) )
			continue;
		const vec3 L = texelFetch( cardRadiance, at, 0 ).xyz;
		const float l = max( max( max( L.x, L.y ), L.z ), 0.0 );
		lowest = min( lowest, floatBitsToUint( l ) );   // (a non-negative float's bits order as the float does)
	}
	atomicMin( csMinBits, lowest );
	memoryBarrierShared();
	barrier();
	if( lid == 0u )
	{
		const uint s = 4u * ( ( ay >> 4u ) * 128u + ( ax >> 4u ) );
		const float code = 0.01 * max( uintBitsToFloat( csMinBits ), 5.9604645e-6 );
		csSlot[s] = csMinBits == 0x7F7FFFFFu ? 0u : floatBitsToUint( 1.0 / code );
		if( ( mode & 1u ) != 0u )
		{
			csSlot[s + 1u] = 0u;
			csSlot[s + 2u] = 0u;
			csSlot[s + 3u] = 0u;
		}
	}
@else
	if( caMode.x == 1u )
	{
		if( gl_GlobalInvocationID.x == 0u )
			caReport[0] = 0u;
		return;
	}
	// THE CLUSTERS: 4^3 bricks each, per axis ceil( bricks / 4 )
	const uvec3 cd = ( caDims.xyz + 3u ) / 4u;
	const uint clusters = cd.x * cd.y * cd.z;
	const float side = caSelf.w;
	if( caMode.x == 2u )
	{
		const uint q = gl_GlobalInvocationID.x;
		if( q >= clusters )
			return;
		const uvec3 cq = uvec3( q % cd.x, ( q / cd.x ) % cd.y, q / ( cd.x * cd.y ) );
		float most = 0.0;
		uvec3 mlo = uvec3( 4u, 4u, 4u ), mhi = uvec3( 0u, 0u, 0u );
		for( uint z = 0u; z < 4u; ++z )
			for( uint y = 0u; y < 4u; ++y )
				for( uint x = 0u; x < 4u; ++x )
				{
					const uvec3 bb = cq * 4u + uvec3( x, y, z );
					if( any( greaterThanEqual( bb, caDims.xyz ) ) )
						continue;
					const uint b = bb.x + caDims.x * ( bb.y + caDims.y * bb.z );
					const uint f = caState[8u * b];
					float m = ( f & CA_RAN ) != 0u ? uintBitsToFloat( caState[8u * b + 4u] ) : 0.0;
					m = max( m, uintBitsToFloat( caState[8u * b + 7u] ) );
					if( !( m > 0.0 ) )
						continue;
					if( caInnerLo.w > 0.5 )
					{
						const vec3 lo = caSelf.xyz + vec3( bb ) * side;
						if( all( greaterThanEqual( lo, caInnerLo.xyz ) ) && all( lessThanEqual( lo + side, caInnerHi.xyz ) ) )
							continue;
					}
					most = max( most, m );
					mlo = min( mlo, uvec3( x, y, z ) );
					mhi = max( mhi, uvec3( x, y, z ) + 1u );
				}
		caCluster[2u * q] = floatBitsToUint( most );
		caCluster[2u * q + 1u] = most > 0.0 ? ( mlo.x | ( mlo.y << 3u ) | ( mlo.z << 6u ) | ( mhi.x << 9u ) |
												( mhi.y << 12u ) | ( mhi.z << 15u ) )
											: 0u;
		return;
	}
	const uint k = gl_GlobalInvocationID.x;
	if( k >= caMode.y )
		return;
	const vec4 c0 = caCards[2u * k], c1 = caCards[2u * k + 1u];
	const vec3 centre = c0.xyz, halfExt = c1.xyz;
	const uint s = 4u * floatBitsToUint( c0.w );
	const float sens = uintBitsToFloat( caSlot[s] );
	const bool want = sens > 0.0 && caSlot[s + 3u] == 0u;
	if( !want )
		return;
	float sum = 0.0;   // codes
	for( uint q = 0u; q < clusters; ++q )
	{
		const float m = uintBitsToFloat( caCluster[2u * q] );
		if( !( m > 0.0 ) )
			continue;
		const uint bx = caCluster[2u * q + 1u];
		const uvec3 mlo = uvec3( bx & 7u, ( bx >> 3u ) & 7u, ( bx >> 6u ) & 7u );
		const uvec3 mhi = uvec3( ( bx >> 9u ) & 7u, ( bx >> 12u ) & 7u, ( bx >> 15u ) & 7u );
		const uvec3 cq = uvec3( q % cd.x, ( q / cd.x ) % cd.y, q / ( cd.x * cd.y ) );
		const vec3 lo = caSelf.xyz + vec3( cq * 4u + mlo ) * side;
		const vec3 ext = vec3( mhi - mlo ) * side;
		const float area = ext.x * ext.y + ext.y * ext.z + ext.z * ext.x;   // the moved box's largest silhouette
		const vec3 gap = max( abs( centre - ( lo + 0.5 * ext ) ) - ( halfExt + 0.5 * ext ), vec3( 0.0, 0.0, 0.0 ) );
		const float d2 = dot( gap, gap );
		sum += sens * m * ( d2 > 0.0 ? min( 1.0, area / ( CA_PI * d2 ) ) : 1.0 );
	}
	if( !( sum > 0.0 ) )
		return;
	// THE FIXED POINT, ROUNDED ONCE (PhotonBrickFeedback_cs.glsl's rule): the sum in units plus the fraction carried,
	// floored, the new fraction kept; saturating.
	uint acc = caSlot[s + 1u];
	const float units = sum * CA_ONE + uintBitsToFloat( caSlot[s + 2u] );
	if( !( units < 4294967040.0 ) )
	{
		acc = 0xFFFFFFFFu;
		caSlot[s + 2u] = 0u;
	}
	else
	{
		const float whole = floor( units );
		caSlot[s + 2u] = floatBitsToUint( units - whole );
		const uint q = uint( whole );
		acc = q > 0xFFFFFFFFu - acc ? 0xFFFFFFFFu : acc + q;
	}
	caSlot[s + 1u] = acc;
	// A CODE OR MORE: listed once
	if( acc >= uint( CA_ONE ) )
	{
		const uint at = atomicAdd( caReport[0], 1u );
		if( at < caMode.z )
		{
			caReport[1u + at] = s >> 2u;
			caSlot[s + 3u] = 1u;
		}
	}
@end
}
