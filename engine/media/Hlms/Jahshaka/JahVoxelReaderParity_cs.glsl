// engine.voxel_reader_parity's COMPUTE half (PHOTON-READER-1): the same cones as
// the fragment half (JahVoxelReaderParity_ps.glsl, generated), marched through
// the one voxel reader as the Hlms consumers insert it - the pieces
// JahVoxelSample, JahVoxelMarch and JahVoxelParity - over the chain bound the
// way the irradiance field's generation job binds it (one array per volume
// kind, iso then X, Y, Z; one sampler).
@insertpiece( SetCrossPlatformSettings )

@pset( vctTexUnit, 0 )
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

vulkan( layout( ogre_s0 ) uniform sampler vSmp );

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

struct ParityParams
{
	vec4 chainInvRes[8];
	vec4 chainFromPrev[14];
	vec4 cones[256];
	vec4 counts;
	vec4 sdf;
};
layout( std430, ogre_U0 ) readonly restrict buffer paramsLayout { ParityParams params; };
layout( std430, ogre_U1 ) writeonly restrict buffer outLayout { vec4 answers[]; };

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

#define JAH_VOX_MAX_CASCADES @value( hlms_num_vct_cascades )
#define JAH_VOX_COUNT @value( hlms_num_vct_cascades )
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
#define JAH_VOX_INVRES( c ) params.chainInvRes[c].xyz
#define JAH_VOX_MAXLOD( c ) params.chainInvRes[c].w
#define JAH_VOX_FROM_PREV_SCALE( c ) params.chainFromPrev[( (c) - 1 ) * 2]
#define JAH_VOX_FROM_PREV_OFFSET( c ) params.chainFromPrev[( (c) - 1 ) * 2 + 1]
#define JAH_VOX_SDF_MAXMIP params.sdf.x
#define JAH_VOX_SDF_FACTOR params.sdf.y
#define JAH_PARITY_CONE( i ) params.cones[i]

@insertpiece( JahVoxelSample )
@insertpiece( JahVoxelMarch )
@insertpiece( JahVoxelParity )

void main()
{
	int px = int( gl_GlobalInvocationID.x );
	int cone = px / 4;
	if( cone >= int( params.counts.x ) )
		return;
	answers[px] = jahParityAnswer( cone, px - 4 * cone );
}
