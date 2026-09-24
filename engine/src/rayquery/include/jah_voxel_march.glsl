// THE CONE MARCH OVER THE CASCADE CHAIN — the one copy (PHOTON-READER-1; the
// render audit's section 7.1 "ONE VOXEL RADIANCE READER", F4/F7/F8/F9).
//
// WHO READS IT. The pixel shader's diffuse and specular cones
// (Hlms/Pbs/Any/Vct_piece_ps.any), the bounce-injection job's diffuse cones
// (VCT/LightVctBounceInject_piece_cs.any) and the irradiance field's probe rays
// (Compute/Algorithms/IrradianceFields/IrradianceFieldGenFromVct_piece_cs.any),
// all through the piece `JahVoxelMarch` that the build WRAPS this file into
// (irisgl/engine/CMakeLists.txt). Before this file the march existed THREE
// times with three different rule sets — the pixel's (age carry, the escape
// estimate, the one-cell hop bias, the single transmittance), the bounce job's
// twin (the age carry but no escape and no hop bias) and the field's (cascade 0
// only, no age, no escape returned) — and the two integrals of the scene's
// radiance they produced were blended against each other on screen (audit F7).
//
// THE AT-SIGN RULE: this file becomes Hlms input, so no character of it may be
// the Hlms directive mark, comments included. It includes nothing; the caller
// includes jah_rq_finite.glsl and jah_voxel_sample.glsl before it (the Hlms
// wrapper puts both in the piece `JahVoxelSample`, inserted first).
//
// HOW A CALLER BINDS IT — the sample file's macros, and:
//
//     JAH_VOX_MAX_CASCADES      a PREPROCESSOR constant: how many cascades the
//                               shader declares (the hop code and the chain
//                               parameters exist only above one)
//     JAH_VOX_COUNT             int: how many are bound
//     JAH_VOX_INVRES(c)         vec3: 1 / cascade c's resolution, per axis
//     JAH_VOX_MAXLOD(c)         float: the mip at which cascade c hands a cone
//                               to the next one (the next cascade's cell over
//                               this one's, as a mip; the last one's is past
//                               any mip)
//     JAH_VOX_FROM_PREV_SCALE(c)  vec4, c >= 1: xyz = cascade c-1's normalised
//                               space to cascade c's (the scale), w = cascade
//                               c's radiance over cascade 0's (the stored units
//                               of each cascade differ by its own multiplier)
//     JAH_VOX_FROM_PREV_OFFSET(c) vec4, c >= 1: xyz = the offset of that map,
//                               w = the specular walk's per-hop weight slope
//     JAH_VOX_SDF_MAXMIP, JAH_VOX_SDF_FACTOR   (only with JAH_MARCH_SDF) the
//                               specular empty-space skip's two parameters
//
// THE RULES, written once, each with the number of the fork change that made
// it (irisgl/docs/OGRE_BUILD.md resolves the numbers):
//
//   THE AGE CARRY (0066 pixel, 0074 bounce). `dist` positions the samples from
//   THIS cascade's entry point; `travelled` is the cone's age since the
//   SURFACE, and it is what sizes the footprint. They are equal in the first
//   cascade. At a hop the age is carried into the new cascade's units along
//   the direction of travel. The specular walk does NOT carry it (it restarts
//   at every hop, upstream's behaviour): its environment share rides the raw
//   directional COLOUR opacity, which over-states occlusion at a coarse mip, and
//   the true (wider) footprint fed to it took the reflected sky from
//   1.00/0.85/0.85 to 1.00/0.43/0.34 at one/two/four cascades (gi.cascades case
//   7). The specular walk keeps NO escape estimate at all (SPEC-ESCAPE-CRUD:
//   nobody read it); reading the occupancy estimate there is its own lane.
//
//   THE ESCAPE IS THE OPACITY (PHOTON-VOXEL-3; was 0021, 0084). What the ambient
//   (the sky) rides is the composite's own opacity
//   along the cone's axis at every mip. The separate occupancy estimate the
//   anisotropic stretch used to read (the MIN over the three axis volumes) read
//   0 for any thin sheet on the directional store; it is deleted, with the
//   escapeAlpha member and the JAH_MARCH_NO_ESCAPE flag that skipped it.
//
//   THE HOP IS CONTIGUOUS (PHOTON-VOXEL-3; 0033 assign, 0074 single
//   transmittance). A cone that leaves a cascade - its footprint has grown to the
//   next cascade's cell, or it has left the box - continues in the next one FROM THE
//   POINT THE PREVIOUS ONE HAD READ UP TO: the next cascade's first plane is the one
//   holding that point, weighted by its unvisited part, so no stretch of the ray is read
//   twice or by nobody. (Fork change 0070 - READER-1's record stands as history -
//   stepped the continuation one cell OF THE NEW CASCADE along the surface normal: up to
//   1.875 m unread at the outermost cascade. What 0070 approximated with that constant -
//   keeping a coarse cascade from reading the surface the cone left - is now THE ORIGIN PLANE
//   below.) The continuation starts its accumulators at the running totals, so what it
//   returns ALREADY carries the transmittance of every cascade before it: the colour is
//   ADDED once, alpha is ASSIGNED.
//
//   THE PLANE MARCH (PHOTON-VOXEL-3): ONE DEPTH PLANE PER FETCH. A trilinear fetch at an
//   arbitrary point blends the texel holding a surface with the texel BEHIND it along
//   the ray, linearly, into one sample; the capped additive composite then spends that
//   sample's opacity before the front surface's own share is used up, and the colour
//   behind leaks in - up to (phi/s)(1 - phi/s) = 25 % of it at every level, by the
//   surface's phase phi in its texel (round 8: a lit floor read straight down came back at
//   0.516 / 0.396 / 0.335 of its 0.55 at the directional levels 1 / 2 / 3, the slab's
//   unlit underside blended in). No per-sample weight can separate two texels one fetch
//   has already mixed. So, along the ray's DOMINANT axis a (the most cells crossed per
//   unit length: argmax |d_a| / invRes_a - argmax |d_a| when the cascade's cell counts are
//   equal), every sample sits ON A TEXEL-PLANE CENTRE of the level it reads: the trilinear
//   weight along a is exactly 1 and the fetch is bilinear across the plane (the cone's
//   lateral filtering kept). The next sample is the next plane: one texel of that level
//   along a, Delta t = T / |d_a|. Every plane is fetched once, in order, so the composite
//   along the ray IS the front-to-back composite of consecutive planes, and the fetch
//   count does not rise (an oblique ray fetches fewer). A plane's opacity is WHAT A
//   PLANE READS (below).
//   A PLANE'S WEIGHT is its UNVISITED fraction along a, w = |far face - B| / T (B the
//   coordinate the march has read up to; the far face the plane's face in the direction of
//   travel) - a difference of the coordinates over the texel, the same arithmetic in every
//   stage; it replaces Delta/s and the CDF weights. Along the march it is 1: A COARSER
//   LEVEL IS TAKEN ONLY ON ITS OWN PLANE BOUNDARY (the finer level reads one more plane
//   first, which ends there - the grids are powers of two apart), so every plane read is
//   read whole. A coarse plane read for the half the finer planes had not covered, at w =
//   1/2, took half of whatever surface lay in it - a lit floor in that half read 0.5 of
//   itself and the slab's unlit underside filled the rest: gi.cards' wall indirect 0.130 at
//   h 0.6 m against 0.260 around it. w < 1 is left only for a cascade's FIRST plane, the
//   one holding its start (the start bias point, or where the previous cascade stopped).
//   THE EXIT per cascade: the isotropic level is read while the footprint's level is below
//   the hand-over mip; on the anisotropic tiers the isotropic level 0 is read until the
//   footprint outgrows it (lod > 0.5), then the directional volumes. The LEVEL a plane
//   reads is an INTEGER level, the footprint's (jahVoxelKernelMip floored: the texel never
//   wider than half the footprint). A cascade ends at an opacity of 0.95, at the hand-over
//   mip, or at its box (below).
//
//   WHAT A PLANE READS (PHOTON-VOXEL-4; jah_voxel_sample.glsl, jahVoxelReadPlane): its own
//   axis at the plane; the other two axes through a kernel the size of the cone's
//   footprint (the coverage chain at the fractional level lod - 1), crossed over the length
//   the cone spends in the plane; the ORIGIN PLANE - the surface the cone left, found by
//   its stored depth within one fine cell - never counted, at any level and in any cascade.
//   The origin plane rides the hop with the position. The rule was chosen on the CPU
//   emulation of this march over read-back stores (spikes/photon-voxel-4/lab, validated
//   against this file to 0.001 of alpha): the axis-crossing gate of B.1 read a surface
//   only where the cone's AXIS crossed its plane (the open floor's wall cone 0.077 of the
//   cone-trace reference's 0.413) and a floor lying on a voxel face at half its opacity
//   (the fetch's blend weight); what replaced it, and before it the origin rule, is here.
//
//   THE OPAQUE THRESHOLD, 0.95 (the march stops at it and the escape is
//   1 - min( 1, alpha / 0.95 )): the store's quantum is at most 0.3 % on a wall crossing
//   (the 10-bit resolve, 1/2046 per voxel; the 1/512 grid, 1/1024 per contribution);
//   the 5 % the threshold leaves covers the OBLIQUE LINE-INTEGRAL SPREAD - a tilted open
//   sheet reads 0.967 on average and 0.83 at worst along its own normal
//   (gi.voxel_coverage) - geometry, not quantum.

