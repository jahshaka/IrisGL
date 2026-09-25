// Jahshaka/CardLight — THE LIT CARD (PHOTON-CARDS-1, SC-1c): one texel of the
// surface cache's atlas per thread, its RADIANCE written into the sixth layer.
//
// WHAT A TEXEL IS. The capture (JahCardCapture_piece_ps.any) stored, per card
// texel, kD (the datablock's diffuse ALREADY divided by pi), the shading
// normal in the capture camera's view space, the distance from the capture
// camera's plane, the emissive radiance, and (shadow term, GGX alpha). The
// camera was orthographic, so the texel's WORLD POSITION is arithmetic: the
// camera position, plus the texel's place in the ortho window along u and v,
// minus the stored distance along the card's outward axis d. One workgroup
// z-slice per card (the host's relight list), 8 x 8 threads, a 128 page is
// 16 x 16 groups; a smaller card's surplus threads return.
//
// RADIANCE = DIRECT + INDIRECT + EMISSIVE, and the two lit halves have two
// budgets (OgreSurfaceCache.cpp, planRelights): the direct half is a light
// loop and is recomputed at every relight; the INDIRECT half is the voxel
// march and is cached in its own layer (`cardIndirect`), recomputed only when
// the card asks for it (mode bit 1: a fresh capture, or the chain re-injected)
// and otherwise read back (mode 0) — or zero (mode bit 2: a card captured
// this frame whose march did not fit the indirect budget yet).
//
// DIRECT = the sum over the scene's lights of HlmsPbs's own BRDF_Default
// diffuse lobe — jahDisneyDiffuse, the fork's JahBrdf piece
// (Hlms/Pbs/Any/JahBrdf_piece_all.any), the SAME function BRDF_Default calls —
// with the light terms of 800.PixelShader_piece_ps.any's DoPointLights /
// DoSpotLights (the attenuation 1 / (0.5 + (linear + quadratic d) d), the
// range fade of fork change 0018, the spot cone) as JahBrdf's
// jahLightAttenuation / jahSpotAttenuation — the functions 800.PixelShader's
// own light loops call (PHOTON-CARDS-5 measured a lamp at three distances: the
// card 0.987-0.993 of the head-on pixel). `pbsDirect()` in test_gi_field_energy.cpp
// writes the same arithmetic out in C++, so a test holds this to that closed
// form.
//
// A TEXEL IS READ FROM EVERY DIRECTION (a reflection ray's hit, a gather's),
// so it stores the HEAD-ON diffuse and the READ restores the view term
// (JahCardView, jah_card_view.glsl — PHOTON-CARDS-5): THE DIRECT half at V = N
// (viewScatter = 1, fd90 on H = normalize( L + N )), with the texel's mean
// light direction written beside it (the Albedo and Normal layers' alpha) so
// the read takes the lobe at the ray's own V; THE ENVIRONMENT half at the
// lobe's HEMISPHERICAL mean albedo, jahDiffuseAlbedoHemi(r) — the bounce job's
// convention (a voxel re-emits the same mean; PHOTON-CARDS-2 audit F4), which
// the read turns into the directional albedo at its own N.V. The specular lobe
// is not cached (Lumen's rule: the surface cache is a diffuse store).
//
// VISIBILITY. The SUN's is the stored shadow term (the capture's PSSM term of
// the first shadow-casting directional light — the prepass writes that one
// term and nothing else; its casters are the still world) times the MOVERS'
// term, traced by the ray tier into cardMoverVis for the cards a mover's
// footprint reaches (PHOTON-CARDS-4). A point or spot light is UNSHADOWED here: its
// visibility is the traced residue's, where rays exist (PHOTON P5). Area
// lights are not summed (their LTC path is not transcribed). Stated, all three.
//
// INDIRECT = THE PIXEL'S OWN DIFFUSE ENVIRONMENT TERM, from the texel. With GI
// OFF it is the SH ambient at the texel's normal (the engine's SH x gain, the
// pixel's envColourD); with the chain it is the pixel's diffuse GI: the ONE diffuse
// cone integrator (JahVoxelCones, jah_voxel_cones.glsl — the frame, the cone
// set, the weights, the start bias, the escape weight; the same TEXT the
// pixel's computeVctProbe and the bounce job run) over the ONE voxel reader
// (JahVoxelSample + JahVoxelMarch) and the ONE environment (JahEnvironment).
// Decoded exactly as the pixel decodes it — the voxels' share times the
// volume's multiplier, the environment's share as radiance — and turned into
// outgoing radiance as BRDF_EnvMap does, envColourD x diffuse x pi x the lobe's
// albedo, with the albedo taken as its HEMISPHERICAL mean jahDiffuseAlbedoHemi
// (the fork's JahDiffuseAlbedo piece; above) where the pixel takes it at its own
// view angle (the read restores that). One term the pixel has that the card does not, stated: a
// diffuse fresnel (fresnelD — PbsBrdf::Default carries none; the
// SeparateDiffuseFresnel BRDFs do, and their card is brighter by 1 - F).
// And the frame is built on the card's PLANE where the stored normal is the
// plane's within the format's quantum, on the STORED shading normal elsewhere,
// where the pixel builds it on the geometric one (they differ under a normal map).
//
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

