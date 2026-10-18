// Jahshaka - THE SUN MAP'S RECEIVERS (ENGINE-UPGRADE-U5 stage 4b, the lead's decision 15; the counting arm
// shadow.casterReceiverProbe). A caster changes the picture only where its shadow can fall on a receiver the
// camera sees. Per sun map, a grid of light-space TILES (tileTexels x tileTexels texels of the map) holds the
// FARTHEST receiver depth from the light (reverse-Z: the SMALLEST value; empty = all bits set):
//   mode 0, a thread per pixel of the view's id depth (the Atom surfaces it sees): the pixel unprojected to
//     the world with the inverse of the matrix it was drawn with, projected into the map;
//   mode 1, a thread per GPU scene instance on the stock (PBS) path in the camera's frustum - their pixels
//     are not in that depth when the maps render: the whole box is taken as receiver (a superset);
//   mode 2, a thread per DRAWN CLUSTER RECORD of a caster cut (the map's first or late list): its bounding
//     sphere in the map, dilated by the filter's reach and the receivers' normal offset, is TESTED - wanted if
//     some tile it touches holds a receiver farther from the light than the cluster's nearest point - and
//     COUNTED (clusters and triangles, all and the ones the test would cut). Nothing is drawn differently.
// The receivers' depth is moved toward the far side by `misc.z` (the normal offset's reach along the light)
// before it is kept: the test then keeps whatever any receiver's filter could compare against.
@insertpiece( SetCrossPlatformSettings )

