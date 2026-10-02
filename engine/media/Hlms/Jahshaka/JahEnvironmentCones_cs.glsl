// gi.env_cone's job (PHOTON-ENV-1): the one environment's cone lookup against
// the cone integral it stands in for, on the same cube, in one dispatch.
//
// `lookup` is jahEnvCone exactly as every consumer runs it (the piece
// JahEnvironment, wrapped from src/rayquery/include/jah_environment.glsl) with
// a unit gain; `reference` is the mean of the cube's FINEST mip over 1024
// directions spread uniformly over the cone's solid angle — stratified in
// cos(theta) and turned by the golden angle in phi, so the samples tile the
// cap evenly. Both read the same trilinear sampler. (1024, not 64: against the
// physical sky's hard horizon 64 directions put 1.7 % of error of their own into
// the four-cone aperture's mean at a 5-degree sun — CONE-ENV-EDGE-1.)
@insertpiece( SetCrossPlatformSettings )

vulkan_layout( ogre_t0 ) uniform textureCube envCube;
vulkan( layout( ogre_s0 ) uniform sampler envSmp );

struct ConeParams
{
	vec4 queries[64];
	vec4 counts;
};
layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { ConeParams params; };
layout( std430, ogre_U1 ) writeonly restrict buffer outLayout { vec4 answers[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_ENV_CUBE_ON true
#define JAH_ENV_SAMPLE( d, l ) textureLod( samplerCube( envCube, envSmp ), d, l ).xyz
#define JAH_ENV_MIPS params.counts.y
#define JAH_ENV_GAIN vec3( 1.0 )
#define JAH_ENV_SH_C0 vec3( 0.0 )
#define JAH_ENV_SH_C1 vec3( 0.0 )
#define JAH_ENV_SH_C2 vec3( 0.0 )
#define JAH_ENV_SH_C3 vec3( 0.0 )
#define JAH_ENV_SH_C4 vec3( 0.0 )
#define JAH_ENV_SH_C5 vec3( 0.0 )
#define JAH_ENV_SH_C6 vec3( 0.0 )
#define JAH_ENV_SH_C7 vec3( 0.0 )
#define JAH_ENV_SH_C8 vec3( 0.0 )
@insertpiece( JahEnvironment )

void main()
{
	const int q = int( gl_GlobalInvocationID.x );
	if( q >= int( params.counts.x ) )
		return;
	const vec3 axis = normalize( params.queries[q].xyz );
	const float t = max( params.queries[q].w, 0.0 );

	// A NEGATIVE tan asks for the four-cone set's escape about a +Y normal (jahEnvQuadrant).
	const vec3 lookup = params.queries[q].w < 0.0 ? jahEnvQuadrant( axis, vec3( 0.0, 1.0, 0.0 ) )
												 : jahEnvCone( axis, t );

	// The frame about the axis (any orthonormal one: the cap is symmetric).
	const vec3 helper = abs( axis.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
	const vec3 tx = normalize( cross( helper, axis ) );
	const vec3 ty = cross( axis, tx );
	const float cosMax = 1.0 / sqrt( 1.0 + t * t );
	const float golden = 2.39996323;
	vec3 sum = vec3( 0.0 );
	for( int k = 0; k < 1024; ++k )
	{
		const float c = 1.0 - ( float( k ) + 0.5 ) / 1024.0 * ( 1.0 - cosMax );
		const float sn = sqrt( max( 1.0 - c * c, 0.0 ) );
		const float phi = golden * float( k );
		const vec3 d = axis * c + ( tx * cos( phi ) + ty * sin( phi ) ) * sn;
		sum += textureLod( samplerCube( envCube, envSmp ), vec3( d.x, d.y, -d.z ), 0.0 ).xyz;
	}
	if( params.queries[q].w < 0.0 )
	{
		// ...and its reference is the cone's quadrant of the +Y hemisphere, cosine-weighted,
		// over the same 1024 directions (32 strata in sin^2 theta by 32 in azimuth).
		const vec3 az = normalize( vec3( axis.x, 0.0, axis.z ) );
		const vec3 bz = cross( vec3( 0.0, 1.0, 0.0 ), az );
		sum = vec3( 0.0 );
		for( int k = 0; k < 1024; ++k )
		{
			const float u = ( float( k / 32 ) + 0.5 ) / 32.0;
			const float phi = ( ( float( k % 32 ) + 0.5 ) / 32.0 - 0.5 ) * 1.5707963;
			const vec3 d = vec3( 0.0, sqrt( 1.0 - u ), 0.0 ) + ( az * cos( phi ) + bz * sin( phi ) ) * sqrt( u );
			sum += textureLod( samplerCube( envCube, envSmp ), vec3( d.x, d.y, -d.z ), 0.0 ).xyz;
		}
	}
	answers[2 * q + 0] = vec4( lookup, jahEnvLodForCone( t ) );
	answers[2 * q + 1] = vec4( sum / 1024.0, 0.0 );
}
