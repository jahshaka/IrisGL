// THE PLANET'S ATMOSPHERE (SKY-ATMOSPHERE-1) — the one copy of the model's
// arithmetic. Hillaire, "A Scalable and Production Ready Sky and Atmosphere
// Rendering Technique" (EGSR 2020): a planet of radius Rb inside a shell of air
// to Rt, Rayleigh + Mie scattering with exponential density profiles and an
// ozone layer that only absorbs. Read by the four LUT jobs (JahAtmo*_cs.glsl),
// the sky quad and the cloud sheet (JahAtmoSky_ps.glsl, JahCloudLayer_ps.glsl,
// raw copies of this file) and the pixel shader's aerial perspective (the
// JahFog piece inserts the wrapped piece JahAtmosphere).
//
// UNITS: kilometres for every length, per kilometre for every coefficient. The
// LUTs hold radiance for a sun of UNIT illuminance at the top of the air; the
// consumers multiply by the sun's illuminance in the renderer's units.
//
// THE PARAMETER BLOCK, five vec4s every caller passes through:
//   planet   x = bottom radius, y = top radius, z = the observer's radius (the
//            LUTs are evaluated for ONE observer, at the ground), w = the
//            aerial-perspective volume's far distance
//   rayleigh rgb = scattering at the ground, w = scale height
//   mie      x = scattering at the ground, y = extinction at the ground,
//            z = scale height, w = the phase's asymmetry g
//   ozone    rgb = absorption at the layer's peak (a tent 30 km wide centred
//            at 25 km), w unused
//   ground   rgb = the planet's albedo (Lambert), w unused
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahAtmosphere):
// no character of it may be the Hlms directive mark, comments included.

#ifndef JAH_ATMOSPHERE_GLSL
#define JAH_ATMOSPHERE_GLSL

#define JAH_ATMO_PI 3.14159265358979

// ---- the medium -----------------------------------------------------------
float jahAtmoOzoneDensity( float h )
{
	return max( 0.0, 1.0 - abs( h - 25.0 ) / 15.0 );
}

// Scattering (Rayleigh rgb, Mie scalar) and extinction at altitude h (km).
// The altitude is held at the ground: a point below the planet's surface (a
// scene's basement, a ray the camera aims under the planet) breathes the air
// at sea level, never a denser one.
void jahAtmoMedium( float h, vec4 rayleigh, vec4 mie, vec4 ozone,
					out vec3 scatR, out float scatM, out vec3 ext )
{
	h = max( h, 0.0 );
	const float dR = exp( -h / rayleigh.w );
	const float dM = exp( -h / mie.z );
	scatR = rayleigh.rgb * dR;
	scatM = mie.x * dM;
	ext = scatR + vec3( mie.y * dM ) + ozone.rgb * jahAtmoOzoneDensity( h );
}

// ---- geometry -------------------------------------------------------------
// The nearest non-negative distance along a unit ray from ro to a sphere of
// `radius` at the origin, or -1.
float jahAtmoRaySphere( vec3 ro, vec3 rd, float radius )
{
	const float b = dot( ro, rd );
	const float c = dot( ro, ro ) - radius * radius;
	const float disc = b * b - c;
	if( disc < 0.0 )
		return -1.0;
	const float s = sqrt( disc );
	const float t0 = -b - s;
	const float t1 = -b + s;
	if( t1 < 0.0 )
		return -1.0;
	return t0 >= 0.0 ? t0 : t1;
}

// A LUT's texel centres span [0; 1] (Hillaire's sub-UVs): the value at a
// parameter's end lives in the end texel's centre, never half a texel beyond.
float jahAtmoToSubUv( float u, float res ) { return ( u + 0.5 / res ) * ( res / ( res + 1.0 ) ); }
float jahAtmoFromSubUv( float u, float res ) { return ( u - 0.5 / res ) * ( res / ( res - 1.0 ) ); }

