@insertpiece( SetCrossPlatformSettings )

// PHOTON'S SCROLL SHIFT (PHOTON-7 stage 2): ONE VOLUME'S LEVEL 0 MOVED BY -s TEXELS ALONG ONE AXIS, IN PLACE.
// A cascade that scrolls by s cells keeps the voxels and the light its new box shares with the old one: at the
// commit every level-0 volume of the store and of the light is shifted where it lies (a voxel at new cell n is the
// old cell n + s), the entering slab's texels at the far end cleared, and the slab the scratch holds is copied into
// them (PhotonScrollCopy_cs.glsl). The readers keep the unshifted store until that frame.
//
// ONE THREAD PER COLUMN along the axis (for a volume that packs two halves along x - the directional direct parts -
// one per half-column: z indexes the half), walking toward the far end. For s > 0 new[n] = old[n + s] for n
// ascending from 0, then the last s texels cleared; for s < 0 the mirror, descending. The reads are batched b at a
// time (b = min(|s|, 8)): a batch reads [n + s, n + s + b) before it writes [n, n + b), and b <= |s| keeps every read
// ahead of every write - no texel is read after it was written. EXACT: a load and a store of one format is the
// identity on every code (the integer formats as integers: copy_uint).
//
// No Hlms directive mark in any comment of this file.

@property( copy_uint )
	layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
	uniform restrict uimage3D volume;
	#define JAH_TEXEL uint4
	#define JAH_CLEAR clearU
@else
	layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
	uniform restrict image3D volume;
	#define JAH_TEXEL float4
	#define JAH_CLEAR clearF
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vulkan( layout( ogre_P0 ) uniform Params { )
	// x the axis (0 x, 1 y, 2 z), y the shift in texels (an int's bits), z the column's length in texels, w where the
	// second half starts along x (a packed volume shifted along x; 0 otherwise)
	uniform uint4 shiftAxis;
	// the columns: the extents of the two other axes in increasing order (x, y) and the halves (z: 1, or 2 for a
	// packed volume shifted along x)
	uniform uint4 shiftColumns;
	uniform float4 clearF;
	uniform uint4 clearU;
vulkan( }; )

void main()
{
	const uint axis = shiftAxis.x;
	const int s = int( shiftAxis.y );
	const int len = int( shiftAxis.z );
	const uint3 id = gl_GlobalInvocationID.xyz;
	if( id.x >= shiftColumns.x || id.y >= shiftColumns.y || id.z >= shiftColumns.z )
		return;
	int3 p0 = axis == 0u ? int3( 0, int( id.x ), int( id.y ) )
						 : ( axis == 1u ? int3( int( id.x ), 0, int( id.y ) ) : int3( int( id.x ), int( id.y ), 0 ) );
	if( axis == 0u )
		p0.x = int( id.z * shiftAxis.w );
	const int3 d = axis == 0u ? int3( 1, 0, 0 ) : ( axis == 1u ? int3( 0, 1, 0 ) : int3( 0, 0, 1 ) );
	const int m = s > 0 ? s : -s;
	const int b = min( m, 8 );
	const JAH_TEXEL clearValue = JAH_TEXEL( JAH_CLEAR );
	JAH_TEXEL buf[8];
	if( s > 0 )
	{
		for( int n = 0; n < len - m; n += b )
		{
			const int cnt = min( b, len - m - n );
			for( int k = 0; k < 8; ++k )
				if( k < cnt )
					buf[k] = imageLoad( volume, p0 + d * ( n + k + m ) );
			for( int k = 0; k < 8; ++k )
				if( k < cnt )
					imageStore( volume, p0 + d * ( n + k ), buf[k] );
		}
		for( int n = max( len - m, 0 ); n < len; ++n )
			imageStore( volume, p0 + d * n, clearValue );
	}
	else if( s < 0 )
	{
		for( int n = len - 1; n >= m; n -= b )
		{
			const int cnt = min( b, n - m + 1 );
			for( int k = 0; k < 8; ++k )
				if( k < cnt )
					buf[k] = imageLoad( volume, p0 + d * ( n - k - m ) );
			for( int k = 0; k < 8; ++k )
				if( k < cnt )
					imageStore( volume, p0 + d * ( n - k ), buf[k] );
		}
		for( int n = 0; n < min( m, len ); ++n )
			imageStore( volume, p0 + d * n, clearValue );
	}
}
