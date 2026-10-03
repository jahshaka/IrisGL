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

// THE GGX LOBE'S DOMINANT DIRECTION (ENV-RAY-MATCH, PHOTON P3) — where a
// PREFILTERED lookup of the environment should point for a surface of normal
// `n`, mirror direction `r` and GGX alpha `alpha` (perceptual roughness
// squared). The split-sum chain is convolved with N = V = R (its own
// assumption), so reading it along R centres the lobe on the mirror
// direction; the true lobe — what the ray tier's VNDF samples ARE — leans
// toward the normal (the off-specular peak) and is clipped by the horizon.
// This is Frostbite's getSpecularDominantDir ("Moving Frostbite to PBR",
// Lagarde and de Rousiers 2014, listing 22): A FIT TO THE LOBE'S CENTROID, not
// the physics — measured on gi.env_ray_match it halves the environment
// lookup's distance from the rays' answer (the horizon clip and the dropped
// G2/G1 remain). Space-free: n and r in any one frame. It needs no binding,
// so it is defined for every includer (HlmsPbs's pixel shader inserts this
// piece for it with or without the environment's bindings below).
#ifndef JAH_ENV_DOMINANT_GLSL
#define JAH_ENV_DOMINANT_GLSL
vec3 jahEnvDominantDir( vec3 n, vec3 r, float alpha )
{
	const float smoothness = clamp( 1.0 - alpha, 0.0, 1.0 );
	const float f = smoothness * ( sqrt( smoothness ) + alpha );
	return normalize( mix( n, r, f ) );
}
#endif

// THE REST NEEDS THE CALLER'S BINDINGS (the macros above), so an includer that
// has none — HlmsPbs's pixel shader without VCT — gets the helper only.
#if !defined( JAH_ENVIRONMENT_GLSL ) && defined( JAH_ENV_GAIN )
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
/// error at the deleted six-cone set's aperture). The 0.6 is MEASURED, not derived: gi.env_cone
/// swept it (1.0 / 0.7 / 0.6 / 0.5 / 0.35) over 26 directions, three apertures
/// and two sun heights; 0.5-0.7 is the flat optimum and 0.6 keeps every
/// aperture under 4 % mean error at both (the deleted six-cone set 1.4 / 3.8 %, the field's
/// probe ray 0.7 / 1.2 %, a 0.1 rad specular cone 1.0 / 2.3 %, noon / low sun).
/// The GGX moment is numerical (400,000 stratified samples per roughness, 801
/// roughnesses; its maximum is exactly 1/3, at r = 1), tabulated below against
/// s = sqrt( M / (1/3) ), on which r is close to linear. (For a cone below a
/// hemisphere s stays under sqrt( 0.9 ), so the lookup never reaches r = 1: the
/// widest lobe a cone reads is r ~ 0.87.)
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

/// CONE-ENV-EDGE-1: A CONE IS READ AS EQUAL CELLS, AND THE CELL COUNT GROWS WITH
/// THE APERTURE. One fetch of the GGX chain weighs a step inside the cone (the
/// physical sky's hard horizon over the dark planet, the glow round a low sun)
/// by its lobe's peak, not by the cone's area, and the wider the cone the worse.
/// The cone is cut into rings of equal-solid-angle cells — the centre cap, then
/// 6, then 12 round it (7, 19 cells) — each read as a cone of its own cell's
/// size. MEASURED (gi.env_cone, 26 directions, 5-degree sun, against a
/// 1024-direction integral): one fetch 4.1 % at tan 0.15, 6.8 % at tan 0.372,
/// 19.6 % at tan 0.983; 7 cells 0.7 %, 2.0 % and 9.7 %; 19 cells 0.3 %, 1.0 % and
/// 3.4 % (35-degree sun, tan 0.983: one fetch 5.9 %, 7 cells 2.2 %, 19 cells
/// 1.0 %); 37 cells buy nothing more at tan 0.983 (3.5 %).
///
/// THE COUNT NEVER STEPS. A caller's aperture varies continuously (a reflection's
/// footprint lobeAlpha / sqrt(N), a gather ray's sqrt(2 / rays)), so a change of
/// count must be crossed where the two reads agree. One fetch and 7 cells agree
/// only as the cone closes — at a 5-degree sun they differ by six times the sky's
/// mean luminance toward the sun's glow at tan 0.35, and a smoothstep over
/// 0.20-0.35 still stepped the read 23 % of that mean per 0.0025 of tan — while
/// 7 cells shrink to the one fetch at tan 0 (every cell on the axis at mip 0).
/// So EVERY open cone reads at least 7 cells (only a mirror's, tan 0, is the one
/// fetch the cells collapse to — even a switch at tan 0.01 stepped the read 3.8 %
/// of the sky's mean toward a 5-degree sun's glow), and 7 cells hand over to 19
/// through a smoothstep over [kJahEnvCells19Lo, kJahEnvCells19Hi], where the two
/// agree to a few per cent and both are read (gi.env_cone sweeps tan 0 to 1.1 and
/// asserts the read never steps).
const float kJahEnvCells19Lo = 0.50;
const float kJahEnvCells19Hi = 0.65;

