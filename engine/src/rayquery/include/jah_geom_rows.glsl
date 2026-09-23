// THE GEOMETRY ROW DECODE — the one copy (PHOTON-CARDS-2; moved out of the fork's
// VCT/VoxelGeometry_piece_cs.any, which it replaces). Read by the voxeliser's two
// jobs (VCT/Voxelizer_piece_cs.any, VCT/AabbCalcultor_piece_cs.any — both insert
// the piece JahGeomRows, which Jahshaka's build WRAPS this file into) and by the
// ray jobs' hit normal (jah_rq_geom.glsl, through GL_GOOGLE_include_directive).
// The caller enables GL_EXT_buffer_reference and GL_EXT_buffer_reference_uvec2;
// the functions take a GeometryRow the caller fetched from its own binding, so
// this file fixes no binding. No Hlms directive mark anywhere in this file.
//
// What the voxeliser's piece said about it, kept:
//
// JAHSHAKA (ATOM P4) - READING THE RASTER'S OWN GEOMETRY FROM A COMPUTE SHADER.
//
// The voxelizer used to be handed PRIVATE COPIES of every mesh it voxelized: the
// host downloaded each vertex buffer to the CPU, unpacked it to eight floats per
// vertex, uploaded that into one big buffer, and copied every index buffer into
// two more. This piece is what replaced all of it. A GEOMETRY ROW holds the
// device addresses of the vertex and index buffers THE RASTER DRAWS FROM, plus
// the vertex layout, and every fetch goes through a buffer reference built from
// that address.
//
// WHY A uvec2 AND NOT A uint64_t: shaderInt64 is not an enabled device feature,
// so GL_EXT_buffer_reference_uvec2 is how a reference is constructed. Verified at
// this pin: both extensions compile through Ogre's GLSL front end with its exact
// settings and emit SPIR-V 1.0 with SPV_KHR_physical_storage_buffer - no SPIR-V
// version raise is needed (unlike subgroup operations).
//
// THE FORMAT IS DATA, NOT A PERMUTATION. The index width and the normal/uv
// packing used to be shader properties, because they described which private
// buffer a dispatch bound. They are fields of a row now, so one job voxelizes a
// 16-bit and a 32-bit mesh in the same dispatch.
//
// This piece is Vulkan GLSL. The HLSL and Metal entry points of these jobs are
// upstream's and are not built in this fork.

#ifndef JAH_GEOM_ROWS_GLSL
#define JAH_GEOM_ROWS_GLSL

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer GeomFloatRef
{
	float v[];
};
layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer GeomUintRef
{
	uint v[];
};

// Mirrors VctVoxelizer::GeometryRow (48 bytes, three uvec4 lanes).
struct GeometryRow
{
	uvec4 addresses;	// posAddrLo, posAddrHi, idxAddrLo, idxAddrHi
	uvec4 layout0;		// vertexStride, posOffset, normalOffset, uvOffset (bytes)
	uvec4 layout1;		// flags, idxBias, pad, pad
};

// Mirrors VctVoxelizer's VoxelizerGeomFlag.
#define GeomFlagIndex32			0x0001u
#define GeomNormalMask			0x0070u
#define GeomNormalNone			0x0000u
#define GeomNormalFloat3		0x0010u
#define GeomNormalShort4Snorm	0x0020u
#define GeomNormalHalf4			0x0030u
#define GeomUvMask				0x0700u
#define GeomUvNone				0x0000u
#define GeomUvFloat2			0x0100u
#define GeomUvHalf2				0x0200u

// A vertex's first float lane, in FLOATS from the buffer's start. Every offset a
// row carries is a byte offset and every lane this engine bakes is 4-byte
// aligned (the buffer_reference_align above says so and nothing may break it).
#define GEOM_LANE( row, vertexIdx, byteOffset ) \
	( ( ( vertexIdx ) * ( row ).layout0.x + ( byteOffset ) ) >> 2u )

