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
//      atomic on count[11] per survivor), firstInstance = the slot — at the
//      survivor's index, or, for a TWO-SIDED or a MASKED survivor, at the next command
//      of the list's range for it (cut.w on): two-sided at cut.w (count[17]), masked
//      at 2 cut.w (count[18]), masked two-sided at 3 cut.w (count[19]) — the passes
//      draw each range with its own pipeline (no cull; a fragment stage that discards
//      at the cut-out's mask, ENGINE-POLISH-ATOM-1 stage 3). THE FACES ARE THE PASS'S:
//      a caster request (it asks for kGpuAtomCaster) reads the caster's two-sided bit,
//      every other the colour's;
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
// THE PER-CLUSTER OCCLUSION (ATOM-8 stage 1; SPECS/ATOM_8_SPEC.md, Nanite's two-pass at cluster level): a view's
// list (params.occl.x) tests every cluster its cut would draw, the box of its culling sphere, against the pyramid
// (JahHzbTest.glsl: the 4-texel mip, the farthest depth under the rectangle, the box's nearest point, the margin).
// In the FIRST list (occl.w = 1: last frame's pyramid, last frame's matrix) a cluster it hides is NOT drawn: it is
// appended to THE REJECTED LIST (slot, global cluster, depth) - one reservation per workgroup on count[23], the late
// cut's dispatch kept in count[20..22] - and the late pass re-tests exactly those against this frame's pyramid
// (JahCullCutLate_cs.glsl). An entry past the list's budget is DRAWN (conservative: an overflow never hides anything)
// and counted (count[24]). In the LATE list (occl.w = 0: this frame's pyramid, this frame's matrix, the re-tested
// objects) a hidden cluster is dropped - it is behind depth of this very frame (count[35]). Every list with a
// viewport rectangle (occl.y) culls a cluster whose rectangle, through THIS frame's matrix, covers no pixel centre:
// no fragment exists for it (count[25]). A cluster the test kept although its sphere's CENTRE lies behind the
// occluders - kept by the sphere's depth slack alone - is counted (count[33] clusters, [34] triangles: the measure
// of what a tighter bound would add), and one kept with the far plane under its rectangle (count[36], [37]: the sky
// class). The root cut (the overflow arm) tests nothing.
//
// THE SHARED-MEMORY LAW (the stage 1 read): the job holds 30,720 B of group mask + six 64-word scan arrays + the
// lane-0 words = 32,300 of the 32,768 B every device offers; the NEXT 64-word array breaks it - add none.
//
// THE DEPTH TEST RUNS IN BOTH PASSES (pass 1 counts, pass 2 writes): its verdict per cluster is not stored (no shared
// memory is left for it), and the two evaluations take the same inputs; pass 2 never writes past lane 0's
// reservations (recEnd, rejEnd).
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
	uvec4 hzb;              // x levels (0 = no pyramid), y width, z height, w reverse-Z
	uvec4 cut;              // x stream capacity (indices), y record capacity
	uvec4 hzbRect;          // the viewport's rectangle in the pyramid's mip 0 (w 0 = all of it)
	vec4  viewProjNowRow[4];// THIS frame's cull matrix (the pixel-centre test; viewProjRow is the pyramid's)
	uvec4 viewRect;         // the pass's viewport in its target's pixels (x0, y0, w, h; w 0 = no pixel test)
	uvec4 occl;             // x the cluster test's footprint (texels; 0 = no test), y the pixel-centre cull, z the
	                        // rejected budget, w 1 = first list
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
@property( cull_hzb )
vulkan_layout( ogre_t0 ) uniform texture2D hzbTexture;
@end
@property( cut_rejected )
// THE REJECTED CLUSTERS (the first list's, ATOM-8 stage 1): uvec4 (slot, global cluster, depth, 0) an entry, a
// skip's slot all ones; the next free unit (after the probe's).
@property( cut_probe )
layout( std430, ogre_U11 ) writeonly restrict buffer rejectedLayout { uvec4 rejected[]; };
@else
layout( std430, ogre_U10 ) writeonly restrict buffer rejectedLayout { uvec4 rejected[]; };
@end
@end
@property( cut_boxes )
// THE CLUSTERS' OWN BOXES (ATOM-8 stage 2; GpuScene::clusterBoxBuffer, one vec4 a cluster beside the cluster table):
// the depth test's bound - the next free unit (after the probe's and the rejected list's).
@property( cut_probe && cut_rejected )
layout( std430, ogre_U12 ) readonly restrict buffer boxLayout { vec4 clusterBoxes[]; };
@end
@property( cut_probe && !cut_rejected )
layout( std430, ogre_U11 ) readonly restrict buffer boxLayout { vec4 clusterBoxes[]; };
@end
@property( !cut_probe && cut_rejected )
layout( std430, ogre_U11 ) readonly restrict buffer boxLayout { vec4 clusterBoxes[]; };
@end
@property( !cut_probe && !cut_rejected )
layout( std430, ogre_U10 ) readonly restrict buffer boxLayout { vec4 clusterBoxes[]; };
@end
@end
@property( cut_probe )
// THE CUT PROBE (ATOM-2 stage 1; Scene::atomCutProbe — one frame on demand, never a frame's path): every group of
// every survivor's DAG with the terms the rule judged it on. [0] = (the cursor, the capacity in records, the records
// dropped past it, 0), then two uvec4 a record: (slot, the group's index in its mesh's DAG, its depth, the verdict
// bits: 1 affordable, 2 terminal) and the float bits of (the error, allowed, the distance d, radius x scale).
// Only the view's own lists carry it (the caster cut never does: its receiver buffers hold U10 and U11).
layout( std430, ogre_U10 ) restrict buffer probeLayout { uvec4 probe[]; };
@end
@property( cut_receivers )
// THE SUN MAP'S RECEIVERS (ENGINE-UPGRADE-U5 stage 4b; JahReceiverGrid_cs builds the grid): the request (the map's
// cull matrix, its size, the tile, the dilation, the depth margin) and the tiles of farthest receiver depth.
struct RecvParams
{
	vec4  invViewProjRow[4];
	vec4  lightRow[4];
	vec4  planes[6];
	uvec4 dims;     // z tiles x, w tiles y
	uvec4 map;      // x map width, y map height, z tile texels
	vec4  misc;     // y the dilation in texels along the map's x, w along its y (decision 19.1)
	uvec4 counts;   // w the pyramid's levels
	uvec4 rect;
};
layout( std430, ogre_U10 ) readonly restrict buffer recvParamsLayout { RecvParams recv; };
// The grid as a FARTHEST-DEPTH PYRAMID in one buffer (JahReceiverGrid_cs: the tiles at level 0, the levels one
// after the other; an empty region all bits set): the test reads <= 5 x 5 texels at the level where the cluster's
// rectangle is <= 4 texels wide - the farthest receiver under them.
layout( std430, ogre_U11 ) readonly restrict buffer recvGridLayout { uint recvGrid[]; };
uvec2 jahRecvLevelSize( uint k )
{
	return max( uvec2( recv.dims.z, recv.dims.w ) >> k, uvec2( 1u ) );
}
@end

