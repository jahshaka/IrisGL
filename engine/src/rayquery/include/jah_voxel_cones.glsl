// THE DIFFUSE CONE INTEGRATOR — the one copy (PHOTON-CARDS-2 part A; the
// CARDS-1 audit's F3, the one-reader law).
//
// WHO READS IT. THE DIFFUSE IS GATHERED BY RAYS (PHOTON-7 stage 10, 10-1; L5: ONE
// estimator, jahRayDiffuse below over jah_voxel_ray.glsl): the surface cache's card job
// (Hlms/Jahshaka/JahCardLight_cs.glsl), the bounce-injection job
// (Photon/Voxel/LightVctBounceInject_piece_cs.any), the no-ray route's screen integrate and
// the pixel's passes where no field answers (Photon/Hlms/PhotonVct_piece_ps.any) - through
// the piece `JahVoxelCones` the build WRAPS this file into (irisgl/engine/CMakeLists.txt).
// The four-cone diffuse set (jahDiffuseCones) and the sky's sixteen-cone share
// (jahSkyShareFine) are deleted, and so is the SPECULAR cone (PHOTON-7 stage 10-3: the lobe is gathered by the same
// rays, jahRaySpecular below - L9 is physics). Before this file the integrator — the cone
// frame, the two cone sets, their weights and apertures, the start bias, the
// escape weight — existed three times: the pixel's, a line-for-line copy in the
// card job, and the bounce job's own (a cross-product frame about world X and
// the box's normalised axes taken as the world's). A card's bounce and the
// pixel's were two integrals of one volume; now they are one text.
//
// WHAT IT IS NOT: the march. Every cone is walked by jahConeMarch
// (jah_voxel_march.glsl, the piece JahVoxelMarch), every ray by jahVoxelRay
// (jah_voxel_ray.glsl, the piece JahVoxelRay), and every escape reads the one
// environment (jah_environment.glsl, the piece JahEnvironment); the three pieces
// are inserted BEFORE this one.
//
// HOW A CALLER BINDS IT — the march's macros and the environment's, and:
//
//     JAH_CONES_TO_LS(d)      a direction in the BASIS's space (the space the
//                             caller built the cone frame in) to cascade 0's
//                             normalised space, unit length
//     JAH_CONES_TO_WORLD(d)   the same direction in WORLD axes, unit length
//                             (the environment is stored in world axes)
//
// The pixel shader works in view space (its frame is built in the world and
// composed back into view space once), the two compute jobs in world space
// (jahConeBasisWorld) — the functions below never know which.
//
// THE AT-SIGN RULE: this file becomes Hlms input, so no character of it may be
// the Hlms directive mark, comments included (the wrapper fails the build).

#ifndef JAH_VOXEL_CONES_GLSL
#define JAH_VOXEL_CONES_GLSL

// (THE CONE FRAME - jahConeBasis, jahConeBasisWorld - is jah_ray_set.glsl's: the world caches' ray set and every cone
// set share it; the piece JahVoxelRay carries it, inserted before this one.)

/// THE START BIAS: ONE CELL ALONG THE NORMAL, WHICHEVER WAY THE SURFACE FACES
/// (fork change 0070). `normalLS` is the normal in cascade 0's normalised
/// space; the bias direction is it at unit length, and the start point is one
/// cell of cascade 0 along it — COMPONENTWISE (a unit direction times the
/// per-axis inverse resolution is one cell long for every direction; the
/// scalar L1 step is 1.73 cells along a body diagonal and banded a sphere along
/// six arcs). Upstream's bias was one voxel for an axis normal and TWELVE for a
/// diagonal one — a seam by construction on any curved surface.
vec3 jahConeBiasDir( vec3 normalLS )
{
	return normalLS * ( 1.0 / max( length( normalLS ), 1e-6 ) );
}

vec3 jahConeStart( vec3 posLS, vec3 biasDirLS )
{
	return posLS + biasDirLS * JAH_VOX_INVRES( 0 );
}