uint geomIndex( GeometryRow row, uint indexElement )
{
	// `idxBias` is the index ELEMENTS skipped by flooring the index buffer's
	// address to a 4-byte boundary - a 16-bit buffer can start on an odd uint16
	// and a reference of uints may not.
	uint element = indexElement + row.layout1.y;
	GeomUintRef idxRef = GeomUintRef( row.addresses.zw );
	if( ( row.layout1.x & GeomFlagIndex32 ) != 0u )
		return idxRef.v[element];
	uint packed = idxRef.v[element >> 1u];
	return ( ( element & 0x01u ) != 0u ) ? ( packed >> 16u ) : ( packed & 0xFFFFu );
}

vec3 geomPosition( GeometryRow row, uint vertexIdx )
{
	GeomFloatRef posRef = GeomFloatRef( row.addresses.xy );
	uint lane = GEOM_LANE( row, vertexIdx, row.layout0.y );
	return vec3( posRef.v[lane], posRef.v[lane + 1u], posRef.v[lane + 2u] );
}

vec3 geomNormal( GeometryRow row, uint vertexIdx )
{
	uint fmt = row.layout1.x & GeomNormalMask;
	if( fmt == GeomNormalNone )
		return vec3( 0.0f, 1.0f, 0.0f );	// what the download path fell back to

	uint lane = GEOM_LANE( row, vertexIdx, row.layout0.z );
	if( fmt == GeomNormalFloat3 )
	{
		GeomFloatRef posRef = GeomFloatRef( row.addresses.xy );
		return vec3( posRef.v[lane], posRef.v[lane + 1u], posRef.v[lane + 2u] );
	}

	GeomUintRef rawRef = GeomUintRef( row.addresses.xy );
	uint w0 = rawRef.v[lane];
	uint w1 = rawRef.v[lane + 1u];
	if( fmt == GeomNormalShort4Snorm )
	{
		// A VET_SHORT4_SNORM normal is a QTANGENT, not a vector: Ogre stores the
		// tangent-space rotation as a quaternion and the NORMAL is its X AXIS.
		// This is the same decode VertexBufferDownloadHelper::getNormal did for
		// this element type (OgreVertexBufferDownloadHelper.h, the
		// `Dealing with QTangents` arm: snorm16 x4 -> Quaternion -> xAxis()), and
		// unpackSnorm2x16 is GLSL's spelling of Bitwise::snorm16ToFloat.
		// Returning the quaternion's imaginary part instead - which is what a
		// naive xyz read gives - would be a different vector entirely.
		vec2 qxy = unpackSnorm2x16( w0 );
		vec2 qzw = unpackSnorm2x16( w1 );
		float qx = qxy.x, qy = qxy.y, qz = qzw.x, qw = qzw.y;
		float fTy = 2.0f * qy;
		float fTz = 2.0f * qz;
		return vec3( 1.0f - ( fTy * qy + fTz * qz ),
					   fTy * qx + fTz * qw,
					   fTz * qx - fTy * qw );
	}
	vec2 hxy = unpackHalf2x16( w0 );
	return vec3( hxy.x, hxy.y, unpackHalf2x16( w1 ).x );
}

vec2 geomUv( GeometryRow row, uint vertexIdx )
{
	uint fmt = row.layout1.x & GeomUvMask;
	if( fmt == GeomUvNone )
		return vec2( 0.0f, 0.0f );

	uint lane = GEOM_LANE( row, vertexIdx, row.layout0.w );
	if( fmt == GeomUvFloat2 )
	{
		GeomFloatRef posRef = GeomFloatRef( row.addresses.xy );
		return vec2( posRef.v[lane], posRef.v[lane + 1u] );
	}
	GeomUintRef rawRef = GeomUintRef( row.addresses.xy );
	return unpackHalf2x16( rawRef.v[lane] );
}

#endif   // JAH_GEOM_ROWS_GLSL
