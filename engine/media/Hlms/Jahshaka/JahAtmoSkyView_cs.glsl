// THE PLANET'S ATMOSPHERE, LUT 3 OF 4 — THE SKY VIEW (SKY-ATMOSPHERE-1).
//
// 192 x 108 texels over every direction the observer can look (the sky-view
// parameterisation, jahAtmoDirUnit: the horizon gets the resolution): the
// radiance of the sky for a sun of unit illuminance — single scattering with
// the Rayleigh and Mie phases, the multiple scattering from LUT 2, and, for a
// ray that hits the planet, the planet itself: its albedo lit by the sun, seen
// through the air in front of it. That is
// the darker band under a sharp horizon. Rebuilt when the model or the sun
// moves; the sky pass and the World fog read it.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D transmittanceLut;
vulkan_layout( ogre_t1 ) uniform texture2D multiScatterLut;
vulkan( layout( ogre_s0 ) uniform sampler lutSampler0 );
vulkan( layout( ogre_s1 ) uniform sampler lutSampler1 );

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D skyViewLut;

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
	const ivec2 px = ivec2( gl_GlobalInvocationID.xy );
	const ivec2 size = imageSize( skyViewLut );
	if( px.x >= size.x || px.y >= size.y )
		return;
	const vec2 unit = vec2( clamp( jahAtmoFromSubUv( ( float( px.x ) + 0.5 ) / float( size.x ), float( size.x ) ), 0.0, 1.0 ),
							clamp( jahAtmoFromSubUv( ( float( px.y ) + 0.5 ) / float( size.y ), float( size.y ) ), 0.0, 1.0 ) );
	float viewZenithCos, lightViewCos;
	jahAtmoUnitDir( unit, atmoPlanet, viewZenithCos, lightViewCos );
	// The local frame: the sun's azimuth along +x.
	const float muS = clamp( atmoSun.y, -1.0, 1.0 );
	const vec3 toSun = vec3( sqrt( max( 1.0 - muS * muS, 0.0 ) ), muS, 0.0 );
	const float sinZ = sqrt( max( 1.0 - viewZenithCos * viewZenithCos, 0.0 ) );
	const vec3 rd = vec3( sinZ * lightViewCos, viewZenithCos,
						  sinZ * sqrt( max( 1.0 - lightViewCos * lightViewCos, 0.0 ) ) );
	const vec3 ro = vec3( 0.0, atmoPlanet.z, 0.0 );
	// A ray aimed a hair under the horizon from the observer grazes the ground:
	// the lower half of the table is exactly the rays that meet it.
	const bool below = unit.y >= 0.5;
	float tBottom = jahAtmoRaySphere( ro, rd, atmoPlanet.x );
	const float tTop = max( jahAtmoRaySphere( ro, rd, atmoPlanet.y ), 0.0 );
	if( below && tBottom < 0.0 )
		tBottom = sqrt( max( atmoPlanet.z * atmoPlanet.z - atmoPlanet.x * atmoPlanet.x, 0.0 ) );
	const bool hitsGround = below && tBottom > 0.0;
	const float tMax = hitsGround ? tBottom : tTop;

	const float cosTheta = dot( rd, toSun );
	const float phaseR = jahAtmoRayleighPhase( cosTheta );
	const float phaseM = jahAtmoMiePhase( atmoMie.w, cosTheta );
	const int kSteps = 32;
	vec3 L = vec3( 0.0 );
	vec3 throughput = vec3( 1.0 );
	for( int s = 0; s < kSteps; ++s )
	{
		// quadratic steps, the sample 0.3 of the way into its segment (Hillaire)
		const float t0 = tMax * ( float( s ) / float( kSteps ) ) * ( float( s ) / float( kSteps ) );
		const float t1 = tMax * ( float( s + 1 ) / float( kSteps ) ) * ( float( s + 1 ) / float( kSteps ) );
		const float t = t0 + ( t1 - t0 ) * 0.3;
		const float dt = t1 - t0;
		const vec3 p = ro + rd * t;
		const float pr = length( p );
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
	if( hitsGround )
	{
		// THE PLANET: Lambert, lit by the sun through the air above it (the
		// reference's ground term; the sky's own light on the far ground is
		// not carried), seen through the air in front of it.
		const vec3 p = ro + rd * tBottom;
		const vec3 up = normalize( p );
		const float sunCos = dot( up, toSun );
		const vec3 sun = jahTransmittance( atmoPlanet.x, sunCos ) * max( sunCos, 0.0 );
		L += throughput * atmoGround.rgb / JAH_ATMO_PI * sun;
	}
	imageStore( skyViewLut, px, vec4( L, 1.0 ) );
}
