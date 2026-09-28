// Jahshaka — ATOM-CLUSTER-CUT: THE CUT, the cull's mode-3 job after the compaction
// (SPECS/v2/CLUSTER_CUT_DESIGN.md D1 and D2). ONE WORKGROUP PER SURVIVOR, its 64
// threads striding the survivor's mesh's clusters; the dispatch is sized by the GPU
// (the compaction leaves the survivor count in count[5], fork 1bccc3f93+a98e2b0af (was 0032)'s indirect
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
// THE BUDGET IS NEVER EXCEEDED, NEVER SILENT, AND AN OVERFLOW LOSES NO OBJECT
// (the fix round's F1): the stream is two regions — the MAIN run [0, cut.z) every
// survivor's cut reserves from, and a COARSE reserve [cut.z, cut.x) — and a survivor
// whose cut does not fit the main region draws its ROOT CUT from the reserve that
// frame instead: every terminal group's clusters (the coarsest end the DAG has, the
// cut at an infinite tolerance), so the object is there, coarse, for the frames the
// budget takes to grow. Counted in count[12] (drawn coarse); a survivor that fits
// neither draws nothing and is counted in count[13] (the only way an object can be
// missing, and the host grows the budget from either through the stats ring).
//
// EACH GROUP IS JUDGED ONCE (F2/F3): pass 0 evaluates every group of the mesh into
// a shared bitmask (30 KB: 245,760 groups), and both cluster passes read it — a
// group within an ulp of its threshold can no longer answer differently at the two
// places a cluster needs it (its own group, its children's refined group), and the
// rule runs once per group instead of four times per cluster. A mesh with more
// groups than the mask holds evaluates inline (the arithmetic is `precise`).
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
#define JAH_TERMINAL 3.0e38
#define JAH_CUT_WIDTH @value( threads_per_group_x )
#define JAH_GROUP_WORDS 7680u   // 30 KB: the whole job stays under the 32 KB every device offers (Apple, lavapipe)

shared uint gIdx[JAH_CUT_WIDTH];
shared uint gRec[JAH_CUT_WIDTH];
shared uint gRootIdx[JAH_CUT_WIDTH];
shared uint gRootRec[JAH_CUT_WIDTH];
shared uint gAffordable[JAH_GROUP_WORDS];
shared uint gBase;
shared uint gRecBase;
shared uint gMode;   // 0 nothing, 1 the cut, 2 the root cut
// Record slots a reservation took and could not use (its range ran past the record
// capacity): written as SKIPS, because the emit job's dispatch is the record cursor
// and would otherwise read a stale record of an earlier frame. BOTH words are written
// by lane 0 for every workgroup (ATOM-BLACK-FRAMES-1): shared memory starts with
// whatever the SM's previous workgroup left there, and a base left unwritten beside a
// zero count once wrapped the skip loop's unsigned bound - the loop then stamped skips
// over the whole record buffer up to the budget, the emit skipped nearly every cluster,
// and the id pass drew this frame's commands over the previous frame's stream: whole
// frames of garbage ids that the decode refused (black), at random, 1 to 3 per 100.
shared uint gSkipBase[2];
shared uint gSkipCount[2];

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

vec4 gRow0;
vec4 gRow1;
vec4 gRow2;
float gScale;
uint gGroupBase;
bool gUseMask;

bool jahCutEvaluate( uint g )
{
	JahClusterGroup gr = groups[g];
	float allowed = jahClusterGroupAllowed( gr.sphere, gRow0, gRow1, gRow2, gScale, params.eye.xyz,
											params.lod.x, params.lod.y, params.lod.z, params.lod.w > 0.5 );
	return jahClusterGroupAffordable( gr.error.x, allowed );
}

bool jahCutAffordable( uint g )
{
	if( gUseMask )
	{
		uint l = g - gGroupBase;
		return ( gAffordable[l >> 5u] & ( 1u << ( l & 31u ) ) ) != 0u;
	}
	return jahCutEvaluate( g );
}

bool jahCutDrawn( uvec4 range )
{
	bool isLevel0 = range.w == JAH_NO_GROUP;
	bool own = jahCutAffordable( range.z );
	bool refined = isLevel0 ? false : jahCutAffordable( range.w );
	return jahClusterDrawn( own, isLevel0, refined );
}

// THE ROOT CUT: the cut at an infinite tolerance — every finite group affordable, so a
// cluster is drawn when its own group is TERMINAL and it is a leaf or was produced by
// a finite group.
bool jahRootDrawn( uvec4 range )
{
	if( groups[range.z].error.x < JAH_TERMINAL )
		return false;
	return range.w == JAH_NO_GROUP || groups[range.w].error.x < JAH_TERMINAL;
}

