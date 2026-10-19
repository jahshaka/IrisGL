// Jahshaka - THE SUN MAP'S RECEIVERS (ENGINE-UPGRADE-U5 stage 4b, the lead's decisions 15 and 17). A caster changes
// the picture only where its shadow can fall on a receiver the camera sees. Per sun map, a grid of light-space
// TILES (tileTexels x tileTexels texels of the map) holds the FARTHEST receiver depth from the light (reverse-Z:
// the SMALLEST value; empty = all bits set), which the caster cut's receiver test reads (JahCullCut_cs,
// cut_receivers):
//   mode 0, a thread per pixel of the view's id depth of this frame (every surface the Atom decode shades):
//     the pixel unprojected to the world with the inverse of the matrix it was drawn with, projected into the map;
//   mode 1, a workgroup per GPU scene instance NOT drawn by the id pass (the stock (PBS) route: blended,
//     a cut-out still masking, a helper, a skinned item, one still loading, one with no mesh row - hidden ones too,
//     a superset; an Atom cut-out is in the id depth, holed where its mask is)
//     in the camera's frustum: its pixels are not in that depth when the maps render, so its whole box is taken as
//     receiver. A SKINNED item's box is its mesh's bind pose moved with the node, which a pose can leave (a limb
//     swung out): its receiver is the SPHERE of twice the box's half-diagonal about its centre (decision 19.5).
//     The id pass draws no skinned item (AtomDrawStatus 'skinned': the stock route), so every one is here.
// Nothing else samples a sun map: the particles and billboards are unlit, a decal is shaded on the surface it
// lies on, a ray hit takes the sun from its own shadow ray. The receivers' depth is moved toward the far side by
// `misc.z` (the normal offset's reach along the light) before it is kept.
@insertpiece( SetCrossPlatformSettings )

#define JAH_FLAG_SKINNED 0x20u   // GpuScene.h kGpuSkinned

struct RecvParams
{
	vec4  invViewProjRow[4];  // the view's draw matrix, inverted: rows
	vec4  lightRow[4];        // the map's cull matrix (y row negated): rows
	vec4  planes[6];          // the camera's frustum, inward
	uvec4 dims;               // x depth width, y depth height, z tiles x, w tiles y
	uvec4 map;                // x map width, y map height, z tile texels
	vec4  misc;               // x the depth's clear value, y the dilation in texels (x), z the depth margin, w the dilation (y)
	uvec4 counts;             // x instances, y flags required, z flags forbidden
	uvec4 rect;               // the id depth's viewport: x0, y0, width, height
};