// THE BOX TEST (JahHzbTest.glsl), after hzbTexture's declaration.
@insertpiece( JahHzbTest )

#define JAH_INSTANCE_LANES 10u
#define JAH_FLAG_TWO_SIDED 0x800u          // GpuScene.h kGpuTwoSided (the colour pass's faces)
#define JAH_FLAG_CASTER_TWO_SIDED 0x4000u  // GpuScene.h kGpuCasterTwoSided (the caster's faces)
#define JAH_FLAG_ALPHA_TESTED 0x10u        // GpuScene.h kGpuAlphaTested (the masked ranges)
#define JAH_FLAG_ATOM_CASTER 0x2000u       // GpuScene.h kGpuAtomCaster (a caster request asks for it)
#define JAH_NO_GROUP 0xFFFFFFFFu
#define JAH_TERMINAL 3.0e38
#define JAH_CUT_WIDTH @value( threads_per_group_x )
#define JAH_GROUP_WORDS 7680u   // 30 KB: the whole job stays under the 32 KB every device offers (Apple, lavapipe)

shared uint gIdx[JAH_CUT_WIDTH];
shared uint gRec[JAH_CUT_WIDTH];
shared uint gRootIdx[JAH_CUT_WIDTH];
shared uint gRootRec[JAH_CUT_WIDTH];
shared uint gOccIdx[JAH_CUT_WIDTH];
shared uint gOccRec[JAH_CUT_WIDTH];
shared uint gRejBase;
shared uint gRejCount;
shared uint gRejMode;
shared uint gRecEnd;    // the end of this workgroup's record reservation   // 0 none, 1 the entries are written, 2 overflowed (the clusters drawn), 3 the entries are skips
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
// THE INSTANCE'S SHADOW CONSTANT BIAS IS NEGATIVE (raster.w, lane 9): its raster depth moves NEARER the light than
// its geometry says, so no depth comparison made from its sphere is exact - the receiver test keeps it whole, as
// the light-depth test does (JahCullTest_cs, decision 15.3; decision 19.2).
bool gNegativeBias;
uint gGroupBase;
bool gUseMask;