void jahScan( uint lane )
{
	// HILLIS-STEELE, inclusive, the four sums at once (JahCullCompact_cs.glsl's scan).
	for( uint off = 1u; off < JAH_CUT_WIDTH; off <<= 1u )
	{
		uint a = lane >= off ? gIdx[lane - off] : 0u;
		uint b = lane >= off ? gRec[lane - off] : 0u;
		uint c = lane >= off ? gRootIdx[lane - off] : 0u;
		uint d = lane >= off ? gRootRec[lane - off] : 0u;
		barrier();
		gIdx[lane] += a;
		gRec[lane] += b;
		gRootIdx[lane] += c;
		gRootRec[lane] += d;
		barrier();
	}
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
	uint groupCount = floatBitsToUint( meshes[meshIndex].localBoundsMin.w );
	gScale = jahWorldMaxAxisScale( gRow0, gRow1, gRow2 );
	gGroupBase = dag.z;
	gUseMask = groupCount <= JAH_GROUP_WORDS * 32u;

	// ---- pass 0: every group judged ONCE -----------------------------------------
	if( gUseMask )
	{
		uint words = ( groupCount + 31u ) >> 5u;
		for( uint w = lane; w < words; w += JAH_CUT_WIDTH )
			gAffordable[w] = 0u;
		barrier();
		for( uint g = lane; g < groupCount; g += JAH_CUT_WIDTH )
			if( jahCutEvaluate( dag.z + g ) )
				atomicOr( gAffordable[g >> 5u], 1u << ( g & 31u ) );
		barrier();
	}

	// ---- pass 1: what this lane's clusters draw, in the cut and in the root cut ---
	uint myIdx = 0u, myRec = 0u, myRootIdx = 0u, myRootRec = 0u;
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		uvec4 range = clusters[dag.x + c].range;
		if( jahCutDrawn( range ) )
		{
			myIdx += range.y;
			++myRec;
		}
		if( jahRootDrawn( range ) )
		{
			myRootIdx += range.y;
			++myRootRec;
		}
	}
	gIdx[lane] = myIdx;
	gRec[lane] = myRec;
	gRootIdx[lane] = myRootIdx;
	gRootRec[lane] = myRootRec;
	barrier();
	jahScan( lane );
	uint totalIdx = gIdx[JAH_CUT_WIDTH - 1u];
	uint totalRec = gRec[JAH_CUT_WIDTH - 1u];
	uint rootIdx = gRootIdx[JAH_CUT_WIDTH - 1u];
	uint rootRec = gRootRec[JAH_CUT_WIDTH - 1u];
	if( lane == 0u )
	{
		atomicAdd( counter[14], dag.y );
		gSkipBase[0] = 0u;
		gSkipBase[1] = 0u;
		gSkipCount[0] = 0u;
		gSkipCount[1] = 0u;
		uint mode = 0u;
		uint base = 0u, recBase = 0u, drawnIdx = 0u;
		if( totalIdx != 0u )
		{
			base = atomicAdd( counter[11], totalIdx );
			if( base + totalIdx <= params.cut.z )
			{
				recBase = atomicAdd( counter[8], totalRec );
				if( recBase + totalRec <= params.cut.y )
				{
					mode = 1u;
					drawnIdx = totalIdx;
				}
				else
				{
					gSkipBase[0] = recBase;
					gSkipCount[0] = totalRec;
				}
			}
		}
		if( mode == 0u && totalIdx != 0u && rootIdx != 0u )
		{
			// THE COARSE RESERVE: the root cut, from its own region of the stream.
			uint rb = params.cut.z + atomicAdd( counter[15], rootIdx );
			if( rb + rootIdx <= params.cut.x )
			{
				uint rr = atomicAdd( counter[8], rootRec );
				if( rr + rootRec <= params.cut.y )
				{
					mode = 2u;
					base = rb;
					recBase = rr;
					drawnIdx = rootIdx;
				}
				else
				{
					gSkipBase[1] = rr;
					gSkipCount[1] = rootRec;
				}
			}
			if( mode == 2u )
				atomicAdd( counter[12], 1u );
		}
		if( mode == 0u && totalIdx != 0u )
			atomicAdd( counter[13], 1u );
		if( mode != 0u )
			atomicAdd( counter[4], drawnIdx / 3u );
		uint o = i * 5u;
		draws[o + 0u] = drawnIdx;
		draws[o + 1u] = 1u;
		draws[o + 2u] = mode != 0u ? base : 0u;
		draws[o + 3u] = 0u;
		draws[o + 4u] = slot;
		slotBase[slot] = mode != 0u ? base / 3u : 0u;
		gBase = base;
		gRecBase = recBase;
		gMode = mode;
	}
	barrier();

	// ---- pass 2: the records, at this lane's offsets inside the run ----------------
	// Reserved-but-unusable record slots become skips (inside the capacity only: the
	// emit job never reads past it). The loop counts OFFSETS from the base: at most
	// gSkipCount[k] records, the base written by lane 0 for every workgroup.
	for( uint k = 0u; k < 2u; ++k )
	{
		uint skipBase = gSkipBase[k];
		uint skipCount = gSkipCount[k];
		for( uint j = lane; j < skipCount; j += JAH_CUT_WIDTH )
		{
			uint r = skipBase + j;
			if( r < params.cut.y )
				records[r] = uvec4( 0xFFFFFFFFu, 0u, 0u, 0u );
		}
	}
	uint mode = gMode;
	if( mode == 0u )
		return;
	uint idx = gBase + ( mode == 1u ? gIdx[lane] - myIdx : gRootIdx[lane] - myRootIdx );
	uint rec = gRecBase + ( mode == 1u ? gRec[lane] - myRec : gRootRec[lane] - myRootRec );
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		uvec4 range = clusters[dag.x + c].range;
		if( mode == 1u ? !jahCutDrawn( range ) : !jahRootDrawn( range ) )
			continue;
		uint depth = range.w == JAH_NO_GROUP ? 0u : uint( groups[range.w].error.z ) + 1u;
		records[rec] = uvec4( slot, dag.x + c, idx, depth );
		idx += range.y;
		++rec;
	}
}
