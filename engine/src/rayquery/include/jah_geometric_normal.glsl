// THE GEOMETRIC NORMAL AT A PIXEL (PHOTON-7 stage 6, the ray origin's direction; READ_A finding 1) - one text for every
// ray job that leaves a surface the depth buffer shows (the reflection's trace, the sun contact).
//
// The G-buffer's normal is the SHADED one (normal maps, smoothed vertex normals); a ray must leave the surface the
// depth buffer actually drew, so the lift that keeps it outside the traced geometry runs along the GEOMETRIC normal -
// the cross of the two screen axes' steps to neighbour points of the SAME surface.
//
// WHICH NEIGHBOUR IS THE SAME SURFACE: per axis, the side whose SECOND neighbour continues the plane through this pixel
// and the first - the two-tap extrapolation residual |2 d1 - d2 - d0| of the RAW depth (an affine function of the
// image coordinate on any plane, under either projection: 1 / z is affine under perspective, z under orthographic), the
// smaller side taken. Never the nearer in depth: at a CONCAVE contact seen from the foreshortened side (a floor pixel a
// fraction of a row below a wall's or a foot's base) the nearer neighbour is the one ACROSS THE CREASE - the floor's
// own step down is the larger - so the normal came out as the wall's, the lift went sideways, and with tMin 0 about half
// of those texels hit their own floor at t ~ 0: a one-texel line of wrong reflection along every contact (READ_A
// finding 1). A side with only one neighbour on a drawn surface has no residual and is taken only where the other side
// has none either; an axis with no neighbour at all, a degenerate cross, or a normal more than 60 degrees from the
// shaded one (dot < 0.5: a silhouette pixel whose both neighbours lie on other surfaces) answers the shaded normal.
//
// THE HOST DEFINES, before including this file:
//   JAH_GN_RAW( q )           the raw depth at the full-resolution pixel q (ivec2); <= 0 or >= 1 = nothing drawn
//   JAH_GN_WORLD( q, raw )    the world point the pixel q shows at that raw depth (its centre)
// No at-sign anywhere in this file (the house rule for every shared include).
#ifndef JAH_GEOMETRIC_NORMAL_GLSL
#define JAH_GEOMETRIC_NORMAL_GLSL

/// The geometric normal at the pixel `px` of a `fullRes` target whose surface point is `world` at raw depth `raw0`,
/// facing the viewer `V` (unit, toward the eye); `shadedN` where the depth cannot say.
vec3 jahGeometricNormal( ivec2 px, ivec2 fullRes, float raw0, vec3 world, vec3 shadedN, vec3 V )
{
	vec3 axisStep[2];
	for( int a = 0; a < 2; ++a )
	{
		const ivec2 e = a == 0 ? ivec2( 1, 0 ) : ivec2( 0, 1 );
		float bestResidual = 3.0e38;
		bool found = false, foundWithResidual = false;
		for( int s = -1; s <= 1; s += 2 )
		{
			const ivec2 q1 = px + e * s;
			if( any( lessThan( q1, ivec2( 0 ) ) ) || any( greaterThanEqual( q1, fullRes ) ) )
				continue;
			const float r1 = JAH_GN_RAW( q1 );
			if( !( r1 > 0.0 && r1 < 1.0 ) )
				continue;
			const ivec2 q2 = px + e * ( 2 * s );
			float residual = 3.0e38;
			bool hasResidual = false;
			if( all( greaterThanEqual( q2, ivec2( 0 ) ) ) && all( lessThan( q2, fullRes ) ) )
			{
				const float r2 = JAH_GN_RAW( q2 );
				if( r2 > 0.0 && r2 < 1.0 )
				{
					residual = abs( 2.0 * r1 - r2 - raw0 );
					hasResidual = true;
				}
			}
			// a side with a residual beats one without; between two with, the smaller residual
			const bool better = !found || ( hasResidual && !foundWithResidual ) ||
								( hasResidual == foundWithResidual && residual < bestResidual );
			if( !better )
				continue;
			bestResidual = residual;
			found = true;
			foundWithResidual = hasResidual;
			axisStep[a] = ( JAH_GN_WORLD( q1, r1 ) - world ) * float( s );
		}
		if( !found )
			return shadedN;
	}
	vec3 n = cross( axisStep[0], axisStep[1] );
	const float l2 = dot( n, n );
	if( !( l2 > 0.0 ) || !( l2 < 3.0e38 ) )
		return shadedN;
	n *= inversesqrt( l2 );
	n = dot( n, V ) >= 0.0 ? n : -n;
	return dot( n, shadedN ) < 0.5 ? shadedN : n;
}

#endif   // JAH_GEOMETRIC_NORMAL_GLSL
