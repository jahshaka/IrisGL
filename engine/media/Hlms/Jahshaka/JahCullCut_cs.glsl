// Jahshaka — ATOM-CLUSTER-CUT: THE CUT, the cull's mode-3 job after the compaction
// (SPECS/v2/CLUSTER_CUT_DESIGN.md D1 and D2). ONE WORKGROUP PER SURVIVOR, its 64
// threads striding the survivor's mesh's clusters; the dispatch is sized by the GPU
// (the compaction leaves the survivor count in count[5], ogre-patch 0032's indirect
// dispatch reads it).
//
// THE RULE IS THE CLUSTER CUT'S (JahClusterCut.glsl, the twin of Types.h's
// clusterGroupAllowed / clusterGroupAffordable / clusterDrawn): a cluster is drawn
// when its own group is NOT affordable and its refined group IS, or it is a leaf —
// each group judged at its OWN distance with the view's tolerance. The two group
// answers a cluster needs are evaluated inline, per cluster (a dot, a sqrt and a
// compare each): the flat evaluation, with no per-(instance, group) table between
// two dispatches to size, write and read.
//
// WHAT IT WRITES — the compaction's first half:
//   1. the survivor's DRAW COMMAND: indexCount = the drawn clusters' indices,
//      firstIndex = where its contiguous run of the view's stream starts (one
//      atomic on count[11] per survivor), firstInstance = the slot;
//   2. per drawn cluster a RECORD (slot, the cluster's global index, its first
//      index in the stream, its depth) — the emit job's work list, one atomic on
//      count[8] per survivor, the offsets inside the run a workgroup prefix sum;
//   3. the slot's first TRIANGLE in the stream (the id pass's fragment adds its
//      primitive id to it).
// THE BUDGET IS NEVER EXCEEDED AND NEVER SILENT: a survivor whose run does not fit
// the stream's or the records' capacity draws NOTHING this frame (its command
// counts zero indices, its records are written as skips) and is counted in
// count[12] with the indices it asked for in count[13]; the host doubles the
// budget from the stats ring (GpuCull::noteCutOverflow).
//
// No Hlms directive mark appears in any comment of this file.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( JahLevelRuleScale )
@insertpiece( JahLevelRuleCurrency )
@insertpiece( JahClusterCut )

struct CullParams
{
	vec4  planes[6];
	vec4  viewProjRow[4];
	vec4  eye;              // xyz the camera
	vec4  lod;              // x tolerance (samples), y proj[1][1], z viewport height, w 1 = orthographic
	uvec4 counts;           // x instanceCount, y flagsRequired, z flagsForbidden, w mode
	uvec4 hzb;
	uvec4 cut;              // x stream capacity (indices), y record capacity
};

struct GpuMesh
{
	uvec4 counts;
	vec4  localBoundsMin;
	vec4  localBoundsMax;
	uvec4 dag;              // x first cluster, y cluster count, z first group, w the cluster row
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { CullParams params; };
// The instance table as raw vec4 LANES (GpuScene.h: 10 per entry; the world rows are
// lanes 0-2, boundsMin lane 6 with the mesh index bit-cast into w).
layout( std430, ogre_U1 ) readonly restrict buffer instLayout { vec4 instanceWords[]; };
layout( std430, ogre_U2 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( std430, ogre_U3 ) readonly restrict buffer clusterLayout { JahCluster clusters[]; };
layout( std430, ogre_U4 ) readonly restrict buffer groupLayout { JahClusterGroup groups[]; };
layout( std430, ogre_U5 ) readonly restrict buffer survLayout { uint survivors[]; };
layout( std430, ogre_U6 ) restrict buffer cntLayout { uint counter[]; };
layout( std430, ogre_U7 ) writeonly restrict buffer drawLayout { uint draws[]; };
layout( std430, ogre_U8 ) writeonly restrict buffer recordLayout { uvec4 records[]; };
layout( std430, ogre_U9 ) writeonly restrict buffer slotBaseLayout { uint slotBase[]; };

#define JAH_INSTANCE_LANES 10u
#define JAH_NO_GROUP 0xFFFFFFFFu
#define JAH_CUT_WIDTH @value( threads_per_group_x )

shared uint gIdx[JAH_CUT_WIDTH];
shared uint gRec[JAH_CUT_WIDTH];
shared uint gBase;
shared uint gRecBase;
shared uint gFit;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vec4 gRow0;
vec4 gRow1;
vec4 gRow2;
float gScale;

bool jahCutAffordable( uint g )
{
	JahClusterGroup gr = groups[g];
	float allowed = jahClusterGroupAllowed( gr.sphere, gRow0, gRow1, gRow2, gScale, params.eye.xyz,
											params.lod.x, params.lod.y, params.lod.z, params.lod.w > 0.5 );
	return jahClusterGroupAffordable( gr.error.x, allowed );
}

bool jahCutDrawn( uint c )
{
	uvec4 range = clusters[c].range;
	bool isLevel0 = range.w == JAH_NO_GROUP;
	bool own = jahCutAffordable( range.z );
	bool refined = isLevel0 ? false : jahCutAffordable( range.w );
	return jahClusterDrawn( own, isLevel0, refined );
}

void main()
{
	uint i = gl_WorkGroupID.x;
	uint lane = gl_LocalInvocationID.x;
	// The whole workgroup leaves together (every thread reads the same count).
	if( i >= counter[0] )
		return;

	uint slot = survivors[i];
	gRow0 = instanceWords[slot * JAH_INSTANCE_LANES + 0u];
	gRow1 = instanceWords[slot * JAH_INSTANCE_LANES + 1u];
	gRow2 = instanceWords[slot * JAH_INSTANCE_LANES + 2u];
	uint meshIndex = floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + 6u].w );
	uvec4 dag = meshes[meshIndex].dag;
	gScale = jahWorldMaxAxisScale( gRow0, gRow1, gRow2 );

