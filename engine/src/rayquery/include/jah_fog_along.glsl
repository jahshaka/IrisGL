// THE FOG LAW — the media between a surface and whoever looks at it, ONE copy
// (PHOTON-I-1). The colour pass applies it along the eye's ray (the JahFog piece
// inserts this file, wrapped into the piece JahFogAlong, and maps its pass
// buffer onto the parameters below); the ray jobs apply it along a REFLECTION's
// ray (rq_reflect.comp, rq_hit_composite.comp include it), and the screen
// reflection's resolve re-fogs a screen hit with it (JahSsrResolve_ps.glsl, the
// raw copy). Nobody restates it.
//
// A reflection's light crossed the medium twice: from what it reflects to the
// reflecting surface (the reflection ray's length), then from the surface to the
// eye. The colour pass fogs the second leg; the first is fogged here, with the
// same media in the same order, as the mirror's VIRTUAL EYE sees it: the
// reflection ray extended back by the eye's distance to its origin. Every medium
// but the exponential height fog is a function of the leg alone; that one starts
// at its start distance from the EYE, which the second leg has already covered
// (`startUsed`), so the first leg never applies it a second time.
//
// THE ORDER (what the colour pass does to a surface, outermost last):
//   0. upstream's distance block — HlmsPbs 800.PixelShader_piece_ps.any, under
//      hlms_fog: the World fog under any sky but the atmosphere, towards the
//      authored colour, its breakthrough bending it on the surface's luminance
//      (the block's own unclamped weight). THE ONE MIRRORED BLOCK: the colour pass
//      runs upstream's own text, the ray jobs this copy of it
//      (jahFogUpstreamDistance), and gi.reflect_fog holds the two together;
//   1. under the atmosphere (the passes' jah_atmo_ap): the air's aerial
//      perspective at the ray's direction and length, then the World fog towards
//      the sky's radiance above the horizon (breakthrough clamped to [0, 1]),
//      then its height layer in that colour; under any other sky: the height
//      layer in the authored colour;
//   2. the exponential height fog (jah_height_fog.glsl, the one copy of its
//      integral).
// Steps 1 and 2 are jahFogMedia, the function both the colour pass and the ray
// jobs call. The air's table is evaluated for the eye's observer: a reflection ray
// that starts metres from the eye reads it as if it started there (the table
// varies over kilometres). A ray that ESCAPED is the sky: the colour pass draws
// the sky with no distance fog and the height fog's quad over it at
// kHeightFogSkyDistance, so an escape takes the height fog alone, at that
// distance (jahFogAlongSky).
//
// HOW A CALLER BINDS IT (define these first):
//     JAH_FOG_P( k )          vec4, k = 0..7 — OgreScene::fogAlong's eight:
//       0  rgb = the World fog's authored colour, w = the height layer's density
//       1  x = the height layer's falloff, y = its reference height, z = 1 under the
//          atmosphere (the air's table bound), w = the World fog's density there
//       2  x = upstream's block density, y/z = its breakthrough pair (the block's
//          packing: min x falloff, -falloff), w = the air's aerial scale
//       3  xyz = the sun at the top of the air (skyE), w = 1 when the passes fog at all
//       4  the sun's direction, 5 the planet (bottom, top, observer, aerial far, km)
//       6  the height fog (density, falloff, base height, start distance)
//       7  rgb = the height fog's colour, w = 1 when it is on
//     JAH_FOG_AERIAL( uvw )   vec4: the aerial table at uvw (only read when P1.z is 1)
//     JAH_FOG_HAS_AIR         defined when jah_atmosphere.glsl is included before
//                             this file (the branch is compiled out otherwise)
//     JAH_FOG_HAS_HEIGHT_FOG  defined when jah_height_fog.glsl is included before
//                             this file (ditto)
// The colour pass compiles each branch only in the permutation that reads it, so
// its P1.z and P7.w are constants there.
//
// THIS FILE IS HLMS INPUT (the build wraps it into the piece JahFogAlong): no
// character of it may be the Hlms directive mark, comments included.
#ifndef JAH_FOG_ALONG_GLSL
#define JAH_FOG_ALONG_GLSL

/// The distance the colour passes fog the sky at, metres (Types.h kHeightFogSkyDistance).
const float kJahFogSkyDistance = 100000.0;

