// THE ONE ENVIRONMENT — what every escape sees (PHOTON-ENV-1; the design
// SPECS/photon/B3_ONE_ENV_DESIGN.md, P4 items ONE-ENV and GA-SKY).
//
// ONE ENVIRONMENT, TWO LOOKUPS, stated once:
//   * THE ENVIRONMENT CUBE is the scene's sky captured WITHOUT the sun disc (the
//     disc is the direct sun, a light, and it lives outside the capture's queue
//     range and visibility mask — JahshakaSkyCapture.compositor), GGX-prefiltered
//     into a mip chain (Ogre's ibl_specular). A CONE of half-angle theta reads it
//     through jahEnvCone: the chain's mip whose lobe matches the cone (below).
//   * THE DIFFUSE ENVIRONMENT is the nine-band SH irradiance, the cosine
//     convolution of the same sky — exact for a cosine lobe about a NORMAL
//     (jahEnvIrradiance).
// Both are scaled by the scene's Sky Light (intensity times tint) at their
// source, so "no Sky Light, no environment" holds for both at once.
//
// WHO READS IT: the pixel shader's voxel cones (their escape, diffuse and
// specular), the probe-array pass's no-probe fallback, the bounce-injection job
// (an escaping bounce cone sees the sky once, at injection) and the ray jobs'
// miss (the gather's and the reflection's). The Hlms consumers insert the piece
// `JahEnvironment` that the build WRAPS this file into
// (irisgl/engine/CMakeLists.txt); the ray jobs include it through glslang. No
// hand copy exists anywhere. The wrapper makes this file Hlms input, so NO
// CHARACTER OF IT MAY BE THE HLMS DIRECTIVE MARK, comments included.
//
// HOW A CALLER BINDS IT (define these first; nothing here fixes a binding):
//
//     JAH_ENV_CUBE_ON          bool: the environment cube is bound
//     JAH_ENV_SAMPLE( d, lod ) vec3: the cube in CUBE-FRAME direction d at mip lod
//                              (the caller's own texture and sampler)
//     JAH_ENV_MIPS             float: the cube's mip count
//     JAH_ENV_GAIN             vec3: the Sky Light's gain on the cube
//     JAH_ENV_SH_C0 .. JAH_ENV_SH_C8
//                              vec3: the environment's nine SH coefficients in
//                              the engine's WORLD basis {1, y, z, x, xy, yz,
//                              3z^2 - 1, zx, x^2 - y^2} (Scene::setAmbientSh's
//                              order), already carrying the Sky Light's gain:
//                              the COSINE-CONVOLVED irradiance over pi, which is
//                              what the engine pushes and HlmsPbs evaluates
//
// Every direction handed to this file is in WORLD axes. Ogre samples cubemaps
// LEFT-HANDED (the sky/IBL adoption's fact): the flip is written once, here.

#ifndef JAH_ENVIRONMENT_GLSL
#define JAH_ENVIRONMENT_GLSL

/// The nine-band evaluation in the engine's WORLD basis, with a scale per BAND.
/// The coefficients are the cosine-convolved irradiance over pi (band l scaled
/// by A_l / pi: 1, 2/3, 1/4 — Ramamoorthi and Hanrahan), so (1, 1) evaluates
/// the IRRADIANCE a normal receives, and (3/2, 4) undoes the convolution and
/// evaluates the RADIANCE arriving from a direction (band-limited to two bands).
vec3 jahEnvShEval( vec3 n, float k1, float k2 )
{
	return JAH_ENV_SH_C0 +
		   k1 * ( JAH_ENV_SH_C1 * n.y + JAH_ENV_SH_C2 * n.z + JAH_ENV_SH_C3 * n.x ) +
		   k2 * ( JAH_ENV_SH_C4 * ( n.x * n.y ) + JAH_ENV_SH_C5 * ( n.y * n.z ) +
				  JAH_ENV_SH_C6 * ( 3.0 * n.z * n.z - 1.0 ) + JAH_ENV_SH_C7 * ( n.z * n.x ) +
				  JAH_ENV_SH_C8 * ( n.x * n.x - n.y * n.y ) );
}

