// Jahshaka - ATOM P4b's VOXEL GATHER, jobs 1 and 3 of 3 (GpuVoxelGather.h).
// ONE THREAD PER INSTANCE SLOT of the GPU scene's table. The property
// gather_write selects the pass: 0 COUNTS records per (octant, bucket), 1 WRITES
// them into the ranges the scan made, and adds what it wrote to the readout.
// BOTH PASSES MUST DECIDE IDENTICALLY, which is why they are one source: a
// record counted by the first pass and not written by the second would leave a
// hole the voxelize shader reads as a zero record.
//
// THE TABLES' LAYOUTS ARE CONTRACTS (irisgl/engine/src/GpuScene.h): world
// transforms are three vec4 ROWS, never a mat3x4 (a GLSL matrix in std430 is
// column-major, and a transposed read is silently right for a translation). The
// mesh index and the flags word ride bit-cast in the w lanes of the bounds.
//
// THE LEVEL RULE is the quality currency's cascade case: one SAMPLE is a cell,
// the tolerance is kCascadeLodCellFraction of it, and the instance's largest
// axis scale takes it into the units the baked bounds are in. The two functions
// below are the same text as JahCullTest_cs.glsl's, whose copy
// engine.lod_rule_parity holds to the C++ header; THIS copy is held to it by
// gi.cascade_lod's exact per-level counts and gi.voxel_feed's in-test reference.
// THE SCALE IS THE LONGEST COLUMN of the 3x3 (the transformed basis vector),
// which is the node's derived scale for any rotation. The cull's copy takes the
// longest ROW, which agrees only for uniform or axis-aligned scale.
@insertpiece( SetCrossPlatformSettings )

struct VoxelGatherParams
{
	uvec4 counts;       // x instance slots, y octants, z material buckets, w flags
	uvec4 caps;         // x record capacity, y range count, z indices per partition
	vec4  lod;          // x cell, y tolerance, z rule 2's minimum extent
	vec4  octMin[8];
	vec4  octMax[8];
};

struct GpuInstance
{
	vec4  world[3];
	vec4  prevWorld[3];
	vec4  boundsMin;    // xyz world AABB min; w = the mesh table index, bit-cast
	vec4  boundsMax;    // xyz world AABB max; w = the flags word, bit-cast
	uvec4 ids;          // y = the material word (pool, slot)
	uvec4 pad;
};

struct GpuMesh
{
	uvec4 counts;       // x vertices, y level-0 indices, z LEVEL COUNT, w submeshes
	vec4  localBoundsMin;
	vec4  localBoundsMax;
};

struct GpuMeshLevel
{
	uint  firstIndex;
	uint  indexCount;
	float bound;        // the level's MEASURED deviation, in MESH units
	uint  geomRow;      // submesh 0's geometry row
	uint  partBase;
	uint  partCount;
	uint  pad0;
	uint  pad1;
};

struct PartAabb
{
	vec4 centre;        // mesh-local
	vec4 halfSize;
};

