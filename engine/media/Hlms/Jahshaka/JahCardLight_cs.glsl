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
// diffuse lobe, TRANSCRIBED from 200.BRDFs_piece_ps.any (BRDF_Default, the
// normalised Disney diffuse: energyBias, energyFactor, fd90, lightScatter,
// viewScatter) and the light loops of 800.PixelShader_piece_ps.any
// (DoDirectionalLights / DoPointLights / DoSpotLights: the attenuation
// 1 / (0.5 + (linear + quadratic d) d), the range fade of patch 0018, the spot
// cone) — the same arithmetic `pbsDirect()` in test_gi_field_energy.cpp writes
// out in C++, so a test can hold this to that closed form.
//
// THE VIEW DIRECTION IS THE NORMAL. A cached texel is read from any direction,
// so it stores the view-INDEPENDENT diffuse: V = N makes the lobe's one
// view-dependent factor (viewScatter, which multiplies (1 - NdotV)^5) exactly
// 1. The specular lobe is not cached (Lumen's rule: the surface cache is a
// diffuse store).
//
// VISIBILITY. The SUN's is the stored shadow term (the capture's PSSM term of
// the first shadow-casting directional light — the prepass writes that one
// term and nothing else). A point or spot light is UNSHADOWED here: its
// visibility is the traced residue's, where rays exist (PHOTON P5). Area
// lights are not summed (their LTC path is not transcribed). Stated, all three.
//
// INDIRECT = THE PIXEL'S OWN DIFFUSE GI, from the texel: the ONE voxel reader
// (JahVoxelSample + JahVoxelMarch, jah_voxel_march.glsl) walked over the
// cones of Vct_piece_ps.any's computeVctProbe — the same cone set, the same
// weights, the same world-anchored frame (patch 0083's buildConeBasis in the
// cubemap frame), the same one-cell bias along the normal (patch 0070) — and
// each cone's escape reading the ONE environment (JahEnvironment's
// jahEnvCone) at the cone's aperture. Decoded exactly as the pixel decodes it
// — the voxels' share times the volume's multiplier, the environment's share
// as radiance — and turned into outgoing radiance exactly as BRDF_EnvMap
// does: envColourD x diffuse x pi x the diffuse lobe's energy factor
// (the piece jahDiffuseEnergyFactor, 200.BRDFs_piece_ps.any, WRITTEN OUT
// here because a compute job cannot reach the Pbs library's pieces). Two
// terms the pixel has that the card does not, stated: BRDF_EnvMap's
// envBRDF.z (the LTC table's third channel, bound only once a scene holds an
// area light; 1 otherwise, which is what this is) and a diffuse fresnel
// (PbsBrdf::Default carries none).
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

	// Vct_piece_ps.any's buildConeBasis (patch 0083), unchanged.
	mat3 jahCardConeBasis( vec3 n )
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

	// THE PIXEL'S DIFFUSE GI at world point P with normal N: envColourD, i.e.
	// the voxels' share times the multiplier plus the environment's share.
	vec3 jahCardEnvColourD( vec3 P, vec3 N )
	{
		vec3 posLS = ( P - gp.volumeOrigin.xyz ) * gp.volumeInvSize.xyz;
		// A world direction in the volume's normalised space (toVctProbeSpaceDir).
		vec3 dirLS = normalize( N * gp.volumeInvSize.xyz );
		vec3 biasDirLS = dirLS;
		posLS += biasDirLS * gp.chainInvRes[0].xyz;

		// The frame is built in the cubemap frame (z flipped), as the pixel
		// builds it from invViewMatCubemap, and brought back to the world.
		vec3 nC = vec3( N.x, N.y, -N.z );
		mat3 basisC = jahCardConeBasis( nC );

		// THE PIXEL'S CONE SET, whichever it is: HlmsPbs's `vct_cone_dirs` (6 with
		// setVctFullConeCount, else 4 — the engine leaves it at 4), copied onto
		// this job by the host so the two sets cannot drift.
@property( vct_cone_dirs == 6 )
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
@else
		const int kCones = 4;
		const vec3 coneDirs[4] = vec3[4]( vec3( 0.707107, 0.0, 0.707107 ),
										  vec3( 0.0, 0.707107, 0.707107 ),
										  vec3( -0.707107, 0.0, 0.707107 ),
										  vec3( 0.0, -0.707107, 0.707107 ) );
		const float coneWeights[4] = float[4]( 0.25, 0.25, 0.25, 0.25 );
		const float coneAngleTan = 0.98269;
		const uint coneFlags = JAH_MARCH_LODSTEP;
@end

		vec3 light = vec3( 0.0, 0.0, 0.0 );
		vec3 envD = vec3( 0.0, 0.0, 0.0 );
		for( int i = 0; i < kCones; ++i )
		{
			vec3 dC = basisC * coneDirs[i];
			vec3 dirWorld = vec3( dC.x, dC.y, -dC.z );
			vec3 dir = normalize( dirWorld * gp.volumeInvSize.xyz );
			JahConeResult result = jahConeMarch( posLS, dir, coneAngleTan, biasDirLS, dirLS,
												 coneFlags );
			light += coneWeights[i] * result.colour;
			envD += coneWeights[i] * ( 1.0 - min( 1.0, result.escapeAlpha / 0.95 ) ) *
					jahEnvCone( dirWorld, coneAngleTan );
		}
		return light * gp.counts.y + envD;
	}
