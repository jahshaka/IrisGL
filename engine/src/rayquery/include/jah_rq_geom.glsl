// A RAY'S HIT, AS A SURFACE — its GEOMETRIC normal, rebuilt from the triangle
// the ray committed to (PHOTON-CARDS-2 fix round, audit F3).
//
// WHY: the card read picks the card that FACES the hit surface
// (jah_rq_card.glsl). A committed hit carries no normal (VK_KHR_ray_tracing_
// position_fetch would; the fork does not enable it), and the reversed ray is
// not the normal: within two card texels of a box's edge, at more than 45
// degrees of incidence, the NEIGHBOUR face's card faces the ray more squarely
// and wins — a stripe with the other face's lighting — and past 87 degrees the
// true card fails the facing test outright.
//
// HOW: the GPU scene's GEOMETRY ROWS (GpuScene::geomBuffer — one row per
// mesh, level and submesh, the device addresses of the vertex and index
// buffers THE RASTER DRAWS FROM plus the vertex layout: Ogre's
// VctVoxelizer::GeometryRow, 48 bytes, three uvec4 lanes) and a per-slot table
// the ray tier writes as it writes the TLAS (the row of the level the slot's
// NEAR copy was built from, OgreRayQuery.cpp's writeRayInstances). The hit's
// geometry index is the submesh (rows of one level are contiguous), its
// primitive index the triangle; three indices, three positions, the object-to-
// world transform the ray query hands back, one cross product. A row the scene
// never staged reads zero addresses and the function says so.
//
// THE DECODE IS THE VOXELISER'S (Samples/Media/VCT/VoxelGeometry_piece_cs.any,
// geomIndex / geomPosition — the same row, the same 4-byte-aligned uint and
// float references built from uvec2 addresses): that piece is Hlms media and
// this file is compiled offline by glslang, so the few lines are the second
// copy of that decode, stated.
//
// HOW A CALLER BINDS IT: JAH_GEOM_SLOTS (uint, entries in the per-slot table),
// JAH_GEOM_ROW_OF(slot) (uint, 0xFFFFFFFF = no row), JAH_GEOM_LANE(row, k)
// (uvec4, lane k = 0..2 of a row). The caller enables GL_EXT_buffer_reference
// and GL_EXT_buffer_reference_uvec2.
//
// No Hlms directive mark anywhere in this file.

#ifndef JAH_RQ_GEOM_GLSL
#define JAH_RQ_GEOM_GLSL

layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahGeomFloats
{
	float v[];
};
layout( buffer_reference, std430, buffer_reference_align = 4 ) readonly buffer JahGeomUints
{
	uint v[];
};

uint jahGeomIndex( uvec4 addr, uvec4 layout1, uint element )
{
	// layout1.y: index ELEMENTS skipped by flooring the address to 4 bytes.
	element += layout1.y;
	JahGeomUints idx = JahGeomUints( addr.zw );
	if( ( layout1.x & 1u ) != 0u )
		return idx.v[element];
	const uint packed = idx.v[element >> 1u];
	return ( ( element & 1u ) != 0u ) ? ( packed >> 16u ) : ( packed & 0xFFFFu );
}

vec3 jahGeomPosition( uvec4 addr, uvec4 layout0, uint vertex )
{
	JahGeomFloats pos = JahGeomFloats( addr.xy );
	const uint lane = ( vertex * layout0.x + layout0.y ) >> 2u;
	return vec3( pos.v[lane], pos.v[lane + 1u], pos.v[lane + 2u] );
}

/// The hit triangle's geometric normal in WORLD space, turned to face the ray's
/// origin (against `dir`). False when the slot has no geometry row.
bool jahHitGeometricNormal( uint slot, uint geometryIndex, uint primitive, mat4x3 objectToWorld,
							vec3 dir, out vec3 n )
{
	n = -dir;
	if( slot >= JAH_GEOM_SLOTS )
		return false;
	uint row = JAH_GEOM_ROW_OF( slot );
	if( row == 0xFFFFFFFFu )
		return false;
	row += geometryIndex;
	const uvec4 addr = JAH_GEOM_LANE( row, 0u );
	const uvec4 layout0 = JAH_GEOM_LANE( row, 1u );
	const uvec4 layout1 = JAH_GEOM_LANE( row, 2u );
	if( ( addr.x | addr.y ) == 0u || ( addr.z | addr.w ) == 0u )
		return false;
	vec3 w[3];
	for( uint k = 0u; k < 3u; ++k )
	{
		const uint vtx = jahGeomIndex( addr, layout1, primitive * 3u + k );
		w[k] = objectToWorld * vec4( jahGeomPosition( addr, layout0, vtx ), 1.0 );
	}
	const vec3 c = cross( w[1] - w[0], w[2] - w[0] );
	const float len = length( c );
	if( !( len > 1e-12 ) )
		return false;
	n = c / len;
	if( dot( n, dir ) > 0.0 )
		n = -n;
	return true;
}

#endif   // JAH_RQ_GEOM_GLSL
