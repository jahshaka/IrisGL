// ONE VOXEL RADIANCE READ — one cascade, one point (PHOTON-READER-1; the
// render audit's section 7.1 "ONE VOXEL RADIANCE READER").
//
// THE ONE SOURCE, FOUR CONSUMERS. This file is plain Vulkan GLSL and it is read
// two ways:
//   * glslang's offline path (the ray jobs: rq_reflect, rq_probe_gather) reads
//     it through `#include`, with `-I src/rayquery/include`;
//   * the Hlms paths (the pixel shader's cone march, the bounce-injection job,
//     the irradiance field's generation job) read it as the piece
//     `JahVoxelSample`, which the build WRAPS this file into
//     (irisgl/engine/CMakeLists.txt, `Hlms/Jahshaka/JahVoxelSample_piece_all.any`).
// No hand copy of it exists anywhere. The wrapper makes this file Hlms input,
// so NO CHARACTER OF IT MAY BE THE HLMS DIRECTIVE MARK, comments included (the
// Hlms parser executes that mark wherever it finds it — DOCS/traps/ENGINE.md).
// It includes nothing (the Hlms path has no include): a caller includes
// jah_rq_finite.glsl FIRST (the wrapper puts it in the same piece).
//
// HOW A CALLER BINDS IT. The shader's bindings are its own; this file reads the
// volumes through macros the caller defines before it:
//
//     JAH_VOX_SAMPLE_ISO(c,u,l)  vec4: cascade c's total at uvw u - level 0 (the mean of a
//                                voxel's two sides), the only level a light read takes
//                                from it (its coarse levels exist only for the specular
//                                SDF skip, jah_voxel_march.glsl)
//     JAH_VOX_SAMPLE_X(c,u,l)    vec4: the X-axis directional volume (both
//     JAH_VOX_SAMPLE_Y(c,u,l)    signs packed side by side along u.x, so u.x
//     JAH_VOX_SAMPLE_Z(c,u,l)    runs over [0, 1) and the halves are 0.5 apart) - every
//                                level past 0, at every tier (PHOTON-MARCH-1)
//     JAH_VOX_SAMPLE_COVP(c,u,l) vec4: cascade c's COVERAGE of the faces looking +a (xyz = O_x+,
//     JAH_VOX_SAMPLE_COVN(c,u,l)   O_y+, O_z+) and of those looking -a (PHOTON-VOXEL-3/-4;
//                                VctLighting's coverageIndex(0 / 1))
//     JAH_VOX_SAMPLE_POSP(c,u,l) vec4: cascade c's SURFACE POSITION per half (xyz = O_a x p_a;
//     JAH_VOX_SAMPLE_POSN(c,u,l)   positionIndex(0 / 1)) - the origin plane's test. LEVEL 0 holds p
//                                RELATIVE TO THE TEXEL'S OWN CELL (PHOTON-7 stage 1: the same bits at
//                                every placement), every coarser level p absolute in the cascade's
//                                normalised space; the readers below add a level-0 texel's cell
//     JAH_VOX_INVRES(c)          vec3: 1 / cascade c's resolution, per axis
//     JAH_VOX_HAS_BACK           0 or 1, a PREPROCESSOR constant (PHOTON-VOXEL-5): the level-0
//                                BACK side and the voxeliser's normal are bound (the bounce job
//                                reads the sides' mean and binds neither):
//     JAH_VOX_SAMPLE_BACK(c,u,l) vec4: level 0's back side (premultiplied like the isotropic
//                                volume, which holds the two sides' MEAN - the front is twice
//                                the mean minus the back); VctLighting::backIndex()
//     JAH_VOX_SAMPLE_NRM(c,u,l)  vec4: the voxeliser's normal (xyz biased, the canonical normal of a
//                                two-sided voxel: the front is the side it points to); normalIndex()
//
// THE VOLUME'S DIRECTIONAL READ IS THE ONLY RULE HERE — ONE RULE AT EVERY MIP AND EVERY TIER
// (PHOTON-VOXEL-3; PHOTON-MARCH-1): a step takes the opacity of what it crosses, per axis,
// from the per-axis coverage at mip 0 and from the half of each axis volume composited
// TOWARDS the reader above it - and THE COLOUR THAT COMES WITH AN OPACITY IS THE
// COVERAGE-WEIGHTED RADIANCE OF THE SAME HALF-AXIS FACES THE OPACITY COUNTS (the directional
// store keeps (radiance x k, k) per half-axis: JahDirectionalComposite_piece_cs.any). The
// isotropic store the Low tier kept took the colour as a texel's MEAN radiance over every
// surface in it: a wall of L standing on a plate read 0.998 / 0.954 / 0.873 L at 32 / 64 / 128
// cells of a 9 m cascade (gi.march_underread), 0.999 / 0.996 / 0.990 through this law. The
// pixel's cones, the bounce, the field, the cards and the ray hit read through it; nothing
// else reads a volume.

