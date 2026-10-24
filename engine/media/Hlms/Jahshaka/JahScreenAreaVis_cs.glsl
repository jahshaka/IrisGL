@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

// THE NO-RAY ROUTE'S AREA LIGHT VISIBILITY, JOB 1 OF 2: THE TRACE (PHOTON-7 stage 7, 7-6; SPECS/PHOTON_7_SPEC.md "AREA
// LIGHTS, ONE LAW ON EVERY PATH"). A chain that does not gather has no ray device to trace the pixel's area light
// visibility with, and an LTC area light passed through walls. THE LAW IS THE RAY TIER'S, ONE TEXT
// (jah_area_vis_law.glsl's jahAreaVisTrace, the piece JahAreaVisLaw: one stratum of each forward LTC light a texel a
// frame, the ratio's two sums averaged over time with the reprojected same-surface history and the gather's rest
// law, the diffuse-weighted and the specular-weighted sets); only HOW A STRATUM IS SEEN is this route's: THE VOXEL
// CHAIN'S OWN SHADOW MARCH (LightInjection's jahInjectVisibility, the piece JahInjectionReader - the march the voxels'
// own direct term is shadowed with, on the world shells, every cascade's coverage and surface position per half-axis),
// from the stratum ray's start for its reach. Its accuracy is the cell (cascade 0's: 7.8 cm at High and Epic, 15.6 cm
// at Low and Medium - the start's bias is one such cell along the normal, the cones' jahConeStart) and VOXEL-LEAK-1's
// thin wall - named, not hidden. The transmittance it returns is fractional (a part-covered cell passes its share).
// THE IMAGES (this job's own: a sum set a word of four halves, so the job binds five storage images - the device
// floor's eight, the Mac's included): the previous frame's sums and surface word read as textures, this frame's
// written, the rest means read and written. Job 2 (JahScreenAreaVisResolve_cs.glsl) upsamples them into `jahAreaVis`.
// The host half: OgreAreaVis.cpp. MONO ONLY (the ray tier's rule: a stereo view declines - stage 9's stereo item).

vulkan_layout( ogre_t0 ) uniform texture2D sceneDepth;
vulkan_layout( ogre_t1 ) uniform texture2D gBufNormals;
vulkan_layout( ogre_t2 ) uniform texture2D gBufShadowRoughness;
vulkan_layout( ogre_t3 ) uniform utexture2D prevSU;
vulkan_layout( ogre_t4 ) uniform utexture2D prevS2U2;
vulkan_layout( ogre_t5 ) uniform utexture2D prevG;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) ) uniform restrict writeonly uimage2D curSU;
layout( vulkan( ogre_u1 ) vk_comma @insertpiece( uav1_pf_type ) ) uniform restrict writeonly uimage2D curS2U2;
layout( vulkan( ogre_u2 ) vk_comma @insertpiece( uav2_pf_type ) ) uniform restrict writeonly uimage2D curG;
layout( vulkan( ogre_u3 ) vk_comma @insertpiece( uav3_pf_type ) ) uniform restrict uimage2D restSU;
layout( vulkan( ogre_u4 ) vk_comma @insertpiece( uav4_pf_type ) ) uniform restrict uimage2D restS2U2;

#define JAH_AREA_VIS_PARAMS_BLOCK layout( std430, ogre_U5 ) readonly restrict buffer
@insertpiece( JahAreaVisParams )

/// THE CHAIN THE MARCH RUNS THROUGH (LightInjection_cs.glsl's chain parameters, for cascade 0 as the march's own
/// entry: PhotonVoxelLighting::chainMarchParams): per entry j its box corner relative to cascade 0's (metres; w its
/// cell) and its resolution (xyz) with the world distance its shell ends at (w); cascade 0's corner in the world (xyz)
/// and 1 / its resolution; how many entries.
struct AreaChainParams
{
	vec4 origin[8];
	vec4 res[8];
	vec4 corner0;
	vec4 invRes0;
	vec4 count;
};
layout( std430, ogre_U6 ) readonly restrict buffer areaChainLayout { AreaChainParams areaChain; };

@property( hlms_num_vct_cascades )
	@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )
	@set( jahAreaTex, 6 )
	vulkan( layout( ogre_s6 ) uniform sampler voxelAlbedoSampler );
	vulkan_layout( ogre_t@value(jahAreaTex) ) uniform texture3D chainCovP[@value( hlms_num_vct_cascades )];
	@add( jahAreaTex, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(jahAreaTex) ) uniform texture3D chainCovN[@value( hlms_num_vct_cascades )];
	@add( jahAreaTex, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(jahAreaTex) ) uniform texture3D chainPosP[@value( hlms_num_vct_cascades )];
	@add( jahAreaTex, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(jahAreaTex) ) uniform texture3D chainPosN[@value( hlms_num_vct_cascades )];
	// the march's chain, in the names LightInjection's reader reads them by
	#define p_chainSelf 0u
	#define p_chainOrigin areaChain.origin
	#define p_chainRes areaChain.res
	#define p_invVoxelResolution areaChain.invRes0.xyz
	#define JAH_INJ_CHAIN @value( hlms_num_vct_cascades )
	@insertpiece( JahInjectionReader )
