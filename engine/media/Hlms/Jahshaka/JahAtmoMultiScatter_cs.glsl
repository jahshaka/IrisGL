// THE PLANET'S ATMOSPHERE, LUT 2 OF 4 — MULTIPLE SCATTERING (SKY-ATMOSPHERE-1).
//
// 32 x 32 texels, (the sun's zenith cosine, the altitude): Hillaire's
// Psi_ms — the light scattered two and more times that reaches a point, as an
// isotropic source. From the point, 64 directions over the sphere integrate
// the second order L_2 (single scattering of a unit sun, isotropic phase, the
// lit ground included) and f_ms, the fraction of isotropic light the
// surroundings send back; every higher order is the geometric series
// L_2 / ( 1 - f_ms ). One thread per texel: the job runs only when a dial of
// the model changes.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D transmittanceLut;
vulkan( layout( ogre_s0 ) uniform sampler lutSampler );

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D multiScatterLut;

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
	return textureLod( sampler2D( transmittanceLut, lutSampler ),
					   jahAtmoTransmittanceUv( r, mu, atmoPlanet ), 0.0 ).rgb;
}

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	const ivec2 px = ivec2( gl_GlobalInvocationID.xy );
	const ivec2 size = imageSize( multiScatterLut );
	if( px.x >= size.x || px.y >= size.y )
		return;
	const float u = clamp( jahAtmoFromSubUv( ( float( px.x ) + 0.5 ) / float( size.x ), float( size.x ) ), 0.0, 1.0 );
	const float v = clamp( jahAtmoFromSubUv( ( float( px.y ) + 0.5 ) / float( size.y ), float( size.y ) ), 0.0, 1.0 );
	const float muS = u * 2.0 - 1.0;
	const float r = atmoPlanet.x + v * ( atmoPlanet.y - atmoPlanet.x ) + 1e-3;
	const vec3 ro = vec3( 0.0, r, 0.0 );
	const vec3 toSun = vec3( sqrt( max( 1.0 - muS * muS, 0.0 ) ), muS, 0.0 );
	const float isotropic = 1.0 / ( 4.0 * JAH_ATMO_PI );

	const int kSqrt = 8;
	const int kSteps = 20;
	vec3 sumL = vec3( 0.0 );
	vec3 sumF = vec3( 0.0 );
	for( int j = 0; j < kSqrt; ++j )
	{
		for( int i = 0; i < kSqrt; ++i )
		{
			// uniform over the sphere: phi even, cos(theta) even
			const float phi = 2.0 * JAH_ATMO_PI * ( float( i ) + 0.5 ) / float( kSqrt );
			const float cosT = 1.0 - 2.0 * ( float( j ) + 0.5 ) / float( kSqrt );
			const float sinT = sqrt( max( 1.0 - cosT * cosT, 0.0 ) );
			const vec3 rd = vec3( sinT * cos( phi ), cosT, sinT * sin( phi ) );
			const float tBottom = jahAtmoRaySphere( ro, rd, atmoPlanet.x );
			const float tTop = jahAtmoRaySphere( ro, rd, atmoPlanet.y );
			const bool hitsGround = tBottom > 0.0;
			const float tMax = hitsGround ? tBottom : max( tTop, 0.0 );
			const float dt = tMax / float( kSteps );
			vec3 throughput = vec3( 1.0 );
			vec3 L = vec3( 0.0 );
			vec3 F = vec3( 0.0 );
			for( int s = 0; s < kSteps; ++s )
			{
				const vec3 p = ro + rd * ( ( float( s ) + 0.5 ) * dt );
				const float pr = length( p );
				const vec3 up = p / pr;
				vec3 scatR, ext;
				float scatM;
				jahAtmoMedium( pr - atmoPlanet.x, atmoRayleigh, atmoMie, atmoOzone, scatR, scatM, ext );
				const vec3 scat = scatR + vec3( scatM );
				const vec3 stepT = exp( -ext * dt );
				const float sunCos = dot( up, toSun );
				const float shadow = jahAtmoRaySphere( p, toSun, atmoPlanet.x - 1e-3 ) >= 0.0 ? 0.0 : 1.0;
				const vec3 S = scat * isotropic * shadow * jahTransmittance( pr, sunCos );
				const vec3 safeExt = max( ext, vec3( 1e-9 ) );
				L += throughput * ( S - S * stepT ) / safeExt;
				F += throughput * ( scat - scat * stepT ) / safeExt;
				throughput *= stepT;
			}
			if( hitsGround )
			{
				const vec3 p = ro + rd * tBottom;
				const vec3 up = normalize( p );
				const float sunCos = dot( up, toSun );
				L += throughput * jahTransmittance( atmoPlanet.x, sunCos ) * max( sunCos, 0.0 ) *
					 atmoGround.rgb / JAH_ATMO_PI;
			}
			sumL += L;
			sumF += F;
		}
	}
	// each direction stands for 4 pi / 64 of the sphere, and the isotropic
	// phase of the NEXT event is 1 / 4 pi: the two cancel into a plain mean
	const float n = float( kSqrt * kSqrt );
	const vec3 L2 = sumL / n;
	const vec3 fms = sumF / n;
	const vec3 psi = L2 / max( vec3( 1.0 ) - fms, vec3( 1e-4 ) );
	imageStore( multiScatterLut, px, vec4( psi, 1.0 ) );
}
