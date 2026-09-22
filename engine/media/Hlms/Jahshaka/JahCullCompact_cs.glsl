// Jahshaka — ATOM P3's CULL, job 2 of 3: COMPACT (design section 1.2).
//
// The visibility words job 1 wrote are a bitmap over the whole table; every pass
// that follows wants a LIST. This job turns one into the other in a single
// dispatch: a workgroup-wide exclusive prefix sum over its own 64 words, ONE
// global atomic per workgroup for the base its survivors start at, and a write
// per survivor.
//
// THE SCAN IS SHARED MEMORY, AND THAT IS A MEASUREMENT, NOT A PREFERENCE.
// Subgroup arithmetic (`subgroupAdd` / `subgroupExclusiveAdd`) is the shorter
// and faster spelling and it DOES NOT COMPILE THROUGH THIS PIN'S GLSL FRONT END:
// VulkanProgram::compile never calls setEnvClient/setEnvTarget for GLSL, so
// glslang defaults to Vulkan 1.0 / SPIR-V 1.0 and answers
// "'subgroup op' : requires SPIR-V 1.3" to every one of them (measured
// 2026-09-22, spikes/atom-substrate-1/glslang-probe). Raising that environment
// is an Ogre patch with a capability query attached, named in the lane's report;
// until it lands there is ONE path here and it is this one, which also happens
// to be the path lavapipe needs.
//
// THE ORDER OF THE LIST IS NOT THE ORDER OF THE TABLE, and no consumer may
// assume it is: the workgroups take their bases by atomic arrival, so slot 900
// can precede slot 5. What IS guaranteed is that the list is exactly the set of
// survivors, with no gap and no repeat, and that `count[0]` is its length.
//
// count[] IS ALSO THE INDIRECT ARGUMENT for job 3: [1..3] are its thread-group
// counts, and every workgroup atomicMax-es [1] with the groups its own tail
// would need. The last arriving workgroup therefore leaves ceil(total / 64)
// there without a second pass and without anybody knowing which one it was.
@insertpiece( SetCrossPlatformSettings )

@property( syntax == glsl )
	#define ogre_U0 binding = 0
	#define ogre_U1 binding = 1
	#define ogre_U2 binding = 2
	#define ogre_U3 binding = 3
@end

// THE WHOLE REQUEST STRUCT, not a uvec4 at offset 0. The first version of this
// file declared `uvec4 limits` and read the instance count out of it, which is
// `planes[0]` reinterpreted — a bug that LOOKS like it works, because a real
// plane's first float has a huge bit pattern and every slot compares below it.
// It shows up only on a request whose planes start at zero (the parity suite's
// permissive frustum), where the count reads 0 and nothing survives. The layout
// is a contract: read it as the contract, not at an offset.
struct CullParams
{
	vec4  planes[6];
	vec4  viewProjRow[4];
	vec4  eye;
	vec4  lod;
	uvec4 counts;           // x instanceCount, y flagsRequired, z flagsForbidden, w mode
	uvec4 hzb;
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { CullParams params; };
layout( std430, ogre_U1 ) readonly restrict buffer visLayout { uint visible[]; };
layout( std430, ogre_U2 ) writeonly restrict buffer survLayout { uint survivors[]; };
layout( std430, ogre_U3 ) restrict buffer cntLayout { uint counter[]; };

#define JAH_SCAN_WIDTH @value( threads_per_group_x )

shared uint gScan[JAH_SCAN_WIDTH];
shared uint gBase;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	uint lane = gl_LocalInvocationID.x;
	uint slot = gl_GlobalInvocationID.x;
	uint mine = ( slot < params.counts.x && visible[slot] != 0u ) ? 1u : 0u;
	gScan[lane] = mine;
	barrier();

	// HILLIS-STEELE, inclusive, in place. Every step reads a value written by
	// the step before it, so both barriers are load-bearing: the read of
	// gScan[lane - off] must see the previous step complete, and the write must
	// not land before every lane has taken its read.
	for( uint off = 1u; off < JAH_SCAN_WIDTH; off <<= 1u )
	{
		uint add = lane >= off ? gScan[lane - off] : 0u;
		barrier();
		gScan[lane] += add;
		barrier();
	}

	uint total = gScan[JAH_SCAN_WIDTH - 1u];
	if( lane == 0u )
	{
		gBase = total != 0u ? atomicAdd( counter[0], total ) : 0u;
		if( total != 0u )
			atomicMax( counter[1], ( gBase + total + JAH_SCAN_WIDTH - 1u ) / JAH_SCAN_WIDTH );
	}
	barrier();

	if( mine != 0u )
		survivors[gBase + gScan[lane] - 1u] = slot;   // inclusive scan minus one = exclusive
}