	// ---- pass 1: what this lane's clusters draw --------------------------------
	uint myIdx = 0u;
	uint myRec = 0u;
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		if( jahCutDrawn( dag.x + c ) )
		{
			myIdx += clusters[dag.x + c].range.y;
			++myRec;
		}
	}
	gIdx[lane] = myIdx;
	gRec[lane] = myRec;
	barrier();
	// HILLIS-STEELE, inclusive, both sums at once (JahCullCompact_cs.glsl's scan).
	for( uint off = 1u; off < JAH_CUT_WIDTH; off <<= 1u )
	{
		uint addIdx = lane >= off ? gIdx[lane - off] : 0u;
		uint addRec = lane >= off ? gRec[lane - off] : 0u;
		barrier();
		gIdx[lane] += addIdx;
		gRec[lane] += addRec;
		barrier();
	}
	uint totalIdx = gIdx[JAH_CUT_WIDTH - 1u];
	uint totalRec = gRec[JAH_CUT_WIDTH - 1u];
	if( lane == 0u )
	{
		atomicAdd( counter[14], dag.y );
		uint base = totalIdx != 0u ? atomicAdd( counter[11], totalIdx ) : 0u;
		uint recBase = totalRec != 0u ? atomicAdd( counter[8], totalRec ) : 0u;
		bool fit = base + totalIdx <= params.cut.x && recBase + totalRec <= params.cut.y;
		if( !fit )
		{
			atomicAdd( counter[12], 1u );
			atomicAdd( counter[13], totalIdx );
		}
		else
		{
			atomicAdd( counter[4], totalIdx / 3u );
		}
		uint o = i * 5u;
		draws[o + 0u] = fit ? totalIdx : 0u;
		draws[o + 1u] = 1u;
		draws[o + 2u] = fit ? base : 0u;
		draws[o + 3u] = 0u;
		draws[o + 4u] = slot;
		slotBase[slot] = fit ? base / 3u : 0u;
		gBase = base;
		gRecBase = recBase;
		gFit = fit ? 1u : 0u;
	}
	barrier();

	// ---- pass 2: the records, at this lane's offsets inside the run -------------
	uint idx = gBase + gIdx[lane] - myIdx;
	uint rec = gRecBase + gRec[lane] - myRec;
	bool fit = gFit != 0u;
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		if( !jahCutDrawn( dag.x + c ) )
			continue;
		uvec4 range = clusters[dag.x + c].range;
		if( rec < params.cut.y )
		{
			uint depth = range.w == JAH_NO_GROUP ? 0u : uint( groups[range.w].error.z ) + 1u;
			records[rec] = fit ? uvec4( slot, dag.x + c, idx, depth ) : uvec4( 0xFFFFFFFFu, 0u, 0u, 0u );
		}
		idx += range.y;
		++rec;
	}
}