#ifndef JAH_VOXEL_SAMPLE_GLSL
#define JAH_VOXEL_SAMPLE_GLSL

#ifndef JAH_VOX_HAS_BACK
#define JAH_VOX_HAS_BACK 0
#endif

/// THE LEVEL A READ TAKES FOR A FOOTPRINT 2^lod CELLS WIDE: the level whose TRILINEAR
/// KERNEL is the footprint. A trilinear fetch weighs the texels within one texel of the
/// sample, so its kernel is two texels wide: the level whose texel is half the
/// footprint. (Upstream read the footprint's own level - a kernel twice the footprint.)
/// `texelCells` is the level-0 texel in cells: 1 for the total's level 0, 2 for a
/// directional one (half resolution, both signs packed along x).
float jahVoxelKernelMip( float lod, float texelCells )
{
	return max( lod - 1.0 - log2( texelCells ), 0.0 );
}

/// THE DOMINANT AXIS of a direction in a cascade's normalised space: the axis whose texel
/// planes the ray crosses most often per unit length (the plane march, jah_voxel_march.glsl).
int jahVoxelAxis( vec3 dir, vec3 invRes )
{
	const vec3 perLen = abs( dir ) / invRes;
	return perLen.x >= perLen.y ? ( perLen.x >= perLen.z ? 0 : 2 ) : ( perLen.y >= perLen.z ? 1 : 2 );
}

/// THE ONE READ RULE, AT EVERY LEVEL (PHOTON-VOXEL-3, PHOTON-VOXEL-4): A PLANE TAKES THE
/// SURFACES ITS STRETCH OF THE CONE CROSSES, FROM THE SIDE THEY FACE. The store keeps, per
/// voxel and axis and SIDE, the fraction of the voxel's face its surfaces looking +a (or -a)
/// cover along that axis (the coverage split by the side a face looks to: VoxelMerge_piece_
/// cs.any) and where along that axis they lie (the surface position, O-premultiplied); a
/// directional level keeps, per texel and axis, what a reader crossing the texel's whole depth
/// along that axis sees of it, composited from the half that faces the reader. A ray
/// travelling +a meets the faces looking -a: a closed wall is read ONCE at any cell size and a
/// solid's back face never. One plane of the march (the texel plane of level m across the
/// dominant axis a, jah_voxel_march.glsl) reads:
///
///   THE PLANE'S OWN AXIS a, AT THE PLANE, from the half that looks back at the travel: the
///   texel's full-crossing opacity (level 0: the voxel's O_a; a directional level: alpha_a)
///   times w, the part of the plane's depth not yet read (1 but on a
///   cascade's first plane). Sampled at the plane's centre, the fetch is that texel along a.
///
///   THE OTHER TWO AXES b, THROUGH A KERNEL THE SIZE OF THE CONE'S FOOTPRINT: the coverage
///   chain at the FRACTIONAL level lk = lod - 1 (a texel as wide as the footprint's radius:
///   a bilinear kernel of texel K has the per-axis spread K / 2 of a uniform disc of radius
///   K), each half crossed at THE CONE'S CROSSING RATE toward it over the length the cone
///   spends in the plane: O_b+/- rate+/- len / cell_b. The rate (jahVoxelConeRate) is the
///   b-distance the cone's rays heading that way advance per unit of axial length - the
///   axis's own |d_b| for a ray, and for a cone the mean over its cross-section, so a
///   surface PARALLEL to the axis still meets the rim. Consecutive planes sample the
///   kernel's hat at the plane spacing and the hats sum to one - a surface is read to its
///   full coverage across the planes that straddle it, never the fraction one fetch's blend
///   weight holds. WHAT IT DOES NOT CLOSE: the kernel is centred on the axis, so the rays
///   heading to a surface and the part of the footprint the surface covers are not
///   correlated - the model's own residual, measured per set and cell size on analytic
///   stores (tests/gi/voxellab, SYNTH), shrinking with the aperture.
///
///   THE ORIGIN PLANE IS THE HEMISPHERE'S BOUNDARY, NEVER AN OCCLUDER: along the axis n the
///   surface's normal is dominant on, a surface of either half whose position lies within one
///   fine cell of the origin's depth, or behind it, is not counted - the cone's own floor at
///   any distance and at any level, the underside of the slab it stands on, and nothing
///   else. `org`: x the axis n (-1:
///   no origin plane - a probe ray in free space, a point read), y the origin's coordinate
///   along n in this cascade's normalised space, z the normal's sign along n.
///
/// With the light that comes with it (the plane axis: the texel's premultiplied light; the
/// kernel's axes: the directional volume's half facing the rays that meet it - times what
/// they take); the march composites ADDITIVELY, capped at 1.
///
/// WHY ADDITIVE: a surface split between two voxels puts half its coverage in each, and the
/// halves ADD UP to the surface - opaque. Composited as independent occluders they
/// multiplied to 0.75 (PHOTON-VOXEL-3 run 2: spikes/photon-voxel-3/EVIDENCE.txt).