#ifndef JAH_VOXEL_MARCH_GLSL
#define JAH_VOXEL_MARCH_GLSL

/// THE WALK'S VARIANTS, as literal flags a caller passes (the compiler folds
/// every branch on them).
///   DIFFUSE is the default: the per-cascade hand-over mip, the age carried at
///   a hop, the colour hop weighted by the cascade multiplier.
const uint JAH_MARCH_SPECULAR = 1u;	///< maxLod 11 per cascade, no age carry, the specular hop weight
const uint JAH_MARCH_SDF = 2u;		///< the specular empty-space skip (a single volume only, as upstream)
/// Stop after the FIRST plane, read WHOLE (the parity harness's "the march at zero
/// length": what the march reads at a point, against the ray hit's read of it). No
/// consumer passes it.
const uint JAH_MARCH_ONE_STEP = 32u;

struct JahConeResult
{
	vec3 colour;		///< cascade 0's stored units (JAH_VOX_FROM_PREV_SCALE(c).w converts)
	float alpha;		///< the composite's opacity along the cone - what the escape rides
	float lodLevel;
	vec3 posLS;			///< the point the march has read up to, in lastCascade's normalised space
	float travelled;	///< the cone's age, in lastCascade's normalised units
	float travelledC0;	///< the same age in cascade 0's normalised units
	int lastCascade;
};