@end

// THE STORE ROUNDS. The device converts a float into R11G11B10F by
// TRUNCATION (measured: 0.50829 stored as 0.5), so a channel lost up to one
// whole mantissa step — 1/64 on red and green, 1/32 (3 %) on blue. Scaling by
// one plus half a step first makes the truncation a round to nearest: half a
// step at most. Only on that format (the host's property): the RGBA16F
// fallback stores unrounded.
vec3 jahCardRound( vec3 v )
{
@property( jah_card_round_r11g11b10 )
	return v * vec3( 1.0 + 1.0 / 128.0, 1.0 + 1.0 / 128.0, 1.0 + 1.0 / 64.0 );
@else
	return v;
@end
}

// BRDF_Default's diffuse at V = N, times NdotL (200.BRDFs_piece_ps.any).
float jahCardDiffuse( vec3 N, vec3 L, float perceptualRoughness )
{
	float NdotL = clamp( dot( N, L ), 0.0, 1.0 );
	vec3 H = normalize( L + N );
	float VdotH = clamp( dot( N, H ), 0.0, 1.0 );
	float energyBias = perceptualRoughness * 0.5;
	float energyFactor = mix( 1.0, 1.0 / 1.51, perceptualRoughness );
	float fd90 = energyBias + 2.0 * VdotH * VdotH * perceptualRoughness;
	float lightScatter = 1.0 + ( fd90 - 1.0 ) * pow( 1.0 - NdotL, 5.0 );
	// viewScatter = 1.0 + ( fd90 - 1.0 ) * pow( 1.0 - NdotV, 5.0 ) = 1 at V = N.
	return NdotL * lightScatter * energyFactor;
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
				atten = 1.0 / ( 0.5 + ( l.attenuation.y + l.attenuation.z * d ) * d );
				atten *= max( ( l.attenuation.x - d ) * l.attenuation.w, 0.0 );
				if( type > 1.5 )
				{
					float spotCosAngle = dot( -L, l.spotDirection.xyz );
					if( spotCosAngle < l.spotParams.y )
						continue;
					float spotAtten = clamp( ( spotCosAngle - l.spotParams.y ) * l.spotParams.x, 0.0, 1.0 );
					atten *= pow( spotAtten, l.spotParams.z );
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
			// BRDF_EnvMap: envColourD x diffuse x pi x jahDiffuseEnergyFactor.
			indirect = jahCardEnvColourD( P, N ) * kD * 3.141592654 *
					   mix( 1.0, 1.0 / 1.51, perceptualRoughness );
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
