@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

// THE NO-RAY ROUTE'S AREA LIGHT VISIBILITY, JOB 2 OF 2: THE RESOLVE (PHOTON-7 stage 7, 7-6; JahScreenAreaVis_cs.glsl is
// job 1). One thread a FULL-RESOLUTION pixel: the joint-bilateral upsample of the trace's two sums and their ratio into
// `jahAreaVis` (RG32_UINT: x the diffuse visibility of light i in byte i, y the specular's) - jah_area_vis_law.glsl's
// jahAreaVisResolve, ONE TEXT with the ray tier's rq_area_vis_resolve.comp; this job binds it. The pass reads it
// through the fork's hook in DoAreaLtcLights (custom_ps_areaLtcVisibility, JahAreaVis_piece_ps.any), as on the ray tier.

vulkan_layout( ogre_t0 ) uniform texture2D sceneDepth;
vulkan_layout( ogre_t1 ) uniform texture2D gBufNormals;
vulkan_layout( ogre_t2 ) uniform utexture2D curSU;
vulkan_layout( ogre_t3 ) uniform utexture2D curS2U2;
vulkan_layout( ogre_t4 ) uniform utexture2D curG;

layout( vulkan( ogre_u0 ) vk_comma @insertpiece( uav0_pf_type ) ) uniform restrict writeonly uimage2D outVis;

#define JAH_AREA_VIS_PARAMS_BLOCK layout( std430, ogre_U1 ) readonly restrict buffer
@insertpiece( JahAreaVisParams )

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

/// A SUM SET'S WORD of four halves (JahScreenAreaVis_cs.glsl's packing: S then U)
vec4 jahAreaSumA( uvec4 w ) { return vec4( unpackHalf2x16( w.x ), unpackHalf2x16( w.y ) ); }
vec4 jahAreaSumB( uvec4 w ) { return vec4( unpackHalf2x16( w.z ), unpackHalf2x16( w.w ) ); }

#define JAH_AREA_DEPTH( pix ) texelFetch( sceneDepth, pix, 0 ).x
#define JAH_AREA_NORMAL( pix ) texelFetch( gBufNormals, pix, 0 ).xyz
#define JAH_AREA_CUR_S( t ) jahAreaSumA( texelFetch( curSU, t, 0 ) )
#define JAH_AREA_CUR_U( t ) jahAreaSumB( texelFetch( curSU, t, 0 ) )
#define JAH_AREA_CUR_S2( t ) jahAreaSumA( texelFetch( curS2U2, t, 0 ) )
#define JAH_AREA_CUR_U2( t ) jahAreaSumB( texelFetch( curS2U2, t, 0 ) )
#define JAH_AREA_CUR_G( t ) texelFetch( curG, t, 0 )
#define JAH_AREA_STORE_VIS( pix, words ) imageStore( outVis, pix, uvec4( words, 0u, 0u ) )
@insertpiece( JahAreaVisLaw )

void main()
{
	jahAreaVisResolve( ivec2( gl_GlobalInvocationID.xy ) );
}
