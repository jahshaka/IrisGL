// THE DEPTH WALK (GATHER-NOISE-1's fix round; PHOTON-2 stage 2's tail moved it here, 1.6 B4 / DEEP_AUDIT
// photon-diffuse F13): is a probe reached from a surface point WITHOUT anything standing between them - the one
// visibility test every read of a neighbouring probe makes (rq_probe_integrate.comp's wide young read, its bilinear
// four, its twin and both silhouette steps; rq_probe_filter.comp's 3 x 3 taps). The includer declares `sceneDepth`
// (the eye-layered depth it samples) and includes jah_probe_params.glsl and jah_reproject.glsl first.
#ifndef JAH_PROBE_SEEN_GLSL
#define JAH_PROBE_SEEN_GLSL

/// THE OCCLUSION TEST: is the probe at
/// `probe` reached from the pixel's surface point `x` WITHOUT anything standing
/// between them? The segment is walked in the eye's own picture, one sample
/// every 3 pixels of its length on screen (2 to 16): at each, the depth buffer
/// must not hold a surface NEARER the eye than the segment itself (beyond a 3 %
/// tolerance: the segment lies on the surface, whose own depth matches it). A
/// partition, a column or a step between the two is exactly such a nearer
/// surface — a 0.1 m partition seen from above is a few pixels wide, which a
/// fixed four samples stepped over (gi.flythrough_noise.Leak).
bool seenAlongSurface( vec3 x, vec2 xUv, vec3 probe, JahEye e, int eye, vec2 size )
{
	vec2 pUv;
	float pz;
	if( jahReproject( probe, e.camPos, e.perspective, e.rayTL, e.rayRight, e.rayDown, e.fwd, pUv, pz ) != 0 )
		return false;
	const int n = clamp( int( length( ( pUv - xUv ) * size ) / 3.0 ), 2, 16 );
	for( int i = 1; i < n; ++i )
	{
		const vec3 w = mix( x, probe, float( i ) / float( n ) );
		vec2 uvS;
		float z;
		if( jahReproject( w, e.camPos, e.perspective, e.rayTL, e.rayRight, e.rayDown, e.fwd, uvS, z ) != 0 )
			return false;
		const float raw = texture( sceneDepth, vec3( uvS, float( eye ) ) ).x;
		if( raw <= 0.0 || raw >= 1.0 )
			continue;		// nothing drawn there: the sky stands behind every surface
		if( jahViewDistance( raw ) < z * 0.97 )
			return false;
	}
	return true;
}

#endif   // JAH_PROBE_SEEN_GLSL