/// The CDF of a tent of half-width one (support [-1, 1]).
float jahVoxelTentCdf( float u )
{
	const float t = clamp( u, -1.0, 1.0 );
	return t < 0.0 ? 0.5 * ( t + 1.0 ) * ( t + 1.0 ) : 1.0 - 0.5 * ( 1.0 - t ) * ( 1.0 - t );
}

/// THE CONE'S CROSSING RATE toward one side along b: `c` the axis's component toward it
/// (d_b for the rays heading +b, -d_b for -b), `k` = tan x sqrt(1 - d_b^2), the reach of
/// the cone's cross-section along b (a uniform disc of radius tan at unit axial distance,
/// whose points project on b as k X, X of density (2 / pi) sqrt(1 - X^2) on [-1, 1]). The
/// mean of max(0, c + k X): with t = clamp(-c / k), A(x) = (x sqrt(1 - x^2) + asin x) / 2
/// and B(x) = -(1 - x^2)^(3/2) / 3, it is (2 / pi) (c (A(1) - A(t)) + k (B(1) - B(t))).
/// A ray (k = 0): max(0, c).
float jahVoxelConeRate( float c, float k )
{
	if( !( k > 1e-6 ) )
		return max( c, 0.0 );
	const float t = clamp( -c / k, -1.0, 1.0 );
	const float st = sqrt( max( 1.0 - t * t, 0.0 ) );
	const float aOne = 0.78539816;	// A(1) = pi / 4
	const float aT = 0.5 * ( t * st + asin( t ) );
	const float bT = -( st * st * st ) * ( 1.0 / 3.0 );	// B(1) = 0
	return 0.63661977 * ( c * ( aOne - aT ) - k * bT );
}

/// THE ORIGIN PLANE'S GATE for the surfaces a fetch holds along the origin's axis n (their
/// O-premultiplied position pO and coverage o): 0 at or behind the origin's depth plus one
/// fine cell (`invResN`), 1 half a cell beyond, a tent's CDF between.
float jahVoxelOriginGate( float pO, float o, vec4 org, float invResN )
{
	if( !( o > 0.0 ) )
		return 1.0;
	return jahVoxelTentCdf( ( ( pO / o - org.y ) * org.z - invResN ) / ( 0.5 * invResN ) );
}

/// No origin plane (a probe ray in free space; a point read).
const vec4 kJahVoxelNoOrigin = vec4( -1.0, 0.0, 0.0, 0.0 );

/// The coverage and the position of half `h` (0: the faces looking +a, 1: -a).
vec4 jahVoxelCov( int c, int h, vec3 u, float l )
{
	return h == 0 ? JAH_VOX_SAMPLE_COVP( c, u, l ) : JAH_VOX_SAMPLE_COVN( c, u, l );
}
vec4 jahVoxelPos( int c, int h, vec3 u, float l )
{
	return h == 0 ? JAH_VOX_SAMPLE_POSP( c, u, l ) : JAH_VOX_SAMPLE_POSN( c, u, l );
}

