// THE MEDIA ALONG A RAY THAT DID NOT START AT THE EYE (PHOTON-I-1 fix 5).
//
// A reflection's light crossed the medium twice: from what it reflects to the
// reflecting surface (the reflection ray's length), then from the surface to the
// eye. The colour pass fogs the second leg (JahFog_piece_vs_piece_ps.any, along the
// eye's ray); this file fogs the FIRST, with the same media in the same order and
// the same arithmetic, along the reflection ray from its origin:
//   1. upstream's distance block (HlmsPbs 800.PixelShader, under hlms_fog): the World
//      fog under any sky but the atmosphere, towards the authored colour, its
//      breakthrough bending it on the surface's luminance (the block's own
//      unclamped weight);
//   2. under the atmosphere (the passes' jah_atmo_ap): the air's aerial perspective at
//      the ray's direction and length, then the World fog towards the sky's radiance
//      above the horizon (breakthrough clamped to [0, 1]), then its height layer in
//      that colour; under any other sky: the height layer in the authored colour;
//   3. the exponential height fog (jah_height_fog.glsl, the one copy of its integral).
// The air's table is evaluated for the eye's observer: a reflection ray that starts
// metres from the eye reads it as if it started there (the table varies over
// kilometres). A ray that ESCAPED is the sky: the colour pass draws the sky with no
// distance fog and the height fog's quad over it at kHeightFogSkyDistance, so an
// escape takes the height fog alone, at that distance (jahFogAlongSky).
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
// and include jah_atmosphere.glsl and jah_height_fog.glsl before this file.
#ifndef JAH_FOG_ALONG_GLSL
#define JAH_FOG_ALONG_GLSL

/// The distance the colour passes fog the sky at, metres (Types.h kHeightFogSkyDistance).
const float kJahFogSkyDistance = 100000.0;

vec3 jahFogAlong( vec3 c, float eyeY, vec3 dir, float L )
{
	if( JAH_FOG_P( 3 ).w > 0.5 )
	{
		// 1. upstream's block, verbatim (an identity under the atmosphere: density 0).
		{
			const float lum = dot( c, vec3( 0.212655, 0.715158, 0.072187 ) );
			const float lumW = max( exp2( JAH_FOG_P( 2 ).z * lum + JAH_FOG_P( 2 ).y ), 0.0 );
			float w = exp2( -L * JAH_FOG_P( 2 ).x );
			w = mix( 1.0, w, lumW );
			c = mix( JAH_FOG_P( 1 ).z > 0.5 ? vec3( 0.0 ) : JAH_FOG_P( 0 ).xyz, c, w );
		}
		const float surfY = eyeY + dir.y * L;
		// The height layer's transmittance (JahFog: from the eye's height to the surface's).
		float layerT = 1.0;
		if( JAH_FOG_P( 0 ).w > 0.0 )
		{
			const float k = JAH_FOG_P( 1 ).x;
			const float a = exp2( clamp( -k * ( eyeY - JAH_FOG_P( 1 ).y ), -60.0, 60.0 ) );
			const float u = clamp( k * ( surfY - eyeY ) * 0.6931472, -60.0, 60.0 );
			const float ramp = abs( u ) < 0.001 ? ( 1.0 - 0.5 * u ) : ( ( 1.0 - exp( -u ) ) / u );
			layerT = exp2( -JAH_FOG_P( 0 ).w * a * L * ramp );
		}
		if( JAH_FOG_P( 1 ).z > 0.5 )
		{
			// 2. the air, then the World fog and its layer towards the sky above.
			const vec4 planet = JAH_FOG_P( 5 );
			const vec4 ap = JAH_FOG_AERIAL( vec3( jahAtmoDirUv( dir, JAH_FOG_P( 4 ).xyz, planet, vec2( 32.0, 64.0 ) ),
												  jahAtmoApW( L * 0.001 * JAH_FOG_P( 2 ).w, planet, 32.0 ) ) );
			const vec3 surf = c;
			vec3 o = surf * ap.w + ap.xyz * JAH_FOG_P( 3 ).xyz;
			const vec3 fogLin =
				JAH_FOG_AERIAL( vec3( jahAtmoDirUvAbove( dir, JAH_FOG_P( 4 ).xyz, planet, vec2( 32.0, 64.0 ) ),
									  jahAtmoApSkyW( 32.0 ) ) ).xyz * JAH_FOG_P( 3 ).xyz;
			const float lum = dot( surf, vec3( 0.212655, 0.715158, 0.072187 ) );
			const float brk = clamp( exp2( JAH_FOG_P( 2 ).z * lum + JAH_FOG_P( 2 ).y ), 0.0, 1.0 );
			const float td = mix( 1.0, exp2( -L * JAH_FOG_P( 1 ).w ), brk );
			o = mix( fogLin, o, td );
			if( JAH_FOG_P( 0 ).w > 0.0 )
				o = mix( fogLin, o, layerT );
			c = o;
		}
		else if( JAH_FOG_P( 0 ).w > 0.0 )
			c = mix( JAH_FOG_P( 0 ).xyz, c, layerT );
	}
	// 3. the exponential height fog.
	if( JAH_FOG_P( 7 ).w > 0.5 )
		c = mix( JAH_FOG_P( 7 ).xyz, c, jahHeightFogT( eyeY, dir.y, L, JAH_FOG_P( 6 ) ) );
	return c;
}

/// An ESCAPED ray: the sky, which the colour pass fogs with the height fog alone.
vec3 jahFogAlongSky( vec3 c, float eyeY, vec3 dir )
{
	if( JAH_FOG_P( 7 ).w > 0.5 )
		c = mix( JAH_FOG_P( 7 ).xyz, c, jahHeightFogT( eyeY, dir.y, kJahFogSkyDistance, JAH_FOG_P( 6 ) ) );
	return c;
}

#endif   // JAH_FOG_ALONG_GLSL
