// jah_world_exit.glsl (PHOTON-7 10-3 R4): where a ray leaves the world. Include AFTER the job's uniform block (the
// function reads JAH_HIT_PARAMS, jah_rq_hit_params.glsl's member of it).
#ifndef JAH_WORLD_EXIT_GLSL
#define JAH_WORLD_EXIT_GLSL
/// THE RAY's EXIT FROM THE WORLD (PHOTON-7 10-3 R4; L9: "ran out of distance" is not a miss): the distance along unit
/// `d` from `o` to where it leaves the world's box (the traced instances' union, grown by a centimetre) - the only
/// place a ray's miss is the sky. `fallback` where no box is known.
float jahWorldExitT( vec3 o, vec3 d, float fallback )
{
	if( !JAH_WORLD_ON )
		return fallback;
	const vec3 lo = JAH_HIT_PARAMS.worldMin.xyz - vec3( 0.01 ), hi = JAH_HIT_PARAMS.worldMax.xyz + vec3( 0.01 );
	float tExit = 1e30;
	for( int a = 0; a < 3; ++a )
	{
		if( d[a] == 0.0 )
			continue;
		tExit = min( tExit, ( ( d[a] > 0.0 ? hi[a] : lo[a] ) - o[a] ) / d[a] );
	}
	return max( tExit, 0.0 );
}

#endif   // JAH_WORLD_EXIT_GLSL
