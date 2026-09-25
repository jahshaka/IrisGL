// Jahshaka — ATOM P3's CULL, job 1 of 3: TEST (SPECS/atom/A4_SUBSTRATE_CULL_DESIGN.md
// section 1.2). ONE THREAD PER INSTANCE of the GPU scene's table.
//
// It answers two questions per instance and writes both, and it is the ONLY job
// of the three that looks at the scene at all: the compaction and the draw
// writer read what this one decided.
//
//   1. IS IT IN? flag predicates, then the six frustum planes against the world
//      AABB the table carries, then — when the request asks for it — the
//      hierarchical depth test against the pyramid.
//   2. WHICH LEVEL? the quality currency's level walk (jahshaka/engine/Types.h,
//      `allowedWorldError` + `lodLevelForWorldError`), evaluated here on the
//      device. THIS IS THE GLSL HALF OF A PAIR and `engine.lod_rule_parity`
//      holds it to the C++ half to 1e-4 over 10,000 triples; a change here that
//      is not made there fails that suite and nothing else, which is exactly
//      why the suite exists.
//
// THE TABLE'S LAYOUT IS A CONTRACT (irisgl/engine/src/GpuScene.h): the world
// transform is THREE vec4 ROWS and not a mat3x4 — a GLSL matrix in std430 is
// COLUMN-major, so reading it as a matrix transposes it silently and correctly
// for a pure translation, which is the worst way to find out. The mesh index
// and the flags word are bit-cast into the w lanes of the two bounds vectors so
// one fetch gives a consumer the bounds and the thing it needs to decide with.
//
// THE DEPTH CONVENTION IS PASSED IN, never assumed: this engine runs reverse-Z
// (near = 1, far = 0), so the CLOSEST depth of a footprint is its MAXIMUM and
// the pyramid stores maxima. `reverseZ` carries it from C++ so a build that
// turns reverse depth off tests the other way instead of inverting the answer.
@insertpiece( SetCrossPlatformSettings )

// ---- the request ---------------------------------------------------------
// One small buffer, written once per request by the consumer. Rows, not a
// matrix, for the reason the table's own world transform is rows.
struct CullParams
{
	vec4  planes[6];        // inward-pointing, normalised (a, b, c, d)
	vec4  viewProjRow[4];   // ROW i in element i
	vec4  eye;              // xyz the camera position; w the LOD switch band (0 = none)
	vec4  lod;              // x tolerance (samples), y proj[1][1], z viewport height, w 1 = orthographic
	uvec4 counts;           // x instanceCount, y flagsRequired, z flagsForbidden, w mode
	uvec4 hzb;              // x levels (0 = frustum only), y width, z height, w reverseZ
};

struct GpuInstance
{
	vec4  world[3];
	vec4  prevWorld[3];
	vec4  boundsMin;        // xyz world AABB min; w = the mesh table index, bit-cast
	vec4  boundsMax;        // xyz world AABB max; w = the flags word, bit-cast
	uvec4 ids;
	uvec4 raster;       // x = the PBS material word, y = the tangent byte offset (GpuScene.h)
};

// 48 bytes since ATOM P4b: `positionAddress`/`indexAddress` are DELETED. One index
// address per MESH cannot name a LEVEL's indices (each level is its own
// IndexBufferPacked), so the addresses moved to the geometry ROW table, one row per
// (mesh, level, submesh), reached through GpuMeshLevel::geomRow.
struct GpuMesh
{
	uvec4 counts;           // x vertices, y level-0 indices, z LEVEL COUNT, w submeshes
	vec4  localBoundsMin;
	vec4  localBoundsMax;   // w = the level-0 bound, which is 0 by definition
};

// 32 bytes since ATOM P4b: the partition range beside the index range.
struct GpuMeshLevel
{
	uint  firstIndex;
	uint  indexCount;
	float bound;            // the level's MEASURED deviation, in MESH units
	uint  geomRow;          // the GEOMETRY ROW of submesh 0; submesh s is geomRow + s
	uint  partBase;
	uint  partCount;
	uint  pad0;
	uint  pad1;
};

layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { CullParams params; };
layout( std430, ogre_U1 ) readonly restrict buffer instLayout { GpuInstance instances[]; };
layout( std430, ogre_U2 ) readonly restrict buffer meshLayout { GpuMesh meshes[]; };
layout( std430, ogre_U3 ) readonly restrict buffer levelLayout { GpuMeshLevel levels[]; };
layout( std430, ogre_U4 ) writeonly restrict buffer visLayout { uint visible[]; };
layout( std430, ogre_U5 ) writeonly restrict buffer lvlLayout { uint outLevel[]; };
layout( std430, ogre_U6 ) restrict buffer heldLayout { uint heldLevel[]; };