/// Steps 1 and 2 on `c` (linear radiance): a ray from height `originY` reaching a
/// surface at height `surfY` after `L` metres in world direction `dir`, which began
/// `startUsed` metres from the eye (0 for the eye's own ray).
vec3 jahFogMedia( vec3 c, float originY, float surfY, vec3 dir, float L, float startUsed )
{
	// THE HEIGHT LAYER's transmittance: density( y ) = D * 2^( -k * ( y - y0 ) ),
	// integrated along the ray. With u = k * dy * ln2 the integral is
	// D * A * L * (1 - e^-u) / u, where A = 2^( -k * ( originY - y0 ) ), L the ray's
	// length and dy the rise from the origin to the surface. (1 - e^-u) / u tends to 1
	// as u -> 0 (a level ray sees constant density); the clamps keep the exponentials
	// finite for extreme falloffs instead of producing NaNs.
	float layerT = 1.0;
	if( JAH_FOG_P( 0 ).w > 0.0 )
	{
		const float k = JAH_FOG_P( 1 ).x;
		const float a = exp2( clamp( -k * ( originY - JAH_FOG_P( 1 ).y ), -60.0, 60.0 ) );
		const float u = clamp( k * ( surfY - originY ) * 0.6931472, -60.0, 60.0 );
		const float ramp = abs( u ) < 0.001 ? ( 1.0 - 0.5 * u ) : ( ( 1.0 - exp( -u ) ) / u );
		layerT = exp2( -JAH_FOG_P( 0 ).w * a * L * ramp );
	}
#ifdef JAH_FOG_HAS_AIR
	if( JAH_FOG_P( 1 ).z > 0.5 )
	{
		// THE AIR (SKY-ATMOSPHERE-1): the aerial-perspective table at this ray and
		// this distance times the aerial scale — the light the air between the
		// origin and the surface scatters in (xyz, times the sun at the top of the
		// air) and what of the surface survives it (w). Scale 0 reads the table's
		// first slice, the origin itself: exactly (0, 0, 0, 1).
		const vec4 planet = JAH_FOG_P( 5 );
		const vec4 ap = JAH_FOG_AERIAL( vec3( jahAtmoDirUv( dir, JAH_FOG_P( 4 ).xyz, planet, vec2( 32.0, 64.0 ) ),
											  jahAtmoApW( L * 0.001 * JAH_FOG_P( 2 ).w, planet, 32.0 ) ) );
		vec3 o = c * ap.w + ap.xyz * JAH_FOG_P( 3 ).xyz;
		// THE WORLD FOG, ON TOP: its own medium, fogging towards the sky just above
		// the horizon at this azimuth (a fog thick enough to hide the ground reads
		// as the horizon sky behind it — no line). The breakthrough bends it on the
		// unfogged colour's luminance, clamped to [0; 1] (upstream's max( exp2, 0 )
		// is 1.017 at luminance 0 with the shipped pair, which lerped PAST the fog
		// colour); then the height layer in the same sky colour.
		const vec3 fogLin =
			JAH_FOG_AERIAL( vec3( jahAtmoDirUvAbove( dir, JAH_FOG_P( 4 ).xyz, planet, vec2( 32.0, 64.0 ) ),
								  jahAtmoApSkyW( 32.0 ) ) ).xyz * JAH_FOG_P( 3 ).xyz;
		const float lum = dot( c, vec3( 0.212655, 0.715158, 0.072187 ) );
		const float brk = clamp( exp2( JAH_FOG_P( 2 ).z * lum + JAH_FOG_P( 2 ).y ), 0.0, 1.0 );
		const float td = mix( 1.0, exp2( -L * JAH_FOG_P( 1 ).w ), brk );
		o = mix( fogLin, o, td );
		if( JAH_FOG_P( 0 ).w > 0.0 )
			o = mix( fogLin, o, layerT );
		c = o;
	}
	else
#endif
	if( JAH_FOG_P( 0 ).w > 0.0 )
		c = mix( JAH_FOG_P( 0 ).xyz, c, layerT );
#ifdef JAH_FOG_HAS_HEIGHT_FOG
	// The exponential height fog, from the start distance off the EYE: the ray
	// extended back by `startUsed` to the (virtual) eye, the start no nearer than
	// the origin.
	if( JAH_FOG_P( 7 ).w > 0.5 )
	{
		const vec4 hf = startUsed > 0.0 ? vec4( JAH_FOG_P( 6 ).xyz, max( JAH_FOG_P( 6 ).w, startUsed ) )
										: JAH_FOG_P( 6 );
		c = mix( JAH_FOG_P( 7 ).xyz, c,
				 jahHeightFogT( surfY - dir.y * ( L + startUsed ), dir.y, L + startUsed, hf ) );
	}
#endif
	return c;
}

/// Step 0, upstream's distance block (the one mirrored block, above).
vec3 jahFogUpstreamDistance( vec3 c, float L )
{
	const float lum = dot( c, vec3( 0.212655, 0.715158, 0.072187 ) );
	const float lumW = max( exp2( JAH_FOG_P( 2 ).z * lum + JAH_FOG_P( 2 ).y ), 0.0 );
	float w = exp2( -L * JAH_FOG_P( 2 ).x );
	w = mix( 1.0, w, lumW );
	return mix( JAH_FOG_P( 1 ).z > 0.5 ? vec3( 0.0 ) : JAH_FOG_P( 0 ).xyz, c, w );
}

/// A reflection ray from `originY` (which the eye sees `eyeDistance` away) that met
/// a surface after `L` metres: the whole law along it.
vec3 jahFogAlong( vec3 c, float originY, vec3 dir, float L, float eyeDistance )
{
	if( JAH_FOG_P( 3 ).w > 0.5 )
		c = jahFogUpstreamDistance( c, L );
	return jahFogMedia( c, originY, originY + dir.y * L, dir, L, eyeDistance );
}

/// An ESCAPED reflection ray: the sky, which the colour pass fogs with the height
/// fog alone, at the sky's distance from the (virtual) eye.
vec3 jahFogAlongSky( vec3 c, float originY, vec3 dir, float eyeDistance )
{
#ifdef JAH_FOG_HAS_HEIGHT_FOG
	if( JAH_FOG_P( 7 ).w > 0.5 )
	{
		const vec4 hf = vec4( JAH_FOG_P( 6 ).xyz, max( JAH_FOG_P( 6 ).w, eyeDistance ) );
		c = mix( JAH_FOG_P( 7 ).xyz, c,
				 jahHeightFogT( originY - dir.y * eyeDistance, dir.y, kJahFogSkyDistance, hf ) );
	}
#endif
	return c;
}

#endif   // JAH_FOG_ALONG_GLSL