// Mirrors InstanceBuffer in Voxelizer_piece_cs.any (VctVoxelizer::kInstanceRecordBytes).
struct Record
{
	vec4  worldRow[3];
	vec4  aabb0;        // xyz world AABB centre
	vec4  aabb1;        // xyz world AABB half size; w = the material SLOT, bit-cast
	uvec4 meshData;     // geometry row, first index, index count, unused
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { VoxelGatherParams params; };
layout( std430, ogre_U1 ) readonly restrict buffer instLayout { GpuInstance instances[]; };
layout( std430, ogre_U2 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( std430, ogre_U3 ) readonly restrict buffer levelLayout { GpuMeshLevel levels[]; };
layout( std430, ogre_U4 ) readonly restrict buffer partLayout { PartAabb parts[]; };
layout( std430, ogre_U5 ) restrict buffer workLayout { uint work[]; };
layout( std430, ogre_U6 ) readonly restrict buffer rangesLayout { uvec2 ranges[]; };
layout( std430, ogre_U7 ) writeonly restrict buffer recordLayout { Record records[]; };
layout( std430, ogre_U8 ) restrict buffer readoutLayout { uint readout[]; };
layout( std430, ogre_U9 ) readonly restrict buffer maskLayout { uint budgetMask[]; };

#define JAH_LEVELS_PER_MESH 8u
#define JAH_NONE 0xFFFFFFFFu
#define JAH_GI_VISIBLE 8u
#define JAH_FLAG_LOD 1u
#define JAH_FLAG_BUDGETED 2u
// The readout's word offsets (GpuVoxelGather.h, VoxelReadoutWord).
#define JAH_READ_INDEX_TOTAL 0u
#define JAH_READ_HISTOGRAM 1u
#define JAH_READ_INSTANCES 17u
#define JAH_READ_CANDIDATES 18u
#define JAH_READ_RECORDS 19u
#define JAH_READ_OVERFLOW 20u

@insertpiece( JahLevelRuleScale )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// What a consumer may afford, in the units the BAKED bounds are measured in.
float jahAllowedWorldError( float tolerance, float footprint, float meshToWorldScale )
{
	if( tolerance <= 0.0 || footprint <= 0.0 || meshToWorldScale <= 0.0 )
		return 0.0;
	return tolerance * footprint / meshToWorldScale;
}

// The COARSEST level whose bound is STRICTLY below what the consumer affords, the
// bounds being non-decreasing so the first failure ends the walk.
uint jahLevelForAllowed( uint meshIndex, uint levelCount, float allowed )
{
	if( allowed <= 0.0 )
		return 0u;
	uint level = 0u;
	uint base = meshIndex * JAH_LEVELS_PER_MESH;
	for( uint i = 1u; i < levelCount && i < JAH_LEVELS_PER_MESH; ++i )
	{
		if( !( levels[base + i].bound < allowed ) )
			break;
		level = i;
	}
	return level;
}

void main()
{
	uint slot = gl_GlobalInvocationID.x;
	if( slot >= params.counts.x )
		return;

	vec4 bMin = instances[slot].boundsMin;
	vec4 bMax = instances[slot].boundsMax;
	uint meshIndex = floatBitsToUint( bMin.w );
	uint flags = floatBitsToUint( bMax.w );

	// ---- the predicates, in the order the CPU feed asked them ------------------
	if( ( flags & JAH_GI_VISIBLE ) == 0u )
		return;
	if( meshIndex == JAH_NONE )
		return;
	uint word = instances[slot].ids.y;
	if( word == JAH_NONE )
		return;                             // the store has not converted it yet
	uint bucket = word >> 16u;
	uint matSlot = word & 0xFFFFu;
	if( bucket >= params.counts.z )
		return;
	// RULE 2: a coarse cascade declines what cannot fill half a voxel of it.
	vec3 ext = bMax.xyz - bMin.xyz;
	if( max( max( ext.x, ext.y ), ext.z ) < params.lod.z )
		return;
	if( ( params.counts.w & JAH_FLAG_BUDGETED ) != 0u &&
		( ( budgetMask[slot >> 5u] >> ( slot & 31u ) ) & 1u ) == 0u )
		return;

	// ---- the level -----------------------------------------------------------
	vec4 r0 = instances[slot].world[0];
	vec4 r1 = instances[slot].world[1];
	vec4 r2 = instances[slot].world[2];
	uint levelCount = meshes[meshIndex].counts.z;
	uint level = 0u;
	if( ( params.counts.w & JAH_FLAG_LOD ) != 0u && levelCount > 1u )
	{
		float scale = jahWorldMaxAxisScale( r0, r1, r2 );
		float allowed = jahAllowedWorldError( params.lod.y, params.lod.x, scale );
		level = jahLevelForAllowed( meshIndex, levelCount, allowed );
	}
	GpuMeshLevel L = levels[meshIndex * JAH_LEVELS_PER_MESH + level];
	if( L.geomRow == JAH_NONE || L.partCount == 0u )
		return;                             // no readable geometry at this level

@property( gather_write )
	atomicAdd( readout[JAH_READ_CANDIDATES], 1u );
	bool wroteAny = false;
@end

	uint perPart = params.caps.z;
	for( uint j = 0u; j < L.partCount; ++j )
	{
		PartAabb pa = parts[L.partBase + j];
		vec4 c = vec4( pa.centre.xyz, 1.0 );
		vec3 h = pa.halfSize.xyz;
		// The partition's world AABB: centre transformed, half size through the
		// absolute rows - conservative, which is all a broadphase needs to be.
		vec3 wc = vec3( dot( r0, c ), dot( r1, c ), dot( r2, c ) );
		vec3 wh = vec3( dot( abs( r0.xyz ), h ), dot( abs( r1.xyz ), h ), dot( abs( r2.xyz ), h ) );
		vec3 wMin = wc - wh;
		vec3 wMax = wc + wh;
		uint first = L.firstIndex + j * perPart;
		uint num = min( L.indexCount - j * perPart, perPart );

@property( gather_write )
		// THE ATTACH SET's reading (Types.h, voxelTriangles and voxelLevels): every
		// candidate partition, in the box or not, exactly as the CPU feed counted.
		atomicAdd( readout[JAH_READ_INDEX_TOTAL], num );
		atomicAdd( readout[JAH_READ_HISTOGRAM + min( level, 15u )], 1u );
@end

		for( uint o = 0u; o < params.counts.y; ++o )
		{
			if( any( greaterThan( wMin, params.octMax[o].xyz ) ) ||
				any( lessThan( wMax, params.octMin[o].xyz ) ) )
				continue;
			uint p = o * params.counts.z + bucket;
@property( !gather_write )
			atomicAdd( work[p], 1u );
@else
			uint cursor = atomicAdd( work[params.caps.y + p], 1u );
			if( cursor >= ranges[p].y )
			{
				// Past the range the scan could give it: the capacity bound was
				// exceeded. DROPPED AND COUNTED, never written out of bounds.
				atomicAdd( readout[JAH_READ_OVERFLOW], 1u );
				continue;
			}
			Record rec;
			rec.worldRow[0] = r0;
			rec.worldRow[1] = r1;
			rec.worldRow[2] = r2;
			rec.aabb0 = vec4( wc, 0.0 );
			rec.aabb1 = vec4( wh, uintBitsToFloat( matSlot ) );
			rec.meshData = uvec4( L.geomRow, first, num, 0u );
			records[ranges[p].x + cursor] = rec;
			atomicAdd( readout[JAH_READ_RECORDS], 1u );
			wroteAny = true;
@end
		}
	}

@property( gather_write )
	if( wroteAny )
		atomicAdd( readout[JAH_READ_INSTANCES], 1u );
@end
}
