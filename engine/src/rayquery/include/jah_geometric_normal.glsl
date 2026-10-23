// THE GEOMETRIC NORMAL AT A PIXEL (PHOTON-7 stage 6, the ray origin's direction; READ_A finding 1) - one text for every
// ray job that leaves a surface the depth buffer shows (the reflection's trace, the sun contact).
//
// The G-buffer's normal is the SHADED one (normal maps, smoothed vertex normals); a ray must leave the surface the
// depth buffer actually drew, so the lift that keeps it outside the traced geometry runs along the GEOMETRIC normal -
// the cross of the two screen axes' steps to neighbour points of the SAME surface.
//
// WHICH NEIGHBOURS ARE THE SAME SURFACE: per axis, the taps that CONTINUE THE PLANE through this pixel - the
// two-tap extrapolation residual of the RAW depth (an affine function of the image coordinate on any plane, under either
// projection: 1 / z is affine under perspective, z under orthographic), on each side |2 d1 - d2 - d0| and centred
// |d-1 + d+1 - 2 d0|, the smallest taken. Never the nearer in depth: at a CONCAVE contact seen from the foreshortened
// side (a floor pixel a fraction of a row below a wall's or a foot's base) the nearer neighbour is the one ACROSS THE
// CREASE, the normal came out as the wall's and the lift went sideways (READ_A finding 1).
// THE FALLBACK IS THE RECONSTRUCTION'S OWN FAILURE, NEVER AN ANGLE (READ_B finding 6): a drawn surface is triangles and
// the rasteriser's depth is affine across a triangle, so the residual of three taps on one plane is the depth's rounding
// alone - kGnQuantumUlps units in the last place of the largest tap (a 32-bit float depth). An axis none of whose three
// candidates is within that quantum (a silhouette pixel, a feature one or two pixels wide, triangles under two pixels, a
// border pixel whose inward taps cross a crease) has no plane through the pixel: the SHADING normal answers, and the
// caller's lift and fold both take it (the fold then has nothing to do: a direction is drawn above the shading plane) -
// never a sideways pick. The 60-degree test it replaces discarded a CORRECT geometric normal far from a strongly
// tilted shading one (a normal map, smoothed normals over a coarse cut), and with it the fold: a lobe under the true
// plane hit its own surface at t ~ 0 (rtreflect.tilted_normal).
//
// THE HOST DEFINES, before including this file:
//   JAH_GN_RAW( q )           the raw depth at the full-resolution pixel q (ivec2); <= 0 or >= 1 = nothing drawn
//   JAH_GN_WORLD( q, raw )    the world point the pixel q shows at that raw depth (its centre)
// No at-sign anywhere in this file (the house rule for every shared include).
#ifndef JAH_GEOMETRIC_NORMAL_GLSL
#define JAH_GEOMETRIC_NORMAL_GLSL

/// The rasteriser's depth rounding in units in the last place: an affine depth evaluated at three pixel centres of one
/// triangle has a second difference of a few units at most.
const float kGnQuantumUlps = 8.0;
float jahGnUlp( float x )
{
	return exp2( floor( log2( max( abs( x ), 1e-30 ) ) ) - 23.0 );
}
bool jahGnDrawn( ivec2 q, ivec2 fullRes, out float raw )
{
	raw = 0.0;
	if( any( lessThan( q, ivec2( 0 ) ) ) || any( greaterThanEqual( q, fullRes ) ) )
		return false;
	raw = JAH_GN_RAW( q );
	return raw > 0.0 && raw < 1.0;
}

/// The geometric normal at the pixel `px` of a `fullRes` target whose surface point is `world` at raw depth `raw0`,
/// facing the viewer `V` (unit, toward the eye); `shadedN` where the depth has no plane through the pixel.
vec3 jahGeometricNormal( ivec2 px, ivec2 fullRes, float raw0, vec3 world, vec3 shadedN, vec3 V )
{
	vec3 axisStep[2];
	for( int a = 0; a < 2; ++a )
	{
		const ivec2 e = a == 0 ? ivec2( 1, 0 ) : ivec2( 0, 1 );
		float rm1, rm2, rp1, rp2;
		const bool m1 = jahGnDrawn( px - e, fullRes, rm1 ), m2 = jahGnDrawn( px - 2 * e, fullRes, rm2 );
		const bool p1 = jahGnDrawn( px + e, fullRes, rp1 ), p2 = jahGnDrawn( px + 2 * e, fullRes, rp2 );
		float best = 3.0e38;
		int pick = 0;   // 1 = the +1 side, -1 = the -1 side, 2 = centred
		if( p1 && p2 )
		{
			const float r = abs( 2.0 * rp1 - rp2 - raw0 );
			if( r <= kGnQuantumUlps * jahGnUlp( max( raw0, max( rp1, rp2 ) ) ) && r < best ) { best = r; pick = 1; }
		}
		if( m1 && m2 )
		{
			const float r = abs( 2.0 * rm1 - rm2 - raw0 );
			if( r <= kGnQuantumUlps * jahGnUlp( max( raw0, max( rm1, rm2 ) ) ) && r < best ) { best = r; pick = -1; }
		}
		if( m1 && p1 )
		{
			const float r = abs( rm1 + rp1 - 2.0 * raw0 );
			if( r <= kGnQuantumUlps * jahGnUlp( max( raw0, max( rm1, rp1 ) ) ) && r < best ) { best = r; pick = 2; }
		}
		if( pick == 0 )
			return shadedN;   // no plane continues through this pixel along this axis
		axisStep[a] = pick == 1 ? JAH_GN_WORLD( px + e, rp1 ) - world
					: pick == -1 ? world - JAH_GN_WORLD( px - e, rm1 )
								 : 0.5 * ( JAH_GN_WORLD( px + e, rp1 ) - JAH_GN_WORLD( px - e, rm1 ) );
	}
	vec3 n = cross( axisStep[0], axisStep[1] );
	const float l2 = dot( n, n );
	if( !( l2 > 0.0 ) || !( l2 < 3.0e38 ) )
		return shadedN;
	n *= inversesqrt( l2 );
	return dot( n, V ) >= 0.0 ? n : -n;
}

#endif   // JAH_GEOMETRIC_NORMAL_GLSL
