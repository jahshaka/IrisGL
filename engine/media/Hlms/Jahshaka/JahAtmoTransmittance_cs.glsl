// THE PLANET'S ATMOSPHERE, LUT 1 OF 4 — TRANSMITTANCE (SKY-ATMOSPHERE-1).
//
// 256 x 64 texels, (the ray's zenith cosine, the radius) in Bruneton's
// parameterisation (jahAtmoTransmittanceUnit): the fraction of light that
// crosses the air from a point at radius r along direction mu to the top of the
// atmosphere, per channel — Beer-Lambert over the optical depth of the three
// species, 40 midpoint steps. The planet is not in it: a consumer tests the
// ground itself. Rebuilt only when a dial of the model changes.
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) )
uniform restrict writeonly image2D transmittanceLut;

vulkan( layout( ogre_P0 ) uniform Params { )
	uniform vec4 atmoPlanet;
	uniform vec4 atmoRayleigh;
	uniform vec4 atmoMie;
	uniform vec4 atmoOzone;
	uniform vec4 atmoGround;
	uniform vec4 atmoSun;
vulkan( }; )

@insertpiece( JahAtmosphere )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

void main()
{
	const ivec2 px = ivec2( gl_GlobalInvocationID.xy );
	const ivec2 size = imageSize( transmittanceLut );
	if( px.x >= size.x || px.y >= size.y )
		return;
	const vec2 unit = vec2( jahAtmoFromSubUv( ( float( px.x ) + 0.5 ) / float( size.x ), float( size.x ) ),
							jahAtmoFromSubUv( ( float( px.y ) + 0.5 ) / float( size.y ), float( size.y ) ) );
	float r, mu;
	jahAtmoTransmittanceRMu( clamp( unit, 0.0, 1.0 ), atmoPlanet, r, mu );
	const vec3 ro = vec3( 0.0, r, 0.0 );
	const vec3 rd = vec3( sqrt( max( 1.0 - mu * mu, 0.0 ) ), mu, 0.0 );
	const float tTop = max( jahAtmoRaySphere( ro, rd, atmoPlanet.y ), 0.0 );
	const int kSteps = 40;
	const float dt = tTop / float( kSteps );
	vec3 depth = vec3( 0.0 );
	for( int i = 0; i < kSteps; ++i )
	{
		const vec3 p = ro + rd * ( ( float( i ) + 0.5 ) * dt );
		vec3 scatR, ext;
		float scatM;
		jahAtmoMedium( length( p ) - atmoPlanet.x, atmoRayleigh, atmoMie, atmoOzone, scatR, scatM, ext );
		depth += ext * dt;
	}
	imageStore( transmittanceLut, px, vec4( exp( -depth ), 1.0 ) );
}