/// LEVEL 0'S POSITION IS RELATIVE TO ITS OWN CELL (PHOTON-7 stage 1, L1; VoxelMerge_piece_cs.any's THE SURFACE
/// POSITION): O x f, f the surfaces' place in the texel's cell. A read whose point lies on a texel CENTRE along axis
/// a (a plane's own axis: the march fetches every plane at its centre) holds that one texel along a - its cell index
/// restores the absolute O x p the gates compare (the cascade's normalised space, as every coarser level stores it).
float jahVoxelPos0Centred( float pORel, float o, float coordA, float invResA )
{
	const float resA = 1.0 / invResA;
	return ( pORel + o * min( floor( coordA * resA ), resA - 1.0 ) ) * invResA;
}

/// ...and a level-0 read FILTERED across two cells along a (the kernel's lateral read on a level-0 plane): with u =
/// coord x res - 1/2, i0 = floor(u), t = u - i0 the upper cell's weight, the filtered O x f is V = sum w O f and its
/// coverage C = sum w O; the absolute sum w O (i + f) = V + i0 C + t C1 (i1 - i0), C1 the upper cell's own coverage
/// (one fetch at its centre, the same lateral weights) - both cells clamped to the box as the sampler clamps them.
float jahVoxelPos0Filtered( int c, int h, vec3 u, int a, float pORel, float o, float invResA )
{
	const float resA = 1.0 / invResA;
	const float x = u[a] * resA - 0.5;
	const float i0 = floor( x );
	const float t = x - i0;
	const float i0c = clamp( i0, 0.0, resA - 1.0 );
	const float i1c = clamp( i0 + 1.0, 0.0, resA - 1.0 );
	float up = 0.0;
	if( i1c > i0c && t > 0.0 )
	{
		vec3 uUp = u;
		uUp[a] = ( i1c + 0.5 ) * invResA;
		up = t * jahVoxelCov( c, h, uUp, 0.0 )[a] * ( i1c - i0c );
	}
	return ( pORel + i0c * o + up ) * invResA;
}

/// The packed-halves coordinate of an anisotropic read at ONE level: x folded into the first half
/// (the sign half is added per axis by the read below) and CLAMPED PER HALF like CLAMP_TO_EDGE on
/// the half's own texels (PHOTON-7 stage 1, AB A11): the sample's centre stays half a texel
/// `texelU` (that level's, in the packed u) inside the half, so a bilinear footprint never
/// straddles the seam into the OTHER SIGN's texel at the box's far end (a read at the box's -x
/// face took a quarter of it: gi.directional_seam, 0.75 of the lit floor).
vec3 jahVoxelAnisoUvw( vec3 posLS, float texelU )
{
	vec3 uvw = posLS;
	const float h = 0.5 * texelU;
	uvw.x = clamp( uvw.x * 0.5, h, 0.5 - h );
	return uvw;
}

/// One sample of cascade c's directional volume for axis b at the packed `u`, level `mip`.
vec4 jahVoxelAxisSample( int c, int b, vec3 u, float mip )
{
	if( b == 0 )
		return JAH_VOX_SAMPLE_X( c, u, mip );
	if( b == 1 )
		return JAH_VOX_SAMPLE_Y( c, u, mip );
	return JAH_VOX_SAMPLE_Z( c, u, mip );
}