// THE CLUSTER'S FRUSTUM TEST (SPEED-VR-MEM, perf audit atom-raster 2c). The instance
// test (JahCullTest_cs.glsl) passes a survivor whose box touches the view; its cut then
// emitted every drawn cluster, so an asset around the camera drew its whole 360-degree
// cut (the 5 M shell at 1.3 R: 639 k triangles). A cluster whose bounding sphere (the
// bake's, mesh space, culling only - JahClusterCut.glsl) lies wholly behind one of the
// request's six planes rasterises nothing in this view, so it is not drawn: the same
// planes, the same inward normalised form and the same conservative sense as the
// instance's test, on the sphere carried to world space by the instance's rows (its
// centre transformed, its radius by the largest axis scale). The pick is EXACT in the
// picture - no triangle of a dropped cluster can cover a pixel of this view - and the cut
// stays crack-free (the clusters around a seam that are in view are all still drawn). A
// relative margin of 2^-12 of the radius (plus 1 micrometre) keeps a cluster whose
// sphere only grazes a plane drawn against the rounding of the transform.
// THE FOUR SIDE PLANES ONLY (planes 0-3, fillCullFrustum's order), never the depth
// pair: a shadow camera draws with DEPTH CLAMP (Camera::getNeedsDepthClamp), so a
// caster in front of its near plane still writes the near depth and shadows what lies
// behind it - dropping it there would move the shadow (the selftest's B2 pose caught
// it). The side planes alone still drop every cluster outside the picture's rectangle,
// and for a perspective view everything behind the eye.
bool jahClusterInFrustum( vec4 sphere )
{
	vec3 c = vec3( dot( gRow0.xyz, sphere.xyz ) + gRow0.w,
				   dot( gRow1.xyz, sphere.xyz ) + gRow1.w,
				   dot( gRow2.xyz, sphere.xyz ) + gRow2.w );
	float r = sphere.w * gScale;
	r += r * ( 1.0 / 4096.0 ) + 1.0e-6;
	for( int i = 0; i < 4; ++i )
	{
		vec4 pl = params.planes[i];
		if( dot( pl.xyz, c ) + pl.w < -r )
			return false;
	}
	return true;
}