/// THE DIFFUSE GI AT A SURFACE POINT, GATHERED BY RAYS (PHOTON-7 stage 10, 10-1; L5: ONE estimator; L9: no light
/// passes a wall) - the world caches' (the card relight, the bounce's voxel sides) and the pixel's where it reads no
/// field (the no-ray route's screen integrate; the blended and refractive passes): the cosine-weighted mean radiance over
/// the hemisphere as the mean of `rays` fixed cosine-distributed occupancy rays (jah_voxel_ray.glsl's one set). A HIT
/// reads the side of the voxel facing the ray; an ESCAPE (out of the last cascade: its world shell `lastShellC0`, or its
/// box in the band) the sky over the ray's bin (a cone of the bin's solid angle, 2 pi / N);
/// an EXHAUSTED ray is no sample (the mean is over the rays that are). The sky enters ONLY at an earned miss.
/// `surfLS` the surface point and `nLS` its unit normal (cascade 0's normalised space), `basis` the frame whose third
/// axis is the normal (the caller's space: JAH_CONES_TO_LS / _TO_WORLD take its directions to the volume and the
/// world), `epsC0` the start's offset along the normal (cascade 0's units), `ownCell` the voxel side's own cell
/// (w 0: none), `firstShellC0` its chain's first world shell end (0: the finest cascade's own), `planeTest0` whether the
/// chain's first cascade is the finest (its planar cells take the plane test: jah_voxel_ray.glsl).
/// THE PIXEL'S RAYS (the no-ray route's screen integrate and the passes where no field answers): one count for both, so
/// the two routes are one integral (gi.screen_integrate). A pixel is recomputed every frame, so it traces fewer than a
/// cache texel.
const int kJahPixelDiffuseRays = 8;

/// THE LAST CALL'S OPEN SHARE (PHOTON-7 10-4 R11): the cosine-weighted share of its rays that left the chain, each by
/// what the panes it crossed passed (1 with no ray taken) - the no-ray integrate's measured sky visibility (jahSkyVis).
float gJahRayDiffuseOpen = 1.0;

vec3 jahRayDiffuse( vec3 surfLS, vec3 nLS, mat3 basis, int rays, float epsC0, ivec4 ownCell,
					float firstShellC0, float lastShellC0, bool planeTest0 )
{
	const int n = max( rays, 1 );
	const vec3 startLS = surfLS + nLS * epsC0;
	const float binCos = 1.0 - 1.0 / float( n );
	const float binTan = sqrt( max( 1.0 - binCos * binCos, 0.0 ) ) / binCos;
	vec3 sum = vec3( 0.0, 0.0, 0.0 );
	float taken = 0.0, open = 0.0;
	for( int i = 0; i < n; ++i )
	{
		const vec3 d = basis * jahRayCosineDir( i, n );
		const JahVoxelRayResult r =
			jahVoxelRay( startLS, JAH_CONES_TO_LS( d ), surfLS, nLS, epsC0, ownCell, firstShellC0, lastShellC0, planeTest0 );
		if( r.exitReason == kJahRayHit )
		{
			sum += r.radiance;
			taken += 1.0;
		}
		else if( r.exitReason == kJahRayEscaped || r.exitReason == kJahRayOutside )
		{
			// (OUTSIDE: no cascade held the ray - the chain knows nothing of that world, and the diffuse takes what GI
			// off takes there, the sky over the bin: gi.cascades 4 and 5 - a pixel beyond a one-cascade chain, a
			// teleport's window before the chain is rebuilt around the eye - read black, 0.06 x the ambient)
			sum += r.radiance + r.throughput * jahEnvCone( JAH_CONES_TO_WORLD( d ), binTan );   // (the panes: PHOTON-7 10-4 R7)
			taken += 1.0;
			open += r.throughput;
		}
	}
	gJahRayDiffuseOpen = taken > 0.0 ? open / taken : 1.0;
	return taken > 0.0 ? sum * ( 1.0 / taken ) : vec3( 0.0, 0.0, 0.0 );
}