// THE PYRAMID THIS READS IS A FARTHEST-DEPTH CHAIN, not a closest-depth one, and
// the difference is the whole correctness of the test (ATOM-SUBSTRATE-1 fix
// round): a level must hold the FARTHEST depth of its footprint, so that
// "everything under this rectangle is nearer than me" is a sound conclusion. A
// closest-depth level makes a texel that is half wall and half sky report the
// wall, and an object visible through the sky half is then culled. The chain's
// reduce direction is a property of its job (`hzb_farthest`) and the engine
// builds the farthest chain for a cull; a screen-space trace wanting the
// closest chain asks for its own build.
//
// THE PYRAMID IS A PERMUTATION, not an always-bound slot: a job that declares a
// texture unit and has nothing in it segfaults in Ogre's own descriptor-set
// validity check (HlmsComputeJob::_calculateNumThreadGroupsBasedOnSetting ->
// HlmsManager::getDescriptorSetTexture2 -> checkValidity, measured 2026-09-22),
// and binding a dummy texture to satisfy it would be a lie in the root layout.
// Both permutations keep FIXED-SIZE bindings, which is what the microcode cache
// requires (ogre-patch 0063 skips a shader that reflects ARRAY bindings).
@property( cull_hzb )
	vulkan_layout( ogre_t0 ) uniform texture2D hzbTexture;
@end

// Levels per mesh in the range table (GpuScene::kLevelsPerMesh).
#define JAH_LEVELS_PER_MESH 8u

@insertpiece( JahLevelRuleScale )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// ---- THE QUALITY CURRENCY, GLSL half (Types.h has the C++ half) -----------
// jahSampleFootprint + jahAllowedWorldError: ONE definition, in
// JahLevelRule_piece_cs.any, shared with the voxel gather and the cluster cut.
@insertpiece( JahLevelRuleCurrency )

// THE LEVEL WALK, identical to lodLevelForWorldError: the COARSEST level whose
// bound is STRICTLY below what the consumer affords, the bounds being
// non-decreasing so the first failure ends the walk.
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