// ---- the transmittance LUT: (radius, cos of the zenith angle) -------------
// Bruneton and Neyret's parameterisation: x = the distance to the top of the
// air along the ray, normalised between its minimum and maximum at this
// radius; y = the radius as the horizon distance rho over its maximum H.
vec2 jahAtmoTransmittanceUnit( float r, float mu, vec4 planet )
{
	const float Rb = planet.x, Rt = planet.y;
	r = clamp( r, Rb, Rt );
	const float H = sqrt( max( Rt * Rt - Rb * Rb, 0.0 ) );
	const float rho = sqrt( max( r * r - Rb * Rb, 0.0 ) );
	const float disc = r * r * ( mu * mu - 1.0 ) + Rt * Rt;
	const float d = max( 0.0, -r * mu + sqrt( max( disc, 0.0 ) ) );
	const float dMin = Rt - r;
	const float dMax = rho + H;
	return vec2( ( d - dMin ) / max( dMax - dMin, 1e-6 ), rho / H );
}

void jahAtmoTransmittanceRMu( vec2 unit, vec4 planet, out float r, out float mu )
{
	const float Rb = planet.x, Rt = planet.y;
	const float H = sqrt( max( Rt * Rt - Rb * Rb, 0.0 ) );
	const float rho = H * unit.y;
	r = sqrt( rho * rho + Rb * Rb );
	const float dMin = Rt - r;
	const float dMax = rho + H;
	const float d = dMin + unit.x * ( dMax - dMin );
	mu = d == 0.0 ? 1.0 : ( H * H - rho * rho - d * d ) / ( 2.0 * r * d );
	mu = clamp( mu, -1.0, 1.0 );
}

vec2 jahAtmoTransmittanceUv( float r, float mu, vec4 planet )
{
	const vec2 u = jahAtmoTransmittanceUnit( r, mu, planet );
	return vec2( jahAtmoToSubUv( u.x, 256.0 ), jahAtmoToSubUv( u.y, 64.0 ) );
}

// ---- the multiple-scattering LUT: (cos of the sun's zenith, altitude) -----
vec2 jahAtmoMultiScatterUv( float r, float muS, vec4 planet )
{
	const vec2 u = vec2( clamp( muS * 0.5 + 0.5, 0.0, 1.0 ),
						 clamp( ( r - planet.x ) / ( planet.y - planet.x ), 0.0, 1.0 ) );
	return vec2( jahAtmoToSubUv( u.x, 32.0 ), jahAtmoToSubUv( u.y, 32.0 ) );
}

// ---- directions: the sky-view parameterisation ------------------------------
// ONE mapping for the sky-view LUT and the aerial-perspective volume's two
// direction axes. u: the azimuth from the sun's own azimuth, sqrt-spaced so the
// sun's side gets the resolution; v: the view's zenith angle, the upper half
// [0; 0.5] the rays that reach space and the lower half [0.5; 1] the rays that
// hit the planet, both sqrt-spaced towards the HORIZON — the line the sky needs
// sharpest. The observer is at radius planet.z.
float jahAtmoHorizonBeta( vec4 planet )
{
	const float vHorizon = sqrt( max( planet.z * planet.z - planet.x * planet.x, 0.0 ) );
	return acos( clamp( vHorizon / planet.z, -1.0, 1.0 ) );
}

// cos of the angle between the view's and the sun's horizontal directions.
float jahAtmoLightViewCos( vec3 dir, vec3 toSun )
{
	const vec2 a = dir.xz, b = toSun.xz;
	const float la = dot( a, a ), lb = dot( b, b );
	if( la < 1e-12 || lb < 1e-12 )
		return 1.0;
	return clamp( dot( a, b ) * inversesqrt( la * lb ), -1.0, 1.0 );
}

// Unit coordinates of a world direction (y up); `below` = the ray hits the
// planet (the lower half).
vec2 jahAtmoDirUnit( vec3 dir, vec3 toSun, vec4 planet, out bool below )
{
	const float beta = jahAtmoHorizonBeta( planet );
	const float zenithHorizon = JAH_ATMO_PI - beta;
	const float viewZenith = acos( clamp( dir.y, -1.0, 1.0 ) );
	float v;
	below = viewZenith >= zenithHorizon;
	if( !below )
	{
		float c = viewZenith / zenithHorizon;
		c = 1.0 - sqrt( max( 1.0 - c, 0.0 ) );
		v = c * 0.5;
	}
	else
	{
		float c = ( viewZenith - zenithHorizon ) / max( beta, 1e-6 );
		v = sqrt( clamp( c, 0.0, 1.0 ) ) * 0.5 + 0.5;
	}
	const float u = sqrt( clamp( -jahAtmoLightViewCos( dir, toSun ) * 0.5 + 0.5, 0.0, 1.0 ) );
	return vec2( u, v );
}

