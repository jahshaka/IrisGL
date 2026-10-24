// Jahshaka/ScreenIntegrate - THE NO-RAY ROUTE'S SCREEN INTEGRATE (PHOTON-7 stage 7, 7-6; L4 on every chain).
//
// On a chain that does not gather (no ray device, the project's ray row off, a Live or undeclared offscreen view) the
// opaque passes bind no world cache either: this job, between the prepass and the opaque passes (the chain's compute
// pass "Jahshaka screen integrate"), runs per pixel what the pixel shader's voxel route ran (PhotonVct_piece_ps.any's
// computeVctProbe, PhotonIfd_piece_ps.any and JahIfd_piece_ps.any) from the prepass's depth, normal and roughness, and
// writes the SAME screen products a gathering chain reads:
//   jahProbeIrradiance (u0, RGBA16F) - THE DIFFUSE, E/pi at the pixel's normal, w = 1 (the gather's convention):
//       the irradiance field's cage where one is bound (jah_field_read.glsl, the one text) and, where its confidence
//       fades, its fallback - the cone diffuse x (1 - confidence) under a cascade chain, the environment's diffuse x
//       (1 - confidence) under a single volume (JahIfd_piece_ps.any's two regimes; the cones are marched only where
//       that weight is above zero) - or, with no field, the cone diffuse (the four-cone set, JahVoxelCones);
//   jahRoughSpec (u1, RGBA16F) - THE CONE'S SPECULAR: rgb the voxels' share (the narrow lobe's march colour, fogged
//       along its leg by the one fog law; the rough lobe's stopped share), w the share of the lobe the voxels did not
//       stop - JahScreenCone_piece_ps.any composes the environment's share by the MATERIAL's rule in the pixel;
//   jahConeHit (u2, R16F) - the cone's hit distance along the reflection, metres (the parallax-corrected probes'
//       hybrid weight, the fork's forwardPlusDoCubemaps under jah_screen_cone).
// IN WORLD SPACE, the card job's construction (JahCardLight_cs.glsl): a world point reaches cascade 0's normalised
// space through the volume's box, the cone frame is jahConeBasisWorld( N ), the environment is read in world axes.
// THE SURFACE is the prepass's: its SHADING normal (the voxel route built the diffuse frame on the geometric one - they
// differ under a normal map) and its roughness.
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D sceneDepth;
vulkan_layout( ogre_t1 ) uniform texture2D gBufNormals;
vulkan_layout( ogre_t2 ) uniform texture2D gBufShadowRoughness;