/// THE ORIGIN PLANE of a cone set that starts at `startLS` (jahConeStart's point) biased
/// along `biasDirLS`: the axis the normal is dominant on, the surface's coordinate along it
/// (the start minus the bias) and the normal's sign (jahVoxelReadPlane never counts that
/// plane - the hemisphere's boundary).
vec4 jahConeOrigin( vec3 startLS, vec3 biasDirLS )
{
	const vec3 a = abs( biasDirLS );
	const int n = a.x >= a.y ? ( a.x >= a.z ? 0 : 2 ) : ( a.y >= a.z ? 1 : 2 );
	if( !( a[n] > 0.0 ) )
		return kJahVoxelNoOrigin;
	return vec4( float( n ), startLS[n] - biasDirLS[n] * JAH_VOX_INVRES( 0 )[n],
				 biasDirLS[n] > 0.0 ? 1.0 : -1.0, 0.0 );
}

/// THE SHARE OF A CONE BELOW ITS SURFACE: the rays through its cross-section (a disc of radius
/// tan at unit axial distance) that point into the surface - stopped by it. With sin e = d.N,
/// the disc's rays reach below where tan cos e X < -sin e: the disc's segment beyond
/// t = tan e / tan, a fraction (acos t - t sqrt(1 - t^2)) / pi; none when the rim clears the
/// surface (t >= 1: the diffuse sets, 45-degree cones of 44.5 and 60-degree ones of 30). A
/// grazing specular cone read the sky below the horizon through its own surface
/// (gi.ddgi_ambient's sealed room). `d`, `N` unit, in one space.
float jahConeBelow( vec3 d, vec3 N, float tanHalf )
{
	const float se = dot( d, N );
	const float ce = sqrt( max( 1.0 - se * se, 0.0 ) );
	if( !( tanHalf > 0.0 ) || !( ce > 1e-6 ) )
		return se < 0.0 ? 1.0 : 0.0;
	const float t = clamp( se / ( ce * tanHalf ), -1.0, 1.0 );
	return ( acos( t ) - t * sqrt( max( 1.0 - t * t, 0.0 ) ) ) * 0.31830989;
}