/// THE SPECULAR LOBE, GATHERED BY RAYS (PHOTON-7 stage 10-3; L9 is physics: a ray does not pass a wall). The no-ray
/// route's screen integrate (Jahshaka/ScreenIntegrate) and the pixel's blended and refractive passes
/// (PhotonVct_piece_ps.any) read the reflection's lobe by the world caches' one occupancy ray (jah_voxel_ray.glsl): `rays`
/// fixed GGX directions about the reflection R (Hammersley strata, each reflected about its half vector, weighted by
/// R . L), one ray (`rays` 1) the mirror direction alone. A HIT reads the side of the voxel facing the ray; a ray that
/// leaves the chain (escaped, or no cascade held it) is the lobe's OPEN share - the caller lights it from the environment
/// (the composite's envColourS: the prefiltered lobe times `open`), so the sky enters only where a ray earned it; a ray
/// below the surface is stopped by the surface itself; an exhausted ray is no sample. It replaced THE SPECULAR CONE:
/// its colour mixed a wall's two faces in coarse cells and its escape share passed a wall's transmittance - the sealed
/// box's metal panels read 3.1 % (0.05) and 12.2 % (0.5) of the roof's light, its panels' grazing borders 105 %, and the
/// captured sky's cube on the narrow lobe 199 % (gi.sealed_sky_noray, red before). `surfLS` the surface point, `nLS` its
/// unit normal (cascade 0's normalised space), `R` and `N` the reflection and the normal in the caller's space
/// (JAH_CONES_TO_LS takes a direction to the volume), `epsC0` the start's offset along the normal, `lastShellC0` the
/// last cascade's world shell, `planeTest0` whether the chain's first cascade is the finest.
struct JahRaySpecular
{
	vec3 radiance;	// the hits' radiance, the mean over the sampled lobe (the open share's light is the caller's)
	float open;		// the lobe's share no geometry stopped
	float hitC0;	// the hits' mean distance, cascade 0's units (0: no hit)
	float hits;		// the hits' weight
	float openC0;	// THE OPEN SHARE'S READ LENGTH (PHOTON-7 10-4 R4): how far the chain read the escapes before they left it,
					// their mean, cascade 0's units (0: no escape, or none a cascade held) - where a traced escape starts
};
float jahRadicalInverse2( uint b )
{
	b = ( b << 16u ) | ( b >> 16u );
	b = ( ( b & 0x55555555u ) << 1u ) | ( ( b & 0xAAAAAAAAu ) >> 1u );
	b = ( ( b & 0x33333333u ) << 2u ) | ( ( b & 0xCCCCCCCCu ) >> 2u );
	b = ( ( b & 0x0F0F0F0Fu ) << 4u ) | ( ( b & 0xF0F0F0F0u ) >> 4u );
	b = ( ( b & 0x00FF00FFu ) << 8u ) | ( ( b & 0xFF00FF00u ) >> 8u );
	return float( b ) * 2.3283064365386963e-10;
}
JahRaySpecular jahRaySpecular( vec3 surfLS, vec3 nLS, vec3 R, vec3 N, float perceptualRoughness, int rays, float epsC0,
							   float lastShellC0, bool planeTest0 )
{
	JahRaySpecular o;
	o.radiance = vec3( 0.0, 0.0, 0.0 );
	o.open = 0.0;
	o.hitC0 = 0.0;
	o.hits = 0.0;
	o.openC0 = 0.0;
	const int n = max( rays, 1 );
	const float a = max( perceptualRoughness * perceptualRoughness, 1e-4 );
	// THE SET IS BUILT IN THE WORLD, whatever the caller's space: the screen integrate (world) and the pixel (the
	// cubemap frame, the world's z mirrored) draw the SAME directions, so the two routes are one estimator to the bit
	// of their inputs (gi.screen_integrate). JAH_CONES_TO_WORLD is an involution in every caller (identity, or the z
	// flip), so it also takes a world direction back to the caller's space.
	const vec3 Rw = normalize( JAH_CONES_TO_WORLD( R ) );
	const vec3 Nw = normalize( JAH_CONES_TO_WORLD( N ) );
	const mat3 basis = jahConeBasisWorld( Rw );
	const vec3 startLS = surfLS + nLS * epsC0;
	float wSum = 0.0, wOpen = 0.0;
	for( int i = 0; i < n; ++i )
	{
		vec3 L = Rw;
		if( n > 1 )
		{
			const float u1 = ( float( i ) + 0.5 ) / float( n );
			const float cosT = sqrt( max( ( 1.0 - u1 ) / ( 1.0 + ( a * a - 1.0 ) * u1 ), 0.0 ) );
			const float sinT = sqrt( max( 1.0 - cosT * cosT, 0.0 ) );
			const float phi = 6.2831853 * jahRadicalInverse2( uint( i ) );
			const vec3 H = normalize( basis * vec3( sinT * cos( phi ), sinT * sin( phi ), cosT ) );
			L = normalize( 2.0 * dot( Rw, H ) * H - Rw );
		}
		const float w = max( dot( Rw, L ), 0.0 );
		if( !( w > 0.0 ) )
			continue;
		if( dot( Nw, L ) <= 0.0 )
		{
			wSum += w;   // stopped by the surface itself
			continue;
		}
		const JahVoxelRayResult r = jahVoxelRay( startLS, JAH_CONES_TO_LS( JAH_CONES_TO_WORLD( L ) ), surfLS, nLS, epsC0,
												 ivec4( 0 ), 0.0, lastShellC0, planeTest0 );
		if( r.exitReason == kJahRayHit )
		{
			o.radiance += r.radiance * w;
			o.hitC0 += r.tC0 * w;
			o.hits += w;
			wSum += w;
		}
		else if( r.exitReason == kJahRayEscaped || r.exitReason == kJahRayOutside )
		{
			// (the panes crossed: their own light, and what they pass of the open share - PHOTON-7 10-4 R7)
			o.radiance += r.radiance * w;
			wSum += w;
			wOpen += w * r.throughput;
			o.openC0 += w * r.throughput * ( r.exitReason == kJahRayEscaped ? r.tC0 : 0.0 );
		}
	}
	o.openC0 = wOpen > 0.0 ? o.openC0 / wOpen : 0.0;
	if( wSum > 0.0 )
	{
		o.radiance *= 1.0 / wSum;
		o.open = wOpen / wSum;
	}
	o.hitC0 = o.hits > 0.0 ? o.hitC0 / o.hits : 0.0;
	return o;
}

