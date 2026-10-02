// THE DIFFUSE CONE INTEGRATOR — the one copy (PHOTON-CARDS-2 part A; the
// CARDS-1 audit's F3, the one-reader law).
//
// WHO READS IT. The pixel shader's diffuse GI (Hlms/Pbs/Any/Vct_piece_ps.any,
// computeVctProbe), the bounce-injection job (VCT/LightVctBounceInject_piece_cs.any)
// and the surface cache's card job (Hlms/Jahshaka/JahCardLight_cs.glsl), all
// through the piece `JahVoxelCones` the build WRAPS this file into
// (irisgl/engine/CMakeLists.txt). Before this file the integrator — the cone
// frame, the two cone sets, their weights and apertures, the start bias, the
// escape weight — existed three times: the pixel's, a line-for-line copy in the
// card job, and the bounce job's own (a cross-product frame about world X and
// the box's normalised axes taken as the world's). A card's bounce and the
// pixel's were two integrals of one volume; now they are one text.
//
// WHAT IT IS NOT: the march. Every cone is walked by jahConeMarch
// (jah_voxel_march.glsl, the piece JahVoxelMarch) and every escape reads the
// one environment (jahEnvCone, jah_environment.glsl, the piece JahEnvironment);
// both pieces are inserted BEFORE this one.
//
// HOW A CALLER BINDS IT — the march's macros and the environment's, and:
//
//     JAH_CONES_SIX           1 = the six-cone set (HlmsPbs's vct_cone_dirs 6,
//                             setVctFullConeCount), 0 = the four-cone set
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
/// the six cones of every axis-aligned surface to odd angles, where each sample
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

/// THE DIFFUSE GI AT A SURFACE POINT: the cone set walked from `posLS` (already
/// off the surface: jahConeStart), in the frame `basis` (third axis = the
/// normal, in the caller's space).
///   `light`  the voxels' share, weighted — in cascade 0's STORED units (the
///            caller decodes it by the volume's multiplier)
///   `envD`   the environment's share: each cone's ESCAPE (the part of the cone
///            the voxels did not stop, the march's escape estimate, not the
///            colour's opacity — they differ only where the anisotropic volumes
///            are read) times the one environment IN THAT CONE'S DIRECTION AT
///            THAT CONE'S APERTURE, weighted — radiance, in whatever units the
///            caller bound the environment in
/// THE TWO CONE SETS: six cones (one on the normal weighted 0.25, five at 60
/// degrees weighted 0.15, tan of the half angle 0.577 = 30 degrees) or four
/// (at 45 degrees, 0.25 each, tan 0.98269 = 44.5 degrees). The engine leaves
/// HlmsPbs at four. The weights carry the cosine; each of the four cones' escapes
/// reads its azimuthal quadrant of the hemisphere cosine-weighted (jahEnvQuadrant),
/// the six-cone set its own solid angle (jahEnvCone).
void jahDiffuseCones( vec3 posLS, vec4 origin, mat3 basis,
					  out vec3 light, out vec3 envD )
{
#if JAH_CONES_SIX
	const int kCones = 6;
	const vec3 coneDirs[6] = vec3[6]( vec3( 0.0, 0.0, 1.0 ),
									  vec3( 0.866025, 0.0, 0.5 ),
									  vec3( 0.267617, 0.823639, 0.5 ),
									  vec3( -0.700629, 0.509037, 0.5 ),
									  vec3( -0.700629, -0.509037, 0.5 ),
									  vec3( 0.267617, -0.823639, 0.5 ) );
	const float coneWeights[6] = float[6]( 0.25, 0.15, 0.15, 0.15, 0.15, 0.15 );
	const float coneAngleTan = 0.577;
	const uint coneFlags = 0u;
#else
	const int kCones = 4;
	const vec3 coneDirs[4] = vec3[4]( vec3( 0.707107, 0.0, 0.707107 ),
									  vec3( 0.0, 0.707107, 0.707107 ),
									  vec3( -0.707107, 0.0, 0.707107 ),
									  vec3( 0.0, -0.707107, 0.707107 ) );
	const float coneWeights[4] = float[4]( 0.25, 0.25, 0.25, 0.25 );
	const float coneAngleTan = 0.98269;
	const uint coneFlags = 0u;
#endif
	light = vec3( 0.0, 0.0, 0.0 );
	envD = vec3( 0.0, 0.0, 0.0 );
	for( int i = 0; i < kCones; ++i )
	{
		vec3 d = basis * coneDirs[i];
		vec4 coneOrigin = origin;
		coneOrigin.w = jahConeBelow( normalize( d ), basis[2], coneAngleTan );
		JahConeResult result = jahConeMarch( posLS, JAH_CONES_TO_LS( d ), coneAngleTan, coneOrigin, coneFlags );
		light += coneWeights[i] * result.colour;
		envD += coneWeights[i] * ( 1.0 - min( 1.0, result.alpha / 0.95 ) ) *
#if JAH_CONES_SIX
				jahEnvCone( JAH_CONES_TO_WORLD( d ), coneAngleTan );
#else
				jahEnvQuadrant( JAH_CONES_TO_WORLD( d ), JAH_CONES_TO_WORLD( basis[2] ) );   // its quadrant, cosine-weighted
#endif
	}
}

/// THE SKY'S SHARE ON A FINER QUADRATURE (CONTACT-OCCLUSION-1): what jahDiffuseCones'
/// `envD` estimates, on sixteen 20.4-degree cones instead of the set's four 44.5-degree
/// (or six 30-degree) ones - cosine-distributed, four rings of four, equal weights (each
/// the same projected solid angle), the ring below the horizon's rim stopped by the
/// surface (jahConeBelow), over the same march and the same environment lookup.
///
/// WHY A SECOND QUADRATURE FOR THE SKY. The four-cone set puts two axes ON the horizon of
/// a vertical surface standing on a floor, and a cone that wide marching along an infinite
/// plane saturates on it although half of it is sky: a white wall under the sky alone
/// stored 0.39-0.49 of its analytic sky share (albedo x the lobe x 1/2) where an open floor
/// stored 0.99 (gi.contact_occlusion's store readout); with this set 0.81-0.96. The sky
/// term a SURFACE STORE re-emits - the voxel's (the bounce job) and the card's (the
/// relight) - is computed once per injection or relight, so it takes the finer set; the
/// per-pixel diffuse keeps the fast one (a pixel's own cones run only with the field off).
vec3 jahSkyShareFine( vec3 posLS, vec4 origin, mat3 basis )
{
	const float kTan = 0.372;
	vec3 sky = vec3( 0.0, 0.0, 0.0 );
	for( int i = 0; i < 4; ++i )
	{
		const float u1 = ( float( i ) + 0.5 ) * 0.25;
		const float r = sqrt( u1 );
		const float z = sqrt( 1.0 - u1 );
		for( int j = 0; j < 4; ++j )
		{
			const float phi = 1.5707963 * ( float( j ) + 0.5 * float( i & 1 ) ) + 0.3926991;
			const vec3 d = basis * vec3( r * cos( phi ), r * sin( phi ), z );
			vec4 o = origin;
			o.w = jahConeBelow( normalize( d ), basis[2], kTan );
			JahConeResult res = jahConeMarch( posLS, JAH_CONES_TO_LS( d ), kTan, o, 0u );
			sky += ( 1.0 - min( 1.0, res.alpha / 0.95 ) ) * jahEnvCone( JAH_CONES_TO_WORLD( d ), kTan );
		}
	}
	return sky * 0.0625;
}

#endif   // JAH_VOXEL_CONES_GLSL