struct RecvParams
{
	vec4  invViewProjRow[4];  // the view's draw matrix, inverted: rows
	vec4  lightRow[4];        // the map's cull matrix (y row negated): rows
	vec4  planes[6];          // the camera's frustum, inward
	uvec4 dims;               // x depth width, y depth height, z tiles x, w tiles y
	uvec4 map;                // x map width, y map height, z tile texels, w record budget
	vec4  misc;               // x the depth's clear value, y the dilation in texels, z the depth margin
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

struct GpuCluster
{
	uint firstIndex;
	uint indexCount;
	uint group;
	uint refined;
	vec4 sphere;
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { RecvParams params; };
layout( std430, ogre_U1 ) restrict buffer gridLayout { uint grid[]; };
@property( jah_receiver_mode >= 1 )
layout( std430, ogre_U2 ) readonly restrict buffer instLayout { GpuInstance instances[]; };
@end
@property( jah_receiver_mode == 2 )
layout( std430, ogre_U3 ) readonly restrict buffer clusterLayout { GpuCluster clusters[]; };
layout( std430, ogre_U4 ) readonly restrict buffer recordLayout { uvec4 records[]; };
layout( std430, ogre_U5 ) readonly restrict buffer listCountLayout { uint listCount[]; };
layout( std430, ogre_U6 ) restrict buffer outLayout { uint outCount[]; };
@end
@property( jah_receiver_mode == 0 )
vulkan_layout( ogre_t0 ) uniform texture2D depthTexture;
@end

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
	atomicMin( grid[uint( tile.y ) * params.dims.z + uint( tile.x )], floatBitsToUint( d ) );
}

@property( jah_receiver_mode == 2 )
// WANTED: some tile under the texel rectangle [lo, hi] - dilated by the filter's reach and the normal offset
// (misc.y) - holds a receiver FARTHER from the light than `nearest` (reverse-Z: a smaller value). A
// rectangle too wide to walk is kept: the test may keep, never drop.
bool jahWanted( vec2 lo, vec2 hi, float nearest )
{
	lo = max( lo - params.misc.y, vec2( 0.0 ) );
	hi = min( hi + params.misc.y, vec2( params.map.xy ) - vec2( 1.0 ) );
	if( lo.x > hi.x || lo.y > hi.y )
		return false;
	ivec2 a = ivec2( floor( lo / float( params.map.z ) ) );
	ivec2 b = ivec2( floor( hi / float( params.map.z ) ) );
	if( ( b.x - a.x + 1 ) * ( b.y - a.y + 1 ) > 4096 )
		return true;
	for( int y = a.y; y <= b.y; ++y )
		for( int x = a.x; x <= b.x; ++x )
		{
			uint g = grid[uint( y ) * params.dims.z + uint( x )];
			if( g != 0xFFFFFFFFu && nearest > uintBitsToFloat( g ) )
				return true;
		}
	return false;
}
@end

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
	uint slot = gl_GlobalInvocationID.x;
	if( slot >= params.counts.x )
		return;
	vec4 bMin = instances[slot].boundsMin;
	vec4 bMax = instances[slot].boundsMax;
	uint flags = floatBitsToUint( bMax.w );
	if( ( flags & params.counts.y ) != params.counts.y || ( flags & params.counts.z ) != 0u )
		return;
	if( floatBitsToUint( bMin.w ) == 0xFFFFFFFFu )
		return;
	for( int i = 0; i < 6; ++i )
	{
		vec4 pl = params.planes[i];
		vec3 pv = vec3( pl.x >= 0.0 ? bMax.x : bMin.x, pl.y >= 0.0 ? bMax.y : bMin.y, pl.z >= 0.0 ? bMax.z : bMin.z );
		if( dot( pl.xyz, pv ) + pl.w < 0.0 )
			return;
	}
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
		return;
	ivec2 a = ivec2( floor( lo / float( params.map.z ) ) );
	ivec2 b = ivec2( floor( hi / float( params.map.z ) ) );
	for( int y = a.y; y <= b.y; ++y )
		for( int x = a.x; x <= b.x; ++x )
			jahKeep( ivec2( x, y ), farthest );
@end
@property( jah_receiver_mode == 2 )
	uint r = gl_GlobalInvocationID.x;
	if( r >= min( listCount[8], params.map.w ) )
		return;
	uvec4 rec = records[r];
	uint slot = rec.x;
	if( slot >= params.counts.x )
		return;   // a skipped (overflow) record
	GpuCluster cl = clusters[rec.y];
	uint tris = cl.indexCount / 3u;
	vec4 w0 = instances[slot].world[0], w1 = instances[slot].world[1], w2 = instances[slot].world[2];
	vec4 c4 = vec4( cl.sphere.xyz, 1.0 );
	vec3 centre = vec3( dot( w0, c4 ), dot( w1, c4 ), dot( w2, c4 ) );
	float scale = max( max( length( vec3( w0.x, w1.x, w2.x ) ), length( vec3( w0.y, w1.y, w2.y ) ) ),
					   length( vec3( w0.z, w1.z, w2.z ) ) );
	float radius = cl.sphere.w * scale;
	vec3 q = jahLight( centre );
	vec3 ext = radius * vec3( length( params.lightRow[0].xyz ), length( params.lightRow[1].xyz ),
							  length( params.lightRow[2].xyz ) );
	vec2 t = jahTexel( q );
	vec2 half2 = vec2( ext.x * 0.5 * float( params.map.x ), ext.y * 0.5 * float( params.map.y ) );
	// THE CLUSTER'S TEST (its sphere), and its INSTANCE's (the world box, the depth test's own grain: a
	// receiver cull per instance would cut a cluster only with its whole instance).
	bool wanted = jahWanted( t - half2, t + half2, q.z + ext.z );
	vec4 bMin = instances[slot].boundsMin;
	vec4 bMax = instances[slot].boundsMax;
	vec2 lo = vec2( 1.0e30 ), hi = vec2( -1.0e30 );
	float nearestBox = -1.0e30;
	for( int c = 0; c < 8; ++c )
	{
		vec3 p = vec3( ( c & 1 ) != 0 ? bMax.x : bMin.x, ( c & 2 ) != 0 ? bMax.y : bMin.y, ( c & 4 ) != 0 ? bMax.z : bMin.z );
		vec3 qc = jahLight( p );
		vec2 tc = jahTexel( qc );
		lo = min( lo, tc );
		hi = max( hi, tc );
		nearestBox = max( nearestBox, qc.z );
	}
	bool wantedBox = jahWanted( lo, hi, nearestBox );
	atomicAdd( outCount[0], 1u );
	atomicAdd( outCount[4], tris );
	if( !wanted )
	{
		atomicAdd( outCount[1], 1u );
		atomicAdd( outCount[5], tris );
	}
	if( !wantedBox )
	{
		atomicAdd( outCount[2], 1u );
		atomicAdd( outCount[6], tris );
	}
@end
}