/// Is the sample's centre inside the cascade's unit box? THE MARCH ENDS AT THE BOX
/// (PHOTON-VOXEL-3; upstream's test let the FOOTPRINT overlap the box - |p - 0.5| <= 0.5 +
/// diameter / 2 - and read samples centred outside it, where the sampler clamps to the
/// border texel: at a coarse level the whole volume's mean, the origin surface included;
/// gi.ddgi_ambient's open floor read 0.81 and 1.6 of the level-5 texel from above the
/// volume). The next cascade picks the cone up where this one stopped (the contiguous
/// hop); past the last one nothing was voxelised. THE TAIL: a plane whose centre lies
/// outside on a MINOR axis is still read, its position clamped into the box, and ends the
/// cascade - its stretch would otherwise be read by nobody (a wall in the volume's last
/// cell: a sealed room's 4/255 of sky through it).
bool jahMarchInsideBox( vec3 samplePosLS )
{
	return all( greaterThanEqual( samplePosLS, vec3( 0.0 ) ) ) &&
		   all( lessThanEqual( samplePosLS, vec3( 1.0 ) ) );
}

/// THE PLANE of the texel extent `texelLS` (along axis a) that holds coordinate `b`,
/// going the way `sgn` says: xy = its near and far faces in the direction of travel.
/// The 1e-4 of a texel resolves a point ON a face to the plane beyond it.
vec2 jahMarchPlane( float b, float texelLS, float sgn )
{
	const float k = sgn > 0.0 ? floor( b / texelLS + 1e-4 ) : ceil( b / texelLS - 1e-4 ) - 1.0;
	const float lo = k * texelLS;
	return sgn > 0.0 ? vec2( lo, lo + texelLS ) : vec2( lo + texelLS, lo );
}