// THE RECEIVER TEST (stage 4b, decision 17): a caster cluster changes the picture only where its shadow can fall on
// a receiver the camera sees. Its sphere in the map, dilated by the shadow filter's reach, the receivers' normal
// offset and a tile (recv.misc.y texels along the map's x, recv.misc.w along its y: a focused map's window is not
// square), is WANTED when some tile under it holds a receiver FARTHER from the light than the sphere's nearest
// point (reverse-Z: a smaller value); else none of its texels is ever compared against a receiver's depth it could
// beat - not drawn, and the picture is the same. An instance with a negative shadow bias is kept whole.
bool jahClusterReceived( vec4 sphere )
{
@property( cut_receivers )
	if( gNegativeBias )
		return true;
	vec4 c4 = vec4( dot( gRow0.xyz, sphere.xyz ) + gRow0.w,
					dot( gRow1.xyz, sphere.xyz ) + gRow1.w,
					dot( gRow2.xyz, sphere.xyz ) + gRow2.w, 1.0 );
	float r = sphere.w * gScale;
	r += r * ( 1.0 / 4096.0 ) + 1.0e-6;
	vec3 q = vec3( dot( recv.lightRow[0], c4 ), dot( recv.lightRow[1], c4 ), dot( recv.lightRow[2], c4 ) );
	vec3 ext = r * vec3( length( recv.lightRow[0].xyz ), length( recv.lightRow[1].xyz ), length( recv.lightRow[2].xyz ) );
	float nearest = q.z + ext.z;
	vec2 t = vec2( ( q.x * 0.5 + 0.5 ) * float( recv.map.x ), ( 0.5 - q.y * 0.5 ) * float( recv.map.y ) );
	vec2 half2 = vec2( ext.x * 0.5 * float( recv.map.x ), ext.y * 0.5 * float( recv.map.y ) ) + recv.misc.yw;
	vec2 lo = max( t - half2, vec2( 0.0 ) );
	vec2 hi = min( t + half2, vec2( recv.map.xy ) - vec2( 1.0 ) );
	if( lo.x > hi.x || lo.y > hi.y )
		return true;   // outside the map's rectangle: the side planes' business, kept here
	// IN TILES, at the mip where the rectangle is <= 4 texels wide (<= 5 texel indices a side, <= 25 reads): each
	// texel holds the farthest receiver of its footprint, so the farthest under the rectangle is the least of
	// those. (The 2-texel mip of the depth pyramids kept 3.14x the id pass on Bistro's interior map 1 where the
	// tile walk keeps 2.48x; the 4-texel mip is the measured compromise.)
	vec2 t0 = lo / float( recv.map.z ), t1 = hi / float( recv.map.z );
	float widest = max( t1.x - t0.x, t1.y - t0.y );
	int mip = clamp( int( ceil( log2( max( widest, 1.0 ) / 4.0 ) ) ), 0, int( recv.counts.w ) - 1 );
	uint base = 0u;
	for( int j = 0; j < mip; ++j )
	{
		uvec2 sz = jahRecvLevelSize( uint( j ) );
		base += sz.x * sz.y;
	}
	ivec2 mipSize = ivec2( jahRecvLevelSize( uint( mip ) ) );
	ivec2 a = clamp( ivec2( floor( t0 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );
	ivec2 b = clamp( ivec2( floor( t1 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );
	uint farthest = 0xFFFFFFFFu;
	for( int y = a.y; y <= b.y; ++y )
		for( int x = a.x; x <= b.x; ++x )
			farthest = min( farthest, recvGrid[base + uint( y ) * uint( mipSize.x ) + uint( x )] );
	return farthest != 0xFFFFFFFFu && nearest > uintBitsToFloat( farthest );
@else
	return true;
@end
}

// THE CLUSTER'S DEPTH TEST (ATOM-8 stage 1): 0 drawn, 1 hidden by the pyramid, 2 covers no pixel centre. `slack`:
// kept although the sphere's centre lies behind the occluders under its rectangle; `sky`: kept with the far plane
// (nothing drawn) somewhere under its rectangle.
uint jahClusterOcclusion( uint global, vec4 sphere, out bool slack, out bool sky )
{
	slack = false;
	sky = false;
	if( params.occl.x == 0u )
		return 0u;
	// THE BOUND (ATOM-8 stage 2): the cluster's own box, the instance's rows carrying it (the sphere stays the LOD's).
	vec3 c, ax, ay, az;
@property( cut_boxes )
	jahClusterObb( clusterBoxes[global], sphere, gRow0, gRow1, gRow2, gScale, c, ax, ay, az );
@else
	jahClusterObb( vec4( 0.0, 0.0, 0.0, uintBitsToFloat( 0xFFFFFFFFu ) ), sphere, gRow0, gRow1, gRow2, gScale, c, ax,
				   ay, az );
@end
	bool reverseZ = params.hzb.w != 0u;
	vec2 lo, hi;
	float nearest;
	if( params.occl.y != 0u && params.viewRect.z != 0u &&
		jahHzbObbRect( c, ax, ay, az, params.viewProjNowRow[0], params.viewProjNowRow[1], params.viewProjNowRow[2],
					   params.viewProjNowRow[3], reverseZ, lo, hi, nearest ) )
	{
		vec2 t0, t1;
		jahHzbTexels( lo, hi, params.viewRect, vec2( params.viewRect.zw ), t0, t1 );
		if( !jahHzbCoversPixelCentre( t0, t1 ) )
			return 2u;
	}
@property( cull_hzb )
	if( params.hzb.x == 0u || gNegativeBias )
		return 0u;
	if( !jahHzbObbRect( c, ax, ay, az, params.viewProjRow[0], params.viewProjRow[1], params.viewProjRow[2],
						params.viewProjRow[3], reverseZ, lo, hi, nearest ) )
		return 0u;   // the box crosses the near plane: not testable, kept
	vec2 t0, t1;
	jahHzbTexels( lo, hi, params.hzbRect, vec2( float( params.hzb.y ), float( params.hzb.z ) ), t0, t1 );
	int mip;
	float farthestUnder = jahHzbFarthestUnder( t0, t1, params.hzb, float( params.occl.x ), mip );
	if( jahHzbOccluded( nearest, farthestUnder, reverseZ ) )
		return 1u;
	vec4 cc = jahHzbClip( params.viewProjRow[0], params.viewProjRow[1], params.viewProjRow[2], params.viewProjRow[3], c );
	slack = cc.w > 0.0 && jahHzbOccluded( cc.z / cc.w, farthestUnder, reverseZ );
	sky = farthestUnder == ( reverseZ ? 0.0 : 1.0 );
@end
	return 0u;
}

bool jahCutEvaluate( uint g )
{
	JahClusterGroup gr = groups[g];
	return jahClusterGroupVerdict( gr, gRow0, gRow1, gRow2, gScale, params.eye.xyz, params.lod.x, params.lod.y,
								   params.lod.z, params.lod.w > 0.5 );
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

// THE ROOT CUT: the cut at an infinite tolerance (JahClusterCut.glsl jahClusterRootDrawn, the
// piece the parity job runs too) — a cluster is drawn when its own group is the LAST of its
// chain (terminal, or the empty top: it produced nothing) and it is a leaf or was produced by
// a finite group.
bool jahRootDrawn( uvec4 range )
{
	bool isLevel0 = range.w == JAH_NO_GROUP;
	return jahClusterRootDrawn( groups[range.z].error, isLevel0, isLevel0 ? 0.0 : groups[range.w].error.x );
}

void jahScan( uint lane )
{
	// HILLIS-STEELE, inclusive, the six sums at once (JahCullCompact_cs.glsl's scan).
	for( uint off = 1u; off < JAH_CUT_WIDTH; off <<= 1u )
	{
		uint a = lane >= off ? gIdx[lane - off] : 0u;
		uint b = lane >= off ? gRec[lane - off] : 0u;
		uint c = lane >= off ? gRootIdx[lane - off] : 0u;
		uint d = lane >= off ? gRootRec[lane - off] : 0u;
		uint e = lane >= off ? gOccIdx[lane - off] : 0u;
		uint f = lane >= off ? gOccRec[lane - off] : 0u;
		barrier();
		gIdx[lane] += a;
		gRec[lane] += b;
		gRootIdx[lane] += c;
		gRootRec[lane] += d;
		gOccIdx[lane] += e;
		gOccRec[lane] += f;
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
	gNegativeBias = instanceWords[slot * JAH_INSTANCE_LANES + 9u].w < 0.0;
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

@property( cut_probe )
	// ---- THE PROBE: every group, judged by the function pass 0 judges it by -------
	for( uint g = lane; g < groupCount; g += JAH_CUT_WIDTH )
	{
		JahClusterGroup gr = groups[dag.z + g];
		float allowed = jahClusterGroupAllowed( gr.sphere, gRow0, gRow1, gRow2, gScale, params.eye.xyz,
												params.lod.x, params.lod.y, params.lod.z, params.lod.w > 0.5 );
		float d = jahClusterGroupDistance( gr.sphere, gRow0, gRow1, gRow2, gScale, params.eye.xyz,
										   params.lod.w > 0.5 );
		uint verdict = ( jahCutEvaluate( dag.z + g ) ? 1u : 0u ) | ( gr.error.x >= JAH_TERMINAL ? 2u : 0u );
		uint r = atomicAdd( probe[0].x, 1u );
		if( r < probe[0].y )
		{
			probe[1u + 2u * r] = uvec4( slot, g, uint( gr.error.z ), verdict );
			probe[2u + 2u * r] = uvec4( floatBitsToUint( gr.error.x ), floatBitsToUint( allowed ),
										floatBitsToUint( d ), floatBitsToUint( gr.sphere.w * gScale ) );
		}
		else
		{
			atomicAdd( probe[0].z, 1u );
		}
	}
@end

	// ---- pass 1: what this lane's clusters draw, in the cut and in the root cut ---
	uint myIdx = 0u, myRec = 0u, myRootIdx = 0u, myRootRec = 0u, myOccIdx = 0u, myOccRec = 0u;
	uint myPixel = 0u, myLateDrop = 0u, mySlack = 0u, mySlackTris = 0u, mySky = 0u, mySkyTris = 0u;
	bool firstList = params.occl.w != 0u;
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		uvec4 range = clusters[dag.x + c].range;
		if( !jahClusterInFrustum( clusters[dag.x + c].sphere ) )
			continue;
		// THE RECEIVER TEST, for a cluster one of the two cuts draws only (the DAG's others never reach a map).
		bool cutHere = jahCutDrawn( range ), rootHere = jahRootDrawn( range );
		if( ( cutHere || rootHere ) && !jahClusterReceived( clusters[dag.x + c].sphere ) )
			continue;
		if( cutHere )
		{
			// THE CLUSTER'S DEPTH TEST (ATOM-8 stage 1)
			bool slack, sky;
			uint v = jahClusterOcclusion( dag.x + c, clusters[dag.x + c].sphere, slack, sky );
			if( v == 2u )
			{
				cutHere = false;
				++myPixel;
			}
			else if( v == 1u )
			{
				cutHere = false;
				if( firstList )
				{
					myOccIdx += range.y;
					++myOccRec;
				}
				else
				{
					++myLateDrop;
				}
			}
			else
			{
				if( slack )
				{
					++mySlack;
					mySlackTris += range.y / 3u;
				}
				if( sky )
				{
					++mySky;
					mySkyTris += range.y / 3u;
				}
			}
		}
		if( cutHere )
		{
			myIdx += range.y;
			++myRec;
		}
		if( rootHere )
		{
			myRootIdx += range.y;
			++myRootRec;
		}
	}
	gIdx[lane] = myIdx;
	gRec[lane] = myRec;
	gRootIdx[lane] = myRootIdx;
	gRootRec[lane] = myRootRec;
	gOccIdx[lane] = myOccIdx;
	gOccRec[lane] = myOccRec;
	if( myPixel != 0u )
		atomicAdd( counter[25], myPixel );
	if( myLateDrop != 0u )
		atomicAdd( counter[35], myLateDrop );
	if( mySlack != 0u )
	{
		atomicAdd( counter[33], mySlack );
		atomicAdd( counter[34], mySlackTris );
	}
	if( mySky != 0u )
	{
		atomicAdd( counter[36], mySky );
		atomicAdd( counter[37], mySkyTris );
	}
	barrier();
	jahScan( lane );
	uint occIdx = gOccIdx[JAH_CUT_WIDTH - 1u];
	uint occRec = gOccRec[JAH_CUT_WIDTH - 1u];
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
		// THE REJECTED CLUSTERS' RESERVATION (ATOM-8 stage 1), before the stream's: an overflow draws them.
		gRejBase = 0u;
		gRejCount = 0u;
		gRejMode = 0u;
		if( occRec != 0u )
		{
			uint budget = params.occl.z;
			uint rb = atomicAdd( counter[38], occRec );
			atomicMax( counter[20], min( ( rb + occRec + 63u ) / 64u, ( budget + 63u ) / 64u ) );
			gRejBase = rb;
			gRejCount = occRec;
			if( rb + occRec <= budget )
			{
				gRejMode = 1u;
			}
			else
			{
				gRejMode = 2u;
			}
		}
		if( gRejMode == 2u )
		{
			totalIdx += occIdx;
			totalRec += occRec;
		}
		uint mode = 0u;
		uint base = 0u, recBase = 0u, drawnIdx = 0u;
		if( totalIdx != 0u )
		{
			base = atomicAdd( counter[11], totalIdx );
			if( base + totalIdx <= params.cut.z )
			{
				recBase = atomicAdd( counter[8], totalRec );
				// THE DRAWN REDUCE'S DISPATCH (count[26..28], ATOM-8): one 64-wide group per 64 records reserved.
				atomicMax( counter[26], ( recBase + totalRec + 63u ) / 64u );
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
				atomicMax( counter[26], ( rr + rootRec + 63u ) / 64u );
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
		// The entries are the clusters' only while their cut is the one drawn (or nothing of the object was drawn
		// at all); a root cut or a missing object leaves them skips (never a stale entry for the late cut).
		if( gRejMode == 1u && !( mode == 1u || totalIdx == 0u ) )
			gRejMode = 3u;
		if( gRejMode == 2u && mode != 1u )
			gRejMode = 3u;
		// THE COUNTS, EXACT (the stage 1 read): [23] the entries this cut wrote, [24] the overflowed ones it drew.
		if( gRejMode == 1u )
			atomicAdd( counter[23], occRec );
		if( gRejMode == 2u )
			atomicAdd( counter[24], occRec );
		uint o = i * 5u;
		// A TWO-SIDED or MASKED survivor's command (ATOM-TWO-SIDED-1; ENGINE-POLISH-ATOM-1
		// stage 3): in the list's range for it, which the passes draw with that range's
		// pipeline (count[17..19] the lengths); its command at the survivor index draws
		// nothing. The flags word is the instance's lane 7, w; the faces bit is the
		// request's pass's (a caster request asks for kGpuAtomCaster).
		uint flags = floatBitsToUint( instanceWords[slot * JAH_INSTANCE_LANES + 7u].w );
		const uint facesBit = ( params.counts.y & JAH_FLAG_ATOM_CASTER ) != 0u ? JAH_FLAG_CASTER_TWO_SIDED
																				: JAH_FLAG_TWO_SIDED;
		const uint range = ( ( flags & facesBit ) != 0u ? 1u : 0u ) +
						   ( ( flags & JAH_FLAG_ALPHA_TESTED ) != 0u ? 2u : 0u );
		if( params.cut.w != 0u && range != 0u && mode != 0u )
		{
			draws[o + 0u] = 0u;
			draws[o + 1u] = 0u;
			draws[o + 2u] = 0u;
			draws[o + 3u] = 0u;
			draws[o + 4u] = slot;
			o = ( range * params.cut.w + atomicAdd( counter[16u + range], 1u ) ) * 5u;
		}
		draws[o + 0u] = drawnIdx;
		draws[o + 1u] = 1u;
		draws[o + 2u] = mode != 0u ? base : 0u;
		draws[o + 3u] = 0u;
		draws[o + 4u] = slot;
		slotBase[slot] = mode != 0u ? base / 3u : 0u;
		gBase = base;
		gRecBase = recBase;
		gRecEnd = recBase + ( mode == 1u ? totalRec : mode == 2u ? rootRec : 0u );
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
	uint rejMode = gRejMode;
@property( cut_rejected )
	// THE REJECTED RANGE AS SKIPS when its entries are not this cut's (inside the budget only).
	if( rejMode == 2u || rejMode == 3u )
	{
		for( uint j = lane; j < gRejCount; j += JAH_CUT_WIDTH )
		{
			uint e = gRejBase + j;
			if( e < params.occl.z )
				rejected[e] = uvec4( 0xFFFFFFFFu, 0u, 0u, 0u );
		}
	}
@end
	bool writeRejected = rejMode == 1u;
	bool drawOccluded = rejMode == 2u;
	if( mode == 0u && !writeRejected )
		return;
	uint idx = gBase + ( mode == 1u ? gIdx[lane] - myIdx + ( drawOccluded ? gOccIdx[lane] - myOccIdx : 0u )
									: gRootIdx[lane] - myRootIdx );
	uint rec = gRecBase + ( mode == 1u ? gRec[lane] - myRec + ( drawOccluded ? gOccRec[lane] - myOccRec : 0u )
									   : gRootRec[lane] - myRootRec );
	// The second lock on the reservations (the two passes evaluate the same tests on the same inputs): nothing is
	// written past what lane 0 reserved.
	uint recEnd = gRecEnd;
	uint rejE = gRejBase + ( gOccRec[lane] - myOccRec );
	uint rejEnd = gRejBase + gRejCount;
	for( uint c = lane; c < dag.y; c += JAH_CUT_WIDTH )
	{
		uvec4 range = clusters[dag.x + c].range;
		if( !jahClusterInFrustum( clusters[dag.x + c].sphere ) )
			continue;
		if( mode == 2u ? !jahRootDrawn( range ) : !jahCutDrawn( range ) )
			continue;
		if( !jahClusterReceived( clusters[dag.x + c].sphere ) )
			continue;
		uint depth = range.w == JAH_NO_GROUP ? 0u : uint( groups[range.w].error.z ) + 1u;
		bool slack = false, sky = false;
		if( mode != 2u )
		{
			uint v = jahClusterOcclusion( dag.x + c, clusters[dag.x + c].sphere, slack, sky );
			if( v == 2u )
				continue;
			if( v == 1u && !firstList )
				continue;   // the late list: behind this frame's own depth
			if( v == 1u && !drawOccluded )
			{
@property( cut_rejected )
				if( writeRejected && rejE < rejEnd )
					rejected[rejE] = uvec4( slot, dag.x + c, depth, 0u );
@end
				++rejE;
				continue;
			}
			if( mode == 0u )
				continue;   // nothing of the object drawn: only its rejected entries
		}
		if( rec < recEnd )
		{
			records[rec] = uvec4( slot, dag.x + c, idx, depth );
@property( cut_probe )
			// THE PROBE'S CLASS BITS (ATOM-8 stage 2): per record of this list, after the group records - the sky
			// class (bit 2r) and the depth-slack class (bit 2r + 1) of the cluster the record draws.
			if( sky || slack )
			{
				uint bit = 2u * rec;
				uint at = 1u + 2u * probe[0].y + ( bit >> 7u );
				uint lane4 = ( bit >> 5u ) & 3u;
				uint mask = ( sky ? 1u : 0u ) << ( bit & 31u ) | ( slack ? 2u : 0u ) << ( bit & 31u );
				if( at < uint( probe.length() ) )
				{
					if( lane4 == 0u ) atomicOr( probe[at].x, mask );
					else if( lane4 == 1u ) atomicOr( probe[at].y, mask );
					else if( lane4 == 2u ) atomicOr( probe[at].z, mask );
					else atomicOr( probe[at].w, mask );
				}
			}
@end
		}
		idx += range.y;
		++rec;
	}
}