@end

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( JahAreaStrata )
@insertpiece( JahReproject )

/// A SUM SET AS ONE WORD of four halves (two vec4 sums: S then U) - the ray tier's two rgba16f images' precision.
uvec4 jahAreaPackSums( vec4 a, vec4 b )
{
	return uvec4( packHalf2x16( a.xy ), packHalf2x16( a.zw ), packHalf2x16( b.xy ), packHalf2x16( b.zw ) );
}
vec4 jahAreaSumA( uvec4 w ) { return vec4( unpackHalf2x16( w.x ), unpackHalf2x16( w.y ) ); }
vec4 jahAreaSumB( uvec4 w ) { return vec4( unpackHalf2x16( w.z ), unpackHalf2x16( w.w ) ); }

/// A STRATUM SEEN THROUGH THE VOXEL CHAIN: the transmittance from its start along its direction for its reach (no
/// chain built yet: unshadowed - the voxels hold nothing to stop it). The march starts in cascade 0 (a start its box
/// does not hold reads on in the first cascade that does); no origin gate (oAxis -1): the start is a cell off the
/// surface, past its own voxel's planes.
float jahAreaVoxelVisible( vec3 originQ, vec3 wq, float reachQ )
{
@property( hlms_num_vct_cascades )
	if( areaChain.count.x < 0.5 )
		return 1.0;
	const vec3 sW = originQ - areaChain.corner0.xyz;
	const ivec3 iv = ivec3( floor( sW / areaChain.origin[0].w ) );
	return jahInjectVisibility( sW, wq, reachQ, iv, -1, 0.0, 0.0 );
@else
	return 1.0;
@end
}

#define JAH_AREA_DEPTH( pix ) texelFetch( sceneDepth, pix, 0 ).x
#define JAH_AREA_NORMAL( pix ) texelFetch( gBufNormals, pix, 0 ).xyz
#define JAH_AREA_ALPHA( pix ) texelFetch( gBufShadowRoughness, pix, 0 ).y
#define JAH_AREA_PREV_S( pc ) jahAreaSumA( texelFetch( prevSU, pc, 0 ) )
#define JAH_AREA_PREV_U( pc ) jahAreaSumB( texelFetch( prevSU, pc, 0 ) )
#define JAH_AREA_PREV_S2( pc ) jahAreaSumA( texelFetch( prevS2U2, pc, 0 ) )
#define JAH_AREA_PREV_U2( pc ) jahAreaSumB( texelFetch( prevS2U2, pc, 0 ) )
#define JAH_AREA_PREV_G( pc ) texelFetch( prevG, pc, 0 )
#define JAH_AREA_STORE_CUR( tc, S, U, S2, U2, G )                                                                       \
	{                                                                                                                  \
		imageStore( curSU, tc, jahAreaPackSums( S, U ) );                                                              \
		imageStore( curS2U2, tc, jahAreaPackSums( S2, U2 ) );                                                          \
		imageStore( curG, tc, G );                                                                                     \
	}
#define JAH_AREA_REST_LOAD( tc, S, U, S2, U2 )                                                                          \
	{                                                                                                                  \
		const uvec4 jahRestA = imageLoad( restSU, tc );                                                                \
		const uvec4 jahRestB = imageLoad( restS2U2, tc );                                                              \
		S = jahAreaSumA( jahRestA );                                                                                   \
		U = jahAreaSumB( jahRestA );                                                                                   \
		S2 = jahAreaSumA( jahRestB );                                                                                  \
		U2 = jahAreaSumB( jahRestB );                                                                                  \
	}
#define JAH_AREA_REST_STORE( tc, S, U, S2, U2 )                                                                         \
	{                                                                                                                  \
		imageStore( restSU, tc, jahAreaPackSums( S, U ) );                                                             \
		imageStore( restS2U2, tc, jahAreaPackSums( S2, U2 ) );                                                         \
	}
#define JAH_AREA_VISIBLE( originQ, wq, reachQ ) jahAreaVoxelVisible( originQ, wq, reachQ )
// (the march's clearance a cell along the normal, the cones' convention: jah_area_vis_law.glsl)
#define JAH_AREA_LIFT_ALONG_NORMAL 1
@insertpiece( JahAreaVisLaw )

void main()
{
	jahAreaVisTrace( ivec2( gl_GlobalInvocationID.xy ) );
}
