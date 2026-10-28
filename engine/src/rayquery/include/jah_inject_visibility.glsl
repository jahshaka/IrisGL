// THE VOXEL CHAIN'S SHADOW MARCH TO A LAMP - ONE TEXT for every reader of the voxels' lamp visibility (PHOTON-7 10-4 R9,
// the lead's ruling 2026-10-27): the light injection's own direct term (LightInjection_piece_cs.any, where it was born),
// the no-ray route's area light strata (JahScreenAreaVis_cs.glsl) and the colour passes' Forward+ lamps at the no-ray
// tier (JahLampRay_piece_ps.any's voxel arm). Moved here from the injection's piece JahInjectionReader word for word (its
// types spelled in GLSL); the build wraps it as the piece JahInjectVisibility.
//
// THE CONSUMER'S NAMES (macros or declarations before it): p_chainSelf (the march's own entry), p_chainOrigin[j] (entry
// j's box corner relative to cascade 0's, metres; w its cell), p_chainRes[j] (its resolution, xyz; w the world distance
// its shell ends at), JAH_INJ_CHAIN (how many entries) and the four per-cascade arrays chainCovP, chainCovN, chainPosP,
// chainPosN (texelFetch'd at level 0). Needs jahVoxelOriginGate (jah_voxel_sample.glsl, the piece JahVoxelSample) and
// jahVoxelFacesCross (jah_voxel_faces.glsl) before it.
//
// THE AT-SIGN RULE: this file becomes Hlms input (the piece JahInjectVisibility), so no character of it may be the Hlms
// directive mark, comments included.

#ifndef JAH_INJECT_VISIBILITY_GLSL
#define JAH_INJECT_VISIBILITY_GLSL

// THE CROSSING TOLERANCE, in cells: the cell-relative position's quantum is 1/65535 of a cell and a ray's coordinate
// at a texel plane carries a few float ulps of the box (~1e-5 cells at 128): a face ON a texel plane (a wall one cell
// thick, whose faces are both texel planes) is crossed where the ray enters or leaves its voxel, within this.
#define JAH_INJ_TOL 1e-4f

// THE EXACT SHADOW MARCH (PHOTON-VOXEL-5; spikes/photon-voxel-5/lab3). A 3D-DDA over the level-0
// voxels from the start toward the lamp: in every voxel the ray passes, each half's SURFACE
// PLANE along each axis (its stored position) that the ray crosses inside that voxel stops it
// by the half's coverage - from either side, like any shadow ray (a face hidden inside another
// solid must be shadowed by that solid's back face) - capped per axis like one surface, the faces it enters and those
// it leaves counted by the inside fraction's law (below); the
// ORIGIN PLANE (the lit face itself, axis `oAxis` at `oCoord`, facing `oSign`) never, through
// the one reader's gate. The march it replaces stepped one cell from the voxel's centre and
// took a coverage times the cells crossed: a wall sharing the lit face's cell was never read,
// and at 1.875 m cells every wall voxel of a room holds two walls (the leak room's corner: a
// side wall's inner face lit by the lamp outside, 21 of 36 cones red; this march, 0).
// Everything here is in the CELLS of the cascade being marched (PHOTON-7 stage 1): `vLo` the voxel's low face
// along b, `f` its half's stored O x place-in-its-cell, `sb` / `db` the ray's start and step along b.
float jahInjectPlane( float o, float f, int b, float lo, float hi, float sb, float db, float vLo,
							 int oAxis, float oCoord, float oSign, float hSign, out float tCross )
{
	tCross = 0.0f;
	if( !( o > 0.0f ) )
		return 0.0f;
	// The surfaces of this half lie in THIS voxel: the stored position is their place in the voxel's own cell
	// (16-bit, PHOTON-7 stage 1: the same bits at every placement), so the plane is the voxel's low face plus that
	// share of the cell; the crossing is tested with JAH_INJ_TOL.
	const float pp = clamp( vLo + f / o, vLo, vLo + 1.0f );
	if( !( pp >= lo - JAH_INJ_TOL && pp <= hi + JAH_INJ_TOL ) )
		return 0.0f;
	// where along the ray the plane is crossed (the march takes a voxel's faces in that order)
	tCross = ( pp - sb ) / db;
	// THE ORIGIN'S OWN SURFACES ONLY (decision 22.2): the gate keeps the march from stopping on the lit face's own
	// quantised planes - faces looking the way the origin looks (hSign == oSign). A face looking BACK at it within
	// the gate's cell and a half is another surface (a plate's end facing a wall 0.15 m off it): an occluder, and
	// the inside fraction must see it enter, or the solid's far face reads as an open surface seen from behind.
	// (In cells: the gate's one fine cell is 1. Only the origin's own cascade passes an origin: oAxis -1 else.)
	if( b == oAxis && hSign == oSign )
	{
		const float g = jahVoxelOriginGate( pp * o, o, vec4( float( oAxis ), oCoord, oSign, 0.0f ), 1.0f );
		if( g < 1.0f )
			return o * g;
	}
	// a plane through the start is the start's own surface
	return abs( pp - sb ) > JAH_INJ_TOL ? o : 0.0f;
}

