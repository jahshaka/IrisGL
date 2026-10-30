// THE HALF-AXES OF A LEVEL-0 VOXEL (VOXEL-R13; SPECS/VOXEL_R13_SPEC.md) - one text for the voxeliser that writes a half's
// own faces, the light injection and the bounce that light and gather each half from its own faces, the step that builds
// the directional chain from the halves, and the ray tier's gather (rq_voxel_sides.comp).
//
// A HALF q = h x 3 + a holds the faces of a voxel looking +a (h 0, the P half) or -a (h 1, the N half), each counted by
// its coverage contribution along a (VoxelMerge_piece_cs.any, THE DIRECTIONAL COVERAGE). The light of a voxel is stored
// PER HALF at level 0 (its six half lights; PhotonVoxelLighting's half store), each lit and gathered FROM ITS OWN FACES:
// along a where they lie (the surface position per half), ACROSS a where their centroid lies, along THEIR OWN mean
// normal. A voxel holding a corner of a closed box puts the box's outer face and its inner face in DIFFERENT halves
// whenever they look different ways (the sealed room's walls, roof and floor: the six axis directions), so no inside face
// shares a light with an outside one there. (The two-side store paired the halves by the sign of one canonical normal and
// mixed a roof's sky-lit top with the room's inner wall face in every cell holding the roof's edge: VOXEL-R13 stage 0,
// spikes/voxel-r13/stage0.) WHAT A HALF CANNOT TELL APART: two faces looking the SAME way in one cell (a floor's top and
// the ground's top 0.4 m below it, in a 1.875 m cell) - the halves' named limit.
//
// This file is plain Vulkan GLSL, read by glslang (the ray jobs' include path) and wrapped into the Hlms piece
// JahVoxelHalves (irisgl/engine/CMakeLists.txt): NO CHARACTER OF IT MAY BE THE HLMS DIRECTIVE MARK, comments included.

#ifndef JAH_VOXEL_HALVES_GLSL
#define JAH_VOXEL_HALVES_GLSL

/// The two other axes of axis a, in increasing order: the across-axis coordinates of a half's centroid and normal.
ivec2 jahHalfOtherAxes( int a )
{
	return a == 0 ? ivec2( 1, 2 ) : ( a == 1 ? ivec2( 0, 2 ) : ivec2( 0, 1 ) );
}

/// THE HALF STORE's TEXEL of cell `cell`'s half (h, axis a): ONE volume six times the voxels wide, the directional
/// volumes' packing generalised to the six halves - axis a's -a half (h 1) in [2a W, (2a + 1) W), its +a half (h 0) in
/// [(2a + 1) W, (2a + 2) W); `width` W, the voxel volume's own width.
ivec3 jahHalfTexel( ivec3 cell, int h, int a, int width )
{
	return ivec3( cell.x + ( 2 * a + ( h == 0 ? 1 : 0 ) ) * width, cell.y, cell.z );
}

/// ...and a FILTERED read's coordinate in it: `u` a point in the voxel volume's normalised box, its x kept a half texel
/// inside the volume (a bilinear tap never reaches the next half's block), mapped into half (h, axis a)'s block.
vec3 jahHalfUvw( vec3 u, int h, int a, float invWidth )
{
	return vec3( ( clamp( u.x, 0.5 * invWidth, 1.0 - 0.5 * invWidth ) + float( 2 * a + ( h == 0 ? 1 : 0 ) ) ) *
					 ( 1.0 / 6.0 ),
				 u.y, u.z );
}

/// THE HALF'S NORMAL from its stored HEMISPHERE OCTAHEDRON: `enc` the two unorm coordinates (0.5 0.5 the half's own
/// direction), the half's direction the pole - unit, in the world's axes. The octahedron keeps the step even over the
/// hemisphere (8 bits: about half a degree near the pole).
vec3 jahHalfNormalDecode( vec2 enc, int a, int h )
{
	const vec2 uv = enc * 2.0 - 1.0;
	const ivec2 o = jahHalfOtherAxes( a );
	vec3 n = vec3( 0.0 );
	n[a] = max( 1.0 - abs( uv.x ) - abs( uv.y ), 0.0 ) * ( h == 0 ? 1.0 : -1.0 );
	n[o.x] = uv.x;
	n[o.y] = uv.y;
	const float l = length( n );
	if( !( l > 1e-6 ) )
	{
		n = vec3( 0.0 );
		n[a] = h == 0 ? 1.0 : -1.0;
		return n;
	}
	return n * ( 1.0 / l );
}

/// The stored pair of half h (0 the +a half: rg, 1 the -a half: ba) of an axis's normal or centroid texel.
vec2 jahHalfNrmPick( vec4 enc, int h )
{
	return h == 0 ? enc.xy : enc.zw;
}

/// ...and its encoding from the half's NORMAL SUM in the half's own frame: `along` the sum's component along the half's
/// direction (never negative: every face of the half looks its way), `across` its two across-axis components (the
/// lower other axis first). No sum: the half's direction (0.5 0.5).
vec2 jahHalfNormalEncode( float along, vec2 across )
{
	const float l1 = max( along, 0.0 ) + abs( across.x ) + abs( across.y );
	if( !( l1 > 0.0 ) )
		return vec2( 0.5 );
	return clamp( across * ( 0.5 / l1 ) + 0.5, vec2( 0.0 ), vec2( 1.0 ) );
}

/// THE HALF'S FACE POINT in the cell's own coordinates [0, 1]^3: along its axis a where its faces lie (`pO` / `o`, the
/// surface position per half and its coverage), across a where their centroid lies (`side`, the two across-axis
/// coordinates, the lower other axis first). A half with no coverage has no face: the cell's centre.
vec3 jahHalfFacePoint( int a, float o, float pO, vec2 side )
{
	vec3 p = vec3( 0.5 );
	if( !( o > 0.0 ) )
		return p;
	const ivec2 oa = jahHalfOtherAxes( a );
	p[a] = clamp( pO / o, 0.0, 1.0 );
	p[oa.x] = clamp( side.x, 0.0, 1.0 );
	p[oa.y] = clamp( side.y, 0.0, 1.0 );
	return p;
}

/// THE TWO SIDES FROM THE HALVES - VOXEL-R13 STAGE 1 ONLY, deleted with the two-side store in stage 2: the level-0
/// readers still read a front and a back, and they are DERIVED from the halves by the side rule the readers apply - a
/// half's faces belong to the front where they look along the voxel's canonical normal on their axis, to the back where
/// they look against it (a two-sided voxel's), to both at half weight where the normal lies in their plane; every half of
/// a one-sided voxel to the front (its back is the inside of a solid: 0). The weight of half (h, a) in side `side` (0 the
/// front, 1 the back).
float jahHalfSideWeight( vec3 n, bool twoSided, int h, int a, int side )
{
	if( !twoSided )
		return side == 0 ? 1.0 : 0.0;
	const float along = ( h == 0 ? 1.0 : -1.0 ) * n[a];
	if( along > 0.0 )
		return side == 0 ? 1.0 : 0.0;
	if( along < 0.0 )
		return side == 1 ? 1.0 : 0.0;
	return 0.5;
}

#endif   // JAH_VOXEL_HALVES_GLSL