// The inverse, for the jobs that fill a texel: the view's zenith cosine and its
// azimuth cosine from the sun.
void jahAtmoUnitDir( vec2 unit, vec4 planet, out float viewZenithCos, out float lightViewCos )
{
	const float beta = jahAtmoHorizonBeta( planet );
	const float zenithHorizon = JAH_ATMO_PI - beta;
	if( unit.y < 0.5 )
	{
		float c = 1.0 - 2.0 * unit.y;
		c = 1.0 - c * c;
		viewZenithCos = cos( zenithHorizon * c );
	}
	else
	{
		float c = 2.0 * unit.y - 1.0;
		viewZenithCos = cos( zenithHorizon + beta * c * c );
	}
	const float c = unit.x * unit.x;
	lightViewCos = -( c * 2.0 - 1.0 );
}

// The texture coordinate of a direction in a LUT `res` texels wide and high,
// HELD ON ITS OWN SIDE OF THE HORIZON: a ray that reaches space never reads a
// texel of the planet's half and the reverse, so the horizon is a line one
// pixel wide however coarse the LUT is (the bilinear fetch would otherwise
// blend the brightest sky with the darkest ground over a texel's height).
vec2 jahAtmoDirUv( vec3 dir, vec3 toSun, vec4 planet, vec2 res )
{
	bool below;
	vec2 unit = jahAtmoDirUnit( dir, toSun, planet, below );
	const float halfRows = res.y * 0.5;
	// the last texel centre of the upper half and the first of the lower, in
	// unit (texel-centre) coordinates
	const float lastUp = ( halfRows - 1.0 ) / ( res.y - 1.0 );
	const float firstDown = halfRows / ( res.y - 1.0 );
	unit.y = below ? max( unit.y, firstDown ) : min( unit.y, lastUp );
	return vec2( jahAtmoToSubUv( unit.x, res.x ), jahAtmoToSubUv( unit.y, res.y ) );
}

// The same, held in the UPPER half whatever the ray: the sky at the
// direction's azimuth just above the horizon for a ray below it. What a medium
// that hides the planet (the World fog) fades towards.
vec2 jahAtmoDirUvAbove( vec3 dir, vec3 toSun, vec4 planet, vec2 res )
{
	bool below;
	vec2 unit = jahAtmoDirUnit( dir, toSun, planet, below );
	const float lastUp = ( res.y * 0.5 - 1.0 ) / ( res.y - 1.0 );
	unit.y = below ? lastUp : min( unit.y, lastUp );
	return vec2( jahAtmoToSubUv( unit.x, res.x ), jahAtmoToSubUv( unit.y, res.y ) );
}

// ---- the aerial-perspective volume's depth axis -----------------------------
// `slices` slices, slice s at distance planet.w * ( s / ( slices - 1 ) )^2 — slice
// 0 is the eye itself (nothing in front of it), the near slices metres apart.
// Returns the texture coordinate for a distance in km.
float jahAtmoApW( float distKm, vec4 planet, float slices )
{
	const float s = sqrt( clamp( distKm / planet.w, 0.0, 1.0 ) ) * ( slices - 1.0 );
	return ( s + 0.5 ) / slices;
}
float jahAtmoApSliceKm( float s, vec4 planet, float slices )
{
	const float f = s / ( slices - 1.0 );
	return planet.w * f * f;
}

// ---- phase functions --------------------------------------------------------
float jahAtmoRayleighPhase( float cosTheta )
{
	return 3.0 / ( 16.0 * JAH_ATMO_PI ) * ( 1.0 + cosTheta * cosTheta );
}
// Henyey-Greenstein, peaked FORWARD (cosTheta = 1 is looking at the sun).
float jahAtmoMiePhase( float g, float cosTheta )
{
	const float g2 = g * g;
	const float denom = max( 1.0 + g2 - 2.0 * g * cosTheta, 1e-6 );
	return ( 1.0 - g2 ) / ( 4.0 * JAH_ATMO_PI * denom * sqrt( denom ) );
}

#endif