/// ONE CASCADE's march - THE PLANE MARCH (the file's header). `posLS` is the point the
/// cone has been read up to (the start bias point on the first cascade, the previous
/// cascade's end at a hop); `startingTravelled` the age it arrives with (0 on the first
/// cascade and on every specular hop); `startingLodLevel` a floor on the footprint's lod
/// (the hop's carried lod; the parity harness's point-read lod); `origin` the origin plane in
/// this cascade's space (jahVoxelReadPlane; kJahVoxelNoOrigin for a ray in free space).
JahConeResult jahConeMarchCascade( int c, vec3 posLS, vec3 dirLS, float tanHalfAngle,
								   float startingLodLevel, float startingAlpha,
								   float startingTravelled, vec4 origin, uint flags )
{
	const bool specular = ( flags & JAH_MARCH_SPECULAR ) != 0u;
	const bool oneStep = ( flags & JAH_MARCH_ONE_STEP ) != 0u;
	const vec3 invRes = JAH_VOX_INVRES( c );
	const float maxLod = specular ? 11.0 : JAH_VOX_MAXLOD( c );	// 2048^3 is the largest possible volume
	const float vctInvResolution = dot( abs( dirLS ), invRes );
	const float resolution = 1.0 / vctInvResolution;
	const int axis = jahVoxelAxis( dirLS, invRes );
	const float da = dirLS[axis];
	const float sgn = da > 0.0 ? 1.0 : -1.0;
	const float invAbsDa = 1.0 / abs( da );
	const float startA = posLS[axis];

#if JAH_VOX_HAS_ANISO
	const bool aniso = JAH_VOX_ANISO;
#else
	const bool aniso = false;
#endif

	float alpha = startingAlpha;
	vec3 color = vec3( 0.0 );
	float readTo = startA;		// B: the coordinate along the axis the march has read up to
	float lodLevel = startingLodLevel;
	float skipLod = 1.0;
	bool prevDirectional = false;	// the plane read last: which volume, which level (-1: none yet)
	float prevMip = -1.0;
	bool done = !jahMarchInsideBox( posLS ) || da == 0.0;
	int steps = 0;
	while( !done && alpha < 0.95 && steps < 256 )
	{
		++steps;
		// Where the march stands: the point read up to, its age and its footprint.
		const float tRead = ( readTo - startA ) * sgn * invAbsDa;
		const vec3 readPosLS = clamp( posLS + tRead * dirLS, vec3( 0.0 ), vec3( 1.0 ) );
		const float travelled = max( startingTravelled + tRead, vctInvResolution );
		lodLevel = max( startingLodLevel,
						log2( max( vctInvResolution, 2.0 * tanHalfAngle * travelled ) * resolution ) );
		if( !oneStep && lodLevel >= maxLod )
			break;	// the hand-over: the next cascade reads from here

		// WHICH VOLUME AND WHICH LEVEL (THE EXIT): the footprint's integer level - the texel
		// half the footprint (jahVoxelKernelMip, floored); the anisotropic tiers read the
		// isotropic level 0 until the footprint outgrows it. No height rule: the surface the
		// cone left is gated out by its position, at any level (the file's header).
		bool directional = aniso && lodLevel > 0.5;
		float texelCells = directional ? 2.0 : 1.0;
		float mip = ( aniso && !directional ) ? 0.0 : floor( jahVoxelKernelMip( lodLevel, texelCells ) );

		// THE PLANE: the one of this level holding the point read up to, fetched at its
		// centre. A COARSER LEVEL IS TAKEN ONLY ON ITS OWN PLANE BOUNDARY: while the point read
		// up to lies inside a coarse plane (the finer planes have read part of it), the march
		// reads one more plane of the level it was on - which ends on the boundary, the grids
		// being powers of two apart - so every coarse plane it reads is unvisited, WHOLE. (A
		// coarse plane weighted by its unvisited fraction, w = |far - B| / T, took the half
		// the finer planes had not read at half the plane's opacity, wherever the surface in
		// it lay: a lit floor in that half read 0.5 and the slab's underside filled the rest -
		// the gi.cards wall's indirect 0.130 at h 0.6 against 0.260 around it.)
		float texelLS = texelCells * exp2( mip ) * invRes[axis];
		vec2 faces = jahMarchPlane( readTo, texelLS, sgn );
		if( prevMip >= 0.0 && abs( faces.x - readTo ) > 1e-4 * texelLS &&
			( directional != prevDirectional || mip != prevMip ) )
		{
			directional = prevDirectional;
			texelCells = directional ? 2.0 : 1.0;
			mip = prevMip;
			texelLS = texelCells * exp2( mip ) * invRes[axis];
			faces = jahMarchPlane( readTo, texelLS, sgn );
		}
		prevDirectional = directional;
		prevMip = mip;
		const float tCentre = ( 0.5 * ( faces.x + faces.y ) - startA ) * sgn * invAbsDa;
		// (The point read is AT the point it is given: the harness hands it a plane centre.)
		const vec3 centreLS = oneStep ? posLS : posLS + tCentre * dirLS;
		done = !jahMarchInsideBox( centreLS );	// THE TAIL: read, clamped, and end
		const vec3 samplePosLS = clamp( centreLS, vec3( 0.0 ), vec3( 1.0 ) );
		// THE PLANE (jah_voxel_sample.glsl, jahVoxelReadPlane): its own axis read whole but
		// for the part a cascade's first plane has already behind it (w), the other two axes
		// through the footprint's kernel over the length the cone spends in the plane. The
		// point read is one whole plane.
		const float w = oneStep ? 1.0 : abs( faces.y - readTo ) / texelLS;
		// the kernel at the middle of the stretch this plane stands for
		const float tMid = ( 0.5 * ( readTo + faces.y ) - startA ) * sgn * invAbsDa;
		const vec3 kernelLS = oneStep ? samplePosLS : clamp( posLS + tMid * dirLS, vec3( 0.0 ), vec3( 1.0 ) );
		const vec4 sampleColour = jahVoxelReadPlane( c, samplePosLS, kernelLS, dirLS, axis, mip, directional,
													 w, readTo, w * texelLS * invAbsDa,
													 jahVoxelKernelLevel( lodLevel, texelCells, mip ),
													 tanHalfAngle, origin );
		float nextReadTo = faces.y;

#ifdef JAH_VOX_SDF_FACTOR
		if( ( flags & JAH_MARCH_SDF ) != 0u )
		{
			// THE EMPTY-SPACE SKIP (upstream's, specular only, one volume only): a
			// near-mirror cone is a path trace, so the opacity of a coarser mip stands in
			// for a distance field and the march skips whole planes across empty space.
			// Empirically tuned upstream; the blend on the cone angle turns it off as the
			// cone widens.
			float finalOpac = JAH_VOX_SAMPLE_ISO( c, samplePosLS, skipLod ).w;
			float skipFactor = exp2( max( 0.0, skipLod * 0.5 - 1.0 ) ) * ( 1.0 - finalOpac ) +
							   finalOpac;
			skipFactor = mix( skipFactor, 1.0,
							  min( -1.0 + finalOpac * JAH_VOX_SDF_FACTOR + tanHalfAngle * 50.0,
								   1.0 ) );
			skipLod = clamp( skipLod + ( 1.0 - finalOpac ) * 2.0 - 1.0, 1.0, JAH_VOX_SDF_MAXMIP );
			nextReadTo += sgn * floor( max( skipFactor - 1.0, 0.0 ) ) * texelLS;
		}
#endif

		// ADDITIVE, capped: the plane's opacity is taken until the cone is opaque - the
		// pieces of one surface split between planes add up to that surface. The colour
		// comes with the share of it the cone still had room for (whole, as read: a GPU's
		// x / x is not exactly 1, and the ray hit's read must agree to the bit).
		const float take = min( sampleColour.w, 1.0 - alpha );
		if( take >= sampleColour.w )
			color += sampleColour.xyz;
		else if( sampleColour.w > 0.0 )
			color += sampleColour.xyz * ( take / sampleColour.w );
		alpha += take;

		readTo = nextReadTo;
		if( sgn > 0.0 ? readTo >= 1.0 : readTo <= 0.0 )
			done = true;	// the box's far face along the axis
		if( oneStep )
			break;
	}

	const float tEnd = ( readTo - startA ) * sgn * invAbsDa;
	JahConeResult result;
	result.colour = color;
	result.alpha = alpha;
	result.lodLevel = lodLevel;
	result.posLS = posLS + tEnd * dirLS;
	result.travelled = max( startingTravelled + tEnd, vctInvResolution );
	result.travelledC0 = result.travelled;
	result.lastCascade = c;
	return result;
}

