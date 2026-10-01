@insertpiece( SetCrossPlatformSettings )
@insertpiece( DeclUavCrossPlatform )

#define ushort2 uvec2

@insertpiece( PreBindingsHeaderCS )

vulkan_layout( ogre_T0 ) uniform samplerBuffer directionsBuffer;

vulkan( layout( ogre_s1 ) uniform sampler probeSampler );

// Jahshaka (PHOTON-READER-1): EVERY cascade's volumes, the probe rays walk the
// whole chain. The same declaration as the bounce-injection job's: one array per
// volume kind, unit order iso, then X, Y, Z (IrradianceField::bindChainToGenerationJob).
// uses_array_bindings = hlms_num_vct_cascades - 1 (arrays of more than one binding)
@pset( vctTexUnit, 1 )
@psub( uses_array_bindings, hlms_num_vct_cascades, 1 )

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
// Jahshaka (PHOTON-VOXEL-3): every cascade's PER-HALF-AXIS COVERAGE (+a faces, then -a).
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeCovN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-4): every cascade's SURFACE POSITION per half-axis (+a, then -a), the last kinds.
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosP[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbePosN[@value( hlms_num_vct_cascades )];
@add( vctTexUnit, hlms_num_vct_cascades )
// Jahshaka (PHOTON-VOXEL-5): the anisotropic tiers' level-0 back side and the voxelizer's normal.
@property( vct_anisotropic )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeBack[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform texture3D vctProbeNrm[@value( hlms_num_vct_cascades )];
	@add( vctTexUnit, hlms_num_vct_cascades )
@end

// Jahshaka (PHOTON-ENV-1): the environment cube, after every volume, while the
// lighting has one (IrradianceField::bindChainToGenerationJob).
@property( jah_env )
	vulkan_layout( ogre_t@value(vctTexUnit) ) uniform textureCube envCube;
	@add( vctTexUnit, 1 )
@end

// Jahshaka (PHOTON-FIELD-ROTATE-1): READ and written - every texel blends with its
// own history (the probe's mean over its integrations).
layout( vulkan( ogre_u0 ) vk_comma @insertpiece(uav0_pf_type) )
uniform restrict image2D irradianceField;

layout( vulkan( ogre_u1 ) vk_comma @insertpiece(uav1_pf_type) )
uniform restrict image2D irradianceFieldDepth;

// One work group per probe: every ray's radiance and depth, and its direction.
shared float4 g_rayColourDepth[@value( num_rays_per_probe )];
shared float4 g_rayDir[@value( num_rays_per_probe )];
// The probe's history: x = the samples the blend keeps, y = 1 when the probe is skipped.
shared float2 g_probeHistory;

layout( local_size_x = @value( threads_per_group_x ),
		local_size_y = @value( threads_per_group_y ),
		local_size_z = @value( threads_per_group_z ) ) in;

@insertpiece( HeaderCS )

//in uvec3 gl_NumWorkGroups;
//in uvec3 gl_WorkGroupID;
//in uvec3 gl_LocalInvocationID;
//in uvec3 gl_GlobalInvocationID;
//in uint  gl_LocalInvocationIndex;

void main()
{
	@insertpiece( BodyCS )
}
