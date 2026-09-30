// THE EXPONENTIAL HEIGHT FOG (SKY-DEFAULTS-1) — the one copy of its arithmetic,
// read by the PBS pixel shader (the JahFog piece inserts the wrapped piece
// JahHeightFog) and the height fog's sky quad (JahHeightFogSky_ps.glsl, a raw
// copy of this file). Types.h HeightFogDesc states the model.
//
// UNREAL'S LINE INTEGRAL, VERBATIM (HeightFogCommon: GetExponentialHeightFog),
// in metres, with its start distance handled as its later form does it: the ray
// begins AT the start distance, and the density there is the origin term.
//
//   density(y) = D * 2^( -k * (y - y0) )
//   origin     = D * 2^( -k * (yStart - y0) )      yStart = the ray's height at s
//   F          = k * (rise of the segment [s, L])
//   integral   = origin * (L - s) * (1 - 2^-F) / F  (Taylor ln2 - ln2^2/2 F near 0)
//   T          = 2^( -integral )
//
// THE PARAMETERS, one vec4: x = D (per metre), y = k (per metre), z = y0 (world
// Y), w = s (metres). Everything else is the ray: the camera's world height, the
// unit direction's Y, the distance to the surface (or the sky distance).
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahHeightFog): no
// character of it may be the Hlms directive mark, comments included.

#ifndef JAH_HEIGHT_FOG_GLSL
#define JAH_HEIGHT_FOG_GLSL

// The transmittance (1 = clear) between the eye and `dist` along a ray of
// unit-direction height `dirY`, from an eye at world height `camY`.
float jahHeightFogT( float camY, float dirY, float dist, vec4 fog )
{
	const float segment = dist - fog.w;
	if( !( fog.x > 0.0 ) || !( segment > 0.0 ) )
		return 1.0;
	const float yStart = camY + dirY * fog.w;
	const float origin = fog.x * exp2( -max( -127.0, fog.y * ( yStart - fog.z ) ) );
	const float f = max( -127.0, fog.y * dirY * segment );
	const float perLength = abs( f ) > 1.0e-4 ? ( 1.0 - exp2( -f ) ) / f
										   : ( 0.6931472 - 0.2402265 * f );
	const float integral = origin * perLength * segment;
	// An overflowed integral is a fully fogged ray (a ray under the horizon at
	// the sky distance): exp2 of minus infinity is zero, never a NaN, because
	// every factor above is finite and non-negative or the clamp held it.
	return clamp( exp2( -integral ), 0.0, 1.0 );
}

#endif
