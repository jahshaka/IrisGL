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
//     JAH_VOX_HAS_ANISO          0 or 1, a PREPROCESSOR constant: the three
//                                anisotropic slots are DECLARED in this shader
//     JAH_VOX_ANISO              bool: they carry data (a runtime choice is
//                                fine; the Low tier builds without them)
//     JAH_VOX_SAMPLE_ISO(c,u,l)  vec4: cascade c's isotropic volume at uvw u,
//                                mip l (Low: every mip; anisotropic tiers: the
//                                full-resolution level the march starts in)
//     JAH_VOX_SAMPLE_X(c,u,l)    vec4: the X-axis anisotropic volume (both
//     JAH_VOX_SAMPLE_Y(c,u,l)    signs packed side by side along u.x, so u.x
//     JAH_VOX_SAMPLE_Z(c,u,l)    runs over [0, 1) and the halves are 0.5 apart)
//     JAH_VOX_SAMPLE_COVP(c,u,l) vec4: cascade c's COVERAGE of the faces looking +a (xyz = O_x+,
//     JAH_VOX_SAMPLE_COVN(c,u,l)   O_y+, O_z+) and of those looking -a (PHOTON-VOXEL-3/-4;
//                                VctLighting's coverageIndex(0 / 1))
//     JAH_VOX_SAMPLE_POSP(c,u,l) vec4: cascade c's SURFACE POSITION per half (xyz = O_a x p_a,
//     JAH_VOX_SAMPLE_POSN(c,u,l)   p absolute in the cascade's normalised space; positionIndex(0 / 1))
//                                - the origin plane's test
//     JAH_VOX_INVRES(c)          vec3: 1 / cascade c's resolution, per axis
//     JAH_VOX_HAS_BACK           0 or 1, a PREPROCESSOR constant (PHOTON-VOXEL-5): the anisotropic
//                                tiers' level-0 BACK side and the voxeliser's normal are bound:
//     JAH_VOX_SAMPLE_BACK(c,u,l) vec4: level 0's back side (premultiplied like the isotropic
//                                volume, which holds the two sides' MEAN - the front is twice
//                                the mean minus the back); VctLighting::backIndex()
//     JAH_VOX_SAMPLE_NRM(c,u,l)  vec4: the voxeliser's normal (xyz biased, the canonical normal of a
//                                two-sided voxel: the front is the side it points to); normalIndex()
//
// THE VOLUME'S DIRECTIONAL READ IS THE ONLY RULE HERE — ONE RULE AT EVERY MIP
// (PHOTON-VOXEL-3, jahVoxelReadIso's comment): a step takes the opacity of what it
// crosses, per axis, from the per-axis coverage at mip 0 and from the half of each
// axis volume composited TOWARDS the reader above it. The pixel's cones, the bounce,
// the field, the cards and the ray hit read through it; nothing else reads a volume.

#ifndef JAH_VOXEL_SAMPLE_GLSL
#define JAH_VOXEL_SAMPLE_GLSL

#ifndef JAH_VOX_HAS_BACK
#define JAH_VOX_HAS_BACK 0
#endif

/// THE LEVEL A READ TAKES FOR A FOOTPRINT 2^lod CELLS WIDE: the level whose TRILINEAR
/// KERNEL is the footprint. A trilinear fetch weighs the texels within one texel of the
/// sample, so its kernel is two texels wide: the level whose texel is half the
/// footprint. (Upstream read the footprint's own level - a kernel twice the footprint.)
/// `texelCells` is the level-0 texel in cells: 1 for the isotropic volume, 2 for a
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
///   texel's full-crossing opacity (isotropic: 2^m O_a, the chain holding a mean per cell;
///   directional: alpha_a) times w, the part of the plane's depth not yet read (1 but on a
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
/// kernel's axes: the kernel's mean radiance - on the directional tiers the half facing the
/// rays that meet it - times what they take); the march composites ADDITIVELY, capped at 1.
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

/// The packed-halves coordinate of an anisotropic read: x is saturated into the
/// box and folded into the first half (the sign half is added per axis by the
/// read below).
vec3 jahVoxelAnisoUvw( vec3 posLS )
{
	vec3 uvw = posLS;
	uvw.x = clamp( uvw.x, 0.0, 1.0 ) * 0.5;
	return uvw;
}