/// Cascade c's directional volume for axis b at `posLS`, level `mip`, the texel read by a
/// reader travelling +b (`positive`) or -b - composited from the faces that look back at it.
/// THE PER-HALF CLAMP IS EACH LEVEL'S OWN (stage 1's read F2): a trilinear read blends levels
/// floor( mip ) and ceil( mip ), and one clamp at the coarser level's half texel pinned the finer
/// level up to half a coarse texel inside the face (its edge texel never read alone there; at the
/// chain's next-to-last level the half's x resolution gone). And past the directional chain's
/// LAST level (each half one texel along the shortest axis) the half texel exceeded a quarter of
/// u and the clamp's min passed its max - undefined: the second half read the seam's two texels
/// half and half. So the level is clamped to the chain's last (the sampler clamps it there too);
/// where no level's clamp binds the read is the one trilinear sample it always was (bit-identical:
/// every read off the faces), and within a coarse half texel of a face each level is fetched with
/// its own clamp and the two blended by the level's fraction.
vec4 jahVoxelReadAxis( int c, int b, vec3 posLS, bool positive, float mip )
{
	const vec3 invRes = JAH_VOX_INVRES( c );
	// the packed width is the cascade's resolution (two halves of res / 2 two-cell texels): a level-m texel is
	// 2^m / res of u; the chain's last level is the shortest axis's halves at one texel, log2( res_min ) - 1
	const float lastLevel = max( floor( -log2( max( invRes.x, max( invRes.y, invRes.z ) ) ) - 0.5 ), 0.0 );
	const float m = clamp( mip, 0.0, lastLevel );
	const float m0 = floor( m );
	const float m1 = ceil( m );
	const float off = positive ? 0.0 : 0.5;
	const float x = posLS.x * 0.5;
	const float h1 = 0.5 * exp2( m1 ) * invRes.x;
	if( x >= h1 && x <= 0.5 - h1 )
		return jahVoxelAxisSample( c, b, vec3( x + off, posLS.yz ), mip );
	const vec4 v0 = jahVoxelAxisSample( c, b, jahVoxelAnisoUvw( posLS, exp2( m0 ) * invRes.x ) + vec3( off, 0.0, 0.0 ), m0 );
	if( m1 == m0 )
		return v0;
	const vec4 v1 = jahVoxelAxisSample( c, b, jahVoxelAnisoUvw( posLS, exp2( m1 ) * invRes.x ) + vec3( off, 0.0, 0.0 ), m1 );
	return mix( v0, v1, m - m0 );
}

/// THE FIRST PLANE (a cascade's, holding the point the cone has read up to): part of its depth
/// is behind that point. Its surface counts by WHERE it lies - all of it ahead, none behind, a
/// tent of one fine cell between - never by the unread fraction w: a floor 0.1 m below a start
/// in its own texel, counted at w, let the rest of the cone through it (the gate lab's sealed
/// box, spikes/photon-voxel-4/lab). The texel's own far face bounds nothing: the surface is
/// this texel's.
float jahVoxelFirstPlane( float pO, float o, float readTo, float dirA, float invResA )
{
	if( !( o > 0.0 ) )
		return 0.0;
	return jahVoxelTentCdf( ( pO / o - readTo ) * ( dirA > 0.0 ? 1.0 : -1.0 ) / ( 0.5 * invResA ) );
}

/// THE KERNEL'S LEVEL: the footprint's radius (lod - 1, fractional) - and never finer than
/// the plane's own texel (2^mip texel cells), or the kernel's hats, sampled one plane apart,
/// would not span the spacing and a surface between two samples would be read in part.
float jahVoxelKernelLevel( float lod, float texelCells, float mip )
{
	return max( max( lod - 1.0, 0.0 ), log2( texelCells ) + mip );
}

