// THE PLANET'S ATMOSPHERE, LUT 4 OF 4 — THE AERIAL PERSPECTIVE (SKY-ATMOSPHERE-1).
//
// A 3D table: two axes are the view DIRECTION in the sky view's own
// parameterisation (the azimuth from the sun, the zenith angle sqrt-spaced
// towards the horizon), the third is DISTANCE — slice s at planet.w * (s / (N-1))^2,
// slice 0 the eye itself. Each texel is the light the air scatters towards the
// eye between the eye and that distance along that direction (rgb, a sun of
// unit illuminance, single + multiple scattering) and the mean transmittance
// over the same stretch (a) — what every lit pixel, the cloud sheet and the
// sun's surroundings are seen through: pixel = surface * a + rgb * E.
//
// ONE TABLE FOR EVERY CAMERA. Parameterised by direction rather than by a
// camera's frustum, it answers the editor's view, every probe face, a planar
// mirror and both eyes of a headset alike, and it is rebuilt only when the
// model or the sun moves — never per frame, never per view. The observer is
// the model's (at the ground); a surface below the planet's surface breathes
// sea-level air.
//
// One thread per direction marches the whole column, four midpoint steps a
// slice, writing each slice as it passes it.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D transmittanceLut;
vulkan_layout( ogre_t1 ) uniform texture2D multiScatterLut;
vulkan( layout( ogre_s0 ) uniform sampler lutSampler0 );
vulkan( layout( ogre_s1 ) uniform sampler lutSampler1 );

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image3D aerialLut;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 atmoPlanet;
	uniform vec4 atmoRayleigh;
	uniform vec4 atmoMie;
	uniform vec4 atmoOzone;
	uniform vec4 atmoGround;
	uniform vec4 atmoSun;
vulkan( }; )

@insertpiece( JahAtmosphere )

vec3 jahTransmittance( float r, float mu )
{
	return textureLod( sampler2D( transmittanceLut, lutSampler0 ),
					   jahAtmoTransmittanceUv( r, mu, atmoPlanet ), 0.0 ).rgb;
}
vec3 jahMultiScatter( float r, float muS )
{
	return textureLod( sampler2D( multiScatterLut, lutSampler1 ),
					   jahAtmoMultiScatterUv( r, muS, atmoPlanet ), 0.0 ).rgb;
}

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	const ivec3 size = imageSize( aerialLut );
	const ivec2 px = ivec2( gl_GlobalInvocationID.xy );
	if( px.x >= size.x || px.y >= size.y )
		return;
	const vec2 unit = vec2( clamp( jahAtmoFromSubUv( ( float( px.x ) + 0.5 ) / float( size.x ), float( size.x ) ), 0.0, 1.0 ),
							clamp( jahAtmoFromSubUv( ( float( px.y ) + 0.5 ) / float( size.y ), float( size.y ) ), 0.0, 1.0 ) );
	float viewZenithCos, lightViewCos;
	jahAtmoUnitDir( unit, atmoPlanet, viewZenithCos, lightViewCos );
	const float muS = clamp( atmoSun.y, -1.0, 1.0 );
	const vec3 toSun = vec3( sqrt( max( 1.0 - muS * muS, 0.0 ) ), muS, 0.0 );
	const float sinZ = sqrt( max( 1.0 - viewZenithCos * viewZenithCos, 0.0 ) );
	const vec3 rd = vec3( sinZ * lightViewCos, viewZenithCos,
						  sinZ * sqrt( max( 1.0 - lightViewCos * lightViewCos, 0.0 ) ) );
	const vec3 ro = vec3( 0.0, atmoPlanet.z, 0.0 );
	const float cosTheta = dot( rd, toSun );
	const float phaseR = jahAtmoRayleighPhase( cosTheta );
	const float phaseM = jahAtmoMiePhase( atmoMie.w, cosTheta );
	const float slices = float( size.z );

	vec3 L = vec3( 0.0 );
	vec3 throughput = vec3( 1.0 );
	imageStore( aerialLut, ivec3( px, 0 ), vec4( 0.0, 0.0, 0.0, 1.0 ) );
	const int kSub = 4;
	for( int s = 1; s < size.z; ++s )
	{
		const float a = jahAtmoApSliceKm( float( s - 1 ), atmoPlanet, slices );
		const float b = jahAtmoApSliceKm( float( s ), atmoPlanet, slices );
		const float dt = ( b - a ) / float( kSub );
		for( int k = 0; k < kSub; ++k )
		{
			vec3 p = ro + rd * ( a + ( float( k ) + 0.5 ) * dt );
			float pr = length( p );
			// under the planet's surface the air (and the sun on it) is the ground's
			if( pr < atmoPlanet.x + 1e-3 )
			{
				p *= ( atmoPlanet.x + 1e-3 ) / pr;
				pr = atmoPlanet.x + 1e-3;
			}
			const vec3 up = p / pr;
			vec3 scatR, ext;
			float scatM;
			jahAtmoMedium( pr - atmoPlanet.x, atmoRayleigh, atmoMie, atmoOzone, scatR, scatM, ext );
			const vec3 stepT = exp( -ext * dt );
			const float sunCos = dot( up, toSun );
			const float shadow = jahAtmoRaySphere( p, toSun, atmoPlanet.x - 1e-3 ) >= 0.0 ? 0.0 : 1.0;
			const vec3 S = shadow * jahTransmittance( pr, sunCos ) * ( scatR * phaseR + vec3( scatM * phaseM ) ) +
						   jahMultiScatter( pr, sunCos ) * ( scatR + vec3( scatM ) );
			L += throughput * ( S - S * stepT ) / max( ext, vec3( 1e-9 ) );
			throughput *= stepT;
		}
		imageStore( aerialLut, ivec3( px, s ),
					vec4( L, dot( throughput, vec3( 1.0 / 3.0 ) ) ) );
	}
}