struct GpuInstance
{
	vec4  world[3];
	vec4  prevWorld[3];
	vec4  boundsMin;
	vec4  boundsMax;
	uvec4 ids;
	uvec4 raster;
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { RecvParams params; };
@property( jah_receiver_mode == 2 )
// the level this dispatch reduces (a parameter, one compiled shader for every level - decision 19.6)
vulkan( layout( ogre_P0 ) uniform Params { )
	uniform uint recvLevel;
vulkan( }; )
@end
layout( std430, ogre_U1 ) restrict buffer gridLayout { uint grid[]; };
@property( jah_receiver_mode == 1 )
layout( std430, ogre_U2 ) readonly restrict buffer instLayout { GpuInstance instances[]; };
@end
@property( jah_receiver_mode == 0 )
vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
@end
// THE GRID IS A PYRAMID in the one buffer: level 0 the tiles, level k (k >= 1) each texel the least - the farthest
// receiver - of level k - 1's 2 x 2 under it (and the third row / column of an odd level's last texel: the
// footprint never loses an edge), the levels one after the other (jahRecvLevelOffset). Mode 2 reduces level
// recvLevel from the one before; the caster cut tests at the level where its rectangle is <= 4 texels wide.
uvec2 jahRecvLevelSize( uint k )
{
	return max( uvec2( params.dims.z, params.dims.w ) >> k, uvec2( 1u ) );
}
uint jahRecvLevelOffset( uint k )
{
	uint o = 0u;
	for( uint j = 0u; j < k; ++j )
	{
		uvec2 sz = jahRecvLevelSize( j );
		o += sz.x * sz.y;
	}
	return o;
}

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vec3 jahLight( vec3 w )
{
	vec4 h = vec4( w, 1.0 );
	return vec3( dot( params.lightRow[0], h ), dot( params.lightRow[1], h ), dot( params.lightRow[2], h ) );
}

// The light-space point's texel (the cull's convention: x right, rows down from the map's top).
vec2 jahTexel( vec3 q )
{
	return vec2( ( q.x * 0.5 + 0.5 ) * float( params.map.x ), ( 0.5 - q.y * 0.5 ) * float( params.map.y ) );
}

void jahKeep( ivec2 tile, float depth )
{
	if( tile.x < 0 || tile.y < 0 || tile.x >= int( params.dims.z ) || tile.y >= int( params.dims.w ) )
		return;
	float d = clamp( depth - params.misc.z, 0.0, 1.0 );
	uint at = uint( tile.y ) * params.dims.z + uint( tile.x );
	uint bits = floatBitsToUint( d );
	// READ FIRST: thousands of the camera's pixels fall in one tile, and only the ones farther than what it holds
	// need the atomic (the min is the same either way).
	if( grid[at] > bits )
		atomicMin( grid[at], bits );
}


void main()
{
@property( jah_receiver_mode == 0 )
	uvec2 px = gl_GlobalInvocationID.xy;
	if( px.x >= params.rect.z || px.y >= params.rect.w )
		return;
	ivec2 at = ivec2( px + params.rect.xy );
	float d = texelFetch( depthTexture, at, 0 ).x;
	if( d == params.misc.x )
		return;   // nothing drawn here
	vec4 ndc = vec4( ( float( px.x ) + 0.5 ) / float( params.rect.z ) * 2.0 - 1.0,
					 ( float( px.y ) + 0.5 ) / float( params.rect.w ) * 2.0 - 1.0, d, 1.0 );
	vec4 w = vec4( dot( params.invViewProjRow[0], ndc ), dot( params.invViewProjRow[1], ndc ),
				   dot( params.invViewProjRow[2], ndc ), dot( params.invViewProjRow[3], ndc ) );
	if( !( abs( w.w ) > 0.0 ) )
		return;
	vec3 q = jahLight( w.xyz / w.w );
	vec2 t = jahTexel( q );
	if( t.x < 0.0 || t.y < 0.0 || t.x >= float( params.map.x ) || t.y >= float( params.map.y ) )
		return;
	jahKeep( ivec2( floor( t / float( params.map.z ) ) ), q.z );
@end
@property( jah_receiver_mode == 1 )
	// ONE WORKGROUP PER INSTANCE, its lanes striding the box's tiles (a ground plane covers the whole map); the
	// dispatch is capped at 65,535 workgroups (the device's x limit) and a workgroup strides the slots past it.
	uint lane = gl_LocalInvocationIndex;
	for( uint slot = gl_WorkGroupID.x; slot < params.counts.x; slot += gl_NumWorkGroups.x )
	{
	vec4 bMin = instances[slot].boundsMin;
	vec4 bMax = instances[slot].boundsMax;
	uint flags = floatBitsToUint( bMax.w );
	if( ( flags & params.counts.y ) != params.counts.y || ( flags & params.counts.z ) != 0u )
		continue;
	if( ( flags & JAH_FLAG_SKINNED ) != 0u )
	{
		// a pose may leave the bind pose's box: the sphere of twice its half-diagonal, as a box
		vec3 c = ( bMin.xyz + bMax.xyz ) * 0.5;
		float r = 2.0 * length( bMax.xyz - bMin.xyz ) * 0.5;
		bMin.xyz = c - vec3( r );
		bMax.xyz = c + vec3( r );
	}
	bool inside = true;
	for( int i = 0; i < 6; ++i )
	{
		vec4 pl = params.planes[i];
		vec3 pv = vec3( pl.x >= 0.0 ? bMax.x : bMin.x, pl.y >= 0.0 ? bMax.y : bMin.y, pl.z >= 0.0 ? bMax.z : bMin.z );
		if( dot( pl.xyz, pv ) + pl.w < 0.0 )
			inside = false;
	}
	if( !inside )
		continue;
	vec2 lo = vec2( 1.0e30 ), hi = vec2( -1.0e30 );
	float farthest = 1.0e30;
	for( int c = 0; c < 8; ++c )
	{
		vec3 p = vec3( ( c & 1 ) != 0 ? bMax.x : bMin.x, ( c & 2 ) != 0 ? bMax.y : bMin.y, ( c & 4 ) != 0 ? bMax.z : bMin.z );
		vec3 q = jahLight( p );
		vec2 t = jahTexel( q );
		lo = min( lo, t );
		hi = max( hi, t );
		farthest = min( farthest, q.z );
	}
	lo = max( lo, vec2( 0.0 ) );
	hi = min( hi, vec2( params.map.xy ) - vec2( 1.0 ) );
	if( lo.x > hi.x || lo.y > hi.y )
		continue;
	ivec2 a = ivec2( floor( lo / float( params.map.z ) ) );
	ivec2 b = ivec2( floor( hi / float( params.map.z ) ) );
	uint w = uint( b.x - a.x + 1 ), h = uint( b.y - a.y + 1 );
	for( uint i = lane; i < w * h; i += @value( threads_per_group_x ) )
		jahKeep( a + ivec2( int( i % w ), int( i / w ) ), farthest );
	}
@end
@property( jah_receiver_mode == 2 )
	uint k = recvLevel;
	uvec2 dst = gl_GlobalInvocationID.xy;
	uvec2 dstSize = jahRecvLevelSize( k ), srcSize = jahRecvLevelSize( k - 1u );
	if( dst.x >= dstSize.x || dst.y >= dstSize.y )
		return;
	uint srcBase = jahRecvLevelOffset( k - 1u ), dstBase = jahRecvLevelOffset( k );
	uvec2 extra = uvec2( ( ( srcSize.x & 1u ) != 0u && dst.x == dstSize.x - 1u ) ? 1u : 0u,
						 ( ( srcSize.y & 1u ) != 0u && dst.y == dstSize.y - 1u ) ? 1u : 0u );
	uint kept = 0xFFFFFFFFu;
	for( uint y = 0u; y <= 1u + extra.y; ++y )
		for( uint x = 0u; x <= 1u + extra.x; ++x )
		{
			uvec2 s = min( dst * 2u + uvec2( x, y ), srcSize - uvec2( 1u ) );
			kept = min( kept, grid[srcBase + s.y * srcSize.x + s.x] );
		}
	grid[dstBase + dst.y * dstSize.x + dst.x] = kept;
@end
}