/// The mip of the prefiltered chain that holds the cone's lobe.
float jahEnvLodForCone( float tanHalfAngle )
{
	const float r = jahEnvRoughnessForCone( tanHalfAngle );
	return max( JAH_ENV_MIPS - 1.0, 0.0 ) * r * ( 2.0 - r );
}

/// The cone about CUBE-FRAME direction `d` whose cap is `omega` = 1 - cos(half-angle)
/// (2 pi omega steradians) as 1 + 3 rings (rings + 1) cells of equal solid angle:
/// the centre cap and `rings` rings of 6 k cells. A ring's cells sit at the ring's
/// median solid angle, the odd rings turned half a cell, and every cell is read
/// through the chain as the cap of its own solid angle. Exact solid angles, no
/// small-angle constants; every angle from its cap through the half-angle form
/// (1 - cos a = 2 sin^2(a/2)), which keeps a narrow cone exact in float.
vec3 jahEnvConeCells( vec3 d, float omega, int rings )
{
	const float n = 1.0 + 3.0 * float( rings * ( rings + 1 ) );
	const float cellHalf = 2.0 * asin( sqrt( 0.5 * omega / n ) );
	const float lod = jahEnvLodForCone( tan( cellHalf ) );
	const vec3 tx = normalize( abs( d.y ) < 0.99 ? cross( d, vec3( 0.0, 1.0, 0.0 ) )
											: cross( d, vec3( 1.0, 0.0, 0.0 ) ) );
	const vec3 ty = cross( d, tx );
	vec3 sum = max( JAH_ENV_SAMPLE( d, lod ), vec3( 0.0 ) );
	for( int k = 1; k <= rings; ++k )
	{
		const float inner = 1.0 + 3.0 * float( ( k - 1 ) * k );
		const float outer = 1.0 + 3.0 * float( k * ( k + 1 ) );
		const float ringAngle = 2.0 * asin( sqrt( 0.25 * ( inner + outer ) / n * omega ) );
		const float c = cos( ringAngle );
		const float sn = sin( ringAngle );
		const int cells = 6 * k;
		const float dphi = 6.2831853 / float( cells );
		const float turn = ( k & 1 ) == 1 ? 0.5 : 0.0;
		for( int j = 0; j < cells; ++j )
		{
			const float phi = ( float( j ) + turn ) * dphi;
			sum += max( JAH_ENV_SAMPLE( d * c + ( tx * cos( phi ) + ty * sin( phi ) ) * sn, lod ),
						vec3( 0.0 ) );
		}
	}
	return sum / n;
}

/// What a CONE of half-angle atan( tanHalfAngle ) about `dirWorld` sees of the
/// environment: radiance. With no cube bound (a scene with no sky, lit by a
/// flat or hemisphere ambient) the environment IS its SH, and the cone reads
/// the RADIANCE the SH describes in its own direction — the coefficients
/// de-convolved, NOT the irradiance: a set of cones that each read the
/// irradiance at its own axis and are then weighted by the cosine convolves the
/// sky twice (measured: a hemisphere ambient's zenith band came back at 0.63 of
/// itself through the deleted six-cone set, and the voxel volume's face stepped against the
/// SH outside it).
vec3 jahEnvCone( vec3 dirWorld, float tanHalfAngle )
{
	if( JAH_ENV_CUBE_ON )
	{
		const vec3 d = vec3( dirWorld.x, dirWorld.y, -dirWorld.z );
		const float t = max( tanHalfAngle, 0.0 );
		if( t <= 0.0 )
			return max( JAH_ENV_SAMPLE( d, jahEnvLodForCone( t ) ), vec3( 0.0 ) ) * JAH_ENV_GAIN;
		// 1 - cos(atan t), cancellation-free.
		const float root = sqrt( 1.0 + t * t );
		const float omega = t * t / ( root * ( 1.0 + root ) );
		const vec3 seven = jahEnvConeCells( d, omega, 1 );
		if( t <= kJahEnvCells19Lo )
			return seven * JAH_ENV_GAIN;
		const float w = smoothstep( kJahEnvCells19Lo, kJahEnvCells19Hi, t );
		return mix( seven, jahEnvConeCells( d, omega, 2 ), w ) * JAH_ENV_GAIN;
	}
	return max( jahEnvShEval( dirWorld, 1.5, 4.0 ), vec3( 0.0 ) );
}