/// THE CONE'S LOBE ON THE PREFILTERED CHAIN — the aperture to roughness mapping.
///
/// Ogre's convolution stores perceptual roughness r at mip m of an N-mip chain
/// with m / (N - 1) = r (2 - r) (CompositorPassIblSpecular::lodToPerceptualRoughness,
/// inverted). Its lobe is the split-sum GGX lobe (N = V = R, weighted by N.L),
/// not a box, so no roughness reproduces a uniform cone exactly. THE MAPPING, in
/// one formula: the lobe whose mean of (1 - cos) about its axis is 0.6 of the
/// cone's — M_ggx(r) = 0.6 (1 - cos theta) / 2. Matching the full moment
/// (factor 1) would be the band-1 answer — exact for a linear gradient of
/// radiance — but a GGX lobe's moment is carried by its long tail, so the
/// matched lobe reaches far past the cone's rim and pulls the bright horizon
/// into a zenith-pointing cone (measured on the shipped sky, low sun: 10 % mean
/// error at the six-cone aperture). The 0.6 is MEASURED, not derived: gi.env_cone
/// swept it (1.0 / 0.7 / 0.6 / 0.5 / 0.35) over 26 directions, three apertures
/// and two sun heights; 0.5-0.7 is the flat optimum and 0.6 keeps every
/// aperture under 4 % mean error at both (six-cone 1.4 / 3.8 %, the field's
/// probe ray 0.7 / 1.2 %, a 0.1 rad specular cone 1.0 / 2.3 %, noon / low sun).
/// The GGX moment is numerical (400,000 stratified samples per roughness, 801
/// roughnesses; its maximum is exactly 1/3, at r = 1), tabulated below against
/// s = sqrt( M / (1/3) ), on which r is close to linear. A cone wider than the
/// widest lobe the chain holds reads r = 1.
float jahEnvRoughnessForCone( float tanHalfAngle )
{
	const float kLut[17] = float[17]( 0.0000, 0.0999, 0.1518, 0.1966, 0.2384, 0.2790,
									  0.3194, 0.3606, 0.4031, 0.4478, 0.4954, 0.5473,
									  0.6051, 0.6714, 0.7508, 0.8526, 1.0000 );
	const float t = max( tanHalfAngle, 0.0 );
	const float cosTheta = 1.0 / sqrt( 1.0 + t * t );
	// s = sqrt( 0.6 * ((1 - cos) / 2) / (1/3) ) = sqrt( 0.9 (1 - cos) )
	const float s = sqrt( clamp( 0.9 * ( 1.0 - cosTheta ), 0.0, 1.0 ) );
	const float x = s * 16.0;
	const int i = min( int( x ), 15 );
	return mix( kLut[i], kLut[i + 1], clamp( x - float( i ), 0.0, 1.0 ) );
}

/// The mip of the prefiltered chain that holds the cone's lobe.
float jahEnvLodForCone( float tanHalfAngle )
{
	const float r = jahEnvRoughnessForCone( tanHalfAngle );
	return max( JAH_ENV_MIPS - 1.0, 0.0 ) * r * ( 2.0 - r );
}

/// What a CONE of half-angle atan( tanHalfAngle ) about `dirWorld` sees of the
/// environment: radiance. With no cube bound (a scene with no sky, lit by a
/// flat or hemisphere ambient) the environment IS its SH, and the cone reads
/// the RADIANCE the SH describes in its own direction — the coefficients
/// de-convolved, NOT the irradiance: a set of cones that each read the
/// irradiance at its own axis and are then weighted by the cosine convolves the
/// sky twice (measured: a hemisphere ambient's zenith band came back at 0.63 of
/// itself through the six cones, and the voxel volume's face stepped against the
/// SH outside it).
vec3 jahEnvCone( vec3 dirWorld, float tanHalfAngle )
{
	if( JAH_ENV_CUBE_ON )
	{
		const vec3 d = vec3( dirWorld.x, dirWorld.y, -dirWorld.z );
		return max( JAH_ENV_SAMPLE( d, jahEnvLodForCone( tanHalfAngle ) ), vec3( 0.0 ) ) *
			   JAH_ENV_GAIN;
	}
	return max( jahEnvShEval( dirWorld, 1.5, 4.0 ), vec3( 0.0 ) );
}

/// The diffuse environment for a surface whose normal is `nWorld`: the cosine
/// convolution of the sky, divided by pi (radiance units — what a Lambertian
/// surface of albedo 1 would reflect).
vec3 jahEnvIrradiance( vec3 nWorld )
{
	return max( jahEnvShEval( nWorld, 1.0, 1.0 ), vec3( 0.0 ) );
}

#endif   // JAH_ENVIRONMENT_GLSL