@set( siTexUnit, 3 )
@property( hlms_num_vct_cascades )
	@set( vctTexUnit, siTexUnit )
	@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )
	vulkan( layout( ogre_s3 ) uniform sampler vSmp );
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbes[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeX[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeY[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeZ[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovP[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovN[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosP[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosN[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeBack[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeNrm[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	@set( siTexUnit, vctTexUnit )
@end
@property( jah_env )
	vulkan_layout( ogre_t@value(siTexUnit) ) uniform textureCube envCube;
	vulkan( layout( ogre_s@value(siTexUnit) ) uniform sampler envSmp );
	@add( siTexUnit, 1 )
@end
@property( jah_si_field )
	vulkan_layout( ogre_t@value(siTexUnit) ) uniform texture2D fieldIrradiance;
	vulkan( layout( ogre_s@value(siTexUnit) ) uniform sampler fieldSmp );
	@add( siTexUnit, 1 )
	vulkan_layout( ogre_t@value(siTexUnit) ) uniform texture2D fieldDepth;
	@add( siTexUnit, 1 )
@end
@property( jah_si_air )
	vulkan_layout( ogre_t@value(siTexUnit) ) uniform texture3D fogAerial;
	vulkan( layout( ogre_s@value(siTexUnit) ) uniform sampler fogSmp );
	@add( siTexUnit, 1 )
@end

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) ) uniform restrict writeonly image2D outIrradiance;
layout( vulkan( ogre_u1 ) vk_comma @insertpiece( uav1_pf_type ) ) uniform restrict writeonly image2D outSpecular;
layout( vulkan( ogre_u2 ) vk_comma @insertpiece( uav2_pf_type ) ) uniform restrict writeonly image2D outConeHit;

// THE JOB'S PARAMETERS (OgreScreenIntegrate.cpp fills them per view and frame; every member a vec4).
struct ScreenGiParams
{
	// THE CAMERA'S IMAGE (rq_sun_contact.comp's five numbers: camPos.w 1 perspective / 0 orthographic; the image ray
	// rayTL + rayRight u + rayDown v over the whole target, its component along fwd 1)
	vec4 camPos;
	vec4 rayTL;
	vec4 rayRight;
	vec4 rayDown;
	vec4 fwd;
	vec4 projParams;	// x = A, y = B of Camera::getProjectionParamsAB() (reverse-Z safe)
	vec4 viewAxisX;		// the view space's axes in world space (the G-buffer normal's rotation back)
	vec4 viewAxisY;
	vec4 viewAxisZ;
	vec4 resolution;	// xy = the target
	// THE CHAIN AND THE ENVIRONMENT (JahCardLight_cs.glsl's CardGiParams, member for member)
	vec4 chainInvRes[8];
	vec4 chainFromPrev[14];
	vec4 volumeOrigin;	// xyz = cascade 0's volume origin, world
	vec4 volumeInvSize;	// xyz = 1 / its size, world
	vec4 counts;		// x = cascades, y = the specular SDF's max mip, z = its factor
	vec4 envGainMips;	// xyz = the Sky Light's gain on the cube, w = its mips
	vec4 envSh[9];		// the environment's nine SH coefficients, world axes
	// THE IRRADIANCE FIELD for an IDENTITY view (PhotonIrradianceField::fillConstBufferData: three rows world to grid,
	// ( Nx, Nx * Ny ), the depth and irradiance atlases) and its window word ( slot packed, Ny, Nz )
	vec4 fieldRow0;
	vec4 fieldRow1;
	vec4 fieldRow2;
	vec4 fieldProbes;
	vec4 fieldDepthAtlas;
	vec4 fieldIrradAtlas;
	vec4 fieldWin;
	// THE FOG LAW (OgreScene::fogAlong's eight: jah_fog_along.glsl's JAH_FOG_P)
	vec4 fog[8];
};
layout( std430, ogre_U3 ) readonly restrict buffer screenGiLayout { ScreenGiParams p; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// THE ONE ENVIRONMENT (JahEnvironment), bound as every reader binds it
@property( jah_env )
	#define JAH_ENV_CUBE_ON true
	#define JAH_ENV_SAMPLE( d, l ) textureLod( samplerCube( envCube, envSmp ), d, l ).xyz
@else
	#define JAH_ENV_CUBE_ON false
	#define JAH_ENV_SAMPLE( d, l ) vec3( 0.0, 0.0, 0.0 )
@end
#define JAH_ENV_MIPS p.envGainMips.w
#define JAH_ENV_GAIN p.envGainMips.xyz
#define JAH_ENV_SH_C0 p.envSh[0].xyz
#define JAH_ENV_SH_C1 p.envSh[1].xyz
#define JAH_ENV_SH_C2 p.envSh[2].xyz
#define JAH_ENV_SH_C3 p.envSh[3].xyz
#define JAH_ENV_SH_C4 p.envSh[4].xyz
#define JAH_ENV_SH_C5 p.envSh[5].xyz
#define JAH_ENV_SH_C6 p.envSh[6].xyz
#define JAH_ENV_SH_C7 p.envSh[7].xyz
#define JAH_ENV_SH_C8 p.envSh[8].xyz
@insertpiece( JahEnvironment )

// THE FOG LAW (PHOTON-I-1: one law on every reflection path) on the cone's voxel share, as the ray jobs bind it
@property( jah_si_fog )
	#define JAH_FOG_P( k ) p.fog[k]
	@property( jah_si_air )
		#define JAH_FOG_AERIAL( uvw ) textureLod( sampler3D( fogAerial, fogSmp ), uvw, 0.0 )
		#define JAH_FOG_HAS_AIR
	@end
	#define JAH_FOG_HAS_HEIGHT_FOG
	@insertpiece( JahAtmosphere )
	@insertpiece( JahHeightFog )
	@insertpiece( JahFogAlong )
@end

@property( hlms_num_vct_cascades )
	#define JAH_VOX_MAX_CASCADES @value( hlms_num_vct_cascades )
	#define JAH_VOX_COUNT int( p.counts.x )
	#define JAH_VOX_SAMPLE_ISO( c, u, l ) textureLod( sampler3D( vctProbes[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_COVP( c, u, l ) textureLod( sampler3D( vctProbeCovP[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_COVN( c, u, l ) textureLod( sampler3D( vctProbeCovN[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_POSP( c, u, l ) textureLod( sampler3D( vctProbePosP[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_POSN( c, u, l ) textureLod( sampler3D( vctProbePosN[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_X( c, u, l ) textureLod( sampler3D( vctProbeX[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_Y( c, u, l ) textureLod( sampler3D( vctProbeY[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_Z( c, u, l ) textureLod( sampler3D( vctProbeZ[c], vSmp ), u, l )
	#define JAH_VOX_PER_SIDE 1
	#define JAH_VOX_SAMPLE_BACK( c, u, l ) textureLod( sampler3D( vctProbeBack[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_NRM( c, u, l ) textureLod( sampler3D( vctProbeNrm[c], vSmp ), u, l )
	#define JAH_VOX_INVRES( c ) p.chainInvRes[c].xyz
	#define JAH_VOX_MAXLOD( c ) p.chainInvRes[c].w
	#define JAH_VOX_FROM_PREV_SCALE( c ) p.chainFromPrev[( (c) - 1 ) * 2]
	#define JAH_VOX_FROM_PREV_OFFSET( c ) p.chainFromPrev[( (c) - 1 ) * 2 + 1]
	@property( jah_si_sdf )
		#define JAH_VOX_SDF_MAXMIP p.counts.y
		#define JAH_VOX_SDF_FACTOR p.counts.z
	@end
	@insertpiece( JahVoxelSample )
	@insertpiece( JahVoxelMarch )
	#define JAH_CONES_TO_LS( d ) normalize( ( d ) * p.volumeInvSize.xyz )
	#define JAH_CONES_TO_WORLD( d ) ( d )
	@insertpiece( JahVoxelCones )
@end

@property( jah_si_field )
	@insertpiece( JahFieldWindow )
	#define JAH_FIELD_IRRAD( uv ) textureLod( sampler2D( fieldIrradiance, fieldSmp ), uv, 0.0 )
	#define JAH_FIELD_DEPTH( uv ) textureLod( sampler2D( fieldDepth, fieldSmp ), uv, 0.0 ).xy
	@insertpiece( JahFieldRead )
@end

/// The view distance a depth texel stands for (reverse-Z safe, both projections) - rq_sun_contact.comp's.
float siViewDistance( float rawDepth )
{
	if( p.camPos.w < 0.5 )
		return abs( ( rawDepth - p.projParams.x ) / p.projParams.y );
	return p.projParams.y / ( rawDepth - p.projParams.x );
}

void main()
{
	const ivec2 full = ivec2( p.resolution.xy );
	const ivec2 pix = ivec2( gl_GlobalInvocationID.xy );
	if( any( greaterThanEqual( pix, full ) ) )
		return;
	const float rawDepth = texelFetch( sceneDepth, pix, 0 ).x;
	vec3 nVS = texelFetch( gBufNormals, pix, 0 ).xyz * 2.0 - 1.0;
	if( !( rawDepth > 0.0 && rawDepth < 1.0 && dot( nVS, nVS ) > 1e-6 ) )
	{
		// no surface: nothing reads these texels (the sky, a cleared pixel)
		imageStore( outIrradiance, pix, vec4( 0.0, 0.0, 0.0, 1.0 ) );
		imageStore( outSpecular, pix, vec4( 0.0, 0.0, 0.0, 1.0 ) );
		imageStore( outConeHit, pix, vec4( 0.0 ) );
		return;
	}
	nVS = normalize( nVS );
	// THE G-BUFFER CANNOT HOLD A ZERO: R10G10B10A2 x 2 - 1 puts 0 between two codes (511 and 512 of 1023 read
	// -0.00098 and +0.00098), so an axis-aligned surface arrives a code off its axis - and the cone frame is built about
	// the world's axes (jahConeBasis: Frisvad's special direction IS an axis, where a hair off it turns the frame by any
	// angle about the normal; measured: a world +z face's cone diffuse 20 % off the pixel's, which built it on the exact
	// geometric normal). In WORLD space a component within the code's reach - half a code per view axis, summed over
	// the three: 3 x 0.5 x 2 / 1023 - is zero.
	vec3 N = normalize( p.viewAxisX.xyz * nVS.x + p.viewAxisY.xyz * nVS.y + p.viewAxisZ.xyz * nVS.z );
	N = normalize( mix( N, vec3( 0.0 ), lessThanEqual( abs( N ), vec3( 3.0 / 1023.0 ) ) ) );
	const float dist = siViewDistance( rawDepth );
	const vec2 uv = ( vec2( pix ) + 0.5 ) / vec2( full );
	const vec3 plane = p.rayTL.xyz + p.rayRight.xyz * uv.x + p.rayDown.xyz * uv.y;
	const vec3 P = p.camPos.w < 0.5 ? p.camPos.xyz + plane + p.fwd.xyz * dist : p.camPos.xyz + plane * dist;
	// toward the eye (perspective) or the view axis (orthographic)
	const vec3 V = p.camPos.w < 0.5 ? -p.fwd.xyz : normalize( p.camPos.xyz - P );
	const vec3 R = reflect( -V, N );
	const float roughness = sqrt( max( texelFetch( gBufShadowRoughness, pix, 0 ).y * 0.999 + 0.001, 1e-6 ) );
	const float eyeDist = length( P - p.camPos.xyz );

	vec3 diffuse = vec3( 0.0 );
	vec4 specular = vec4( 0.0, 0.0, 0.0, 1.0 );
	float hitT = 0.0;
@property( hlms_num_vct_cascades )
	// THE CONE START (PhotonVct_piece_ps.any's jahVctPixelStart, in world space)
	vec3 posLS = ( P - p.volumeOrigin.xyz ) * p.volumeInvSize.xyz;
	const vec3 biasDirLS = jahConeBiasDir( normalize( N * p.volumeInvSize.xyz ) );
	posLS = jahConeStart( posLS, biasDirLS );
	const vec4 origin = jahConeOrigin( posLS, biasDirLS );
	const mat3 basis = jahConeBasisWorld( N );
@end

	// ---- THE DIFFUSE ------------------------------------------------------------------------------------------------
@property( jah_si_field )
	{
		const vec3 gridPos = vec3( dot( p.fieldRow0, vec4( P, 1.0 ) ), dot( p.fieldRow1, vec4( P, 1.0 ) ),
								   dot( p.fieldRow2, vec4( P, 1.0 ) ) );
		const vec3 gridNormal = normalize( vec3( dot( p.fieldRow0.xyz, N ), dot( p.fieldRow1.xyz, N ),
												 dot( p.fieldRow2.xyz, N ) ) );
		const vec3 gridView = vec3( dot( p.fieldRow0.xyz, V ), dot( p.fieldRow1.xyz, V ), dot( p.fieldRow2.xyz, V ) );
		const uint packed = uint( p.fieldWin.x + 0.5 );
		const JahFieldAnswer a = jahFieldRead( gridPos, gridNormal, gridView,
											   vec3( p.fieldProbes.x, p.fieldWin.y, p.fieldWin.z ),
											   uvec3( packed & 127u, ( packed >> 7u ) & 127u, ( packed >> 14u ) & 127u ),
											   p.fieldProbes.xy, p.fieldDepthAtlas, p.fieldIrradAtlas );
		diffuse = a.irradiance;
		const float fallback = 1.0 - a.confidence;
	@property( hlms_num_vct_cascades > 1 )
		// UNDER A CASCADE CHAIN THE FALLBACK IS THE CHAIN'S OWN BOUNCE (PHOTON_SPEC 13 G3): the cones, marched only
		// where the field's confidence leaves them a weight (SPEED-GPU, audit PH-2)
		if( fallback > 0.0 )
		{
			vec3 light;
			vec3 envD;
			jahDiffuseCones( posLS, origin, basis, light, envD );
			diffuse += ( light + envD ) * fallback;
		}
	@else
		// ...UNDER A SINGLE VOLUME, the environment's diffuse (what an unoccluded surface had before the field)
		if( fallback > 0.0 )
			diffuse += jahEnvIrradiance( N ) * fallback;
	@end
	}
@else
	@property( hlms_num_vct_cascades )
	{
		vec3 light;
		vec3 envD;
		jahDiffuseCones( posLS, origin, basis, light, envD );
		diffuse = light + envD;
	}
	@else
		// NO VOLUME BUILT YET (the chain's first frames): the environment's diffuse
		diffuse = jahEnvIrradiance( N );
	@end
@end

	// ---- THE CONE'S SPECULAR ----------------------------------------------------------------------------------------
@property( hlms_num_vct_cascades )
	{
		const vec3 reflVecLS = normalize( R * p.volumeInvSize.xyz );
		const JahSpecularCone sc = jahSpecularCone( posLS, biasDirLS, origin, reflVecLS, R, N, roughness,
	@property( jah_si_sdf )
													JAH_MARCH_SDF );
	@else
													0u );
	@end
		JahConeResult m = sc.march;
	@property( jah_si_fog )
		// THE FOG LAW ON THE VOXELS' SHARE: the light the cone gathered crossed the medium from where it came (the
		// opacity-weighted distance of the planes it took, cascade 0's units turned into metres along R) to this
		// surface; the eye's leg is the colour pass's. Unpremultiplied for the law, then the coverage again.
		if( m.alpha > 0.0 && p.fog[3].w > 0.5 )
		{
			const float at = m.takeAtC0 / m.alpha / max( length( R * p.volumeInvSize.xyz ), 1e-6 );
			m.colour = m.alpha * jahFogAlong( m.colour / m.alpha, P.y, R, at, eyeDist );
		}
	@end
		vec3 voxelShare = sc.roughLobe ? vec3( 0.0 ) : m.colour;
		if( sc.roughLobe )
		{
			// THE STOPPED SHARE'S LIGHT IS WHAT THE QUERY STOPPED ON (BISTRO-FIXES-3)
			const float occlusion = 1.0 - sc.specAlpha;
			const float above = clamp( ( m.alpha - sc.below ) / max( 1.0 - sc.below, 1e-4 ), 0.0, 1.0 );
			if( above > 0.0 )
				voxelShare += m.colour / max( 1.0 - sc.below, 1e-4 ) * ( occlusion / above );
		}
		specular = vec4( voxelShare, sc.specAlpha );
		// THE HIT: how far along the reflection the march read, cascade 0's units into metres
		hitT = m.travelledC0 / max( length( R * p.volumeInvSize.xyz ), 1e-6 );
	}
@end

	imageStore( outIrradiance, pix, vec4( diffuse, 1.0 ) );
	imageStore( outSpecular, pix, specular );
	imageStore( outConeHit, pix, vec4( hitT, 0.0, 0.0, 0.0 ) );
}