/// THE WALK: cascade 0 from `posLS0` (already off the surface - the caller's own start
/// bias, jahConeStart: one cell of cascade 0 along the normal), then every cascade out
/// while the cone is not yet opaque.
JahConeResult jahConeMarch( vec3 posLS0, vec3 dirLS, float tanHalfAngle, vec4 origin, uint flags )
{
	const bool specular = ( flags & JAH_MARCH_SPECULAR ) != 0u;
	// THE SHARE OF THE CONE BELOW ITS SURFACE (origin.w, the caller's jahConeBelow) is stopped
	// by the surface - the hemisphere's boundary is opaque from above - and starts the composite.
	JahConeResult result = jahConeMarchCascade( 0, posLS0, dirLS, tanHalfAngle, 0.0, origin.w, 0.0, origin, flags );
#if JAH_VOX_MAX_CASCADES > 1
	float toC0 = 1.0;	// cascade j's normalised units to cascade 0's, along dirLS
	for( int j = 1; j < JAH_VOX_COUNT && result.alpha < 0.95; ++j )
	{
		const float prevCascadeMaxLod = JAH_VOX_MAXLOD( j - 1 );
		const vec4 fromPrevScale = JAH_VOX_FROM_PREV_SCALE( j );
		const vec4 fromPrevOffset = JAH_VOX_FROM_PREV_OFFSET( j );

		// CONTIGUOUS: the next cascade reads on from the point the previous one had read
		// up to (its first plane is the one holding it, weighted by the unvisited part).
		const vec3 newPosLS = result.posLS * fromPrevScale.xyz + fromPrevOffset.xyz;
		// The origin plane rides the same map (its axis and sign are the cascades' own).
		if( origin.x >= 0.0 )
		{
			const int n = max( int( origin.x ), 0 );   // clamped: see jahVoxelReadPlane
			origin.y = origin.y * fromPrevScale[n] + fromPrevOffset[n];
		}

		const float hopScale = dot( abs( dirLS ), fromPrevScale.xyz );
		JahConeResult newRes = jahConeMarchCascade(
			j, newPosLS, dirLS, tanHalfAngle,
			specular ? 0.0 : max( result.lodLevel - prevCascadeMaxLod, 0.0 ), result.alpha,
			specular ? 0.0 : result.travelled * hopScale, origin, flags );

		if( specular )
		{
			// The specular hop weight: upstream's per-cascade brightness ramp on
			// the cascade multiplier, by how far into the new cascade's mips the
			// cone has got (its own "hacky" equalisation, kept as it stands).
			float strength = mix( 0.5, fromPrevScale.w,
								  clamp( newRes.lodLevel * fromPrevOffset.w, 0.0, 1.0 ) );
			result.colour += newRes.colour * strength;
		}
		else
		{
			result.colour += newRes.colour * fromPrevScale.w;
			result.travelled = newRes.travelled;
		}
		result.alpha = newRes.alpha;
		result.lodLevel = newRes.lodLevel;
		result.posLS = newRes.posLS;
		toC0 *= hopScale;
		result.travelledC0 = newRes.travelled / toC0;
		result.lastCascade = j;
	}
#endif
	return result;
}

#endif   // JAH_VOXEL_MARCH_GLSL