/// THE CASTER SHARE of axis a from a surface-position texel's alpha (FAR-SUN-1; packed 5 bits
/// an axis by VoxelMerge_piece_cs.any's voxelMergeCasterShare).
float jahInjectCasterShare( float alpha, int a )
{
	const uint packed = uint( alpha * 65535.0f + 0.5f );
	return float( ( packed >> uint( 5 * a ) ) & 31u ) * ( 1.0f / 31.0f );
}

/// A NON-FINITE value, by its bits (trap 5: a comparison is no NaN test under the compiler's fast math).
bool jahInjNonFinite( float x ) { return ( floatBitsToUint( x ) & 0x7F800000u ) == 0x7F800000u; }
bool jahInjNonFinite4( vec4 v )
{
	return jahInjNonFinite( v.x ) || jahInjNonFinite( v.y ) || jahInjNonFinite( v.z ) || jahInjNonFinite( v.w );
}

/// THE TRANSMITTANCE from `sW` (metres from THIS cascade's box corner, inside voxel `ivSelf` of it) along the unit
/// direction `w` to the lamp at `tEnd` metres (a directional light: past any box) - THROUGH THE CHAIN ON FIXED
/// SHELLS (PHOTON-7 stage 1, L1: LIGHT IS A WORLD FUNCTION; PREFLIGHT_1_2 §3). It used to end at this cascade's box,
/// so a voxel's direct term depended on where the box stood (the court's walk vs teleport, 15 codes). Shell j of
/// the march runs on cascade j's level-0 cells over the world distance [D_(j-1), D_j) from the start, D_j the sum of
/// eight cells of every cascade up to j (Epic 0.625 / 1.875 / 5.625 m, Medium 1.25 / 3.75 / 7.5, Low 1.25), ENDING
/// ON THE FIRST CELL PLANE OF CASCADE j + 1 PAST D_j - so the next shell enters its first coarse cell at a face and
/// reads it whole (a coarse plane is the coverage-weighted mean of the fine surfaces: entered mid-way it would
/// re-count what the fine march took) - and the last cascade to its exit; past it the sun is unoccluded and a lamp
/// reached. The shells are a function of the world point, the direction and the lattices, never of a box, wherever
/// they lie inside their boxes; a shell that leaves its box first (THE BAND: a voxel within its shell's reach of its
/// box's face) hands over there, mid-cell, and that voxel is the placement's (the stage's named residual, gated
/// out of gi.light_is_world). A cascade whose box does not hold the point the march has reached is passed over: the
/// next one reads on from it. AN OUTER LOOP OVER THE SHELLS, the DDA inside: the chain's arrays are indexed by the
/// loop's own counter, the same for every invocation still marching.
float jahInjectVisibility( vec3 sW, vec3 w, float tEnd, ivec3 ivSelf, int oAxis, float oCoord, float oSign )
{
	// THE INSIDE FRACTION (SUN-INJECTION-RES-1, ENGINE-UPGRADE-U5 stage 5 and decision 22.2). A ray that crosses a
	// solid meets it twice - where it enters (a FRONT face, looking back at the ray) and where it leaves (a BACK face,
	// looking the ray's way) - and both faces carry the same coverage of the same rows of the cell: a 0.1 m plate's
	// two ends each cover 0.71 of a 0.14 m cell's face, ONE band 0.1 m tall. Summed, the plate stopped 1.42 of the
	// ray (the face column's row at the plate's height read 0.643 / 0.000 / 0.155 of the face at 32 / 64 / 128 cells
	// against the coverage's 0.643 / 0.286 / 0.571); taking the larger of the entries and the exits counted a solid
	// once but let TWO OPEN single-sided surfaces facing away from each other along the ray (foliage cards, an
	// awning over a slat: the voxeliser puts a one-sided face in the half its normal looks to) through as one solid
	// - 0.4 of the sun past two 0.6 cards. So the march carries T, the light left, and f, the fraction of the ray's
	// cross-section INSIDE a solid, and takes every face it crosses IN THE ORDER IT CROSSES THEM:
	//   a FRONT face of coverage c blocks (T x (1 - c)) and enters (f += c (1 - f));
	//   a BACK face of coverage c first leaves what was entered (min( c, f ) leaves f and blocks nothing - the
	//   solid's front already did), and its REMAINDER c - min( c, f ) is an open surface seen from behind,
	//   which blocks (T x (1 - remainder)).
	// One solid crossed end to end: once. Two solids: twice. A march starting inside a solid: stopped by its exit
	// (nothing entered). Two open faces back to back: (1 - c)^2. T and f carry from shell to shell.
	float T = 1.0f, fIn = 0.0f;
	float tCur = 0.0f;
	// (a ray not whole - a non-finite start, direction or end - meets no chain: nothing it could cross is known)
	if( jahInjNonFinite( sW.x ) || jahInjNonFinite( sW.y ) || jahInjNonFinite( sW.z ) || jahInjNonFinite( w.x ) ||
		jahInjNonFinite( w.y ) || jahInjNonFinite( w.z ) || jahInjNonFinite( tEnd ) )
		return T;
	for( uint j = p_chainSelf; j < uint( JAH_INJ_CHAIN ) && T > 0.0f && tCur < tEnd; ++j )
	{
		// A CHAIN ENTRY NOT WHOLE (a non-finite origin, cell or shell end, or a cell not positive: its bits) is NO
		// CHAIN for the ray from here - the march ends, never a step count no exit bounds (PHOTON-7 stage 5: a
		// non-finite parameter defeats every exit of the DDA below)
		if( jahInjNonFinite4( p_chainOrigin[j] ) || jahInjNonFinite4( p_chainRes[j] ) ||
			floatBitsToInt( p_chainOrigin[j].w ) <= 0 )
			break;
		const float cellJ = p_chainOrigin[j].w;
		// THIS SHELL'S END: the first cell plane of the next cascade crossed past D_j, or the lamp
		float tShell = tEnd;
		if( j + 1u < uint( JAH_INJ_CHAIN ) && p_chainRes[j].w < tShell )
		{
			const float dJ = p_chainRes[j].w;
			const float cellN = jahInjNonFinite( p_chainOrigin[j + 1u].w ) || floatBitsToInt( p_chainOrigin[j + 1u].w ) <= 0
									? cellJ   // (the next entry not whole: this shell ends on its own lattice)
									: p_chainOrigin[j + 1u].w;
			const vec3 qN = ( sW + w * dJ - p_chainOrigin[j + 1u].xyz ) / cellN;
			for( int a = 0; a < 3; ++a )
			{
				if( w[a] == 0.0f )
					continue;
				const float plane = w[a] > 0.0f ? floor( qN[a] ) + 1.0f : ceil( qN[a] ) - 1.0f;
				tShell = min( tShell, dJ + ( plane - qN[a] ) * cellN / w[a] );
			}
		}
		// THE MARCH IN CASCADE j's CELLS: the start (t = 0) and the step per metre, and the voxel the march enters
		// at tCur (the origin's own voxel in this cascade; else the one the ray enters past the hand-over's plane)
		const vec3 qS = ( sW - p_chainOrigin[j].xyz ) / cellJ;
		const vec3 dq = w / cellJ;
		const ivec3 res = ivec3( p_chainRes[j].xyz + 0.5f );
		ivec3 iv = ivSelf;
		if( j != p_chainSelf )
			iv = ivec3( floor( qS + dq * tCur + sign( dq ) * 1e-4f ) );
		if( iv.x < 0 || iv.y < 0 || iv.z < 0 || iv.x >= res.x || iv.y >= res.y || iv.z >= res.z )
			continue;	// this box does not hold the point: the next cascade reads on from it
		const int oAxisJ = j == p_chainSelf ? oAxis : -1;
		ivec3 stp;
		vec3 tMax, tDel;
		float tStop = tShell;
		for( int a = 0; a < 3; ++a )
		{
			stp[a] = dq[a] > 0.0f ? 1 : -1;
			if( dq[a] == 0.0f )
			{
				tMax[a] = 1e30f;
				tDel[a] = 1e30f;
				continue;
			}
			tMax[a] = ( float( iv[a] ) + ( dq[a] > 0.0f ? 1.0f : 0.0f ) - qS[a] ) / dq[a];
			tDel[a] = 1.0f / abs( dq[a] );
			tStop = min( tStop, ( dq[a] > 0.0f ? float( res[a] ) - qS[a] : -qS[a] ) / dq[a] );
		}
		float tIn = tCur;
		const int maxSteps = res.x + res.y + res.z + 3;
		for( int it = 0; it < maxSteps && T > 0.0f; ++it )
		{
			const float tOut = min( min( min( tMax.x, tMax.y ), tMax.z ), tStop );
			const vec3 cP = texelFetch( chainCovP[j], iv, 0 ).xyz;
			const vec3 cN = texelFetch( chainCovN[j], iv, 0 ).xyz;
			if( cP.x + cP.y + cP.z + cN.x + cN.y + cN.z > 0.0f )
			{
				const vec4 pP4 = texelFetch( chainPosP[j], iv, 0 );
				const vec4 pN4 = texelFetch( chainPosN[j], iv, 0 );
				// THE VOXEL'S FACES THE RAY CROSSES: up to two per axis (a front, a back), each with where it is crossed.
				float evT[6], evC[6];
				bool evFront[6];
				int evN = 0;
				// THE CASTER SHARE PER HALF-AXIS (FAR-SUN-1; VoxelMerge_piece_cs.any): only the
				// surfaces that CAST a shadow stop the march - the shadow maps' caster set - while
				// every surface still bounces. 1 in a voxel of casters: the pre-lane march exactly.
				for( int b = 0; b < 3; ++b )
				{
					if( dq[b] == 0.0f )
						continue;
					const float e0 = qS[b] + dq[b] * tIn, e1 = qS[b] + dq[b] * tOut;
					const float lo = min( e0, e1 ), hi = max( e0, e1 );
					const float vLo = float( iv[b] );
					float tP, tN;
					const float takeP =
						jahInjectPlane( cP[b], pP4[b], b, lo, hi, qS[b], dq[b], vLo, oAxisJ, oCoord, oSign, 1.0f, tP ) *
							jahInjectCasterShare( pP4.w, b );
					const float takeN =
						jahInjectPlane( cN[b], pN4[b], b, lo, hi, qS[b], dq[b], vLo, oAxisJ, oCoord, oSign, -1.0f, tN ) *
							jahInjectCasterShare( pN4.w, b );
					// the faces looking +b face a ray travelling -b (it enters through them), and the other way
					const bool pFront = dq[b] < 0.0f;
					if( takeP > 0.0f )
					{
						evT[evN] = tP;
						evC[evN] = min( takeP, 1.0f );
						evFront[evN] = pFront;
						++evN;
					}
					if( takeN > 0.0f )
					{
						evT[evN] = tN;
						evC[evN] = min( takeN, 1.0f );
						evFront[evN] = !pFront;
						++evN;
					}
				}
				// IN THE ORDER THE RAY CROSSES THEM - the one face model (jah_voxel_faces.glsl, the piece JahVoxelFaces:
				// the occupancy ray's transmissive cells take the same function, PHOTON-7 10-4 R7)
				jahVoxelFacesCross( evT, evC, evFront, evN, T, fIn );
			}
			if( tOut >= tStop )
				break;
			const int a = tMax.x <= tMax.y ? ( tMax.x <= tMax.z ? 0 : 2 ) : ( tMax.y <= tMax.z ? 1 : 2 );
			iv[a] += stp[a];
			if( iv[a] < 0 || iv[a] >= res[a] )
				break;
			tIn = tMax[a];
			tMax[a] += tDel[a];
		}
		tCur = tStop;
	}
	return max( 0.0f, T );
}

#endif   // JAH_INJECT_VISIBILITY_GLSL