#if JAH_VOX_HAS_ANISO
/// Cascade c's directional volume for axis b at `posLS`, level `mip`, the texel read by a
/// reader travelling +b (`positive`) or -b - composited from the faces that look back at it.
vec4 jahVoxelReadAxis( int c, int b, vec3 posLS, bool positive, float mip )
{
	const vec3 anisoUvw = jahVoxelAnisoUvw( posLS );
	const vec3 u = anisoUvw + vec3( positive ? 0.0 : 0.5, 0.0, 0.0 );
	if( b == 0 )
		return JAH_VOX_SAMPLE_X( c, u, mip );
	if( b == 1 )
		return JAH_VOX_SAMPLE_Y( c, u, mip );
	return JAH_VOX_SAMPLE_Z( c, u, mip );
}
#endif

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
#if JAH_VOX_HAS_ANISO
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
#endif
	{
		vec4 s = JAH_VOX_SAMPLE_ISO( c, posLS, mip );
#if JAH_VOX_HAS_BACK
		// LEVEL 0 PER SIDE (PHOTON-VOXEL-5): a voxel holding both faces of a slab thinner than the
		// cell keeps two lights - the isotropic volume their mean, the back beside it - and the
		// faces looking back at the travel (half ha) are the side the normal's component along the
		// axis says: along it the front (twice the mean minus the back), against it the back. A
		// one-sided voxel's back IS its light, so either answer is its light. The normal is read
		// filtered like the light (the canonical normal of a two-sided voxel agrees between
		// neighbours: VoxelMerge_piece_cs.any).
		{
			const vec4 bk = JAH_VOX_SAMPLE_BACK( c, posLS, mip );
			const float along = ( ha == 0 ? 1.0 : -1.0 ) * ( JAH_VOX_SAMPLE_NRM( c, posLS, mip )[axis] * 2.0 - 1.0 );
			if( along < 0.0 )
				s.xyz = bk.xyz;
			else if( along > 0.0 )
				s.xyz = max( 2.0 * s.xyz - bk.xyz, vec3( 0.0 ) );
		}
#endif
		// a ray takes both halves (any face it crosses stops it), a cone the one looking back
		const float oBack = jahVoxelCov( c, ha, posLS, mip )[axis];
		const float o = ray ? min( 1.0, oBack + jahVoxelCov( c, 1 - ha, posLS, mip )[axis] ) : oBack;
		aA = exp2( mip ) * o;
		cA = ( s.w > 0.0 ) ? s.xyz * ( aA / s.w ) : vec3( 0.0 );
		if( n == axis || w < 1.0 )
		{
			const float pO = jahVoxelPos( c, ha, posLS, mip )[axis];
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

	// THE OTHER TWO AXES, through the footprint kernel, each half at the cone's rate
#if JAH_VOX_HAS_ANISO
	const bool anisoTier = JAH_VOX_ANISO;
#else
	const bool anisoTier = false;
#endif
	vec3 isoRad = vec3( 0.0 );
	if( !anisoTier )
	{
		const vec4 sk = JAH_VOX_SAMPLE_ISO( c, kernelLS, lk );
		isoRad = ( sk.w > 0.0 ) ? sk.xyz * ( 1.0 / sk.w ) : vec3( 0.0 );
	}
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
			gN = jahVoxelOriginGate( jahVoxelPos( c, h, kernelLS, lk )[ni], ok[ni], org, invRes[ni] );
		for( int b = 0; b < 3; ++b )
		{
			if( b == axis || !( ok[b] > 0.0 ) )
				continue;
			// rays heading +b meet the faces looking -b (h = 1), rays heading -b the +b ones
			const float cb = ray ? abs( dir[b] ) : ( h == 1 ? dir[b] : -dir[b] );
			const float rate = jahVoxelConeRate( cb, tanHalf * sqrt( max( 1.0 - dir[b] * dir[b], 0.0 ) ) );
			const float xb = ( b == n ? gN : 1.0 ) * ok[b] * ( rate * lenLS / invRes[b] );
			if( !( xb > 0.0 ) )
				continue;
			vec3 rad = isoRad;
#if JAH_VOX_HAS_ANISO
			if( anisoTier )
			{
				const vec4 v = jahVoxelReadAxis( c, b, kernelLS, h == 1, max( lk - 1.0, 0.0 ) );
				rad = ( v.w > 0.0 ) ? v.xyz * ( 1.0 / v.w ) : vec3( 0.0 );
			}
#endif
			x += xb;
			colour += xb * rad;
		}
	}
	return vec4( colour, x );
}

/// THE LEVEL-0 TEXEL of the volume a read takes, in cells: 2 for the directional volumes
/// (half resolution, both signs packed along x), 1 for the isotropic one.
float jahVoxelTexelCells( bool directional )
{
	return directional ? 2.0 : 1.0;
}

/// Cascade c's radiance at `posLS` (the cascade's normalised [0, 1] box) seen along `dir`
/// for a footprint `lod`: ONE PLANE of the march read whole at that point as a ray (aperture
/// 0), with no origin plane (the ray hit: a point on the surface it reads; the march's own one-step read, to
/// the bit - engine.voxel_reader_parity). The directional volumes where the anisotropic
/// slots carry data, the isotropic volume otherwise. Capped at 1.
vec4 jahVoxelSample( int c, vec3 posLS, vec3 dir, float lod )
{
	const vec3 invRes = JAH_VOX_INVRES( c );
	const int axis = jahVoxelAxis( dir, invRes );
	const float invAbsDa = 1.0 / abs( dir[axis] );
#if JAH_VOX_HAS_ANISO
	const bool directional = JAH_VOX_ANISO && lod > 0.5;
	const bool anisoTier = JAH_VOX_ANISO;
#else
	const bool directional = false;
	const bool anisoTier = false;
#endif
	const float texelCells = jahVoxelTexelCells( directional );
	const float mip = ( anisoTier && !directional ) ? 0.0 : floor( jahVoxelKernelMip( lod, texelCells ) );
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