/// ONE PLANE OF THE MARCH (the rule above): cascade c at `posLS` (the plane's centre),
/// the kernel at `kernelLS` (the middle of the stretch the plane stands for - the centre but
/// on a cascade's first plane), travelling along `dir` (unit, normalised space) with the
/// cone's `tanHalf`; `axis` the plane's axis, `mip` and `directional` its level and volume,
/// `w` its unread part and `readTo` the coordinate along the axis read up to (the first
/// plane's surface by position), `lenLS` the length the cone spends in it (w T / |d_a|), `lk`
/// the kernel's fractional level (jahVoxelKernelLevel), `org` the origin plane. Returns the
/// premultiplied light and the opacity, uncapped.
vec4 jahVoxelReadPlane( int c, vec3 posLS, vec3 kernelLS, vec3 dir, int axis, float mip,
						bool directional, float w, float readTo, float lenLS, float lk, float tanHalf,
						vec4 org )
{
	const vec3 invRes = JAH_VOX_INVRES( c );
	const int n = int( org.x );
	// THE INDEX IS CLAMPED (every use is guarded by n == axis or n >= 0): a caller passing
	// the constant kJahVoxelNoOrigin folds n to -1, and SPIR-V rejects a constant negative
	// access chain even in a branch that never runs (VUID-VkShaderModuleCreateInfo-pCode-08737).
	const int ni = max( n, 0 );
	const bool ray = !( tanHalf > 0.0 );
	// THE PLANE'S OWN AXIS, at the plane, from the half looking back at the travel
	const int ha = dir[axis] > 0.0 ? 1 : 0;
	float aA;
	vec3 cA;
	float gA = 1.0;
	if( directional )
	{
		const vec4 v = jahVoxelReadAxis( c, axis, posLS, dir[axis] > 0.0, mip );
		aA = v.w;
		cA = v.xyz;
		if( n == axis || w < 1.0 )
		{
			const float pO = jahVoxelPos( c, ha, posLS, mip + 1.0 )[axis];
			const float oO = jahVoxelCov( c, ha, posLS, mip + 1.0 )[axis];
			if( n == axis )
				gA = jahVoxelOriginGate( pO, oO, org, invRes[ni] );
			if( w < 1.0 )
				gA *= jahVoxelFirstPlane( pO, oO, readTo, dir[axis], invRes[axis] );
		}
	}
	else
	{
		// LEVEL 0 (the only level a light read takes from the total; `mip` is 0 here)
		vec4 s = JAH_VOX_SAMPLE_ISO( c, posLS, 0.0 );
#if JAH_VOX_HAS_BACK
		// LEVEL 0 PER SIDE (PHOTON-VOXEL-5): a voxel holding both faces of a slab thinner than the
		// cell keeps two lights - the isotropic volume their mean, the back beside it - and the
		// travel meets the side its canonical normal FACES (BUGS-1 item 4: the sign of n . -dir,
		// the whole normal against the whole direction; the normal's component along the travel's
		// dominant axis alone read a +y normal met along x as the MEAN): facing the travel the
		// front (twice the mean minus the back), facing away the back. A one-sided voxel's back IS
		// its light, so either answer is its light. The normal is read filtered like the light
		// (the canonical normal of a two-sided voxel agrees between neighbours: VoxelMerge_piece_cs.any).
		{
			const vec4 bk = JAH_VOX_SAMPLE_BACK( c, posLS, 0.0 );
			const float along = dot( JAH_VOX_SAMPLE_NRM( c, posLS, 0.0 ).xyz * 2.0 - 1.0, -dir );
			if( along < 0.0 )
				s.xyz = bk.xyz;
			else if( along > 0.0 )
				s.xyz = max( 2.0 * s.xyz - bk.xyz, vec3( 0.0 ) );
		}
#endif
		// a ray takes both halves (any face it crosses stops it), a cone the one looking back
		const float oBack = jahVoxelCov( c, ha, posLS, 0.0 )[axis];
		const float o = ray ? min( 1.0, oBack + jahVoxelCov( c, 1 - ha, posLS, 0.0 )[axis] ) : oBack;
		aA = o;
		cA = ( s.w > 0.0 ) ? s.xyz * ( aA / s.w ) : vec3( 0.0 );
		if( n == axis || w < 1.0 )
		{
			// (level 0: the plane's own texel along the axis, its cell added - jahVoxelPos0Centred)
			const float pO = jahVoxelPos0Centred( jahVoxelPos( c, ha, posLS, 0.0 )[axis], oBack, posLS[axis],
												   invRes[axis] );
			if( n == axis )
				gA = jahVoxelOriginGate( pO, oBack, org, invRes[ni] );
			if( w < 1.0 )
				gA *= jahVoxelFirstPlane( pO, oBack, readTo, dir[axis], invRes[axis] );
		}
	}
	// (on the first plane w < 1 and the gate above is the whole weight: where the surface lies)
	const float kA = w < 1.0 ? gA : gA * w;
	float x = kA * aA;
	vec3 colour = kA * cA;

	// THE OTHER TWO AXES, through the footprint kernel, each half at the cone's rate, the
	// colour from the directional volume's half facing the rays that meet it
	// A RAY (aperture 0) IS STOPPED BY ANY FACE IT CROSSES, from either side: it may start
	// inside a solid (a field probe in a wall) and the solid's back face then stops it. A
	// ray from outside meets the front face first, so both halves together - capped per
	// texel like one surface - read it once. A CONE from a surface reads, per half, the
	// faces looking back at its rays (the split's reason: a partially covering wall read
	// through both of its faces doubled).
	const vec3 okBoth = ray ? min( jahVoxelCov( c, 0, kernelLS, lk ).xyz + jahVoxelCov( c, 1, kernelLS, lk ).xyz,
								   vec3( 1.0 ) )
							: vec3( 0.0 );
	for( int h = 0; h < ( ray ? 1 : 2 ); ++h )
	{
		const vec3 ok = ray ? okBoth : jahVoxelCov( c, h, kernelLS, lk ).xyz;
		float gN = 1.0;
		if( n >= 0 && n != axis )
		{
			float pON = jahVoxelPos( c, h, kernelLS, lk )[ni];
			// a level-0 plane reads its kernel at level 0 (lk = 0): cell-relative, filtered across two cells
			if( !directional )
				pON = jahVoxelPos0Filtered( c, h, kernelLS, ni, pON, ok[ni], invRes[ni] );
			gN = jahVoxelOriginGate( pON, ok[ni], org, invRes[ni] );
		}
		for( int b = 0; b < 3; ++b )
		{
			if( b == axis || !( ok[b] > 0.0 ) )
				continue;
			// rays heading +b meet the faces looking -b (h = 1), rays heading -b the +b ones - a RAY
			// by its own direction on each axis (BUGS-1 item 4: its one pass read h = 0 on every axis)
			const bool hb = ray ? dir[b] > 0.0 : h == 1;
			const float cb = ray ? abs( dir[b] ) : ( h == 1 ? dir[b] : -dir[b] );
			const float rate = jahVoxelConeRate( cb, tanHalf * sqrt( max( 1.0 - dir[b] * dir[b], 0.0 ) ) );
			const float xb = ( b == n ? gN : 1.0 ) * ok[b] * ( rate * lenLS / invRes[b] );
			if( !( xb > 0.0 ) )
				continue;
			const vec4 v = jahVoxelReadAxis( c, b, kernelLS, hb, max( lk - 1.0, 0.0 ) );
			const vec3 rad = ( v.w > 0.0 ) ? v.xyz * ( 1.0 / v.w ) : vec3( 0.0 );
			x += xb;
			colour += xb * rad;
		}
	}
	return vec4( colour, x );
}