/// THE LAW OF THE NO-RAY SPECULAR (PHOTON-7 stage 10-3, the roughness split, measured on six metal panels against the
/// 256-ray reference: spikes/photon-7/stage10/10-3/specKalphaB and specLaws). THE RAYS, K(alpha): the least count whose
/// error stays inside the diffuse's own (mae 3.66 codes) - 1 (the mirror direction) below 0.08, 4 below 0.2, 16 below
/// 0.375, 32 above. Fixed directions draw the lobe's content K times over (petals, visible at 0.15-0.5 at every K up
/// to 32: on the owner's review list). THE FIELD, above the measured crossing: the irradiance field read along R (its
/// cage tested on the surface's normal, the lookup along R: jah_field_read.glsl's jahFieldReadDir) is a cosine lobe
/// about R - right for the widest lobes (mae 0.87 at 0.75, 2.75 at 1.0), too wide below (12.8 at 0.5) - so it takes
/// the lobe across 0.62 - 0.76.
int jahSpecularRays( float perceptualRoughness )
{
	return perceptualRoughness < 0.08 ? 1 : perceptualRoughness < 0.2 ? 4 : perceptualRoughness < 0.375 ? 16 : 32;
}
float jahSpecularFieldShare( float perceptualRoughness )
{
	return smoothstep( 0.62, 0.76, perceptualRoughness );
}

#endif   // JAH_VOXEL_CONES_GLSL