/// THE ESCAPE OF ONE OF THE FOUR DIFFUSE CONES (CONE-ENV-EDGE-1; jah_voxel_cones.glsl
/// jahDiffuseCones): the cosine-weighted mean radiance over the cone's AZIMUTHAL
/// QUADRANT of the hemisphere about `normalWorld`. The four cones at 45 degrees cut
/// the hemisphere into four quadrants of exactly a quarter of its projected solid
/// angle each - that is what their 0.25 weights are (each cone's cosine-weighted
/// solid angle over pi) - so the set's open-sky sum is the irradiance over pi. A
/// cone's own 44.5-degree cap read uniformly is not that: the caps overlap, reach
/// below the horizon and weigh the grazing band like the zenith, and against the
/// cosine-weighted quadrants (gi.env_cone, +Y, 4 x 1024 directions) the set read
/// 1.07x the irradiance at a 35-degree sun and 1.40x at a 5-degree one. The read:
/// nine cosine strata per quadrant (3 x 3 in sin^2 theta and azimuth), each through
/// the chain at a lobe of FOUR times its stratum's solid angle (projected pi / 36,
/// over its cosine). Nine nodes stand 19 degrees above a low sun's glow at best;
/// the wider lobe carries it (the sweep, quadrant toward a 5-degree sun / the set:
/// 1x 0.85 / 0.92, 2x 0.89 / 0.94, 4x 0.96 / 0.98, 6x 1.00 / 1.01 but its side
/// quadrants 1.09 at 35 degrees; 16 to 32 nodes at 1x still 0.87-0.95 toward the
/// sun). Measured: 1.002x at 35 degrees, 0.979x at 5. Nine fetches a cone (the cap
/// read took 26). With no cube, the SH at the set's own band weights 1 / P_l( cos
/// 45 ) - sqrt( 2 ) and 4 (CARD-VIEW-BIAS-1's ring rule: the set's sum IS the SH
/// irradiance) - UNCLAMPED: under a directional SH a single quadrant's band-weighted
/// value can be negative while the set's sum is not, and clamping each cone broke
/// that sum (the merge read's worth-a-look 3: a high light, no ambient - 94 % of
/// normals had a negative cone; spikes/photon-ii-1/p2/wal3). The CALLER clamps the
/// set's sum (jahDiffuseCones).
vec3 jahEnvQuadrant( vec3 coneWorld, vec3 normalWorld )
{
	if( JAH_ENV_CUBE_ON )
	{
		const vec3 az = normalize( coneWorld - normalWorld * dot( coneWorld, normalWorld ) );
		const vec3 bz = cross( normalWorld, az );
		vec3 sum = vec3( 0.0 );
		for( int j = 0; j < 3; ++j )
		{
			const float u1 = ( float( j ) + 0.5 ) / 3.0;
			const float st = sqrt( u1 ), ct = sqrt( 1.0 - u1 );
			// four times the stratum's solid angle: projected pi / 4 / 9, over its cosine
			const float omega = 4.0 * 0.0872665 / max( ct, 0.05 );
			const float lod = jahEnvLodForCone( tan( 2.0 * asin( min( sqrt( omega / 12.566371 ), 1.0 ) ) ) );
			for( int k = 0; k < 3; ++k )
			{
				const float phi = ( ( float( k ) + 0.5 ) / 3.0 - 0.5 ) * 1.5707963;
				const vec3 w = normalWorld * ct + ( az * cos( phi ) + bz * sin( phi ) ) * st;
				sum += max( JAH_ENV_SAMPLE( vec3( w.x, w.y, -w.z ), lod ), vec3( 0.0 ) );
			}
		}
		return sum * ( 1.0 / 9.0 ) * JAH_ENV_GAIN;
	}
	return jahEnvShEval( coneWorld, 1.41421356, 4.0 );
}

/// What a GGX LOBE of perceptual roughness `r` about `dirWorld` sees of the
/// environment: the prefiltered chain at exactly the mip the convolution stored r
/// at, (N - 1) r (2 - r) — the same mip HlmsPbs reads for that roughness outside
/// every volume. With no cube, the radiance the SH describes in that direction.
vec3 jahEnvLobe( vec3 dirWorld, float r )
{
	if( JAH_ENV_CUBE_ON )
	{
		const float rr = clamp( r, 0.0, 1.0 );
		const float lod = max( JAH_ENV_MIPS - 1.0, 0.0 ) * rr * ( 2.0 - rr );
		const vec3 d = vec3( dirWorld.x, dirWorld.y, -dirWorld.z );
		return max( JAH_ENV_SAMPLE( d, lod ), vec3( 0.0 ) ) * JAH_ENV_GAIN;
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

#endif   // JAH_ENVIRONMENT_GLSL (and the bindings)
