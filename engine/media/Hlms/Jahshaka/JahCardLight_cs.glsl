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
// jahLightAttenuation / jahSpotAttenuation. (800.PixelShader still spells
// those two terms out inline at each of its light loops: the pixel side of
// that pair is not converged yet.) `pbsDirect()` in test_gi_field_energy.cpp
// writes the same arithmetic out in C++, so a test holds this to that closed
// form.
//
// A TEXEL IS READ FROM EVERY DIRECTION (a reflection ray's hit, a gather's),
// so it stores a view-INDEPENDENT diffuse. The INDIRECT half takes the lobe's
// HEMISPHERICAL mean albedo, jahDiffuseAlbedoHemi(r) — the bounce job's
// convention for the same "read from everywhere" situation (the voxel re-emits
// the same mean), so a card and a voxel hold one quantity (PHOTON-CARDS-2 audit
// F4, the lead's decision). THE DIRECT half keeps V = N (viewScatter = 1):
// fd90 there depends on the half vector of each outgoing direction, so its
// hemispherical mean has no closed form this file can write in one line; its
// mean over outgoing directions is 1 + (fd90 - 1) / 21 of V = N's, at most 7 %
// off at r = 1 and exact at r = 0 — stated, the one convention still split.
// The specular lobe is not cached (Lumen's rule: the surface cache is a
// diffuse store).
//
// VISIBILITY. The SUN's is the stored shadow term (the capture's PSSM term of
// the first shadow-casting directional light — the prepass writes that one
// term and nothing else). A point or spot light is UNSHADOWED here: its
// visibility is the traced residue's, where rays exist (PHOTON P5). Area
// lights are not summed (their LTC path is not transcribed). Stated, all three.
//
// INDIRECT = THE PIXEL'S OWN DIFFUSE GI, from the texel: the ONE diffuse
// cone integrator (JahVoxelCones, jah_voxel_cones.glsl — the frame, the cone
// set, the weights, the start bias, the escape weight; the same TEXT the
// pixel's computeVctProbe and the bounce job run) over the ONE voxel reader
// (JahVoxelSample + JahVoxelMarch) and the ONE environment (JahEnvironment).
// Decoded exactly as the pixel decodes it — the voxels' share times the
// volume's multiplier, the environment's share as radiance — and turned into
// outgoing radiance as BRDF_EnvMap does, envColourD x diffuse x pi x the lobe's
// albedo, with the albedo taken as its HEMISPHERICAL mean jahDiffuseAlbedoHemi
// (the fork's JahDiffuseAlbedo piece; above) where the pixel takes it at its own
// view angle. One term the pixel has that the card does not, stated: a
// diffuse fresnel (fresnelD — PbsBrdf::Default carries none; the
// SeparateDiffuseFresnel BRDFs do, and their card is brighter by 1 - F).
// And the frame is built on the STORED shading normal where the pixel builds
// it on the geometric one (they differ under a normal map).
//
// No at-sign in any comment of this file (the Hlms parser reads them).
@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

vulkan_layout( ogre_t0 ) uniform texture2D cardAlbedo;
vulkan_layout( ogre_t1 ) uniform texture2D cardNormal;
vulkan_layout( ogre_t2 ) uniform texture2D cardDepth;
vulkan_layout( ogre_t3 ) uniform texture2D cardEmissive;
vulkan_layout( ogre_t4 ) uniform texture2D cardShadowRough;

@property( hlms_num_vct_cascades )
	@pset( vctTexUnit, 5 )
	@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )
	vulkan( layout( ogre_s5 ) uniform sampler vSmp );
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
	@property( jah_env )
		vulkan_layout( ogre_t@value(vctTexUnit) ) uniform textureCube envCube;
	@end
@end

// ONE CARD TO RELIGHT. rect = (atlasX, atlasY, size, mode); camPos = the
// capture camera's position; axisU.xyz / axisV.xyz = the card's u and v with
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
};
layout( std430, ogre_U2 ) readonly restrict buffer giLayout { CardGiParams gp; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@property( hlms_num_vct_cascades )
	#define JAH_VOX_MAX_CASCADES @value( hlms_num_vct_cascades )
	#define JAH_VOX_COUNT int( gp.counts.x )
	#define JAH_VOX_SAMPLE_ISO( c, u, l ) textureLod( sampler3D( vctProbes[c], vSmp ), u, l )
	@property( vct_anisotropic )
		#define JAH_VOX_HAS_ANISO 1
		#define JAH_VOX_ANISO true
		#define JAH_VOX_SAMPLE_X( c, u, l ) textureLod( sampler3D( vctProbeX[c], vSmp ), u, l )
		#define JAH_VOX_SAMPLE_Y( c, u, l ) textureLod( sampler3D( vctProbeY[c], vSmp ), u, l )
		#define JAH_VOX_SAMPLE_Z( c, u, l ) textureLod( sampler3D( vctProbeZ[c], vSmp ), u, l )
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

	@property( jah_env )
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
		jahDiffuseCones( posLS, biasDirLS, dirLS, jahConeBasisWorld( N ), light, envD );
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
		vec3 kD = texelFetch( cardAlbedo, at, 0 ).xyz;
		vec3 nV = texelFetch( cardNormal, at, 0 ).xyz * 2.0 - 1.0;
		vec3 N = normalize( r.axisU.xyz * nV.x + r.axisV.xyz * nV.y + r.axisD.xyz * nV.z );
		vec2 sr = texelFetch( cardShadowRough, at, 0 ).xy;
		// Patch 0043's range: stored = (alpha - 0.001) * 1.001001.
		float alpha = sr.y / 1.001001 + 0.001;
		float perceptualRoughness = sqrt( max( alpha, 0.0 ) );
		// Image row 0 is the card's +v side (the capture camera's +Y).
		vec2 f = ( vec2( t ) + 0.5 ) / float( size );
		vec3 P = r.camPos.xyz + r.axisU.xyz * ( r.axisU.w * ( f.x - 0.5 ) ) +
				 r.axisV.xyz * ( r.axisV.w * ( 0.5 - f.y ) ) - r.axisD.xyz * depth;

		vec3 direct = vec3( 0.0, 0.0, 0.0 );
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
			float visibility = l.diffuse.w > 0.5 ? sr.x : 1.0;
			direct += l.diffuse.xyz * ( jahCardDiffuse( N, L, perceptualRoughness ) * atten * visibility );
		}

		// THE INDIRECT HALF (mode bit 1 marches, bit 2 says "none yet", else the
		// cached layer).
		if( ( mode & 1u ) != 0u )
		{
@property( hlms_num_vct_cascades )
			// BRDF_EnvMap's envColourD x diffuse x pi x the lobe's albedo, at its
			// hemispherical mean (the bounce's convention; the header says why).
			indirect = jahCardEnvColourD( P, N ) * kD * 3.141592654 *
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
