// gi.env_cone's job (PHOTON-ENV-1): the one environment's cone lookup against
// the cone integral it stands in for, on the same cube, in one dispatch.
//
// `lookup` is jahEnvCone exactly as every consumer runs it (the piece
// JahEnvironment, wrapped from src/rayquery/include/jah_environment.glsl) with
// a unit gain; `reference` is the mean of the cube's FINEST mip over 64
// directions spread uniformly over the cone's solid angle — stratified in
// cos(theta) and turned by the golden angle in phi, so the 64 samples tile the
// cap evenly. Both read the same trilinear sampler.
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
#define JAH_ENV_SH( n ) vec3( 0.0 )
@insertpiece( JahEnvironment )

void main()
{
	const int q = int( gl_GlobalInvocationID.x );
	if( q >= int( params.counts.x ) )
		return;
	const vec3 axis = normalize( params.queries[q].xyz );
	const float t = max( params.queries[q].w, 0.0 );

	const vec3 lookup = jahEnvCone( axis, t );

	// The frame about the axis (any orthonormal one: the cap is symmetric).
	const vec3 helper = abs( axis.y ) < 0.99 ? vec3( 0.0, 1.0, 0.0 ) : vec3( 1.0, 0.0, 0.0 );
	const vec3 tx = normalize( cross( helper, axis ) );
	const vec3 ty = cross( axis, tx );
	const float cosMax = 1.0 / sqrt( 1.0 + t * t );
	const float golden = 2.39996323;
	vec3 sum = vec3( 0.0 );
	for( int k = 0; k < 64; ++k )
	{
		const float c = 1.0 - ( float( k ) + 0.5 ) / 64.0 * ( 1.0 - cosMax );
		const float sn = sqrt( max( 1.0 - c * c, 0.0 ) );
		const float phi = golden * float( k );
		const vec3 d = axis * c + ( tx * cos( phi ) + ty * sin( phi ) ) * sn;
		sum += textureLod( samplerCube( envCube, envSmp ), vec3( d.x, d.y, -d.z ), 0.0 ).xyz;
	}
	answers[2 * q + 0] = vec4( lookup, jahEnvLodForCone( t ) );
	answers[2 * q + 1] = vec4( sum / 64.0, 0.0 );
}
