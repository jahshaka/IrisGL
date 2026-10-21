// Jahshaka — ATOM-8 stage 1: THE LATE CUT, the rejected clusters re-tested against THIS frame's pyramid.
//
// The first list's cut (JahCullCut_cs.glsl) appended every cluster last frame's pyramid hid to its REJECTED LIST
// (slot, global cluster, depth; a skip's slot all ones; count[38] the first list's cursor, the dispatch its
// count[20..22]: one thread an entry, 64 a group). Here, in the LATE list's request (this frame's cull matrix and
// pyramid - built from the first pass's depth and the stock depth - in params), each entry is tested again by the
// SAME box test (JahHzbTest.glsl) without the frustum (the first cut's planes were this frame's) and without the
// pixel-centre test (the first cut's used this frame's matrix too): still hidden = dropped, exactly (it lies behind
// depth of this frame's picture, count[35]); otherwise it is DRAWN LATE - the stream run it needs (the late list's counter[11], inside the
// main region; a cluster that cannot fit is counted missing, count[13], and the budget grows), ONE RECORD (counter[8],
// the emit job's work list, counter[26] the drawn reduce's dispatch) and ONE DRAW COMMAND OF ITS OWN in the late
// list's per-cluster ranges (two-sided / masked by the slot's flags, the cut's ranges' rule; counter[29 + range]),
// whose firstInstance is the ENTRY: the identity table maps it to (slot, the cluster's first stream triangle) - the
// id pass's vertex stage reads it in place of the slot base (atom_id.vert). No Hlms directive mark appears in any
// comment of this file.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( JahLevelRuleScale )

struct CullParams
{
	vec4  planes[6];
	vec4  viewProjRow[4];
	vec4  eye;
	vec4  lod;
	uvec4 counts;
	uvec4 hzb;              // x levels, y width, z height, w reverse-Z
	uvec4 cut;              // x stream capacity, y record capacity, z the main region's end
	uvec4 hzbRect;
	vec4  viewProjNowRow[4];
	uvec4 viewRect;
	uvec4 occl;             // z the rejected budget
};

struct GpuCluster
{
	uvec4 range;            // y index count
	vec4  sphere;           // mesh space
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { CullParams params; };
layout( std430, ogre_U1 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U2 ) readonly restrict buffer clusterLayout { GpuCluster clusters[]; };
layout( std430, ogre_U3 ) readonly restrict buffer firstCntLayout { uint firstCounter[]; };
layout( std430, ogre_U4 ) readonly restrict buffer rejectedLayout { uvec4 rejected[]; };
layout( std430, ogre_U5 ) restrict buffer cntLayout { uint counter[]; };
layout( std430, ogre_U6 ) writeonly restrict buffer recordLayout { uvec4 records[]; };
layout( std430, ogre_U7 ) writeonly restrict buffer drawLayout { uint clusterDraws[]; };
layout( std430, ogre_U8 ) writeonly restrict buffer idLayout { uvec2 clusterIds[]; };
layout( std430, ogre_U9 ) readonly restrict buffer boxLayout { vec4 clusterBoxes[]; };   // the clusters' own boxes
vulkan_layout( ogre_t0 ) uniform texture2D hzbTexture;
@insertpiece( JahHzbTest )

#define JAH_INSTANCE_LANES 10u
#define JAH_FLAG_TWO_SIDED 0x800u
#define JAH_FLAG_ALPHA_TESTED 0x10u

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	uint e = gl_GlobalInvocationID.x;
	uint budget = params.occl.z;
	if( e >= min( firstCounter[38], budget ) )   // the rejected list's cursor
		return;
	uvec4 entry = rejected[e];
	uint slot = entry.x;
	if( slot == 0xFFFFFFFFu )
		return;
	vec4 r0 = instanceWords[slot * JAH_INSTANCE_LANES + 0u];
	vec4 r1 = instanceWords[slot * JAH_INSTANCE_LANES + 1u];
	vec4 r2 = instanceWords[slot * JAH_INSTANCE_LANES + 2u];
	float scale = jahWorldMaxAxisScale( r0, r1, r2 );
	uvec4 range = clusters[entry.y].range;
	vec4 sphere = clusters[entry.y].sphere;
	vec3 c, ax, ay, az;
	jahClusterObb( clusterBoxes[entry.y], sphere, r0, r1, r2, scale, c, ax, ay, az );
	bool reverseZ = params.hzb.w != 0u;
	vec2 lo, hi;
	float nearest;
	bool testable = jahHzbObbRect( c, ax, ay, az, params.viewProjRow[0], params.viewProjRow[1], params.viewProjRow[2],
								   params.viewProjRow[3], reverseZ, lo, hi, nearest );
	// (No pixel-centre test here: every entry passed it in the first cut - the same box through this frame's matrix.)
	vec2 t0, t1;
	if( testable && params.hzb.x != 0u )
	{
		jahHzbTexels( lo, hi, params.hzbRect, vec2( float( params.hzb.y ), float( params.hzb.z ) ), t0, t1 );
		int mip;
		if( jahHzbOccluded( nearest, jahHzbFarthestUnder( t0, t1, params.hzb, float( params.occl.x ), mip ), reverseZ ) )
		{
			atomicAdd( counter[35], 1u );
			return;
		}
	}
	// ---- DRAWN LATE ----------------------------------------------------------------
	uint n = range.y;
	uint base = atomicAdd( counter[11], n );
	if( base + n > params.cut.z )
	{
		atomicAdd( counter[13], 1u );
		return;
	}
	uint rec = atomicAdd( counter[8], 1u );
	atomicMax( counter[26], ( rec + 64u ) / 64u );
	if( rec >= params.cut.y )
	{
		atomicAdd( counter[13], 1u );
		return;
	}
	records[rec] = uvec4( slot, entry.y, base, entry.z );
	atomicAdd( counter[4], n / 3u );
	uint flags = floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + 7u].w );
	uint kind = ( ( flags & JAH_FLAG_TWO_SIDED ) != 0u ? 1u : 0u ) + ( ( flags & JAH_FLAG_ALPHA_TESTED ) != 0u ? 2u : 0u );
	uint o = ( kind * budget + atomicAdd( counter[29u + kind], 1u ) ) * 5u;
	clusterDraws[o + 0u] = n;
	clusterDraws[o + 1u] = 1u;
	clusterDraws[o + 2u] = base;
	clusterDraws[o + 3u] = 0u;
	clusterDraws[o + 4u] = e;
	clusterIds[e] = uvec2( slot, base / 3u );
}