// (The Albedo and Normal layers are UAVs, u6 and u7 below: the job writes the
// mean light direction into their alpha — JahCardView.)
vulkan_layout( ogre_t0 ) uniform texture2D cardDepth;
vulkan_layout( ogre_t1 ) uniform texture2D cardEmissive;
vulkan_layout( ogre_t2 ) uniform texture2D cardShadowRough;

@set( jahCloudUnit, 3 )
@property( hlms_num_vct_cascades )
	@pset( vctTexUnit, 3 )
	@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )
	vulkan( layout( ogre_s3 ) uniform sampler vSmp );
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbes[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	@property( vct_anisotropic )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeX[@value( hlms_num_vct_cascades )];
		@add( vctTexUnit, hlms_num_vct_cascades )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeY[@value( hlms_num_vct_cascades )];
		@add( vctTexUnit, hlms_num_vct_cascades )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeZ[@value( hlms_num_vct_cascades )];
		@add( vctTexUnit, hlms_num_vct_cascades )
	@end
	// PHOTON-VOXEL-3: the per-axis coverage, the light-volume list's last kind.
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovP[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovN[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	// PHOTON-VOXEL-4: the per-axis surface position, the light-volume list's last kind.
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosP[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosN[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	// PHOTON-VOXEL-5: the anisotropic tiers' level-0 back side and the voxeliser's normal (the
	// list's last two kinds: VctLighting::backIndex / normalIndex).
	@property( vct_anisotropic )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeBack[@value( hlms_num_vct_cascades )];
		@add( vctTexUnit, hlms_num_vct_cascades )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeNrm[@value( hlms_num_vct_cascades )];
		@add( vctTexUnit, hlms_num_vct_cascades )
	@end
	@property( jah_env )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform textureCube envCube;
		@add( vctTexUnit, 1 )
	@end
	@set( jahCloudUnit, vctTexUnit )
@end
// THE CLOUD LAYER'S FIELD (CLOUDS-2D-2), the last unit: the host binds it, with
// its wrapped sampler, only while a 2D cloud layer shades the sun.
@property( jah_cloud_shadow )
	vulkan_layout( ogre_t@value(jahCloudUnit) ) uniform texture2D cloudField;
	vulkan( layout( ogre_s@value(jahCloudUnit) ) uniform sampler cloudSmp );
@end

// ONE CARD TO RELIGHT. rect = (atlasX, atlasY, size, mode); camPos = the
// capture camera's position, .w = 1 when the card carries a movers' trace; axisU.xyz / axisV.xyz = the card's u and v with
// the ortho window's width / height in .w; axisD.xyz = the card's outward axis.
struct CardRelight
{
	vec4 rect;
	vec4 camPos;
	vec4 axisU;
	vec4 axisV;
	vec4 axisD;
};
layout( std430, ogre_U0 ) readonly restrict buffer relightLayout { CardRelight relights[]; };

// ONE LIGHT, in WORLD space, in the pass buffer's own layout (OgreHlmsPbs.cpp's
// light0Buf writer): position.xyz = the position, or the direction TOWARDS a
// directional light; position.w = the type (0 directional, 1 point, 2 spot);
// diffuse.xyz = colour times power; diffuse.w = 1 when its visibility is the
// captured shadow term; attenuation = (range, linear, quadratic, 1 / range);
// spotParams = (1 / (cos inner/2 - cos outer/2), cos outer/2, falloff).
struct CardLight
{
	vec4 position;
	vec4 diffuse;
	vec4 attenuation;
	vec4 spotDirection;
	vec4 spotParams;
};
layout( std430, ogre_U1 ) readonly restrict buffer lightLayout
{
	uvec4 lightCount;
	CardLight lights[];
};

// (The UAV slots: the three buffers first, then the two images — the root
// layout wants each kind contiguous.)
layout( vulkan( ogre_u3 ) vk_comma @insertpiece( uav3_pf_type ) )
uniform restrict writeonly image2D cardRadiance;
layout( vulkan( ogre_u4 ) vk_comma @insertpiece( uav4_pf_type ) )
uniform restrict image2D cardIndirect;
// THE MOVERS' VISIBILITY (PHOTON-CARDS-4): the ray tier's trace of the sun
// against the shadow-casting movers (rq_card_movers.comp), read where the card
// carries one (camPos.w = 1). The captured term holds the still world only.
layout( vulkan( ogre_u5 ) vk_comma @insertpiece( uav5_pf_type ) )
uniform restrict readonly image2D cardMoverVis;
// THE ALBEDO AND NORMAL LAYERS (PHOTON-CARDS-5): read here as the capture copied
// them, and their ALPHA (which no reader used: the capture writes 1) rewritten
// with the texels' mean light direction, octahedral (JahCardView) — the card
// read restores the diffuse lobe's view term from it.
layout( vulkan( ogre_u6 ) vk_comma @insertpiece( uav6_pf_type ) )
uniform restrict image2D cardAlbedo;
layout( vulkan( ogre_u7 ) vk_comma @insertpiece( uav7_pf_type ) )
uniform restrict image2D cardNormal;

// THE CHAIN AND THE ENVIRONMENT, as VctLighting hands them to every reader
// (getCascadeChainParams, the cascade-0 volume's box, getFinalMultiplier,
// getEnvironmentCube / Gain / Sh).
struct CardGiParams
{
	vec4 chainInvRes[8];
	vec4 chainFromPrev[14];
	vec4 volumeOrigin;		// xyz = cascade 0's volume origin, world
	vec4 volumeInvSize;		// xyz = 1 / its size, world
	vec4 counts;			// x = cascades, y = the decode multiplier
	vec4 envGainMips;		// xyz = the Sky Light's gain on the cube, w = its mips
	vec4 envSh[9];			// the environment's nine SH coefficients, world axes
	vec4 cloudMap;			// the cloud shadow: 1 / tile, strength, scroll xz (CLOUDS-2D-2)
	vec4 cloudSun;			// ...and the sun's throw xz, the altitude, 1 / mu_s
};
layout( std430, ogre_U2 ) readonly restrict buffer giLayout { CardGiParams gp; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

// THE ONE ENVIRONMENT (JahEnvironment), bound with or without the chain: with
// GI OFF its SH is the pixel's whole ambient (PHOTON-CARDS-5), and the host
// fills envSh from the scene's own coefficients (the sky's SH x the Sky Light's
// gain, what the engine pushes to HlmsPbs) when no chain hands them over.
@property( hlms_num_vct_cascades && jah_env )
	#define JAH_ENV_CUBE_ON true
	#define JAH_ENV_SAMPLE( d, l ) textureLod( samplerCube( envCube, vSmp ), d, l ).xyz
@else
	#define JAH_ENV_CUBE_ON false
	#define JAH_ENV_SAMPLE( d, l ) vec3( 0.0, 0.0, 0.0 )
@end
#define JAH_ENV_MIPS gp.envGainMips.w
#define JAH_ENV_GAIN gp.envGainMips.xyz
#define JAH_ENV_SH_C0 gp.envSh[0].xyz
#define JAH_ENV_SH_C1 gp.envSh[1].xyz
#define JAH_ENV_SH_C2 gp.envSh[2].xyz
#define JAH_ENV_SH_C3 gp.envSh[3].xyz
#define JAH_ENV_SH_C4 gp.envSh[4].xyz
#define JAH_ENV_SH_C5 gp.envSh[5].xyz
#define JAH_ENV_SH_C6 gp.envSh[6].xyz
#define JAH_ENV_SH_C7 gp.envSh[7].xyz
#define JAH_ENV_SH_C8 gp.envSh[8].xyz
@insertpiece( JahEnvironment )

@property( hlms_num_vct_cascades )
	#define JAH_VOX_MAX_CASCADES @value( hlms_num_vct_cascades )
	#define JAH_VOX_COUNT int( gp.counts.x )
	#define JAH_VOX_SAMPLE_ISO( c, u, l ) textureLod( sampler3D( vctProbes[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_COVP( c, u, l ) textureLod( sampler3D( vctProbeCovP[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_COVN( c, u, l ) textureLod( sampler3D( vctProbeCovN[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_POSP( c, u, l ) textureLod( sampler3D( vctProbePosP[c], vSmp ), u, l )
	#define JAH_VOX_SAMPLE_POSN( c, u, l ) textureLod( sampler3D( vctProbePosN[c], vSmp ), u, l )
	@property( vct_anisotropic )
		#define JAH_VOX_HAS_ANISO 1
		#define JAH_VOX_ANISO true
		#define JAH_VOX_SAMPLE_X( c, u, l ) textureLod( sampler3D( vctProbeX[c], vSmp ), u, l )
		#define JAH_VOX_SAMPLE_Y( c, u, l ) textureLod( sampler3D( vctProbeY[c], vSmp ), u, l )
		#define JAH_VOX_SAMPLE_Z( c, u, l ) textureLod( sampler3D( vctProbeZ[c], vSmp ), u, l )
		#define JAH_VOX_HAS_BACK 1
		#define JAH_VOX_SAMPLE_BACK( c, u, l ) textureLod( sampler3D( vctProbeBack[c], vSmp ), u, l )
		#define JAH_VOX_SAMPLE_NRM( c, u, l ) textureLod( sampler3D( vctProbeNrm[c], vSmp ), u, l )
	@else
		#define JAH_VOX_HAS_ANISO 0
		#define JAH_VOX_ANISO false
	@end
	#define JAH_VOX_INVRES( c ) gp.chainInvRes[c].xyz
	#define JAH_VOX_MAXLOD( c ) gp.chainInvRes[c].w
	#define JAH_VOX_FROM_PREV_SCALE( c ) gp.chainFromPrev[( (c) - 1 ) * 2]
	#define JAH_VOX_FROM_PREV_OFFSET( c ) gp.chainFromPrev[( (c) - 1 ) * 2 + 1]
	@insertpiece( JahVoxelSample )
	@insertpiece( JahVoxelMarch )

	// THE ONE DIFFUSE CONE INTEGRATOR (JahVoxelCones): the frame is built from
	// the texel's WORLD normal (jahConeBasisWorld: the pixel's cubemap-frame
	// construction, brought back to world axes), a world direction reaches the
	// volume's normalised space through the box, and the environment is read in
	// world axes as it is.
	@property( vct_cone_dirs == 6 )
		#define JAH_CONES_SIX 1
	@else
		#define JAH_CONES_SIX 0
	@end
	#define JAH_CONES_TO_LS( d ) normalize( ( d ) * gp.volumeInvSize.xyz )
	#define JAH_CONES_TO_WORLD( d ) ( d )
	@insertpiece( JahVoxelCones )

	// THE PIXEL'S DIFFUSE GI at world point P with normal N: envColourD, i.e.
	// the voxels' share times the multiplier plus the environment's share.
	vec3 jahCardEnvColourD( vec3 P, vec3 N )
	{
		vec3 posLS = ( P - gp.volumeOrigin.xyz ) * gp.volumeInvSize.xyz;
		vec3 dirLS = normalize( N * gp.volumeInvSize.xyz );
		vec3 biasDirLS = jahConeBiasDir( dirLS );
		posLS = jahConeStart( posLS, biasDirLS );
		vec3 light;
		vec3 envD;
		jahDiffuseCones( posLS, jahConeOrigin( posLS, biasDirLS ), jahConeBasisWorld( N ), light, envD );
		return light * gp.counts.y + envD;
	}
@end

// THE STORE ROUNDS TO NEAREST. The device converts a float into R11G11B10F by
// TRUNCATION (measured: 0.50829 stored as 0.5), so a channel lost up to one
// whole mantissa step — 6 mantissa bits on red and green, 5 on blue. Adding
// HALF A STEP of the target format to the float's own bits first — bit 16 for
// red and green (23 - 6 - 1), bit 17 for blue (23 - 5 - 1) — makes the
// truncation a round to nearest in EVERY octave (a carry into the exponent is
// the octave's own round-up): half a step at most. What stood here was a
// constant pre-scale, v x (1 + 1/128, 1 + 1/128, 1 + 1/64): half a step only at
// the BOTTOM of an octave, a whole step at its top — it read blue 1.1-1.5 %
// HIGH on gi.card_lighting's crate top (a value at 1.6 x its octave's floor).
// Below the format's smallest normal (2^-14) its step is absolute and this adds
// less than half of it: a radiance under 6e-5, stated. Only on that format (the
// host's property): the RGBA16F fallback stores unrounded.
vec3 jahCardRound( vec3 v )
{
@property( jah_card_round_r11g11b10 )
	// Clamped to [0, the format's largest finite value] (65024 on red and green,
	// 64512 on blue) before the add: +Inf's bits plus half a step are a NaN
	// pattern (audit F8), and so is the largest FLOAT's; the format's own maximum
	// stays finite through the add and stores as itself.
	const uvec3 bits = floatBitsToUint( clamp( v, vec3( 0.0, 0.0, 0.0 ),
											   vec3( 65024.0, 65024.0, 64512.0 ) ) );
	return uintBitsToFloat( bits + uvec3( 0x10000u, 0x10000u, 0x20000u ) );
@else
	return v;
@end
}

@insertpiece( JahBrdf )
@insertpiece( JahDiffuseAlbedo )
@insertpiece( JahCardView )
@property( jah_cloud_shadow )
	#define JAH_CLOUD_TAU( uv ) textureLod( sampler2D( cloudField, cloudSmp ), uv, 0.0 ).x
	@insertpiece( JahCloudShadow )
@end

// BRDF_Default's diffuse at V = N (NdotV = 1, so viewScatter is 1), times NdotL.
float jahCardDiffuse( vec3 N, vec3 L, float perceptualRoughness )
{
	float NdotL = clamp( dot( N, L ), 0.0, 1.0 );
	vec3 H = normalize( L + N );
	float VdotH = clamp( dot( N, H ), 0.0, 1.0 );
	return NdotL * jahDisneyDiffuse( NdotL, 1.0, VdotH, perceptualRoughness );
}

void main()
{
	CardRelight r = relights[gl_WorkGroupID.z];
	uint size = uint( r.rect.z );
	uint mode = uint( r.rect.w );
	uvec2 t = gl_GlobalInvocationID.xy;
	if( t.x >= size || t.y >= size )
		return;
	ivec2 at = ivec2( uint( r.rect.x ) + t.x, uint( r.rect.y ) + t.y );

	vec3 radiance = vec3( 0.0, 0.0, 0.0 );
	vec3 indirect = vec3( 0.0, 0.0, 0.0 );
	float depth = texelFetch( cardDepth, at, 0 ).x;
	if( depth > 0.0 )
	{
		// Read raw and written back raw, the alpha replaced (no re-quantisation).
		const vec4 albedoRaw = imageLoad( cardAlbedo, at );
		const vec4 normalRaw = imageLoad( cardNormal, at );
		vec3 kD = albedoRaw.xyz;
		vec3 nV = normalRaw.xyz * 2.0 - 1.0;
		vec3 N = normalize( r.axisU.xyz * nV.x + r.axisV.xyz * nV.y + r.axisD.xyz * nV.z );
		// THE CONE FRAME ON THE CARD'S PLANE (PHOTON-VOXEL-4). The card is a planar capture
		// along axisD, and a texel of a surface IN that plane has the plane's normal exactly;
		// the stored 8-bit normal cannot hold it (0 decodes to -0.0039: 0.32 degrees off), and
		// the four-cone set's 45-degree axes put the reader's plane axis on a tie that tilt
		// breaks - up to 5.7 % of the indirect (gi.cone_integrator_parity). A stored normal
		// within the format's quantum of the card's axis (1 degree) IS the plane: the frame
		// takes axisD. Any other texel (a curved or oblique surface the card also holds)
		// keeps its stored normal.
		const vec3 jahCardPlaneN = normalize( r.axisD.xyz );
		const vec3 Ncone = dot( N, jahCardPlaneN ) > 0.99985 ? jahCardPlaneN : N;
		vec2 sr = texelFetch( cardShadowRough, at, 0 ).xy;
		// Patch 0043's range: stored = (alpha - 0.001) * 1.001001.
		float alpha = sr.y / 1.001001 + 0.001;
		float perceptualRoughness = sqrt( max( alpha, 0.0 ) );
		// Image row 0 is the card's +v side (the capture camera's +Y).
		vec2 f = ( vec2( t ) + 0.5 ) / float( size );
		vec3 P = r.camPos.xyz + r.axisU.xyz * ( r.axisU.w * ( f.x - 0.5 ) ) +
				 r.axisV.xyz * ( r.axisV.w * ( 0.5 - f.y ) ) - r.axisD.xyz * depth;

		// THE SUN'S TWO TERMS: the captured one (the still world) times the
		// traced one (the movers), where the host traced this card.
		float moverVis = r.camPos.w > 0.5 ? imageLoad( cardMoverVis, at ).x : 1.0;
		vec3 direct = vec3( 0.0, 0.0, 0.0 );
		// THE MEAN LIGHT DIRECTION (JahCardView): each light's direction weighted
		// by its share of the direct half's luminance.
		vec3 lightMean = vec3( 0.0, 0.0, 0.0 );
		uint n = lightCount.x;
		for( uint i = 0u; i < n; ++i )
		{
			CardLight l = lights[i];
			float type = l.position.w;
			vec3 L;
			float atten = 1.0;
			if( type < 0.5 )
			{
				L = l.position.xyz;
			}
			else
			{
				L = l.position.xyz - P;
				float d = length( L );
				if( d > l.attenuation.x || d <= 0.0 )
					continue;
				L *= 1.0 / d;
				atten = jahLightAttenuation( d, l.attenuation );
				if( type > 1.5 )
				{
					float spotCosAngle = dot( -L, l.spotDirection.xyz );
					if( spotCosAngle < l.spotParams.y )
						continue;
					atten *= jahSpotAttenuation( spotCosAngle, l.spotParams.xyz );
				}
			}
			float visibility = l.diffuse.w > 0.5 ? sr.x * moverVis : 1.0;
@property( jah_cloud_shadow )
			// A DIRECTIONAL LIGHT CROSSES THE CLOUD SHEET (CLOUDS-2D-2): the
			// pixel's own factor, at this texel's world point.
			if( type < 0.5 )
				visibility *= jahCloudTransmittance( P, gp.cloudMap, gp.cloudSun );
@end
			// The PLANE-SNAPPED normal (Ncone), the one the read's view-term ratio
			// divides by (jah_rq_card.glsl): the ratio is then exact by text.
			const vec3 share = l.diffuse.xyz * ( jahCardDiffuse( Ncone, L, perceptualRoughness ) * atten * visibility );
			direct += share;
			lightMean += L * dot( share, vec3( 0.2126, 0.7152, 0.0722 ) );
		}
		// No light reaches the texel: its direct half is zero and the direction is
		// never used; the normal stands in.
		const vec3 lightDir = dot( lightMean, lightMean ) > 1e-20 ? normalize( lightMean ) : Ncone;
		const vec2 lightOct = jahCardOctEncode( lightDir );
		imageStore( cardAlbedo, at, vec4( albedoRaw.xyz, lightOct.x ) );
		imageStore( cardNormal, at, vec4( normalRaw.xyz, lightOct.y ) );

		// THE INDIRECT HALF (mode bit 1 marches, bit 2 says "none yet", else the
		// cached layer).
		if( ( mode & 1u ) != 0u )
		{
@property( hlms_num_vct_cascades )
			// BRDF_EnvMap's envColourD x diffuse x pi x the lobe's albedo, at its
			// hemispherical mean (the bounce's convention; the header says why).
			indirect = jahCardEnvColourD( P, Ncone ) * kD * 3.141592654 *
					   jahDiffuseAlbedoHemi( perceptualRoughness );
@else
			// GI OFF (PHOTON-CARDS-5): the pixel's envColourD is the SH irradiance
			// at its normal — the sky's SH x the Sky Light's gain, the engine's own
			// coefficients — so the card's environment half is that, through the
			// same BRDF_EnvMap arithmetic. Cached like the march: the SH is in the
			// indirect signature (OgreScene::updateSurfaceCache), so a change
			// re-runs this branch.
			indirect = jahEnvIrradiance( Ncone ) * kD * 3.141592654 *
					   jahDiffuseAlbedoHemi( perceptualRoughness );
@end
		}
		else if( ( mode & 2u ) == 0u )
		{
			indirect = imageLoad( cardIndirect, at ).xyz;
		}
		radiance = direct * kD + indirect + texelFetch( cardEmissive, at, 0 ).xyz;
	}
	if( ( mode & 3u ) != 0u )
		imageStore( cardIndirect, at, vec4( jahCardRound( indirect ), 1.0 ) );
	imageStore( cardRadiance, at, vec4( jahCardRound( radiance ), 1.0 ) );
}
