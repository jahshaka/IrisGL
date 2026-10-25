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
// (jahSkyShareFine) are deleted. What still marches a cone is the SPECULAR lobe
// (jahSpecularCone, below). Before this file the integrator — the cone
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

/// THE CONE FRAME IS A FUNCTION OF THE NORMAL AND OF NOTHING ELSE (fork change
/// 0083). An orthonormal frame whose third axis is `n`, by Frisvad's method
/// (JGT 2012) — continuous over the whole sphere but for ONE direction, where
/// the branch hands back a fixed frame instead of dividing by zero.
///
/// `n` is in a CAMERA-INDEPENDENT space — the cubemap frame (world with z
/// negated, the frame HlmsPbs's invViewMatCubemap takes a view direction to) —
/// because a frame is only worth building once it cannot turn with the head.
///
/// AND IT IS BUILT ABOUT THAT SPACE'S OWN AXES, a decision measured rather than
/// argued: Frisvad's special direction is the antipode of (0,0,1), and a fixed
/// pre-rotation (onto a body diagonal, which no axis-aligned surface has) turns
/// the cones of every axis-aligned surface to odd angles, where each sample
/// is a three-way blend of the anisotropic volumes, and the bounce comes out
/// dimmer — gi.field_follows' red-wall bounce read 0.0174 with the axes and
/// 0.0105 with the body diagonal. The price: the special direction is a world
/// axis, where a surface within a hair of it takes the fixed frame while its
/// neighbours take a rapidly turning one. "The frame is axis-aligned for axis
/// normals" and "the reference direction is an axis" are the same statement.
///
/// The two frames this replaced were both unstable against an anisotropic
/// voxel field (the diffuse set is not azimuthally symmetric, so turning the
/// frame about the normal reads different axis volumes): the material's
/// per-vertex TANGENT frame (a kink along every quad's diagonal) and the screen
/// derivative fallback.
mat3 jahConeBasis( vec3 n )
{
	vec3 t;
	vec3 b;
	if( n.z < -0.9999999 )
	{
		t = vec3( 0.0, -1.0, 0.0 );
		b = vec3( -1.0, 0.0, 0.0 );
	}
	else
	{
		float a = 1.0 / ( 1.0 + n.z );
		float c = -n.x * n.y * a;
		t = vec3( 1.0 - n.x * n.x * a, c, -n.x );
		b = vec3( c, 1.0 - n.y * n.y * a, -n.y );
	}
	return mat3( t, b, n );
}

/// The same frame for a WORLD normal, returned in world axes: built in the
/// cubemap frame (z negated) exactly as the pixel builds it, and brought back
/// by negating z again (exact in floating point, so a compute job's cone
/// directions are the pixel's to the bit, before the pixel's view rotation).
mat3 jahConeBasisWorld( vec3 nWorld )
{
	mat3 c = jahConeBasis( vec3( nWorld.x, nWorld.y, -nWorld.z ) );
	return mat3( vec3( c[0].x, c[0].y, -c[0].z ), vec3( c[1].x, c[1].y, -c[1].z ),
				 vec3( c[2].x, c[2].y, -c[2].z ) );
}

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

/// THE SPECULAR START: ON ITS OWN AXIS, AT THE DIFFUSE START'S HEIGHT (PHOTON-PHYSICS-1,
/// CONE-EMITTER-1). The diffuse set starts one cell along the normal (jahConeStart); a specular
/// cone started there too walked a line PARALLEL to the reflection, displaced one cell of
/// cascade 0 along the normal - the mirror image of everything it read slid by that cell (a
/// 1 m emitter panel in a roughness-0.05 lobe read 0.74 of its radiance where the true
/// reflection met it 0.25 m below its edge: the displaced line met it 0.09 m below). It starts
/// on the reflection instead, where the reflection has risen the same one cell above the
/// surface - the same height, so the surface's own voxels stay behind it as before (and the
/// origin plane keeps them out of every read) - at most FOUR cells along a grazing reflection
/// (below a rise of 1/4 it starts lower than a cell, which the origin plane still covers).
/// THE CONE'S APEX STAYS AT THE SURFACE: w is the length skipped, the age the march starts with
/// (jahConeMarchAged), so the footprint at a distance is the lobe's from the surface and not
/// from the start (started along its axis with no age, a roughness-0.5 cone read a 1 m panel
/// 1.4 m away with a footprint 16 % narrow).
/// `surfaceLS` the surface point, `dirLS` the unit reflection and `biasDirLS` the unit normal,
/// cascade 0's normalised space (cubic cells: a unit direction times the inverse resolution is
/// one cell long, and its rise along the normal is dot( dir, normal ) cells).
vec4 jahSpecularConeStart( vec3 surfaceLS, vec3 dirLS, vec3 biasDirLS )
{
	const float cells = 1.0 / max( dot( dirLS, biasDirLS ), 0.25 );
	const vec3 step = dirLS * JAH_VOX_INVRES( 0 ) * cells;
	return vec4( surfaceLS + step, length( step ) );
}

