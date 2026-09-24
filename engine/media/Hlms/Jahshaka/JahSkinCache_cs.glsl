// Jahshaka - THE GPU SKIN CACHE (PHOTON-SKIN-1, RY-R4; SkinCache.h).
//
// ONE THREAD PER VERTEX of every rigged item whose pose moved this frame; the
// workgroup's Y names the item (one job record each). It reads the MESH's
// bind-pose vertex through the GPU scene's geometry row (the same row the
// voxeliser reads - JahGeomRows), skins it with the item's bone palette and
// writes the posed vertex into the item's SKIN CACHE: a second vertex buffer in
// the raster's own 48-byte layout (float3 position, float3 normal, float4
// tangent, float2 uv) that the ray tier builds the item's bottom-level structure
// from and the GPU scene's row override names. The Item's own VAO is untouched:
// Ogre's vertex shader keeps skinning the picture (one truth), this is the copy
// the rays, the hit decode and any row reader see.
//
// THE ARITHMETIC IS OGRE'S VERTEX SHADER'S (Hlms/Pbs/Any/Main/800.VertexShader_
// piece_vs.any, the SkeletonTransform piece): four influences, a plain weighted
// sum of 3x4 row-major bone matrices with NO renormalisation (the weights were
// normalised once at upload, OgreMesh.cpp), the normal through the same 3x3 (the
// pin's default - no accurate non-uniform normal scaling) and normalised, the
// tangent through the same 3x3 with its handedness kept. The one difference is
// the SPACE: the palette the host uploads is Ogre's own bone matrices taken back
// into the item's LOCAL space (the node's inverse world applied once per bone on
// the CPU), because a bottom-level structure is built in object space and the
// top-level instance carries the node's world transform - so a character that
// walks without changing pose costs no skin pass at all.
//
// No Hlms directive mark appears in any comment of this file.
@insertpiece( SetCrossPlatformSettings )

@piece( CustomGlslExtensions )
	#extension GL_EXT_buffer_reference: require
	#extension GL_EXT_buffer_reference_uvec2: require
@end

@insertpiece( JahGeomRows )

// Mirrors detail::SkinJobRecord (SkinCache.h), 32 bytes.
struct SkinJob
{
	uvec4 a;	// x source geometry row (the MESH's level 0, submesh 0), y vertex count,
				// z palette base (in vec4 rows), w cache address low
	uvec4 b;	// x cache address high, y source tangent byte offset (0xFFFFFFFF none),
				// z source blend-index byte offset (low 16) and blend-weight byte
				// offset (high 16), w the palette's bone count
};

layout( std430, ogre_U0 ) readonly restrict buffer jobLayout { SkinJob jobs[]; };
layout( std430, ogre_U1 ) readonly restrict buffer geomLayout { GeometryRow geometryTable[]; };
layout( std430, ogre_U2 ) readonly restrict buffer paletteLayout { vec4 palette[]; };

layout( buffer_reference, std430, buffer_reference_align = 4 ) writeonly buffer SkinOutRef
{
	float v[];
};

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// The raster layout's float lanes (OgreMesh.cpp's `V`): 12 floats per vertex.
#define SKIN_OUT_STRIDE_FLOATS 12u
#define JAH_NONE 0xFFFFFFFFu

void main()
{
	const SkinJob job = jobs[gl_WorkGroupID.y];
	const uint vtx = gl_GlobalInvocationID.x;
	if( vtx >= job.a.y )
		return;

	const GeometryRow row = geometryTable[job.a.x];
	// A row the table never staged reads zero addresses; the host never queues a
	// job for one, and this is the second lock on that door (a zero address is a
	// device loss, never a validation error).
	if( ( row.addresses.x | row.addresses.y ) == 0u )
		return;

	const vec3 pos = geomPosition( row, vtx );
	const vec3 nrm = geomNormal( row, vtx );
	const vec2 uv = geomUv( row, vtx );

	GeomFloatRef srcF = GeomFloatRef( row.addresses.xy );
	GeomUintRef srcU = GeomUintRef( row.addresses.xy );
	vec4 tan = vec4( 1.0, 0.0, 0.0, 1.0 );
	if( job.b.y != JAH_NONE )
	{
		const uint lane = GEOM_LANE( row, vtx, job.b.y );
		tan = vec4( srcF.v[lane], srcF.v[lane + 1u], srcF.v[lane + 2u], srcF.v[lane + 3u] );
	}
	// VET_UBYTE4 blend indices (one word) and VET_FLOAT4 weights.
	const uint packedIdx = srcU.v[GEOM_LANE( row, vtx, job.b.z & 0xFFFFu )];
	const uint wLane = GEOM_LANE( row, vtx, job.b.z >> 16u );
	const uint lastBone = max( job.b.w, 1u ) - 1u;
	const vec4 weights = vec4( srcF.v[wLane], srcF.v[wLane + 1u], srcF.v[wLane + 2u], srcF.v[wLane + 3u] );

	vec3 outPos = vec3( 0.0 );
	vec3 outNrm = vec3( 0.0 );
	vec3 outTan = vec3( 0.0 );
	for( uint k = 0u; k < 4u; ++k )
	{
		// THE SECOND LOCK. The first is at attach: OgreScene::attachSkinnedMesh
		// refuses a mesh whose largest blend index is past the blend-index map
		// (OgreSkeleton.cpp), so the raster and this cache never draw two
		// different wrong pictures. Clamped here anyway into THIS item's palette:
		// an index that got past it reads the item's last bone, never the next
		// item's rows.
		const uint bone = min( ( packedIdx >> ( 8u * k ) ) & 0xFFu, lastBone );
		const float w = weights[k];
		const uint base = job.a.z + bone * 3u;
		const vec4 r0 = palette[base + 0u];
		const vec4 r1 = palette[base + 1u];
		const vec4 r2 = palette[base + 2u];
		const vec4 p = vec4( pos, 1.0 );
		outPos += vec3( dot( r0, p ), dot( r1, p ), dot( r2, p ) ) * w;
		outNrm += vec3( dot( r0.xyz, nrm ), dot( r1.xyz, nrm ), dot( r2.xyz, nrm ) ) * w;
		outTan += vec3( dot( r0.xyz, tan.xyz ), dot( r1.xyz, tan.xyz ), dot( r2.xyz, tan.xyz ) ) * w;
	}
	const float nl = length( outNrm );
	outNrm = nl > 0.0 ? outNrm / nl : vec3( 0.0, 1.0, 0.0 );
	const float tl = length( outTan );
	outTan = tl > 0.0 ? outTan / tl : vec3( 1.0, 0.0, 0.0 );

	SkinOutRef dst = SkinOutRef( uvec2( job.a.w, job.b.x ) );
	const uint o = vtx * SKIN_OUT_STRIDE_FLOATS;
	dst.v[o + 0u] = outPos.x;
	dst.v[o + 1u] = outPos.y;
	dst.v[o + 2u] = outPos.z;
	dst.v[o + 3u] = outNrm.x;
	dst.v[o + 4u] = outNrm.y;
	dst.v[o + 5u] = outNrm.z;
	dst.v[o + 6u] = outTan.x;
	dst.v[o + 7u] = outTan.y;
	dst.v[o + 8u] = outTan.z;
	dst.v[o + 9u] = tan.w;
	dst.v[o + 10u] = uv.x;
	dst.v[o + 11u] = uv.y;
}
