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
//   at every hop, upstream's behaviour): its escape rides the raw directional
//   alpha, which over-states occlusion at a coarse mip, and the true (wider)
//   footprint fed to that estimate took the reflected sky from 1.00/0.85/0.85
//   to 1.00/0.43/0.34 at one/two/four cascades (gi.cascades case 7); the cure
//   is the specular escape reading the occupancy estimate too — its own lane.
//
//   THE ESCAPE ESTIMATE (0021, 0084). `alpha` is the colour composite's
//   opacity and decides when the march stops; `escapeAlpha` is the separate
//   estimate of how much of the cone was stopped at all, which is what the
//   ambient (the sky) rides. They are the same accumulation wherever the
//   isotropic volume is read and differ only in the anisotropic stretch, where
//   the escape reads jahVoxelOccupancy (why: its comment, in the sample file).
//
//   THE HOP (0070 bias, 0033 assign, 0074 single transmittance). A cone that
//   leaves a cascade — its footprint has grown to the next cascade's cell, or
//   it has left the box — continues in the next one from where it stopped,
//   stepped back one start bias along the measure direction (upstream's gap
//   term) and forward ONE CELL OF THE NEW CASCADE along the bias direction
//   (componentwise: a unit direction times the per-axis inverse resolution is
//   one cell long for every direction; the scalar L1 step is 1.73 cells along a
//   diagonal and banded a sphere along six arcs). The continuation starts its
//   accumulators at the running totals, so what it returns ALREADY carries the
//   transmittance of every cascade before it: the colour is ADDED once (never
//   times 1 - alpha again — that squared the transmittance and darkened every
//   outer cascade by it), and alpha / escape are ASSIGNED (adding them doubled
//   the opacity per hop and ended every walk after one cascade).
//
//   THE EXIT per cascade: the isotropic stretch runs while the mip is below the
//   hand-over mip (isotropic volumes) or at most half a mip (anisotropic ones,
//   whose own stretch then runs to the hand-over mip); both stop at an opacity
//   of 0.95 or once the footprint's box has left the cascade's.

#ifndef JAH_VOXEL_MARCH_GLSL
#define JAH_VOXEL_MARCH_GLSL

/// THE WALK'S VARIANTS, as literal flags a caller passes (the compiler folds
/// every branch on them).
///   DIFFUSE is the default: the per-cascade hand-over mip, the age carried at
///   a hop, the colour hop weighted by the cascade multiplier.
const uint JAH_MARCH_SPECULAR = 1u;	///< maxLod 11 per cascade, the mip from the footprint, no age carry, the specular hop weight
const uint JAH_MARCH_SDF = 2u;		///< the specular empty-space skip (a single volume only, as upstream)
const uint JAH_MARCH_LODSTEP = 4u;	///< the four-cone diffuse set's fixed mip step
const uint JAH_MARCH_GAP_ALONG_DIR = 8u;	///< the hop's step back is measured along the cone, not along the bias direction

struct JahConeResult
{
	vec3 colour;		///< cascade 0's stored units (JAH_VOX_FROM_PREV_SCALE(c).w converts)
	float alpha;		///< the colour composite's opacity
	float escapeAlpha;	///< the escape estimate's opacity (== alpha on the isotropic path)
	float lodLevel;
	vec3 posLS;			///< where the march stopped, in lastCascade's normalised space
	float travelled;	///< the cone's age, in lastCascade's normalised units
	float travelledC0;	///< the same age in cascade 0's normalised units
	int lastCascade;
};