/// THE DIFFUSE GI AT A SURFACE POINT, GATHERED BY RAYS (PHOTON-7 stage 10, 10-1; L5: ONE estimator; L9: no light
/// passes a wall) - the world caches' (the card relight, the bounce's voxel sides) and the pixel's where it reads no
/// field (the no-ray route's screen integrate; the blended and refractive passes): the cosine-weighted mean radiance over
/// the hemisphere as the mean of `rays` fixed cosine-distributed occupancy rays (jah_voxel_ray.glsl) turned by `turn`
/// about the normal. A HIT reads the side of the voxel facing the ray; an ESCAPE (out of the last cascade: its world
/// shell `lastShellC0`, or its box in the band) the sky over the ray's bin (a cone of the bin's solid angle, 2 pi / N);
/// an EXHAUSTED ray is no sample (the mean is over the rays that are). The sky enters ONLY at an earned miss.
/// `surfLS` the surface point and `nLS` its unit normal (cascade 0's normalised space), `basis` the frame whose third
/// axis is the normal (the caller's space: JAH_CONES_TO_LS / _TO_WORLD take its directions to the volume and the
/// world), `epsC0` the start's offset along the normal (cascade 0's units), `ownCell` the voxel side's own cell
/// (w 0: none), `firstShellC0` its chain's first world shell end (0: the finest cascade's own).
/// THE PIXEL'S RAYS (the no-ray route's screen integrate and the passes where no field answers): one count for both, so
/// the two routes are one integral (gi.screen_integrate). A pixel is recomputed every frame, so it traces fewer than a
/// cache texel, at a fixed turn (smooth across a surface; a per-pixel key would be noise).
const int kJahPixelDiffuseRays = 8;

vec3 jahRayDiffuse( vec3 surfLS, vec3 nLS, mat3 basis, int rays, float turn, float epsC0, ivec4 ownCell,
					float firstShellC0, float lastShellC0 )
{
	const int n = max( rays, 1 );
	const vec3 startLS = surfLS + nLS * epsC0;
	const float binCos = 1.0 - 1.0 / float( n );
	const float binTan = sqrt( max( 1.0 - binCos * binCos, 0.0 ) ) / binCos;
	vec3 sum = vec3( 0.0, 0.0, 0.0 );
	float taken = 0.0;
	for( int i = 0; i < n; ++i )
	{
		const vec3 d = basis * jahRayCosineDir( i, n, turn );
		const JahVoxelRayResult r =
			jahVoxelRay( startLS, JAH_CONES_TO_LS( d ), surfLS, nLS, epsC0, ownCell, firstShellC0, lastShellC0 );
		if( r.exitReason == kJahRayHit )
		{
			sum += r.radiance;
			taken += 1.0;
		}
		else if( r.exitReason == kJahRayEscaped )
		{
			sum += jahEnvCone( JAH_CONES_TO_WORLD( d ), binTan );
			taken += 1.0;
		}
	}
	return taken > 0.0 ? sum * ( 1.0 / taken ) : vec3( 0.0, 0.0, 0.0 );
}

/// THE SPECULAR CONE AT A SURFACE POINT (PHOTON-7 stage 7, 7-6: ONE text for the pixel's passes that still bind the
/// voxels - the blended and refractive ones, PhotonVct_piece_ps.any - and the no-ray route's screen integrate,
/// Jahshaka/ScreenIntegrate). The march of the reflection's lobe; the caller composes it (the fog law on the voxels'
/// share, the environment's share, the rough lobe's two shares - PhotonVct_piece_ps.any says the rules).
///   `posLS` the diffuse set's start (jahConeStart) and `biasDirLS` its unit normal, cascade 0's normalised space;
///   `origin` the set's origin plane (jahConeOrigin); `reflVecLS` the unit reflection in the same space;
///   `reflDir` / `normal` the reflection and the normal in ONE space of the caller's (their cosine is what is read);
///   `perceptualRoughness` the surface's; `flags` JAH_MARCH_SDF or 0 (JAH_MARCH_SPECULAR is always set).
/// A LOBE WIDER THAN 45 DEGREES IS NOT A CONE (PHOTON-VOXEL-3): at tan > 1 the march is the occlusion query of the lobe
/// (capped at tan 1, started at the diffuse start: no mirror image to slide), `roughLobe` says so, and `specAlpha` is
/// then the above-horizon share's visibility (BISTRO-FIXES-3); otherwise it is the lobe's escape.
struct JahSpecularCone
{
	JahConeResult march;
	float specAlpha;
	bool roughLobe;
	float below;
};
JahSpecularCone jahSpecularCone( vec3 posLS, vec3 biasDirLS, vec4 origin, vec3 reflVecLS, vec3 reflDir, vec3 normal,
								 float perceptualRoughness, uint flags )
{
	JahSpecularCone s;
	// 1 / (1 - 0.02) = 1.020408163
	const float vctRoughness = ( perceptualRoughness - 0.02 ) * 1.020408163;
	const float specConeAngleTan = max( 0.0, tan( vctRoughness * 0.5 * 3.141592654 * 0.99 ) );
	s.roughLobe = specConeAngleTan > 1.0;
	// the lobe's share below the surface is stopped by it (jahConeBelow)
	vec4 specOrigin = origin;
	specOrigin.w = jahConeBelow( normalize( reflDir ), normalize( normal ), min( specConeAngleTan, 1.0 ) );
	s.below = specOrigin.w;
	// ...and it starts ON the reflection, one cell above the surface, aged by the stretch it skipped
	vec4 start = jahSpecularConeStart( posLS - biasDirLS * JAH_VOX_INVRES( 0 ), reflVecLS, biasDirLS );
	if( s.roughLobe )
		start = vec4( posLS, 0.0 );
	s.march = jahConeMarchAged( start.xyz, reflVecLS, min( specConeAngleTan, 1.0 ), specOrigin,
								JAH_MARCH_SPECULAR | flags, start.w );
	s.specAlpha = 1.0 - min( 1.0, s.march.alpha / 0.95 );
	if( s.roughLobe )
	{
		const float above = clamp( ( s.march.alpha - s.below ) / max( 1.0 - s.below, 1e-4 ), 0.0, 1.0 );
		s.specAlpha = 1.0 - min( 1.0, above / 0.95 );
	}
	return s;
}

#endif   // JAH_VOXEL_CONES_GLSL