vec4 jahClipOf( vec3 p )
{
	vec4 h = vec4( p, 1.0 );
	return vec4( dot( params.viewProjRow[0], h ), dot( params.viewProjRow[1], h ),
				 dot( params.viewProjRow[2], h ), dot( params.viewProjRow[3], h ) );
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

	visible[slot] = 0u;
	outLevel[slot] = 0u;

	// ---- the predicates ---------------------------------------------------
	if( ( flags & params.counts.y ) != params.counts.y )
		return;
	if( ( flags & params.counts.z ) != 0u )
		return;
	if( meshIndex == 0xFFFFFFFFu )
		return;

	// ---- the frustum, against the world AABB ------------------------------
	// The POSITIVE VERTEX only: the corner furthest along the plane's normal.
	// If that one is behind the plane the whole box is.
	for( int i = 0; i < 6; ++i )
	{
		vec4 pl = params.planes[i];
		vec3 pv = vec3( pl.x >= 0.0 ? bMax.x : bMin.x,
						pl.y >= 0.0 ? bMax.y : bMin.y,
						pl.z >= 0.0 ? bMax.z : bMin.z );
		if( dot( pl.xyz, pv ) + pl.w < 0.0 )
			return;
	}

	// ---- the level (mode >= 1 reads it; it costs one walk either way) -----
	vec3 centre = 0.5 * ( bMin.xyz + bMax.xyz );
	// The largest axis scale of the instance's transform - the longest COLUMN of
	// the row-major 3x4 (JahLevelRule_piece_cs.any says why not a row).
	float scale = jahWorldMaxAxisScale( instances[slot].world[0], instances[slot].world[1],
										instances[slot].world[2] );

	uint levelCount = meshes[meshIndex].counts.z;
	if( params.lod.x > 0.0 && scale > 0.0 && levelCount > 1u )
	{
		vec3 lMin = meshes[meshIndex].localBoundsMin.xyz;
		vec3 lMax = meshes[meshIndex].localBoundsMax.xyz;
		float worldRadius = 0.5 * length( lMax - lMin ) * scale;
		// ORTHOGRAPHIC: no distance term (the CPU strategy's ortho case) - one metre
		// makes the currency's footprint 2 / (proj11 * H), the window over the target.
		float d = params.lod.w > 0.5 ? 1.0 : max( 0.0, distance( centre, params.eye.xyz ) - worldRadius );
		float allowed = jahAllowedWorldError(
			params.lod.x, jahSampleFootprint( d, params.lod.y, params.lod.z ), scale );
		uint level = jahLevelForAllowed( meshIndex, levelCount, allowed );
		// THE SWITCH BAND (ogre-patch 0075's LodStrategy::lodSet, on the GPU): held is
		// this list's last BANDED level for the slot (0xFFFFFFFF = none, and so no
		// band on the first sight). The band is measured on the threshold being
		// crossed - the next level's bound going coarser, the held level's own going
		// finer - and a value past it moves, however many levels at once.
		// THE STATE BELONGS TO THE OBJECT, NOT THE SLOT: the table is swap-on-remove,
		// so a slot's held level is kept with the node id and mesh it was chosen for,
		// and another object renumbered into the slot (or a mesh swapped under it)
		// starts with no band - Ogre's mHysteresisLod = 0xFF on a new object.
		if( params.eye.w > 0.0 )
		{
			uint heldAt = slot * 3u;
			uint held = heldLevel[heldAt];
			if( heldLevel[heldAt + 1u] != instances[slot].ids.x || heldLevel[heldAt + 2u] != meshIndex )
				held = 0xFFFFFFFFu;
			if( level != held && held < levelCount )
			{
				uint base = meshIndex * JAH_LEVELS_PER_MESH;
				float threshold = level > held ? levels[base + held + 1u].bound : levels[base + held].bound;
				float band = abs( threshold ) * params.eye.w;
				if( level > held ? ( allowed < threshold + band ) : ( allowed > threshold - band ) )
					level = held;
			}
			heldLevel[heldAt] = level;
			heldLevel[heldAt + 1u] = instances[slot].ids.x;
			heldLevel[heldAt + 2u] = meshIndex;
		}
		outLevel[slot] = level;
	}

	// ---- the hierarchical depth test --------------------------------------
@property( cull_hzb )
	if( params.hzb.x > 0u )
	{
		// The eight corners, projected. A box that crosses the near plane
		// (any w at or below zero) is NOT tested: its screen rectangle is not
		// a rectangle, and a hierarchical test that guesses there rejects
		// something visible, which is the one error it may never make.
		vec2 lo = vec2( 1.0e30 );
		vec2 hi = vec2( -1.0e30 );
		float nearest = params.hzb.w != 0u ? -1.0e30 : 1.0e30;
		bool testable = true;
		for( int c = 0; c < 8; ++c )
		{
			vec3 p = vec3( ( c & 1 ) != 0 ? bMax.x : bMin.x,
						   ( c & 2 ) != 0 ? bMax.y : bMin.y,
						   ( c & 4 ) != 0 ? bMax.z : bMin.z );
			vec4 clip = jahClipOf( p );
			if( clip.w <= 0.0 )
			{
				testable = false;
				break;
			}
			vec3 ndc = clip.xyz / clip.w;
			lo = min( lo, ndc.xy );
			hi = max( hi, ndc.xy );
			nearest = params.hzb.w != 0u ? max( nearest, ndc.z ) : min( nearest, ndc.z );
		}

		if( testable )
		{
			vec2 size = vec2( float( params.hzb.y ), float( params.hzb.z ) );
			// NDC to TEXELS. Ogre's Vulkan viewport has a NEGATIVE height, so
			// clip-space y is UP and the flip lives in the viewport transform;
			// the pyramid's mip 0 is a copy of the depth ATTACHMENT, whose row
			// 0 is the top. So y inverts here and x does not.
			vec2 t0 = vec2( ( lo.x * 0.5 + 0.5 ) * size.x, ( 0.5 - hi.y * 0.5 ) * size.y );
			vec2 t1 = vec2( ( hi.x * 0.5 + 0.5 ) * size.x, ( 0.5 - lo.y * 0.5 ) * size.y );
			vec2 rect = max( t1 - t0, vec2( 0.0 ) );
			// THE MIP WHERE THE RECTANGLE SPANS AT MOST 2 TEXELS PER AXIS, so
			// the four CORNER fetches below cover every texel it touches.
			// (`/4.0` here would pick a level where the rectangle is 4 texels
			// wide and the four corners leave up to 12 of its 16 texels
			// unsampled — a hierarchical test that reads only the corners of
			// its own footprint can reject something visible in the middle.)
			float widest = max( rect.x, rect.y );
			int mip = int( ceil( log2( max( widest, 1.0 ) / 2.0 ) ) );
			mip = clamp( mip, 0, int( params.hzb.x ) - 1 );
			ivec2 mipSize = ivec2( max( ivec2( params.hzb.yz ) >> mip, ivec2( 1 ) ) );
			ivec2 a = clamp( ivec2( floor( t0 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );
			ivec2 b = clamp( ivec2( floor( t1 / float( 1 << mip ) ) ), ivec2( 0 ), mipSize - 1 );

			// THE FARTHEST DEPTH ANYWHERE UNDER THE RECTANGLE is the one to
			// beat. Each texel already holds the farthest of ITS footprint, so
			// over the four it is the farthest of those — the smallest value
			// under reverse-Z. Anything nearer than that, anywhere under the
			// rectangle, would be a surface this object could still be seen
			// past, which is why the extreme and not an average is right.
			float farthestUnder = params.hzb.w != 0u ? 1.0e30 : -1.0e30;
			for( int yy = 0; yy < 2; ++yy )
			{
				for( int xx = 0; xx < 2; ++xx )
				{
					ivec2 uv = ivec2( xx == 0 ? a.x : b.x, yy == 0 ? a.y : b.y );
					float d = texelFetch( hzbTexture, uv, mip ).x;
					farthestUnder = params.hzb.w != 0u ? min( farthestUnder, d )
													  : max( farthestUnder, d );
				}
			}
			// OCCLUDED when even the box's NEAREST point is farther than the
			// FARTHEST thing drawn under it: every pixel of the rectangle then
			// already holds something in front of this object. In reverse-Z
			// farther is smaller.
			bool occluded = params.hzb.w != 0u ? ( nearest < farthestUnder )
											   : ( nearest > farthestUnder );
			if( occluded )
				return;
		}
	}
@end

	visible[slot] = 1u;
}