/// ONE CASCADE's march. `startingTravelled` is the age the cone arrives with
/// (0 on the first cascade and on every specular hop).
JahConeResult jahConeMarchCascade( int c, vec3 posLS, vec3 dirLS, float tanHalfAngle,
								   float startingLodLevel, float startingAlpha,
								   float startingEscapeAlpha, float startingTravelled, uint flags )
{
	bool specular = ( flags & JAH_MARCH_SPECULAR ) != 0u;
	vec4 invRes_maxLod = vec4( JAH_VOX_INVRES( c ), JAH_VOX_MAXLOD( c ) );
	float vctInvResolution = dot( abs( dirLS ), invRes_maxLod.xyz );
	float resolution = 1.0 / vctInvResolution;
	float maxLod = specular ? 11.0 : invRes_maxLod.w;	// 2048^3 is the largest possible volume

	float dist = vctInvResolution;
	float travelled = max( startingTravelled, vctInvResolution );
	float alpha = startingAlpha;
	float escapeAlpha = startingEscapeAlpha;
	vec3 color = vec3( 0.0, 0.0, 0.0 );

	float diameter = max( vctInvResolution, 2.0 * tanHalfAngle * travelled );

	float lodLevel = specular ? log2( diameter * resolution ) : startingLodLevel;
	float skipLod = 1.0;

	vec3 nextPosLS = posLS + dist * dirLS;

	// An AABB against an AABB: the unit box and a box centred on the sample
	// whose half-size is half the footprint.
	float threshold = 0.5 + diameter * 0.5;

#if JAH_VOX_HAS_ANISO
	bool aniso = JAH_VOX_ANISO;
#else
	bool aniso = false;
#endif
	while( alpha < 0.95 &&
		   abs( nextPosLS - 0.5 ).x <= threshold &&
		   abs( nextPosLS - 0.5 ).y <= threshold &&
		   abs( nextPosLS - 0.5 ).z <= threshold &&
		   ( aniso ? lodLevel <= 0.5 : lodLevel < maxLod ) )
	{
		threshold = 0.5 + diameter * 0.5;

		vec4 sampleColour = JAH_VOX_SAMPLE_ISO( c, nextPosLS, lodLevel );

#ifdef JAH_VOX_SDF_FACTOR
		if( ( flags & JAH_MARCH_SDF ) != 0u )
		{
			// THE EMPTY-SPACE SKIP (upstream's, specular only, one volume only):
			// a near-mirror cone is a path trace and marching it cell by cell is
			// slow, so the opacity of a coarser mip stands in for a distance
			// field and the step grows across empty space. Empirically tuned
			// upstream; it hurts quality once the cone widens, which is what the
			// blend on the cone angle turns it off by.
			float finalOpac = JAH_VOX_SAMPLE_ISO( c, nextPosLS, skipLod ).w;
			float skipFactor = exp2( max( 0.0, skipLod * 0.5 - 1.0 ) ) * ( 1.0 - finalOpac ) +
							   finalOpac;
			skipFactor = mix( skipFactor, 1.0,
							  min( -1.0 + finalOpac * JAH_VOX_SDF_FACTOR + tanHalfAngle * 50.0,
								   1.0 ) );
			skipLod = clamp( skipLod + ( 1.0 - finalOpac ) * 2.0 - 1.0, 1.0, JAH_VOX_SDF_MAXMIP );

			dist += diameter * 0.5 * skipFactor;
			travelled += diameter * 0.5 * skipFactor;
		}
		else
#endif
		{
			dist += diameter * 0.5;
			travelled += diameter * 0.5;
		}

		float a = ( 1.0 - alpha );

		color += sampleColour.xyz * a;
		alpha += a * sampleColour.w;
		escapeAlpha += ( 1.0 - escapeAlpha ) * sampleColour.w;
		nextPosLS = posLS + dist * dirLS;
		diameter = max( vctInvResolution, 2.0 * tanHalfAngle * travelled );
		if( !specular && ( flags & JAH_MARCH_LODSTEP ) != 0u )
			lodLevel += 1.0;
		else
			lodLevel = log2( diameter * resolution );
	}

#if JAH_VOX_HAS_ANISO
	if( aniso )
	{
		while( alpha < 0.95 &&
			   lodLevel < maxLod &&
			   abs( nextPosLS.x - 0.5 ) <= threshold &&
			   abs( nextPosLS.y - 0.5 ) <= threshold &&
			   abs( nextPosLS.z - 0.5 ) <= threshold )
		{
			threshold = 0.5 + diameter * 0.5;

			vec3 sampleUVW = jahVoxelAnisoUvw( nextPosLS );
			vec4 sampleColour = jahVoxelSampleAniso( c, sampleUVW, dirLS, lodLevel );

			float a = ( 1.0 - alpha );

			color += sampleColour.xyz * a;
			alpha += a * sampleColour.w;
			escapeAlpha += ( 1.0 - escapeAlpha ) * jahVoxelOccupancy( c, sampleUVW, lodLevel );

			dist += diameter * 0.5;
			travelled += diameter * 0.5;
			nextPosLS = posLS + dist * dirLS;
			diameter = max( vctInvResolution, 2.0 * tanHalfAngle * travelled );
			if( !specular && ( flags & JAH_MARCH_LODSTEP ) != 0u )
				lodLevel += 1.0;
			else
				lodLevel = log2( diameter * resolution );
		}
	}
#endif

	JahConeResult result;
	result.colour = color;
	result.alpha = alpha;
	result.escapeAlpha = escapeAlpha;
	result.lodLevel = lodLevel;
	result.posLS = nextPosLS;
	result.travelled = travelled;
	result.travelledC0 = travelled;
	result.lastCascade = c;
	return result;
}

/// THE WALK: cascade 0 from `posLS0` (already off the surface — the caller's
/// own start bias), then every cascade out while the cone is not yet opaque.
///   `biasDirLS`  the hop's bias direction in cascade 0's normalised space (the
///                surface's geometric normal; zero for a point in free space)
///   `measureDirLS`  what the hop's step back is measured along when
///                JAH_MARCH_GAP_ALONG_DIR is not set (the same normal,
///                unnormalised as the caller holds it)
JahConeResult jahConeMarch( vec3 posLS0, vec3 dirLS, float tanHalfAngle, vec3 biasDirLS,
							vec3 measureDirLS, uint flags )
{
	bool specular = ( flags & JAH_MARCH_SPECULAR ) != 0u;
	JahConeResult result = jahConeMarchCascade( 0, posLS0, dirLS, tanHalfAngle, 0.0, 0.0, 0.0,
												0.0, flags );
#if JAH_VOX_MAX_CASCADES > 1
	float toC0 = 1.0;	// cascade j's normalised units to cascade 0's, along dirLS
	for( int j = 1; j < JAH_VOX_COUNT && result.alpha < 0.95; ++j )
	{
		vec3 gapDir = ( flags & JAH_MARCH_GAP_ALONG_DIR ) != 0u ? dirLS : measureDirLS;
		float startBias = dot( abs( gapDir ), JAH_VOX_INVRES( j ) );
		float prevCascadeMaxLod = JAH_VOX_MAXLOD( j - 1 );
		vec4 fromPrevScale = JAH_VOX_FROM_PREV_SCALE( j );
		vec4 fromPrevOffset = JAH_VOX_FROM_PREV_OFFSET( j );

		vec3 newPosLS = ( result.posLS * fromPrevScale.xyz + fromPrevOffset.xyz ) -
						dirLS * startBias + biasDirLS * JAH_VOX_INVRES( j );

		float hopScale = dot( abs( dirLS ), fromPrevScale.xyz );
		JahConeResult newRes = jahConeMarchCascade(
			j, newPosLS, dirLS, tanHalfAngle, max( result.lodLevel - prevCascadeMaxLod, 0.0 ),
			result.alpha, result.escapeAlpha, specular ? 0.0 : result.travelled * hopScale,
			flags );

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
		result.escapeAlpha = newRes.escapeAlpha;
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