/// THE LEVEL-0 TEXEL of the volume a read takes, in cells: 2 for the directional volumes
/// (half resolution, both signs packed along x), 1 for the total's level 0.
float jahVoxelTexelCells( bool directional )
{
	return directional ? 2.0 : 1.0;
}

/// Cascade c's radiance at `posLS` (the cascade's normalised [0, 1] box) seen along `dir`
/// for a footprint `lod`: ONE PLANE of the march read whole at that point as a ray (aperture
/// 0), with no origin plane (the ray hit: a point on the surface it reads; the march's own one-step read, to
/// the bit - engine.voxel_reader_parity). Level 0 of the total for a footprint within a
/// cell, the directional volumes past it. Capped at 1.
vec4 jahVoxelSample( int c, vec3 posLS, vec3 dir, float lod )
{
	const vec3 invRes = JAH_VOX_INVRES( c );
	const int axis = jahVoxelAxis( dir, invRes );
	const float invAbsDa = 1.0 / abs( dir[axis] );
	const bool directional = lod > 0.5;
	const float texelCells = jahVoxelTexelCells( directional );
	const float mip = directional ? floor( jahVoxelKernelMip( lod, texelCells ) ) : 0.0;
	const float texelLS = texelCells * exp2( mip ) * invRes[axis];
	const vec4 r = jahVoxelReadPlane( c, posLS, posLS, dir, axis, mip, directional, 1.0, 0.0,
									  texelLS * invAbsDa, jahVoxelKernelLevel( lod, texelCells, mip ), 0.0,
									  kJahVoxelNoOrigin );
	return r.w > 1.0 ? vec4( r.xyz * ( 1.0 / r.w ), 1.0 ) : r;
}

/// THE FOOTPRINT'S LOD for a world footprint against a cascade's cell (both in world
/// units): log2 of the footprint in cells - the march's own lod - never below 0 and
/// never past the sixth. The reads map it to their levels (jahVoxelKernelMip).
float jahVoxelFootprintLod( float footprint, float texel )
{
	return clamp( log2( max( footprint, texel ) / texel ), 0.0, 6.0 );
}

/// A read that holds nothing, or holds something that is not a number, is no
/// answer (the finite test is on the BITS: a self-equality is folded to true on
/// this stack — jah_rq_finite.glsl).
bool jahVoxelSampleUsable( vec4 s )
{
	return s.w > 0.02 && finite3( s.xyz ) && finite1( s.w );
}

#endif   // JAH_VOXEL_SAMPLE_GLSL
